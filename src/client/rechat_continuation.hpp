#pragma once

#include "client/action_execution.hpp"
#include "client/context_publication.hpp"
#include "client/conversation_holds.hpp"
#include "client/dialogue_requests.hpp"
#include "client/player_dialogue.hpp"
#include "client/rechat_prefetch.hpp"
#include "client/social_task_execution.hpp"
#include "client/speech_delivery.hpp"
#include "client/synth_client.hpp"
#include "config/config.hpp"
#include "core/snapshot.hpp"
#include "diagnostics/conversation_trace.hpp"
#include "integration/prompt_target.hpp"
#include "presentation/dialogue_presentation.hpp"
#include "presentation/scene_admission.hpp"
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

// Owns one session's NPC continuation (rechat): the pending parent, the fresh-scene capture request
// and the speculative prefetched child, the parent completion and delivery gates, the bounded
// fresh-scene retries, the child RPC scheduling, the speculative first-line media request and the
// social execution continuation hooks. RechatPrefetch remains the speculative buffer primitive.
//
// A parent is the last eligible NPC line of a followup-capable response. Only the completed
// response's final currently playing line may promote it to one speculative child; otherwise the
// delivered parent is captured again (scene contract) or queued directly (legacy servers). Child
// lines reach the session line handler only after the exact parent caption is delivered and a
// fresh scene revalidates every copied speaker and action actor without retargeting.
//
// Lock order is action queue, then rechat: the gates below run their rechat check inside
// ActionExecution::while_idle()/try_while_idle(). Otherwise the rechat lock is a leaf: it is never
// held across HTTP, a runtime call, publication, notification or line routing. No engine object is
// touched here; the host captures the fresh scene outside the session and passes a copied snapshot.
//
// The session keeps line routing, the response binding and its rollback and turn replacement; this
// object reaches them only through the route_line and notify callbacks. Retained actors revalidate
// through PlayerDialogue's exact selected-target policy. The session calls the hooks below from its
// pump, line handler, capture entry points, turn replacement, halt and conversation ending.
//
// Lifetimes: every borrowed owner, lane, flag and the trace outlive this object; it is declared after
// all of them. Network tasks queued here capture this object, copied snapshots, lines, bindings and
// the shared prefetch only; the session joins the network lane in its destructor body before any
// member is destroyed, and a rejected, expired or discarded task is destroyed unrun with its response
// guard released once. The speculative media job on the speech lane captures only the copied line and
// shared prefetch, never this object.
class RechatContinuation final {
public:
    using Snapshot = std::shared_ptr<const core::RuntimeSnapshot>;
    using Notify = std::function<void(std::string)>;
    // Routes a child line to the session line handler with allow_followup and its released prefetch.
    using RouteLine = std::function<void(const protocol_native::Line&, const Snapshot&, const core::CancellationToken&,
                                         bool, const std::shared_ptr<RechatPrefetch>&)>;

    struct Dependencies final {
        const config::Config& configuration;
        SynthClient& client;
        tasks::TaskLanes& network;
        DialogueRequests& requests;
        SpeechDelivery& speech;
        ContextPublication& context;
        SocialTaskExecution& social;
        ActionExecution& actions;
        PlayerDialogue& dialogue;
        ConversationHolds& holds;
        diagnostics::ConversationTrace& trace;
        const std::atomic_bool& terminal;
        const std::atomic_bool& paused;
        RouteLine route_line;
        Notify notify;
    };

    explicit RechatContinuation(Dependencies dependencies)
        : configuration_{dependencies.configuration},
          client_{dependencies.client},
          network_{dependencies.network},
          requests_{dependencies.requests},
          speech_{dependencies.speech},
          context_{dependencies.context},
          social_{dependencies.social},
          actions_{dependencies.actions},
          dialogue_{dependencies.dialogue},
          conversation_holds_{dependencies.holds},
          trace_{dependencies.trace},
          terminal_{dependencies.terminal},
          paused_{dependencies.paused},
          route_line_{std::move(dependencies.route_line)},
          notify_{std::move(dependencies.notify)} {
        if (!route_line_ || !notify_)
            throw std::invalid_argument{"rechat continuation requires line and notification sinks"};
    }

