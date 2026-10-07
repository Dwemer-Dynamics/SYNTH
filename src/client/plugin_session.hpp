#pragma once

#include "audio/dialogue_audio.hpp"
#include "audio/native_playback.hpp"
#include "client/synth_client.hpp"
#include "client/rechat_prefetch.hpp"
#include "client/action_inventory.hpp"
#include "client/speech_facing_binding.hpp"
#include "client/quest_event_delivery.hpp"
#include "client/actor_event_delivery.hpp"
#include "client/winhttp_transport.hpp"
#include "config/config.hpp"
#include "core/snapshot.hpp"
#include "core/player_event_sampler.hpp"
#include "core/save_context.hpp"
#include "core/automatic_diary_queue.hpp"
#include "input/native_microphone.hpp"
#include "integration/external_requests.hpp"
#include "integration/prompt_target.hpp"
#include "media_fetch/media_fetch.hpp"
#include "media_capture/window_capture.hpp"
#include "runtime/game_thread_dispatcher.hpp"
#include "runtime/action_completion.hpp"
#include "actions/equipment.hpp"
#include "targeting/targeting.hpp"
#include "tasks/task_lanes.hpp"
#include "voice/voice_sample_worker.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <charconv>
#include <condition_variable>
#include <cmath>
#include <cstdio>
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

class PluginSession final {
public:
    using Snapshot = std::shared_ptr<const core::RuntimeSnapshot>;
    using ActorEnricher = std::function<Snapshot(const Snapshot&, std::uint32_t)>;

    // Installed on the game thread when the coordinator-created session is adopted.
    void set_actor_enricher(ActorEnricher enrich) { actor_enricher_ = std::move(enrich); }

    PluginSession(config::Config configuration,
                  core::RuntimeVariant variant,
                  std::string runtime_version,
                  runtime::GameThreadDispatcher& dispatcher,
                  core::RuntimeGeneration generation,
                  core::SaveContextStore* save_context = nullptr,
                  core::SaveContextStore::Binding save_binding = {}, bool rest_events_available = false,
                  bool quest_events_available = false, bool actor_events_available = false)
        : dispatcher_{dispatcher},
          save_context_{save_context},
          save_binding_{std::move(save_binding)},
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
                      .capabilities = capabilities(variant, rest_events_available, quest_events_available, actor_events_available),
                      .request_timeout_ms = configuration_.behavior.ai_response_timeout_seconds * 1000U,
                      .maximum_response_bytes = configuration_.limits.max_response_bytes,
                      .protocol_version = 2,
                  }},
          cancellation_{generation},
          turn_cancellation_{cancellation_.token()},
          network_({tasks::LaneConfig{"network", configuration_.limits.max_pending_requests, 1, 3}}, 2),
          automatic_diary_enabled_{variant == core::RuntimeVariant::flat && rest_events_available},
          automatic_diaries_{generation},
          quest_events_enabled_{variant == core::RuntimeVariant::flat && quest_events_available},
          quest_events_{generation},
          actor_events_enabled_{variant == core::RuntimeVariant::flat && actor_events_available},
          actor_events_{client_.runtime_session_id(),generation},
          player_events_{generation, variant},
          voice_samples_{configuration_.server.base_url, configuration_.server.connect_timeout_ms,
                         cancellation_.token(), client_.runtime_session_id()} {}

    PluginSession(const PluginSession&) = delete;
    PluginSession& operator=(const PluginSession&) = delete;

    ~PluginSession() {
        request_stop();
        microphone_.cancel();
        transport_.cancel_all();
        network_.shutdown(tasks::ShutdownMode::cancel_pending);
        speech_.shutdown(tasks::ShutdownMode::cancel_pending);
        control_.shutdown(tasks::ShutdownMode::cancel_pending);
        playback_.halt();
    }

    [[nodiscard]] bool initialize(Snapshot snapshot) {
        if (!snapshot || snapshot->generation() != cancellation_.token().generation() ||
            initialized_.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }
        if (!submit(std::move(snapshot), true)) {
            initialized_.store(false, std::memory_order_release);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool publish_context(Snapshot snapshot) {
        if (!snapshot || !client_.ready()) {
            return false;
        }
        return submit(std::move(snapshot), false);
    }

    [[nodiscard]] bool initialization_failed() const noexcept {
        return initialization_failed_.load(std::memory_order_acquire);
    }

    // Save/settings invalidation stops admission immediately; the coordinator
    // performs potentially blocking transport/audio teardown after retirement.
    void request_stop() noexcept {
        trace_->record(diagnostics::ConversationStage::session_retired,cancellation_.token().generation().value());
        terminal_.store(true, std::memory_order_release);
        if (save_context_) save_context_->stop_writer(save_binding_.epoch);
        cancellation_.cancel();
        quest_events_.stop();
        actor_events_.stop();
        { std::scoped_lock lock{diary_mutex_}; automatic_diaries_.clear(); }
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
    [[nodiscard]] std::string voice_sample_status() const { return voice_samples_.status(); }

    // Check before optional native capture; queue admission repeats this gate afterward.
    [[nodiscard]] bool context_observation_ready() const {
        return !(!client_.ready() || terminal_.load(std::memory_order_acquire) ||
            runtime_paused_.load(std::memory_order_acquire) || microphone_.recording() ||
            active_responses_.load(std::memory_order_acquire) != 0 ||
            active_actions_.load(std::memory_order_acquire) != 0 ||
            speech_jobs_.load(std::memory_order_acquire) != 0 || !playback_.idle() ||
            world_context_tasks_.load(std::memory_order_acquire) != 0);
    }

    [[nodiscard]] std::uint64_t acknowledged_inventory_frame() const noexcept {
        return acknowledged_inventory_frame_.load(std::memory_order_acquire);
    }

    // Persist observations without activation or model work. Inventory reconciliation may force unchanged world context.
    [[nodiscard]] bool observe_world_context(Snapshot snapshot, bool inventory_refresh = false) {
        if (!snapshot || !context_observation_ready() ||
            snapshot->generation() != cancellation_.token().generation() ||
            !world_context_needed(*snapshot, inventory_refresh)) return false;
        const auto accepted = network_.try_submit("network", tasks::TaskClass::background,
            tasks::TaskLanes::Clock::now() + std::chrono::milliseconds{request_timeout_ms()}, current_turn_token(),
            tasks::TaskLanes::track_activity(world_context_tasks_,
            [this, snapshot = std::move(snapshot), inventory_refresh](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire) ||
                    runtime_paused_.load(std::memory_order_acquire)) return;
                try {
                    const auto context_turn = client_.begin_context_turn();
                    // A normal request may have acknowledged a newer scene while this work waited.
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire) ||
                        runtime_paused_.load(std::memory_order_acquire) || !world_context_needed(*snapshot, inventory_refresh)) return;
                    const auto result = publish_snapshot(snapshot, std::nullopt, cancellation);
                    if (result.status == RequestStatus::complete) return;
                } catch (const std::exception&) {}
                if (!cancellation.is_cancelled() && !terminal_.load(std::memory_order_acquire) &&
                    !world_context_failure_reported_.exchange(true, std::memory_order_acq_rel))
                    push_notification("SYNTH world context was not saved; retrying on a later observation");
            }));
        return accepted == tasks::SubmitResult::accepted;
    }

    // The adapter must bind copied scalars to this generation's capture before calling; no engine pointers enter the queue.
    [[nodiscard]] bool observe_quest_events(Snapshot snapshot, std::span<const core::QuestEvent> events) {
        if (!quest_events_enabled_ || !quest_event_ready_.load(std::memory_order_acquire) ||
            terminal_.load(std::memory_order_acquire)) return false;
        return quest_events_.enqueue(std::move(snapshot), events);
    }

    // Called by the game pump; one background attempt per interval, with all encoding and HTTP on the network lane.
    void pump_quest_events() {
        if (!quest_events_enabled_ || !quest_event_ready_.load(std::memory_order_acquire) || !quest_events_.pending() ||
            terminal_.load(std::memory_order_acquire) || runtime_paused_.load(std::memory_order_acquire) ||
            microphone_.recording() || active_responses_.load(std::memory_order_acquire) != 0 ||
            active_actions_.load(std::memory_order_acquire) != 0 || speech_jobs_.load(std::memory_order_acquire) != 0 ||
            !playback_.idle() || quest_event_tasks_.load(std::memory_order_acquire) != 0) return;
        const auto now = tasks::TaskLanes::Clock::now();
        if (now < next_quest_event_attempt_) return;
        next_quest_event_attempt_ = now + std::chrono::seconds{5};
        (void)network_.try_submit("network", tasks::TaskClass::background,
            now + std::chrono::milliseconds{request_timeout_ms()}, current_turn_token(),
            tasks::TaskLanes::track_activity(quest_event_tasks_,
            [this](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire) ||
                    runtime_paused_.load(std::memory_order_acquire)) return;
                std::shared_ptr<const QuestEventDelivery::Prepared> batch;
                try {
                    batch = quest_events_.prepare();
                    if (!batch) return;
                    const auto context_turn = client_.begin_context_turn();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    const auto result = publish_snapshot(batch->snapshot, std::nullopt, cancellation, nullptr, batch->batch);
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    if (result.status == RequestStatus::complete) {
                        (void)quest_events_.finish(batch, true);
                        quest_event_failure_reported_.store(false, std::memory_order_release);
                    } else if (result.status == RequestStatus::http_failure && result.http_status >= 400 &&
                               result.http_status < 500 && result.http_status != 408 && result.http_status != 429) {
                        (void)quest_events_.finish(batch, false);
                        push_notification("SYNTH quest events rejected; check logs before enabling further capture");
                    } else if (!quest_event_failure_reported_.exchange(true, std::memory_order_acq_rel)) {
                        push_notification("SYNTH quest events not acknowledged; retaining capture for retry");
                    }
                } catch (const std::invalid_argument&) {
                    (void)quest_events_.finish(batch, false);
                    push_notification("SYNTH quest batch failed validation and was not sent");
                } catch (const std::exception&) {
                    if (!quest_event_failure_reported_.exchange(true, std::memory_order_acq_rel))
                        push_notification("SYNTH quest event delivery failed; retaining capture for retry");
                }
            }));
    }

    [[nodiscard]] std::uint64_t dropped_quest_events() const noexcept { return quest_events_.dropped(); }
    [[nodiscard]] std::uint64_t unobserved_quest_events() const noexcept { return quest_events_.unobserved(); }
    [[nodiscard]] bool quest_events_ready() const noexcept {
        return quest_event_ready_.load(std::memory_order_acquire) && !terminal_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool quest_event_capture_ready() const noexcept { return quest_events_ready() && !quest_events_.full(); }

    [[nodiscard]] bool actor_events_ready() const noexcept {
        return actor_events_enabled_ && client_.actor_events_ready() && !terminal_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool actor_event_capture_ready() const noexcept { return actor_events_ready() && !actor_events_.full(); }
    [[nodiscard]] const std::string& actor_event_session_id() const noexcept { return client_.runtime_session_id(); }
    [[nodiscard]] std::uint64_t dropped_actor_events() const noexcept { return actor_events_.dropped(); }
    [[nodiscard]] bool observe_actor_events(ActorEventDelivery::Capture capture) {
        return actor_event_capture_ready() && actor_events_.enqueue(std::move(capture));
    }

    // Game-pump scheduling only. Fragment preparation, exact-state selection and HTTP all run on the bounded network lane.
    void pump_actor_events() {
        if (!actor_events_enabled_ || !client_.actor_events_ready() || !actor_events_.pending() ||
            terminal_.load(std::memory_order_acquire) || runtime_paused_.load(std::memory_order_acquire) ||
            microphone_.recording() || active_responses_.load(std::memory_order_acquire)!=0 ||
            active_actions_.load(std::memory_order_acquire)!=0 || speech_jobs_.load(std::memory_order_acquire)!=0 ||
            !playback_.idle() || actor_event_tasks_.load(std::memory_order_acquire)!=0) return;
        const auto now=tasks::TaskLanes::Clock::now();
        if (now<next_actor_event_attempt_) return;
        next_actor_event_attempt_=now+std::chrono::seconds{5};
        (void)network_.try_submit("network",tasks::TaskClass::background,
            now+std::chrono::milliseconds{request_timeout_ms()},current_turn_token(),
            tasks::TaskLanes::track_activity(actor_event_tasks_,[this](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire) ||
                    runtime_paused_.load(std::memory_order_acquire)) return;
                std::shared_ptr<const ActorEventDelivery::Prepared> batch;
                try {
                    batch=actor_events_.prepare();if (!batch) return;
                    const auto context_turn=client_.begin_context_turn();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    const auto result=publish_snapshot(batch->capture->scene,std::nullopt,cancellation,nullptr,std::nullopt,std::nullopt,batch);
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    if (result.status==RequestStatus::complete) {
                        (void)actor_events_.finish(batch,true);actor_event_failure_reported_.store(false,std::memory_order_release);
                    } else if (result.status==RequestStatus::http_failure && result.http_status>=400 &&
                        result.http_status<500 && result.http_status!=408 && result.http_status!=429) {
                        (void)actor_events_.finish(batch,false);
                        push_notification("SYNTH actor events rejected; check logs before further capture");
                    } else if (!actor_event_failure_reported_.exchange(true,std::memory_order_acq_rel))
                        push_notification("SYNTH actor events not acknowledged; retaining original capture for retry");
                } catch (const std::invalid_argument&) {
                    (void)actor_events_.finish(batch,false);
                    push_notification("SYNTH actor batch failed validation and was not sent");
                } catch (const std::exception&) {
                    if (!actor_event_failure_reported_.exchange(true,std::memory_order_acq_rel))
                        push_notification("SYNTH actor event delivery failed; retaining original capture for retry");
                }
            }));
    }

    // Game-pump-only capture cadence: independent of networking, microphone and dialogue activity.
    [[nodiscard]] bool player_event_sample_due(core::SnapshotClock::time_point now) {
        return !terminal_.load(std::memory_order_acquire) && client_.player_events_ready() && player_events_.due(now);
    }

    void observe_player_events(std::optional<core::PlayerEventSample> sample) {
        if (terminal_.load(std::memory_order_acquire)) return;
        const auto previous = player_events_.front() ? player_events_.front()->serial : 0;
        if (sample) (void)player_events_.observe(std::move(*sample));
        else player_events_.unavailable();
        if (player_event_owner_ && previous != (player_events_.front() ? player_events_.front()->serial : 0))
            player_event_owner_->cancel();
    }

    [[nodiscard]] std::array<std::uint64_t, 2> player_event_health() const noexcept {
        return {player_events_.pending(), player_events_.dropped()};
    }

    // Apply worker receipts on the producer thread, then attach the immutable event to a fresh scene.
    [[nodiscard]] bool publish_player_event(Snapshot snapshot) {
        const auto receipt = player_event_receipt_.exchange(0, std::memory_order_acq_rel);
        if (receipt != 0) (void)player_events_.acknowledge(receipt & core::RuntimeGeneration::maximum_wire_value,
                                                       (receipt >> 63) == 0, client_.player_reactions_ready());
        const auto* pending = player_events_.front();
        if (!pending || !snapshot || !client_.player_events_ready() || terminal_.load(std::memory_order_acquire) ||
            runtime_paused() || microphone_.recording() || !playback_.idle() ||
            active_responses_.load(std::memory_order_acquire) != 0 || active_actions_.load(std::memory_order_acquire) != 0 ||
            speech_jobs_.load(std::memory_order_acquire) != 0 || player_event_tasks_.load(std::memory_order_acquire) != 0) return false;
        const auto& observed = pending->after;
        if (snapshot->generation() != observed.generation || snapshot->variant() != observed.variant ||
            snapshot->player().form_id() != observed.form_id || snapshot->player().origin_plugin() != observed.origin_plugin ||
            snapshot->player().playthrough_id() != observed.playthrough_id) return false;
        const auto now = core::SnapshotClock::now();
        if (now < next_player_event_attempt_ || now < observed.observed_at) return false;
        next_player_event_attempt_ = now + std::chrono::seconds{5};
        const protocol_native::NativePlayerEvent event{*pending, static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - observed.observed_at).count())};
        player_event_owner_.emplace(current_turn_token());
        const auto accepted = network_.try_submit("network", tasks::TaskClass::background,
            now + std::chrono::milliseconds{request_timeout_ms()}, player_event_owner_->token(),
            tasks::TaskLanes::track_activity(player_event_tasks_,
            [this, snapshot=std::move(snapshot), event](const core::CancellationToken& cancellation) {
                try {
                    const auto context_turn = client_.begin_context_turn();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire) || runtime_paused() ||
                        core::SnapshotClock::now() - snapshot->captured_at() > std::chrono::seconds{2}) return;
                    const auto outcome = publish_snapshot(snapshot, std::nullopt, cancellation, nullptr, std::nullopt, event);
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    if (outcome.status == RequestStatus::complete) {
                        player_event_receipt_.store(event.transition.serial, std::memory_order_release);
                        player_event_failure_reported_.store(false, std::memory_order_release);
                    } else if (outcome.status == RequestStatus::http_failure && outcome.http_status >= 400 &&
                               outcome.http_status < 500 && outcome.http_status != 408 && outcome.http_status != 429) {
                        player_event_receipt_.store(event.transition.serial | (1ULL << 63), std::memory_order_release);
                        push_notification("SYNTH player event rejected; check logs");
                    } else if (!player_event_failure_reported_.exchange(true, std::memory_order_acq_rel))
                        push_notification("SYNTH player event not acknowledged; retaining evidence for retry");
                } catch (const std::exception&) {
                    if (!player_event_failure_reported_.exchange(true, std::memory_order_acq_rel))
                        push_notification("SYNTH player event delivery failed; retaining evidence for retry");
                }
            }));
        return accepted == tasks::SubmitResult::accepted;
    }

    [[nodiscard]] bool activate_target(Snapshot snapshot, bool manual) {
        if (!snapshot || !client_.ready() || runtime_paused_.load(std::memory_order_acquire)) {
            return false;
        }
        // Ambient activation must not fill the request queue while a reply or
        // speech is outstanding. Keep current-turn admission available to input.
        if (!manual && (microphone_.recording() || active_actions_.load(std::memory_order_acquire) != 0 ||
                        active_responses_.load(std::memory_order_acquire) != 0 ||
                        speech_jobs_.load(std::memory_order_acquire) != 0 || !playback_.idle())) {
            return false;
        }
        auto target = selected_target(*snapshot, !manual);
        if (!target) {
            if (!manual) {
                std::scoped_lock lock{activation_mutex_};
                last_automatic_actor_.clear();
            }
            push_notification("SYNTH could not find an eligible target to activate");
            return false;
        }
        const auto actor = identity(*target);
        if (!enrich_selected_actor(snapshot, actor)) return false;
        const auto deadline = tasks::TaskLanes::Clock::now() +
                              std::chrono::milliseconds{request_timeout_ms() * 2ULL};
        const auto accepted = network_.try_submit(
            "network", manual ? tasks::TaskClass::current_turn : tasks::TaskClass::background,
            deadline, manual ? begin_player_turn(actor_key(actor)) : current_turn_token(),
            tasks::TaskLanes::track_activity(active_responses_,
            [this, snapshot = std::move(snapshot), actor, manual](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;

                try {
                    const auto context_turn = client_.begin_context_turn();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    const auto context_outcome = publish_snapshot(snapshot, actor, cancellation);
                    if (context_outcome.status != RequestStatus::complete) {
                        push_notification("SYNTH activation context failed: " + context_outcome.detail);
                        return;
                    }
                    const auto outcome = client_.activate(actor, manual ? "manual" : "automatic", context_outcome.context_binding);
                    if (outcome.status == RequestStatus::complete) {
                        clear_rechat_suppression(actor);
                        {
                            std::scoped_lock lock{conversation_mutex_};
                            const auto key = actor_key(actor);
                            if (std::ranges::find(active_quest_actors_, key) == active_quest_actors_.end()) {
                                if (active_quest_actors_.size() == 128) active_quest_actors_.erase(active_quest_actors_.begin());
                                active_quest_actors_.push_back(key);
                            }
                        }
                        if (manual) {
                            push_notification("SYNTH activated " + actor.display_name);
                        } else if (snapshot->game_time_ticks() > 0) {
                            const auto key = actor_key(actor);
                            bool newly_observed{};
                            {
                                std::scoped_lock lock{activation_mutex_};
                                newly_observed = key != last_automatic_actor_;
                                last_automatic_actor_ = key;
                            }
                            if (newly_observed) {
                                const auto greeting = client_.trigger(
                                    "auto_greeting", actor,
                                    [this, snapshot, cancellation](const protocol_native::Line& line) {
                                        handle_line(line, snapshot, cancellation);
                                    },
                                    snapshot->game_time_ticks(), std::nullopt, context_outcome.context_binding);
                                if (greeting.status != RequestStatus::complete) {
                                    push_notification("SYNTH auto greeting stopped: " + greeting.detail);
                                }
                            }
                        }
                    } else if (manual) {
                        push_notification("SYNTH activation failed: " + outcome.detail);
                    }
                } catch (const std::exception& error) {
                    if (manual) push_notification(std::string{"SYNTH activation: "} + error.what());
                }
            }));
        if (accepted != tasks::SubmitResult::accepted) {
            if (manual) push_notification("SYNTH network queue is full; activation deferred");
            return false;
        }
        claim_external_work(actor_key(actor));
        return true;
    }

    // Combat cancels one response owner, not the session or its observation/activation state.
    void interrupt_dialogue_for_combat() {
        cancel_voice_capture();
        std::optional<std::string> actor;
        {
            std::scoped_lock lock{conversation_mutex_};
            actor = external_owner_.owner();
        }
        // An interrupted unfinished microphone turn must not retain an unresolved target lock.
        if (actor && actor->empty()) actor.reset();
        (void)begin_player_turn(actor.value_or(""));
        {
            std::scoped_lock lock{conversation_mutex_};
            external_owner_.begin(std::move(actor));
        }
        mark_activity();
        push_notification("SYNTH combat started; active dialogue cancelled");
    }

    void hard_halt() noexcept {
        terminal_.store(true, std::memory_order_release);
        halting_.store(true, std::memory_order_release);
        microphone_.cancel();
        transport_.cancel_all();
        playback_.halt();
        dispatcher_.discard_pending();
        (void)network_.discard_pending();
        (void)speech_.discard_pending();
        {
            std::scoped_lock lock{action_mutex_};
            pending_actions_.clear();
        }
        {
            std::scoped_lock lock{rechat_mutex_};
            pending_rechat_.reset();
            rechat_capture_.reset();
            if (prefetched_rechat_) prefetched_rechat_->prefetch->cancel();
            prefetched_rechat_.reset();
        }
        {
            std::scoped_lock lock{conversation_mutex_};
            conversation_cooldowns_.clear();
            suppressed_rechat_actors_.clear();
        }
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

    struct ControlState final {
        protocol_native::ControlSelection selection{"STANDARD", 1};
        std::string status;
        bool pending{};
        std::uint64_t revision{};
        tasks::TaskLanes::Clock::time_point deadline{};
    };

    [[nodiscard]] ControlState control_state() const {
        std::scoped_lock lock{control_mutex_};
        if (control_state_.pending && tasks::TaskLanes::Clock::now() >= control_state_.deadline) {
            control_state_.pending = false;
            control_state_.status = "Settings request timed out; reopen the menu to refresh";
            ++control_state_.revision;
        }
        return control_state_;
    }

    // Capture menu ownership once; a later selection never performs a fresh nearest-NPC fallback.
    [[nodiscard]] std::optional<integration::PromptTarget> prepare_control_target(const Snapshot& snapshot) {
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
        return integration::PromptTarget{cancellation_.token(), best->form_id(), best->base_form_id(),
            best->origin_plugin(), best->base_origin_plugin(), best->playthrough_id(), best->name()};
    }

    // A bounded worker RPC owns settings acknowledgements; render/game threads never wait on HTTP.
    [[nodiscard]] bool request_control(std::string setting = "status", std::string value = "") {
        if (!client_.ready() || terminal_.load(std::memory_order_acquire)) return false;
        std::uint64_t revision{};
        {
            std::scoped_lock lock{control_mutex_};
            if (control_state_.pending) return false;
            control_state_.pending = true;
            control_state_.status = "Waiting for server confirmation...";
            ++control_state_.revision;
            revision = control_state_.revision;
            control_state_.deadline = tasks::TaskLanes::Clock::now() + std::chrono::seconds{15};
        }
        const auto accepted = network_.try_submit("network", tasks::TaskClass::background,
            tasks::TaskLanes::Clock::now() + std::chrono::seconds{15}, cancellation_.token(),
            [this, setting = std::move(setting), value = std::move(value), revision](const core::CancellationToken& cancellation) {
                ControlState updated = control_state();
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
                std::scoped_lock lock{control_mutex_};
                if (control_state_.revision != revision) return;
                updated.pending = false;
                updated.revision = control_state_.revision + 1;
                control_state_ = std::move(updated);
            });
        if (accepted != tasks::SubmitResult::accepted) {
            std::scoped_lock lock{control_mutex_};
            control_state_.pending = false;
            control_state_.status = "Settings queue is busy; try again";
            ++control_state_.revision;
            return false;
        }
        return true;
    }

    [[nodiscard]] std::optional<audio::PlaybackFrame> playback_frame() const noexcept {
        return playback_.active_frame();
    }

    [[nodiscard]] presentation::DialogueCaptions::Caption subtitle_frame(
        std::uint64_t generation, const std::optional<audio::PlaybackFrame>& playback,
        bool hidden, core::SnapshotClock::time_point now) {
        return captions_.frame(generation, playback ? playback->caption : nullptr,
                               hidden || composing_.load(std::memory_order_acquire) || terminal_.load(std::memory_order_acquire), now);
    }

    void set_composing(bool composing) {
        composing_.store(composing, std::memory_order_release);
        playback_.set_paused(composing || runtime_paused());
    }

    // Dialogue-only interruption leaves the connection and runtime session alive.
    void stop_dialogue() {
        if (terminal_.load(std::memory_order_acquire)) return;
        cancel_voice_capture();
        (void)begin_player_turn();
        mark_activity();
    }

    void set_runtime_paused(bool paused) {
        const auto previous = runtime_paused_.exchange(paused, std::memory_order_acq_rel);
        playback_.set_paused(paused || composing_.load(std::memory_order_acquire));
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
            microphone_.recording() || runtime_paused_.load(std::memory_order_acquire) ||
            terminal_.load(std::memory_order_acquire) || !client_.ready() || !playback_.idle() ||
            speech_jobs_.load(std::memory_order_acquire) != 0 || active_responses_.load(std::memory_order_acquire) != 0 ||
            active_actions_.load(std::memory_order_acquire) != 0) return false;
        const auto journal = context::quest_observation(*snapshot);
        std::uint64_t previous_sequence{};
        {
            std::unique_lock lock{world_context_mutex_, std::try_to_lock};
            if (!lock.owns_lock() || !journal || !acknowledged_quest_context_ ||
                acknowledged_quest_player_ != actor_key(identity(snapshot->player())) ||
                !context::tracked_quest_changed(*acknowledged_quest_context_, *journal)) return false;
            previous_sequence = acknowledged_quest_sequence_;
        }
        const auto selected = select_observation_reactor(*snapshot, now);
        if (!selected) return false;
        return queue_context_trigger("quest_updated", identity(*selected), std::move(snapshot),
            "quest reaction stopped", std::nullopt, previous_sequence);
    }

    // Only acknowledged, recent transitions can request one comment; lack of a speaker never blocks history delivery.
    [[nodiscard]] bool maybe_trigger_player_reaction(Snapshot snapshot, core::SnapshotClock::time_point now) {
        const auto* event = player_events_.reaction(now);
        if (!event || !snapshot || !client_.player_reactions_ready() || snapshot->variant() != core::RuntimeVariant::flat ||
            snapshot->player().in_combat() ||
            microphone_.recording() || runtime_paused() || terminal_.load(std::memory_order_acquire) ||
            !client_.ready() || !playback_.idle() || speech_jobs_.load(std::memory_order_acquire) != 0 ||
            active_responses_.load(std::memory_order_acquire) != 0 || active_actions_.load(std::memory_order_acquire) != 0 ||
            player_event_tasks_.load(std::memory_order_acquire) != 0) return false;
        const auto& observed = event->after;
        if (snapshot->generation() != observed.generation || snapshot->player().form_id() != observed.form_id ||
            snapshot->player().origin_plugin() != observed.origin_plugin || snapshot->player().playthrough_id() != observed.playthrough_id)
            return false;
        const auto selected = select_observation_reactor(*snapshot, now);
        if (!selected) return false;
        const auto serial = event->serial;
        if (!queue_context_trigger("player_reaction", identity(*selected), std::move(snapshot),
            "player reaction stopped", std::nullopt, std::nullopt, serial, observed.observed_at + std::chrono::seconds{60})) return false;
        (void)player_events_.consume_reaction(serial);
        return true;
    }

