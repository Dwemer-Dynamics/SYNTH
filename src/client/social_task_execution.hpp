#pragma once

#include "client/context_publication.hpp"
#include "client/dialogue_requests.hpp"
#include "client/player_dialogue.hpp"
#include "client/social_task_delivery.hpp"
#include "client/synth_client.hpp"
#include "config/config.hpp"
#include "core/snapshot.hpp"
#include "runtime/fallout_runtime.hpp"
#include "runtime/social_task.hpp"
#include "tasks/task_lanes.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace synth::client {

// Owns one session's saved social tasks: the SocialTaskController and SocialTaskDelivery state, the
// per-execution and per-exchange cancellation sources, the pending progress and dispatched native
// visit commands, the progress/checkpoint task counts and serials, the save checkpoint frontier and
// its notice, and the visit, progress, checkpoint, release and hydrate workflows.
//
// The walk/visit and the recipient exchange have independent owners. The execution source is a child
// of the session token and lives for one execution; the dialogue source is a child of the progress
// task that reported arrival and owns only the exchange's response. Player turns interrupt only an
// exchange (interrupt_dialogue); a walk survives unrelated input and stops on explicit Stop, failure,
// an action for its performer, an authoritative terminal status or session retirement.
//
// Native visits run only on the game thread through the selected IFalloutRuntime in pump() and
// release_for_action(), never under the social lock. The social lock is a leaf: it is never held
// across a runtime call, publication, HTTP, notification or line routing. Progress and checkpoint
// HTTP run on the session's bounded network lane; there are no other threads.
//
// The session keeps line routing, rechat, turn replacement and cross-feature admission. It reaches
// them only through the player_activity, route_line and notify callbacks, and calls the delivery
// hooks below from its line handler and rechat workflow.
//
// Lifetimes: the borrowed configuration, client, network lane, response accounting, context
// publication and terminal flag outlive this object; it is declared after all of them. Network tasks
// queued here, their line callbacks and the invalidate_response closure placed in each progress
// ContextBinding capture this object and copied snapshots/commands only; the session joins the
// network lane in its destructor body before any member is destroyed, and a rejected, expired or
// discarded task is destroyed unrun with its counter guards released once.
class SocialTaskExecution final {
public:
    using Snapshot = std::shared_ptr<const core::RuntimeSnapshot>;
    using Notify = std::function<void(std::string)>;
    // Player input, menu, composing, action or speech activity, excluding outstanding responses.
    using PlayerActivity = std::function<bool()>;
    // Routes a validated progress line; the bool is the line handler's allow_followup flag.
    using RouteLine = std::function<void(const protocol_native::Line&, const Snapshot&, const core::CancellationToken&, bool)>;

    struct Dependencies final {
        const config::Config& configuration;
        SynthClient& client;
        tasks::TaskLanes& network;
        DialogueRequests& requests;
        ContextPublication& context;
        const std::atomic_bool& terminal;
        core::CancellationToken session;
        PlayerActivity player_activity;
        RouteLine route_line;
        Notify notify;
    };

    explicit SocialTaskExecution(Dependencies dependencies)
        : configuration_{dependencies.configuration},
          client_{dependencies.client},
          network_{dependencies.network},
          requests_{dependencies.requests},
          context_{dependencies.context},
          terminal_{dependencies.terminal},
          session_{std::move(dependencies.session)},
          player_activity_{std::move(dependencies.player_activity)},
          route_line_{std::move(dependencies.route_line)},
          notify_{std::move(dependencies.notify)} {
        if (!player_activity_ || !route_line_ || !notify_)
            throw std::invalid_argument{"social task execution requires activity, line and notification sinks"};
        tasks_.bind_session(client_.runtime_session_id());
    }

    SocialTaskExecution(const SocialTaskExecution&) = delete;
    SocialTaskExecution& operator=(const SocialTaskExecution&) = delete;

    // Session retirement drops every task and exchange; the coordinator releases native visits.
    void reset() {
        std::scoped_lock lock{mutex_};
        tasks_.reset();delivery_.clear();pending_progress_.reset();
    }

    [[nodiscard]] bool capture_due(core::SnapshotClock::time_point now) {
        if(!client_.social_tasks_ready()||terminal_.load(std::memory_order_acquire)||now<next_capture_)return false;
        std::unique_lock lock{mutex_,std::try_to_lock};
        if(!lock.owns_lock()||(!checkpoint_needed_&&!tasks_.active_task()&&std::ranges::none_of(tasks_.reminders(),[](const auto& task) {
            return task.status!="completed"&&task.status!="failed"&&task.status!="cancelled";
        })))return false;
        next_capture_=now+std::chrono::milliseconds{500};return true;
    }