    RechatContinuation(const RechatContinuation&) = delete;
    RechatContinuation& operator=(const RechatContinuation&) = delete;

    // Turn replacement and hard halt drop every parent, capture and speculative child.
    void clear() {
        std::scoped_lock lock{mutex_};
        pending_.reset();
        capture_.reset();
        if (prefetched_) prefetched_->prefetch->cancel();
        prefetched_.reset();
    }

    // Ending a conversation drops only that actor's continuation.
    void forget(const std::string& key) {
        std::scoped_lock lock{mutex_};
        if (pending_ && actor_key(pending_->actor) == key) {
            pending_.reset();
        }
        if (capture_ && actor_key(capture_->actor)==key) capture_.reset();
        if (prefetched_ && actor_key(prefetched_->actor)==key) {
            prefetched_->prefetch->cancel();
            prefetched_.reset();
        }
    }

    // Ambient gate: true only while no action is queued and no continuation exists.
    [[nodiscard]] bool idle() {
        return actions_.while_idle([&] {
            std::scoped_lock lock{mutex_};
            return !pending_ && !capture_ && !prefetched_;
        });
    }

    // Nonblocking frame-rate variant; a contended action or rechat lock reads as busy.
    [[nodiscard]] bool try_idle() {
        return actions_.try_while_idle([&] {
            std::unique_lock rechat{mutex_,std::try_to_lock};
            return rechat.owns_lock() && !pending_ && !capture_ && !prefetched_;
        });
    }

    // Combat identification never leaves its own line as a continuation parent.
    void drop_combat_parent(const protocol_native::Line& line) {
        std::scoped_lock lock{mutex_};
        if (pending_ && pending_->parent.request_id==line.request_id &&
            pending_->binding.sequence==line.context_sequence) {
            pending_.reset();
        }
    }

    void complete_parent(const std::string& request_id, bool complete) {
        std::scoped_lock lock{mutex_};
        if (pending_ && pending_->parent.request_id == request_id)
            pending_->response_complete = complete;
    }

    // Called for each followup-capable NPC-identity dialogue line after its caption is queued.
    void observe_speaker(const protocol_native::Line& line, const protocol_native::Identity& actor, const Snapshot& snapshot,
                         const core::CancellationToken& cancellation, const presentation::DialogueCaptions::Caption& caption,
                         ContextBinding binding) {
        if (actor_key(actor) == actor_key(identity(snapshot->player()))) return;
        // A later ineligible NPC line must not leave an earlier line as the continuation parent.
        {
            std::scoped_lock lock{mutex_};
            pending_.reset();
            capture_.reset();
        }
        const auto current_actor = std::ranges::find_if(
            snapshot->actors(), [&](const core::ActorSnapshot& candidate) {
                return actor_key(identity(candidate)) == actor_key(actor);
            });
        if (current_actor != snapshot->actors().end() &&
            targeting::dialogue_actor_available(*current_actor) &&
            !dialogue_.consume_rechat_suppression(actor_key(actor)) &&
            !dialogue_.on_cooldown(actor_key(actor), core::SnapshotClock::now())) {
            std::scoped_lock lock{mutex_};
            pending_ = PendingRechat{actor, snapshot,
                std::move(binding),
                {line.request_id, line.line_id}, caption};
            pending_->target={cancellation,current_actor->form_id(),current_actor->base_form_id(),
                current_actor->origin_plugin(),current_actor->base_origin_plugin(),current_actor->playthrough_id(),
                current_actor->name(),true};
        }
    }

    // Game pump: a speculative child owns the pump until it is captured or invalid. True means return.
    [[nodiscard]] bool pump_prefetched() {
        std::scoped_lock lock{mutex_};
        if (!prefetched_) return false;
        auto& deferred = *prefetched_;
        const auto now = tasks::TaskLanes::Clock::now();
        if (deferred.prefetch->invalid(now)) prefetched_.reset();
        else if (runtime_paused() || dialogue_.voice_active()) return true;
        else if (auto lines = deferred.prefetch->take(now)) {
            deferred.prefetched_lines = std::move(*lines);
            capture_ = std::move(deferred);
            prefetched_.reset();
        }
        return true;
    }

