#pragma once

#include "client/ambient_exchange.hpp"
#include "client/automatic_activation.hpp"
#include "client/context_publication.hpp"
#include "client/dialogue_requests.hpp"
#include "client/observed_event_delivery.hpp"
#include "client/player_dialogue.hpp"
#include "client/synth_client.hpp"
#include "config/config.hpp"
#include "context/context.hpp"
#include "core/companion_combat_comment.hpp"
#include "core/snapshot.hpp"
#include "targeting/targeting.hpp"
#include "tasks/task_lanes.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace synth::client {

// Owns one session's NPC activation and unsolicited context reactions: the activation request
// (context publication, activation ACK, automatic greeting), the flat automatic cadence and VR
// last-automatic actor, the bounded list of actors whose activation the server acknowledged, the
// shared activity clock with the bored and combat-bark claims, observation reactor selection, and
// the context trigger RPC (bored, combat bark, quest/player reaction and external one-shots).
//
// The session keeps cross-feature admission: every maybe_trigger_* and activate_target gate over
// readiness, pause, microphone, responses, actions, speech and rechat, target selection, player
// turn replacement (passed per call to submit_activation) and response line routing. It calls in
// on the game thread with a copied snapshot and a resolved actor identity; enrichment through the
// observed-event owner runs there before worker admission and outside every lock here.
//
// Locks: activation_mutex_ and activity_mutex_ are leaves over copied values. Reactor selection
// try-locks the acknowledged actor list and then PlayerDialogue's conversation state, and returns
// on contention; the worker takes the list lock alone. No lock here is held across HTTP, a callback,
// the enricher or a notification. automatic_current() also runs on the network worker; its voice
// read is PlayerDialogue's single game-thread-published voice activity value.
//
// Lifetimes: the borrowed configuration, client, network lane, response accounting, context
// publication, dialogue, observed-event owner and flags outlive this object; the session declares it
// last. Its network tasks capture this object and copied snapshots/identities only; their line
// callback routes into the session. The session joins the network lane in its destructor body before
// any member is destroyed, and a rejected, expired or discarded task is destroyed unrun with its
// response guard released once.
class ActivationTriggers final {
public:
    using Snapshot = std::shared_ptr<const core::RuntimeSnapshot>;
    using Notify = std::function<void(std::string)>;
    // Session control-mode read: no settings request pending and STANDARD selected.
    using AmbientReady = std::function<bool()>;
    // Session response routing for a trigger's lines; one-shots disallow follow-up.
    using LineSink = std::function<void(const protocol_native::Line&, const Snapshot&, const core::CancellationToken&,
                                        bool allow_followup, bool combat_bark,
                                        std::optional<core::SnapshotClock::time_point> companion_start_by)>;

    struct Dependencies final {
        const config::Config& configuration;
        SynthClient& client;
        tasks::TaskLanes& network;
        DialogueRequests& requests;
        ContextPublication& context;
        PlayerDialogue& dialogue;
        ObservedEventDelivery& observations;
        const std::atomic_bool& terminal;
        const std::atomic_bool& paused;
        const std::atomic_bool& composing;
        core::CancellationToken session;
        AmbientReady ambient_ready;
        LineSink line;
        Notify notify;
    };

    explicit ActivationTriggers(Dependencies dependencies)
        : configuration_{dependencies.configuration},
          client_{dependencies.client},
          network_{dependencies.network},
          requests_{dependencies.requests},
          context_{dependencies.context},
          dialogue_{dependencies.dialogue},
          observations_{dependencies.observations},
          terminal_{dependencies.terminal},
          paused_{dependencies.paused},
          composing_{dependencies.composing},
          session_{std::move(dependencies.session)},
          ambient_ready_{std::move(dependencies.ambient_ready)},
          line_{std::move(dependencies.line)},
          notify_{std::move(dependencies.notify)} {
        if (!ambient_ready_ || !line_ || !notify_)
            throw std::invalid_argument{"activation triggers require ambient mode, line and notification sinks"};
    }

    ActivationTriggers(const ActivationTriggers&) = delete;
    ActivationTriggers& operator=(const ActivationTriggers&) = delete;

