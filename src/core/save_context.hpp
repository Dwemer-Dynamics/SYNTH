#pragma once

#include "core/runtime_identity.hpp"

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
inline constexpr std::uint32_t saved_context_record_version = 2;
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


// SYNTH owns the playthrough identity: a random UUID v4, never the engine player ID or a server checkpoint.
using PlaythroughUuid = std::array<std::uint8_t, 16>;
using PlaythroughOwner = std::array<std::uint8_t, 129>;

[[nodiscard]] constexpr PlaythroughUuid uuid_v4(PlaythroughUuid random) noexcept {
    random[6] = static_cast<std::uint8_t>((random[6] & 0x0F) | 0x40);
    random[8] = static_cast<std::uint8_t>((random[8] & 0x3F) | 0x80);
    return random;
}

[[nodiscard]] constexpr bool valid_uuid_v4(const PlaythroughUuid& value) noexcept {
    return (value[6] & 0xF0) == 0x40 && (value[8] & 0xC0) == 0x80;
}

// Canonical lowercase 8-4-4-4-12 text, null-padded into the fixed owner slot without allocating.
[[nodiscard]] constexpr PlaythroughOwner uuid_owner(const PlaythroughUuid& value) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    PlaythroughOwner owner{};
    std::size_t out{};
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) owner[out++] = '-';
        owner[out++] = static_cast<std::uint8_t>(digits[value[i] >> 4]);
        owner[out++] = static_cast<std::uint8_t>(digits[value[i] & 0x0F]);
    }
    return owner;
}

[[nodiscard]] inline std::string uuid_text(const PlaythroughUuid& value) {
    const auto owner = uuid_owner(value);
    return std::string(owner.begin(), owner.begin() + 36);
}

// Version 2 SCTX: lane, UUID, anchor flag and an optional exact saved checkpoint owned by that UUID.
inline constexpr std::uint32_t legacy_saved_context_record_version = 1;
using SavedRecordBytes = std::array<std::uint8_t, 18 + std::tuple_size_v<SavedContextBytes>>;

struct SavedRecord final {
    RuntimeVariant lane{};
    PlaythroughUuid uuid{};
    std::optional<SavedContextBytes> anchor;
};

[[nodiscard]] inline bool anchor_owned_by(const SavedContextBytes& anchor, const PlaythroughOwner& owner) {
    return std::equal(owner.begin(), owner.end(), anchor.begin()) && decode_saved_context(anchor).has_value();
}

[[nodiscard]] inline std::optional<SavedRecordBytes> encode_saved_record(const SavedRecord& value) {
    if (!valid_uuid_v4(value.uuid) || (value.anchor && !anchor_owned_by(*value.anchor, uuid_owner(value.uuid))))
        return std::nullopt;
    SavedRecordBytes bytes{};
    bytes[0] = value.lane == RuntimeVariant::vr ? 2 : 1;
    std::ranges::copy(value.uuid, bytes.begin() + 1);
    if (value.anchor) {
        bytes[17] = 1;
        std::ranges::copy(*value.anchor, bytes.begin() + 18);
    }
    return bytes;
}

[[nodiscard]] inline std::optional<SavedRecord> decode_saved_record(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != SavedRecordBytes{}.size() || (bytes[0] != 1 && bytes[0] != 2) || bytes[17] > 1) return std::nullopt;
    SavedRecord value{bytes[0] == 2 ? RuntimeVariant::vr : RuntimeVariant::flat};
    std::ranges::copy(bytes.subspan(1, 16), value.uuid.begin());
    if (!valid_uuid_v4(value.uuid)) return std::nullopt;
    const auto anchor = bytes.subspan(18);
    if (bytes[17] == 0) {
        if (!std::ranges::all_of(anchor, [](auto byte) { return byte == 0; })) return std::nullopt;
        return value;
    }
    value.anchor.emplace();
    std::ranges::copy(anchor, value.anchor->begin());
    if (!anchor_owned_by(*value.anchor, uuid_owner(value.uuid))) return std::nullopt;
    return value;
}

// Why a load did or did not restore its saved identity. Anything but restored starts a separate history.
enum class LoadedIdentity : std::uint8_t {
    restored, missing, legacy, malformed, duplicate, unsupported, wrong_lane, contradicted, incomplete
};
enum class IdentityResolution : std::uint8_t { restored, new_game, separated, random_failed, load_failed, stale };

struct ResolvedIdentity final {
    IdentityResolution resolution{};
    LoadedIdentity cause{};
};

// Only these outcomes leave an identity active in the current world, so only they may publish readiness.
[[nodiscard]] constexpr bool identity_resolved(const ResolvedIdentity& value) noexcept {
    return value.resolution == IdentityResolution::restored || value.resolution == IdentityResolution::new_game ||
        value.resolution == IdentityResolution::separated;
}

