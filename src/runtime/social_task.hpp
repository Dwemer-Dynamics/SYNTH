#pragma once
#include "runtime/fallout_runtime.hpp"
#include "json/json.hpp"
#include <cmath>
#include <optional>
#include <span>

namespace synth::runtime {
inline bool social_plugin_equal(std::string_view a,std::string_view b) noexcept {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](unsigned char x,unsigned char y) {
        const auto fold=[](unsigned char c){return c>='A'&&c<='Z'?static_cast<unsigned char>(c-'A'+'a'):c;};
        return fold(x)==fold(y);
    });
}
struct SocialLocationRef final {
    std::uint32_t form_id{};std::string origin_plugin;
    bool operator==(const SocialLocationRef& other) const noexcept {return form_id==other.form_id&&social_plugin_equal(origin_plugin,other.origin_plugin);}
};
struct SocialTaskDescriptor final {
    std::string task_id;
    std::uint64_t revision{};
    RuntimeActorIdentity performer,recipient;
    std::string topic,when,location_name,status;
    std::optional<SocialLocationRef> location_ref;
    std::optional<std::string> execution_id;
    std::string performer_name,recipient_name;
};
inline bool social_id(std::string_view text,std::size_t limit=128) noexcept {
    return !text.empty() && text.size()<=limit && ((text.front()>='a'&&text.front()<='z')||(text.front()>='A'&&text.front()<='Z')||(text.front()>='0'&&text.front()<='9')) && std::ranges::all_of(text,[](unsigned char c) {
        return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c==':'||c=='-';
    });
}
inline bool social_same_actor(const RuntimeActorIdentity& a,const RuntimeActorIdentity& b) noexcept {
    return a.form_id==b.form_id && social_plugin_equal(a.origin_plugin,b.origin_plugin) && a.playthrough_id==b.playthrough_id;
}
inline bool social_actor_valid(const RuntimeActorIdentity& actor) noexcept {
    return actor.form_id && !actor.origin_plugin.empty() && actor.origin_plugin.size()<=255 &&
        actor.origin_plugin.find_first_of("/\\") == std::string::npos && actor.origin_plugin.find('\0')==std::string::npos && json::detail::valid_utf8(actor.origin_plugin) &&
        (actor.origin_plugin.ends_with(".esm")||actor.origin_plugin.ends_with(".esp")||actor.origin_plugin.ends_with(".esl")) && social_id(actor.playthrough_id);
}
inline bool social_task_valid(const SocialTaskDescriptor& task) noexcept {
    const std::array statuses{"waiting_departure","waiting_return","ready","travelling","arrived","conversing","completed","cancelled","failed"};
    return task.task_id.starts_with("social:") && task.task_id.size()==39 && std::ranges::all_of(std::string_view{task.task_id}.substr(7),[](unsigned char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}) &&
        task.revision>0 && task.revision<=9'007'199'254'740'991ULL && social_actor_valid(task.performer) && social_actor_valid(task.recipient) &&
        task.performer.form_id!=0x14 && task.recipient.form_id!=0x14 && !social_same_actor(task.performer,task.recipient) &&
        task.performer.playthrough_id==task.recipient.playthrough_id && !task.topic.empty() && task.topic.size()<=512 &&
        json::detail::valid_utf8(task.topic) && task.topic.find('\0')==std::string::npos && task.location_name.size()<=128 && json::detail::valid_utf8(task.location_name) && task.location_name.find('\0')==std::string::npos &&
        (task.when=="now"||task.when=="on_return") && std::ranges::find(statuses,task.status)!=statuses.end() &&
        (!task.execution_id||social_id(*task.execution_id)) && (task.when!="on_return"||
            (task.location_ref && social_actor_valid({task.location_ref->form_id,task.location_ref->origin_plugin,"reference"})));
}
struct SocialTaskActorState final {
    RuntimeActorIdentity actor;core::Vec3 position;
    bool loaded{},alive{},disabled{},in_combat{},hostile{},native_follower{},same_area{};
};
struct SocialTaskFrame final {
    std::uint64_t generation{};RuntimeActorIdentity player;std::optional<SocialLocationRef> location;
    std::vector<SocialTaskActorState> actors;bool paused{};
};
enum class SocialTaskCommandKind : unsigned char {claim,visit,release,arrived,spoken,failed,cancelled};
struct SocialTaskCommand final {
    SocialTaskCommandKind kind{};SocialTaskDescriptor task;std::string execution_id;
    core::SnapshotClock::time_point deadline;
    std::string proof_request_id,proof_line_id;
};
enum class SocialTaskVisitFeedback : unsigned char {pending,arrived,released,unavailable,failed};

