#pragma once

#include "client/actor_state_combat.hpp"

#include "audio/dialogue_audio.hpp"
#include "audio/native_playback.hpp"
#include "client/synth_client.hpp"
#include "client/speech_receipts.hpp"
#include "client/ambient_exchange.hpp"
#include "client/automatic_activation.hpp"
#include "client/activation_triggers.hpp"
#include "client/speech_delivery.hpp"
#include "client/speech_facing_binding.hpp"
#include "client/conversation_holds.hpp"
#include "client/player_dialogue.hpp"
#include "client/dialogue_requests.hpp"
#include "client/context_publication.hpp"
#include "client/action_execution.hpp"
#include "client/social_task_execution.hpp"
#include "client/rechat_continuation.hpp"
#include "client/quest_event_delivery.hpp"
#include "client/observed_event_delivery.hpp"
#include "client/diary_requests.hpp"
#include "client/visual_capture.hpp"
#include "client/server_control.hpp"
#include "client/winhttp_transport.hpp"
#include "config/config.hpp"
#include "core/snapshot.hpp"
#include "core/save_context.hpp"
#include "integration/external_requests.hpp"
#include "integration/prompt_target.hpp"
#include "runtime/game_thread_dispatcher.hpp"
#include "targeting/targeting.hpp"
#include "tasks/task_lanes.hpp"
#include "voice/voice_sample_worker.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <charconv>
#include <condition_variable>
#include <cmath>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <shellapi.h>

namespace synth::client {

// Composition root for one runtime session: constructs the feature owners in dependency order, fans
// out the ordered game-thread pump and lifecycle (Stop, combat interrupt, turn replacement, hard
// halt, retirement), forwards the stable flat/VR entry API and routes response lines and rejected
// presentation between owners. It keeps only cross-feature admission and priority checks plus the
// shared flags, lanes and notification queue; feature state lives in the owners declared below.
class PluginSession final {
public:
    using Snapshot = std::shared_ptr<const core::RuntimeSnapshot>;
    using ActorEnricher = ObservedEventDelivery::ActorEnricher;

    // Installed on the game thread when the coordinator-created session is adopted.
    void set_actor_enricher(ActorEnricher enrich) { observations_.set_actor_enricher(std::move(enrich)); }

    PluginSession(config::Config configuration,
                  core::RuntimeVariant variant,
                  std::string runtime_version,
                  runtime::GameThreadDispatcher& dispatcher,
                  core::RuntimeGeneration generation,
                  core::SaveContextStore* save_context = nullptr,
                  core::SaveContextStore::Binding save_binding = {}, bool rest_events_available = false,
                  bool quest_events_available = false, bool actor_events_available = false)
        : dispatcher_{dispatcher},
          configuration_{std::move(configuration)},
          transport_{configuration_.server.base_url,
                     configuration_.server.connect_timeout_ms},
          client_{transport_,
                  SessionOptions{
                      .runtime_session_id = make_runtime_session_id(variant),
                      .runtime_variant = variant == core::RuntimeVariant::vr ? "vr" : "flat",
                      .client_version = "0.1.0",
                      .runtime_version = std::move(runtime_version),
                      .locale = "en-US",
                      .capabilities = ContextPublication::capabilities(variant, rest_events_available, quest_events_available, actor_events_available),
                      .request_timeout_ms = configuration_.behavior.ai_response_timeout_seconds * 1000U,
                      .maximum_response_bytes = configuration_.limits.max_response_bytes,
                      .protocol_version = 2,
                  }},
          cancellation_{generation},
          network_({tasks::LaneConfig{"network", configuration_.limits.max_pending_requests, 1, 3}}, 2),
          quest_events_{generation},
          dialogue_{configuration_, client_, control_, cancellation_.token(), trace_,
                    [this](std::string message) { push_notification(std::move(message)); }},
          requests_{{client_, network_, terminal_, conversation_holds_,
                     [this](const Snapshot& snapshot, const std::optional<protocol_native::Identity>& target,
                            core::CancellationToken cancellation, const protocol_native::ControlSelection& controls) {
                         return publish_snapshot(snapshot, target, std::move(cancellation), nullptr, {}, {}, {}, controls);
                     },
                     [this](const protocol_native::Line& line, const Snapshot& snapshot,
                            const core::CancellationToken& cancellation) { handle_line(line, snapshot, cancellation); },
                     [this](const RequestOutcome& outcome) { return social_.notify_input(outcome); },
                     [this](std::string message) { push_notification(std::move(message)); }}},
          speech_{configuration_, {client_, network_, cancellation_.token(), terminal_, trace_,
                                   [this](std::string message) { push_notification(std::move(message)); },
                                   // Invoked only by network-lane receipt drains, which the destructor joins first.
                                   [this](std::uint64_t generation, const core::CancellationToken& cancellation) {
                                       context_.note_heard_receipt(generation, cancellation);
                                   }}},
          voice_samples_{configuration_.server.base_url, configuration_.server.connect_timeout_ms,
                         cancellation_.token(), client_.runtime_session_id()},
          context_{{configuration_, client_, network_, requests_, voice_samples_, quest_events_,
                    ObservedEventDelivery::quest_events_enabled(variant, quest_events_available),
                    terminal_, runtime_paused_, cancellation_.token(), save_context, std::move(save_binding),
                    [this](const core::RuntimeSnapshot& snapshot) {
                        const auto selected = dialogue_.selected_target(snapshot, false);
                        return selected ? std::optional{identity(*selected)} : std::nullopt;
                    },
                    [this](const RequestOutcome& outcome, const core::RuntimeSnapshot& snapshot) {
                        social_.hydrate(outcome, snapshot);
                    },
                    [this](core::CancellationToken cancellation) { invalidate_response(std::move(cancellation)); },
                    [this](std::string message) { push_notification(std::move(message)); }}},
          social_{{configuration_, client_, network_, requests_, context_, terminal_, cancellation_.token(),
                   [this] {
                       return runtime_paused() || composing_.load(std::memory_order_acquire) || dialogue_.recording() ||
                           actions_.active() != 0 || speech_.jobs_active() || !speech_.playback_idle() || !speech_.captions_idle();
                   },
                   [this](const protocol_native::Line& line, const Snapshot& snapshot,
                          const core::CancellationToken& cancellation, bool allow_followup) {
                       handle_line(line, snapshot, cancellation, allow_followup);
                   },
                   [this](std::string message) { push_notification(std::move(message)); }}},
          actions_{{configuration_, client_, network_, dispatcher_, dialogue_, context_, terminal_, cancellation_.token(),
                    [this](const protocol_native::Line& line, const Snapshot& snapshot,
                           const core::CancellationToken& cancellation) { handle_line(line, snapshot, cancellation); },
                    [this](const protocol_native::Identity& actor) { end_conversation(actor); },
                    [this](const protocol_native::Identity& actor, runtime::IFalloutRuntime& runtime) {
                        return social_.release_for_action(actor, runtime);
                    },
                    [this](std::string message) { push_notification(std::move(message)); }}},
          rechat_{{configuration_, client_, network_, requests_, speech_, context_, social_, actions_, dialogue_,
                   conversation_holds_, *trace_, terminal_, runtime_paused_,
                   [this](const protocol_native::Line& line, const Snapshot& snapshot, const core::CancellationToken& cancellation,
                          bool allow_followup, const std::shared_ptr<RechatPrefetch>& prefetch) {
                       handle_line(line, snapshot, cancellation, allow_followup, prefetch);
                   },
                   [this](std::string message) { push_notification(std::move(message)); }}},
          observations_{{configuration_, client_, network_, context_, quest_events_, terminal_, runtime_paused_, cancellation_.token(),
                         variant, quest_events_available, actor_events_available,
                         [this] {
                             return dialogue_.recording() || requests_.responses() != 0 || actions_.active() != 0 ||
                                 speech_.jobs_active() || !speech_.playback_idle();
                         },
                         [this] { return current_turn_token(); },
                         [this](std::string message) { push_notification(std::move(message)); }}},
          activation_{{configuration_, client_, network_, requests_, context_, dialogue_, observations_,
                       terminal_, runtime_paused_, composing_, cancellation_.token(),
                       [this] { return server_control_.ambient_mode_ready(); },
                       [this](const protocol_native::Line& line, const Snapshot& snapshot,
                              const core::CancellationToken& cancellation, bool allow_followup, bool combat_bark) {
                           handle_line(line, snapshot, cancellation, allow_followup, {}, combat_bark);
                       },
                       [this](std::string message) { push_notification(std::move(message)); }}},
          diary_{{configuration_, client_, network_, context_, terminal_, runtime_paused_, halting_, cancellation_.token(),
                  variant == core::RuntimeVariant::flat && rest_events_available,
                  [this](std::string message) { push_notification(std::move(message)); }}},
          vision_{{configuration_, client_, network_, requests_, context_, dialogue_, observations_,
                   terminal_, runtime_paused_, halting_, cancellation_.token(),
                   [this](const protocol_native::Line& line, const Snapshot& snapshot,
                          const core::CancellationToken& cancellation) { handle_line(line, snapshot, cancellation); },
                   [this](std::string message) { push_notification(std::move(message)); }}} {}

