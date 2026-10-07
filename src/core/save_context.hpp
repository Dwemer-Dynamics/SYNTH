#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace synth::core {

// An acknowledged observation, not a completed dialogue or an exact history watermark.
struct SavedContext final {
    std::string playthrough_id;
    std::string session_id;
    std::uint64_t generation{};
    std::uint64_t context_sequence{};
    bool operator==(const SavedContext&) const = default;
};

inline constexpr std::uint32_t saved_context_record_type = 0x53435458; // SCTX
inline constexpr std::uint32_t saved_context_record_version = 1;
using SavedContextBytes = std::array<std::uint8_t, 274>;

inline bool valid_saved_context(const SavedContext& value) noexcept {
    const auto valid_id = [](std::string_view id) {
        const auto alnum = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        };
        return !id.empty() && id.size() <= 128 && alnum(id.front()) &&
            std::ranges::all_of(id, [&](char c) { return alnum(c) || c == '.' || c == '_' || c == ':' || c == '-'; });
    };
    constexpr auto maximum = 9'007'199'254'740'991ULL;
    return valid_id(value.playthrough_id) && valid_id(value.session_id) &&
        value.generation > 0 && value.generation <= maximum &&
        value.context_sequence > 0 && value.context_sequence <= maximum;
}

// Fixed-size little-endian encoding avoids ABI padding and unbounded co-save allocations.
inline std::optional<SavedContextBytes> encode_saved_context(const SavedContext& value) {
    if (!valid_saved_context(value)) return std::nullopt;
    SavedContextBytes bytes{};
    std::ranges::copy(value.playthrough_id, bytes.begin());
    std::ranges::copy(value.session_id, bytes.begin() + 129);
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[258 + i] = static_cast<std::uint8_t>(value.generation >> (i * 8));
        bytes[266 + i] = static_cast<std::uint8_t>(value.context_sequence >> (i * 8));
    }
    return bytes;
}

inline std::optional<SavedContext> decode_saved_context(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != SavedContextBytes{}.size()) return std::nullopt;
    const auto read_id = [&](std::size_t offset) -> std::optional<std::string> {
        const auto slot = bytes.subspan(offset, 129);
        const auto end = std::ranges::find(slot, 0);
        if (end == slot.end() || !std::all_of(end, slot.end(), [](auto byte) { return byte == 0; }))
            return std::nullopt;
        return std::string(slot.begin(), end);
    };
    const auto playthrough = read_id(0);
    const auto session = read_id(129);
    if (!playthrough || !session) return std::nullopt;
    SavedContext value{*playthrough, *session};
    for (std::size_t i = 0; i < 8; ++i) {
        value.generation |= std::uint64_t{bytes[258 + i]} << (i * 8);
        value.context_sequence |= std::uint64_t{bytes[266 + i]} << (i * 8);
    }
    return valid_saved_context(value) ? std::optional{value} : std::nullopt;
}

// The lock protects only bounded value copies. No engine, transport, logging or file calls occur under it.
class SaveContextStore final {
public:
    struct Binding final {
        std::uint64_t epoch{};
        std::optional<SavedContext> loaded_context;
    };

    [[nodiscard]] std::uint64_t reset() {
        std::scoped_lock lock{mutex_};
        loaded_.reset();
        latest_.reset();
        bound_ = false;
        return ++epoch_;
    }

    void restore(std::uint64_t epoch, std::optional<SavedContextBytes> bytes) {
        if (bytes && !decode_saved_context(*bytes)) bytes.reset();
        std::scoped_lock lock{mutex_};
        if (epoch != epoch_) return;
        loaded_ = latest_ = bytes;
    }

    [[nodiscard]] Binding bind(std::string_view playthrough) {
        std::array<std::uint8_t, 129> owner{};
        if (playthrough.empty() || playthrough.size() > 128) return {reset(), std::nullopt};
        std::ranges::copy(playthrough, owner.begin());
        std::optional<SavedContextBytes> loaded;
        std::uint64_t epoch;
        {
            std::scoped_lock lock{mutex_};
            epoch = ++epoch_;
            owner_ = owner;
            bound_ = true;
            if (loaded_ && !std::equal(owner.begin(), owner.end(), loaded_->begin())) loaded_.reset();
            if (latest_ && !std::equal(owner.begin(), owner.end(), latest_->begin())) latest_.reset();
            loaded = loaded_;
        }
        auto context = loaded ? decode_saved_context(*loaded) : std::nullopt;
        return {epoch, std::move(context)};
    }

    void stop_writer(std::uint64_t epoch) {
        std::scoped_lock lock{mutex_};
        if (epoch == epoch_) { ++epoch_; bound_ = false; }
    }

    [[nodiscard]] bool acknowledge(std::uint64_t epoch, const SavedContext& context) {
        const auto bytes = encode_saved_context(context);
        if (!bytes) return false;
        std::scoped_lock lock{mutex_};
        if (epoch != epoch_ || !bound_ || !std::equal(owner_.begin(), owner_.end(), bytes->begin())) return false;
        // A later ACK must not be replaced if workers finish their bookkeeping out of order.
        if (latest_ && std::equal(bytes->begin(), bytes->begin() + 266, latest_->begin())) {
            std::uint64_t previous{};
            for (std::size_t i = 0; i < 8; ++i) previous |= std::uint64_t{(*latest_)[266 + i]} << (i * 8);
            if (context.context_sequence <= previous) return false;
        }
        latest_ = bytes;
        return true;
    }

    [[nodiscard]] std::optional<SavedContextBytes> for_save() const {
        std::scoped_lock lock{mutex_};
        return latest_;
    }

private:
    mutable std::mutex mutex_;
    std::uint64_t epoch_{};
    bool bound_{};
    std::array<std::uint8_t, 129> owner_{};
    std::optional<SavedContextBytes> loaded_;
    std::optional<SavedContextBytes> latest_;
};

// Interface-shaped helpers allow hostile/truncated co-save records to be tested without Fallout.
template <class Serialization>
bool write_saved_context(const Serialization& serialization, const SaveContextStore& store) {
    const auto bytes = store.for_save();
    return !bytes || serialization.WriteRecord(saved_context_record_type, saved_context_record_version,
                                               bytes->data(), static_cast<std::uint32_t>(bytes->size()));
}

template <class Serialization>
void read_saved_context(const Serialization& serialization, SaveContextStore& store) {
    const auto epoch = store.reset();
    std::optional<SavedContextBytes> restored;
    bool seen{};
    std::uint32_t type{}, version{}, length{};
    for (unsigned record = 0; record < 64; ++record) {
        if (!serialization.GetNextRecordInfo(type, version, length)) {
            store.restore(epoch, restored);
            return;
        }
        if (type != saved_context_record_type) continue;
        if (seen || version != saved_context_record_version || length != SavedContextBytes{}.size()) return;
        seen = true;
        SavedContextBytes bytes{};
        if (serialization.ReadRecordData(bytes.data(), length) != length || !decode_saved_context(bytes)) return;
        restored = bytes;
    }
    // Too many records, duplicates, unknown versions and malformed records leave no inferred anchor.
}

} // namespace synth::core
