#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace synth::core {

enum class RestStart : std::uint64_t { sleep = 2, wait = 4 };

struct RestEventBatch final {
    static constexpr std::size_t capacity = 16;
    std::array<RestStart, capacity> events{};
    std::size_t size{};
};

// A packed FIFO keeps repeated starts without allocation; epochs fence callbacks inside an old handler.
class RestEventMailbox final {
public:
    void arm() noexcept { state_.fetch_or(armed, std::memory_order_release); }

    void invalidate() noexcept {
        auto current = state_.load(std::memory_order_relaxed);
        while (!state_.compare_exchange_weak(current, ((current & ~flags) + epoch_step),
                   std::memory_order_acq_rel, std::memory_order_relaxed)) {}
    }

    [[nodiscard]] std::uint64_t stamp() const noexcept {
        return state_.load(std::memory_order_acquire) & ~pending;
    }

    [[nodiscard]] bool record(std::uint64_t owner, RestStart event) noexcept {
        if ((owner & armed) == 0 || (event != RestStart::sleep && event != RestStart::wait)) return false;
        auto current = state_.load(std::memory_order_acquire);
        // Bound work inside an engine callback. Overflow drops the newest start, never an older queued start.
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            if ((current & ~pending) != owner) return false;
            const auto count = (current & count_mask) >> count_shift;
            if (count == RestEventBatch::capacity) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            auto next = current + count_step;
            if (event == RestStart::wait) next |= std::uint64_t{1} << count;
            if (state_.compare_exchange_weak(current, next,
                    std::memory_order_acq_rel, std::memory_order_acquire)) return true;
        }
        if ((current & ~pending) == owner) dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    [[nodiscard]] RestEventBatch take() noexcept {
        const auto previous = state_.fetch_and(~pending, std::memory_order_acq_rel);
        RestEventBatch result;
        if ((previous & armed) == 0) return result;
        result.size = static_cast<std::size_t>((previous & count_mask) >> count_shift);
        for (std::size_t i = 0; i < result.size; ++i)
            result.events[i] = (previous & (std::uint64_t{1} << i)) != 0 ? RestStart::wait : RestStart::sleep;
        return result;
    }

    // Process-wide health only: never use this counter as an event or associate it with a replacement save.
    [[nodiscard]] std::uint64_t dropped_total() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    static constexpr unsigned count_shift = 16;
    static constexpr std::uint64_t count_step = std::uint64_t{1} << count_shift;
    static constexpr std::uint64_t count_mask = 31 * count_step;
    static constexpr std::uint64_t armed = std::uint64_t{1} << 21;
    static constexpr std::uint64_t pending = armed - 1, flags = armed * 2 - 1, epoch_step = armed * 2;
    static_assert(std::atomic_uint64_t::is_always_lock_free);
    std::atomic_uint64_t state_{};
    std::atomic_uint64_t dropped_{};
};

} // namespace synth::core
