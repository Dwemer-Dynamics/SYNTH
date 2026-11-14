#pragma once

#include "client/actor_event_delivery.hpp"
#include "client/context_publication.hpp"
#include "client/dialogue_observation_delivery.hpp"
#include "client/player_dialogue.hpp"
#include "client/quest_event_delivery.hpp"
#include "client/synth_client.hpp"
#include "config/config.hpp"
#include "core/enemy_awareness.hpp"
#include "core/player_event_sampler.hpp"
#include "core/snapshot.hpp"
#include "tasks/task_lanes.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace synth::client {

// Owns one session's observed-event delivery and actor observation state: scheduling, delivery and
// retirement of the session's borrowed quest event buffer (whose journal witnesses
// ContextPublication acknowledges directly), the actor event buffer, the player event sampler with its game-thread receipt and reaction FIFO, the
// enemy awareness sampler and cache, and the installed selected-actor enricher. Each delivery keeps
// its own outstanding-task counter, failure-report latch and five-second attempt cadence. A
// complete response is the only acknowledgement; ambiguous failures retain the exact batch.
//
// Producers (observe_*, enemy awareness capture, player sampling and the enricher) run on the game
// thread with copied scalars or snapshots. The pump_* and publish_player_event calls only admit one
// background task on the bounded network lane; preparation, encoding and HTTP run on that worker,
// under client_.begin_context_turn(). Player event receipts travel back through one atomic and are
// applied on the producer thread. The enemy awareness mutex is a leaf held only over copied values;
// the enricher's runtime capture runs outside it, and no lock here is held across HTTP or a callback.
//
// The session keeps the cross-feature admission (the busy predicate: microphone recording, live
// responses, queued actions, speech jobs or playback), turn ownership (current_turn), the reaction
// triggers that consume player reactions, and notifications. Flat-only quest and actor delivery
// follow the advertised variant; player events and enemy awareness follow server readiness.
//
// Lifetimes: the borrowed configuration, client, network lane, context publication, quest delivery
// and flags outlive this object; it is declared after all of them. Network tasks queued here capture this object and
// copied snapshots/events only; the session joins the network lane in its destructor body before any
// member is destroyed, and a rejected, expired or discarded task is destroyed unrun with its counter
// guard released once.
class ObservedEventDelivery final {
public:
    using Snapshot = std::shared_ptr<const core::RuntimeSnapshot>;
    using Notify = std::function<void(std::string)>;
    // Game-thread runtime capture of one selected actor over the copied scene.
    using ActorEnricher = std::function<Snapshot(const Snapshot&, std::uint32_t)>;
    // Cross-feature delivery gate evaluated by the session: recording, responses, actions or speech.
    using Busy = std::function<bool()>;
    using CurrentTurn = std::function<core::CancellationToken()>;

    struct Dependencies final {
        const config::Config& configuration;
        SynthClient& client;
        tasks::TaskLanes& network;
        ContextPublication& context;
        QuestEventDelivery& quest_events;
        const std::atomic_bool& terminal;
        const std::atomic_bool& paused;
        core::CancellationToken session;
        core::RuntimeVariant variant;
        bool quest_events_available;
        bool actor_events_available;
        bool observed_dialogue_available;
        Busy busy;
        CurrentTurn current_turn;
        Notify notify;
    };

    [[nodiscard]] static constexpr bool quest_events_enabled(core::RuntimeVariant variant, bool available) noexcept {
        return variant == core::RuntimeVariant::flat && available;
    }