    [[nodiscard]] bool automatic_current(const Snapshot& snapshot) const {
        const auto now=core::SnapshotClock::now();
        return snapshot&&!paused_.load(std::memory_order_acquire)&&ambient_ready_()&&!dialogue_.voice_active()&&
            !composing_.load(std::memory_order_acquire)&&now>=snapshot->captured_at()&&
            now-snapshot->captured_at()<=std::chrono::seconds{2};
    }

    // Game thread: an automatic pass found no eligible target.
    void forget_automatic_target(core::RuntimeVariant variant) {
        std::scoped_lock lock{activation_mutex_};
        if(variant==core::RuntimeVariant::flat)automatic_activation_cadence_.clear();
        else last_automatic_actor_.clear();
    }

    // Game thread, after the session's admission and target selection. `begin_turn` replaces the
    // player turn for a manual activation and is invoked at most once, during this call.
    template <class BeginTurn>
    [[nodiscard]] bool submit_activation(Snapshot snapshot, const protocol_native::Identity& actor, bool manual,
                                         BeginTurn&& begin_turn) {
        const bool flat_automatic=!manual&&snapshot->variant()==core::RuntimeVariant::flat;
        const auto automatic_key=actor_key(actor);
        if(flat_automatic) {
            std::scoped_lock lock{activation_mutex_};
            if(!automatic_activation_cadence_.due(automatic_key,tasks::TaskLanes::Clock::now()))return false;
        }
        if (!observations_.enrich_selected_actor(snapshot, actor)) return false;
        const auto deadline = flat_automatic ? snapshot->captured_at()+std::chrono::seconds{2}
            : tasks::TaskLanes::Clock::now()+std::chrono::milliseconds{request_timeout_ms()*2ULL};
        const auto accepted = network_.try_submit(
            "network", manual ? tasks::TaskClass::current_turn : tasks::TaskClass::background,
            deadline, manual ? std::forward<BeginTurn>(begin_turn)() : dialogue_.current_turn(),
            requests_.track_response(
            [this, snapshot = std::move(snapshot), actor, manual,flat_automatic](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                if(flat_automatic&&!automatic_current(snapshot))return;

                try {
                    const auto context_turn = client_.begin_context_turn();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    if(flat_automatic&&!automatic_current(snapshot))return;
                    const auto context_outcome = context_.publish(snapshot, actor, cancellation);
                    if (context_outcome.status != RequestStatus::complete) {
                        notify_("SYNTH activation context failed: " + context_outcome.detail);
                        return;
                    }
                    const auto outcome = client_.activate(actor, manual ? "manual" : "automatic", context_outcome.context_binding);
                    if (outcome.status == RequestStatus::complete) {
                        dialogue_.clear_rechat_suppression(actor_key(actor));
                        {
                            std::scoped_lock lock{active_quest_actors_mutex_};
                            const auto key = actor_key(actor);
                            if (std::ranges::find(active_quest_actors_, key) == active_quest_actors_.end()) {
                                if (active_quest_actors_.size() == 128) active_quest_actors_.erase(active_quest_actors_.begin());
                                active_quest_actors_.push_back(key);
                            }
                        }
                        if (manual) {
                            notify_("SYNTH activated " + actor.display_name);
                        } else if (snapshot->game_time_ticks() > 0) {
                            const auto key = actor_key(actor);
                            if(flat_automatic&&!automatic_current(snapshot))return;
                            bool newly_observed=flat_automatic;
                            if(!flat_automatic) {
                                std::scoped_lock lock{activation_mutex_};
                                newly_observed = key != last_automatic_actor_;
                                last_automatic_actor_ = key;
                            }
                            if (newly_observed) {
                                const auto greeting = client_.trigger(
                                    "auto_greeting", actor,
                                    [this, snapshot, cancellation](const protocol_native::Line& line) {
                                        line_(line, snapshot, cancellation, true, false, std::nullopt);
                                    },
                                    snapshot->game_time_ticks(), std::nullopt, context_outcome.context_binding);
                                if (greeting.status != RequestStatus::complete) {
                                    notify_("SYNTH auto greeting stopped: " + greeting.detail);
                                }
                            }
                        }
                    } else if (manual) {
                        notify_("SYNTH activation failed: " + outcome.detail);
                    }
                } catch (const std::exception& error) {
                    if (manual) notify_(std::string{"SYNTH activation: "} + error.what());
                }
            }));
        if (accepted != tasks::SubmitResult::accepted) {
            if (manual) notify_("SYNTH network queue is full; activation deferred");
            return false;
        }
        if(flat_automatic) {
            std::scoped_lock lock{activation_mutex_};
            automatic_activation_cadence_.admitted(automatic_key,tasks::TaskLanes::Clock::now());
        }
        dialogue_.claim_external(actor_key(actor));
        return true;
    }