    // Fresh copied state enters here on the game thread. Physical visit polling is
    // nonblocking; context publication and progress HTTP stay on the background lane.
    void pump(Snapshot snapshot, runtime::IFalloutRuntime& runtime) {
        runtime.assert_game_thread();
        const auto now=core::SnapshotClock::now();
        if(!snapshot||snapshot->variant()!=core::RuntimeVariant::flat||!client_.social_tasks_ready()||
            terminal_.load(std::memory_order_acquire)||snapshot->generation()!=session_.generation()||
            runtime.generation()!=snapshot->generation()||now<snapshot->captured_at()||
            now-snapshot->captured_at()>std::chrono::seconds{2})return;
        if(now<next_pump_)return;
        next_pump_=now+std::chrono::milliseconds{250};
        const bool busy=player_busy();
        bool checkpoint{};
        {
            std::scoped_lock lock{mutex_};
            checkpoint=checkpoint_needed_&&!release_requested_&&!dialogue_owner_;
        }
        if(checkpoint) {
            if(!busy&&progress_tasks_.load(std::memory_order_acquire)==0&&checkpoint_tasks_.load(std::memory_order_acquire)==0)
                queue_checkpoint(std::move(snapshot));
            return;
        }
        runtime::SocialTaskFrame frame;
        frame.generation=snapshot->generation().value();
        frame.player={snapshot->player().form_id(),snapshot->player().origin_plugin(),snapshot->player().playthrough_id()};
        if(snapshot->world()&&snapshot->world()->location_ref)
            frame.location=runtime::SocialLocationRef{snapshot->world()->location_ref->form_id,snapshot->world()->location_ref->origin_plugin};
        for(const auto& actor:snapshot->actors())frame.actors.push_back({
            {actor.form_id(),actor.origin_plugin(),actor.playthrough_id()},actor.position(),true,actor.alive(),actor.disabled(),
            actor.in_combat(),actor.hostile_to_player(),actor.teammate(),actor.conversation_area_available()});
        std::optional<runtime::SocialTaskCommand> native;
        core::CancellationToken owner;
        {
            std::unique_lock lock{mutex_,std::try_to_lock};
            if(!lock.owns_lock())return;
            if(const auto proof=delivery_.delivered_proof())
                tasks_.note_spoken(proof->task_id,proof->execution_id,proof->request_id,proof->line_id);
            if(pending_progress_&&now>=pending_progress_->deadline&&
                pending_progress_->kind!=runtime::SocialTaskCommandKind::failed&&
                pending_progress_->kind!=runtime::SocialTaskCommandKind::cancelled) {
                if(pending_progress_->kind==runtime::SocialTaskCommandKind::claim) {
                    tasks_.claim_failed();clear_execution_locked();
                } else {
                    tasks_.abort();pending_progress_.reset();release_requested_=true;++progress_serial_;
                }
            }
            // Explicit Stop/failed visits drain even while player input/audio is busy.
            frame.paused=busy&&!release_requested_;
            if(!pending_progress_) {
                if(auto command=tasks_.poll(frame,now)) {
                    if(!execution_owner_||execution_id_!=command->execution_id) {
                        execution_owner_.emplace(session_);
                        execution_id_=command->execution_id;
                    }
                    if(command->kind==runtime::SocialTaskCommandKind::visit||command->kind==runtime::SocialTaskCommandKind::release) {
                        native=*command;native_command_=*command;owner=execution_owner_->token();
                    } else pending_progress_=std::move(command);
                }
            }
            if(!tasks_.active_task()&&!pending_progress_&&!native_command_&&
                progress_tasks_.load(std::memory_order_acquire)==0&&execution_owner_)clear_execution_locked();
        }
        if(native) {
            const auto result=runtime.social_visit({native->task.task_id,native->execution_id,native->task.performer,
                native->task.recipient,owner,native->deadline,native->kind==runtime::SocialTaskCommandKind::release});
            std::scoped_lock lock{mutex_};
            const auto feedback=static_cast<runtime::SocialTaskVisitFeedback>(result.status);
            tasks_.visit_feedback(result.task_id,result.execution_id,feedback);
            if(result.status==runtime::RuntimeSocialVisitStatus::failed||result.status==runtime::RuntimeSocialVisitStatus::unavailable)
                release_requested_=true;
            if(result.status==runtime::RuntimeSocialVisitStatus::released) {
                native_command_.reset();
                if(!tasks_.active_task())clear_execution_locked();
            }
        }
        if(!busy&&progress_tasks_.load(std::memory_order_acquire)==0)queue_progress(std::move(snapshot));
    }

