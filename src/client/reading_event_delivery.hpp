#pragma once

#include "core/reading_event.hpp"
#include "core/snapshot.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

namespace synth::client {

// One generation's bounded readings, each with the immutable scene it happened in. The game pump only
// attempts admission; the worker prepares, publishes and finishes. Overflow drops, never waits.
class ReadingEventDelivery final {
public:
    using Snapshot = std::shared_ptr<const core::RuntimeSnapshot>;
    static constexpr std::size_t capacity = 4;
    struct Capture final {
        Snapshot scene;
        core::ReadingEvent event;
    };
    using Prepared = std::shared_ptr<const Capture>;

    explicit ReadingEventDelivery(core::RuntimeGeneration generation) : generation_{generation} {}
    void stop() noexcept { stopped_.store(true, std::memory_order_release); }
    [[nodiscard]] bool pending() const noexcept { return pending_.load(std::memory_order_acquire) != 0; }
    [[nodiscard]] bool full() const noexcept { return pending_.load(std::memory_order_acquire) >= capacity; }
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

    [[nodiscard]] bool enqueue(Snapshot scene, core::ReadingEvent event) {
        if (!scene || scene->variant() != core::RuntimeVariant::flat || scene->generation() != generation_ ||
            !event.valid() || stopped_.load(std::memory_order_acquire)) return false;
        std::unique_lock lock{mutex_, std::try_to_lock};
        if (!lock.owns_lock() || pending_.load(std::memory_order_relaxed) == capacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (stopped_.load(std::memory_order_acquire)) return false;
        auto capture = std::make_shared<Capture>();
        capture->scene = std::move(scene);
        capture->event = std::move(event);
        slots_[(head_ + pending_.load(std::memory_order_relaxed)) % capacity] = std::move(capture);
        pending_.fetch_add(1, std::memory_order_release);
        return true;
    }

    // Repeated calls return the same capture until finish(); a retry carries the original id and scene.
    [[nodiscard]] Prepared prepare() {
        std::scoped_lock lock{mutex_};
        if (stopped_.load(std::memory_order_acquire) || pending_.load(std::memory_order_relaxed) == 0) return {};
        return slots_[head_];
    }

    [[nodiscard]] bool finish(const Prepared& capture, bool acknowledged) {
        std::scoped_lock lock{mutex_};
        if (stopped_.load(std::memory_order_acquire) || !capture || pending_.load(std::memory_order_relaxed) == 0 ||
            capture != slots_[head_]) return false;
        if (!acknowledged) dropped_.fetch_add(1, std::memory_order_relaxed);
        slots_[head_].reset();
        head_ = (head_ + 1) % capacity;
        pending_.fetch_sub(1, std::memory_order_release);
        return true;
    }

private:
    const core::RuntimeGeneration generation_;
    std::atomic_bool stopped_{};
    std::atomic_size_t pending_{};
    std::atomic_uint64_t dropped_{};
    std::mutex mutex_;
    std::array<Prepared, capacity> slots_{};
    std::size_t head_{};
};

} // namespace synth::client
