#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace synth::core {

// Admits a native BookMenu open only after a later game-thread observation proves the same
// candidate is freshly and visibly displayed. The epoch and menu counters are atomics any thread
// may advance; the pending candidate is touched only by the game thread (open hook and pump).
class ReadableNoteGate final {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t max_body_bytes = 16 * 1024;
    static constexpr auto admission_window = std::chrono::seconds{2};

    // Stable base identity and the exact bounded body; origin and reference are not authenticated.
    struct Read final {
        std::uint64_t epoch{}, generation{};
        std::uint32_t book_form_id{};
        std::string body;
    };
    // Counter snapshot taken by the hook before the original open runs.
    struct Attempt final {
        std::uint64_t epoch{}, opens{}, closes{};
    };
    struct Display final {
        std::uint64_t generation{};
        bool on_stack{}, display_enabled{};
        std::uint32_t book_form_id{};
        std::string_view body;
    };

    // Scalar MenuOpenCloseEvent observation for BookMenu only; any thread, no engine access.
    void observe_menu(bool opening) noexcept { (opening ? opens_ : closes_).fetch_add(1, std::memory_order_acq_rel); }

    // Any thread, before anything else is inspected: every known attempt supersedes older work.
    Attempt begin_attempt() noexcept {
        const auto epoch = epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
        return {epoch, opens_.load(std::memory_order_acquire), closes_.load(std::memory_order_acquire)};
    }
    // Any thread (lifecycle, ownership loss): the game-thread owner drops the candidate later.
    void invalidate() noexcept { epoch_.fetch_add(1, std::memory_order_acq_rel); }

    // Game thread, after the original open returned exactly once. Rejected when a newer attempt or
    // invalidation happened meanwhile, the menu was already on stack, or the body is unsafe.
    bool stage(const Attempt& attempt, std::uint64_t generation, bool menu_was_open, std::uint32_t book,
               std::string_view body, Clock::time_point now) noexcept {
        if (attempt.epoch != epoch_.load(std::memory_order_acquire)) return false; // Never overwrite newer work.
        pending_.reset();
        if (menu_was_open || book == 0 || (book >> 24) == 0xFF || body.empty() || body.size() > max_body_bytes ||
            !valid_utf8(body))
            return false;
        try {
            pending_ = Pending{{attempt.epoch, generation, book, std::string{body}}, attempt.opens, attempt.closes,
                               now + admission_window};
        } catch (...) {
            pending_.reset();
            return false;
        }
        return true;
    }

    // Game-thread pump: emits at most once per epoch, or drops on any mismatch, close, timeout,
    // supersession or staleness. Waiting is allowed only before the menu opens.
    std::optional<Read> admit(const Display& display, Clock::time_point now) noexcept {
        if (!pending_) return {};
        auto& candidate = *pending_;
        const auto opens = opens_.load(std::memory_order_acquire);
        const auto closes = closes_.load(std::memory_order_acquire);
        if (candidate.read.epoch != epoch_.load(std::memory_order_acquire) || closes != candidate.closes ||
            now >= candidate.deadline || display.generation != candidate.read.generation ||
            opens > candidate.opens + 1) {
            pending_.reset();
            return {};
        }
        if (opens == candidate.opens) return {}; // Open requested; the menu has not opened yet.
        if (!display.on_stack || !display.display_enabled || display.book_form_id != candidate.read.book_form_id ||
            display.body != candidate.read.body) {
            pending_.reset();
            return {};
        }
        auto read = std::move(candidate.read);
        pending_.reset();
        return read;
    }

    // Game thread only.
    void drop() noexcept { pending_.reset(); }
    [[nodiscard]] bool pending() const noexcept { return pending_.has_value(); }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_.load(std::memory_order_acquire); }

    [[nodiscard]] static bool valid_utf8(std::string_view text) noexcept {
        for (std::size_t i = 0; i < text.size();) {
            const auto lead = static_cast<unsigned char>(text[i]);
            std::size_t length{};
            if (lead < 0x80) length = 1;
            else if ((lead >> 5) == 0x6) length = 2;
            else if ((lead >> 4) == 0xE) length = 3;
            else if ((lead >> 3) == 0x1E) length = 4;
            if (length == 0 || i + length > text.size() || lead == 0) return false;
            std::uint32_t code = length == 1 ? lead : lead & (0x7F >> length);
            for (std::size_t k = 1; k < length; ++k) {
                const auto next = static_cast<unsigned char>(text[i + k]);
                if ((next & 0xC0) != 0x80) return false;
                code = (code << 6) | (next & 0x3F);
            }
            constexpr std::uint32_t minimum[]{0, 0, 0x80, 0x800, 0x10000};
            if (code < minimum[length] || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
            i += length;
        }
        return true;
    }

private:
    struct Pending final {
        Read read;
        std::uint64_t opens{}, closes{};
        Clock::time_point deadline{};
    };
    std::atomic_uint64_t epoch_{}, opens_{}, closes_{};
    std::optional<Pending> pending_;
};

// Engine-free byte rules for the two exact rel32 call patches and their shared absolute stub
// (`jmp [rip+0]` followed by the 8-byte thunk, the pinned REL::ASM::JMP14 layout). Ownership is
// the complete known bytes at known addresses only; no patched displacement is ever followed.
struct ReadableNotePatch final {
    using Call = std::array<std::uint8_t, 5>;
    using Stub = std::array<std::uint8_t, 14>;
    enum class Restore { write_original, already_original, foreign };

    // The exact `call rel32` from site to stub, or nothing when the stub is out of rel32 range.
    [[nodiscard]] static std::optional<Call> call(std::uintptr_t site, std::uintptr_t stub) noexcept {
        const auto delta = static_cast<std::int64_t>(stub - (site + 5));
        if (delta < INT32_MIN || delta > INT32_MAX) return {};
        const auto displacement = static_cast<std::int32_t>(delta);
        Call bytes{0xE8};
        std::memcpy(bytes.data() + 1, &displacement, sizeof(displacement));
        return bytes;
    }
    [[nodiscard]] static Stub stub(std::uintptr_t target) noexcept {
        Stub bytes{0xFF, 0x25};
        const auto address = static_cast<std::uint64_t>(target);
        std::memcpy(bytes.data() + 6, &address, sizeof(address));
        return bytes;
    }
    // Both the site and the shared stub must still hold exactly what we installed.
    [[nodiscard]] static bool owned(const Call& site, const Call& patched, const Stub& current,
                                    const Stub& installed) noexcept {
        return site == patched && current == installed;
    }
    // Rollback rewrites only a site whose call and stub still prove our ownership; anything else is kept.
    [[nodiscard]] static Restore restore(const Call& site, const Call& original, const Call& patched,
                                         const Stub& current, const Stub& installed) noexcept {
        if (site == original) return Restore::already_original;
        return owned(site, patched, current, installed) ? Restore::write_original : Restore::foreign;
    }
};

} // namespace synth::core