    // end_conversation: the actor may be greeted again by a later automatic pass.
    void forget(const std::string& key) {
        std::scoped_lock lock{activation_mutex_};
        if (last_automatic_actor_ == key) last_automatic_actor_.clear();
        automatic_activation_cadence_.forget(key);
    }

    // Shared current-owner, crosshair, then nearest selection; never auto-activate an actor to manufacture a reaction.
    [[nodiscard]] std::optional<context::TargetContext> select_observation_reactor(
        const core::RuntimeSnapshot& snapshot, core::SnapshotClock::time_point now) {
        std::optional<context::TargetContext> selected;
        std::unique_lock lock{active_quest_actors_mutex_, std::try_to_lock};
        if (!lock.owns_lock()) return std::nullopt;
        if (!dialogue_.try_with_conversation([&](const std::optional<std::string>& owner, const auto& cooling) {
            const context::PlaythroughIdentity playthrough{snapshot.player().playthrough_id()};
            const auto eligible = [&](const core::ActorSnapshot& actor) {
                const auto key = actor_key(identity(actor));
                return std::ranges::find(active_quest_actors_, key) != active_quest_actors_.end() &&
                    !cooling(key, now) &&
                    targeting::dialogue_actor_available(actor) && actor.life_state() == "alive" &&
                    (actor.posture() == "normal" || actor.posture() == "sitting") &&
                    actor.line_of_sight() == true && !actor.in_combat() && !actor.hostile_to_player() &&
                    !actor.talking_to_player() && targeting::distance(snapshot.player_pose().position(), actor.position()) <=
                        configuration_.activation.automatic_hearing_distance;
            };
            if (owner && !owner->empty()) {
                selected = targeting::select_nearest(snapshot, playthrough, snapshot.player_pose().position(),
                    configuration_.activation.automatic_hearing_distance, [&](const auto& actor) {
                        return actor_key(identity(actor)) == *owner && eligible(actor);
                    }).target;
            }
            if (!selected) selected = targeting::select_flat_crosshair(snapshot, playthrough, {}, eligible).target;
            if (!selected) selected = targeting::select_nearest(snapshot, playthrough, snapshot.player_pose().position(),
                configuration_.activation.automatic_hearing_distance, eligible).target;
        })) return std::nullopt;
        return selected;
    }

    // Registered recorded participant only; nearest wins, ties by reference ID, never owner or crosshair.
    [[nodiscard]] std::optional<std::pair<protocol_native::Identity, std::uint32_t>> select_companion_speaker(
        const core::RuntimeSnapshot& snapshot, const core::PlayerEventTransition& end, core::SnapshotClock::time_point now) {
        const core::ActorSnapshot* selected{};
        std::unique_lock lock{active_quest_actors_mutex_, std::try_to_lock};
        if (!lock.owns_lock()) return std::nullopt;
        if (!dialogue_.try_with_conversation([&](const std::optional<std::string>&, const auto& cooling) {
            selected = core::select_companion_speaker(snapshot, end, configuration_.activation.automatic_hearing_distance,
                [&](const core::ActorSnapshot& actor) {
                    const auto key = actor_key(identity(actor));
                    return std::ranges::find(active_quest_actors_, key) != active_quest_actors_.end() && !cooling(key, now) &&
                        targeting::dialogue_actor_available(actor);
                });
        }) || !selected) return std::nullopt;
        return std::pair{identity(*selected), selected->form_id()};
    }