    PluginSession(const PluginSession&) = delete;
    PluginSession& operator=(const PluginSession&) = delete;

    ~PluginSession() {
        request_stop();
        dialogue_.cancel_microphone();
        transport_.cancel_all();
        network_.shutdown(tasks::ShutdownMode::cancel_pending);
        speech_.join_jobs();
        control_.shutdown(tasks::ShutdownMode::cancel_pending);
        speech_.halt_playback();
    }

    [[nodiscard]] bool initialize(Snapshot snapshot) { return context_.initialize(std::move(snapshot)); }

    [[nodiscard]] bool publish_context(Snapshot snapshot) { return context_.publish_context(std::move(snapshot)); }

    [[nodiscard]] bool initialization_failed() const noexcept { return context_.initialization_failed(); }

    // Save/settings invalidation stops admission immediately; the coordinator
    // performs potentially blocking transport/audio teardown after retirement.
    void request_stop() noexcept {
        trace_->record(diagnostics::ConversationStage::session_retired,cancellation_.token().generation().value());
        terminal_.store(true, std::memory_order_release);
        context_.stop_save_writer();
        cancellation_.cancel();
        social_.reset();
        observations_.stop();
        diary_.stop();
        voice_samples_.request_stop();
    }

    // Called only by the owning coordinator, never from game-thread invalidation or a destructor.
    [[nodiscard]] std::optional<RequestOutcome> retire_remote_session() {
        request_stop();
        return client_.retire_session();
    }

    void import_voice_samples() { voice_samples_.request_all(); }
    [[nodiscard]] std::optional<diagnostics::ConversationRecord> take_conversation_trace() { return trace_->take(); }
    [[nodiscard]] std::uint64_t dropped_conversation_traces() const noexcept { return trace_->dropped(); }
    void cancel_voice_samples() { voice_samples_.cancel(); }
    [[nodiscard]] core::CancellationToken wait_action_lifetime() { return actions_.wait_lifetime(); }
    [[nodiscard]] std::string voice_sample_status() const { return voice_samples_.status(); }

    [[nodiscard]] bool social_task_capture_due(core::SnapshotClock::time_point now) { return social_.capture_due(now); }

    // Fresh copied state enters here on the game thread; see SocialTaskExecution::pump.
    void pump_social_tasks(Snapshot snapshot, runtime::IFalloutRuntime& runtime) { social_.pump(std::move(snapshot), runtime); }

    // Check before optional native capture; queue admission repeats this gate afterward.
    [[nodiscard]] bool context_observation_ready() const {
        return !(!client_.ready() || terminal_.load(std::memory_order_acquire) ||
            runtime_paused_.load(std::memory_order_acquire) || player_voice_active() ||
            requests_.responses() != 0 ||
            actions_.active() != 0 ||
            speech_.jobs_active() || !speech_.playback_idle() ||
            context_.observation_tasks() != 0);
    }

    [[nodiscard]] std::uint64_t acknowledged_inventory_frame() const noexcept {
        return context_.acknowledged_inventory_frame();
    }

    // Persist observations without activation or model work. Inventory reconciliation may force unchanged world context.
    [[nodiscard]] bool observe_world_context(Snapshot snapshot, bool inventory_refresh = false) {
        if (!snapshot || !context_observation_ready() ||
            snapshot->generation() != cancellation_.token().generation() ||
            !context_.world_context_needed(*snapshot, inventory_refresh)) return false;
        return context_.observe_world(std::move(snapshot), inventory_refresh, current_turn_token());
    }

    // Observed-event entry points; ObservedEventDelivery owns capture admission, cadence and receipts.
    [[nodiscard]] bool observe_quest_events(Snapshot snapshot, std::span<const core::QuestEvent> events) {
        return observations_.observe_quest_events(std::move(snapshot), events);
    }
    void pump_quest_events() { observations_.pump_quest_events(); }
    [[nodiscard]] std::uint64_t dropped_quest_events() const noexcept { return observations_.dropped_quest_events(); }
    [[nodiscard]] std::uint64_t unobserved_quest_events() const noexcept { return observations_.unobserved_quest_events(); }
    [[nodiscard]] bool quest_events_ready() const noexcept { return observations_.quest_events_ready(); }
    [[nodiscard]] bool quest_event_capture_ready() const noexcept { return observations_.quest_event_capture_ready(); }

    [[nodiscard]] bool actor_events_ready() const noexcept { return observations_.actor_events_ready(); }
    [[nodiscard]] bool actor_event_capture_ready() const noexcept { return observations_.actor_event_capture_ready(); }
    [[nodiscard]] const std::string& actor_event_session_id() const noexcept { return observations_.actor_event_session_id(); }
    [[nodiscard]] std::uint64_t dropped_actor_events() const noexcept { return observations_.dropped_actor_events(); }
    [[nodiscard]] bool observe_actor_events(ActorEventDelivery::Capture capture) {
        return observations_.observe_actor_events(std::move(capture));
    }
    void pump_actor_events() { observations_.pump_actor_events(); }

    [[nodiscard]] bool player_event_sample_due(core::SnapshotClock::time_point now) {
        return observations_.player_event_sample_due(now);
    }

