#pragma once

#include "adapter_descriptor.hpp"

#include <atomic>
#include <cstdint>

namespace synth::adapters {

enum class LifecycleState : std::uint8_t {
    unloaded,
    loaded,
    ready,
    stopping,
    stopped,
};

enum class LifecycleEvent : std::uint8_t {
    plugin_loaded,
    game_data_ready,
    pre_load_game,
    new_game,
    post_load_game,
    return_to_main_menu,
    shutdown,
};

enum class LifecycleAction : std::uint8_t {
    rejected,
    none,
    ready,
    invalidate,
    invalidate_and_ready,
    stop,
};

// Coalesces callback-time lifecycle work for execution by the game-thread runtime pump.
class DeferredLifecycleActions final {
public:
    void push(LifecycleAction action) noexcept {
        const auto flags = flags_for(action);
        if (flags != 0) pending_.fetch_or(flags, std::memory_order_release);
    }

    [[nodiscard]] LifecycleAction take() noexcept {
        const auto flags = pending_.exchange(0, std::memory_order_acq_rel);
        if ((flags & stop_flag) != 0) return LifecycleAction::stop;
        if ((flags & invalidate_flag) != 0 && (flags & ready_flag) != 0) {
            return LifecycleAction::invalidate_and_ready;
        }
        if ((flags & invalidate_flag) != 0) return LifecycleAction::invalidate;
        if ((flags & ready_flag) != 0) return LifecycleAction::ready;
        return LifecycleAction::none;
    }

private:
    static constexpr std::uint8_t ready_flag = 1U << 0U;
    static constexpr std::uint8_t invalidate_flag = 1U << 1U;
    static constexpr std::uint8_t stop_flag = 1U << 2U;

    [[nodiscard]] static constexpr std::uint8_t flags_for(LifecycleAction action) noexcept {
        switch (action) {
        case LifecycleAction::ready:
            return ready_flag;
        case LifecycleAction::invalidate:
            return invalidate_flag;
        case LifecycleAction::invalidate_and_ready:
            return ready_flag | invalidate_flag;
        case LifecycleAction::stop:
            return stop_flag;
        case LifecycleAction::rejected:
        case LifecycleAction::none:
            return 0;
        }
        return 0;
    }

    std::atomic_uint8_t pending_{};
};

class AdapterLifecycle final {
public:
    [[nodiscard]] LifecycleState state() const noexcept { return state_; }

    [[nodiscard]] bool load(const AdapterDescriptor& descriptor,
                            core::RuntimeVariant detected_variant,
                            Version detected_runtime,
                            Version detected_script_extender) noexcept {
        if (state_ != LifecycleState::unloaded ||
            !supports_exact_environment(
                descriptor, detected_variant, detected_runtime, detected_script_extender)) {
            return false;
        }
        state_ = LifecycleState::loaded;
        return true;
    }

    [[nodiscard]] LifecycleAction handle(LifecycleEvent event) noexcept {
        switch (event) {
        case LifecycleEvent::plugin_loaded:
            return state_ == LifecycleState::loaded ? LifecycleAction::none
                                                    : LifecycleAction::rejected;
        case LifecycleEvent::game_data_ready:
            if (state_ != LifecycleState::loaded) {
                return LifecycleAction::rejected;
            }
            // Loaded forms do not imply that a playable player, cell, or camera exists yet.
            return LifecycleAction::none;
        case LifecycleEvent::post_load_game:
            if (state_ != LifecycleState::loaded) {
                return LifecycleAction::rejected;
            }
            state_ = LifecycleState::ready;
            return LifecycleAction::ready;
        case LifecycleEvent::new_game:
            if (state_ != LifecycleState::loaded && state_ != LifecycleState::ready) {
                return LifecycleAction::rejected;
            }
            state_ = LifecycleState::ready;
            return LifecycleAction::invalidate_and_ready;
        case LifecycleEvent::pre_load_game:
        case LifecycleEvent::return_to_main_menu:
            if (state_ != LifecycleState::loaded && state_ != LifecycleState::ready) {
                return LifecycleAction::rejected;
            }
            state_ = LifecycleState::loaded;
            return LifecycleAction::invalidate;
        case LifecycleEvent::shutdown:
            if (state_ == LifecycleState::unloaded || state_ == LifecycleState::stopped) {
                return LifecycleAction::rejected;
            }
            state_ = LifecycleState::stopping;
            state_ = LifecycleState::stopped;
            return LifecycleAction::stop;
        }
        return LifecycleAction::rejected;
    }

private:
    // F4SE publishes load events while the engine pump reads readiness.
    std::atomic<LifecycleState> state_{LifecycleState::unloaded};
};

}  // namespace synth::adapters