    void mark_activity(core::SnapshotClock::time_point when) {
        std::scoped_lock lock{activity_mutex_};
        last_activity_at_ = when;
    }

    [[nodiscard]] bool claim_bored(core::SnapshotClock::time_point now) {
        std::scoped_lock lock{activity_mutex_};
        if (now - last_activity_at_ <
            std::chrono::seconds{configuration_.behavior.bored_event_seconds}) {
            return false;
        }
        last_activity_at_ = now;
        return true;
    }

    [[nodiscard]] bool claim_combat_bark(core::SnapshotClock::time_point now) {
        std::scoped_lock lock{activity_mutex_};
        if (last_combat_bark_at_.time_since_epoch().count() != 0 &&
            now - last_combat_bark_at_ <
                std::chrono::seconds{configuration_.behavior.combat_bark_period_seconds}) {
            return false;
        }
        last_combat_bark_at_ = now;
        return true;
    }

    void release_combat_bark_claim() {
        std::scoped_lock lock{activity_mutex_};
        last_combat_bark_at_ = {};
    }

    // One 30-second claim shared by every companion; released only when the request is not queued.
    [[nodiscard]] bool claim_companion_comment(core::SnapshotClock::time_point now) {
        std::scoped_lock lock{activity_mutex_};
        if (last_companion_comment_at_ && now - *last_companion_comment_at_ < core::companion_comment_cooldown) return false;
        last_companion_comment_at_ = now;
        return true;
    }
    void release_companion_comment_claim() {
        std::scoped_lock lock{activity_mutex_};
        last_companion_comment_at_.reset();
    }