    [[nodiscard]] std::optional<core::CombatantIdentity> enemy_awareness_capture_target(
        core::SnapshotClock::time_point now,bool player_in_combat) {
        return observations_.enemy_awareness_capture_target(now, player_in_combat);
    }
    void observe_enemy_awareness(const core::CombatantIdentity& owner,const core::EnemyAwarenessSnapshot& captured,
                                 std::uint64_t generation,core::SnapshotClock::time_point now) {
        observations_.observe_enemy_awareness(owner, captured, generation, now);
    }

    void observe_player_events(std::optional<core::PlayerEventSample> sample) { observations_.observe_player_events(std::move(sample)); }
    [[nodiscard]] std::array<std::uint64_t, 2> player_event_health() const noexcept { return observations_.player_event_health(); }
    [[nodiscard]] bool publish_player_event(Snapshot snapshot) { return observations_.publish_player_event(std::move(snapshot)); }

    [[nodiscard]] bool activate_target(Snapshot snapshot, bool manual) {
        if (!snapshot || !client_.ready() || runtime_paused_.load(std::memory_order_acquire)) {
            return false;
        }
        const bool flat_automatic=!manual&&snapshot->variant()==core::RuntimeVariant::flat;
        if(flat_automatic&&(!activation_.automatic_current(snapshot)||!speech_.captions_idle()||snapshot->game_time_ticks()==0))return false;
        // Ambient activation must not fill the request queue while a reply or
        // speech is outstanding. Keep current-turn admission available to input.
        if (!manual && ((flat_automatic?player_voice_active():dialogue_.recording()) || actions_.active() != 0 ||
                        requests_.responses() != 0 ||
                        speech_.jobs_active() || !speech_.playback_idle())) {
            return false;
        }
        auto target = dialogue_.selected_target(*snapshot, !manual);
        if (!target) {
            if (!manual) activation_.forget_automatic_target(snapshot->variant());
            if (manual) push_notification("SYNTH could not find an eligible target to activate");
            return false;
        }
        const auto actor = identity(*target);
        // ActivationTriggers owns cadence, enrichment, the request and its ACK; only a manual turn is replaced.
        return activation_.submit_activation(std::move(snapshot), actor, manual,
                                             [this, &actor] { return begin_player_turn(actor_key(actor)); });
    }

    // Combat cancels one response owner, not the session or its observation/activation state.
    void interrupt_dialogue_for_combat() {
        cancel_voice_capture();
        auto actor = dialogue_.external_owner();
        // An interrupted unfinished microphone turn must not retain an unresolved target lock.
        if (actor && actor->empty()) actor.reset();
        (void)begin_player_turn(actor.value_or(""));
        dialogue_.begin_external(std::move(actor));
        mark_activity();
        push_notification("SYNTH combat started; active dialogue cancelled");
    }

    void hard_halt() noexcept {
        terminal_.store(true, std::memory_order_release);
        observations_.clear_enemy_awareness();
        halting_.store(true, std::memory_order_release);
        dialogue_.cancel_microphone();
        transport_.cancel_all();
        speech_.halt_playback();
        dispatcher_.discard_pending();
        (void)network_.discard_pending();
        (void)speech_.discard_pending_jobs();
        actions_.clear();
        rechat_.clear();
        dialogue_.clear_cooldowns();
        const auto submitted = network_.try_submit(
            "network", tasks::TaskClass::current_turn,
            tasks::TaskLanes::Clock::now() + std::chrono::seconds{2}, cancellation_.token(),
            tasks::TaskLanes::track_activity(halt_requests_, [this](const core::CancellationToken&) {
                try {
                    const auto outcome = client_.hard_halt();
                    push_notification(outcome.status == RequestStatus::complete
                                          ? "SYNTH dialogue halted"
                                          : "SYNTH halted locally; server halt was not acknowledged");
                } catch (const std::exception& error) {
                    push_notification(std::string{"SYNTH halt: "} + error.what());
                }
            }));
        if (submitted != tasks::SubmitResult::accepted) {
            push_notification("SYNTH halted locally; server halt could not be queued");
        }
    }

    template <class Present>
    void drain_notifications(Present&& present) {
        std::deque<std::string> notifications;
        {
            std::scoped_lock lock{notification_mutex_};
            notifications.swap(notifications_);
        }
        for (const auto& notification : notifications) {
            present(notification);
        }
    }

    [[nodiscard]] const config::Config& configuration() const noexcept { return configuration_; }

    using ControlState = ServerControl::State;

    [[nodiscard]] ControlState control_state() const { return server_control_.state(); }

    [[nodiscard]] std::optional<integration::PromptTarget> prepare_control_target(const Snapshot& snapshot) {
        return server_control_.prepare_target(snapshot);
    }

    [[nodiscard]] bool request_control(std::string setting = "status", std::string value = "") {
        return server_control_.request(std::move(setting), std::move(value));
    }

    [[nodiscard]] std::optional<audio::PlaybackFrame> playback_frame() const noexcept {
        return speech_.playback_frame();
    }

    // Release local speech synchronously before coordinator retirement; no network or worker joins.
    [[nodiscard]] bool stop_local_speech(runtime::IFalloutRuntime& runtime, bool discard = false) noexcept {
        return speech_.stop_on_game_thread(runtime, discard);
    }

    [[nodiscard]] presentation::DialogueCaptions::Caption subtitle_frame(
        std::uint64_t generation, const std::optional<audio::PlaybackFrame>& playback,
        bool hidden, core::SnapshotClock::time_point now) {
        return speech_.subtitle_frame(generation, playback,
                                      hidden || composing_.load(std::memory_order_acquire) || terminal_.load(std::memory_order_acquire), now);
    }

    void set_composing(bool composing) {
        composing_.store(composing, std::memory_order_release);
        speech_.set_paused(composing || runtime_paused());
    }

    // Dialogue-only interruption leaves the connection and runtime session alive.
    void stop_dialogue() {
        if (terminal_.load(std::memory_order_acquire)) return;
        observations_.clear_enemy_awareness();
        cancel_voice_capture();
        actions_.stop_wait();
        social_.stop(false);
        (void)begin_player_turn();
        mark_activity();
    }

    void set_runtime_paused(bool paused) {
        const auto previous = runtime_paused_.exchange(paused, std::memory_order_acq_rel);
        speech_.set_paused(paused || composing_.load(std::memory_order_acquire));
        if (previous == paused) return;
        // Menu pause drops any in-flight capture; resuming needs no additional work.
        if (paused) {
            cancel_voice_capture();
        }
    }

    [[nodiscard]] bool runtime_paused() const noexcept {
        return runtime_paused_.load(std::memory_order_acquire);
    }

    // Admit one observed quest change without waiting on worker locks or activating a new NPC.
    [[nodiscard]] bool maybe_trigger_quest_reaction(Snapshot snapshot, core::SnapshotClock::time_point now) {
        if (!snapshot || !client_.quest_reactions_ready() || snapshot->variant() != core::RuntimeVariant::flat ||
            dialogue_.recording() || runtime_paused_.load(std::memory_order_acquire) ||
            terminal_.load(std::memory_order_acquire) || !client_.ready() || !speech_.playback_idle() ||
            speech_.jobs_active() || requests_.responses() != 0 ||
            actions_.active() != 0) return false;
        const auto previous_sequence = context_.quest_reaction_sequence(*snapshot);
        if (!previous_sequence) return false;
        const auto selected = activation_.select_observation_reactor(*snapshot, now);
        if (!selected) return false;
        return activation_.queue_context_trigger("quest_updated", identity(*selected), std::move(snapshot),
            "quest reaction stopped", std::nullopt, *previous_sequence);
    }