    // Game thread, from the action queue before it dispatches an action for `actor`. Stops a task
    // whose exact performer matches and releases its dispatched native visit outside the lock;
    // false keeps the action queued until the adapter reports the visit released.
    [[nodiscard]] bool release_for_action(const protocol_native::Identity& actor,runtime::IFalloutRuntime& runtime) {
        std::optional<runtime::SocialTaskCommand> command;core::CancellationToken token;
        {
            std::scoped_lock lock{mutex_};
            const auto* active=tasks_.active_task();
            if((!active||!social_wire_actor_matches(actor,active->performer))&&
                (!native_command_||!social_wire_actor_matches(actor,native_command_->task.performer)))return true;
            if(!release_requested_) {
                tasks_.stop();release_requested_=true;
                if(dialogue_owner_)dialogue_owner_->cancel();
                delivery_.clear();pending_progress_.reset();++progress_serial_;
            }
            if(!native_command_)return true; // No native visit has been dispatched.
            command=native_command_;token=execution_owner_->token();
        }
        const auto result=runtime.social_visit({command->task.task_id,command->execution_id,command->task.performer,
            command->task.recipient,token,command->deadline,true});
        std::scoped_lock lock{mutex_};
        tasks_.visit_feedback(result.task_id,result.execution_id,static_cast<runtime::SocialTaskVisitFeedback>(result.status));
        if(result.status!=runtime::RuntimeSocialVisitStatus::released)return false;
        native_command_.reset();
        if(!tasks_.active_task())clear_execution_locked();
        return true;
    }

    // Player request outcome hook; true when a rejection notice was already shown.
    [[nodiscard]] bool notify_input(const RequestOutcome& outcome) {
        if(outcome.status!=RequestStatus::complete&&outcome.status!=RequestStatus::server_failure)return false;
        for(const auto& line:outcome.lines)if(const auto* status=std::get_if<protocol_native::Status>(&line.payload)) {
            if(status->code=="social_task_accepted"&&outcome.status==RequestStatus::complete) {
                std::string message="SYNTH saved conversation task";
                for(const auto& item:outcome.lines)if(const auto* offer=std::get_if<protocol_native::SocialTaskOffer>(&item.payload)) {
                    if(offer->task.status=="waiting_departure")message+="; waiting until you leave and return";
                    else if(offer->task.status=="waiting_return")message+="; waiting for your return";
                }
                {std::scoped_lock lock{mutex_};checkpoint_notice_=std::move(message);}
                notify_("SYNTH conversation task accepted; recording it for the next save");return false;
            }
            if(status->code=="social_task_rejected") {
                std::string reason="the task could not be saved";
                if(status->detail=="social_performer_not_follower")reason="select one of your current followers";
                else if(status->detail=="social_performer_not_selected")reason="address the follower who should make the visit";
                else if(status->detail=="social_recipient_ambiguous")reason="the recipient name matches more than one actor";
                else if(status->detail=="social_recipient_unknown"||status->detail=="social_recipient_not_available")reason="the recipient is not available nearby";
                else if(status->detail=="social_destination_ambiguous"||status->detail=="social_destination_unobserved")reason="the destination is not identified in this scene";
                else if(status->detail=="social_task_capacity")reason="the saved task limit has been reached";
                notify_("SYNTH conversation task was not saved: "+reason);return true;
            }
        }
        return false;
    }

    // Local Stop (failed=false) or failure stops the active task and its exchange; the walk then releases.
    void stop(bool failed) {
        std::scoped_lock lock{mutex_};
        if(!tasks_.active_task())return;
        if(failed)tasks_.abort();else tasks_.stop();
        if(dialogue_owner_)dialogue_owner_->cancel();
        delivery_.clear();pending_progress_.reset();release_requested_=true;++progress_serial_;
    }

    // A walk survives unrelated player input. An already-started exchange is
    // interrupted locally and releases its visit instead of presenting late speech.
    void interrupt_dialogue() {
        bool interrupt{};
        {std::scoped_lock lock{mutex_};interrupt=dialogue_owner_.has_value();}
        if(interrupt)stop(false);
    }

