#pragma once

#include "context/context.hpp"

#include <chrono>
#include <compare>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>

namespace synth::actions {

using ActionClock = std::chrono::steady_clock;
inline constexpr std::uint64_t maximum_wire_integer = 9'007'199'254'740'991ULL;

class ResponseGeneration final {
public:
    using value_type = std::uint64_t;

    [[nodiscard]] static constexpr ResponseGeneration from_value(value_type value) {
        if (value == 0 || value > maximum_wire_integer) {
            throw std::invalid_argument{"response generation is outside the wire-safe range"};
        }
        return ResponseGeneration{value};
    }

    [[nodiscard]] constexpr value_type value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
    auto operator<=>(const ResponseGeneration&) const = default;

private:
    explicit constexpr ResponseGeneration(value_type value) noexcept : value_{value} {}
    value_type value_{};
};

struct ActionCorrelation final {
    std::string request_id;
    std::string action_id;
    core::RuntimeGeneration runtime_generation;
    ResponseGeneration response_generation;

    auto operator<=>(const ActionCorrelation&) const = default;
};

struct InspectActorIntent final {
    ActionCorrelation correlation;
    context::ActorIdentity actor;
    ActionClock::time_point deadline;
};

struct UnsupportedIntent final {
    ActionCorrelation correlation;
    std::string action;
    ActionClock::time_point deadline;
};

using ActionIntent = std::variant<InspectActorIntent, UnsupportedIntent>;

enum class TerminalStatus : unsigned char {
    success,
    rejected,
    unavailable,
    timeout,
    cancelled,
    failed,
};

struct InspectActorData final {
    context::ActorIdentity actor;
    std::string display_name;
    core::Vec3 position;

    auto operator<=>(const InspectActorData&) const = default;
};

struct ActionResult final {
    ActionCorrelation correlation;
    TerminalStatus status{TerminalStatus::failed};
    std::string detail;
    std::optional<InspectActorData> inspection;

    auto operator<=>(const ActionResult&) const = default;
};

struct CancellationView final {
    std::function<bool()> cancelled;

    [[nodiscard]] bool is_cancelled() const { return cancelled && cancelled(); }
};

struct InspectActorSnapshot final {
    context::ActorIdentity identity;
    std::string display_name;
    core::Vec3 position;
};

enum class InspectExecutionStatus : unsigned char { success, unavailable, rejected, failed };

struct InspectExecution final {
    InspectExecutionStatus status{InspectExecutionStatus::failed};
    std::optional<InspectActorSnapshot> actor;
    std::string detail;
};

class IInspectActorExecutor {
public:
    virtual ~IInspectActorExecutor() = default;
    [[nodiscard]] virtual InspectExecution inspect_actor(const context::ActorIdentity& actor) = 0;
};

class ActionProcessor final {
public:
    using NowFunction = std::function<ActionClock::time_point()>;

    explicit ActionProcessor(std::size_t replay_capacity,
                             NowFunction now = [] { return ActionClock::now(); })
        : replay_capacity_{replay_capacity}, now_{std::move(now)} {
        if (replay_capacity_ == 0 || !now_) {
            throw std::invalid_argument{"action processor requires a capacity and clock"};
        }
    }

