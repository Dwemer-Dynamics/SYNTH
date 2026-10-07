#pragma once

#include "core/cancellation.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace synth::tasks {

enum class TaskClass : unsigned char {
    background,
    current_turn,
};

enum class SubmitResult : unsigned char {
    accepted,
    saturated,
    stopped,
};

enum class ShutdownMode : unsigned char {
    drain,
    cancel_pending,
};

struct LaneConfig final {
    std::string name;
    std::size_t capacity{};
    std::size_t reserved_current_turn_capacity{};
    std::size_t maximum_current_turn_burst{3};
};

struct LaneHealth final {
    std::string name;
    std::size_t pending_background{};
    std::size_t pending_current_turn{};
    std::size_t capacity{};
    std::size_t reserved_current_turn_capacity{};
    std::size_t accepted{};
    std::size_t saturated{};
    std::size_t executed{};
    std::size_t cancelled{};
    std::size_t expired{};
    std::size_t failed{};
    std::size_t discarded{};
};

struct TaskLanesHealth final {
    std::vector<LaneHealth> lanes;
    bool accepting{};
    bool joined{};
};

class TaskLanes final {
public:
    using Clock = std::chrono::steady_clock;
    using Deadline = Clock::time_point;
    using Task = std::function<void(const core::CancellationToken&)>;
    using NowFunction = std::function<Deadline()>;

    // Keep admission-time activity balanced even when a task never executes. The
    // counter must outlive every copy of the returned task and the owning lanes.
    [[nodiscard]] static Task track_activity(std::atomic_size_t& counter, Task task) {
        if (!task) throw std::invalid_argument{"activity tracking requires a task"};
        struct Activity final {
            explicit Activity(std::atomic_size_t& value) noexcept : counter{value} {
                counter.fetch_add(1, std::memory_order_acq_rel);
            }
            ~Activity() { counter.fetch_sub(1, std::memory_order_acq_rel); }
            std::atomic_size_t& counter;
        };
        return [activity = std::make_shared<Activity>(counter), task = std::move(task)](
                   const core::CancellationToken& cancellation) { task(cancellation); };
    }