    // Game pump after pause/voice gates. Only the completed response's final currently playing line
    // may authorize one speculative child. True means the parent was promoted and the pump returns.
    [[nodiscard]] bool promote_playing_parent() {
        if (!client_.rechat_scene_ready() || requests_.responses() != 0 ||
            actions_.active() != 0 || speech_.jobs_active() ||
            !speech_.captions_idle()) return false;
        const auto playing = speech_.playback_frame();
        // Lock order is action queue, then rechat; no action may be queued while the parent is promoted.
        return actions_.while_idle([&] {
            std::scoped_lock lock{mutex_};
            if (!capture_ && pending_ && playing &&
                playing->caption == pending_->caption && pending_->response_complete &&
                !pending_->binding.cancellation.is_cancelled() &&
                pending_->caption->delivery->load(std::memory_order_acquire) == presentation::DialogueDelivery::playing) {
                pending_->prefetch = std::make_shared<RechatPrefetch>(pending_->caption,
                    tasks::TaskLanes::Clock::now() + std::chrono::milliseconds{request_timeout_ms() * 2ULL});
                capture_ = std::move(pending_);
                pending_.reset();
                return true;
            }
            return false;
        });
    }

    // Game pump once speech, responses and actions are idle. True when a delivered parent was taken.
    [[nodiscard]] bool pump_delivered_parent(std::uint64_t generation) {
        std::optional<PendingRechat> rechat;
        {
            std::scoped_lock lock{mutex_};
            if (pending_ && (pending_->binding.cancellation.is_cancelled() ||
                !pending_->snapshot || pending_->snapshot->generation().value() != generation ||
                pending_->caption->delivery->load(std::memory_order_acquire) == presentation::DialogueDelivery::discarded)) {
                pending_.reset();
                capture_.reset();
            }
            if (pending_ && pending_->response_complete && pending_->caption->delivered()) {
                rechat = std::move(pending_);
                pending_.reset();
                capture_.reset();
            }
        }
        if (!rechat) return false;
        if (client_.rechat_scene_ready()) {
            std::scoped_lock lock{mutex_};
            capture_=std::move(rechat);
        } else if (!client_.uses_rechat_scene_contract()) queue(std::move(*rechat));
        return true;
    }

    // The host captures outside the session: a game-thread capture can retire the whole session.
    [[nodiscard]] std::optional<integration::PromptTarget> capture_target() {
        std::scoped_lock lock{mutex_};
        if (capture_ && capture_->binding.cancellation.is_cancelled()) {
            if (capture_->prefetch) capture_->prefetch->cancel();
            capture_.reset();
        }
        if (runtime_paused() || dialogue_.voice_active()) return {};
        return capture_ ? std::optional{capture_->target} : std::nullopt;
    }

    void submit_capture(Snapshot snapshot, const integration::PromptTarget& target) {
        std::optional<PendingRechat> rechat;
        {
            std::scoped_lock lock{mutex_};
            rechat=std::move(capture_);
            capture_.reset();
        }
        if (rechat && runtime_paused() && !rechat->binding.cancellation.is_cancelled() &&
            !terminal_.load(std::memory_order_acquire)) {
            std::scoped_lock lock{mutex_};
            capture_=std::move(rechat);
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
            !snapshot->world() || !previous.world() ||
            !presentation::same_presentation_scene(*previous.world(),*snapshot->world()) ||
            actor_key(identity(snapshot->player()))!=actor_key(identity(previous.player())) ||
            target.form_id!=rechat->target.form_id || !retained_target(*snapshot,rechat->target)) {
            trace_.record(diagnostics::ConversationStage::continuation_scene_rejected,rechat->binding.generation,
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
                return retained_target(*snapshot,retained);
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
                requests_.track_response(
                    [this, snapshot = std::move(snapshot), lines = std::move(*rechat->prefetched_lines), prefetch = rechat->prefetch](const core::CancellationToken& cancellation) {
                        if (core::SnapshotClock::now()-snapshot->captured_at()>std::chrono::seconds{2}) { prefetch->cancel(); return; }
                        if (!prefetch->release(core::SnapshotClock::now())) return;
                        for (const auto& line : lines) {
                            if (cancellation.is_cancelled()) return;
                            route_line_(line, snapshot, cancellation, true, prefetch);
                        }
                    }));
            if (accepted != tasks::SubmitResult::accepted) rechat->prefetch->cancel();
            return;
        }
        rechat->snapshot=std::move(snapshot);
        rechat->fresh_scene=true;
        queue(std::move(*rechat));
    }

private:
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