    explicit ObservedEventDelivery(Dependencies dependencies)
        : configuration_{dependencies.configuration},
          client_{dependencies.client},
          network_{dependencies.network},
          context_{dependencies.context},
          terminal_{dependencies.terminal},
          paused_{dependencies.paused},
          session_{std::move(dependencies.session)},
          busy_{std::move(dependencies.busy)},
          current_turn_{std::move(dependencies.current_turn)},
          notify_{std::move(dependencies.notify)},
          quest_events_enabled_{quest_events_enabled(dependencies.variant, dependencies.quest_events_available)},
          quest_events_{dependencies.quest_events},
          actor_events_enabled_{dependencies.variant == core::RuntimeVariant::flat && dependencies.actor_events_available},
          actor_events_{client_.runtime_session_id(),session_.generation()},
          observed_dialogue_enabled_{dependencies.variant == core::RuntimeVariant::flat && dependencies.observed_dialogue_available},
          dialogue_observations_{session_.generation()},
          player_events_{session_.generation(), dependencies.variant} {
        if (!busy_ || !current_turn_ || !notify_)
            throw std::invalid_argument{"observed event delivery requires busy, turn and notification sinks"};
    }

    ObservedEventDelivery(const ObservedEventDelivery&) = delete;
    ObservedEventDelivery& operator=(const ObservedEventDelivery&) = delete;

    // Installed on the game thread when the coordinator-created session is adopted.
    void set_actor_enricher(ActorEnricher enrich) { actor_enricher_ = std::move(enrich); }

    // Session retirement: drop awareness, then close the shared quest buffer and the actor buffer.
    void stop() noexcept {
        clear_enemy_awareness();
        quest_events_.stop();
        actor_events_.stop();
        dialogue_observations_.stop();
    }

    void clear_enemy_awareness() noexcept {
        std::scoped_lock lock{enemy_awareness_mutex_}; enemy_awareness_cache_.clear(); enemy_awareness_sampler_.clear();
    }

    // The adapter must bind copied scalars to this generation's capture before calling; no engine pointers enter the queue.
    [[nodiscard]] bool observe_quest_events(Snapshot snapshot, std::span<const core::QuestEvent> events) {
        if (!quest_events_enabled_ || !context_.quest_events_registered() ||
            terminal_.load(std::memory_order_acquire)) return false;
        return quest_events_.enqueue(std::move(snapshot), events);
    }