    // Late stream failure: true when `cancellation` owned the exchange, whose execution then fails.
    [[nodiscard]] bool invalidate_dialogue(const core::CancellationToken& cancellation) {
        std::string execution;
        {
            std::scoped_lock lock{mutex_};
            if(!dialogue_owner_||!dialogue_owner_->cancel_if_owner(cancellation))return false;
            execution=execution_id_;
        }
        fail_execution(execution);
        return true;
    }

    // Fails only the exact current execution; a replacement execution is untouched.
    void fail_execution(std::string_view execution) {
        std::scoped_lock lock{mutex_};
        if(execution_id_!=execution||!tasks_.active_task())return;
        tasks_.abort();release_requested_=true;
        if(dialogue_owner_)dialogue_owner_->cancel();
        pending_progress_.reset();delivery_.clear();++progress_serial_;
    }

    // Authoritative offer line from a current-generation response.
    void accept_offer(const runtime::SocialTaskDescriptor& task,const Snapshot& snapshot) {
        if(!client_.social_tasks_ready()||!snapshot||snapshot->generation()!=session_.generation())return;
        std::scoped_lock lock{mutex_};
        const auto known=checkpoint_revisions_.find(task.task_id);
        if(known==checkpoint_revisions_.end()||known->second<task.revision) {
            if(known==checkpoint_revisions_.end()&&checkpoint_revisions_.size()>=16)checkpoint_revisions_.erase(checkpoint_revisions_.begin());
            checkpoint_revisions_[task.task_id]=task.revision;checkpoint_needed_=true;++checkpoint_serial_;
        }
        if(const auto* active=tasks_.active_task();active&&active->task_id==task.task_id&&
            (task.status=="completed"||task.status=="cancelled"||task.status=="failed"))release_requested_=true;
        tasks_.upsert_authoritative(task,snapshot->generation().value(),
            {snapshot->player().form_id(),snapshot->player().origin_plugin(),snapshot->player().playthrough_id()});
    }

    // Context hook, on the publishing worker after a fresh, current, complete publication.
    void hydrate(const RequestOutcome& outcome, const core::RuntimeSnapshot& snapshot) {
        std::vector<runtime::SocialTaskDescriptor> tasks;
        for(const auto& line:outcome.lines)if(const auto* offer=std::get_if<protocol_native::SocialTaskOffer>(&line.payload))tasks.push_back(offer->task);
        std::scoped_lock lock{mutex_};
        if(const auto* active=tasks_.active_task();active&&std::ranges::none_of(tasks,[&](const auto& task) {
            return task.task_id==active->task_id&&task.status!="completed"&&task.status!="failed"&&task.status!="cancelled";
        }))release_requested_=true;
        tasks_.hydrate(std::move(tasks),snapshot.generation().value(),
            {snapshot.player().form_id(),snapshot.player().origin_plugin(),snapshot.player().playthrough_id()});
    }

    // Delivery hooks for the session's line handler and rechat workflow.
    void complete_request(std::string_view request) {std::scoped_lock lock{mutex_};delivery_.complete_request(request);}
    [[nodiscard]] bool track(const protocol_native::Line& line,const presentation::DialogueCaptions::Caption& caption) {
        std::scoped_lock lock{mutex_};return delivery_.track(line,caption);
    }
    [[nodiscard]] bool owns_request(std::string_view request) {std::scoped_lock lock{mutex_};return delivery_.owns_request(request);}
    // The execution owning a rechat parent request, or empty when the parent is not social.
    [[nodiscard]] std::string execution_for_request(std::string_view request) {
        std::scoped_lock lock{mutex_};
        return delivery_.owns_request(request) ? execution_id_ : std::string{};
    }
    void child_request(std::string_view parent,std::string_view request) {std::scoped_lock lock{mutex_};delivery_.child_request(parent,request);}

private:
    [[nodiscard]] bool player_busy(bool owns_rpc=false) {
        return player_activity_()||
            requests_.responses()>(owns_rpc ? progress_tasks_.load(std::memory_order_acquire) : 0);
    }

    [[nodiscard]] std::uint64_t request_timeout_ms() const noexcept {
        return static_cast<std::uint64_t>(configuration_.behavior.ai_response_timeout_seconds) * 1000ULL;
    }

