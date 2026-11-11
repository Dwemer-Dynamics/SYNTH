#pragma once

#include "client/synth_client.hpp"
#include "config/config.hpp"
#include "core/snapshot.hpp"
#include "integration/prompt_target.hpp"
#include "targeting/targeting.hpp"
#include "tasks/task_lanes.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace synth::client {

// Owns one session's server control settings: the last confirmed mode/model-slot selection, the
// single pending request with its revision and deadline, and the worker RPC that acknowledges it,
// plus the control menu's retained target capture. Readers on the game/render thread copy the state
// under a leaf mutex that is never held across HTTP; the worker task reacquires it only to publish a
// result whose revision still matches. A timed-out request is retired by the next reader.
//
// Lifetimes: the borrowed configuration, client, network lane and terminal flag outlive this object.
// The queued RPC captures this object; the session joins the network lane in its destructor body
// before any member is destroyed, and a rejected, expired or discarded task is destroyed unrun.
class ServerControl final {
public:
    using Snapshot = std::shared_ptr<const core::RuntimeSnapshot>;

    struct State final {
        protocol_native::ControlSelection selection{"STANDARD", 1};
        std::string status;
        bool pending{};
        std::uint64_t revision{};
        tasks::TaskLanes::Clock::time_point deadline{};
    };

    struct Dependencies final {
        const config::Config& configuration;
        SynthClient& client;
        tasks::TaskLanes& network;
        const std::atomic_bool& terminal;
        core::CancellationToken session;
    };

    explicit ServerControl(Dependencies dependencies)
        : configuration_{dependencies.configuration},
          client_{dependencies.client},
          network_{dependencies.network},
          terminal_{dependencies.terminal},
          session_{std::move(dependencies.session)} {}

    ServerControl(const ServerControl&) = delete;
    ServerControl& operator=(const ServerControl&) = delete;

    [[nodiscard]] State state() const {
        std::scoped_lock lock{mutex_};
        if (state_.pending && tasks::TaskLanes::Clock::now() >= state_.deadline) {
            state_.pending = false;
            state_.status = "Settings request timed out; reopen the menu to refresh";
            ++state_.revision;
        }
        return state_;
    }

    // Ambient work runs only in confirmed STANDARD mode, never while a change is unconfirmed.
    [[nodiscard]] bool ambient_mode_ready() const {
        const auto controls = state();
        return !controls.pending && controls.selection.mode == "STANDARD";
    }

    // Capture menu ownership once; a later selection never performs a fresh nearest-NPC fallback.
    [[nodiscard]] std::optional<integration::PromptTarget> prepare_target(const Snapshot& snapshot) const {
        if (!snapshot || terminal_.load(std::memory_order_acquire)) return {};
        const auto selected = snapshot->picked_actor_form_id();
        const core::ActorSnapshot* best{};
        double nearest = configuration_.activation.exterior_distance;
        for (const auto& actor : snapshot->actors()) {
            if (!actor.alive() || actor.disabled() || actor.form_id() == 0x14) continue;
            const auto distance = targeting::distance(snapshot->player_pose().position(), actor.position());
            if (selected && actor.form_id() == *selected) { best = &actor; break; }
            if (distance < nearest) { nearest = distance; best = &actor; }
        }
        if (!best) return {};
        return integration::PromptTarget{session_, best->form_id(), best->base_form_id(),
            best->origin_plugin(), best->base_origin_plugin(), best->playthrough_id(), best->name()};
    }

    // A bounded worker RPC owns settings acknowledgements; render/game threads never wait on HTTP.
    [[nodiscard]] bool request(std::string setting, std::string value) {
        if (!client_.ready() || terminal_.load(std::memory_order_acquire)) return false;
        std::uint64_t revision{};
        {
            std::scoped_lock lock{mutex_};
            if (state_.pending) return false;
            state_.pending = true;
            state_.status = "Waiting for server confirmation...";
            ++state_.revision;
            revision = state_.revision;
            state_.deadline = tasks::TaskLanes::Clock::now() + std::chrono::seconds{15};
        }
        const auto accepted = network_.try_submit("network", tasks::TaskClass::background,
            tasks::TaskLanes::Clock::now() + std::chrono::seconds{15}, session_,
            [this, setting = std::move(setting), value = std::move(value), revision](const core::CancellationToken& cancellation) {
                State updated = state();
                if (!updated.pending || updated.revision != revision) return;
                updated.status = "Server settings unavailable; selection unchanged";
                try {
                    const auto outcome = client_.control(setting, value);
                    if (outcome.status == RequestStatus::complete) {
                        for (const auto& line : outcome.lines) {
                            const auto* status = std::get_if<protocol_native::Status>(&line.payload);
                            if (!status || status->code != "control_state") continue;
                            const auto split = status->detail.find('|');
                            if (split == std::string::npos || split + 2 != status->detail.size()) continue;
                            const auto mode = status->detail.substr(0, split);
                            const auto slot = status->detail.back();
                            if (!protocol_native::valid_control_mode(mode) || slot < '1' || slot > '4') continue;
                            updated.selection = {mode, static_cast<std::uint64_t>(slot - '0')};
                            updated.status = "Server settings confirmed";
                        }
                    }
                } catch (const std::exception&) {}
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                std::scoped_lock lock{mutex_};
                if (state_.revision != revision) return;
                updated.pending = false;
                updated.revision = state_.revision + 1;
                state_ = std::move(updated);
            });
        if (accepted != tasks::SubmitResult::accepted) {
            std::scoped_lock lock{mutex_};
            state_.pending = false;
            state_.status = "Settings queue is busy; try again";
            ++state_.revision;
            return false;
        }
        return true;
    }

private:
    const config::Config& configuration_;
    SynthClient& client_;
    tasks::TaskLanes& network_;
    const std::atomic_bool& terminal_;
    const core::CancellationToken session_;
    mutable std::mutex mutex_;
    mutable State state_;
};

}  // namespace synth::client
