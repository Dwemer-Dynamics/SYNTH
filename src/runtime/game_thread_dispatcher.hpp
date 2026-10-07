#pragma once

#include "fallout_runtime.hpp"

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace synth::runtime {

enum class DispatchResult : unsigned char {
    accepted,
    stale,
    saturated,
    stopped,
};

struct DispatcherHealth final {
    std::size_t pending{};
    std::size_t capacity{};
    std::size_t executed{};
    std::size_t stale{};
    std::size_t saturated{};
    std::size_t failed{};
    std::size_t discarded{};
    bool accepting{};
};

class GameThreadDispatcher final {
public:
    using Command = std::function<void(IFalloutRuntime&)>;

    explicit GameThreadDispatcher(IFalloutRuntime& runtime, std::size_t capacity)
        : runtime_{runtime}, capacity_{capacity} {
        if (capacity == 0) {
            throw std::invalid_argument{"dispatcher capacity must be nonzero"};
        }
        runtime_.assert_game_thread();
    }

    [[nodiscard]] DispatchResult try_enqueue(core::RuntimeGeneration generation, Command command) {
        if (!generation.valid() || !command) {
            throw std::invalid_argument{"dispatcher item requires generation and command"};
        }

        std::scoped_lock lock{mutex_};
        if (!accepting_) {
            return DispatchResult::stopped;
        }
        const auto current = runtime_.generation();
        if (generation != current) {
            ++stale_;
            return DispatchResult::stale;
        }
        std::erase_if(items_, [current](const Item& item) { return item.generation != current; });
        if (items_.size() >= capacity_) {
            ++saturated_;
            return DispatchResult::saturated;
        }
        items_.push_back(Item{generation, std::move(command)});
        return DispatchResult::accepted;
    }

    [[nodiscard]] std::size_t drain() {
        runtime_.assert_game_thread();
        std::deque<Item> batch;
        {
            std::scoped_lock lock{mutex_};
            batch.swap(items_);
        }

        std::size_t drained{};
        for (auto& item : batch) {
            if (item.generation != runtime_.generation()) {
                std::scoped_lock lock{mutex_};
                ++stale_;
                continue;
            }
            try {
                item.command(runtime_);
                std::scoped_lock lock{mutex_};
                ++executed_;
                ++drained;
            } catch (...) {
                std::scoped_lock lock{mutex_};
                ++failed_;
            }
        }
        return drained;
    }

    void stop_accepting(bool discard_pending = false) {
        std::scoped_lock lock{mutex_};
        accepting_ = false;
        if (discard_pending) {
            discarded_ += items_.size();
            items_.clear();
        }
    }

    void discard_pending() {
        std::scoped_lock lock{mutex_};
        discarded_ += items_.size();
        items_.clear();
    }

    [[nodiscard]] DispatcherHealth health() const {
        std::scoped_lock lock{mutex_};
        return DispatcherHealth{
            .pending = items_.size(),
            .capacity = capacity_,
            .executed = executed_,
            .stale = stale_,
            .saturated = saturated_,
            .failed = failed_,
            .discarded = discarded_,
            .accepting = accepting_,
        };
    }

private:
    struct Item final {
        core::RuntimeGeneration generation{};
        Command command{};
    };

    IFalloutRuntime& runtime_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<Item> items_;
    std::size_t executed_{};
    std::size_t stale_{};
    std::size_t saturated_{};
    std::size_t failed_{};
    std::size_t discarded_{};
    bool accepting_{true};
};

}  // namespace synth::runtime