    [[nodiscard]] static protocol_native::Identity identity(const core::ActorSnapshot& actor) {
        return PlayerDialogue::identity(actor);
    }

    void queue_checkpoint(Snapshot snapshot) {
        std::uint64_t serial{};
        {std::scoped_lock lock{mutex_};serial=checkpoint_serial_;}
        (void)network_.try_submit("network",tasks::TaskClass::background,
            core::SnapshotClock::now()+std::chrono::milliseconds{request_timeout_ms()},session_,
            tasks::TaskLanes::track_activity(checkpoint_tasks_,context_.track_observation(
            [this,snapshot=std::move(snapshot),serial](const core::CancellationToken& cancellation) {
                if(cancellation.is_cancelled()||player_busy())return;
                try {
                    const auto transaction=client_.begin_context_turn();
                    if(cancellation.is_cancelled()||player_busy()||core::SnapshotClock::now()-snapshot->captured_at()>std::chrono::seconds{2})return;
                    {
                        std::scoped_lock lock{mutex_};
                        if(!checkpoint_needed_||dialogue_owner_||progress_tasks_.load(std::memory_order_acquire)!=0)return;
                    }
                    const auto outcome=context_.publish(snapshot,std::nullopt,cancellation);
                    if(outcome.status!=RequestStatus::complete||(context_.save_bound()&&!outcome.save_checkpoint_acknowledged)||
                        cancellation.is_cancelled()||terminal_.load(std::memory_order_acquire))return;
                    std::optional<std::string> notice;
                    {
                        std::scoped_lock lock{mutex_};
                        if(serial!=checkpoint_serial_)return;
                        checkpoint_needed_=false;notice=std::move(checkpoint_notice_);checkpoint_notice_.reset();
                    }
                    if(notice)notify_(std::move(*notice));
                } catch(const std::exception&) {} // The pending frontier is retried on a fresh capture.
            })));
    }

    void clear_execution_locked() {
        if(execution_owner_)execution_owner_->cancel();
        if(dialogue_owner_)dialogue_owner_->cancel();
        execution_owner_.reset();dialogue_owner_.reset();execution_id_.clear();
        delivery_.clear();pending_progress_.reset();release_requested_=false;++progress_serial_;
    }

    void fail_progress(const runtime::SocialTaskCommand& command) {
        if(command.kind==runtime::SocialTaskCommandKind::claim) {
            std::scoped_lock lock{mutex_};
            if(execution_id_==command.execution_id) {tasks_.claim_failed();clear_execution_locked();}
        } else fail_execution(command.execution_id);
    }