    // Called by the game pump; one background attempt per interval, with all encoding and HTTP on the network lane.
    void pump_quest_events() {
        if (!quest_events_enabled_ || !context_.quest_events_registered() || !quest_events_.pending() ||
            terminal_.load(std::memory_order_acquire) || paused_.load(std::memory_order_acquire) ||
            busy_() || quest_event_tasks_.load(std::memory_order_acquire) != 0) return;
        const auto now = tasks::TaskLanes::Clock::now();
        if (now < next_quest_event_attempt_) return;
        next_quest_event_attempt_ = now + std::chrono::seconds{5};
        (void)network_.try_submit("network", tasks::TaskClass::background,
            now + std::chrono::milliseconds{request_timeout_ms()}, current_turn_(),
            tasks::TaskLanes::track_activity(quest_event_tasks_,
            [this](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire) ||
                    paused_.load(std::memory_order_acquire)) return;
                std::shared_ptr<const QuestEventDelivery::Prepared> batch;
                try {
                    batch = quest_events_.prepare();
                    if (!batch) return;
                    const auto context_turn = client_.begin_context_turn();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    const auto result = context_.publish(batch->snapshot, std::nullopt, cancellation, nullptr, batch->batch);
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    if (result.status == RequestStatus::complete) {
                        (void)quest_events_.finish(batch, true);
                        quest_event_failure_reported_.store(false, std::memory_order_release);
                    } else if (result.status == RequestStatus::http_failure && result.http_status >= 400 &&
                               result.http_status < 500 && result.http_status != 408 && result.http_status != 429) {
                        (void)quest_events_.finish(batch, false);
                        notify_("SYNTH quest events rejected; check logs before enabling further capture");
                    } else if (!quest_event_failure_reported_.exchange(true, std::memory_order_acq_rel)) {
                        notify_("SYNTH quest events not acknowledged; retaining capture for retry");
                    }
                } catch (const std::invalid_argument&) {
                    (void)quest_events_.finish(batch, false);
                    notify_("SYNTH quest batch failed validation and was not sent");
                } catch (const std::exception&) {
                    if (!quest_event_failure_reported_.exchange(true, std::memory_order_acq_rel))
                        notify_("SYNTH quest event delivery failed; retaining capture for retry");
                }
            }));
    }

    [[nodiscard]] std::uint64_t dropped_quest_events() const noexcept { return quest_events_.dropped(); }
    [[nodiscard]] std::uint64_t unobserved_quest_events() const noexcept { return quest_events_.unobserved(); }
    [[nodiscard]] bool quest_events_ready() const noexcept {
        return context_.quest_events_registered() && !terminal_.load(std::memory_order_acquire);
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
            terminal_.load(std::memory_order_acquire) || paused_.load(std::memory_order_acquire) ||
            busy_() || actor_event_tasks_.load(std::memory_order_acquire)!=0) return;
        const auto now=tasks::TaskLanes::Clock::now();
        if (now<next_actor_event_attempt_) return;
        next_actor_event_attempt_=now+std::chrono::seconds{5};
        (void)network_.try_submit("network",tasks::TaskClass::background,
            now+std::chrono::milliseconds{request_timeout_ms()},current_turn_(),
            tasks::TaskLanes::track_activity(actor_event_tasks_,[this](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire) ||
                    paused_.load(std::memory_order_acquire)) return;
                std::shared_ptr<const ActorEventDelivery::Prepared> batch;
                try {
                    batch=actor_events_.prepare();if (!batch) return;
                    const auto context_turn=client_.begin_context_turn();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    const auto result=context_.publish(batch->capture->scene,std::nullopt,cancellation,nullptr,std::nullopt,std::nullopt,batch);
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    if (result.status==RequestStatus::complete) {
                        (void)actor_events_.finish(batch,true);actor_event_failure_reported_.store(false,std::memory_order_release);
                    } else if (result.status==RequestStatus::http_failure && result.http_status>=400 &&
                        result.http_status<500 && result.http_status!=408 && result.http_status!=429) {
                        (void)actor_events_.finish(batch,false);
                        notify_("SYNTH actor events rejected; check logs before further capture");
                    } else if (!actor_event_failure_reported_.exchange(true,std::memory_order_acq_rel))
                        notify_("SYNTH actor events not acknowledged; retaining original capture for retry");
                } catch (const std::invalid_argument&) {
                    (void)actor_events_.finish(batch,false);
                    notify_("SYNTH actor batch failed validation and was not sent");
                } catch (const std::exception&) {
                    if (!actor_event_failure_reported_.exchange(true,std::memory_order_acq_rel))
                        notify_("SYNTH actor event delivery failed; retaining original capture for retry");
                }
            }));
    }

    [[nodiscard]] bool observed_dialogue_ready() const noexcept {
        return observed_dialogue_enabled_ && client_.observed_dialogue_ready() && !terminal_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool observed_dialogue_capture_ready() const noexcept { return observed_dialogue_ready() && !dialogue_observations_.full(); }
    [[nodiscard]] std::uint64_t dropped_observed_dialogue() const noexcept { return dialogue_observations_.dropped(); }
    // Game pump only: one serial per first observation of this session, consumed even if capture fails.
    [[nodiscard]] std::uint64_t next_observed_dialogue_serial() noexcept { return ++observed_dialogue_serial_; }
    [[nodiscard]] bool observe_dialogue(DialogueObservationDelivery::Prepared capture) {
        return observed_dialogue_capture_ready() && dialogue_observations_.enqueue(std::move(capture));
    }

    // Same delivery rules as actor events: only a complete exact ACK or a definitive rejection retires the capture.
    void pump_observed_dialogue() {
        if (!observed_dialogue_ready() || !dialogue_observations_.pending() || paused_.load(std::memory_order_acquire) ||
            busy_() || observed_dialogue_tasks_.load(std::memory_order_acquire)!=0) return;
        const auto now=tasks::TaskLanes::Clock::now();
        if (now<next_observed_dialogue_attempt_) return;
        next_observed_dialogue_attempt_=now+std::chrono::seconds{5};
        (void)network_.try_submit("network",tasks::TaskClass::background,
            now+std::chrono::milliseconds{request_timeout_ms()},current_turn_(),
            tasks::TaskLanes::track_activity(observed_dialogue_tasks_,[this](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire) ||
                    paused_.load(std::memory_order_acquire)) return;
                DialogueObservationDelivery::Prepared capture;
                try {
                    capture=dialogue_observations_.prepare();if (!capture) return;
                    const auto context_turn=client_.begin_context_turn();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    const auto result=context_.publish(capture->scene,std::nullopt,cancellation,nullptr,std::nullopt,std::nullopt,
                        {},{},nullptr,capture);
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    if (result.status==RequestStatus::complete) {
                        (void)dialogue_observations_.finish(capture,true);observed_dialogue_failure_reported_.store(false,std::memory_order_release);
                    } else if (result.status==RequestStatus::http_failure && result.http_status>=400 &&
                        result.http_status<500 && result.http_status!=408 && result.http_status!=429) {
                        (void)dialogue_observations_.finish(capture,false);
                        notify_("SYNTH observed dialogue rejected; check logs");
                    } else if (!observed_dialogue_failure_reported_.exchange(true,std::memory_order_acq_rel))
                        notify_("SYNTH observed dialogue not acknowledged; retaining original capture for retry");
                } catch (const std::invalid_argument&) {
                    (void)dialogue_observations_.finish(capture,false);
                    notify_("SYNTH observed dialogue failed validation and was not sent");
                } catch (const std::exception&) {
                    if (!observed_dialogue_failure_reported_.exchange(true,std::memory_order_acq_rel))
                        notify_("SYNTH observed dialogue delivery failed; retaining original capture for retry");
                }
            }));
    }

    // Game-pump-only capture cadence: independent of networking, microphone and dialogue activity.
    [[nodiscard]] bool player_event_sample_due(core::SnapshotClock::time_point now) {
        return !terminal_.load(std::memory_order_acquire) && client_.player_events_ready() && player_events_.due(now);
    }

    // Game-pump-only scalar capture ownership. This never publishes context or starts network/provider work.
    [[nodiscard]] std::optional<core::CombatantIdentity> enemy_awareness_capture_target(
        core::SnapshotClock::time_point now,bool player_in_combat) {
        if (terminal_.load(std::memory_order_acquire) || paused_.load(std::memory_order_acquire) || !client_.enemy_awareness_ready()) return {};
        std::scoped_lock lock{enemy_awareness_mutex_};
        if (terminal_.load(std::memory_order_acquire)) return {};
        return enemy_awareness_sampler_.due(monotonic_milliseconds(now),player_in_combat,session_.generation().value());
    }
    void observe_enemy_awareness(const core::CombatantIdentity& owner,const core::EnemyAwarenessSnapshot& captured,
                                 std::uint64_t generation,core::SnapshotClock::time_point now) {
        if (terminal_.load(std::memory_order_acquire) || generation!=session_.generation().value() || !captured.available) return;
        std::scoped_lock lock{enemy_awareness_mutex_};
        if (terminal_.load(std::memory_order_acquire) || !enemy_awareness_sampler_.owns(owner,generation)) return;
        (void)enemy_awareness_cache_.observe(owner,captured,generation,monotonic_milliseconds(now));
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
            paused_.load(std::memory_order_acquire) || busy_() ||
            player_event_tasks_.load(std::memory_order_acquire) != 0) return false;
        const auto& observed = pending->after;
        if (snapshot->generation() != observed.generation || snapshot->variant() != observed.variant ||
            snapshot->player().form_id() != observed.form_id || snapshot->player().origin_plugin() != observed.origin_plugin ||
            snapshot->player().playthrough_id() != observed.playthrough_id) return false;
        const auto now = core::SnapshotClock::now();
        if (now < next_player_event_attempt_ || now < observed.observed_at) return false;
        next_player_event_attempt_ = now + std::chrono::seconds{5};
        const protocol_native::NativePlayerEvent event{*pending, static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - observed.observed_at).count())};
        player_event_owner_.emplace(current_turn_());
        const auto accepted = network_.try_submit("network", tasks::TaskClass::background,
            now + std::chrono::milliseconds{request_timeout_ms()}, player_event_owner_->token(),
            tasks::TaskLanes::track_activity(player_event_tasks_,
            [this, snapshot=std::move(snapshot), event](const core::CancellationToken& cancellation) {
                try {
                    const auto context_turn = client_.begin_context_turn();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire) || paused_.load(std::memory_order_acquire) ||
                        core::SnapshotClock::now() - snapshot->captured_at() > std::chrono::seconds{2}) return;
                    const auto outcome = context_.publish(snapshot, std::nullopt, cancellation, nullptr, std::nullopt, event);
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    if (outcome.status == RequestStatus::complete) {
                        player_event_receipt_.store(event.transition.serial, std::memory_order_release);
                        player_event_failure_reported_.store(false, std::memory_order_release);
                    } else if (outcome.status == RequestStatus::http_failure && outcome.http_status >= 400 &&
                               outcome.http_status < 500 && outcome.http_status != 408 && outcome.http_status != 429) {
                        player_event_receipt_.store(event.transition.serial | (1ULL << 63), std::memory_order_release);
                        notify_("SYNTH player event rejected; check logs");
                    } else if (!player_event_failure_reported_.exchange(true, std::memory_order_acq_rel))
                        notify_("SYNTH player event not acknowledged; retaining evidence for retry");
                } catch (const std::exception&) {
                    if (!player_event_failure_reported_.exchange(true, std::memory_order_acq_rel))
                        notify_("SYNTH player event delivery failed; retaining evidence for retry");
                }
            }));
        return accepted == tasks::SubmitResult::accepted;
    }

    // Game thread: the oldest acknowledged, recent transition that may still request one comment.
    [[nodiscard]] const core::PlayerEventTransition* player_reaction(core::SnapshotClock::time_point now) {
        return player_events_.reaction(now);
    }
    // Companion opt-in: combat ends belong to the participant-bound remark, never the generic reaction.
    [[nodiscard]] const core::PlayerEventTransition* player_reaction_without_combat_end(core::SnapshotClock::time_point now) {
        return player_events_.reaction(now, false);
    }
    [[nodiscard]] bool consume_player_reaction(std::uint64_t serial) { return player_events_.consume_reaction(serial); }
    // Game thread companion encounter evidence; see PlayerEventSampler::observe_participants.
    void observe_combat_participants(const core::RuntimeSnapshot& scene) {
        if (!terminal_.load(std::memory_order_acquire)) player_events_.observe_participants(scene);
    }
    [[nodiscard]] const core::PlayerEventTransition* latest_combat_end(core::SnapshotClock::time_point now) {
        return player_events_.latest_combat_end(now);
    }
    void consume_combat_ends_through(std::uint64_t serial) { (void)player_events_.consume_combat_ends_through(serial); }
    void discard_combat_reactions() { player_events_.discard_combat_reactions(); }
    [[nodiscard]] bool sampled_player_in_combat() const noexcept { return player_events_.in_combat(); }
    [[nodiscard]] std::size_t player_event_tasks() const noexcept { return player_event_tasks_.load(std::memory_order_acquire); }

    // Call only before worker admission; continuations keep their original immutable enriched snapshot.
    [[nodiscard]] bool enrich_selected_actor(Snapshot& snapshot, const protocol_native::Identity& selected) {
        if (!actor_enricher_) return true; // Engine-free sessions keep their explicitly provided fixture evidence.
        if (!snapshot || snapshot->generation() != session_.generation() || terminal_.load(std::memory_order_acquire)) return false;
        const auto key = actor_key(selected);
        const auto found = std::ranges::find_if(snapshot->actors(), [&](const auto& actor) {
            return actor_key(identity(actor)) == key;
        });
        if (found == snapshot->actors().end()) return false;
        try {
            auto enriched = actor_enricher_(snapshot, found->form_id());
            if (!enriched) { notify_("SYNTH selected actor changed or capture is unavailable; try again"); return false; }
            // Capture is game-thread owned; cache mutation and retirement are mutually exclusive.
            const auto observed=std::ranges::find_if(enriched->actors(),[&](const auto& actor){return actor_key(identity(actor))==key;});
            if (observed!=enriched->actors().end() && observed->enemy_awareness().available) {
                std::scoped_lock lock{enemy_awareness_mutex_};
                if (terminal_.load(std::memory_order_acquire) || enriched->generation()!=session_.generation()) return false;
                const auto now=monotonic_milliseconds(core::SnapshotClock::now());
                enemy_awareness_sampler_.select({observed->form_id(),observed->origin_plugin(),observed->playthrough_id(),observed->name(),observed->race()},enriched->generation().value());
                const auto cached=enemy_awareness_cache_.observe({observed->form_id(),observed->origin_plugin(),observed->playthrough_id(),observed->name(),observed->race()},
                    observed->enemy_awareness(),enriched->generation().value(),now);
                enriched=std::make_shared<const core::RuntimeSnapshot>(enriched->with_actor_details(observed->form_id(),observed->with_enemy_awareness(cached)));
            }
            snapshot = std::move(enriched);
            return true;
        } catch (const std::exception& error) {
            notify_(std::string{"SYNTH selected actor capture: "} + error.what());
            return false;
        }
    }