    // Game thread. Publishes the captured frame before issuing an unsolicited dialogue trigger.
    [[nodiscard]] bool queue_context_trigger(std::string kind, protocol_native::Identity actor,
                                             Snapshot snapshot, std::string failure_label,
                                             std::optional<std::string> external_text = std::nullopt,
                                             std::optional<std::uint64_t> previous_sequence = std::nullopt,
                                             std::optional<std::uint64_t> player_event_serial = std::nullopt,
                                             std::optional<core::SnapshotClock::time_point> event_deadline = std::nullopt,
                                             std::optional<core::CancellationToken> owner_turn = std::nullopt) {
        if (!observations_.enrich_selected_actor(snapshot, actor)) return false;
        if (kind == "bored") {
            const auto captured_deadline = snapshot->captured_at() + std::chrono::seconds{2};
            event_deadline = std::min(event_deadline.value_or(captured_deadline), captured_deadline);
            const auto observed_now = core::SnapshotClock::now();
            const auto hearing = snapshot->world() && snapshot->world()->interior
                ? configuration_.activation.interior_hearing_distance : configuration_.activation.exterior_hearing_distance;
            const auto pair = select_ambient_pair(*snapshot, observed_now, session_.generation(),
                hearing, configuration_.activation.automatic_hearing_distance,
                [&](const auto& value) { return !dialogue_.on_cooldown(actor_key(identity(value)), observed_now); });
            if (!pair || actor_key(identity(*pair->speaker)) != actor_key(actor) || !ambient_ready_()) return false;
        }
        const auto owner = actor_key(actor);
        const auto now = tasks::TaskLanes::Clock::now();
        const auto deadline = std::min(now + std::chrono::milliseconds{request_timeout_ms() * 2ULL},
            event_deadline.value_or(core::SnapshotClock::time_point::max()));
        if (deadline <= now) return false;
        const auto accepted = network_.try_submit(
            "network", tasks::TaskClass::current_turn, deadline, owner_turn ? *owner_turn : dialogue_.current_turn(),
            requests_.track_response(
            [this, kind = std::move(kind), actor = std::move(actor),
             snapshot = std::move(snapshot), failure_label = std::move(failure_label), external_text = std::move(external_text), previous_sequence,
             player_event_serial, event_deadline](
                const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;

                try {
                    const auto context_turn = client_.begin_context_turn();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    if (kind == "bored" && (!client_.ambient_npc_ready() || composing_.load(std::memory_order_acquire) ||
                        !ambient_ready_())) return;
                    if (event_deadline && (core::SnapshotClock::now() > *event_deadline || paused_.load(std::memory_order_acquire) ||
                        core::SnapshotClock::now() - snapshot->captured_at() > std::chrono::seconds{2})) return;
                    if (previous_sequence && !context_.quest_sequence_current(*previous_sequence)) return;
                    const auto context_outcome = kind == "bored"
                        ? context_.publish(snapshot, actor, cancellation, nullptr, {}, {}, {}, {"STANDARD", 1})
                        : context_.publish(snapshot, actor, cancellation);
                    if (context_outcome.status != RequestStatus::complete) return;
                    if (event_deadline && (core::SnapshotClock::now() > *event_deadline || cancellation.is_cancelled() ||
                        paused_.load(std::memory_order_acquire))) return;
                    if (kind == "bored" && (composing_.load(std::memory_order_acquire) || !ambient_ready_())) return;
                    const auto one_shot=kind=="external_tts" || kind=="external_comment" || kind=="external_reaction" || kind=="quest_updated" || kind=="player_reaction" || kind=="combat_bark" || kind=="companion_combat_comment";
                    const auto start_by = kind == "companion_combat_comment" ? event_deadline : std::nullopt;
                    const auto outcome = client_.trigger(
                        kind, actor, [this, snapshot, cancellation, actor, one_shot, combat_bark = kind == "combat_bark", start_by](const protocol_native::Line& line) {
                            if (start_by) {
                                if (core::SnapshotClock::now() > *start_by || cancellation.is_cancelled()) return;
                                if (const auto* dialogue = std::get_if<protocol_native::Dialogue>(&line.payload)) {
                                    const auto* speaker = std::get_if<protocol_native::Identity>(&dialogue->speaker);
                                    if (!speaker || actor_key(*speaker) != actor_key(actor)) return;
                                }
                            }
                            line_(line, snapshot, cancellation, !one_shot, combat_bark, start_by);
                        }, std::nullopt, external_text, context_outcome.context_binding, std::nullopt, previous_sequence, player_event_serial);
                    if (outcome.status != RequestStatus::complete && outcome.status != RequestStatus::cancelled) {
                        notify_("SYNTH " + failure_label + ": " + outcome.detail);
                    }
                } catch (const std::exception& error) {
                    notify_("SYNTH " + failure_label + ": " + error.what());
                }
            }));
        if (accepted == tasks::SubmitResult::accepted) dialogue_.claim_external(owner);
        return accepted == tasks::SubmitResult::accepted;
    }

private:
    [[nodiscard]] static protocol_native::Identity identity(const core::ActorSnapshot& actor) {
        return PlayerDialogue::identity(actor);
    }
    [[nodiscard]] static std::string actor_key(const protocol_native::Identity& actor) {
        return PlayerDialogue::actor_key(actor);
    }
    [[nodiscard]] std::uint64_t request_timeout_ms() const noexcept {
        return static_cast<std::uint64_t>(configuration_.behavior.ai_response_timeout_seconds) * 1000ULL;
    }

    const config::Config& configuration_;
    SynthClient& client_;
    tasks::TaskLanes& network_;
    DialogueRequests& requests_;
    ContextPublication& context_;
    PlayerDialogue& dialogue_;
    ObservedEventDelivery& observations_;
    const std::atomic_bool& terminal_;
    const std::atomic_bool& paused_;
    const std::atomic_bool& composing_;
    const core::CancellationToken session_;
    AmbientReady ambient_ready_;
    LineSink line_;
    Notify notify_;
    std::mutex activation_mutex_;
    std::string last_automatic_actor_;
    AutomaticActivationCadence automatic_activation_cadence_;
    std::vector<std::string> active_quest_actors_; // Successful activation ACKs only; bounded.
    std::mutex active_quest_actors_mutex_;
    std::mutex activity_mutex_;
    core::SnapshotClock::time_point last_activity_at_{core::SnapshotClock::now()};
    core::SnapshotClock::time_point last_combat_bark_at_{};
    std::optional<core::SnapshotClock::time_point> last_companion_comment_at_;
};

}  // namespace synth::client
