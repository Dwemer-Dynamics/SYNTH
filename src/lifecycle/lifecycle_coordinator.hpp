#pragma once

#include "core/cancellation.hpp"
#include "core/generation.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace synth::lifecycle {

enum class OperationKind : unsigned char {
    network,
    media,
    action,
};

enum class InvalidationReason : unsigned char {
    new_turn,
    load,
    new_game,
    main_menu,
    shutdown,
};

enum class RegistrationResult : unsigned char {
    accepted,
    duplicate,
    requires_initialization,
    shutdown,
};

enum class CompletionResult : unsigned char {
    accepted,
    unknown_operation,
    stale_runtime_generation,
    stale_response_generation,
    cancelled,
};

class ResponseGeneration final {
public:
    using value_type = std::uint64_t;
    static constexpr value_type maximum_wire_value = 9'007'199'254'740'991ULL;

    constexpr ResponseGeneration() noexcept = default;

    [[nodiscard]] static constexpr ResponseGeneration initial() noexcept {
        return ResponseGeneration{1};
    }

    [[nodiscard]] static constexpr ResponseGeneration from_value(value_type value) {
        if (value == 0 || value > maximum_wire_value) {
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

struct GenerationStamp final {
    core::RuntimeGeneration runtime;
    ResponseGeneration response;

    auto operator<=>(const GenerationStamp&) const = default;
};

struct LifecycleHealth final {
    core::RuntimeGeneration runtime_generation;
    ResponseGeneration response_generation;
    std::size_t active_network{};
    std::size_t active_media{};
    std::size_t active_actions{};
    std::size_t registered{};
    std::size_t completed{};
    std::size_t cancelled{};
    std::size_t stale_completions{};
    std::size_t unknown_completions{};
    bool initialized{};
    bool shutdown{};
};

class LifecycleCoordinator final {
public:
    using CancelFunction = std::function<void()>;

    LifecycleCoordinator() = default;
    LifecycleCoordinator(const LifecycleCoordinator&) = delete;
    LifecycleCoordinator& operator=(const LifecycleCoordinator&) = delete;

    [[nodiscard]] bool initialize() {
        std::scoped_lock lock{mutex_};
        if (shutdown_) {
            return false;
        }
        initialized_ = true;
        return true;
    }

    [[nodiscard]] GenerationStamp stamp() const {
        std::scoped_lock lock{mutex_};
        return {runtime_generation_, response_generation_};
    }

    [[nodiscard]] GenerationStamp begin_new_turn() {
        std::vector<CancelFunction> callbacks;
        GenerationStamp result;
        {
            std::scoped_lock lock{mutex_};
            require_ready_locked();
            advance_response_locked();
            collect_cancellations_locked(callbacks);
            result = {runtime_generation_, response_generation_};
        }
        run_callbacks(callbacks);
        return result;
    }

    [[nodiscard]] GenerationStamp invalidate(InvalidationReason reason) {
        if (reason == InvalidationReason::new_turn) {
            return begin_new_turn();
        }

        std::vector<CancelFunction> callbacks;
        GenerationStamp result;
        {
            std::scoped_lock lock{mutex_};
            if (shutdown_) {
                return {runtime_generation_, response_generation_};
            }
            advance_runtime_locked();
            advance_response_locked();
            initialized_ = false;
            if (reason == InvalidationReason::shutdown) {
                shutdown_ = true;
            }
            collect_cancellations_locked(callbacks);
            result = {runtime_generation_, response_generation_};
        }
        run_callbacks(callbacks);
        return result;
    }

    [[nodiscard]] RegistrationResult register_operation(std::string id,
                                                        OperationKind kind,
                                                        GenerationStamp generation,
                                                        CancelFunction cancel = {}) {
        if (id.empty() || !generation.runtime.valid() || !generation.response.valid()) {
            throw std::invalid_argument{"lifecycle operation requires id and valid generations"};
        }

        std::scoped_lock lock{mutex_};
        if (shutdown_) {
            return RegistrationResult::shutdown;
        }
        if (!initialized_ || generation.runtime != runtime_generation_ ||
            generation.response != response_generation_) {
            return RegistrationResult::requires_initialization;
        }
        if (operations_.contains(id)) {
            return RegistrationResult::duplicate;
        }

        auto source = std::make_shared<core::CancellationSource>(generation.runtime);
        operations_.emplace(std::move(id),
                            Operation{kind, generation, std::move(source), std::move(cancel)});
        ++registered_;
        return RegistrationResult::accepted;
    }

    [[nodiscard]] core::CancellationToken cancellation_token(std::string_view id) const {
        std::scoped_lock lock{mutex_};
        const auto found = operations_.find(std::string{id});
        return found == operations_.end() ? core::CancellationToken{}
                                         : found->second.cancellation->token();
    }

    [[nodiscard]] CompletionResult complete(std::string_view id, GenerationStamp generation) {
        std::scoped_lock lock{mutex_};
        // Check the supplied stamp before lookup so completions for operations removed during
        // invalidation remain observably stale rather than becoming indistinguishable from typos.
        if (generation.runtime != runtime_generation_) {
            ++stale_completions_;
            return CompletionResult::stale_runtime_generation;
        }
        if (generation.response != response_generation_) {
            ++stale_completions_;
            return CompletionResult::stale_response_generation;
        }
        const auto found = operations_.find(std::string{id});
        if (found == operations_.end()) {
            ++unknown_completions_;
            return CompletionResult::unknown_operation;
        }
        if (generation.runtime != found->second.generation.runtime) {
            ++stale_completions_;
            return CompletionResult::stale_runtime_generation;
        }
        if (generation.response != found->second.generation.response) {
            ++stale_completions_;
            return CompletionResult::stale_response_generation;
        }
        if (found->second.cancellation->is_cancelled()) {
            operations_.erase(found);
            return CompletionResult::cancelled;
        }

        operations_.erase(found);
        ++completed_;
        return CompletionResult::accepted;
    }

    [[nodiscard]] LifecycleHealth health() const {
        std::scoped_lock lock{mutex_};
        LifecycleHealth health{
            .runtime_generation = runtime_generation_,
            .response_generation = response_generation_,
            .registered = registered_,
            .completed = completed_,
            .cancelled = cancelled_,
            .stale_completions = stale_completions_,
            .unknown_completions = unknown_completions_,
            .initialized = initialized_,
            .shutdown = shutdown_,
        };
        for (const auto& [id, operation] : operations_) {
            (void)id;
            switch (operation.kind) {
            case OperationKind::network:
                ++health.active_network;
                break;
            case OperationKind::media:
                ++health.active_media;
                break;
            case OperationKind::action:
                ++health.active_actions;
                break;
            }
        }
        return health;
    }

private:
    struct Operation final {
        OperationKind kind;
        GenerationStamp generation;
        std::shared_ptr<core::CancellationSource> cancellation;
        CancelFunction cancel;
    };

    void require_ready_locked() const {
        if (shutdown_ || !initialized_) {
            throw std::logic_error{"lifecycle coordinator requires initialization"};
        }
    }

    void advance_runtime_locked() {
        if (runtime_generation_.value() == core::RuntimeGeneration::maximum_wire_value) {
            throw std::overflow_error{"runtime generation exhausted"};
        }
        runtime_generation_ =
            core::RuntimeGeneration::from_value(runtime_generation_.value() + 1);
    }

    void advance_response_locked() {
        if (response_generation_.value() == ResponseGeneration::maximum_wire_value) {
            throw std::overflow_error{"response generation exhausted"};
        }
        response_generation_ =
            ResponseGeneration::from_value(response_generation_.value() + 1);
    }

    void collect_cancellations_locked(std::vector<CancelFunction>& callbacks) {
        callbacks.reserve(operations_.size());
        for (auto& [id, operation] : operations_) {
            (void)id;
            operation.cancellation->cancel();
            if (operation.cancel) {
                callbacks.push_back(std::move(operation.cancel));
            }
        }
        cancelled_ += operations_.size();
        operations_.clear();
    }

    static void run_callbacks(std::vector<CancelFunction>& callbacks) noexcept {
        for (auto& callback : callbacks) {
            try {
                callback();
            } catch (...) {
                // Cancellation is best-effort and must not prevent other operations from stopping.
            }
        }
    }

    mutable std::mutex mutex_;
    core::RuntimeGeneration runtime_generation_{core::RuntimeGeneration::initial()};
    ResponseGeneration response_generation_{ResponseGeneration::initial()};
    std::unordered_map<std::string, Operation> operations_;
    std::size_t registered_{};
    std::size_t completed_{};
    std::size_t cancelled_{};
    std::size_t stale_completions_{};
    std::size_t unknown_completions_{};
    bool initialized_{};
    bool shutdown_{};
};

}  // namespace synth::lifecycle