private:
    [[nodiscard]] static protocol_native::Identity identity(const core::ActorSnapshot& actor) {
        return PlayerDialogue::identity(actor);
    }
    [[nodiscard]] static std::string actor_key(const protocol_native::Identity& actor) {
        return PlayerDialogue::actor_key(actor);
    }
    [[nodiscard]] static std::uint64_t monotonic_milliseconds(core::SnapshotClock::time_point now) noexcept {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    }
    [[nodiscard]] std::uint64_t request_timeout_ms() const noexcept {
        return static_cast<std::uint64_t>(configuration_.behavior.ai_response_timeout_seconds) * 1000ULL;
    }

    const config::Config& configuration_;
    SynthClient& client_;
    tasks::TaskLanes& network_;
    ContextPublication& context_;
    const std::atomic_bool& terminal_;
    const std::atomic_bool& paused_;
    const core::CancellationToken session_;
    Busy busy_;
    CurrentTurn current_turn_;
    Notify notify_;
    ActorEnricher actor_enricher_;
    std::mutex enemy_awareness_mutex_;
    core::EnemyAwarenessCache enemy_awareness_cache_;
    core::EnemyAwarenessSampler enemy_awareness_sampler_;
    const bool quest_events_enabled_;
    QuestEventDelivery& quest_events_;
    const bool actor_events_enabled_;
    ActorEventDelivery actor_events_;
    std::atomic_size_t actor_event_tasks_{};
    std::atomic_bool actor_event_failure_reported_{};
    tasks::TaskLanes::Deadline next_actor_event_attempt_{};
    const bool observed_dialogue_enabled_;
    DialogueObservationDelivery dialogue_observations_;
    std::atomic_size_t observed_dialogue_tasks_{};
    std::atomic_bool observed_dialogue_failure_reported_{};
    tasks::TaskLanes::Deadline next_observed_dialogue_attempt_{};
    std::uint64_t observed_dialogue_serial_{};
    core::PlayerEventSampler player_events_;
    std::optional<core::CancellationSource> player_event_owner_;
    core::SnapshotClock::time_point next_player_event_attempt_{};
    std::atomic_size_t player_event_tasks_{};
    // Serials occupy 53 wire-safe bits; bit 63 atomically marks a terminal rejection instead of an ACK.
    std::atomic_uint64_t player_event_receipt_{};
    std::atomic_bool player_event_failure_reported_{};
    std::atomic_size_t quest_event_tasks_{};
    std::atomic_bool quest_event_failure_reported_{};
    tasks::TaskLanes::Deadline next_quest_event_attempt_{};
};

}  // namespace synth::client