// NewGame's lifecycle consequence. A stale draw was superseded by a later Revert, load or NewGame that already
// changed the world, so it must not tear down what that change resolved. Any other unresolved outcome
// (random_failed) still tears down the old world without readiness.
enum class NewGameLifecycle : std::uint8_t { none, teardown, ready };

[[nodiscard]] constexpr NewGameLifecycle new_game_lifecycle(const ResolvedIdentity& value) noexcept {
    if (value.resolution == IdentityResolution::stale) return NewGameLifecycle::none;
    return identity_resolved(value) ? NewGameLifecycle::ready : NewGameLifecycle::teardown;
}

[[nodiscard]] constexpr std::string_view to_string(LoadedIdentity value) noexcept {
    switch (value) {
    case LoadedIdentity::restored: return "restored";
    case LoadedIdentity::missing: return "no SYNTH identity record";
    case LoadedIdentity::legacy: return "legacy version 1 record";
    case LoadedIdentity::malformed: return "malformed record";
    case LoadedIdentity::duplicate: return "duplicate record";
    case LoadedIdentity::unsupported: return "unsupported record version";
    case LoadedIdentity::wrong_lane: return "record from the other Fallout lane";
    case LoadedIdentity::contradicted: return "record contradicted by a revert";
    case LoadedIdentity::incomplete: return "record still being read when the load finished";
    }
    return "unknown";
}

// The lock protects only bounded value copies. No engine, transport, RNG, logging or file calls occur under it.
class SaveContextStore final {
public:
    struct Binding final {
        std::uint64_t epoch{};
        std::optional<SavedContext> loaded_context;
    };

    // One load callback's claim on the transaction open when it entered. It stages exactly once, into that
    // transaction only; an abandoned claim (failure or exception) stages a malformed outcome.
    class LoadCallback final {
    public:
        LoadCallback(const LoadCallback&) = delete;
        LoadCallback& operator=(const LoadCallback&) = delete;
        ~LoadCallback() {
            try { stage(LoadedIdentity::malformed, std::nullopt); } catch (...) {}
        }

        [[nodiscard]] bool open() const noexcept { return store_ != nullptr; }

        void stage(LoadedIdentity status, std::optional<SavedRecord> record) {
            if (auto* store = std::exchange(store_, nullptr)) store->stage(ticket_, status, std::move(record));
        }

    private:
        friend SaveContextStore;
        LoadCallback(SaveContextStore* store, std::uint64_t ticket) noexcept : store_{store}, ticket_{ticket} {}
        SaveContextStore* store_;
        std::uint64_t ticket_;
    };

    explicit SaveContextStore(RuntimeVariant lane = RuntimeVariant::flat) noexcept : lane_{lane} {}

    // PreLoadGame: revoke the old world and open a load transaction for native callbacks to stage into.
    void begin_load() {
        std::scoped_lock lock{mutex_};
        revoke_locked();
        transaction_ = Transaction::loading;
        ticket_ = ++tickets_;
    }

    // Load callback entry, before any record is read. A second entry in one load is a duplicate block.
    [[nodiscard]] LoadCallback enter_load() {
        std::scoped_lock lock{mutex_};
        if (transaction_ != Transaction::loading) return {nullptr, 0};
        ++callbacks_;
        ++reading_;
        return {this, ticket_};
    }

    // Inside an open load, Revert keeps the load but contradicts any record a callback has begun to stage.
    // Anywhere else it revokes the current identity at once, bound or not. Returns whether it revoked a world.
    bool revert() {
        std::scoped_lock lock{mutex_};
        if (transaction_ == Transaction::loading) {
            ++epoch_;
            bound_ = false;
            if (callbacks_ > 0) contradicted_ = true;
            return false;
        }
        revoke_locked();
        return true;
    }

    // PostLoadGame: consumes the open load exactly once. A restore activates in this one locked transition;
    // a fresh identity is drawn outside the lock and lands only if no later world change superseded it.
    template <class Random>
    ResolvedIdentity finish_load(bool success, Random&& random) {
        std::uint64_t serial{};
        LoadedIdentity cause{};
        {
            std::scoped_lock lock{mutex_};
            if (transaction_ != Transaction::loading) return {IdentityResolution::stale, LoadedIdentity::missing};
            if (!success) {
                revoke_locked();
                return {IdentityResolution::load_failed, LoadedIdentity::missing};
            }
            cause = contradicted_ ? LoadedIdentity::contradicted
                : callbacks_ > 1 ? LoadedIdentity::duplicate
                : reading_ > 0 ? LoadedIdentity::incomplete
                : staged_.status;
            if (cause == LoadedIdentity::restored && staged_.record) {
                // decode_saved_record already proved the UUID and that any anchor belongs to it.
                commit_locked(staged_.record->uuid, staged_.record->anchor);
                staged_ = {};
                return {IdentityResolution::restored, LoadedIdentity::restored};
            }
            if (cause == LoadedIdentity::restored) cause = LoadedIdentity::malformed;
            resolving_locked();
            serial = serial_;
        }
        return fresh(serial, {IdentityResolution::separated, cause}, std::forward<Random>(random));
    }