private:
    // Shared current-owner, crosshair, then nearest selection; never auto-activate an actor to manufacture a reaction.
    [[nodiscard]] std::optional<context::TargetContext> select_observation_reactor(
        const core::RuntimeSnapshot& snapshot, core::SnapshotClock::time_point now) {
        std::optional<context::TargetContext> selected;
        {
            std::unique_lock lock{conversation_mutex_, std::try_to_lock};
            if (!lock.owns_lock()) return std::nullopt;
            const context::PlaythroughIdentity playthrough{snapshot.player().playthrough_id()};
            const auto eligible = [&](const core::ActorSnapshot& actor) {
                const auto key = actor_key(identity(actor));
                const auto cooldown = conversation_cooldowns_.find(key);
                return std::ranges::find(active_quest_actors_, key) != active_quest_actors_.end() &&
                    (cooldown == conversation_cooldowns_.end() || now >= cooldown->second) &&
                    targeting::dialogue_actor_available(actor) && actor.life_state() == "alive" &&
                    (actor.posture() == "normal" || actor.posture() == "sitting") &&
                    actor.line_of_sight() == true && !actor.in_combat() && !actor.hostile_to_player() &&
                    !actor.talking_to_player() && targeting::distance(snapshot.player_pose().position(), actor.position()) <=
                        configuration_.activation.automatic_hearing_distance;
            };
            if (external_owner_.owner() && !external_owner_.owner()->empty()) {
                selected = targeting::select_nearest(snapshot, playthrough, snapshot.player_pose().position(),
                    configuration_.activation.automatic_hearing_distance, [&](const auto& actor) {
                        return actor_key(identity(actor)) == *external_owner_.owner() && eligible(actor);
                    }).target;
            }
            if (!selected) selected = targeting::select_flat_crosshair(snapshot, playthrough, {}, eligible).target;
            if (!selected) selected = targeting::select_nearest(snapshot, playthrough, snapshot.player_pose().position(),
                configuration_.activation.automatic_hearing_distance, eligible).target;
        }
        return selected;
    }

