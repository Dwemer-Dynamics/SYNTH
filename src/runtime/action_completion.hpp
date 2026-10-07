#pragma once

#include "runtime/fallout_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>

namespace synth::runtime {

// Freeze the execution window on receipt; presentation and worker queues cannot renew it.
[[nodiscard]] inline std::chrono::steady_clock::time_point action_start_deadline(
    std::uint64_t wire_ms, std::uint64_t request_timeout_ms,
    std::chrono::steady_clock::time_point received = std::chrono::steady_clock::now()) {
    return received + std::chrono::milliseconds{std::min<std::uint64_t>({wire_ms, request_timeout_ms, 120000})};
}

struct ActionCompletion final {
    enum class Phase : unsigned char { pending, executing, abandoned, complete };
    std::mutex mutex;
    std::condition_variable changed;
    Phase phase{Phase::pending};
    std::optional<RuntimeActionResult> result;

    // Claim once before installing a multi-frame operation. No lock is held across frames/native calls.
    // Its frame pump must still enforce the original token/deadline and finish after cleanup on every exit.
    [[nodiscard]] bool begin(const core::CancellationToken& cancellation,
                            std::chrono::steady_clock::time_point deadline,
                            std::optional<std::chrono::steady_clock::time_point> now = std::nullopt) {
        bool expired{};
        {
            std::scoped_lock lock{mutex};
            if (cancellation.is_cancelled() || phase != Phase::pending) return false;
            expired = now.value_or(std::chrono::steady_clock::now()) >= deadline;
            if (expired) {
                result.emplace(RuntimeActionResult{RuntimeActionStatus::timed_out,
                    "action expired before game-thread dispatch"});
                phase = Phase::complete;
            } else {
                phase = Phase::executing;
            }
        }
        if (expired) changed.notify_one();
        return !expired;
    }

    // Publish once after execution and any owned cleanup; cancellation must not erase mutation evidence.
    [[nodiscard]] bool finish(RuntimeActionResult completed) {
        {
            std::scoped_lock lock{mutex};
            if (phase != Phase::executing) return false;
            result.emplace(std::move(completed));
            phase = Phase::complete;
        }
        changed.notify_one();
        return true;
    }

    // Synchronous actions use the same claim/publication rules as future frame-spanning operations.
    template <class Execute>
    void execute(Execute&& operation, const core::CancellationToken& cancellation,
                 std::chrono::steady_clock::time_point deadline,
                 std::optional<std::chrono::steady_clock::time_point> now = std::nullopt) {
        if (!begin(cancellation, deadline, now)) return;
        RuntimeActionResult completed;
        try {
            completed = std::forward<Execute>(operation)();
        } catch (const std::exception& error) {
            completed = {RuntimeActionStatus::failed, error.what()};
        } catch (...) {
            completed = {RuntimeActionStatus::failed, "Fallout runtime action failed"};
        }
        (void)finish(std::move(completed));
    }
};

} // namespace synth::runtime