// Server hydration is authoritative. Restored reminders are display hints and never enter executable state.
class SocialTaskController final {
    enum class Phase : unsigned char {claim_wait,visiting,arrival_report,arrival_wait,exchange,spoken_report,spoken_wait,releasing,terminal_report};
    struct Active {SocialTaskDescriptor task;std::string execution;core::SnapshotClock::time_point deadline;Phase phase{Phase::claim_wait};
        std::optional<SocialTaskCommandKind> after_release;std::string proof_request,proof_line;bool visit_dispatched{};};
    std::vector<SocialTaskDescriptor> tasks_,reminders_;
    std::vector<std::pair<std::string,std::uint64_t>> denied_;
    std::optional<Active> active_;
    std::uint64_t generation_{},serial_{};RuntimeActorIdentity player_;std::string session_prefix_;
    static bool terminal(std::string_view status) noexcept {return status=="completed"||status=="failed"||status=="cancelled";}
    static bool pair_ready(const SocialTaskFrame& frame,const SocialTaskDescriptor& task) {
        const SocialTaskActorState* performer{};const SocialTaskActorState* recipient{};
        for (const auto& value:frame.actors) {
            if (social_same_actor(value.actor,task.performer)) {if(performer)return false;performer=&value;}
            if (social_same_actor(value.actor,task.recipient)) {if(recipient)return false;recipient=&value;}
        }
        for (const auto* actor:{performer,recipient}) if (!actor||!actor->loaded||!actor->alive||actor->disabled||actor->in_combat||actor->hostile||!actor->same_area||!actor->position.finite()) return false;
        if (!performer->native_follower) return false;
        const auto d=performer->position, t=recipient->position;
        return (d.x-t.x)*(d.x-t.x)+(d.y-t.y)*(d.y-t.y)+(d.z-t.z)*(d.z-t.z)<=2000.0*2000.0;
    }
public:
    void reset() noexcept {tasks_.clear();denied_.clear();active_.reset();generation_=0;player_={};}
    void bind_session(std::string session) {
        if(!social_id(session,80))throw std::invalid_argument{"invalid social runtime session"};
        if(session_prefix_!=session) {reset();session_prefix_=std::move(session);serial_=0;}
    }
    void restore_reminders(std::vector<SocialTaskDescriptor> reminders) {
        if(reminders.size()>8||!std::ranges::all_of(reminders,social_task_valid))throw std::invalid_argument{"invalid social reminders"};
        reminders_=std::move(reminders);
    }
    [[nodiscard]] const std::vector<SocialTaskDescriptor>& reminders() const noexcept {return reminders_;}
    [[nodiscard]] const SocialTaskDescriptor* active_task() const noexcept {return active_?&active_->task:nullptr;}
    [[nodiscard]] std::string active_execution() const {return active_?active_->execution:std::string{};}
    void hydrate(std::vector<SocialTaskDescriptor> tasks,std::uint64_t generation,RuntimeActorIdentity player) {
        if(!generation||!social_actor_valid(player)||player.form_id!=0x14||tasks.size()>8)throw std::invalid_argument{"invalid social hydration owner"};
        for(std::size_t i=0;i<tasks.size();++i) {
            if(!social_task_valid(tasks[i])||tasks[i].performer.playthrough_id!=player.playthrough_id)throw std::invalid_argument{"invalid saved social task"};
            for(std::size_t j=0;j<i;++j)if(tasks[i].task_id==tasks[j].task_id)throw std::invalid_argument{"duplicate saved social task"};
        }
        if(generation_!=generation||!social_same_actor(player_,player))reset();
        for(auto& incoming:tasks) {
            const auto previous=std::ranges::find_if(tasks_,[&](const auto& value){return value.task_id==incoming.task_id;});
            if(previous!=tasks_.end()&&previous->revision>incoming.revision)incoming=*previous;
        }
        generation_=generation;player_=std::move(player);tasks_=std::move(tasks);reminders_=tasks_;
        if(active_) {
            const auto found=std::ranges::find_if(tasks_,[&](const auto& task){return task.task_id==active_->task.task_id;});
            if(found==tasks_.end()) {active_->phase=Phase::releasing;active_->after_release.reset();return;}
            if(found->revision<active_->task.revision)return;
            const bool newer=found->revision>active_->task.revision;
            if(!social_same_actor(found->performer,active_->task.performer)||!social_same_actor(found->recipient,active_->task.recipient))throw std::invalid_argument{"social task changed actors"};
            if(found->execution_id && *found->execution_id!=active_->execution && active_->phase!=Phase::claim_wait) {
                active_->phase=active_->visit_dispatched?Phase::releasing:Phase::terminal_report;
                active_->after_release.reset();
                if(!active_->visit_dispatched)active_.reset();
                return;
            }
            active_->task=*found;
            if(terminal(found->status)) {active_->phase=Phase::releasing;active_->after_release.reset();}
            else if(found->execution_id && *found->execution_id==active_->execution) {
                if(found->status=="travelling"&&active_->phase==Phase::claim_wait)active_->phase=Phase::visiting;
                if((found->status=="arrived"||found->status=="conversing") && (active_->phase==Phase::arrival_wait || (newer&&active_->phase==Phase::visiting)))active_->phase=Phase::exchange;
            }
        }
    }
    void upsert_authoritative(SocialTaskDescriptor task,std::uint64_t generation,RuntimeActorIdentity player) {
        auto tasks=(generation_==generation && social_same_actor(player_,player))?tasks_:std::vector<SocialTaskDescriptor>{};
        const auto found=std::ranges::find_if(tasks,[&](const auto& value){return value.task_id==task.task_id;});
        if(found==tasks.end())tasks.push_back(std::move(task));else *found=std::move(task);
        hydrate(std::move(tasks),generation,std::move(player));
    }
    void claim_failed() {if(active_&&active_->phase==Phase::claim_wait){denied_.emplace_back(active_->task.task_id,active_->task.revision);active_.reset();}}
    void stop() noexcept {if(active_){active_->phase=active_->visit_dispatched?Phase::releasing:Phase::terminal_report;active_->after_release=SocialTaskCommandKind::cancelled;}}
    void abort() noexcept {if(active_){active_->phase=active_->visit_dispatched?Phase::releasing:Phase::terminal_report;active_->after_release=SocialTaskCommandKind::failed;}}
    void visit_feedback(std::string_view task,std::string_view execution,SocialTaskVisitFeedback feedback) {
        if(!active_||active_->task.task_id!=task||active_->execution!=execution)return;
        if(feedback==SocialTaskVisitFeedback::released&&active_->phase==Phase::releasing) {
            if(active_->after_release)active_->phase=Phase::terminal_report;else active_.reset();return;
        }
        if(feedback==SocialTaskVisitFeedback::arrived&&active_->phase==Phase::visiting)active_->phase=Phase::arrival_report;
        if(feedback==SocialTaskVisitFeedback::unavailable||feedback==SocialTaskVisitFeedback::failed) {
            active_->phase=Phase::releasing;active_->after_release=SocialTaskCommandKind::failed;
        }
    }
    void note_spoken(std::string_view task,std::string_view execution,std::string request,std::string line) {
        if(!active_||active_->task.task_id!=task||active_->execution!=execution||active_->phase!=Phase::exchange||!social_id(request)||!social_id(line))return;
        active_->proof_request=std::move(request);active_->proof_line=std::move(line);active_->phase=Phase::spoken_report;
    }
    [[nodiscard]] std::optional<SocialTaskCommand> poll(const SocialTaskFrame& frame,core::SnapshotClock::time_point now) {
        if(session_prefix_.empty()||!generation_||frame.generation!=generation_||!social_same_actor(frame.player,player_)||frame.paused)return {};
        if(!active_) {
            for(const auto& task:tasks_)if(task.status=="ready"&&pair_ready(frame,task)&&std::ranges::none_of(denied_,[&](const auto& denied){return denied.first==task.task_id&&denied.second>=task.revision;})) {
                if(task.when=="on_return"&&(!frame.location||!task.location_ref||*frame.location!=*task.location_ref))continue;
                const auto execution="visit:"+session_prefix_+":"+std::to_string(generation_)+":"+std::to_string(++serial_);
                active_=Active{task,execution,now+std::chrono::seconds{120}};
                return SocialTaskCommand{SocialTaskCommandKind::claim,task,execution,active_->deadline,{},{}};
            }
            return {};
        }
        auto& a=*active_;
        if(a.phase==Phase::claim_wait) {if(now>=a.deadline)active_.reset();return {};}
        if(a.phase!=Phase::releasing&&a.phase!=Phase::terminal_report&&(now>=a.deadline||!pair_ready(frame,a.task))) {
            a.phase=a.visit_dispatched?Phase::releasing:Phase::terminal_report;a.after_release=SocialTaskCommandKind::failed;
        }
        auto command=SocialTaskCommand{SocialTaskCommandKind::visit,a.task,a.execution,a.deadline,{},{}};
        if(a.phase==Phase::releasing)command.kind=SocialTaskCommandKind::release;
        else if(a.phase==Phase::terminal_report) {command.kind=*a.after_release;active_.reset();}
        else if(a.phase==Phase::arrival_report) {command.kind=SocialTaskCommandKind::arrived;a.phase=Phase::arrival_wait;}
        else if(a.phase==Phase::spoken_report) {command.kind=SocialTaskCommandKind::spoken;command.proof_request_id=a.proof_request;command.proof_line_id=a.proof_line;a.phase=Phase::spoken_wait;}
        else if(a.phase==Phase::visiting||a.phase==Phase::exchange||a.phase==Phase::spoken_wait)command.kind=SocialTaskCommandKind::visit;
        else return {};
        if(command.kind==SocialTaskCommandKind::visit&&active_)active_->visit_dispatched=true;
        return command;
    }
};
} // namespace synth::runtime