    [[nodiscard]] ActionResult process(const ActionIntent& intent,
                                       core::RuntimeGeneration current_runtime_generation,
                                       ResponseGeneration current_response_generation,
                                       const CancellationView& cancellation,
                                       IInspectActorExecutor& executor) {
        const auto correlation = std::visit([](const auto& value) { return value.correlation; }, intent);
        validate_correlation(correlation);
        const auto key = correlation_key(correlation);
        {
            std::scoped_lock lock{mutex_};
            if (active_generation_ != current_runtime_generation) {
                terminal_.clear();
                replay_order_.clear();
                active_generation_ = current_runtime_generation;
            }
            if (const auto found = terminal_.find(key); found != terminal_.end()) {
                return found->second;
            }
        }

        if (correlation.runtime_generation != current_runtime_generation ||
            correlation.response_generation != current_response_generation) {
            return store(key, make_result(correlation, TerminalStatus::rejected,
                                          "correlation generation mismatch"));
        }
        const auto deadline = std::visit([](const auto& value) { return value.deadline; }, intent);
        if (cancellation.is_cancelled()) {
            return store(key, make_result(correlation, TerminalStatus::cancelled, "action cancelled"));
        }
        if (now_() >= deadline) {
            return store(key, make_result(correlation, TerminalStatus::timeout, "action deadline expired"));
        }
        if (const auto* unsupported = std::get_if<UnsupportedIntent>(&intent)) {
            (void)unsupported;
            return store(key, make_result(correlation, TerminalStatus::rejected,
                                          "action is not allowlisted"));
        }

        const auto& inspect = std::get<InspectActorIntent>(intent);
        InspectExecution execution;
        try {
            execution = executor.inspect_actor(inspect.actor);
        } catch (...) {
            return store(key, make_result(correlation, TerminalStatus::failed,
                                          "inspect_actor executor threw"));
        }
        if (cancellation.is_cancelled()) {
            return store(key, make_result(correlation, TerminalStatus::cancelled, "action cancelled"));
        }
        if (now_() >= deadline) {
            return store(key, make_result(correlation, TerminalStatus::timeout, "action deadline expired"));
        }

        switch (execution.status) {
        case InspectExecutionStatus::success:
            if (!execution.actor || execution.actor->identity != inspect.actor ||
                execution.actor->display_name.empty() || !execution.actor->position.finite()) {
                return store(key, make_result(correlation, TerminalStatus::failed,
                                              "inspect_actor returned mismatched data"));
            }
            return store(key,
                         ActionResult{correlation,
                                      TerminalStatus::success,
                                      execution.detail,
                                      InspectActorData{execution.actor->identity,
                                                       execution.actor->display_name,
                                                       execution.actor->position}});
        case InspectExecutionStatus::unavailable:
            return store(key, make_result(correlation, TerminalStatus::unavailable, execution.detail));
        case InspectExecutionStatus::rejected:
            return store(key, make_result(correlation, TerminalStatus::rejected, execution.detail));
        case InspectExecutionStatus::failed:
            return store(key, make_result(correlation, TerminalStatus::failed, execution.detail));
        }
        return store(key, make_result(correlation, TerminalStatus::failed, "invalid executor result"));
    }

    [[nodiscard]] std::size_t terminal_count() const noexcept {
        std::scoped_lock lock{mutex_};
        return terminal_.size();
    }

private:
    static void validate_correlation(const ActionCorrelation& correlation) {
        if (correlation.request_id.empty() || correlation.request_id.size() > 128 ||
            correlation.action_id.empty() || correlation.action_id.size() > 128 ||
            !correlation.runtime_generation.valid() || !correlation.response_generation.valid()) {
            throw std::invalid_argument{"action correlation is invalid"};
        }
    }

    [[nodiscard]] static std::string correlation_key(const ActionCorrelation& correlation) {
        return correlation.request_id + '\x1f' + correlation.action_id + '\x1f' +
               std::to_string(correlation.runtime_generation.value()) + '\x1f' +
               std::to_string(correlation.response_generation.value());
    }

    [[nodiscard]] static ActionResult make_result(ActionCorrelation correlation,
                                                  TerminalStatus status,
                                                  std::string detail) {
        return ActionResult{std::move(correlation), status, std::move(detail), std::nullopt};
    }

    [[nodiscard]] ActionResult store(const std::string& key, ActionResult result) {
        std::scoped_lock lock{mutex_};
        if (const auto found = terminal_.find(key); found != terminal_.end()) {
            return found->second;
        }
        while (terminal_.size() >= replay_capacity_) {
            terminal_.erase(replay_order_.front());
            replay_order_.pop_front();
        }
        terminal_.emplace(key, result);
        replay_order_.push_back(key);
        return result;
    }

    std::size_t replay_capacity_;
    NowFunction now_;
    mutable std::mutex mutex_;
    core::RuntimeGeneration active_generation_{};
    std::list<std::string> replay_order_;
    std::unordered_map<std::string, ActionResult> terminal_;
};

}  // namespace synth::actions