    TaskLanes(std::vector<LaneConfig> configs,
              std::size_t worker_count,
              NowFunction now = [] { return Clock::now(); })
        : now_{std::move(now)} {
        if (configs.empty() || !now_) {
            throw std::invalid_argument{"task lanes require configurations and a clock"};
        }

        lanes_.reserve(configs.size());
        for (auto& config : configs) {
            if (config.name.empty() || config.capacity == 0 ||
                config.reserved_current_turn_capacity > config.capacity ||
                config.maximum_current_turn_burst == 0 || lane_indices_.contains(config.name)) {
                throw std::invalid_argument{"invalid or duplicate task lane configuration"};
            }
            lane_indices_.emplace(config.name, lanes_.size());
            lanes_.push_back(Lane{std::move(config)});
        }

        workers_.reserve(worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    TaskLanes(const TaskLanes&) = delete;
    TaskLanes& operator=(const TaskLanes&) = delete;

    ~TaskLanes() { shutdown(ShutdownMode::cancel_pending); }

    [[nodiscard]] SubmitResult try_submit(std::string_view lane_name,
                                          TaskClass task_class,
                                          Deadline deadline,
                                          core::CancellationToken cancellation,
                                          Task task) {
        if (!task) {
            throw std::invalid_argument{"task lane submission requires a task"};
        }
        // An empty token is already cancelled. Reject this wiring error at
        // admission instead of reporting accepted and silently dropping work.
        if (!cancellation.generation().valid()) {
            throw std::invalid_argument{"task lane submission requires a generation-bound cancellation token"};
        }

        std::unique_lock lock{mutex_};
        const auto found = lane_indices_.find(std::string{lane_name});
        if (found == lane_indices_.end()) {
            throw std::invalid_argument{"unknown task lane"};
        }
        auto& lane = lanes_[found->second];
        if (!accepting_) {
            return SubmitResult::stopped;
        }

        const auto pending = lane.background.size() + lane.current_turn.size();
        const auto admission_limit = task_class == TaskClass::current_turn
                                         ? lane.config.capacity
                                         : lane.config.capacity -
                                               lane.config.reserved_current_turn_capacity;
        if (pending >= admission_limit) {
            ++lane.saturated;
            return SubmitResult::saturated;
        }

        auto& queue = task_class == TaskClass::current_turn ? lane.current_turn : lane.background;
        queue.push_back(Item{deadline, std::move(cancellation), std::move(task)});
        ++lane.accepted;
        lock.unlock();
        condition_.notify_one();
        return SubmitResult::accepted;
    }

    // A zero-worker instance is a deterministic executor for unit tests and embedding loops.
    [[nodiscard]] bool run_one() {
        if (!workers_.empty()) {
            throw std::logic_error{"run_one is available only when no worker threads were requested"};
        }

        Work work;
        {
            std::scoped_lock lock{mutex_};
            if (!take_next_locked(work)) {
                return false;
            }
        }
        execute(std::move(work));
        return true;
    }

    [[nodiscard]] std::size_t run_until_idle() {
        std::size_t count{};
        while (run_one()) {
            ++count;
        }
        return count;
    }

    // Drops queued work without stopping the lanes so a terminal halt can be sent next.
    [[nodiscard]] std::size_t discard_pending() noexcept {
        std::scoped_lock lock{mutex_};
        std::size_t discarded{};
        for (auto& lane : lanes_) {
            const auto count = lane.background.size() + lane.current_turn.size();
            lane.discarded += count;
            discarded += count;
            lane.background.clear();
            lane.current_turn.clear();
        }
        return discarded;
    }

    // Reclaim only invalidated work; current input and session housekeeping survive.
    [[nodiscard]] std::size_t discard_cancelled() noexcept {
        std::scoped_lock lock{mutex_};
        std::size_t discarded{};
        for (auto& lane : lanes_) {
            const auto cancelled = [](const Item& item) { return item.cancellation.is_cancelled(); };
            const auto count = std::erase_if(lane.background, cancelled) +
                               std::erase_if(lane.current_turn, cancelled);
            lane.cancelled += count;
            discarded += count;
        }
        return discarded;
    }

    void shutdown(ShutdownMode mode = ShutdownMode::cancel_pending) noexcept {
        std::scoped_lock shutdown_lock{shutdown_mutex_};
        {
            std::scoped_lock lock{mutex_};
            if (accepting_) {
                accepting_ = false;
                if (mode == ShutdownMode::cancel_pending) {
                    for (auto& lane : lanes_) {
                        lane.discarded += lane.background.size() + lane.current_turn.size();
                        lane.background.clear();
                        lane.current_turn.clear();
                    }
                }
            }
        }
        condition_.notify_all();

        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        {
            std::scoped_lock lock{mutex_};
            joined_ = true;
        }
    }

    [[nodiscard]] TaskLanesHealth health() const {
        std::scoped_lock lock{mutex_};
        TaskLanesHealth result;
        result.accepting = accepting_;
        result.joined = joined_;
        result.lanes.reserve(lanes_.size());
        for (const auto& lane : lanes_) {
            result.lanes.push_back(LaneHealth{
                .name = lane.config.name,
                .pending_background = lane.background.size(),
                .pending_current_turn = lane.current_turn.size(),
                .capacity = lane.config.capacity,
                .reserved_current_turn_capacity = lane.config.reserved_current_turn_capacity,
                .accepted = lane.accepted,
                .saturated = lane.saturated,
                .executed = lane.executed,
                .cancelled = lane.cancelled,
                .expired = lane.expired,
                .failed = lane.failed,
                .discarded = lane.discarded,
            });
        }
        return result;
    }

private:
    struct Item final {
        Deadline deadline;
        core::CancellationToken cancellation;
        Task task;
    };

    struct Lane final {
        explicit Lane(LaneConfig value) : config{std::move(value)} {}

        LaneConfig config;
        std::deque<Item> background;
        std::deque<Item> current_turn;
        std::size_t current_turn_streak{};
        std::size_t accepted{};
        std::size_t saturated{};
        std::size_t executed{};
        std::size_t cancelled{};
        std::size_t expired{};
        std::size_t failed{};
        std::size_t discarded{};
    };

    struct Work final {
        std::size_t lane_index{};
        Item item{};
    };

    [[nodiscard]] bool has_pending_locked() const noexcept {
        for (const auto& lane : lanes_) {
            if (!lane.background.empty() || !lane.current_turn.empty()) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool take_next_locked(Work& work) {
        for (std::size_t offset = 0; offset < lanes_.size(); ++offset) {
            const auto index = (next_lane_ + offset) % lanes_.size();
            auto& lane = lanes_[index];
            if (lane.background.empty() && lane.current_turn.empty()) {
                continue;
            }

            const auto choose_background =
                !lane.background.empty() &&
                (lane.current_turn.empty() ||
                 lane.current_turn_streak >= lane.config.maximum_current_turn_burst);
            auto& queue = choose_background ? lane.background : lane.current_turn;
            if (choose_background) {
                lane.current_turn_streak = 0;
            } else {
                ++lane.current_turn_streak;
            }

            work.lane_index = index;
            work.item = std::move(queue.front());
            queue.pop_front();
            next_lane_ = (index + 1) % lanes_.size();
            return true;
        }
        return false;
    }

    void execute(Work work) noexcept {
        enum class Outcome : unsigned char { executed, cancelled, expired, failed };
        Outcome outcome{};
        if (work.item.cancellation.is_cancelled()) {
            outcome = Outcome::cancelled;
        } else if (now_() >= work.item.deadline) {
            outcome = Outcome::expired;
        } else {
            try {
                work.item.task(work.item.cancellation);
                outcome = Outcome::executed;
            } catch (...) {
                outcome = Outcome::failed;
            }
        }

        std::scoped_lock lock{mutex_};
        auto& lane = lanes_[work.lane_index];
        switch (outcome) {
        case Outcome::executed:
            ++lane.executed;
            break;
        case Outcome::cancelled:
            ++lane.cancelled;
            break;
        case Outcome::expired:
            ++lane.expired;
            break;
        case Outcome::failed:
            ++lane.failed;
            break;
        }
    }

    void worker_loop() noexcept {
        for (;;) {
            Work work;
            {
                std::unique_lock lock{mutex_};
                condition_.wait(lock, [this] { return !accepting_ || has_pending_locked(); });
                if (!take_next_locked(work)) {
                    if (!accepting_) {
                        return;
                    }
                    continue;
                }
            }
            execute(std::move(work));
        }
    }

    NowFunction now_;
    std::vector<Lane> lanes_;
    std::unordered_map<std::string, std::size_t> lane_indices_;
    std::vector<std::thread> workers_;

    mutable std::mutex mutex_;
    std::mutex shutdown_mutex_;
    std::condition_variable condition_;
    std::size_t next_lane_{};
    bool accepting_{true};
    bool joined_{};
};

}  // namespace synth::tasks
