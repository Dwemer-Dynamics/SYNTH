#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace synth::core {

// One copied handle, not an engine reference. A late callback cannot cross an epoch
// or overwrite a publication that completed since it entered the native handler.
class PickedReferenceMailbox final {
public:
    void arm() noexcept { state_.fetch_or(armed, std::memory_order_release); }
    void invalidate() noexcept {
        auto current = state_.load(std::memory_order_relaxed);
        while (!state_.compare_exchange_weak(current, (current & ~flags) + epoch_step,
                   std::memory_order_acq_rel, std::memory_order_relaxed)) {}
    }
    [[nodiscard]] std::uint64_t stamp() const noexcept { return state_.load(std::memory_order_acquire); }
    [[nodiscard]] bool record(std::uint64_t owner, std::uint32_t handle) noexcept {
        if ((owner & armed) == 0) return false;
        // Advance on every publication, even the same handle, so A -> B -> A
        // cannot make an older callback's compare/exchange current again.
        return state_.compare_exchange_strong(owner, ((owner & ~payload) + epoch_step) | observed | handle,
                                              std::memory_order_acq_rel, std::memory_order_acquire);
    }
    [[nodiscard]] std::optional<std::uint32_t> read() const noexcept {
        const auto current = stamp();
        if ((current & (armed | observed)) != (armed | observed)) return {};
        return static_cast<std::uint32_t>(current);
    }
    // Read publication identity and availability atomically for consumers interested only in change notification.
    [[nodiscard]] std::optional<std::uint64_t> observed_stamp() const noexcept {
        const auto current = stamp();
        if ((current & (armed | observed)) != (armed | observed)) return {};
        return current;
    }
private:
    static constexpr std::uint64_t observed = std::uint64_t{1} << 32;
    static constexpr std::uint64_t armed = observed << 1;
    static constexpr std::uint64_t payload = armed - 1, flags = armed * 2 - 1, epoch_step = armed * 2;
    static_assert(std::atomic_uint64_t::is_always_lock_free);
    std::atomic_uint64_t state_{};
};

} // namespace synth::core
