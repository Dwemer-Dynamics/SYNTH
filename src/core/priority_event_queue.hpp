#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace synth::core {

enum class EventPriority : unsigned char {
    background,
    normal,
    player,
    halt,
};

enum class QueuePushResult : unsigned char {
    accepted,
    saturated,
    halted,
};

struct QueueHealth final {
    std::size_t size{};
    std::size_t capacity{};
    std::size_t accepted{};
    std::size_t saturated{};
    std::size_t rejected_after_halt{};
    std::size_t discarded_on_halt{};
    bool halted{};
};

template <class Event>
class BoundedPriorityEventQueue final {
public:
    BoundedPriorityEventQueue(std::size_t capacity,
                              std::size_t reserved_player_capacity,
                              std::size_t reserved_halt_capacity)
        : capacity_{capacity},
          reserved_player_capacity_{reserved_player_capacity},
          reserved_halt_capacity_{reserved_halt_capacity} {
        if (capacity == 0 || reserved_halt_capacity == 0 ||
            reserved_halt_capacity > capacity ||
            reserved_player_capacity > capacity - reserved_halt_capacity) {
            throw std::invalid_argument{"invalid event queue capacity reservation"};
        }
    }

    [[nodiscard]] QueuePushResult try_push(Event event, EventPriority priority) {
        std::scoped_lock lock{mutex_};

        if (halted_) {
            ++rejected_after_halt_;
            return QueuePushResult::halted;
        }

        if (priority == EventPriority::halt) {
            discarded_on_halt_ += size_;
            for (auto& queue : queues_) {
                queue.clear();
            }
            size_ = 0;
            queues_[index(EventPriority::halt)].push_back(std::move(event));
            size_ = 1;
            halted_ = true;
            ++accepted_;
            return QueuePushResult::accepted;
        }

        const auto admission_limit = priority == EventPriority::player
                                         ? capacity_ - reserved_halt_capacity_
                                         : capacity_ - reserved_halt_capacity_ -
                                               reserved_player_capacity_;
        if (size_ >= admission_limit) {
            ++saturated_;
            return QueuePushResult::saturated;
        }

        queues_[index(priority)].push_back(std::move(event));
        ++size_;
        ++accepted_;
        return QueuePushResult::accepted;
    }

    [[nodiscard]] std::optional<Event> try_pop() {
        std::scoped_lock lock{mutex_};
        for (const auto priority : {EventPriority::halt,
                                    EventPriority::player,
                                    EventPriority::normal,
                                    EventPriority::background}) {
            auto& queue = queues_[index(priority)];
            if (!queue.empty()) {
                Event event = std::move(queue.front());
                queue.pop_front();
                --size_;
                return event;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] QueueHealth health() const {
        std::scoped_lock lock{mutex_};
        return QueueHealth{
            .size = size_,
            .capacity = capacity_,
            .accepted = accepted_,
            .saturated = saturated_,
            .rejected_after_halt = rejected_after_halt_,
            .discarded_on_halt = discarded_on_halt_,
            .halted = halted_,
        };
    }

private:
    [[nodiscard]] static constexpr std::size_t index(EventPriority priority) noexcept {
        return static_cast<std::size_t>(priority);
    }

    const std::size_t capacity_;
    const std::size_t reserved_player_capacity_;
    const std::size_t reserved_halt_capacity_;

    mutable std::mutex mutex_;
    std::array<std::deque<Event>, 4> queues_;
    std::size_t size_{};
    std::size_t accepted_{};
    std::size_t saturated_{};
    std::size_t rejected_after_halt_{};
    std::size_t discarded_on_halt_{};
    bool halted_{};
};

}  // namespace synth::core