    template <class Random>
    ResolvedIdentity start_new_game(Random&& random) {
        std::uint64_t serial{};
        {
            std::scoped_lock lock{mutex_};
            revoke_locked();
            resolving_locked();
            serial = serial_;
        }
        return fresh(serial, {IdentityResolution::new_game, LoadedIdentity::missing}, std::forward<Random>(random));
    }

    [[nodiscard]] bool identity_ready() const {
        std::scoped_lock lock{mutex_};
        return identity_.has_value();
    }

    // The current world's lifetime stamp, or 0 without an identity. Every revoke, load, NewGame and
    // activation changes it, so it also separates two lifetimes of the same restored UUID.
    [[nodiscard]] std::uint64_t lifetime() const {
        std::scoped_lock lock{mutex_};
        return identity_ ? serial_ : 0;
    }

    // The shared playthrough_id: the resolved UUID, or nothing until a successful NewGame/load resolves it.
    [[nodiscard]] std::optional<std::string> playthrough_id() const {
        std::optional<PlaythroughUuid> identity;
        {
            std::scoped_lock lock{mutex_};
            identity = identity_;
        }
        return identity ? std::optional{uuid_text(*identity)} : std::nullopt;
    }

    // Only the resolved identity of the same world lifetime binds. A same-world replacement inherits the
    // latest ACK under a new writer epoch.
    [[nodiscard]] std::optional<Binding> bind(std::string_view playthrough, std::uint64_t lifetime) {
        std::optional<SavedContextBytes> loaded;
        std::uint64_t epoch;
        {
            std::scoped_lock lock{mutex_};
            if (!identity_ || lifetime == 0 || lifetime != serial_ || playthrough.size() != 36 ||
                !std::equal(playthrough.begin(), playthrough.end(), owner_.begin())) return std::nullopt;
            epoch = ++epoch_;
            bound_ = true;
            // Replacements inherit the latest ACK; existing copied bindings remain immutable.
            loaded = latest_;
        }
        auto context = loaded ? decode_saved_context(*loaded) : std::nullopt;
        return Binding{epoch, std::move(context)};
    }

    void stop_writer(std::uint64_t epoch) {
        std::scoped_lock lock{mutex_};
        if (epoch == epoch_) { ++epoch_; bound_ = false; }
    }

    // A stale, stopped or reloaded writer must not initialize, publish or dirty a binding it no longer owns.
    [[nodiscard]] bool writer_current(std::uint64_t epoch) const {
        std::scoped_lock lock{mutex_};
        return epoch == epoch_ && bound_;
    }

    [[nodiscard]] bool acknowledge(std::uint64_t epoch, const SavedContext& context) {
        const auto bytes = encode_saved_context(context);
        if (!bytes) return false;
        std::scoped_lock lock{mutex_};
        if (epoch != epoch_ || !bound_ || !identity_ || !std::equal(owner_.begin(), owner_.end(), bytes->begin())) return false;
        // A later ACK must not be replaced if workers finish their bookkeeping out of order.
        if (latest_ && std::equal(bytes->begin(), bytes->begin() + 266, latest_->begin())) {
            std::uint64_t previous{};
            for (std::size_t i = 0; i < 8; ++i) previous |= std::uint64_t{(*latest_)[266 + i]} << (i * 8);
            if (context.context_sequence <= previous) return false;
        }
        latest_ = bytes;
        return true;
    }

    // The identity persists on every normal save, with or without a session, network or ACK.
    [[nodiscard]] std::optional<SavedRecordBytes> for_save() const {
        SavedRecord record{lane_};
        {
            std::scoped_lock lock{mutex_};
            if (!identity_) return std::nullopt;
            record.uuid = *identity_;
            record.anchor = latest_;
        }
        return encode_saved_record(record);
    }

private:
    enum class Transaction : std::uint8_t { none, loading, resolving };
    struct Staged final {
        LoadedIdentity status{LoadedIdentity::missing};
        std::optional<SavedRecord> record;
    };