public:
    [[nodiscard]] bool maybe_trigger_bored(Snapshot snapshot, core::SnapshotClock::time_point now) {
        if (!snapshot || microphone_.recording() || runtime_paused_.load(std::memory_order_acquire) ||
            terminal_.load(std::memory_order_acquire) || !client_.ready() ||
            !playback_.idle() || speech_jobs_.load(std::memory_order_acquire) != 0 ||
            active_responses_.load(std::memory_order_acquire) != 0 ||
            active_actions_.load(std::memory_order_acquire) != 0) {
            return false;
        }
        const auto target = selected_target(*snapshot, true);
        if (!target || !claim_bored(now)) return false;
        if (!queue_context_trigger("bored", identity(*target), std::move(snapshot),
                                   "bored event stopped")) {
            mark_activity(now - std::chrono::seconds{configuration_.behavior.bored_event_seconds});
            return false;
        }
        return true;
    }

    [[nodiscard]] bool maybe_trigger_combat_bark(Snapshot snapshot,
                                                 core::SnapshotClock::time_point now) {
        if (!snapshot || microphone_.recording() || runtime_paused_.load(std::memory_order_acquire) ||
            !configuration_.behavior.combat_barks_enabled ||
            terminal_.load(std::memory_order_acquire) || !client_.ready() || !playback_.idle() ||
            speech_jobs_.load(std::memory_order_acquire) != 0 ||
            active_responses_.load(std::memory_order_acquire) != 0 ||
            active_actions_.load(std::memory_order_acquire) != 0) {
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
        if (!selected.target || !claim_combat_bark(now)) return false;
        if (!queue_context_trigger("combat_bark", identity(*selected.target), std::move(snapshot),
                                   "combat bark stopped")) {
            release_combat_bark_claim();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool halt_complete() const {
        if (!halting_.load(std::memory_order_acquire) ||
            halt_requests_.load(std::memory_order_acquire) != 0 ||
            world_context_tasks_.load(std::memory_order_acquire) != 0 ||
            player_event_tasks_.load(std::memory_order_acquire) != 0 ||
            speech_jobs_.load(std::memory_order_acquire) != 0 ||
            active_responses_.load(std::memory_order_acquire) != 0 ||
            active_actions_.load(std::memory_order_acquire) != 0) {
            return false;
        }
        const auto health = network_.health();
        return std::ranges::none_of(health.lanes, [](const auto& lane) {
            return lane.pending_background != 0 || lane.pending_current_turn != 0;
        });
    }

    [[nodiscard]] bool halting() const noexcept {
        return halting_.load(std::memory_order_acquire);
    }

    void pump(std::uint64_t generation) {
        if (terminal_.load(std::memory_order_acquire)) {
            return;
        }
        captions_.enqueue_failed(failed_playback_caption_);
        if (!failed_playback_caption_ && !playback_.pump(generation, &failed_playback_caption_)) {
            push_notification("SYNTH voice playback failed; dialogue remains available as text");
        }
        captions_.enqueue_failed(failed_playback_caption_);
        {
            std::scoped_lock lock{rechat_mutex_};
            if (prefetched_rechat_) {
                auto& deferred = *prefetched_rechat_;
                const auto now = tasks::TaskLanes::Clock::now();
                if (deferred.prefetch->invalid(now)) prefetched_rechat_.reset();
                else if (runtime_paused() || microphone_.recording()) return;
                else if (auto lines = deferred.prefetch->take(now)) {
                    deferred.prefetched_lines = std::move(*lines);
                    rechat_capture_ = std::move(deferred);
                    prefetched_rechat_.reset();
                }
                return;
            }
        }
        if (runtime_paused_.load(std::memory_order_acquire) || microphone_.recording()) return;
        // Only the completed response's final currently playing line may authorize one speculative child.
        if (client_.rechat_scene_ready() && active_responses_.load(std::memory_order_acquire) == 0 &&
            active_actions_.load(std::memory_order_acquire) == 0 && speech_jobs_.load(std::memory_order_acquire) == 0 &&
            captions_.idle()) {
            const auto playing = playback_.active_frame();
            std::scoped_lock lock{action_mutex_, rechat_mutex_};
            if (pending_actions_.empty() && !rechat_capture_ && pending_rechat_ && playing &&
                playing->caption == pending_rechat_->caption && pending_rechat_->response_complete &&
                !pending_rechat_->binding.cancellation.is_cancelled() &&
                pending_rechat_->caption->delivery->load(std::memory_order_acquire) == presentation::DialogueDelivery::playing) {
                pending_rechat_->prefetch = std::make_shared<RechatPrefetch>(pending_rechat_->caption,
                    tasks::TaskLanes::Clock::now() + std::chrono::milliseconds{request_timeout_ms() * 2ULL});
                rechat_capture_ = std::move(pending_rechat_);
                pending_rechat_.reset();
                return;
            }
        }
        if (!playback_.idle() || speech_jobs_.load(std::memory_order_acquire) != 0 ||
            active_responses_.load(std::memory_order_acquire) != 0 ||
            active_actions_.load(std::memory_order_acquire) != 0 ||
            terminal_.load(std::memory_order_acquire)) {
            return;
        }
        std::optional<PendingAction> pending;
        {
            std::scoped_lock lock{action_mutex_};
            if (!pending_actions_.empty()) {
                pending.emplace(std::move(pending_actions_.front()));
                pending_actions_.pop_front();
            }
        }
        if (pending) {
            execute_action(std::move(*pending));
            return;
        }
        if (!captions_.idle()) return;
        std::optional<PendingRechat> rechat;
        {
            std::scoped_lock lock{rechat_mutex_};
            if (pending_rechat_ && (pending_rechat_->binding.cancellation.is_cancelled() ||
                !pending_rechat_->snapshot || pending_rechat_->snapshot->generation().value() != generation ||
                pending_rechat_->caption->delivery->load(std::memory_order_acquire) == presentation::DialogueDelivery::discarded)) {
                pending_rechat_.reset();
                rechat_capture_.reset();
            }
            if (pending_rechat_ && pending_rechat_->response_complete && pending_rechat_->caption->delivered()) {
                rechat = std::move(pending_rechat_);
                pending_rechat_.reset();
                rechat_capture_.reset();
            }
        }
        if (rechat) {
            if (client_.rechat_scene_ready()) {
                std::scoped_lock lock{rechat_mutex_};
                rechat_capture_=std::move(rechat);
            } else if (!client_.uses_rechat_scene_contract()) queue_rechat(std::move(*rechat));
        } else {
            pump_diary_status();
            pump_automatic_diary();
        }
    }

    // Return copied presentation ownership; native capture must occur outside the session receiver.
    [[nodiscard]] presentation::DialogueCaptions::Caption presentation_capture_candidate(core::SnapshotClock::time_point now) {
        if (terminal_.load(std::memory_order_acquire) || runtime_paused() || composing_.load(std::memory_order_acquire)) return {};
        auto candidate = playback_.validation_candidate(now);
        return candidate ? candidate : captions_.validation_candidate(now);
    }

    void submit_presentation_capture(const presentation::DialogueCaptions::Caption& caption, const Snapshot& snapshot) {
        if (!caption || !caption->admission || caption->cancellation.is_cancelled() ||
            terminal_.load(std::memory_order_acquire) || caption->generation != cancellation_.token().generation().value()) return;
        const auto& admission = *caption->admission;
        const auto now = core::SnapshotClock::now();
        if (!snapshot || !admission.matches(*snapshot,now)) {
            caption->admission->rejected.store(true,std::memory_order_release);
            caption->mark_delivery(presentation::DialogueDelivery::discarded);
            stop_dialogue();
            return;
        }
        caption->admission->accept(snapshot->captured_at());
    }

    // The host captures outside this object: a game-thread capture can retire the whole session.
    [[nodiscard]] std::optional<integration::PromptTarget> rechat_capture_target() {
        std::scoped_lock lock{rechat_mutex_};
        if (rechat_capture_ && rechat_capture_->binding.cancellation.is_cancelled()) {
            if (rechat_capture_->prefetch) rechat_capture_->prefetch->cancel();
            rechat_capture_.reset();
        }
        if (runtime_paused() || microphone_.recording()) return {};
        return rechat_capture_ ? std::optional{rechat_capture_->target} : std::nullopt;
    }

    void submit_rechat_capture(Snapshot snapshot, const integration::PromptTarget& target) {
        std::optional<PendingRechat> rechat;
        {
            std::scoped_lock lock{rechat_mutex_};
            rechat=std::move(rechat_capture_);
            rechat_capture_.reset();
        }
        if (rechat && runtime_paused() && !rechat->binding.cancellation.is_cancelled() &&
            !terminal_.load(std::memory_order_acquire)) {
            std::scoped_lock lock{rechat_mutex_};
            rechat_capture_=std::move(rechat);
            return;
        }
        if (!rechat || !snapshot || target.session.is_cancelled() ||
            rechat->binding.cancellation.is_cancelled() || runtime_paused() ||
            terminal_.load(std::memory_order_acquire)) {
            if (rechat && rechat->prefetch) rechat->prefetch->cancel();
            return;
        }
        const auto now=core::SnapshotClock::now();
        const auto& previous=*rechat->snapshot;
        if (snapshot->generation()!=previous.generation() || snapshot->variant()!=previous.variant() ||
            now<snapshot->captured_at() || snapshot->captured_at()<previous.captured_at() ||
            now-snapshot->captured_at()>std::chrono::seconds{2} ||
            !snapshot->world() || !previous.world() || !snapshot->world()->scene || !previous.world()->scene ||
            !snapshot->world()->scene->valid(snapshot->world()->interior) ||
            snapshot->world()->interior!=previous.world()->interior ||
            snapshot->world()->scene!=previous.world()->scene ||
            actor_key(identity(snapshot->player()))!=actor_key(identity(previous.player())) ||
            target.form_id!=rechat->target.form_id || !selected_target(*snapshot,false,&rechat->target)) {
            trace_->record(diagnostics::ConversationStage::continuation_scene_rejected,rechat->binding.generation,
                rechat->binding.sequence,rechat->parent.request_id,rechat->parent.line_id);
            if (rechat->prefetch) rechat->prefetch->cancel();
            return;
        }
        if (rechat->prefetched_lines) {
            // Revalidate every copied speaker/action actor against the release capture, without retargeting.
            const auto available = [&](const protocol_native::Identity& actor) {
                const auto key = actor_key(actor);
                if (key == actor_key(identity(previous.player()))) return true;
                const auto found = std::ranges::find_if(previous.actors(), [&](const auto& candidate) {
                    return actor_key(identity(candidate)) == key;
                });
                if (found == previous.actors().end()) return false;
                const integration::PromptTarget retained{rechat->binding.cancellation,found->form_id(),found->base_form_id(),
                    found->origin_plugin(),found->base_origin_plugin(),found->playthrough_id(),found->name(),true};
                return selected_target(*snapshot,false,&retained).has_value();
            };
            for (const auto& line : *rechat->prefetched_lines) {
                if (const auto* speech = std::get_if<protocol_native::Dialogue>(&line.payload)) {
                    if (const auto* actor = std::get_if<protocol_native::Identity>(&speech->speaker); actor && !available(*actor)) {
                        rechat->prefetch->cancel();
                        return;
                    }
                }
                if (const auto* action = std::get_if<protocol_native::Action>(&line.payload)) {
                    if (!available(action->actor) || (action->target && !available(*action->target))) {
                        rechat->prefetch->cancel();
                        return;
                    }
                }
            }
            if (rechat->prefetch->invalid(now)) return;
            const auto token = rechat->prefetch->token();
            const auto accepted = network_.try_submit("network", tasks::TaskClass::current_turn,
                now + std::chrono::milliseconds{request_timeout_ms()}, token,
                tasks::TaskLanes::track_activity(active_responses_,
                    [this, snapshot = std::move(snapshot), lines = std::move(*rechat->prefetched_lines), prefetch = rechat->prefetch](const core::CancellationToken& cancellation) {
                        if (core::SnapshotClock::now()-snapshot->captured_at()>std::chrono::seconds{2}) { prefetch->cancel(); return; }
                        if (!prefetch->release(core::SnapshotClock::now())) return;
                        for (const auto& line : lines) {
                            if (cancellation.is_cancelled()) return;
                            handle_line(line, snapshot, cancellation, true, prefetch);
                        }
                    }));
            if (accepted != tasks::SubmitResult::accepted) rechat->prefetch->cancel();
            return;
        }
        rechat->snapshot=std::move(snapshot);
        rechat->fresh_scene=true;
        queue_rechat(std::move(*rechat));
    }

    [[nodiscard]] bool begin_voice_capture(const Snapshot& snapshot) {
        if (!snapshot || snapshot->generation() != cancellation_.token().generation() ||
            terminal_.load(std::memory_order_acquire) ||
            runtime_paused_.load(std::memory_order_acquire) || microphone_.recording()) {
            return false;
        }
        const auto started = microphone_.start();
        if (started) {
            voice_turn_ = begin_player_turn();
            bind_voice_target(snapshot);
            mark_activity();
        }
        push_notification(started ? "SYNTH voice capture started"
                                  : "SYNTH microphone is unavailable");
        return started;
    }

    void cancel_voice_capture() {
        microphone_.cancel();
        open_mic_voice_detected_ = false;
        voice_target_.reset();
        if (!voice_turn_.is_cancelled()) {
            std::scoped_lock lock{conversation_mutex_};
            external_owner_.end("");
        }
    }

    // Called once on the game thread at speech admission, never from the microphone worker.
    void bind_voice_target(const Snapshot& snapshot) {
        voice_target_.reset();
        voice_controls_ = control_state().selection;
        if (!snapshot || snapshot->generation() != cancellation_.token().generation() ||
            voice_turn_.is_cancelled() || !microphone_.recording()) return;
        voice_target_ = prepare_chat_target(snapshot);
    }

    [[nodiscard]] std::uint32_t voice_target_form_id() const noexcept {
        return voice_target_ ? voice_target_->form_id : 0;
    }

    [[nodiscard]] bool end_voice_capture(Snapshot snapshot) {
        auto wave = microphone_.stop();
        const auto retained = std::exchange(voice_target_, std::nullopt);
        if (!voice_turn_.is_cancelled()) {
            std::scoped_lock lock{conversation_mutex_};
            external_owner_.end(""); // Empty/failed recording must not leave an unresolved owner behind.
        }
        if (!wave || !snapshot || snapshot->generation() != cancellation_.token().generation() ||
            voice_turn_.is_cancelled() || runtime_paused_.load(std::memory_order_acquire) ||
            terminal_.load(std::memory_order_acquire)) {
            push_notification("SYNTH voice capture was empty");
            return false;
        }
        const auto controls = voice_controls_;
        const auto selected = retained ? conversation_target(*snapshot, controls, &*retained) : std::nullopt;
        const auto untargeted = controls.mode == "NARRATOR" || controls.mode == "INJECTION_LOG";
        if (!selected && !untargeted) {
            push_notification("SYNTH voice input needs an eligible target");
            return false;
        }
        const auto target = untargeted ? identity(snapshot->player()) : identity(*selected);
        if (!untargeted && !enrich_selected_actor(snapshot, target)) return false;
        const auto deadline = tasks::TaskLanes::Clock::now() +
                              std::chrono::milliseconds{request_timeout_ms() * 2ULL};
        const auto accepted = network_.try_submit(
            "network", tasks::TaskClass::current_turn, deadline, voice_turn_,
            tasks::TaskLanes::track_activity(active_responses_,
            [this, wave = std::move(*wave), snapshot = std::move(snapshot), target, controls](
                const core::CancellationToken& cancellation) mutable {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;

                try {
                    const auto sha256 = media_fetch::sha256_hex(wave);
                    protocol_native::Media media{"media/" + sha256, "audio/wav", sha256,
                                                 static_cast<std::uint64_t>(wave.size())};
                    std::string body(reinterpret_cast<const char*>(wave.data()), wave.size());
                    const auto upload = client_.upload_media(media, std::move(body), [this, cancellation] {
                        return cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire);
                    });
                    if (!upload.delivered() || upload.response.status < 200 ||
                        upload.response.status >= 300) {
                        if (cancellation.is_cancelled()) return;
                        push_notification("SYNTH microphone upload failed");
                        return;
                    }
                    const auto context_turn = client_.begin_context_turn();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    const auto context_outcome = publish_snapshot(snapshot,
                        controls.mode == "NARRATOR" || controls.mode == "INJECTION_LOG" ? std::nullopt : std::optional{target},
                        cancellation, nullptr, {}, {}, {}, controls);
                    if (context_outcome.status != RequestStatus::complete) {
                        if (context_outcome.status != RequestStatus::cancelled)
                            push_notification("SYNTH voice context failed: " + context_outcome.detail);
                        return;
                    }
                    const auto outcome = client_.send_audio(
                        media, [this, snapshot, cancellation](const protocol_native::Line& line) {
                            handle_line(line, snapshot, cancellation);
                        }, context_outcome.context_binding, controls);
                    if (outcome.status != RequestStatus::complete && outcome.status != RequestStatus::cancelled) {
                        push_notification("SYNTH voice request failed: " + outcome.detail);
                    }
                } catch (const std::exception& error) {
                    push_notification(std::string{"SYNTH voice request: "} + error.what());
                }
            }));
        if (accepted != tasks::SubmitResult::accepted) {
            push_notification("SYNTH network queue is full; voice input discarded");
            return false;
        }
        {
            std::scoped_lock lock{conversation_mutex_};
            external_owner_.begin(actor_key(target));
            external_owner_.claim(actor_key(target));
        }
        mark_activity();
        return true;
    }

    // Retain only identity before opening the pausing UI, never the old world snapshot.
    [[nodiscard]] std::optional<integration::PromptTarget> prepare_chat_target(const Snapshot& snapshot) {
        if (!snapshot || snapshot->generation() != cancellation_.token().generation()) return {};
        const auto controls = control_state().selection;
        bool nearby{};
        const auto selected = conversation_target(*snapshot, controls, nullptr, &nearby);
        if (!selected) return {};
        for (const auto& actor : snapshot->actors()) {
            if (actor.form_id() == selected->identity().form().form_id()) {
                return integration::PromptTarget{cancellation_.token(), actor.form_id(), actor.base_form_id(),
                    actor.origin_plugin(), actor.base_origin_plugin(), actor.playthrough_id(), actor.name(), true,
                    controls.mode, controls.model_slot, nearby};
            }
        }
        return {};
    }

    [[nodiscard]] bool submit_text(Snapshot snapshot, std::string text,
                                  const integration::PromptTarget* retained = nullptr) {
        if (!snapshot || text.empty() || text.size() > 4096 ||
            terminal_.load(std::memory_order_acquire) || runtime_paused_.load(std::memory_order_acquire)) {
            return false;
        }
        if (!client_.ready()) {
            push_notification("SYNTH is still connecting; try your message again when connected");
            return false;
        }
        if (text == "/diary") return request_npc_diary(std::move(snapshot), retained);
        if (text == "/diary narrator") return request_diary(std::move(snapshot), protocol_native::DiaryRole::narrator);
        if (text == "/diary player") return request_diary(std::move(snapshot), protocol_native::DiaryRole::player);
        const auto controls = retained && !retained->conversation_mode.empty()
            ? protocol_native::ControlSelection{retained->conversation_mode, retained->conversation_model_slot}
            : control_state().selection;
        const auto selected = conversation_target(*snapshot, controls, retained);
        const auto untargeted = controls.mode == "NARRATOR" || controls.mode == "INJECTION_LOG";
        if (!selected && !untargeted) {
            push_notification("SYNTH text input needs an eligible target");
            return false;
        }
        const auto target = untargeted ? identity(snapshot->player()) : identity(*selected);
        return queue_text_for_actor(std::move(snapshot),std::move(text),target, controls);
    }

    // One-shot integrations still require a completely idle pipeline.
    [[nodiscard]] bool external_ready() {
        const auto idle=client_.ready() && !terminal_.load(std::memory_order_acquire) && !runtime_paused() &&
            !microphone_.recording() && active_actions_.load(std::memory_order_acquire)==0 &&
            active_responses_.load(std::memory_order_acquire)==0 && speech_jobs_.load(std::memory_order_acquire)==0 &&
            playback_.idle();
        if (!idle) return false;
        std::unique_lock actions{action_mutex_,std::try_to_lock};
        if (!actions.owns_lock() || !pending_actions_.empty()) return false;
        std::unique_lock rechat{rechat_mutex_,std::try_to_lock};
        return rechat.owns_lock() && !pending_rechat_ && !rechat_capture_ && !prefetched_rechat_;
    }

    // Queue admission does not grant execution: only exact-owner Ask may pass a busy pipeline later.
    [[nodiscard]] bool external_admission_ready() {
        if (!client_.ready() || terminal_.load(std::memory_order_acquire) || runtime_paused() || microphone_.recording()) return false;
        if (external_ready()) {
            std::unique_lock lock{conversation_mutex_,std::try_to_lock};
            if (!lock.owns_lock()) return false;
            external_owner_.idle();
        }
        return true;
    }

    // Called only by the safe native pump after bounded API admission and scene validation.
    [[nodiscard]] bool submit_external(Snapshot snapshot, const SynthExternalRequestV1& request) {
        if (request.kind == SYNTH_EXTERNAL_OPEN_PROMPT) return false; // Presentation must go through the runtime's prompt callback.
        if (!snapshot || !external_admission_ready() || snapshot->generation()!=cancellation_.token().generation()) return false;
        const auto idle = external_ready();
        if (!idle && request.kind != SYNTH_EXTERNAL_ASK) return false;
        const auto now=core::SnapshotClock::now();
        const auto* selected=integration::external_actor(*snapshot,request.actor_form_id,now,
            configuration_.activation.automatic_hearing_distance);
        if (!selected || conversation_on_cooldown(identity(*selected),now) ||
            (!configuration_.activation.include_hostile && selected->hostile_to_player()) ||
            (selected->in_combat() && !configuration_.behavior.enable_combat_dialogue)) return false;
        const auto target=identity(*selected);
        if (request.kind==SYNTH_EXTERNAL_ASK) {
            {
                std::unique_lock lock{conversation_mutex_,std::try_to_lock};
                if (!lock.owns_lock() || !external_owner_.allows_ask(actor_key(target),idle)) return false;
            }
            return queue_text_for_actor(std::move(snapshot),std::string{request.text},target);
        }
        const auto kind=request.kind==SYNTH_EXTERNAL_SPEAK_EXACT ? "external_tts" :
            request.kind==SYNTH_EXTERNAL_COMMENT ? "external_comment" : "external_reaction";
        const auto accepted=queue_context_trigger(kind,target,std::move(snapshot),"external request failed",
            request.kind==SYNTH_EXTERNAL_COMMENT ? std::nullopt : std::optional{std::string{request.text}});
        if (accepted) mark_activity();
        return accepted;
    }

    // Freeze only the prompt's intended identity; submission must capture a new scene, not reuse this snapshot.
    [[nodiscard]] std::optional<integration::PromptTarget> prepare_external_prompt(Snapshot snapshot, std::uint32_t form_id) {
        if (!snapshot || !external_admission_ready() || snapshot->generation()!=cancellation_.token().generation()) return {};
        const auto now=core::SnapshotClock::now();
        const auto* actor=integration::external_actor(*snapshot,form_id,now,configuration_.activation.automatic_hearing_distance);
        if (!actor || conversation_on_cooldown(identity(*actor),now) ||
            (!configuration_.activation.include_hostile && actor->hostile_to_player()) ||
            (actor->in_combat() && !configuration_.behavior.enable_combat_dialogue)) return {};
        const auto idle=external_ready();
        std::unique_lock lock{conversation_mutex_,std::try_to_lock};
        if (!lock.owns_lock() || !external_owner_.allows_ask(actor_key(identity(*actor)),idle)) return {};
        return integration::PromptTarget{cancellation_.token(),actor->form_id(),actor->base_form_id(),
            actor->origin_plugin(),actor->base_origin_plugin(),actor->playthrough_id(),actor->name()};
    }

    // A targeted draft cannot turn into ordinary crosshair input after load, replacement or session retirement.
    [[nodiscard]] bool submit_prompt(Snapshot snapshot, const integration::PromptTarget& target, std::string text) {
        if (target.session.is_cancelled() || target.session.generation()!=cancellation_.token().generation() ||
            text.empty() || text.size()>4096) return false;
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
                                          std::optional<protocol_native::ControlSelection> retained_controls = {}) {
        const auto controls = retained_controls.value_or(control_state().selection);
        if (controls.mode != "NARRATOR" && controls.mode != "INJECTION_LOG" && !enrich_selected_actor(snapshot, target)) return false;
        const auto deadline = tasks::TaskLanes::Clock::now() +
                              std::chrono::milliseconds{request_timeout_ms() * 2ULL};
        const auto accepted = network_.try_submit(
            "network", tasks::TaskClass::current_turn, deadline, begin_player_turn(actor_key(target)),
            tasks::TaskLanes::track_activity(active_responses_,
            [this, snapshot = std::move(snapshot), text = std::move(text), target, controls](
                const core::CancellationToken& cancellation) mutable {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;

                try {
                    const auto context_turn = client_.begin_context_turn();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    const auto context_outcome = publish_snapshot(snapshot,
                        controls.mode == "NARRATOR" || controls.mode == "INJECTION_LOG" ? std::nullopt : std::optional{target},
                        cancellation, nullptr, {}, {}, {}, controls);
                    if (context_outcome.status != RequestStatus::complete) {
                        if (context_outcome.status != RequestStatus::cancelled)
                            push_notification("SYNTH text context failed: " + context_outcome.detail);
                        return;
                    }
                    const auto outcome = client_.send_text(
                        std::move(text), [this, snapshot, cancellation](const protocol_native::Line& line) {
                            handle_line(line, snapshot, cancellation);
                        }, context_outcome.context_binding, controls);
                    if (outcome.status != RequestStatus::complete && outcome.status != RequestStatus::cancelled) {
                        push_notification("SYNTH text request failed: " + outcome.detail);
                    }
                } catch (const std::exception& error) {
                    push_notification(std::string{"SYNTH text request: "} + error.what());
                }
            }));
        if (accepted != tasks::SubmitResult::accepted) {
            push_notification("SYNTH network queue is full; text input discarded");
            return false;
        }
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

    enum class MicrophoneEvent { none, speech_started, speech_finished };

    // Only speech boundaries request a game-thread snapshot; idle/recording frames do no scene work.
    [[nodiscard]] MicrophoneEvent pump_open_microphone(core::SnapshotClock::time_point now) {
        if (!configuration_.input.open_microphone ||
            configuration_.audio.open_mic_muted || runtime_paused_.load(std::memory_order_acquire) ||
            composing_.load(std::memory_order_acquire) ||
            terminal_.load(std::memory_order_acquire)) {
            return MicrophoneEvent::none;
        }
        if (!microphone_.recording()) {
            if (microphone_.start(30)) {
                open_mic_started_at_ = now;
                open_mic_voice_detected_ = false;
                voice_target_.reset();
            }
            return MicrophoneEvent::none;
        }
        if (microphone_.recent_peak() >= configuration_.audio.open_mic_sensitivity) {
            if (!open_mic_voice_detected_) {
                voice_turn_ = begin_player_turn();
                voice_target_.reset();
                push_notification("SYNTH open microphone detected speech");
                mark_activity(now);
                open_mic_voice_detected_ = true;
                open_mic_last_voice_at_ = now;
                return MicrophoneEvent::speech_started;
            }
            mark_activity(now);
            open_mic_voice_detected_ = true;
            open_mic_last_voice_at_ = now;
            return MicrophoneEvent::none;
        }
        if (open_mic_voice_detected_) {
            const auto silence = std::chrono::duration<double>{now - open_mic_last_voice_at_}.count();
            if (silence >= configuration_.audio.open_mic_end_delay_seconds) {
                open_mic_voice_detected_ = false;
                return MicrophoneEvent::speech_finished;
            }
        } else if (open_mic_started_at_.time_since_epoch().count() != 0 &&
                   now - open_mic_started_at_ >= std::chrono::seconds{29}) {
            microphone_.cancel();
            open_mic_started_at_ = {};
        }
        return MicrophoneEvent::none;
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

    // Bind subject/context and freeze pixels on the pump; encode/hash/upload only on the worker.
    [[nodiscard]] bool submit_visual_capture(Snapshot snapshot, std::string interaction_mode) {
        if (!snapshot || snapshot->generation() != cancellation_.token().generation() || !client_.ready() ||
            terminal_.load(std::memory_order_acquire) ||
            runtime_paused_.load(std::memory_order_acquire) || halting()) {
            return false;
        }
        // Admission is game-thread-only; a worker only releases this lifetime counter.
        if (visual_tasks_.load(std::memory_order_acquire) != 0) {
            push_notification("SYNTH PipVision capture is still processing");
            return false;
        }
        std::optional<protocol_native::Identity> subject;
        if (interaction_mode == "npc_portrait") {
            if (auto target = pointed_subject(*snapshot)) subject = identity(*target);
            if (!subject) {
                push_notification("SYNTH PipVision: target an NPC first");
                return false;
            }
        } else if (interaction_mode == "describe") {
            if (auto target = nearest_visual_subject(*snapshot)) subject = identity(*target);
            if (!subject) {
                push_notification("SYNTH PipVision: no nearby NPC can describe the scene");
                return false;
            }
        } else if (interaction_mode != "store_only") {
            return false;
        }

        if (subject && !enrich_selected_actor(snapshot, *subject)) return false;
        std::vector<protocol_native::Identity> nearby;
        const auto hearing_distance = snapshot->world() && snapshot->world()->interior
            ? configuration_.activation.interior_hearing_distance
            : configuration_.activation.exterior_hearing_distance;
        nearby.reserve(std::min<std::size_t>(snapshot->actors().size(), 16));
        for (const auto& actor : snapshot->actors()) {
            if (nearby.size() == 16) break;
            if (((subject && actor_key(identity(actor)) == actor_key(*subject)) ||
                 targeting::distance(snapshot->player_pose().position(), actor.position()) <= hearing_distance) &&
                targeting::dialogue_actor_available(actor)) {
                nearby.push_back(identity(actor));
            }
        }
        std::optional<protocol_native::WorldState> world;
        if (snapshot->world()) {
            world = protocol_native::WorldState{
                snapshot->world()->location, snapshot->world()->cell, snapshot->world()->worldspace,
                snapshot->world()->weather, snapshot->world()->interior,
                snapshot->world()->game_time_ticks};
        }
        // Share one immutable buffer through std::function copies, not many 128 MB pixel copies.
        const auto pixels = std::make_shared<const media_capture::CapturedPixels>(
            media_capture::capture_game_window_pixels());
        const auto perspective = snapshot->variant() == core::RuntimeVariant::vr ? "hmd" : "first_person";
        const auto deadline = tasks::TaskLanes::Clock::now() +
                              std::chrono::milliseconds{request_timeout_ms() * 2ULL};
        const auto turn = interaction_mode == "describe" ? begin_player_turn(subject ? actor_key(*subject) : std::string{}) : current_turn_token();
        const auto accepted = network_.try_submit(
            "network", tasks::TaskClass::current_turn, deadline, turn,
            tasks::TaskLanes::track_activity(visual_tasks_,
            tasks::TaskLanes::track_activity(active_responses_,
            [this, snapshot = std::move(snapshot), interaction_mode = std::move(interaction_mode),
             pixels = pixels, world = std::move(world), deadline,
             subject, nearby = std::move(nearby),
             perspective = std::string{perspective}](const core::CancellationToken& cancellation) mutable {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;

                try {
                    const auto image = media_capture::encode_jpeg(*pixels);
                    pixels.reset();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire) ||
                        tasks::TaskLanes::Clock::now() >= deadline) return;
                    const auto sha256 = media_fetch::sha256_hex(image.bytes);
                    protocol_native::Media media{"media/" + sha256, image.content_type, sha256,
                                                 static_cast<std::uint64_t>(image.bytes.size())};
                    std::string body(reinterpret_cast<const char*>(image.bytes.data()), image.bytes.size());
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire) ||
                        tasks::TaskLanes::Clock::now() >= deadline) return;
                    const auto upload = client_.upload_media(media, std::move(body), [this, &cancellation] {
                        return cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire);
                    });
                    if (!upload.delivered() || upload.response.status < 200 || upload.response.status >= 300) {
                        push_notification("SYNTH PipVision image upload failed");
                        return;
                    }
                    const auto context_turn = client_.begin_context_turn();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    const auto context_outcome = publish_snapshot(snapshot, subject, cancellation);
                    if (context_outcome.status != RequestStatus::complete) {
                        push_notification("SYNTH PipVision capture context failed: " + context_outcome.detail);
                        return;
                    }
                    const auto outcome = client_.send_visual_capture(
                        interaction_mode, perspective, media, world, subject, nearby,
                        [this, &cancellation] {
                            return cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire);
                        }, context_outcome.context_binding);
                    if (outcome.status != RequestStatus::complete) {
                        push_notification("SYNTH PipVision failed: " + outcome.detail);
                        return;
                    }
                    if (interaction_mode == "describe" && subject) {
                        const auto reaction = client_.trigger(
                            "external_reaction", *subject,
                            [this, snapshot, cancellation](const protocol_native::Line& line) {
                                handle_line(line, snapshot, cancellation);
                            }, std::nullopt,
                            "Describe the bound PipVision capture naturally from your own point of view.",
                            context_outcome.context_binding, outcome.capture_id);
                        if (reaction.status != RequestStatus::complete) {
                            push_notification("SYNTH PipVision description dialogue failed");
                            return;
                        }
                        // The original snapshot predates the accepted visual result. Advance the
                        // co-save frontier after completion without inventing a new game observation.
                        const auto checkpoint = publish_snapshot(snapshot, subject, cancellation);
                        if (checkpoint.status != RequestStatus::complete) {
                            push_notification("SYNTH PipVision described; save checkpoint is not acknowledged");
                            return;
                        }
                    }
                    push_notification(interaction_mode == "npc_portrait"
                                          ? "SYNTH PipVision NPC portrait updated"
                                          : interaction_mode == "describe"
                                                ? "SYNTH PipVision scene described"
                                                : "SYNTH PipVision scene captured");
                } catch (const std::exception& error) {
                    push_notification(std::string{"SYNTH PipVision: "} + error.what());
                }
            })));
        if (accepted != tasks::SubmitResult::accepted) {
            push_notification("SYNTH PipVision queue is full");
            return false;
        }
        claim_external_work(subject ? std::optional{actor_key(*subject)} : std::nullopt);
        return true;
    }

    // Copy the pointed actor and snapshot on the game thread; admission runs only on the network lane.
    [[nodiscard]] bool request_npc_diary(Snapshot snapshot, const integration::PromptTarget* retained = nullptr) {
        return request_diary(std::move(snapshot), protocol_native::DiaryRole::npc, retained);
    }

    [[nodiscard]] bool automatic_diary_ready() const {
        return automatic_diary_enabled_ && client_.ready() && !terminal_.load(std::memory_order_acquire) && !halting();
    }

    // The safe pump supplies one real copied frame, not a fabricated event-start snapshot or a later retarget.
    [[nodiscard]] std::size_t queue_automatic_diaries(Snapshot snapshot, const core::RestEventBatch& starts) {
        if (!automatic_diary_ready() || !snapshot || starts.size > starts.events.size()) return 0;
        const auto now = core::SnapshotClock::now();
        std::scoped_lock lock{diary_mutex_};
        if (terminal_.load(std::memory_order_acquire) || cancellation_.token().is_cancelled()) return 0;
        if (automatic_diaries_.expire(now) != 0) push_notification("SYNTH expired queued automatic diaries before admission");
        std::size_t accepted{};
        for (std::size_t i = 0; i < starts.size; ++i)
            if (automatic_diaries_.push(snapshot, starts.events[i], now)) ++accepted;
        if (accepted != starts.size) push_notification("SYNTH automatic diary queue rejected stale or excess rest events");
        return accepted;
    }

    // Player-owned roles use the copied player, never a fabricated NPC or a game-thread provider call.
    [[nodiscard]] bool request_diary(Snapshot snapshot, protocol_native::DiaryRole role,
                                   const integration::PromptTarget* retained = nullptr) {
        if (!snapshot || !client_.ready() || terminal_.load(std::memory_order_acquire) ||
            runtime_paused_.load(std::memory_order_acquire)) return false;
        {
            std::scoped_lock lock{diary_mutex_};
            if (pending_diary_ || diary_tasks_.load(std::memory_order_acquire) != 0) {
                push_notification("SYNTH is still tracking the previous diary request");
                return false;
            }
        }
        const auto selected = retained ? selected_target(*snapshot, false, retained) : pointed_subject(*snapshot);
        const auto player_owned = role != protocol_native::DiaryRole::npc;
        if (!player_owned && !selected) { push_notification("SYNTH diary needs a pointed NPC"); return false; }
        const auto actor = player_owned ? identity(snapshot->player()) : identity(*selected);
        const auto deadline = tasks::TaskLanes::Clock::now() + std::chrono::milliseconds{request_timeout_ms() * 2ULL};
        const auto accepted = network_.try_submit("network", tasks::TaskClass::background, deadline, cancellation_.token(),
            tasks::TaskLanes::track_activity(diary_tasks_,
            [this, actor, role, player_owned, snapshot = std::move(snapshot)](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                try {
                    const auto context_turn = client_.begin_context_turn();
                    const auto published = publish_snapshot(snapshot, player_owned ? std::nullopt : std::optional{actor}, cancellation);
                    if (published.status != RequestStatus::complete) { push_notification("SYNTH diary context was not acknowledged"); return; }
                    auto admission = client_.request_diary(actor, published.context_binding, role);
                    const auto& outcome = admission.outcome();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    if (outcome.status != RequestStatus::complete) push_notification("SYNTH diary request failed; check logs");
                    else if (outcome.detail == "diary_queued") {
                        std::scoped_lock lock{diary_mutex_};
                        const auto now = tasks::TaskLanes::Clock::now();
                        pending_diary_ = PendingDiary{std::move(admission), now + std::chrono::minutes{30}, now + std::chrono::seconds{5}};
                        push_notification("SYNTH diary queued for background generation");
                    } else push_notification("SYNTH diary disabled: check the selected role's diary setting and connector");
                } catch (const std::exception&) { push_notification("SYNTH diary request failed; check logs"); }
            }));
        if (accepted != tasks::SubmitResult::accepted) push_notification("SYNTH diary queue is full");
        return accepted == tasks::SubmitResult::accepted;
    }

    // Native adapters capture only when a finished diary needs a new save frontier, never on the worker.
    [[nodiscard]] bool diary_checkpoint_needed() {
        if (terminal_.load(std::memory_order_acquire) || runtime_paused_.load(std::memory_order_acquire) ||
            diary_tasks_.load(std::memory_order_acquire) != 0 || active_responses_.load(std::memory_order_acquire) != 0 ||
            active_actions_.load(std::memory_order_acquire) != 0 || speech_jobs_.load(std::memory_order_acquire) != 0 ||
            microphone_.recording() || !playback_.idle()) return false;
        std::scoped_lock lock{diary_mutex_};
        return pending_diary_ && pending_diary_->ready && tasks::TaskLanes::Clock::now() >= pending_diary_->next_poll;
    }

    [[nodiscard]] bool checkpoint_diary(Snapshot snapshot) {
        if (!snapshot || snapshot->generation() != cancellation_.token().generation() || !diary_checkpoint_needed()) return false;
        const auto selected = selected_target(*snapshot, false);
        const auto target = selected ? std::optional{identity(*selected)} : std::nullopt;
        {
            std::scoped_lock lock{diary_mutex_};
            pending_diary_->next_poll = tasks::TaskLanes::Clock::now() + std::chrono::seconds{5};
        }
        const auto accepted = network_.try_submit("network", tasks::TaskClass::background,
            tasks::TaskLanes::Clock::now() + std::chrono::milliseconds{request_timeout_ms()}, cancellation_.token(),
            tasks::TaskLanes::track_activity(diary_tasks_,
            [this, snapshot = std::move(snapshot), target](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                bool saved{};
                try {
                    const auto context_turn = client_.begin_context_turn();
                    saved = publish_snapshot(snapshot, target, cancellation).save_checkpoint_acknowledged;
                } catch (const std::exception&) {}
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                std::scoped_lock lock{diary_mutex_};
                if (!pending_diary_) return;
                if (saved) {
                    const auto partial = pending_diary_->partial;
                    pending_diary_.reset();
                    push_notification(partial ? "SYNTH diary batch partly completed; successful entries included in your next save" :
                        "SYNTH diary complete; included in your next save");
                } else if (++pending_diary_->checkpoint_failures >= 3) {
                    const auto partial = pending_diary_->partial;
                    pending_diary_.reset();
                    push_notification(partial ? "SYNTH diary batch partly generated, but its save checkpoint was not acknowledged" :
                        "SYNTH diary generated, but its save checkpoint was not acknowledged");
                }
            }));
        return accepted == tasks::SubmitResult::accepted;
    }

    [[nodiscard]] bool refresh_dynamic_profiles(Snapshot snapshot, std::string_view scope = "automatic", std::uint32_t target_form_id = 0) {
        if (!snapshot || !client_.ready() || terminal_.load(std::memory_order_acquire) ||
            runtime_paused_.load(std::memory_order_acquire)) {
            return false;
        }
        std::vector<protocol_native::Identity> actors;
        actors.reserve(std::min<std::size_t>(snapshot->actors().size(), 16));
        for (const auto& actor : snapshot->actors()) {
            if (scope == "narrator" || (scope == "target" && actor.form_id() != target_form_id)) continue;
            if (actors.size() == 16) break;
            if (targeting::distance(snapshot->player_pose().position(), actor.position()) <=
                    configuration_.activation.automatic_hearing_distance &&
                targeting::dialogue_actor_available(actor)) {
                actors.push_back(identity(actor));
            }
        }
        const auto include_narrator = scope == "narrator" ||
            (scope == "automatic" && configuration_.behavior.dynamic_profile_include_narrator);
        if (actors.empty() && !include_narrator) return false;
        const auto selected = selected_target(*snapshot, false);
        const auto target = selected ? std::optional{identity(*selected)} : std::nullopt;
        const auto deadline = tasks::TaskLanes::Clock::now() +
                              std::chrono::milliseconds{request_timeout_ms() * 2ULL};
        const auto accepted = network_.try_submit(
            "network", tasks::TaskClass::background, deadline, cancellation_.token(),
            [this, actors = std::move(actors), include_narrator, snapshot = std::move(snapshot), target](
                const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                try {
                    const auto context_turn = client_.begin_context_turn();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    const auto context_outcome = publish_snapshot(snapshot, target, cancellation);
                    if (context_outcome.status != RequestStatus::complete) {
                        push_notification("SYNTH profile context failed: " + context_outcome.detail);
                        return;
                    }
                    const auto outcome = client_.refresh_dynamic_profiles(actors, include_narrator, context_outcome.context_binding);
                    if (outcome.status != RequestStatus::complete) {
                        push_notification("SYNTH dynamic profile refresh failed: " + outcome.detail);
                    }
                } catch (const std::exception& error) {
                    push_notification(std::string{"SYNTH dynamic profile refresh: "} + error.what());
                }
            });
        return accepted == tasks::SubmitResult::accepted;
    }

    [[nodiscard]] static config::Config load_configuration() {
        const auto root = std::filesystem::current_path();
        return config::load(root / "Data/F4SE/Plugins/SYNTH.ini",
                            root / "Data/F4SE/Plugins/SYNTH_custom.ini");
    }

