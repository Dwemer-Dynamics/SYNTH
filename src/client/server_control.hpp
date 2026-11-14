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
#include <string_view>
#include <utility>
#include <variant>

namespace synth::client {

// Owns one session's server control settings: the last confirmed mode/model-slot selection and
// acknowledged audience, the single pending request with its revision and deadline, and the worker
// RPC that acknowledges it, plus the control menu's retained target capture. The audience starts
// Normal with each session, so load, new game and retirement reset it with the session. Readers on
// the game/render thread copy the state under a leaf mutex that is never held across HTTP; the worker
// task reacquires it only to publish a result whose revision still matches. A timed-out request is
// retired by the next reader.
//
// Lifetimes: the borrowed configuration, client, network lane and terminal flag outlive this object.
// The queued RPC captures this object; the session joins the network lane in its destructor body
// before any member is destroyed, and a rejected, expired or discarded task is destroyed unrun.
class ServerControl final {
public:
    using Snapshot = std::shared_ptr<const core::RuntimeSnapshot>;

    struct State final {
        protocol_native::ControlSelection selection{"STANDARD", 1}; // Audience stays Normal; see dialogue_selection.
        std::string status;
        bool pending{};
        std::uint64_t revision{};
        tasks::TaskLanes::Clock::time_point deadline{};
        bool everyone{};                     // Everyone intent for future player captures: acknowledged, or unconfirmed.
        bool audience_unconfirmed{};         // Everyone was requested or held but its acknowledgement was lost.
        std::uint64_t audience_reset_epoch{};
        std::string pending_audience;        // Requested audience while that change is unconfirmed.
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
            // An unanswered request may have changed the server audience; Everyone intent stays, unconfirmed.
            retain_unconfirmed(state_);
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

    // Player typed/voice admission copies this once. Everyone needs its acknowledgement, a still-negotiated
    // contract and a compatible confirmed mode; while a request is pending over Everyone, or toward it,
    // admission sees everyone_unconfirmed and refuses visibly, as it does after a lost acknowledgement until a valid
    // reply or an explicit Normal choice. Normal traffic never waits on the audience.
    [[nodiscard]] protocol_native::ControlSelection dialogue_selection() const {
        const auto controls = state();
        auto selection = controls.selection;
        if (!controls.everyone && !(controls.pending && controls.pending_audience == "everyone")) return selection;
        if (controls.pending || controls.audience_unconfirmed || !client_.everyone_ready() || !protocol_native::everyone_audience_mode(selection.mode)) {
            selection.audience = protocol_native::AudienceIntent::everyone_unconfirmed;
        } else {
            selection.audience = protocol_native::AudienceIntent::everyone;
            selection.audience_reset_epoch = controls.audience_reset_epoch;
        }
        return selection;
    }

    // Visible reason an audience selection cannot be requested, checked before any network work.
    [[nodiscard]] std::optional<std::string> audience_refusal(std::string_view value) const {
        if (value != "normal" && value != "everyone") return "Unknown audience";
        if (value == "everyone" && !client_.everyone_ready()) return "This server does not support the Everyone audience";
        if (value == "everyone" && !protocol_native::everyone_audience_mode(state().selection.mode))
            return "Everyone needs Standard or Shout chat mode";
        return std::nullopt;
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
        const auto refusal = setting == "audience" ? audience_refusal(value) : std::nullopt;
        std::uint64_t revision{};
        {
            std::scoped_lock lock{mutex_};
            if (state_.pending) return false;
            // Without the negotiated contract no audience RPC exists: a refused Everyone choice keeps new player
            // input refused rather than directed, and Normal is chosen locally.
            if (setting == "audience" && !client_.everyone_ready() && (value == "normal" || value == "everyone")) {
                state_.everyone = state_.audience_unconfirmed = refusal.has_value();
                state_.status = refusal ? *refusal + "; choose Normal to send" : "Audience set to Normal";
                ++state_.revision;
                return !refusal;
            }
            if (refusal) return false;
            state_.pending = true;
            state_.pending_audience = setting == "audience" ? value : std::string{};
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
                bool confirmed{};
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
                            // A negotiated reply must carry a coherent audience; one without it confirms nothing.
                            const bool negotiated = client_.everyone_ready();
                            if (negotiated && (!status->control_audience ||
                                (status->control_audience->everyone && !protocol_native::everyone_audience_mode(mode)))) continue;
                            updated.selection = {mode, static_cast<std::uint64_t>(slot - '0')};
                            if (negotiated) {
                                updated.everyone = status->control_audience->everyone;
                                updated.audience_unconfirmed = false;
                            }
                            updated.audience_reset_epoch = negotiated ? status->control_audience->reset_epoch : 0;
                            updated.status = "Server settings confirmed";
                            confirmed = true;
                        }
                    }
                } catch (const std::exception&) {}
                // A reply that confirmed nothing never turns Everyone into directed Normal input.
                if (!confirmed) {
                    const bool everyone = updated.everyone;
                    retain_unconfirmed(updated);
                    if (updated.everyone)
                        updated.status = "Server settings unavailable; Everyone is unconfirmed and nothing is sent until "
                                         "it is confirmed or Normal is chosen";
                    else if (everyone)
                        updated.status = "Server settings unavailable; audience returned to Normal";
                }
                updated.pending_audience.clear();
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
            state_.pending_audience.clear();
            state_.status = "Settings queue is busy; try again";
            ++state_.revision;
            return false;
        }
        return true;
    }

private:
    // Lost acknowledgement: a requested or held Everyone stays as unconfirmed intent; only an explicit Normal
    // choice drops it without a valid reply.
    static void retain_unconfirmed(State& state) {
        state.everyone = state.pending_audience == "everyone" || (state.everyone && state.pending_audience != "normal");
        state.audience_unconfirmed = state.everyone;
        state.pending_audience.clear();
    }

    const config::Config& configuration_;
    SynthClient& client_;
    tasks::TaskLanes& network_;
    const std::atomic_bool& terminal_;
    const core::CancellationToken session_;
    mutable std::mutex mutex_;
    mutable State state_;
};

}  // namespace synth::client