    // Only the exact transaction a callback entered accepts its outcome; a reader from an earlier load cannot
    // stage into a later one, and nothing stages once PostLoadGame consumed the load.
    void stage(std::uint64_t ticket, LoadedIdentity status, std::optional<SavedRecord> record) {
        if (record && record->lane != lane_) status = LoadedIdentity::wrong_lane;
        if (status != LoadedIdentity::restored) record.reset();
        else if (!record) status = LoadedIdentity::malformed;
        std::scoped_lock lock{mutex_};
        if (transaction_ != Transaction::loading || ticket != ticket_) return;
        --reading_;
        staged_ = {status, std::move(record)};
    }

    void revoke_locked() noexcept {
        ++epoch_;
        ++serial_;
        bound_ = false;
        identity_.reset();
        owner_ = {};
        latest_.reset();
        transaction_ = Transaction::none;
        ticket_ = 0;
        staged_ = {};
        callbacks_ = 0;
        reading_ = 0;
        contradicted_ = false;
    }

    // The load (or NewGame) is consumed: no callback stages, no second finish resolves, and any later
    // world change advances serial_ so that the pending fresh identity is discarded.
    void resolving_locked() noexcept {
        transaction_ = Transaction::resolving;
        ticket_ = 0;
        staged_ = {};
    }

    void commit_locked(const PlaythroughUuid& uuid, std::optional<SavedContextBytes> anchor) noexcept {
        ++epoch_;
        ++serial_;
        bound_ = false;
        identity_ = uuid;
        owner_ = uuid_owner(uuid);
        latest_ = std::move(anchor);
        transaction_ = Transaction::none;
    }

    template <class Random>
    ResolvedIdentity fresh(std::uint64_t serial, ResolvedIdentity result, Random&& random) {
        std::optional<PlaythroughUuid> bytes;
        try { bytes = random(); } catch (...) {}
        std::scoped_lock lock{mutex_};
        if (serial != serial_ || transaction_ != Transaction::resolving) return {IdentityResolution::stale, result.cause};
        if (!bytes) {
            transaction_ = Transaction::none;
            return {IdentityResolution::random_failed, result.cause};
        }
        commit_locked(uuid_v4(*bytes), std::nullopt);
        return result;
    }

    mutable std::mutex mutex_;
    const RuntimeVariant lane_;
    std::uint64_t epoch_{};
    std::uint64_t serial_{};
    std::uint64_t tickets_{};
    std::uint64_t ticket_{};
    bool bound_{};
    bool contradicted_{};
    Transaction transaction_{Transaction::none};
    Staged staged_;
    unsigned callbacks_{};
    unsigned reading_{};
    std::optional<PlaythroughUuid> identity_;
    PlaythroughOwner owner_{};
    std::optional<SavedContextBytes> latest_;
};

// Interface-shaped helpers allow hostile/truncated co-save records to be tested without Fallout.
template <class Serialization>
bool write_saved_context(const Serialization& serialization, const SaveContextStore& store) {
    const auto bytes = store.for_save();
    return !bytes || serialization.WriteRecord(saved_context_record_type, saved_context_record_version,
                                               bytes->data(), static_cast<std::uint32_t>(bytes->size()));
}

// Claims the open load at entry, then reads at most 64 records outside the store lock and stages one
// bounded outcome for PostLoadGame. A throw leaves the claim to stage a malformed outcome.
template <class Serialization>
void read_saved_context(const Serialization& serialization, SaveContextStore& store) {
    auto callback = store.enter_load();
    if (!callback.open()) return;
    std::optional<SavedRecord> restored;
    auto status = LoadedIdentity::missing;
    std::uint32_t type{}, version{}, length{};
    unsigned record = 0;
    for (; record < 64; ++record) {
        if (!serialization.GetNextRecordInfo(type, version, length)) break;
        if (type != saved_context_record_type) continue;
        if (status != LoadedIdentity::missing) { status = LoadedIdentity::duplicate; break; }
        status = LoadedIdentity::malformed;
        if (version == legacy_saved_context_record_version) { status = LoadedIdentity::legacy; break; }
        if (version != saved_context_record_version) { status = LoadedIdentity::unsupported; break; }
        if (length != SavedRecordBytes{}.size()) break;
        SavedRecordBytes bytes{};
        if (serialization.ReadRecordData(bytes.data(), length) != length) break;
        restored = decode_saved_record(bytes);
        if (!restored) break;
        status = LoadedIdentity::restored;
    }
    // Too many records, duplicates, legacy, unknown versions and malformed records leave no inferred identity.
    if (record == 64) status = LoadedIdentity::malformed;
    if (status != LoadedIdentity::restored) restored.reset();
    callback.stage(status, std::move(restored));
}

} // namespace synth::core