private:
    struct PendingAction final {
        protocol_native::Action action;
        Snapshot snapshot;
        ContextBinding binding;
        tasks::TaskLanes::Deadline deadline;
    };

    // Atomically retain the emitted actor and parent with their copied world/cancellation owner.
    struct PendingRechat final {
        protocol_native::Identity actor;
        Snapshot snapshot;
        ContextBinding binding;
        protocol_native::RechatParent parent;
        presentation::DialogueCaptions::Caption caption;
        bool response_complete{};
        integration::PromptTarget target;
        bool fresh_scene{};
        unsigned scene_retries{};
        std::shared_ptr<RechatPrefetch> prefetch;
        std::optional<std::vector<protocol_native::Line>> prefetched_lines;
    };

    using RuntimeActionCompletion = runtime::ActionCompletion;

    [[nodiscard]] core::CancellationToken current_turn_token() {
        std::scoped_lock lock{turn_mutex_};
        return turn_cancellation_.token();
    }

    // Game-thread admission only: invalidate old chains without HTTP or worker joins.
    [[nodiscard]] core::CancellationToken begin_player_turn(std::string actor = {}) {
        trace_->record(diagnostics::ConversationStage::turn_admitted,cancellation_.token().generation().value());
        core::CancellationToken turn;
        {
            std::scoped_lock lock{turn_mutex_};
            turn_cancellation_.cancel();
            const core::CancellationSource next{cancellation_.token()};
            turn_cancellation_ = next;
            turn = turn_cancellation_.token();
        }
        (void)network_.discard_cancelled();
        (void)speech_.discard_cancelled();
        {
            std::scoped_lock lock{conversation_mutex_};
            external_owner_.begin(std::move(actor));
        }
        cancel_interrupted_turn();
        playback_.interrupt();
        {
            std::scoped_lock lock{action_mutex_};
            pending_actions_.clear();
        }
        {
            std::scoped_lock lock{rechat_mutex_};
            pending_rechat_.reset();
            rechat_capture_.reset();
            if (prefetched_rechat_) prefetched_rechat_->prefetch->cancel();
            prefetched_rechat_.reset();
        }
        return turn;
    }

    // The control lane can overtake dialogue HTTP without blocking its caller.
    void cancel_interrupted_turn() {
        if (const auto anchor = client_.take_interrupted_turn()) {
            const auto queued = control_.try_submit("control", tasks::TaskClass::current_turn,
                tasks::TaskLanes::Clock::now() + std::chrono::seconds{2}, cancellation_.token(),
                [this, anchor = *anchor](const core::CancellationToken&) {
                    trace_->record(diagnostics::ConversationStage::cancel_sent,anchor.generation,0,anchor.context_request_id);
                    try {
                        const auto outcome = client_.cancel(anchor.context_request_id, anchor.generation);
                        trace_->record(outcome.status == RequestStatus::complete ? diagnostics::ConversationStage::cancel_acknowledged :
                            diagnostics::ConversationStage::cancel_unconfirmed,anchor.generation,0,anchor.context_request_id);
                        if (outcome.status != RequestStatus::complete && outcome.status != RequestStatus::cancelled)
                            push_notification("SYNTH interrupted locally; server cancellation was not acknowledged");
                    } catch (const std::exception&) {
                        trace_->record(diagnostics::ConversationStage::cancel_unconfirmed,anchor.generation,0,anchor.context_request_id);
                        push_notification("SYNTH interrupted locally; server cancellation failed");
                    }
                });
            if (queued != tasks::SubmitResult::accepted)
                push_notification("SYNTH interrupted locally; server cancellation queue is full");
        }
    }

    // A late stream failure invalidates its owned work, never the replacement turn's audio.
    void invalidate_response(core::CancellationToken cancellation) {
        {
            std::scoped_lock lock{turn_mutex_};
            if (!turn_cancellation_.cancel_if_owner(cancellation)) return;
        }
        cancel_interrupted_turn();
    }

    void claim_external_work(std::optional<std::string> actor) {
        std::scoped_lock lock{conversation_mutex_};
        external_owner_.claim(std::move(actor));
    }

    [[nodiscard]] std::uint64_t request_timeout_ms() const noexcept {
        return static_cast<std::uint64_t>(configuration_.behavior.ai_response_timeout_seconds) * 1000ULL;
    }

    void mark_activity(core::SnapshotClock::time_point when = core::SnapshotClock::now()) {
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

    // Publishes the captured frame before issuing an unsolicited dialogue trigger.
    [[nodiscard]] bool queue_context_trigger(std::string kind, protocol_native::Identity actor,
                                             Snapshot snapshot, std::string failure_label,
                                             std::optional<std::string> external_text = std::nullopt,
                                             std::optional<std::uint64_t> previous_sequence = std::nullopt,
                                             std::optional<std::uint64_t> player_event_serial = std::nullopt,
                                             std::optional<core::SnapshotClock::time_point> event_deadline = std::nullopt) {
        if (!enrich_selected_actor(snapshot, actor)) return false;
        const auto owner = actor_key(actor);
        const auto deadline = tasks::TaskLanes::Clock::now() +
                              std::chrono::milliseconds{request_timeout_ms() * 2ULL};
        const auto accepted = network_.try_submit(
            "network", tasks::TaskClass::current_turn, deadline, current_turn_token(),
            tasks::TaskLanes::track_activity(active_responses_,
            [this, kind = std::move(kind), actor = std::move(actor),
             snapshot = std::move(snapshot), failure_label = std::move(failure_label), external_text = std::move(external_text), previous_sequence,
             player_event_serial, event_deadline](
                const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;

                try {
                    const auto context_turn = client_.begin_context_turn();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    if (event_deadline && (core::SnapshotClock::now() > *event_deadline || runtime_paused() ||
                        core::SnapshotClock::now() - snapshot->captured_at() > std::chrono::seconds{2})) return;
                    if (previous_sequence) {
                        std::scoped_lock lock{world_context_mutex_};
                        if (acknowledged_quest_sequence_ != *previous_sequence) return;
                    }
                    const auto context_outcome = publish_snapshot(snapshot, actor, cancellation);
                    if (context_outcome.status != RequestStatus::complete) return;
                    if (event_deadline && (core::SnapshotClock::now() > *event_deadline || cancellation.is_cancelled() || runtime_paused())) return;
                    const auto one_shot=kind=="external_tts" || kind=="external_comment" || kind=="external_reaction" || kind=="quest_updated" || kind=="player_reaction";
                    const auto outcome = client_.trigger(
                        kind, actor, [this, snapshot, cancellation, one_shot](const protocol_native::Line& line) {
                            handle_line(line, snapshot, cancellation, !one_shot);
                        }, std::nullopt, external_text, context_outcome.context_binding, std::nullopt, previous_sequence, player_event_serial);
                    if (outcome.status != RequestStatus::complete) {
                        push_notification("SYNTH " + failure_label + ": " + outcome.detail);
                    }
                } catch (const std::exception& error) {
                    push_notification("SYNTH " + failure_label + ": " + error.what());
                }
            }));
        if (accepted == tasks::SubmitResult::accepted) claim_external_work(owner);
        return accepted == tasks::SubmitResult::accepted;
    }

    [[nodiscard]] static std::string make_runtime_session_id(core::RuntimeVariant variant) {
        static std::atomic_uint64_t sequence{};
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::string{"runtime:"} +
               (variant == core::RuntimeVariant::vr ? "vr:" : "flat:") +
               std::to_string(GetCurrentProcessId()) + ":" + std::to_string(ticks) + ":" +
               std::to_string(sequence.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    [[nodiscard]] static std::vector<std::string> capabilities(core::RuntimeVariant variant, bool rest_events_available,
                                                             bool quest_events_available, bool actor_events_available) {
        std::vector<std::string> result{
            "dialogue.text", "dialogue.audio", "dialogue.turn_ownership", "dialogue.turn_cancel", "dialogue.rechat.ownership", "dialogue.rechat.scene", "dialogue.listener_identity", "control.menu",
            "runtime.retirement", "context.saved_anchor", "media.binary", "context.actor_identity",
            "media.image.upload", "visual_context.capture", "visual_context.describe",
            "visual_context.portrait",
            "profiles.dynamic.refresh", "diary.manual.npc", "diary.manual.narrator", "diary.manual.player", "diary.batch.status",
            "context.actor_state", "context.actor_factions", "context.world",
            "context.loaded_plugins", "context.quests",
            "context.nearby_items", "context.points_of_interest", "agents.manage",
            "action.inspect_actor", "action.inspect_surroundings",
            "action.check_inventory", "action.read_quests", "action.end_conversation",
            "action.sheathe_weapon"};
        if (variant == core::RuntimeVariant::flat && quest_events_available) result.push_back("context.quest_events");
        if (variant == core::RuntimeVariant::flat && actor_events_available) result.push_back("context.actor_events");
        if (variant == core::RuntimeVariant::flat) {
            result.push_back("action.equip_item");
            result.push_back("action.unequip_item");
            result.push_back("action.inventory_observation");
            result.push_back("action.consume");
            result.push_back("action.consume_inventory");
            result.push_back("action.give_item_to");
            result.push_back("action.transfer_inventory");
            result.push_back("action.give_caps_to");
            result.push_back("action.caps_inventory");
            result.push_back("action.take_caps_from_player");
            result.push_back("action.player_caps_inventory");
            result.push_back("context.inventory_512");
            result.push_back("context.player_events");
            result.push_back("dialogue.player_reactions");
            result.push_back("context.quest_tracking");
            result.push_back("dialogue.quest_reactions");
        }
        if (variant == core::RuntimeVariant::vr) {
            result.push_back("runtime.hmd_pose");
            result.push_back("runtime.controller_pose");
        } else if (rest_events_available) {
            result.push_back("diary.automatic.sleep");
            result.push_back("diary.automatic.wait");
        }
        return result;
    }

    [[nodiscard]] bool submit(Snapshot snapshot, bool initialize_first) {
        const auto selected = selected_target(*snapshot, false);
        const auto target = selected ? std::optional{identity(*selected)} : std::nullopt;
        const auto deadline = tasks::TaskLanes::Clock::now() +
                              std::chrono::milliseconds{request_timeout_ms() * 2ULL};
        const auto result = network_.try_submit(
            "network", tasks::TaskClass::current_turn, deadline, cancellation_.token(),
            tasks::TaskLanes::track_activity(active_responses_,
            [this, snapshot = std::move(snapshot), initialize_first, target](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled()) {
                    return;
                }

                try {
                    if (initialize_first) {
                        const auto hmd_fresh = snapshot->hmd_pose_capability(core::SnapshotClock::now()) ==
                                               core::PoseCapability::fresh;
                        std::vector<protocol_native::LoadedPlugin> plugins;
                        plugins.reserve(snapshot->loaded_plugins().size());
                        for (const auto& plugin : snapshot->loaded_plugins()) {
                            plugins.push_back(protocol_native::LoadedPlugin{
                                plugin.name, plugin.form_id_prefix, plugin.light,
                                plugin.compile_index, plugin.small_file_compile_index,
                                plugin.partial_index});
                        }
                        const auto outcome = client_.initialize(
                            snapshot->generation().value(), hmd_fresh, plugins);
                        if (outcome.status != RequestStatus::complete) {
                            push_notification("SYNTH server initialization failed: " + outcome.detail);
                            initialization_failed_.store(true, std::memory_order_release);
                            return;
                        }
                        quest_event_ready_.store(quest_events_enabled_ && client_.quest_events_ready(), std::memory_order_release);
                        push_notification("SYNTH connected");
                        std::vector<std::string> voice_plugins;
                        for (const auto& plugin : snapshot->loaded_plugins()) voice_plugins.push_back(plugin.name);
                        voice_samples_.configure(std::move(voice_plugins));
                        return;
                    }
                    const auto outcome = publish_snapshot(snapshot, target, cancellation);
                    if (outcome.status == RequestStatus::complete) {
                        push_notification("SYNTH context updated");
                    } else {
                        push_notification("SYNTH context update failed: " + outcome.detail);
                    }
                } catch (const std::exception& error) {
                    push_notification(std::string{"SYNTH session: "} + error.what());
                    if (initialize_first) {
                        initialization_failed_.store(true, std::memory_order_release);
                    }
                }
            }));
        if (result != tasks::SubmitResult::accepted) {
            push_notification("SYNTH network queue is full");
            return false;
        }
        return true;
    }

    // This gate is also rechecked on the worker, under the serialized context transaction.
    [[nodiscard]] bool world_context_needed(const core::RuntimeSnapshot& snapshot, bool inventory_refresh = false) {
        if (inventory_refresh && snapshot.player().inventory_observation() == "unavailable") return false;
        const auto observed = context::world_observation(snapshot);
        const auto quests = context::quest_observation(snapshot);
        if (!observed && !quests && !inventory_refresh) return false;
        std::unique_lock lock{world_context_mutex_, std::try_to_lock};
        if (!lock.owns_lock()) return false;
        if (snapshot.generation().value() == acknowledged_world_generation_ &&
            snapshot.frame() < acknowledged_world_frame_) return false;
        return inventory_refresh || (observed && observed != acknowledged_world_context_) ||
               (quests && quests != acknowledged_quest_context_);
    }

    [[nodiscard]] RequestOutcome publish_snapshot(const Snapshot& owned_snapshot,
                                                   const std::optional<protocol_native::Identity>& target, core::CancellationToken cancellation,
                                                   std::vector<protocol_native::Identity>* published_audience = nullptr,
                                                   const std::optional<protocol_native::NativeQuestBatch>& quest_events = std::nullopt,
                                                   const std::optional<protocol_native::NativePlayerEvent>& player_event = std::nullopt,
                                                   const std::shared_ptr<const ActorEventDelivery::Prepared>& actor_events = {},
                                                   const protocol_native::ControlSelection& controls = {}) {
        const auto& snapshot = *owned_snapshot;
        if (actor_events && (actor_events->capture->scene!=owned_snapshot || target || quest_events || player_event ||
            actor_events->actors.empty() || actor_events->actors.size()>ActorEventDelivery::actor_capacity))
            throw std::invalid_argument{"actor event context does not own this fragment"};

        std::vector<protocol_native::Identity> audience;
        std::vector<protocol_native::ActorState> actor_states;
        audience.reserve(std::min<std::size_t>(snapshot.actors().size(), 16));
        actor_states.reserve(std::min<std::size_t>(snapshot.actors().size() + 1, 16));
        auto hearing_distance = snapshot.world() && snapshot.world()->interior
            ? configuration_.activation.interior_hearing_distance
            : configuration_.activation.exterior_hearing_distance;
        hearing_distance = static_cast<std::uint32_t>(targeting::conversation_range(hearing_distance, controls.mode));
        const auto selected_actor_key = target ? actor_key(*target) : std::string{};
        const auto make_state = [&](const core::ActorSnapshot& actor) {
            if (!selected_actor_key.empty() && actor_key(identity(actor)) == selected_actor_key)
                voice_samples_.request(actor.voice_type(), actor.name());
            std::vector<protocol_native::InventoryItem> inventory;
            inventory.reserve(actor.inventory().size());
            for (const auto& item : actor.inventory()) {
                char form_id[11]{};
                std::snprintf(form_id, sizeof(form_id), "0x%08X", item.form_id);
                inventory.push_back(protocol_native::InventoryItem{
                    form_id, item.origin_plugin, item.display_name, item.count, item.form_type,
                    item.value, item.weight, item.equipped});
            }
            std::vector<protocol_native::FactionMembership> factions;
            factions.reserve(actor.factions().size());
            for (const auto& faction : actor.factions()) {
                char form_id[11]{};
                std::snprintf(form_id, sizeof(form_id), "0x%08X", faction.form_id);
                factions.push_back(protocol_native::FactionMembership{
                    form_id, faction.origin_plugin, faction.display_name, faction.editor_id,
                    faction.rank});
            }
            return protocol_native::ActorState{
                .actor = identity(actor),
                .base_form_id = [&] {
                    char form_id[11]{};
                    std::snprintf(form_id, sizeof(form_id), "0x%08X", actor.base_form_id());
                    return std::string{form_id};
                }(),
                .base_origin_plugin = actor.base_origin_plugin(),
                .x = actor.position().x,
                .y = actor.position().y,
                .z = actor.position().z,
                .distance = targeting::distance(snapshot.player_pose().position(), actor.position()),
                .alive = actor.alive(),
                .disabled = actor.disabled(),
                .in_combat = actor.in_combat(),
                .hostile_to_player = actor.hostile_to_player(),
                .sneaking = actor.sneaking(),
                .level = static_cast<std::uint64_t>(std::max<std::int16_t>(0, actor.level())),
                .race = actor.race(),
                .sex = actor.sex(),
                .voice_type = actor.voice_type(),
                .teammate = actor.teammate(),
                .health_percent = actor.health_percent(),
                .action_points_percent = actor.action_points_percent(),
                .inventory = std::move(inventory),
                .line_of_sight = actor.line_of_sight()
                                     ? (*actor.line_of_sight() ? "visible" : "blocked")
                                     : "unknown",
                .factions = std::move(factions),
                .faction_observation = actor.faction_observation(),
                .life_state = actor.life_state(),
                .posture = actor.posture(),
                .weapon_drawn = actor.weapon_drawn(),
                .movement_speed = actor.movement_speed(),
                .sprinting = actor.sprinting(),
                .talking_to_player = actor.talking_to_player(),
                .in_power_armor = actor.in_power_armor(),
                .current_package = actor.current_package()
                    ? protocol_native::PackageState{
                          true,
                          [&] {
                              char form_id[11]{};
                              std::snprintf(form_id, sizeof(form_id), "0x%08X",
                                            actor.current_package()->form_id);
                              return std::string{form_id};
                          }(),
                          actor.current_package()->origin_plugin,
                          actor.current_package()->display_name,
                          actor.current_package()->editor_id}
                    : protocol_native::PackageState{},
                .inventory_observation = actor.inventory_observation(),
                .faction_completeness = actor.faction_completeness(),
                .health_percent_available = actor.health_percent_available(),
                .action_points_percent_available = actor.action_points_percent_available(),
            };
        };
        actor_states.push_back(make_state(snapshot.player()));
        const std::function<bool(const core::ActorSnapshot&)> selected = target
            ? std::function<bool(const core::ActorSnapshot&)>{[&](const auto& actor) {
                  return actor_key(identity(actor)) == selected_actor_key;
              }} : std::function<bool(const core::ActorSnapshot&)>{};
        if (actor_events) {
            // This audience carries exact source identities, not a new hearing/visibility or witness claim.
            for (const auto* resolved:actor_events->resolve_actor_states()) {
                if (resolved==&snapshot.player()) continue;
                audience.push_back(identity(*resolved));actor_states.push_back(make_state(*resolved));
            }
        } else {
            for (const auto* actor : targeting::context_participants(snapshot, hearing_distance, selected)) {
                if (controls.mode == "NARRATOR" || controls.mode == "INJECTION_LOG") continue;
                if (!controls.mode.empty() && !selected_actor_key.empty() && actor_key(identity(*actor)) != selected_actor_key &&
                    (!actor->conversation_area_available() || !targeting::nearby_conversation_audible(
                        hearing_distance, configuration_.activation.automatic_hearing_distance,
                        targeting::distance(snapshot.player_pose().position(), actor->position()), actor->line_of_sight()))) continue;
                if ((controls.mode == "WHISPER" || controls.mode == "CLOSE") &&
                    (actor_key(identity(*actor)) != selected_actor_key ||
                     targeting::distance(snapshot.player_pose().position(), actor->position()) > 200)) continue;
                audience.push_back(identity(*actor));
                if (actor_states.size() < 16) actor_states.push_back(make_state(*actor));
            }
        }
        std::vector<protocol_native::QuestState> active_quests;
        active_quests.reserve(snapshot.active_quests().size());
        for (const auto& quest : snapshot.active_quests()) {
            char form_id[11]{};
            std::snprintf(form_id, sizeof(form_id), "0x%08X", quest.form_id);
            active_quests.push_back(protocol_native::QuestState{
                form_id, quest.origin_plugin, quest.display_name, quest.editor_id,
                quest.current_stage, quest.active_objectives, quest.objectives, quest.objectives_observation, quest.tracked});
        }
        const auto form_id = [](std::uint32_t value) {
            char encoded[11]{};
            std::snprintf(encoded, sizeof(encoded), "0x%08X", value);
            return std::string{encoded};
        };
        std::vector<protocol_native::NearbyItemState> nearby_items;
        nearby_items.reserve(snapshot.nearby_items().size());
        for (const auto& item : snapshot.nearby_items()) {
            nearby_items.push_back(protocol_native::NearbyItemState{
                form_id(item.reference_id), form_id(item.base_form_id), form_id(item.cell_form_id),
                item.reference_origin_plugin, item.base_origin_plugin, item.cell_origin_plugin,
                item.display_name, item.position.x, item.position.y, item.position.z,
                item.distance, item.weight, item.count, item.form_type, item.value,
                item.stealing, item.looking_at, item.held});
        }
        std::vector<protocol_native::PointOfInterestState> points_of_interest;
        points_of_interest.reserve(snapshot.points_of_interest().size());
        for (const auto& poi : snapshot.points_of_interest()) {
            points_of_interest.push_back(protocol_native::PointOfInterestState{
                form_id(poi.reference_id), form_id(poi.base_form_id), form_id(poi.cell_form_id),
                poi.reference_origin_plugin, poi.base_origin_plugin, poi.cell_origin_plugin,
                poi.display_name, poi.kind, poi.position.x, poi.position.y, poi.position.z,
                poi.distance, poi.locked, poi.looking_at});
        }
        auto outcome = client_.publish_context(0, snapshot.player().playthrough_id(),
                                       identity(snapshot.player()),
                                       target,
                                       audience, actor_states,
                                       snapshot.world()
                                           ? std::optional{protocol_native::WorldState{
                                                 snapshot.world()->location, snapshot.world()->cell,
                                                 snapshot.world()->worldspace, snapshot.world()->weather,
                                                 snapshot.world()->interior,
                                                 snapshot.world()->game_time_ticks, snapshot.world()->scene}}
                                           : std::nullopt,
                                       active_quests, nearby_items, points_of_interest,
                                       {}, cancellation, save_binding_.loaded_context, snapshot.active_quests_observation(),
                                       snapshot.nearby_items_observation(), snapshot.points_of_interest_observation(),
                                       protocol_native::retained_collection_observation(
                                           snapshot.actors_observation(), snapshot.actors().size(), audience.size()), quest_events, player_event,
                                       actor_events ? std::optional{actor_events->packet(core::SnapshotClock::now())} : std::nullopt);
        if (outcome.status == RequestStatus::complete && !cancellation.is_cancelled() &&
            !terminal_.load(std::memory_order_acquire)) {
            if (quest_events_enabled_) quest_events_.acknowledge_journal(owned_snapshot, outcome.context_binding.sequence);
            std::scoped_lock lock{world_context_mutex_};
            if (snapshot.generation().value() != acknowledged_world_generation_ ||
                snapshot.frame() >= acknowledged_world_frame_) {
                acknowledged_world_context_ = context::world_observation(snapshot);
                if (auto quests = context::quest_observation(snapshot)) {
                    acknowledged_quest_context_ = std::move(quests);
                    acknowledged_quest_sequence_ = outcome.context_binding.sequence;
                    acknowledged_quest_player_ = actor_key(identity(snapshot.player()));
                }
                acknowledged_world_frame_ = snapshot.frame();
                acknowledged_world_generation_ = snapshot.generation().value();
                world_context_failure_reported_.store(false, std::memory_order_release);
            }
        }
        if (save_context_ && outcome.status == RequestStatus::complete && !cancellation.is_cancelled()
            && !terminal_.load(std::memory_order_acquire)) {
            outcome.save_checkpoint_acknowledged = save_context_->acknowledge(save_binding_.epoch,
                {snapshot.player().playthrough_id(), client_.runtime_session_id(),
                 outcome.context_binding.generation, outcome.context_binding.sequence});
        }
        // Acknowledgment belongs to copied values and the saved frontier, never merely to queue admission.
        if (outcome.status == RequestStatus::complete && !cancellation.is_cancelled() &&
            !terminal_.load(std::memory_order_acquire) &&
            snapshot.generation() == cancellation_.token().generation() &&
            snapshot.player().inventory_observation() != "unavailable" &&
            (!save_context_ || outcome.save_checkpoint_acknowledged)) {
            auto previous = acknowledged_inventory_frame_.load(std::memory_order_acquire);
            while (snapshot.frame() > previous && !acknowledged_inventory_frame_.compare_exchange_weak(
                previous, snapshot.frame(), std::memory_order_release, std::memory_order_acquire)) {}
        }
        if (published_audience && outcome.status == RequestStatus::complete) *published_audience = std::move(audience);
        outcome.context_binding.invalidate_response = [this, cancellation] {
            invalidate_response(cancellation);
        };
        return outcome;
    }

    // Player conversation only: commands retain their existing pointed/explicit selection semantics.
    [[nodiscard]] std::optional<context::TargetContext> conversation_target(
        const core::RuntimeSnapshot& snapshot, const protocol_native::ControlSelection& controls,
        const integration::PromptTarget* retained = nullptr, bool* nearby = nullptr) {
        if (snapshot.generation() != cancellation_.token().generation()) return {};
        const context::PlaythroughIdentity playthrough{snapshot.player().playthrough_id()};
        const auto now = core::SnapshotClock::now();
        auto origin = snapshot.player_pose().position();
        if (snapshot.variant() == core::RuntimeVariant::vr) {
            if (!snapshot.hmd_pose() || !snapshot.hmd_pose()->fresh_at(now) ||
                snapshot.hmd_pose()->frame() != snapshot.frame()) return {};
            origin = snapshot.hmd_pose()->pose().position();
        }
        const auto range = targeting::conversation_range(snapshot.world() && snapshot.world()->interior
            ? configuration_.activation.interior_hearing_distance
            : configuration_.activation.exterior_hearing_distance, controls.mode);
        const auto direct_eligible = [&](const core::ActorSnapshot& actor) {
            return actor.conversation_area_available() && targeting::dialogue_actor_available(actor) &&
                !(configuration_.behavior.scene_safety && actor.talking_to_player()) &&
                !conversation_on_cooldown(identity(actor), now);
        };
        const auto nearby_eligible = [&](const core::ActorSnapshot& actor) {
            return direct_eligible(actor) &&
                (configuration_.activation.include_hostile || !actor.hostile_to_player()) &&
                (configuration_.behavior.enable_combat_dialogue || !actor.in_combat()) &&
                targeting::automatic_actor_class_supported(actor.race(), configuration_.activation.include_creatures) &&
                targeting::nearby_conversation_audible(range, configuration_.activation.automatic_hearing_distance,
                    targeting::distance(origin, actor.position()), actor.line_of_sight());
        };
        if (retained) {
            if (retained->session.is_cancelled() || retained->session.generation() != snapshot.generation()) return {};
            for (const auto& actor : snapshot.actors()) {
                if (!retained->matches(actor) || !direct_eligible(actor) ||
                    (retained->nearby_conversation && !nearby_eligible(actor))) continue;
                const auto distance = targeting::distance(origin, actor.position());
                if (distance > range) return {};
                return context::TargetContext{context::identity_of(actor), actor.name(), actor.position(),
                    distance, snapshot.generation(), snapshot.frame()};
            }
            return {}; // A draft/recording never silently switches to another nearby actor.
        }
        const targeting::SelectionBounds bounds{range, 75.0};
        auto selected = targeting::select_conversation(snapshot, playthrough, now, bounds,
            direct_eligible, nearby_eligible, nearby);
        return std::move(selected.target);
    }

    [[nodiscard]] std::optional<context::TargetContext> selected_target(
        const core::RuntimeSnapshot& snapshot, bool automatic, const integration::PromptTarget* retained = nullptr) {
        const context::PlaythroughIdentity playthrough{snapshot.player().playthrough_id()};
        const auto now = core::SnapshotClock::now();
        const auto eligible = [&](const core::ActorSnapshot& actor) {
            if (!targeting::dialogue_actor_available(actor) ||
                (configuration_.behavior.scene_safety && actor.talking_to_player()) ||
                (automatic && !configuration_.activation.include_hostile &&
                 actor.hostile_to_player())) {
                return false;
            }
            if (conversation_on_cooldown(identity(actor), now)) return false;
            if (!automatic || configuration_.activation.include_creatures) return true;
            return targeting::automatic_actor_class_supported(actor.race(), false);
        };
        if (retained) {
            if (retained->session.is_cancelled() || retained->session.generation() != snapshot.generation() ||
                snapshot.generation() != cancellation_.token().generation()) return {};
            for (const auto& actor : snapshot.actors()) {
                if (!retained->matches(actor) || !eligible(actor)) continue;
                const auto distance = targeting::distance(snapshot.player_pose().position(), actor.position());
                if (distance > targeting::SelectionBounds{}.maximum_distance) return {};
                return context::TargetContext{context::identity_of(actor), actor.name(), actor.position(),
                    distance, snapshot.generation(), snapshot.frame()};
            }
            return {}; // Never silently retarget a draft after pause or actor replacement.
        }
        if (snapshot.variant() == core::RuntimeVariant::flat) {
            auto selected = targeting::select_flat_crosshair(snapshot, playthrough, {}, eligible);
            if (selected.target || !automatic) return std::move(selected.target);
        } else {
            auto selected = targeting::select_vr_controller_ray(
                snapshot, playthrough, targeting::Hand::right, now, {}, eligible);
            if (!selected.target) {
                selected = targeting::select_vr_hmd_gaze(snapshot, playthrough, now, {}, eligible);
            }
            if (selected.target) return std::move(selected.target);
        }
        auto selected = targeting::select_nearest(
            snapshot, playthrough, snapshot.player_pose().position(),
            automatic ? configuration_.activation.automatic_hearing_distance
                      : (snapshot.world() && snapshot.world()->interior
                             ? configuration_.activation.interior_distance
                             : configuration_.activation.exterior_distance),
            eligible);
        return std::move(selected.target);
    }

    [[nodiscard]] std::optional<context::TargetContext> pointed_subject(
        const core::RuntimeSnapshot& snapshot) {
        const context::PlaythroughIdentity playthrough{snapshot.player().playthrough_id()};
        const auto eligible = [](const core::ActorSnapshot& actor) {
            return targeting::dialogue_actor_available(actor);
        };
        if (snapshot.variant() == core::RuntimeVariant::flat) {
            return targeting::select_flat_crosshair(snapshot, playthrough, {}, eligible).target;
        }
        const auto now = core::SnapshotClock::now();
        auto selected = targeting::select_vr_controller_ray(
            snapshot, playthrough, targeting::Hand::right, now, {}, eligible);
        if (!selected.target) {
            selected = targeting::select_vr_hmd_gaze(snapshot, playthrough, now, {}, eligible);
        }
        return std::move(selected.target);
    }

    [[nodiscard]] std::optional<context::TargetContext> nearest_visual_subject(
        const core::RuntimeSnapshot& snapshot) {
        const context::PlaythroughIdentity playthrough{snapshot.player().playthrough_id()};
        const auto eligible = [](const core::ActorSnapshot& actor) {
            return targeting::dialogue_actor_available(actor);
        };
        return targeting::select_nearest(
                   snapshot, playthrough, snapshot.player_pose().position(),
                   configuration_.activation.automatic_hearing_distance, eligible)
            .target;
    }

    void handle_line(const protocol_native::Line& line, const Snapshot& snapshot,
                     const core::CancellationToken& cancellation, bool allow_followup = true,
                     const std::shared_ptr<RechatPrefetch>& prefetch = {}) {
        if (cancellation.is_cancelled() || !snapshot || line.generation != snapshot->generation().value() || line.context_sequence == 0
            || terminal_.load(std::memory_order_acquire)) return;
        trace_->record(diagnostics::ConversationStage::line_applied,line.generation,line.context_sequence,line.request_id,line.line_id);
        if (const auto* end = std::get_if<protocol_native::End>(&line.payload); end && allow_followup) {
            std::scoped_lock lock{rechat_mutex_};
            if (pending_rechat_ && pending_rechat_->parent.request_id == line.request_id)
                pending_rechat_->response_complete = end->status == "complete";
            return;
        }
        if (const auto* dialogue = std::get_if<protocol_native::Dialogue>(&line.payload)) {
            mark_activity();
            const auto speaker = std::visit([](const auto& value) { return value.display_name; },
                                           dialogue->speaker);
            // Preserve VR's existing presentation until its separate HUD ABI is verified.
            if (snapshot->variant() == core::RuntimeVariant::vr && configuration_.behavior.show_subtitles)
                push_notification((speaker.empty() ? "SYNTH" : speaker) + ": " + dialogue->text);
            const auto caption = make_caption(line, *dialogue, cancellation, snapshot);
            if (!dialogue->speech || dialogue->speech->status == "failed")
                captions_.enqueue(caption);
            if (dialogue->speech) {
                queue_speech(line, *dialogue, snapshot, cancellation, caption, prefetch);
            }
            if (!allow_followup) return;
            if (const auto* actor = std::get_if<protocol_native::Identity>(&dialogue->speaker)) {
                if (actor_key(*actor) == actor_key(identity(snapshot->player()))) return;
                // A later ineligible NPC line must not leave an earlier line as the continuation parent.
                {
                    std::scoped_lock lock{rechat_mutex_};
                    pending_rechat_.reset();
                    rechat_capture_.reset();
                }
                const auto current_actor = std::ranges::find_if(
                    snapshot->actors(), [&](const core::ActorSnapshot& candidate) {
                        return actor_key(identity(candidate)) == actor_key(*actor);
                    });
                if (current_actor != snapshot->actors().end() &&
                    targeting::dialogue_actor_available(*current_actor) &&
                    !consume_rechat_suppression(*actor) &&
                    !conversation_on_cooldown(*actor, core::SnapshotClock::now())) {
                    std::scoped_lock lock{rechat_mutex_};
                    pending_rechat_ = PendingRechat{*actor, snapshot,
                        response_binding(line, cancellation),
                        {line.request_id, line.line_id}, caption};
                    pending_rechat_->target={cancellation,current_actor->form_id(),current_actor->base_form_id(),
                        current_actor->origin_plugin(),current_actor->base_origin_plugin(),current_actor->playthrough_id(),
                        current_actor->name(),true};
                }
            }
            return;
        }
        const auto* action = std::get_if<protocol_native::Action>(&line.payload);
        if (action == nullptr || !allow_followup) return;
        if (line.generation != snapshot->generation().value()) {
            return;
        }
        const auto action_deadline = runtime::action_start_deadline(action->deadline_ms, request_timeout_ms());
        {
            std::scoped_lock lock{action_mutex_};
            if (pending_actions_.size() >= 32) {
                push_notification("SYNTH action queue is full");
                return;
            }
            pending_actions_.push_back(PendingAction{*action, snapshot,
                response_binding(line, cancellation),
                action_deadline});
        }
    }

    // Continuations retain the same rollback owner without reading the latest selected actor.
    [[nodiscard]] ContextBinding response_binding(const protocol_native::Line& line,
                                                   core::CancellationToken cancellation) {
        return {line.generation, line.context_sequence, cancellation, line.turn_id, {}, [this, cancellation] {
            invalidate_response(cancellation);
        }};
    }

    // Retry only before a child RPC exists. Pause holds ownership; stale captures get two fresh attempts.
    void defer_rechat_capture(PendingRechat rechat) {
        std::scoped_lock lock{rechat_mutex_};
        if (rechat.binding.cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire) ||
            rechat_capture_ || (!runtime_paused() && ++rechat.scene_retries > 2)) {
            if (rechat.prefetch) rechat.prefetch->cancel();
            trace_->record(diagnostics::ConversationStage::continuation_scene_rejected,rechat.binding.generation,
                rechat.binding.sequence,rechat.parent.request_id,rechat.parent.line_id);
            return;
        }
        prefetched_rechat_.reset();
        rechat_capture_=std::move(rechat);
    }

    void queue_rechat(PendingRechat rechat) {
        const auto prefetch = rechat.prefetch;
        if (rechat.prefetch) {
            rechat.binding.cancellation = rechat.prefetch->token();
            std::scoped_lock lock{rechat_mutex_};
            prefetched_rechat_ = rechat;
        }
        const auto binding = rechat.binding;
        const auto deadline = tasks::TaskLanes::Clock::now() +
                              std::chrono::milliseconds{request_timeout_ms() * 2ULL};
        const auto accepted = network_.try_submit(
            "network", tasks::TaskClass::current_turn, deadline, binding.cancellation,
            tasks::TaskLanes::track_activity(active_responses_,
            [this, rechat = std::move(rechat)](
                const core::CancellationToken& cancellation) mutable {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;

                try {
                    if (rechat.fresh_scene) {
                        if (!client_.rechat_scene_ready()) {
                            if (rechat.prefetch) rechat.prefetch->cancel();
                            return;
                        }
                        if (runtime_paused() || core::SnapshotClock::now()-rechat.snapshot->captured_at()>std::chrono::seconds{2}) {
                            defer_rechat_capture(std::move(rechat));
                            return;
                        }
                        const auto publication=publish_snapshot(rechat.snapshot,rechat.actor,cancellation);
                        if (publication.status==RequestStatus::complete && !cancellation.is_cancelled() &&
                            (runtime_paused() || core::SnapshotClock::now()-rechat.snapshot->captured_at()>std::chrono::seconds{2}) &&
                            !terminal_.load(std::memory_order_acquire)) {
                            // No child request exists yet. Recapture on resume, without spending a second round.
                            defer_rechat_capture(std::move(rechat));
                            return;
                        }
                        if (publication.status!=RequestStatus::complete || cancellation.is_cancelled() ||
                            runtime_paused() || core::SnapshotClock::now()-rechat.snapshot->captured_at()>std::chrono::seconds{2}) {
                            if (rechat.prefetch) rechat.prefetch->cancel();
                            return;
                        }
                        rechat.parent.scene_context_sequence=publication.context_binding.sequence;
                    }
                    trace_->record(diagnostics::ConversationStage::continuation_dispatch,rechat.binding.generation,
                        rechat.binding.sequence,rechat.parent.request_id,rechat.parent.line_id);
                    const auto outcome = client_.trigger(
                        "rechat", rechat.actor, [this, snapshot = rechat.snapshot, cancellation, prefetch = rechat.prefetch](const protocol_native::Line& line) {
                            if (prefetch) {
                                trace_->record(diagnostics::ConversationStage::prefetch_received,line.generation,line.context_sequence,line.request_id,line.line_id);
                                prefetch->append(line);
                                if (prefetch->claim_media(line)) prepare_rechat_media(line, prefetch);
                            }
                            else handle_line(line, snapshot, cancellation);
                            if (const auto* status = std::get_if<protocol_native::Status>(&line.payload);
                                status && status->code == "rechat_complete") {
                                auto stage = diagnostics::ConversationStage::continuation_complete;
                                if (status->detail == "no_candidate") stage = diagnostics::ConversationStage::continuation_no_candidate;
                                else if (status->detail == "private") stage = diagnostics::ConversationStage::continuation_private;
                                else if (status->detail == "budget_exhausted") stage = diagnostics::ConversationStage::continuation_budget_exhausted;
                                else if (status->detail == "ineligible") stage = diagnostics::ConversationStage::continuation_ineligible;
                                trace_->record(stage,line.generation,line.context_sequence,line.request_id,line.line_id);
                            }
                        }, std::nullopt, std::nullopt, rechat.binding, std::nullopt, std::nullopt, std::nullopt,
                        client_.uses_rechat_parent_contract() ? std::optional{rechat.parent} : std::nullopt);
                    if (rechat.prefetch) rechat.prefetch->finish(outcome.status == RequestStatus::complete);
                    if (outcome.status != RequestStatus::complete && outcome.status != RequestStatus::cancelled) {
                        trace_->record(diagnostics::ConversationStage::continuation_failed,rechat.binding.generation,
                            rechat.binding.sequence,rechat.parent.request_id,rechat.parent.line_id);
                        push_notification("SYNTH rechat stopped: " + outcome.detail);
                    }
                } catch (const std::exception& error) {
                    push_notification(std::string{"SYNTH rechat stopped: "} + error.what());
                    if (rechat.prefetch) rechat.prefetch->cancel();
                }
            }));
        if (accepted != tasks::SubmitResult::accepted) {
            trace_->record(diagnostics::ConversationStage::continuation_queue_full,binding.generation,binding.sequence);
            push_notification("SYNTH rechat stopped because the network queue is full");
            if (prefetch) prefetch->cancel();
        }
    }

    [[nodiscard]] presentation::DialogueCaptions::Caption make_caption(
        const protocol_native::Line& line, const protocol_native::Dialogue& dialogue,
        const core::CancellationToken& cancellation, const Snapshot& snapshot) const {
        auto admission = std::make_shared<presentation::SceneAdmission>();
        admission->original = snapshot;
        const auto retain = [&](const protocol_native::Identity& actor) {
            if (!snapshot) { admission->rejected.store(true); return; }
            const auto key = actor_key(actor);
            if (key == actor_key(identity(snapshot->player()))) return;
            const auto found = std::ranges::find_if(snapshot->actors(),[&](const auto& value) { return actor_key(identity(value)) == key; });
            if (found == snapshot->actors().end()) { admission->rejected.store(true); return; }
            admission->actors.push_back({cancellation,found->form_id(),found->base_form_id(),found->origin_plugin(),
                found->base_origin_plugin(),found->playthrough_id(),found->name(),true});
        };
        if (const auto* actor = std::get_if<protocol_native::Identity>(&dialogue.speaker)) retain(*actor);
        if (dialogue.listener) retain(*dialogue.listener);
        return std::make_shared<const presentation::DialogueCaption>(presentation::DialogueCaption{
            .generation = line.generation,
            .speaker = std::visit([](const auto& value) {
                return value.display_name.empty() ? std::string{"SYNTH"} : value.display_name;
            }, dialogue.speaker),
            .text = dialogue.text, .cancellation = cancellation,
            .facing = snapshot ? bind_speech_facing(line, dialogue, *snapshot, cancellation) : nullptr,
            .trace = trace_, .request_id = line.request_id, .line_id = line.line_id, .context_sequence = line.context_sequence,
            .admission = std::move(admission)});
    }

    void queue_speech(const protocol_native::Line& line,
                      const protocol_native::Dialogue& dialogue,
                      const Snapshot& snapshot, const core::CancellationToken& cancellation,
                      presentation::DialogueCaptions::Caption caption,
                      std::shared_ptr<RechatPrefetch> prefetch = {}) {
        const auto& speech = *dialogue.speech;
        if (line.generation != snapshot->generation().value() || speech.status == "failed" ||
            terminal_.load(std::memory_order_acquire)) {
            return;
        }
        const auto deadline = tasks::TaskLanes::Clock::now() +
                              std::chrono::milliseconds{speech.deadline_ms};
        const auto accepted = speech_.try_submit(
            "speech", tasks::TaskClass::current_turn, deadline, cancellation,
            tasks::TaskLanes::track_activity(speech_jobs_,
            [this, line, dialogue, snapshot, deadline, caption, prefetch](const core::CancellationToken& cancellation) {
                try {
                    // Preparation is ahead of this task on the same single-worker FIFO lane.
                    const auto prepared = prefetch ? prefetch->prepared_media(line.line_id) : std::nullopt;
                    auto bytes = prepared ? *prepared : prepare_speech_media(line, *dialogue.speech, cancellation, deadline);
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    if (tasks::TaskLanes::Clock::now() >= deadline || !bytes) {
                        trace_->record(diagnostics::ConversationStage::media_failed,
                            line.generation,line.context_sequence,line.request_id,line.line_id);
                        push_notification("SYNTH voice failed; delivering text only");
                        captions_.enqueue(caption);
                        return;
                    }
                    if (prepared) trace_->record(diagnostics::ConversationStage::media_reused,
                        line.generation,line.context_sequence,line.request_id,line.line_id);
                    enqueue_prepared_speech(line, dialogue, std::move(bytes), snapshot, cancellation, deadline, caption);
                } catch (const std::exception& error) {
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    trace_->record(diagnostics::ConversationStage::media_failed,
                        line.generation,line.context_sequence,line.request_id,line.line_id);
                    push_notification(std::string{"SYNTH voice unavailable; delivering text only: "} +
                                      error.what());
                    captions_.enqueue(caption);
                }
            }));
        if (accepted != tasks::SubmitResult::accepted) {
            push_notification("SYNTH speech queue is full; delivering text only");
            captions_.enqueue(caption);
        }
    }

    // Poll and verify immutable WAV bytes on a worker; this never presents text/audio or executes actions.
    [[nodiscard]] RechatPrefetch::MediaBytes prepare_speech_media(
        const protocol_native::Line& line, protocol_native::Speech speech,
        const core::CancellationToken& cancellation, tasks::TaskLanes::Clock::time_point deadline) {
        trace_->record(diagnostics::ConversationStage::media_prepare_started,
            line.generation,line.context_sequence,line.request_id,line.line_id);
        while (!cancellation.is_cancelled() && !terminal_.load(std::memory_order_acquire) &&
               tasks::TaskLanes::Clock::now() < deadline && speech.status != "ready") {
            const auto status = client_.fetch_tts_status(speech.cache_key, [this, cancellation] {
                return cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire);
            }, deadline);
            if (status.status == RequestStatus::complete && status.value) {
                if (status.value->status == "failed") return {};
                if (status.value->status == "ready" && status.value->media) {
                    speech.status = "ready";
                    speech.media = status.value->media;
                    break;
                }
            }
            std::this_thread::sleep_until(std::min(
                deadline, tasks::TaskLanes::Clock::now() + std::chrono::milliseconds{100}));
        }
        if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire) ||
            tasks::TaskLanes::Clock::now() >= deadline || speech.status != "ready" || !speech.media) return {};
        trace_->record(diagnostics::ConversationStage::speech_ready,
            line.generation,line.context_sequence,line.request_id,line.line_id);
        const auto& media = *speech.media;
        if (media.content_type != "audio/wav" || media.media_id != "media/" + media.sha256 ||
            media.size == 0 || media.size > 16U * 1024U * 1024U) {
            throw std::invalid_argument{"unsupported or invalid TTS media reference"};
        }
        const auto response = client_.fetch_media(media.media_id, [this, cancellation] {
            return cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire);
        }, deadline);
        if (!response.delivered() || response.response.status != 200 ||
            response.response.content_type != media.content_type ||
            response.response.body.size() != media.size) {
            throw std::runtime_error{"native TTS media fetch failed"};
        }
        media_fetch::Bytes bytes;
        bytes.reserve(response.response.body.size());
        for (const auto value : response.response.body) {
            bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
        }
        if (media_fetch::sha256_hex(bytes) != media.sha256) {
            throw std::runtime_error{"native TTS media hash mismatch"};
        }
        if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire) ||
            tasks::TaskLanes::Clock::now() >= deadline) return {};
        trace_->record(diagnostics::ConversationStage::media_prepared,
            line.generation,line.context_sequence,line.request_id,line.line_id);
        return std::make_shared<const std::vector<std::byte>>(std::move(bytes));
    }

    // A single speculative job uses the existing speech lane and is cancelled with its exact parent.
    void prepare_rechat_media(const protocol_native::Line& line, const std::shared_ptr<RechatPrefetch>& prefetch) {
        const auto speech = *std::get<protocol_native::Dialogue>(line.payload).speech;
        const auto deadline = tasks::TaskLanes::Clock::now() + std::chrono::milliseconds{speech.deadline_ms};
        const auto accepted = speech_.try_submit("speech", tasks::TaskClass::current_turn, deadline, prefetch->token(),
            tasks::TaskLanes::track_activity(speech_jobs_, [this, line, speech, deadline, prefetch](const core::CancellationToken& token) {
                RechatPrefetch::MediaBytes bytes;
                try { bytes = prepare_speech_media(line, speech, token, deadline); }
                catch (const std::exception&) { /* Failure stays invisible until normal text delivery is admitted. */ }
                if (!bytes && !token.is_cancelled()) trace_->record(diagnostics::ConversationStage::media_failed,
                    line.generation,line.context_sequence,line.request_id,line.line_id);
                prefetch->finish_media(std::move(bytes));
            }));
        // Queue saturation leaves the line unprepared; normal release can attempt it once.
        if (accepted != tasks::SubmitResult::accepted)
            trace_->record(diagnostics::ConversationStage::media_failed,line.generation,line.context_sequence,line.request_id,line.line_id);
    }

    void enqueue_prepared_speech(const protocol_native::Line& line,
                                const protocol_native::Dialogue& dialogue,
                                RechatPrefetch::MediaBytes shared,
                                const Snapshot& snapshot, const core::CancellationToken& cancellation,
                                tasks::TaskLanes::Clock::time_point deadline,
                                presentation::DialogueCaptions::Caption caption) {

        float volume = static_cast<float>(configuration_.audio.head_voice_volume_percent) / 100.0F;
        float pan{};
        std::string speaker_id{"narrator"};
        if (const auto* actor = std::get_if<protocol_native::Identity>(&dialogue.speaker)) {
            volume = static_cast<float>(configuration_.audio.voice_volume_percent) / 100.0F;
            speaker_id = actor->form_id;
            const core::ActorSnapshot* source{};
            const auto matches = [&](const core::ActorSnapshot& value) {
                return identity(value).form_id == actor->form_id &&
                       value.origin_plugin() == actor->origin_plugin &&
                       value.playthrough_id() == actor->playthrough_id;
            };
            if (matches(snapshot->player())) source = &snapshot->player();
            for (const auto& candidate : snapshot->actors()) {
                if (source == nullptr && matches(candidate)) source = &candidate;
            }
            const auto* listener = snapshot->effective_listener_pose(core::SnapshotClock::now());
            if (source != nullptr && listener != nullptr && configuration_.audio.spatial_playback) {
                audio::SpatialScene scene;
                const auto make_pose = [](const core::WorldPose& pose) {
                    const auto& forward = pose.forward().value();
                    const auto& up = pose.up().value();
                    return audio::Pose{{pose.position().x, pose.position().y, pose.position().z},
                                       {forward.x, forward.y, forward.z}, {up.x, up.y, up.z}, 1};
                };
                const auto listener_pose = make_pose(*listener);
                const auto view = snapshot->variant() == core::RuntimeVariant::vr
                                      ? audio::RuntimeView::vr
                                      : audio::RuntimeView::flat;
                (void)scene.update_listener(view, listener_pose,
                                            view == audio::RuntimeView::vr
                                                ? std::optional{listener_pose}
                                                : std::nullopt,
                                            1);
                (void)scene.set_source(speaker_id,
                                       {source->position().x, source->position().y, source->position().z});
                if (const auto azimuth = scene.azimuth_sine(speaker_id)) {
                    pan = static_cast<float>(configuration_.audio.invert_heading ? -*azimuth : *azimuth);
                }
                if (const auto distance = scene.distance_to(speaker_id)) {
                    const auto interior = snapshot->world().has_value() && snapshot->world()->interior;
                    const auto maximum = std::max(
                        1.0, static_cast<double>(interior
                                                     ? configuration_.activation.interior_distance
                                                     : configuration_.activation.exterior_distance));
                    const auto dropoff = static_cast<double>(
                        interior ? configuration_.audio.interior_dropoff_percent
                                 : configuration_.audio.exterior_dropoff_percent) / 100.0;
                    const auto scaled_distance = *distance * configuration_.audio.distance_scale;
                    volume *= static_cast<float>(
                        std::clamp(1.0 - (scaled_distance / maximum) * dropoff, 0.0, 1.0));
                }
            }
        }
        if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
        if (tasks::TaskLanes::Clock::now() >= deadline) {
            push_notification("SYNTH voice deadline reached; delivering text only");
            captions_.enqueue(caption);
            return;
        }
        if (!playback_.enqueue(audio::NativeClip{line.generation, line.request_id, line.turn_id,
                                                 dialogue.speech->utterance_id, std::move(speaker_id),
                                                 std::move(shared), volume, pan,
                                                 configuration_.audio.pre_clip_ms,
                                                 configuration_.audio.post_clip_ms, cancellation,
                                                 bind_speech_facing(line, dialogue, *snapshot, cancellation),
                                                 caption})) {
            throw std::runtime_error{"native playback queue rejected speech"};
        }
        trace_->record(diagnostics::ConversationStage::audio_queued,line.generation,line.context_sequence,line.request_id,line.line_id);
    }

    void execute_action(PendingAction pending) {
        const auto turn = pending.binding.cancellation;
        if (turn.is_cancelled()) return;
        const auto deadline = pending.deadline;
        // Expired actions still need a bounded terminal receipt; this budget never authorizes execution.
        const auto delivery_deadline = tasks::TaskLanes::Clock::now() +
                                       std::chrono::milliseconds{request_timeout_ms()};
        const auto accepted = network_.try_submit(
            "network", tasks::TaskClass::current_turn, delivery_deadline, turn,
            tasks::TaskLanes::track_activity(active_actions_,
            [this, pending = std::move(pending), deadline](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                const auto& action = pending.action;
                const auto& snapshot = pending.snapshot;
                std::string status{"unsupported_runtime"};
                std::string detail{"action capability is not implemented by this runtime"};
                std::optional<runtime::RuntimeInventoryObservation> inventory_observation;
                std::optional<runtime::RuntimeTransferObservation> transfer_observation;
                if (tasks::TaskLanes::Clock::now() >= deadline) {
                    status = "timed_out";
                    detail = "action expired while awaiting presentation or worker dispatch";
                } else if ((action.name == "equip_item" || action.name == "unequip_item") &&
                    action.capability == "action." + action.name && !client_.equipment_actions_ready()) {
                    status = "rejected";
                    detail = "server has not acknowledged equipment inventory observations; no action was executed";
                } else if (action.name == "consume" && action.capability == "action.consume" && !client_.consumption_ready()) {
                    status = "rejected";
                    detail = "server has not acknowledged consumption inventory observations; no action was executed";
                } else if (action.name == "give_item_to" && action.capability == "action.give_item_to" && !client_.transfer_ready()) {
                    status = "rejected";
                    detail = "server has not acknowledged paired transfer inventories; no action was executed";
                } else if (action.name == "give_caps_to" && action.capability == "action.give_caps_to" && !client_.caps_ready()) {
                    status = "rejected";
                    detail = "server has not acknowledged caps inventory observations; no action was executed";
                } else if (action.name == "take_caps_from_player" && action.capability == "action.take_caps_from_player" && !client_.player_caps_ready()) {
                    status = "rejected";
                    detail = "server has not acknowledged player payment inventories; no action was executed";
                } else if (action.name == "end_conversation" &&
                    action.capability == "action.end_conversation") {
                    const auto matches = [&](const core::ActorSnapshot& actor) {
                        const auto actor_identity = identity(actor);
                        return actor_identity.form_id == action.actor.form_id &&
                               actor.origin_plugin() == action.actor.origin_plugin &&
                               actor.playthrough_id() == action.actor.playthrough_id;
                    };
                    auto present = matches(snapshot->player());
                    for (const auto& actor : snapshot->actors()) {
                        if (!present && matches(actor)) present = true;
                    }
                    if (!present) {
                        status = "unavailable";
                        detail = "canonical actor is no longer present in the captured frame";
                    } else {
                        std::scoped_lock turn_lock{turn_mutex_};
                        if (cancellation.is_cancelled()) return;
                        if (tasks::TaskLanes::Clock::now() >= deadline) {
                            status = "timed_out";
                            detail = "action expired before conversation control";
                        } else {
                            end_conversation(action.actor);
                            status = "succeeded";
                            detail = "SYNTH conversation ended and reactivation cooldown applied";
                        }
                    }
                } else if ((action.name == "sheathe_weapon" || action.name == "equip_item" || action.name == "unequip_item" || action.name == "consume" || action.name == "give_item_to" || action.name == "give_caps_to" || action.name == "take_caps_from_player") &&
                    action.capability == "action." + action.name) {
                    std::uint32_t form_id{};
                    if (action.actor.form_id.size() != 10 ||
                        !action.actor.form_id.starts_with("0x")) {
                        status = "rejected";
                        detail = "action actor has an invalid canonical form ID";
                    } else {
                        const auto first = action.actor.form_id.data() + 2;
                        const auto last = action.actor.form_id.data() + action.actor.form_id.size();
                        const auto parsed = std::from_chars(first, last, form_id, 16);
                        if (parsed.ec != std::errc{} || parsed.ptr != last || form_id == 0) {
                            status = "rejected";
                            detail = "action actor has an invalid canonical form ID";
                        } else {
                            auto completion = std::make_shared<RuntimeActionCompletion>();
                            runtime::RuntimeActionRequest request{
                                runtime::RuntimeActionName::sheathe_weapon,
                                {form_id, action.actor.origin_plugin, action.actor.playthrough_id}};
                            bool request_valid=true;
                            if (action.name == "give_item_to" || action.name == "give_caps_to" || action.name == "take_caps_from_player") {
                                try { request=prepare_transfer_action(*snapshot,action,cancellation,deadline); }
                                catch (const std::invalid_argument& error) {
                                    request_valid=false;status="rejected";detail=error.what();
                                }
                            } else if (action.name != "sheathe_weapon") {
                                request.name = action.name == "consume" ? runtime::RuntimeActionName::consume :
                                               action.name == "equip_item" ? runtime::RuntimeActionName::equip_item
                                                                           : runtime::RuntimeActionName::unequip_item;
                                request.cancellation = cancellation;
                                request.deadline = deadline;
                                request.action_id = action.action_id;
                                const auto* item = action.arguments.find("item");
                                if (item && item->is_string() && action.arguments.as_object().size() == 1 &&
                                    (!action.target || actor_key(*action.target) == actor_key(action.actor))) {
                                    for (const auto& actor : snapshot->actors()) {
                                        if (actor_key(identity(actor)) != actor_key(action.actor)) continue;
                                        request.item = actions::select_equipment_item(actor.inventory(), actor.inventory_observation(), item->as_string());
                                        break;
                                    }
                                }
                            }
                            if (request_valid) {
                                const auto dispatch = dispatcher_.try_enqueue(
                                    snapshot->generation(),
                                    [completion, request, cancellation, deadline](
                                        runtime::IFalloutRuntime& fallout) {
                                        completion->execute([&] { return fallout.execute_action(request); },
                                                            cancellation, deadline);
                                    });
                                if (dispatch == runtime::DispatchResult::stale) {
                                    status = "cancelled";
                                    detail = "runtime generation changed before action dispatch";
                                } else if (dispatch == runtime::DispatchResult::saturated) {
                                    status = "failed";
                                    detail = "game-thread action queue is full";
                                } else if (dispatch == runtime::DispatchResult::stopped) {
                                    status = "cancelled";
                                    detail = "game-thread action dispatcher is stopped";
                                } else {
                                    std::unique_lock lock{completion->mutex};
                                    while (completion->phase !=
                                           RuntimeActionCompletion::Phase::complete) {
                                        if (cancellation.is_cancelled() ||
                                            terminal_.load(std::memory_order_acquire)) {
                                            if (completion->phase ==
                                                RuntimeActionCompletion::Phase::pending) {
                                                completion->phase =
                                                    RuntimeActionCompletion::Phase::abandoned;
                                            }
                                            return;
                                        }
                                        if (tasks::TaskLanes::Clock::now() >= deadline &&
                                            completion->phase == RuntimeActionCompletion::Phase::pending) {
                                            completion->phase =
                                                RuntimeActionCompletion::Phase::abandoned;
                                            status = "timed_out";
                                            detail = "action expired before game-thread dispatch";
                                            break;
                                        }
                                        completion->changed.wait_for(
                                            lock, std::chrono::milliseconds{10});
                                    }
                                    if (completion->result) {
                                        if (completion->result->inventory && completion->result->inventory->valid_for(request))
                                            inventory_observation=completion->result->inventory;
                                        if (completion->result->transfer && completion->result->transfer->valid_for(request))
                                            transfer_observation=completion->result->transfer;
                                        detail = completion->result->detail;
                                        switch (completion->result->status) {
                                        case runtime::RuntimeActionStatus::succeeded:
                                            status = "succeeded";
                                            break;
                                        case runtime::RuntimeActionStatus::rejected:
                                            status = "rejected";
                                            break;
                                        case runtime::RuntimeActionStatus::unavailable:
                                            status = "unavailable";
                                            break;
                                        case runtime::RuntimeActionStatus::failed:
                                            status = "failed";
                                            break;
                                        case runtime::RuntimeActionStatus::unsupported_runtime:
                                            status = "unsupported_runtime";
                                            break;
                                        case runtime::RuntimeActionStatus::timed_out:
                                            status = "timed_out";
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else if (action.name == "inspect_actor" &&
                           action.capability == "action.inspect_actor") {
                    const core::ActorSnapshot* found{};
                    const auto matches = [&](const core::ActorSnapshot& actor) {
                        const auto actor_identity = identity(actor);
                        return actor_identity.form_id == action.actor.form_id &&
                               actor.origin_plugin() == action.actor.origin_plugin &&
                               actor.playthrough_id() == action.actor.playthrough_id;
                    };
                    if (matches(snapshot->player())) found = &snapshot->player();
                    for (const auto& actor : snapshot->actors()) {
                        if (found == nullptr && matches(actor)) found = &actor;
                    }
                    if (found == nullptr) {
                        status = "unavailable";
                        detail = "canonical actor is no longer present in the captured frame";
                    } else {
                        status = "succeeded";
                        json::Value::Object position{{"x", found->position().x},
                                                     {"y", found->position().y},
                                                     {"z", found->position().z}};
                        json::Value::Object inspection{
                            {"display_name", found->name()}, {"form_id", action.actor.form_id},
                            {"origin_plugin", found->origin_plugin()},
                            {"playthrough_id", found->playthrough_id()},
                            {"position", json::Value{std::move(position)}}};
                        inspection.emplace_back("alive", found->alive());
                        inspection.emplace_back("disabled", found->disabled());
                        inspection.emplace_back("in_combat", found->in_combat());
                        inspection.emplace_back("hostile_to_player", found->hostile_to_player());
                        inspection.emplace_back("sneaking", found->sneaking());
                        inspection.emplace_back("level", static_cast<std::int64_t>(found->level()));
                        inspection.emplace_back("race", found->race());
                        inspection.emplace_back("sex", found->sex());
                        inspection.emplace_back("voice_type", found->voice_type());
                        inspection.emplace_back("teammate", found->teammate());
                        inspection.emplace_back("health_percent", found->health_percent_available() ? json::Value{found->health_percent()} : json::Value{});
                        inspection.emplace_back("action_points_percent", found->action_points_percent_available() ? json::Value{found->action_points_percent()} : json::Value{});
                        inspection.emplace_back(
                            "line_of_sight",
                            found->line_of_sight()
                                ? (*found->line_of_sight() ? "visible" : "blocked")
                                : "unknown");
                        detail = json::write(json::Value{std::move(inspection)});
                    }
                } else if (action.name == "inspect_surroundings" &&
                           action.capability == "action.inspect_surroundings") {
                    detail.clear();
                    json::Value::Array actors;
                    json::Value::Array items;
                    json::Value::Array points_of_interest;
                    const auto encode = [&](const auto& actor_rows, const auto& item_rows,
                                            const auto& poi_rows) {
                        return json::write(json::Value{protocol_native::object({
                            {"schema", "synth.action.inspect_surroundings.result.v1"},
                            {"actors", actor_rows}, {"items", item_rows},
                            {"points_of_interest", poi_rows},
                            {"nearby_items_observation", protocol_native::retained_collection_observation(
                                snapshot->nearby_items_observation(), snapshot->nearby_items().size(), item_rows.size())},
                            {"points_of_interest_observation", protocol_native::retained_collection_observation(
                                snapshot->points_of_interest_observation(), snapshot->points_of_interest().size(), poi_rows.size())},
                            {"truncated", actor_rows.size() < snapshot->actors().size() ||
                                              item_rows.size() < snapshot->nearby_items().size() ||
                                              poi_rows.size() < snapshot->points_of_interest().size()},
                        })});
                    };
                    for (const auto& nearby : snapshot->actors()) {
                        if (actors.size() == 2) break;
                        const auto& position = nearby.position();
                        const auto& player_position = snapshot->player_pose().position();
                        const auto dx = position.x - player_position.x;
                        const auto dy = position.y - player_position.y;
                        const auto dz = position.z - player_position.z;
                        auto candidate = actors;
                        candidate.emplace_back(json::Value{protocol_native::object({
                            {"identity", protocol_native::identity_value(identity(nearby))},
                            {"distance", std::sqrt(dx * dx + dy * dy + dz * dz)},
                            {"in_combat", nearby.in_combat()},
                            {"hostile_to_player", nearby.hostile_to_player()},
                            {"line_of_sight", nearby.line_of_sight()
                                                   ? (*nearby.line_of_sight() ? "visible" : "blocked")
                                                   : "unknown"},
                        })});
                        auto encoded = encode(candidate, items, points_of_interest);
                        if (encoded.size() > 1024) break;
                        actors = std::move(candidate);
                        detail = std::move(encoded);
                    }
                    for (const auto& item : snapshot->nearby_items()) {
                        if (items.size() == 2) break;
                        char reference_id[11]{};
                        char base_form_id[11]{};
                        std::snprintf(reference_id, sizeof(reference_id), "0x%08X", item.reference_id);
                        std::snprintf(base_form_id, sizeof(base_form_id), "0x%08X", item.base_form_id);
                        auto candidate = items;
                        candidate.emplace_back(json::Value{protocol_native::object({
                            {"reference_id", reference_id}, {"base_form_id", base_form_id},
                            {"base_origin_plugin", item.base_origin_plugin},
                            {"display_name", item.display_name}, {"distance", item.distance},
                            {"count", static_cast<std::uint64_t>(item.count)},
                            {"stealing", item.stealing},
                            {"looking_at", item.looking_at}, {"held", item.held},
                        })});
                        auto encoded = encode(actors, candidate, points_of_interest);
                        if (encoded.size() > 1024) break;
                        items = std::move(candidate);
                        detail = std::move(encoded);
                    }
                    for (const auto& poi : snapshot->points_of_interest()) {
                        if (points_of_interest.size() == 2) break;
                        char reference_id[11]{};
                        std::snprintf(reference_id, sizeof(reference_id), "0x%08X", poi.reference_id);
                        auto candidate = points_of_interest;
                        candidate.emplace_back(json::Value{protocol_native::object({
                            {"reference_id", reference_id}, {"display_name", poi.display_name},
                            {"kind", poi.kind}, {"distance", poi.distance},
                            {"locked", poi.locked}, {"looking_at", poi.looking_at},
                        })});
                        auto encoded = encode(actors, items, candidate);
                        if (encoded.size() > 1024) break;
                        points_of_interest = std::move(candidate);
                        detail = std::move(encoded);
                    }
                    status = "succeeded";
                    if (detail.empty()) detail = encode(actors, items, points_of_interest);
                } else if (action.name == "check_inventory" &&
                           action.capability == "action.check_inventory") {
                    const core::ActorSnapshot* found{};
                    const auto matches = [&](const core::ActorSnapshot& actor) {
                        const auto actor_identity = identity(actor);
                        return actor_identity.form_id == action.actor.form_id &&
                               actor.origin_plugin() == action.actor.origin_plugin &&
                               actor.playthrough_id() == action.actor.playthrough_id;
                    };
                    if (matches(snapshot->player())) found = &snapshot->player();
                    for (const auto& actor : snapshot->actors()) {
                        if (found == nullptr && matches(actor)) found = &actor;
                    }
                    if (found == nullptr) {
                        status = "unavailable";
                        detail = "canonical actor is no longer present in the captured frame";
                    } else {
                        json::Value::Array items;
                        for (const auto& item : found->inventory()) {
                            char form_id[11]{};
                            std::snprintf(form_id, sizeof(form_id), "0x%08X", item.form_id);
                            items.emplace_back(json::Value{protocol_native::object({
                                {"form_id", form_id}, {"origin_plugin", item.origin_plugin},
                                {"display_name", item.display_name},
                                {"count", static_cast<std::uint64_t>(item.count)},
                                {"value", static_cast<std::int64_t>(item.value)},
                                {"weight", item.weight},
                                {"form_type", static_cast<std::uint64_t>(item.form_type)},
                                {"equipped", item.equipped},
                            })});
                        }
                        auto result = protocol_native::inventory_action_result(
                            action.actor, std::move(items), found->inventory_observation());
                        status = std::move(result.first);
                        detail = std::move(result.second);
                    }
                } else if (action.name == "read_quests" &&
                           action.capability == "action.read_quests") {
                    json::Value::Array quests;
                    for (const auto& quest : snapshot->active_quests()) {
                        char form_id[11]{};
                        std::snprintf(form_id, sizeof(form_id), "0x%08X", quest.form_id);
                        quests.emplace_back(protocol_native::quest_state_value({
                            form_id, quest.origin_plugin, quest.display_name, quest.editor_id,
                            quest.current_stage, quest.active_objectives, quest.objectives,
                            quest.objectives_observation, quest.tracked}, client_.quest_tracking_ready()));
                    }
                    auto result = protocol_native::quest_action_result(
                        std::move(quests), snapshot->active_quests_observation());
                    status = std::move(result.first);
                    detail = std::move(result.second);
                }
                if (detail.size() > 1024) detail.resize(1024);
                try {
                    auto continuation_snapshot=snapshot;
                    std::optional<protocol_native::ActionInventoryObservation> inventory;
                    std::optional<protocol_native::ActionTransferObservation> transfer;
                    if (client_.action_inventory_ready() &&
                        (action.name=="equip_item" || action.name=="unequip_item" ||
                            (action.name=="consume" && client_.consumption_ready())) && action.capability=="action."+action.name) {
                        auto prepared=prepare_action_inventory(snapshot,action,cancellation,inventory_observation,client_.extended_inventory_ready());
                        continuation_snapshot=std::move(prepared.snapshot);inventory=std::move(prepared.inventory);
                    }
                    if (((client_.transfer_ready() && action.name=="give_item_to") || (client_.caps_ready() && action.name=="give_caps_to") ||
                        (client_.player_caps_ready() && action.name=="take_caps_from_player"))
                        && action.capability=="action."+action.name) {
                        auto prepared=prepare_transfer_inventory(snapshot,action,cancellation,transfer_observation,client_.extended_inventory_ready());
                        continuation_snapshot=std::move(prepared.snapshot);transfer=std::move(prepared.transfer);
                    }
                    const auto outcome = client_.send_action_result(
                        action.action_id, action.idempotency_key, std::move(status),
                        std::move(detail), [this, continuation_snapshot, cancellation](const protocol_native::Line& continuation) {
                            handle_line(continuation, continuation_snapshot, cancellation);
                        }, pending.binding, std::move(inventory), std::move(transfer),
                        action.name=="give_caps_to" || action.name=="take_caps_from_player", action.name=="take_caps_from_player");
                    if (outcome.status != RequestStatus::complete) {
                        push_notification("SYNTH action result delivery failed: " + outcome.detail);
                    }
                } catch (const std::exception& error) {
                    push_notification(std::string{"SYNTH action result: "} + error.what());
                    if ((action.name=="give_item_to" || action.name=="give_caps_to" || action.name=="take_caps_from_player") && action.capability=="action."+action.name) {
                        // An impossible pair cannot produce a truthful receipt. Retire only its owned chain.
                        {
                            std::scoped_lock lock{turn_mutex_};
                            (void)turn_cancellation_.cancel_if_owner(cancellation);
                        }
                        if (!pending.binding.context_request_id.empty()) {
                            try {
                                const auto stopped=client_.cancel(pending.binding.context_request_id,pending.binding.generation);
                                if (stopped.status!=RequestStatus::complete && stopped.status!=RequestStatus::cancelled)
                                    push_notification("SYNTH transfer stopped locally; server cancellation was not acknowledged");
                            } catch (const std::exception&) {
                                push_notification("SYNTH transfer stopped locally; server cancellation failed");
                            }
                        }
                    }
                }
            }));
        if (accepted != tasks::SubmitResult::accepted) {
            push_notification("SYNTH action dispatch queue is full");
        }
    }

    [[nodiscard]] static protocol_native::Identity identity(const core::ActorSnapshot& actor) {
        char form_id[11]{};
        std::snprintf(form_id, sizeof(form_id), "0x%08X", actor.form_id());
        return {form_id, actor.origin_plugin(), actor.playthrough_id(), actor.name()};
    }

    [[nodiscard]] static protocol_native::Identity identity(const context::TargetContext& actor) {
        char form_id[11]{};
        std::snprintf(form_id, sizeof(form_id), "0x%08X", actor.identity().form().form_id());
        return {form_id, actor.identity().form().plugin(), actor.identity().playthrough().value(),
                actor.name()};
    }

    [[nodiscard]] static std::string actor_key(const protocol_native::Identity& actor) {
        auto plugin = actor.origin_plugin;
        std::ranges::transform(plugin, plugin.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return actor.playthrough_id + "|" + plugin + "|" + actor.form_id;
    }

    [[nodiscard]] bool conversation_on_cooldown(
        const protocol_native::Identity& actor,
        core::SnapshotClock::time_point now) {
        std::scoped_lock lock{conversation_mutex_};
        const auto found = conversation_cooldowns_.find(actor_key(actor));
        if (found == conversation_cooldowns_.end()) return false;
        if (now >= found->second) {
            conversation_cooldowns_.erase(found);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool consume_rechat_suppression(
        const protocol_native::Identity& actor) {
        std::scoped_lock lock{conversation_mutex_};
        return suppressed_rechat_actors_.erase(actor_key(actor)) != 0;
    }

    void clear_rechat_suppression(const protocol_native::Identity& actor) {
        std::scoped_lock lock{conversation_mutex_};
        suppressed_rechat_actors_.erase(actor_key(actor));
    }

    // Ends only SYNTH-owned dialogue state; vanilla Fallout dialogue remains untouched.
    void end_conversation(const protocol_native::Identity& actor) {
        const auto key = actor_key(actor);
        {
            std::scoped_lock lock{conversation_mutex_};
            external_owner_.end(key);
            conversation_cooldowns_.insert_or_assign(
                key, core::SnapshotClock::now() + std::chrono::seconds{
                    configuration_.behavior.end_conversation_cooldown_seconds});
            suppressed_rechat_actors_.insert(key);
        }
        {
            std::scoped_lock lock{activation_mutex_};
            if (last_automatic_actor_ == key) last_automatic_actor_.clear();
        }
        {
            std::scoped_lock lock{rechat_mutex_};
            if (pending_rechat_ && actor_key(pending_rechat_->actor) == key) {
                pending_rechat_.reset();
            }
            if (rechat_capture_ && actor_key(rechat_capture_->actor)==key) rechat_capture_.reset();
            if (prefetched_rechat_ && actor_key(prefetched_rechat_->actor)==key) {
                prefetched_rechat_->prefetch->cancel();
                prefetched_rechat_.reset();
            }
        }
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

    // Admit one copied rest batch behind existing diary work; saturation retains it only until its bounded expiry.
    void pump_automatic_diary() {
        if (!automatic_diary_ready() || diary_tasks_.load(std::memory_order_acquire) != 0) return;
        std::scoped_lock lock{diary_mutex_};
        const auto now = tasks::TaskLanes::Clock::now();
        if (automatic_diaries_.expire(now) != 0) push_notification("SYNTH expired queued automatic diaries before admission");
        if (pending_diary_ || !automatic_diaries_.front() || now < next_automatic_diary_attempt_) return;
        next_automatic_diary_attempt_ = now + std::chrono::seconds{5};
        const auto item = *automatic_diaries_.front();
        const auto accepted = network_.try_submit("network", tasks::TaskClass::background,
            item.expires, cancellation_.token(), tasks::TaskLanes::track_activity(diary_tasks_,
            [this, item](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                if (tasks::TaskLanes::Clock::now() >= item.expires) {
                    push_notification("SYNTH automatic diary expired before context publication"); return;
                }
                try {
                    const auto context_turn = client_.begin_context_turn();
                    if (tasks::TaskLanes::Clock::now() >= item.expires || cancellation.is_cancelled()) return;
                    std::vector<protocol_native::Identity> audience;
                    const auto published = publish_snapshot(item.snapshot, std::nullopt, cancellation, &audience);
                    if (published.status != RequestStatus::complete) {
                        push_notification("SYNTH automatic diary context was not acknowledged"); return;
                    }
                    if (tasks::TaskLanes::Clock::now() >= item.expires || cancellation.is_cancelled()) {
                        push_notification("SYNTH automatic diary expired or was cancelled before admission"); return;
                    }
                    auto admission = client_.request_diary(protocol_native::DiaryRequest{
                        std::move(audience), true, true, item.reason == core::RestStart::sleep ?
                            protocol_native::DiaryReason::sleep : protocol_native::DiaryReason::wait}, published.context_binding);
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    const auto& outcome = admission.outcome();
                    if (outcome.status != RequestStatus::complete) {
                        push_notification("SYNTH automatic diary admission failed; check server logs");
                    } else if (outcome.detail == "diary_queued") {
                        std::scoped_lock diary_lock{diary_mutex_};
                        if (terminal_.load(std::memory_order_acquire) || cancellation.is_cancelled()) return;
                        const auto admitted_at = tasks::TaskLanes::Clock::now();
                        pending_diary_ = PendingDiary{std::move(admission), admitted_at + std::chrono::minutes{30},
                            admitted_at + std::chrono::seconds{5}};
                        push_notification("SYNTH automatic diary batch queued for background generation");
                    }
                    // Disabled/cooldown roles are normal automatic policy, not a dialogue failure.
                } catch (const std::exception&) { push_notification("SYNTH automatic diary request failed; check server logs"); }
            }));
        if (accepted == tasks::SubmitResult::accepted) automatic_diaries_.pop();
    }

    // One short background RPC per interval; the saved receipt retains every selected role and original context.
    void pump_diary_status() {
        if (diary_tasks_.load(std::memory_order_acquire) != 0) return;
        std::optional<PendingDiary> pending;
        {
            std::scoped_lock lock{diary_mutex_};
            if (!pending_diary_) return;
            const auto now = tasks::TaskLanes::Clock::now();
            if (now >= pending_diary_->deadline) {
                pending_diary_.reset();
                push_notification("SYNTH diary completion tracking timed out; completion is not confirmed");
                return;
            }
            if (pending_diary_->ready || now < pending_diary_->next_poll) return;
            pending_diary_->next_poll = now + std::chrono::seconds{5};
            pending = pending_diary_;
        }
        (void)network_.try_submit("network", tasks::TaskClass::background,
            tasks::TaskLanes::Clock::now() + std::chrono::seconds{3}, cancellation_.token(),
            tasks::TaskLanes::track_activity(diary_tasks_,
            [this, pending = std::move(*pending)](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                RequestOutcome outcome;
                try { outcome = client_.fetch_diary_status(pending.admission); }
                catch (const std::exception&) { outcome.status = RequestStatus::malformed_response; }
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                std::scoped_lock lock{diary_mutex_};
                if (!pending_diary_) return;
                if (outcome.status == RequestStatus::complete) {
                    if (outcome.detail == "diary_queued" || outcome.detail == "diary_running") return;
                    if (outcome.detail == "diary_ready" || outcome.detail == "diary_partial") {
                        pending_diary_->ready = true;
                        pending_diary_->partial = outcome.detail == "diary_partial";
                        return;
                    }
                    pending_diary_.reset();
                    push_notification("SYNTH diary did not complete; the server reported a terminal diary state");
                } else if (outcome.status != RequestStatus::transport_failure &&
                    !(outcome.status == RequestStatus::http_failure && outcome.detail.starts_with("HTTP 503"))) {
                    pending_diary_.reset();
                    push_notification("SYNTH diary completion could not be verified; check server logs");
                }
            }));
    }

    struct PendingDiary final {
        DiaryAdmission admission;
        tasks::TaskLanes::Deadline deadline;
        tasks::TaskLanes::Deadline next_poll;
        bool ready{};
        bool partial{};
        unsigned checkpoint_failures{};
    };

    // Call only before worker admission; continuations keep their original immutable enriched snapshot.
    [[nodiscard]] bool enrich_selected_actor(Snapshot& snapshot, const protocol_native::Identity& selected) {
        if (!actor_enricher_) return true; // Engine-free sessions keep their explicitly provided fixture evidence.
        if (!snapshot || snapshot->generation() != cancellation_.token().generation() || terminal_.load(std::memory_order_acquire)) return false;
        const auto key = actor_key(selected);
        const auto found = std::ranges::find_if(snapshot->actors(), [&](const auto& actor) {
            return actor_key(identity(actor)) == key;
        });
        if (found == snapshot->actors().end()) return false;
        try {
            auto enriched = actor_enricher_(snapshot, found->form_id());
            if (!enriched) { push_notification("SYNTH selected actor changed or capture is unavailable; try again"); return false; }
            snapshot = std::move(enriched);
            return true;
        } catch (const std::exception& error) {
            push_notification(std::string{"SYNTH selected actor capture: "} + error.what());
            return false;
        }
    }

    ActorEnricher actor_enricher_;
    integration::ExternalConversationOwner external_owner_;
    std::vector<std::string> active_quest_actors_; // Successful activation ACKs only; bounded, session-owned.
    runtime::GameThreadDispatcher& dispatcher_;
    core::SaveContextStore* save_context_{};
    const core::SaveContextStore::Binding save_binding_;
    config::Config configuration_;
    mutable std::mutex control_mutex_;
    mutable ControlState control_state_;
    WinHttpTransport transport_;
    SynthClient client_;
    core::CancellationSource cancellation_;
    std::mutex turn_mutex_;
    core::CancellationSource turn_cancellation_;
    core::CancellationToken voice_turn_;
    std::optional<integration::PromptTarget> voice_target_;
    protocol_native::ControlSelection voice_controls_;
    tasks::TaskLanes network_;
    // A single delivery worker preserves streamed sentence order while network/control work continues independently.
    tasks::TaskLanes speech_{{tasks::LaneConfig{"speech", 16, 4, 3}}, 1};
    tasks::TaskLanes control_{{tasks::LaneConfig{"control", 4, 4, 4}}, 1};
    audio::NativeAudioPlayback playback_;
    input::NativeMicrophone microphone_;
    std::atomic_bool initialized_{};
    std::atomic_bool initialization_failed_{};
    std::atomic_bool terminal_{};
    std::atomic_bool runtime_paused_{};
    std::atomic_bool composing_{};
    std::atomic_bool halting_{};
    std::atomic_size_t halt_requests_{};
    std::atomic_size_t speech_jobs_{};
    std::atomic_size_t active_responses_{};
    std::atomic_size_t visual_tasks_{};
    std::atomic_size_t active_actions_{};
    std::atomic_size_t diary_tasks_{};
    std::atomic_size_t world_context_tasks_{};
    std::atomic_bool world_context_failure_reported_{};
    std::atomic_uint64_t acknowledged_inventory_frame_{};
    std::mutex world_context_mutex_;
    std::optional<context::WorldObservation> acknowledged_world_context_;
    std::optional<context::QuestObservation> acknowledged_quest_context_;
    std::uint64_t acknowledged_quest_sequence_{};
    std::string acknowledged_quest_player_;
    std::uint64_t acknowledged_world_frame_{};
    std::uint64_t acknowledged_world_generation_{};
    std::mutex diary_mutex_;
    std::optional<PendingDiary> pending_diary_;
    const bool automatic_diary_enabled_;
    core::AutomaticDiaryQueue automatic_diaries_;
    const bool quest_events_enabled_;
    QuestEventDelivery quest_events_;
    const bool actor_events_enabled_;
    ActorEventDelivery actor_events_;
    std::atomic_size_t actor_event_tasks_{};
    std::atomic_bool actor_event_failure_reported_{};
    tasks::TaskLanes::Deadline next_actor_event_attempt_{};
    core::PlayerEventSampler player_events_;
    std::optional<core::CancellationSource> player_event_owner_;
    core::SnapshotClock::time_point next_player_event_attempt_{};
    std::atomic_size_t player_event_tasks_{};
    // Serials occupy 53 wire-safe bits; bit 63 atomically marks a terminal rejection instead of an ACK.
    std::atomic_uint64_t player_event_receipt_{};
    std::atomic_bool player_event_failure_reported_{};
    std::atomic_size_t quest_event_tasks_{};
    std::atomic_bool quest_event_ready_{};
    std::atomic_bool quest_event_failure_reported_{};
    tasks::TaskLanes::Deadline next_quest_event_attempt_{};
    tasks::TaskLanes::Deadline next_automatic_diary_attempt_{};
    core::SnapshotClock::time_point open_mic_started_at_{};
    core::SnapshotClock::time_point open_mic_last_voice_at_{};
    bool open_mic_voice_detected_{};
    std::mutex activity_mutex_;
    core::SnapshotClock::time_point last_activity_at_{core::SnapshotClock::now()};
    core::SnapshotClock::time_point last_combat_bark_at_{};
    std::mutex activation_mutex_;
    std::string last_automatic_actor_;
    std::mutex conversation_mutex_;
    std::unordered_map<std::string, core::SnapshotClock::time_point> conversation_cooldowns_;
    std::unordered_set<std::string> suppressed_rechat_actors_;
    std::mutex action_mutex_;
    std::deque<PendingAction> pending_actions_;
    std::mutex rechat_mutex_;
    std::shared_ptr<diagnostics::ConversationTrace> trace_{std::make_shared<diagnostics::ConversationTrace>()};
    std::optional<PendingRechat> pending_rechat_;
    std::optional<PendingRechat> rechat_capture_;
    std::optional<PendingRechat> prefetched_rechat_;
    std::mutex notification_mutex_;
    presentation::DialogueCaptions captions_;
    presentation::DialogueCaptions::Caption failed_playback_caption_;
    std::deque<std::string> notifications_;
    voice::SampleWorker voice_samples_;
};

}  // namespace synth::client
