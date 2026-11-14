#pragma once

#include "core/event_witness.hpp"
#include "core/snapshot.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace synth::client {

// One session/generation's first-observation captures, each with the immutable scene it was granted in.
// The game pump only attempts admission; workers prepare, publish and finish. Overflow drops, never waits.
class DialogueObservationDelivery final {
public:
    static constexpr std::size_t capacity = 4;
    struct Capture final {
        std::shared_ptr<const core::RuntimeSnapshot> scene; // Speaker first, then exactly the witnesses.
        core::DialogueObservation observation;
    };
    using Prepared = std::shared_ptr<const Capture>;

    explicit DialogueObservationDelivery(core::RuntimeGeneration generation) : generation_{generation} {}
    void stop() noexcept { stopped_.store(true, std::memory_order_release); }
    [[nodiscard]] bool pending() const noexcept { return pending_.load(std::memory_order_acquire) != 0; }
    [[nodiscard]] bool full() const noexcept { return pending_.load(std::memory_order_acquire) >= capacity; }
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

    [[nodiscard]] bool enqueue(Prepared capture) {
        if (!capture || !capture->scene || capture->scene->generation() != generation_ ||
            capture->scene->variant() != core::RuntimeVariant::flat || !capture->observation.valid() ||
            capture->scene->actors().size() != capture->observation.witnesses.actors.size() + 1 ||
            stopped_.load(std::memory_order_acquire)) return false;
        std::unique_lock lock{mutex_, std::try_to_lock};
        if (!lock.owns_lock() || pending_.load(std::memory_order_relaxed) == capacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (stopped_.load(std::memory_order_acquire)) return false;
        slots_[(head_ + pending_.load(std::memory_order_relaxed)) % capacity] = std::move(capture);
        pending_.fetch_add(1, std::memory_order_release);
        return true;
    }

    // Repeated calls return the same capture until finish(); a replay carries the original id and scene.
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