    // Only acknowledged, recent transitions can request one comment; lack of a speaker never blocks history delivery.
    [[nodiscard]] bool maybe_trigger_player_reaction(Snapshot snapshot, core::SnapshotClock::time_point now) {
        const auto* event = observations_.player_reaction(now);
        if (!event || !snapshot || !client_.player_reactions_ready() || snapshot->variant() != core::RuntimeVariant::flat ||
            snapshot->player().in_combat() ||
            dialogue_.recording() || runtime_paused() || terminal_.load(std::memory_order_acquire) ||
            !client_.ready() || !speech_.playback_idle() || speech_.jobs_active() ||
            requests_.responses() != 0 || actions_.active() != 0 ||
            observations_.player_event_tasks() != 0) return false;
        const auto& observed = event->after;
        if (snapshot->generation() != observed.generation || snapshot->player().form_id() != observed.form_id ||
            snapshot->player().origin_plugin() != observed.origin_plugin || snapshot->player().playthrough_id() != observed.playthrough_id)
            return false;
        const auto selected = activation_.select_observation_reactor(*snapshot, now);
        if (!selected) return false;
        const auto serial = event->serial;
        if (!activation_.queue_context_trigger("player_reaction", identity(*selected), std::move(snapshot),
            "player reaction stopped", std::nullopt, std::nullopt, serial, observed.observed_at + std::chrono::seconds{60})) return false;
        (void)observations_.consume_player_reaction(serial);
        return true;
    }

private:
    [[nodiscard]] bool player_voice_active() const { return dialogue_.voice_active(); }

public:
    [[nodiscard]] bool maybe_trigger_bored(Snapshot snapshot, core::SnapshotClock::time_point now) {
        if (!snapshot || player_voice_active() || composing_.load(std::memory_order_acquire) ||
            runtime_paused_.load(std::memory_order_acquire) || !configuration_.activation.enabled ||
            terminal_.load(std::memory_order_acquire) || !client_.ready() || !client_.ambient_npc_ready() ||
            !speech_.playback_idle() || !speech_.captions_idle() || speech_.jobs_active() ||
            requests_.responses() != 0 ||
            actions_.active() != 0) {
            return false;
        }
        if (!server_control_.ambient_mode_ready()) return false;
        if (!rechat_.idle()) return false;
        const auto hearing = snapshot->world() && snapshot->world()->interior
            ? configuration_.activation.interior_hearing_distance : configuration_.activation.exterior_hearing_distance;
        const auto pair = select_ambient_pair(*snapshot, now, cancellation_.token().generation(), hearing,
            configuration_.activation.automatic_hearing_distance,
            [&](const auto& actor) { return !conversation_on_cooldown(identity(actor), now); });
        if (!pair || !activation_.claim_bored(now)) return false;
        const auto actor = identity(*pair->speaker);
        const auto deadline = snapshot->captured_at() + std::chrono::seconds{2};
        if (!activation_.queue_context_trigger("bored", actor, std::move(snapshot),
                                   "ambient exchange stopped", {}, {}, {}, deadline)) {
            mark_activity(now - std::chrono::seconds{configuration_.behavior.bored_event_seconds});
            return false;
        }
        return true;
    }