    [[nodiscard]] bool runtime_paused() const noexcept { return paused_.load(std::memory_order_acquire); }

    [[nodiscard]] std::uint64_t request_timeout_ms() const noexcept {
        return static_cast<std::uint64_t>(configuration_.behavior.ai_response_timeout_seconds) * 1000ULL;
    }

    [[nodiscard]] static protocol_native::Identity identity(const core::ActorSnapshot& actor) {
        return PlayerDialogue::identity(actor);
    }

    // True only if the retained actor is still a selectable dialogue target in this snapshot.
    [[nodiscard]] bool retained_target(const core::RuntimeSnapshot& snapshot, const integration::PromptTarget& retained) {
        return dialogue_.selected_target(snapshot, false, &retained).has_value();
    }

    [[nodiscard]] static std::string actor_key(const protocol_native::Identity& actor) {
        return PlayerDialogue::actor_key(actor);
    }

    // Retry only before a child RPC exists. Pause holds ownership; stale captures get two fresh attempts.
    void defer_capture(PendingRechat rechat) {
        std::scoped_lock lock{mutex_};
        if (rechat.binding.cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire) ||
            capture_ || (!runtime_paused() && ++rechat.scene_retries > 2)) {
            if (rechat.prefetch) rechat.prefetch->cancel();
            trace_.record(diagnostics::ConversationStage::continuation_scene_rejected,rechat.binding.generation,
                rechat.binding.sequence,rechat.parent.request_id,rechat.parent.line_id);
            return;
        }
        prefetched_.reset();
        capture_=std::move(rechat);
    }

    void queue(PendingRechat rechat) {
        const auto social_execution=social_.execution_for_request(rechat.parent.request_id);
        const auto prefetch = rechat.prefetch;
        if (rechat.prefetch) {
            rechat.binding.cancellation = rechat.prefetch->token();
            std::scoped_lock lock{mutex_};
            prefetched_ = rechat;
        }
        const auto binding = rechat.binding;
        const auto deadline = tasks::TaskLanes::Clock::now() +
                              std::chrono::milliseconds{request_timeout_ms() * 2ULL};
        const auto accepted = network_.try_submit(
            "network", tasks::TaskClass::current_turn, deadline, binding.cancellation,
            requests_.track_response(
            [this, rechat = std::move(rechat),social_execution](
                const core::CancellationToken& cancellation) mutable {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;

                try {
                    if (rechat.fresh_scene) {
                        if (!client_.rechat_scene_ready()) {
                            if (rechat.prefetch) rechat.prefetch->cancel();
                            return;
                        }
                        if (runtime_paused() || core::SnapshotClock::now()-rechat.snapshot->captured_at()>std::chrono::seconds{2}) {
                            defer_capture(std::move(rechat));
                            return;
                        }
                        // Only this still-live turn may inherit Close; a later UI selection cannot widen its capture.
                        const protocol_native::ControlSelection capture_controls{
                            dialogue_.close_capture(cancellation) ? "CLOSE" : "", 1};
                        const auto publication=context_.publish(rechat.snapshot,rechat.actor,cancellation,
                            nullptr, {}, {}, {}, capture_controls);
                        if (publication.status==RequestStatus::complete && !cancellation.is_cancelled() &&
                            (runtime_paused() || core::SnapshotClock::now()-rechat.snapshot->captured_at()>std::chrono::seconds{2}) &&
                            !terminal_.load(std::memory_order_acquire)) {
                            // No child request exists yet. Recapture on resume, without spending a second round.
                            defer_capture(std::move(rechat));
                            return;
                        }
                        if (publication.status!=RequestStatus::complete || cancellation.is_cancelled() ||
                            runtime_paused() || core::SnapshotClock::now()-rechat.snapshot->captured_at()>std::chrono::seconds{2}) {
                            if (rechat.prefetch) rechat.prefetch->cancel();
                            return;
                        }
                        rechat.parent.scene_context_sequence=publication.context_binding.sequence;
                    }
                    trace_.record(diagnostics::ConversationStage::continuation_dispatch,rechat.binding.generation,
                        rechat.binding.sequence,rechat.parent.request_id,rechat.parent.line_id);
                    const auto outcome = client_.trigger(
                        "rechat", rechat.actor, [this, snapshot = rechat.snapshot, cancellation, prefetch = rechat.prefetch, parent_request=rechat.parent.request_id,social_execution](const protocol_native::Line& line) {
                            social_.child_request(parent_request,line.request_id);
                            if (prefetch) {
                                trace_.record(diagnostics::ConversationStage::prefetch_received,line.generation,line.context_sequence,line.request_id,line.line_id);
                                prefetch->append(line);
                                if (prefetch->claim_media(line)) speech_.prepare_rechat_media(line, prefetch);
                            }
                            else route_line_(line, snapshot, cancellation, true, {});
                            if (const auto* status = std::get_if<protocol_native::Status>(&line.payload);
                                status && status->code == "rechat_complete") {
                                auto stage = diagnostics::ConversationStage::continuation_complete;
                                if (status->detail == "no_candidate") stage = diagnostics::ConversationStage::continuation_no_candidate;
                                else if (status->detail == "private") stage = diagnostics::ConversationStage::continuation_private;
                                else if (status->detail == "budget_exhausted") stage = diagnostics::ConversationStage::continuation_budget_exhausted;
                                else if (status->detail == "ineligible") stage = diagnostics::ConversationStage::continuation_ineligible;
                                trace_.record(stage,line.generation,line.context_sequence,line.request_id,line.line_id);
                                if(!social_execution.empty()&&(status->detail=="no_candidate"||status->detail=="private"||
                                    status->detail=="budget_exhausted"||status->detail=="ineligible"))social_.fail_execution(social_execution);
                            }
                        }, std::nullopt, std::nullopt, rechat.binding, std::nullopt, std::nullopt, std::nullopt,
                        client_.uses_rechat_parent_contract() ? std::optional{rechat.parent} : std::nullopt);
                    if (rechat.prefetch) rechat.prefetch->finish(outcome.status == RequestStatus::complete);
                    if (outcome.status != RequestStatus::complete && outcome.status != RequestStatus::cancelled) {
                        if(!social_execution.empty())social_.fail_execution(social_execution);
                        conversation_holds_.suspend(cancellation);
                        trace_.record(diagnostics::ConversationStage::continuation_failed,rechat.binding.generation,
                            rechat.binding.sequence,rechat.parent.request_id,rechat.parent.line_id);
                        notify_("SYNTH rechat stopped: " + outcome.detail);
                    }
                } catch (const std::exception& error) {
                    if(!social_execution.empty())social_.fail_execution(social_execution);
                    conversation_holds_.suspend(cancellation);
                    notify_(std::string{"SYNTH rechat stopped: "} + error.what());
                    if (rechat.prefetch) rechat.prefetch->cancel();
                }
            }));
        if (accepted != tasks::SubmitResult::accepted) {
            trace_.record(diagnostics::ConversationStage::continuation_queue_full,binding.generation,binding.sequence);
            notify_("SYNTH rechat stopped because the network queue is full");
            if (prefetch) prefetch->cancel();
        }
    }

    const config::Config& configuration_;
    SynthClient& client_;
    tasks::TaskLanes& network_;
    DialogueRequests& requests_;
    SpeechDelivery& speech_;
    ContextPublication& context_;
    SocialTaskExecution& social_;
    ActionExecution& actions_;
    PlayerDialogue& dialogue_;
    ConversationHolds& conversation_holds_;
    diagnostics::ConversationTrace& trace_;
    const std::atomic_bool& terminal_;
    const std::atomic_bool& paused_;
    RouteLine route_line_;
    Notify notify_;
    std::mutex mutex_;
    std::optional<PendingRechat> pending_;
    std::optional<PendingRechat> capture_;
    std::optional<PendingRechat> prefetched_;
};

} // namespace synth::client