    void queue_progress(Snapshot snapshot) {
        runtime::SocialTaskCommand command;core::CancellationToken owner;std::uint64_t serial{};
        {
            std::scoped_lock lock{mutex_};
            if(!pending_progress_||!execution_owner_)return;
            command=*pending_progress_;owner=execution_owner_->token();serial=progress_serial_;
        }
        const auto rpc_deadline=core::SnapshotClock::now()+std::chrono::milliseconds{request_timeout_ms()};
        const bool cleanup_report=command.kind==runtime::SocialTaskCommandKind::failed||command.kind==runtime::SocialTaskCommandKind::cancelled;
        const auto accepted=network_.try_submit("network",tasks::TaskClass::background,
            cleanup_report ? rpc_deadline : std::min(command.deadline,rpc_deadline),owner,
            tasks::TaskLanes::track_activity(progress_tasks_,requests_.track_response(
            [this,snapshot=std::move(snapshot),command,serial](const core::CancellationToken& cancellation) {
                if(cancellation.is_cancelled()||terminal_.load(std::memory_order_acquire)||player_busy(true))return;
                {
                    std::scoped_lock lock{mutex_};
                    if(serial!=progress_serial_||!pending_progress_||execution_id_!=command.execution_id)return;
                    pending_progress_.reset();
                }
                try {
                    const auto transaction=client_.begin_context_turn();
                    if(cancellation.is_cancelled()||player_busy(true)||
                        core::SnapshotClock::now()-snapshot->captured_at()>std::chrono::seconds{2}) {
                        std::scoped_lock lock{mutex_};
                        if(serial==progress_serial_)pending_progress_=command;
                        return;
                    }
                    core::CancellationToken response_owner=cancellation;
                    if(command.kind==runtime::SocialTaskCommandKind::arrived) {
                        std::scoped_lock lock{mutex_};
                        if(serial!=progress_serial_)return;
                        dialogue_owner_.emplace(cancellation);
                        response_owner=dialogue_owner_->token();
                        delivery_.begin(command,snapshot->generation().value());
                    }
                    char form[11]{};std::snprintf(form,sizeof(form),"0x%08X",command.task.performer.form_id);
                    protocol_native::Identity actor{form,command.task.performer.origin_plugin,
                        command.task.performer.playthrough_id,command.task.performer_name};
                    for(const auto& captured:snapshot->actors())if(social_wire_actor_matches(identity(captured),command.task.performer))actor=identity(captured);
                    const bool cleanup=command.kind==runtime::SocialTaskCommandKind::failed||command.kind==runtime::SocialTaskCommandKind::cancelled;
                    auto publication=cleanup ? context_.publish(snapshot,std::nullopt,response_owner)
                        : context_.publish(snapshot,actor,response_owner,nullptr,{},{},{},{},&command.task);
                    if(publication.status!=RequestStatus::complete||response_owner.is_cancelled()) {
                        fail_progress(command);return;
                    }
                    auto request_command=command;
                    if(command.kind==runtime::SocialTaskCommandKind::failed||command.kind==runtime::SocialTaskCommandKind::cancelled) {
                        // Stop can overtake a claim/arrival ACK. Cleanup uses the
                        // fresh authoritative revision for this exact execution,
                        // never a stale optimistic revision or another visit owner.
                        for(const auto& line:publication.lines)if(const auto* offer=std::get_if<protocol_native::SocialTaskOffer>(&line.payload);
                            offer&&offer->task.task_id==command.task.task_id&&
                            runtime::social_same_actor(offer->task.performer,command.task.performer)&&
                            runtime::social_same_actor(offer->task.recipient,command.task.recipient)&&
                            (!offer->task.execution_id||*offer->task.execution_id==command.execution_id))request_command.task=offer->task;
                    }
                    publication.context_binding.invalidate_response=[this,execution=command.execution_id] {fail_execution(execution);};
                    const auto outcome=client_.social_task_progress(request_command,
                        [this,snapshot,response_owner,command,serial](const protocol_native::Line& line) {
                            if(response_owner.is_cancelled())return;
                            {
                                std::scoped_lock lock{mutex_};
                                if(serial!=progress_serial_)return;
                                if(command.kind==runtime::SocialTaskCommandKind::arrived)delivery_.root_request(line.request_id);
                            }
                            route_line_(line,snapshot,response_owner,command.kind==runtime::SocialTaskCommandKind::arrived);
                        },publication.context_binding);
                    if(outcome.status!=RequestStatus::complete) {
                        fail_progress(command);
                        if(outcome.status!=RequestStatus::cancelled)notify_("SYNTH saved visit stopped; progress was not acknowledged");
                    } else if(command.kind==runtime::SocialTaskCommandKind::failed||command.kind==runtime::SocialTaskCommandKind::cancelled) {
                        std::scoped_lock lock{mutex_};
                        if(execution_id_==command.execution_id&&!native_command_)clear_execution_locked();
                    }
                } catch(const std::exception&) {
                    fail_progress(command);
                    notify_("SYNTH saved visit stopped; progress could not be saved");
                }
            })));
        (void)accepted; // A full lane retains the pending command for a later bounded pump.
    }

    const config::Config& configuration_;
    SynthClient& client_;
    tasks::TaskLanes& network_;
    DialogueRequests& requests_;
    ContextPublication& context_;
    const std::atomic_bool& terminal_;
    const core::CancellationToken session_;
    PlayerActivity player_activity_;
    RouteLine route_line_;
    Notify notify_;
    std::mutex mutex_;
    runtime::SocialTaskController tasks_;
    SocialTaskDelivery delivery_;
    std::optional<core::CancellationSource> execution_owner_,dialogue_owner_;
    std::string execution_id_;
    std::optional<runtime::SocialTaskCommand> pending_progress_,native_command_;
    std::atomic_size_t progress_tasks_{};
    std::atomic_size_t checkpoint_tasks_{};
    bool checkpoint_needed_{};
    std::uint64_t checkpoint_serial_{};
    std::unordered_map<std::string,std::uint64_t> checkpoint_revisions_;
    std::optional<std::string> checkpoint_notice_;
    std::uint64_t progress_serial_{};
    bool release_requested_{};
    core::SnapshotClock::time_point next_pump_{};
    core::SnapshotClock::time_point next_capture_{};
};

}  // namespace synth::client