    [[nodiscard]] bool maybe_trigger_combat_bark(Snapshot snapshot,
                                                 core::SnapshotClock::time_point now) {
        if (!snapshot || dialogue_.recording() || runtime_paused_.load(std::memory_order_acquire) ||
            !configuration_.behavior.combat_barks_enabled ||
            terminal_.load(std::memory_order_acquire) || !client_.ready() || !speech_.playback_idle() ||
            speech_.jobs_active() ||
            requests_.responses() != 0 ||
            actions_.active() != 0) {
            return false;
        }
        const context::PlaythroughIdentity playthrough{snapshot->player().playthrough_id()};
        const auto eligible = [&](const core::ActorSnapshot& actor) {
            return targeting::dialogue_actor_available(actor) && actor.in_combat() &&
                   (configuration_.activation.include_hostile || !actor.hostile_to_player());
        };
        auto selected = targeting::select_nearest(
            *snapshot, playthrough, snapshot->player_pose().position(),
            configuration_.activation.automatic_hearing_distance,
            eligible);
        if (!selected.target || !activation_.claim_combat_bark(now)) return false;
        const auto deadline = snapshot->captured_at() + std::chrono::seconds{2};
        if (!activation_.queue_context_trigger("combat_bark", identity(*selected.target), std::move(snapshot),
                                   "combat bark stopped", {}, {}, {}, deadline)) {
            activation_.release_combat_bark_claim();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool halt_complete() const {
        if (!halting_.load(std::memory_order_acquire) ||
            halt_requests_.load(std::memory_order_acquire) != 0 ||
            context_.observation_tasks() != 0 ||
            observations_.player_event_tasks() != 0 ||
            speech_.jobs_active() ||
            requests_.responses() != 0 ||
            actions_.active() != 0) {
            return false;
        }
        const auto health = network_.health();
        return std::ranges::none_of(health.lanes, [](const auto& lane) {
            return lane.pending_background != 0 || lane.pending_current_turn != 0;
        });
    }

    [[nodiscard]] bool connection_ready() const noexcept { return client_.ready() && !terminal_.load(std::memory_order_acquire); }
    [[nodiscard]] bool halting() const noexcept {
        return halting_.load(std::memory_order_acquire);
    }

    void pump(std::uint64_t generation, runtime::IFalloutRuntime& runtime) {
        runtime.assert_game_thread();
        if(context_.take_wait_checkpoint_notice(generation))
            push_notification("SYNTH wait outcome included in the next save");
        if (terminal_.load(std::memory_order_acquire)) {
            runtime.update_conversation_holds({});
            (void)stop_local_speech(runtime);
            return;
        }
        const bool holding = !external_ready() || !speech_.captions_idle();
        runtime.update_conversation_holds(conversation_holds_.frame(holding,runtime.generation()));
        speech_.pump(generation, runtime);
        if (rechat_.pump_prefetched()) return;
        if (runtime_paused_.load(std::memory_order_acquire) || player_voice_active()) return;
        // Only the completed response's final currently playing line may authorize one speculative child.
        if (rechat_.promote_playing_parent()) return;
        if (!speech_.playback_idle() || speech_.jobs_active() ||
            requests_.responses() != 0 ||
            actions_.active() != 0 ||
            terminal_.load(std::memory_order_acquire)) {
            return;
        }
        // A queued action outranks rechat and diary work even while it waits for its social release.
        if (actions_.pump(runtime) != ActionExecution::Pump::idle) return;
        if (!speech_.captions_idle()) return;
        if (!rechat_.pump_delivered_parent(generation)) {
            diary_.pump_status();
            diary_.pump_automatic();
        }
    }

    // Return copied presentation ownership; native capture must occur outside the session receiver.
    [[nodiscard]] presentation::DialogueCaptions::Caption presentation_capture_candidate(core::SnapshotClock::time_point now) {
        return speech_.presentation_capture_candidate(now, runtime_paused() || composing_.load(std::memory_order_acquire));
    }

    // SpeechDelivery validates the copied scene; a rejected combat bark loses only its response, any
    // other rejection stops the dialogue.
    void submit_presentation_capture(const presentation::DialogueCaptions::Caption& caption, const Snapshot& snapshot) {
        switch (speech_.validate_presentation(caption, snapshot)) {
        case SpeechDelivery::PresentationCheck::rejected_combat: invalidate_response(caption->cancellation); break;
        case SpeechDelivery::PresentationCheck::rejected: stop_dialogue(); break;
        case SpeechDelivery::PresentationCheck::ignored:
        case SpeechDelivery::PresentationCheck::accepted: break;
        }
    }

    // The host captures outside this object: a game-thread capture can retire the whole session.
    [[nodiscard]] std::optional<integration::PromptTarget> rechat_capture_target() { return rechat_.capture_target(); }

    void submit_rechat_capture(Snapshot snapshot, const integration::PromptTarget& target) {
        rechat_.submit_capture(std::move(snapshot), target);
    }

    [[nodiscard]] bool begin_voice_capture(const Snapshot& snapshot) {
        return dialogue_.begin_voice_capture(snapshot,
            terminal_.load(std::memory_order_acquire) || runtime_paused_.load(std::memory_order_acquire),
            [this] { return begin_player_turn(); }, [this] { mark_activity(); }, current_controls());
    }

    void cancel_voice_capture() { dialogue_.cancel_voice_capture(); }

    // Called once on the game thread at speech admission, never from the microphone worker.
    void bind_voice_target(const Snapshot& snapshot) {
        dialogue_.bind_voice_target(snapshot, current_controls());
    }

    [[nodiscard]] std::uint32_t voice_target_form_id() const noexcept {
        return dialogue_.voice_target_form_id();
    }

    [[nodiscard]] bool end_voice_capture(Snapshot snapshot) {
        auto voice = dialogue_.end_voice_capture(snapshot, [this] {
            return runtime_paused_.load(std::memory_order_acquire) || terminal_.load(std::memory_order_acquire);
        });
        if (!voice) return false;
        const auto controls = voice->controls;
        const auto target = voice->target ? identity(*voice->target) : identity(snapshot->player());
        if (voice->target && !enrich_selected_actor(snapshot, target)) return false;
        const auto deadline = tasks::TaskLanes::Clock::now() +
                              std::chrono::milliseconds{request_timeout_ms() * 2ULL};
        if (!requests_.submit_voice(std::move(snapshot), std::move(voice->wave), target, controls, deadline, voice->turn))
            return false;
        dialogue_.own_external_work(actor_key(target));
        mark_activity();
        return true;
    }

    // Retain only identity before opening the pausing UI, never the old world snapshot.
    [[nodiscard]] std::optional<integration::PromptTarget> prepare_chat_target(const Snapshot& snapshot) {
        return dialogue_.prepare_chat_target(snapshot, current_controls());
    }

    [[nodiscard]] std::vector<integration::PromptTarget> prepare_chat_targets(const Snapshot& snapshot) {
        if (terminal_.load(std::memory_order_acquire)) return {};
        return dialogue_.prepare_chat_targets(snapshot, current_controls());
    }

    [[nodiscard]] bool submit_text(Snapshot snapshot, std::string text,
                                  const integration::PromptTarget* retained = nullptr) {
        if (!snapshot || !PlayerDialogue::text_within_bounds(text) ||
            terminal_.load(std::memory_order_acquire) || runtime_paused_.load(std::memory_order_acquire)) {
            return false;
        }
        if (!client_.ready()) {
            push_notification("SYNTH is still connecting; try your message again when connected");
            return false;
        }
        auto turn = dialogue_.admit_text(*snapshot, std::move(text), retained, client_.native_turn_ready(),
                                         current_controls());
        if (!turn) return false;
        switch (turn->command) {
        case PlayerDialogue::TextCommand::diary_npc: return request_npc_diary(std::move(snapshot), retained);
        case PlayerDialogue::TextCommand::diary_narrator: return request_diary(std::move(snapshot), protocol_native::DiaryRole::narrator);
        case PlayerDialogue::TextCommand::diary_player: return request_diary(std::move(snapshot), protocol_native::DiaryRole::player);
        case PlayerDialogue::TextCommand::text: break;
        }
        const auto target = turn->target ? identity(*turn->target) : identity(snapshot->player());
        return queue_text_for_actor(std::move(snapshot), std::move(turn->text), target, turn->controls, turn->intent);
    }

    // One-shot integrations still require a completely idle pipeline.
    [[nodiscard]] bool external_ready() {
        const auto idle=client_.ready() && !terminal_.load(std::memory_order_acquire) && !runtime_paused() &&
            !dialogue_.recording() && actions_.active()==0 &&
            requests_.responses()==0 && !speech_.jobs_active() &&
            speech_.playback_idle();
        if (!idle) return false;
        return rechat_.try_idle();
    }

    // Queue admission does not grant execution: only exact-owner Ask may pass a busy pipeline later.
    [[nodiscard]] bool external_admission_ready() {
        if (!client_.ready() || terminal_.load(std::memory_order_acquire) || runtime_paused() || dialogue_.recording()) return false;
        return !external_ready() || dialogue_.try_idle_external();
    }

    // Called only by the safe native pump after bounded API admission and scene validation.
    [[nodiscard]] bool submit_external(Snapshot snapshot, const SynthExternalRequestV1& request) {
        if (request.kind == SYNTH_EXTERNAL_OPEN_PROMPT) return false; // Presentation must go through the runtime's prompt callback.
        if (!snapshot || !external_admission_ready() || snapshot->generation()!=cancellation_.token().generation()) return false;
        const auto idle = external_ready();
        if (!idle && request.kind != SYNTH_EXTERNAL_ASK) return false;
        const auto* selected=dialogue_.external_actor(*snapshot,request.actor_form_id,core::SnapshotClock::now());
        if (!selected) return false;
        const auto target=identity(*selected);
        if (request.kind==SYNTH_EXTERNAL_ASK) {
            if (!dialogue_.try_allows_ask(actor_key(target),idle)) return false;
            return queue_text_for_actor(std::move(snapshot),std::string{request.text},target);
        }
        const auto kind=request.kind==SYNTH_EXTERNAL_SPEAK_EXACT ? "external_tts" :
            request.kind==SYNTH_EXTERNAL_COMMENT ? "external_comment" : "external_reaction";
        const auto accepted=activation_.queue_context_trigger(kind,target,std::move(snapshot),"external request failed",
            request.kind==SYNTH_EXTERNAL_COMMENT ? std::nullopt : std::optional{std::string{request.text}});
        if (accepted) mark_activity();
        return accepted;
    }

    // Freeze only the prompt's intended identity; submission must capture a new scene, not reuse this snapshot.
    [[nodiscard]] std::optional<integration::PromptTarget> prepare_external_prompt(Snapshot snapshot, std::uint32_t form_id) {
        if (!snapshot || !external_admission_ready() || snapshot->generation()!=cancellation_.token().generation()) return {};
        const auto* actor=dialogue_.external_actor(*snapshot,form_id,core::SnapshotClock::now());
        if (!actor) return {};
        const auto idle=external_ready();
        if (!dialogue_.try_allows_ask(actor_key(identity(*actor)),idle)) return {};
        return integration::PromptTarget{cancellation_.token(),actor->form_id(),actor->base_form_id(),
            actor->origin_plugin(),actor->base_origin_plugin(),actor->playthrough_id(),actor->name()};
    }

    // A targeted draft cannot turn into ordinary crosshair input after load, replacement or session retirement.
    [[nodiscard]] bool submit_prompt(Snapshot snapshot, const integration::PromptTarget& target, std::string text) {
        if (target.session.is_cancelled() || target.session.generation()!=cancellation_.token().generation() ||
            !PlayerDialogue::text_within_bounds(text)) return false;
        const auto current=prepare_external_prompt(snapshot,target.form_id);
        if (!current || current->origin_plugin!=target.origin_plugin || current->base_form_id!=target.base_form_id ||
            current->base_origin_plugin!=target.base_origin_plugin || current->playthrough_id!=target.playthrough_id) return false;
        const auto found=std::ranges::find_if(snapshot->actors(),[&](const auto& actor){return actor.form_id()==target.form_id;});
        if (found==snapshot->actors().end()) return false;
        const auto actor=identity(*found);
        return queue_text_for_actor(std::move(snapshot),std::move(text),actor);
    }

private:
    // Both typed input and external Ask retain the same exact actor through context publication and response delivery.
    [[nodiscard]] bool queue_text_for_actor(Snapshot snapshot, std::string text, protocol_native::Identity target,
                                          std::optional<protocol_native::ControlSelection> retained_controls = {},
                                          protocol_native::TextIntent intent = protocol_native::TextIntent::unmarked) {
        const auto controls = retained_controls.value_or(server_control_.state().selection);
        if (controls.mode != "NARRATOR" && controls.mode != "INJECTION_LOG" && !enrich_selected_actor(snapshot, target)) return false;
        const auto deadline = tasks::TaskLanes::Clock::now() +
                              std::chrono::milliseconds{request_timeout_ms() * 2ULL};
        if (!requests_.submit_text(std::move(snapshot), std::move(text), target, controls, intent,
                                   deadline, begin_player_turn(actor_key(target)))) return false;
        claim_external_work(actor_key(target));
        mark_activity();
        return true;
    }

public:
    [[nodiscard]] static std::optional<std::string> clipboard_text() {
        if (!OpenClipboard(nullptr)) return std::nullopt;
        struct ClipboardClose final { ~ClipboardClose() { CloseClipboard(); } } close;
        const auto handle = GetClipboardData(CF_UNICODETEXT);
        if (handle == nullptr) return std::nullopt;
        const auto* wide = static_cast<const wchar_t*>(GlobalLock(handle));
        if (wide == nullptr) return std::nullopt;
        struct ClipboardUnlock final {
            HANDLE handle;
            ~ClipboardUnlock() { ::GlobalUnlock(handle); }
        } unlock{handle};
        std::size_t length{};
        while (length <= 4096 && wide[length] != L'\0') ++length;
        if (length == 0 || length > 4096) return std::nullopt;
        const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide,
                                               static_cast<int>(length), nullptr, 0,
                                               nullptr, nullptr);
        if (bytes <= 0 || bytes > 4096) return std::nullopt;
        std::string result(static_cast<std::size_t>(bytes), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, static_cast<int>(length),
                                result.data(), bytes, nullptr, nullptr) != bytes) {
            return std::nullopt;
        }
        return result;
    }

    using MicrophoneEvent = PlayerDialogue::MicrophoneEvent;

    // Only speech boundaries request a game-thread snapshot; idle/recording frames do no scene work.
    [[nodiscard]] MicrophoneEvent pump_open_microphone(core::SnapshotClock::time_point now) {
        const auto pumped = dialogue_.pump_open_microphone(now,
            runtime_paused_.load(std::memory_order_acquire) || composing_.load(std::memory_order_acquire) ||
            terminal_.load(std::memory_order_acquire), [this] { return begin_player_turn(); });
        if (pumped.voiced) mark_activity(now);
        return pumped.event;
    }

    void toggle_open_mic_mute() {
        configuration_.audio.open_mic_muted = !configuration_.audio.open_mic_muted;
        if (configuration_.audio.open_mic_muted) {
            cancel_voice_capture();
        }
        push_notification(configuration_.audio.open_mic_muted
                              ? "SYNTH open microphone muted"
                              : "SYNTH open microphone unmuted");
    }

    void open_control_panel() {
        const auto target = configuration_.server.base_url + "ui/";
        const auto result = reinterpret_cast<std::intptr_t>(
            ShellExecuteA(nullptr, "open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        push_notification(result > 32 ? "SYNTH control panel opened"
                                      : "SYNTH could not open the control panel");
    }

    // VisualCapture owns admission, the copied payload, pixel freeze and its worker; the pointed
    // subject and the "describe" turn replacement stay here and run during this call only.
    [[nodiscard]] bool submit_visual_capture(Snapshot snapshot, std::string interaction_mode) {
        return vision_.submit(std::move(snapshot), std::move(interaction_mode),
            [this](const core::RuntimeSnapshot& current) -> std::optional<protocol_native::Identity> {
                if (auto target = PlayerDialogue::pointed_subject(current)) return identity(*target);
                return std::nullopt;
            },
            [this](std::string actor) { return begin_player_turn(std::move(actor)); });
    }

    // Copy the pointed actor and snapshot on the game thread; admission runs only on the network lane.
    [[nodiscard]] bool request_npc_diary(Snapshot snapshot, const integration::PromptTarget* retained = nullptr) {
        return request_diary(std::move(snapshot), protocol_native::DiaryRole::npc, retained);
    }

    [[nodiscard]] bool automatic_diary_ready() const { return diary_.automatic_ready(); }

    // DiaryRequests owns the rest queue, pending diary and every diary RPC; the session keeps selection.
    [[nodiscard]] std::size_t queue_automatic_diaries(Snapshot snapshot, const core::RestEventBatch& starts) {
        return diary_.queue_automatic(std::move(snapshot), starts);
    }

    [[nodiscard]] bool request_diary(Snapshot snapshot, protocol_native::DiaryRole role,
                                   const integration::PromptTarget* retained = nullptr) {
        return diary_.request(std::move(snapshot), role,
            [this, retained](const core::RuntimeSnapshot& current) -> std::optional<protocol_native::Identity> {
                const auto selected = retained ? dialogue_.selected_target(current, false, retained)
                                               : PlayerDialogue::pointed_subject(current);
                return selected ? std::optional{identity(*selected)} : std::nullopt;
            });
    }

    // Native adapters capture only when a finished diary needs a new save frontier, never on the worker.
    [[nodiscard]] bool diary_checkpoint_needed() {
        if (terminal_.load(std::memory_order_acquire) || runtime_paused_.load(std::memory_order_acquire) ||
            requests_.responses() != 0 ||
            actions_.active() != 0 || speech_.jobs_active() ||
            dialogue_.recording() || !speech_.playback_idle()) return false;
        return diary_.checkpoint_due();
    }

    [[nodiscard]] bool checkpoint_diary(Snapshot snapshot) {
        if (!snapshot || snapshot->generation() != cancellation_.token().generation() || !diary_checkpoint_needed()) return false;
        const auto selected = dialogue_.selected_target(*snapshot, false);
        const auto target = selected ? std::optional{identity(*selected)} : std::nullopt;
        return diary_.checkpoint(std::move(snapshot), target);
    }

    // ContextPublication copies identities and target here and publishes before the refresh RPC.
    [[nodiscard]] bool refresh_dynamic_profiles(Snapshot snapshot, std::string_view scope = "automatic", std::uint32_t target_form_id = 0) {
        return context_.refresh_dynamic_profiles(std::move(snapshot), scope, target_form_id);
    }

    [[nodiscard]] static config::Config load_configuration() {
        const auto root = std::filesystem::current_path();
        return config::load(root / "Data/F4SE/Plugins/SYNTH.ini",
                            root / "Data/F4SE/Plugins/SYNTH_custom.ini");
    }

private:
    [[nodiscard]] core::CancellationToken current_turn_token() { return dialogue_.current_turn(); }

    // Per-call views of session state passed to the dialogue owner; never retained by it.
    [[nodiscard]] PlayerDialogue::Controls current_controls() {
        return [this] { return server_control_.state().selection; };
    }

    // Game-thread admission only: invalidate old chains without HTTP or worker joins.
    [[nodiscard]] core::CancellationToken begin_player_turn(std::string actor = {}) {
        // A walk survives unrelated player input; only an already-started exchange is interrupted.
        social_.interrupt_dialogue();
        conversation_holds_.clear();
        trace_->record(diagnostics::ConversationStage::turn_admitted,cancellation_.token().generation().value());
        const auto turn = dialogue_.replace_turn();
        (void)network_.discard_cancelled();
        (void)speech_.discard_cancelled_jobs();
        dialogue_.begin_external(std::move(actor));
        dialogue_.cancel_interrupted_turn();
        speech_.interrupt_playback();
        actions_.clear();
        rechat_.clear();
        return turn;
    }

    // A late stream failure invalidates its owned work, never the replacement turn's audio.
    void invalidate_response(core::CancellationToken cancellation) {
        if(!dialogue_.cancel_turn_if_owner(cancellation)&&!social_.invalidate_dialogue(cancellation))return;
        dialogue_.cancel_interrupted_turn();
    }

    void claim_external_work(std::optional<std::string> actor) { dialogue_.claim_external(std::move(actor)); }

    [[nodiscard]] std::uint64_t request_timeout_ms() const noexcept {
        return static_cast<std::uint64_t>(configuration_.behavior.ai_response_timeout_seconds) * 1000ULL;
    }

    // Shared activity clock behind the bored claim; ActivationTriggers owns it.
    void mark_activity(core::SnapshotClock::time_point when = core::SnapshotClock::now()) {
        activation_.mark_activity(when);
    }

    [[nodiscard]] static std::string make_runtime_session_id(core::RuntimeVariant variant) {
        static std::atomic_uint64_t sequence{};
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::string{"runtime:"} +
               (variant == core::RuntimeVariant::vr ? "vr:" : "flat:") +
               std::to_string(GetCurrentProcessId()) + ":" + std::to_string(ticks) + ":" +
               std::to_string(sequence.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    [[nodiscard]] RequestOutcome publish_snapshot(const Snapshot& owned_snapshot,
                                                   const std::optional<protocol_native::Identity>& target, core::CancellationToken cancellation,
                                                   std::vector<protocol_native::Identity>* published_audience = nullptr,
                                                   const std::optional<protocol_native::NativeQuestBatch>& quest_events = std::nullopt,
                                                   const std::optional<protocol_native::NativePlayerEvent>& player_event = std::nullopt,
                                                   const std::shared_ptr<const ActorEventDelivery::Prepared>& actor_events = {},
                                                   const protocol_native::ControlSelection& controls = {},
                                                   const runtime::SocialTaskDescriptor* social_pair = nullptr) {
        return context_.publish(owned_snapshot, target, std::move(cancellation), published_audience, quest_events,
                                player_event, actor_events, controls, social_pair);
    }

    void handle_line(const protocol_native::Line& line, const Snapshot& snapshot,
                     const core::CancellationToken& cancellation, bool allow_followup = true,
                     const std::shared_ptr<RechatPrefetch>& prefetch = {}, bool combat_bark = false) {
        if (cancellation.is_cancelled() || !snapshot || line.generation != snapshot->generation().value() || line.context_sequence == 0
            || terminal_.load(std::memory_order_acquire)) return;
        trace_->record(diagnostics::ConversationStage::line_applied,line.generation,line.context_sequence,line.request_id,line.line_id);
        if(const auto* offer=std::get_if<protocol_native::SocialTaskOffer>(&line.payload)) {
            social_.accept_offer(offer->task,snapshot);return;
        }
        if (const auto* status = std::get_if<protocol_native::Status>(&line.payload);
            status && status->code == "speech_not_recognized") {
            push_notification("SYNTH did not recognize speech; try again.");
            return;
        }
        if (const auto* status = std::get_if<protocol_native::Status>(&line.payload);
            status && status->code == "combat_identification_complete") {
            rechat_.drop_combat_parent(line);
            return;
        }
        if (const auto* end = std::get_if<protocol_native::End>(&line.payload); end && allow_followup) {
            if(end->status=="complete")social_.complete_request(line.request_id);
            rechat_.complete_parent(line.request_id, end->status == "complete");
            return;
        }
        if (const auto* dialogue = std::get_if<protocol_native::Dialogue>(&line.payload)) {
            context_.note_dialogue_history(snapshot->variant(), cancellation);
            mark_activity();
            const auto speaker = std::visit([](const auto& value) { return value.display_name; },
                                           dialogue->speaker);
            // Preserve VR's existing presentation until its separate HUD ABI is verified.
            if (snapshot->variant() == core::RuntimeVariant::vr && configuration_.behavior.show_subtitles)
                push_notification((speaker.empty() ? "SYNTH" : speaker) + ": " + dialogue->text);
            const auto caption = speech_.make_caption(line, *dialogue, cancellation, snapshot, combat_bark, conversation_holds_);
            const bool social_reply=social_.track(line,caption);
            speech_.deliver(line, *dialogue, snapshot, cancellation, caption, prefetch);
            if (!allow_followup||social_reply) return;
            if (const auto* actor = std::get_if<protocol_native::Identity>(&dialogue->speaker))
                rechat_.observe_speaker(line, *actor, snapshot, cancellation, caption, response_binding(line, cancellation));
            return;
        }
        const auto* action = std::get_if<protocol_native::Action>(&line.payload);
        if (action == nullptr || !allow_followup) return;
        if(social_.owns_request(line.request_id))return;
        if (line.generation != snapshot->generation().value()) {
            return;
        }
        conversation_holds_.suspend(cancellation); // Release participants before any requested action is dispatched.
        actions_.enqueue(*action, snapshot, response_binding(line, cancellation));
    }

    // Continuations retain the same rollback owner without reading the latest selected actor.
    [[nodiscard]] ContextBinding response_binding(const protocol_native::Line& line,
                                                   core::CancellationToken cancellation) {
        return {line.generation, line.context_sequence, cancellation, line.turn_id, {}, [this, cancellation] {
            invalidate_response(cancellation);
        }};
    }

    [[nodiscard]] static protocol_native::Identity identity(const core::ActorSnapshot& actor) {
        return PlayerDialogue::identity(actor);
    }

    [[nodiscard]] static protocol_native::Identity identity(const context::TargetContext& actor) {
        return PlayerDialogue::identity(actor);
    }

    [[nodiscard]] static std::string actor_key(const protocol_native::Identity& actor) {
        return PlayerDialogue::actor_key(actor);
    }

    [[nodiscard]] bool conversation_on_cooldown(
        const protocol_native::Identity& actor,
        core::SnapshotClock::time_point now) {
        return dialogue_.on_cooldown(actor_key(actor), now);
    }

    // Ends only SYNTH-owned dialogue state; vanilla Fallout dialogue remains untouched.
    void end_conversation(const protocol_native::Identity& actor) {
        const auto key = actor_key(actor);
        dialogue_.end_conversation(key, core::SnapshotClock::now() + std::chrono::seconds{
            configuration_.behavior.end_conversation_cooldown_seconds});
        activation_.forget(key);
        rechat_.forget(key);
    }

    void push_notification(std::string message) {
        if (message.empty()) {
            return;
        }
        std::scoped_lock lock{notification_mutex_};
        if (notifications_.size() == 16) {
            notifications_.pop_front();
        }
        notifications_.push_back(std::move(message));
    }

    // Game-thread runtime capture before worker admission; see ObservedEventDelivery::enrich_selected_actor.
    [[nodiscard]] bool enrich_selected_actor(Snapshot& snapshot, const protocol_native::Identity& selected) {
        return observations_.enrich_selected_actor(snapshot, selected);
    }

    runtime::GameThreadDispatcher& dispatcher_;
    config::Config configuration_;
    WinHttpTransport transport_;
    SynthClient client_;
    core::CancellationSource cancellation_;
    tasks::TaskLanes network_;
    tasks::TaskLanes control_{{tasks::LaneConfig{"control", 4, 4, 4}}, 1};
    std::atomic_bool terminal_{};
    std::atomic_bool runtime_paused_{};
    std::atomic_bool composing_{};
    std::atomic_bool halting_{};
    std::atomic_size_t halt_requests_{};
    ConversationHolds conversation_holds_;
    // Borrows configuration, client, network lane and the terminal flag; owns the confirmed control
    // selection and its pending revision. Its settings RPC captures it; ~PluginSession joins the
    // network lane first. Dialogue, request and activation callbacks read it per call.
    ServerControl server_control_{{configuration_, client_, network_, terminal_, cancellation_.token()}};
    // The single quest event buffer. Declared before both borrowers: context_ acknowledges journal
    // witnesses on its workers, and observations_ owns its scheduling, delivery and stop.
    QuestEventDelivery quest_events_;
    std::shared_ptr<diagnostics::ConversationTrace> trace_{std::make_shared<diagnostics::ConversationTrace>()};
    std::mutex notification_mutex_;
    std::deque<std::string> notifications_;
    // Borrows configuration, client, control lane and trace declared above; ~PluginSession joins the
    // control lane, whose cancel tasks capture this owner, before members are destroyed. Also the
    // exact selected-target authority read by context publication, rechat and diary admission.
    PlayerDialogue dialogue_;
    // Borrows client, network lane, terminal flag and holds declared above; its callbacks capture this
    // session. ~PluginSession joins the network lane before members are destroyed, so every queued,
    // running, rejected or expired request task (and its response-count guard) is gone first.
    DialogueRequests requests_;
    // Declared after every borrowed dependency, so it is destroyed before them. ~PluginSession joins
    // network, then speech jobs, then control, then halts playback; the member destructor repeats
    // join/halt as idempotent no-ops. If a later member's constructor throws, that fallback alone
    // runs, before any pump could have queued a receipt drain on the still-live network lane. Captions
    // are built on the delivering worker with the holds passed per call; presentation validation runs
    // on the game thread and returns a result the session routes.
    SpeechDelivery speech_;
    voice::SampleWorker voice_samples_;
    // Borrows configuration, client, network lane, response accounting, voice samples, quest
    // delivery, the terminal/pause flags and the save store. Its queued tasks (including profile
    // refresh) and the rollback closure in each ContextBinding capture it; ~PluginSession joins the
    // network lane first.
    ContextPublication context_;
    // Borrows configuration, client, network lane, response accounting, context publication and the
    // terminal flag. Its network tasks and progress bindings capture it and its callbacks capture this
    // session; ~PluginSession joins the network lane first. Native visits run only in its game-thread
    // pump and in the action queue's release hook, never from a worker.
    SocialTaskExecution social_;
    // Borrows configuration, client, network lane, dispatcher, dialogue, context and the terminal
    // flag. Its network tasks capture it and its callbacks capture this session; ~PluginSession joins
    // the network lane first. Dispatcher tasks never capture it.
    ActionExecution actions_;
    // Borrows configuration, client, network lane, response accounting, speech, context, social
    // tasks, the action queue (for the action-then-rechat gates), dialogue, holds, trace and the
    // terminal/pause flags. Its network tasks capture it and its callbacks capture this session;
    // ~PluginSession joins the network lane first. Speculative media jobs on the speech lane capture
    // only the copied line and shared prefetch.
    RechatContinuation rechat_;
    // Borrows configuration, client, network lane, context, quest delivery and the
    // terminal/pause flags. Its busy, turn and notification callbacks capture this session; busy reads dialogue,
    // response, action and speech state on the game thread. Its network tasks capture it, and
    // ~PluginSession joins the network lane first, so no task outlives it.
    ObservedEventDelivery observations_;
    // Borrows configuration, client, network lane, response accounting, context, dialogue, observed
    // events (selected-actor enrichment) and the terminal/pause/composing flags.
    // Its ambient-mode and line callbacks capture this session. Its network tasks capture it, and
    // ~PluginSession joins the network lane first, so no task outlives it.
    ActivationTriggers activation_;
    // Borrows configuration, client, network lane, context and the terminal/pause/halting flags; owns
    // the diary task counter, pending diary and automatic rest queue. Its network tasks capture it,
    // and ~PluginSession joins the network lane first, so no task or counter guard outlives it.
    DiaryRequests diary_;
    // Declared last: borrows configuration, client, network lane, response accounting, context,
    // dialogue, observed events and the terminal/pause/halting flags; owns the visual task counter.
    // Its line callback captures this session. Its network task captures it, and ~PluginSession
    // joins the network lane first, so no task or counter guard outlives it.
    VisualCapture vision_;
};

}  // namespace synth::client
