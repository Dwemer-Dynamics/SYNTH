#pragma once
#include "adapters/flat_conversation_approach.hpp"

namespace synth::adapters::commonlib {

// Game-thread-owned navigation only. The client owns task persistence and dialogue dispatch.
class FlatSocialVisit final {
    using Status=runtime::RuntimeSocialVisitStatus;
    enum class Kind : unsigned char { reset, acquire, refresh, release };
    struct Observation { std::uint32_t package{};bool actor{}, listener{}; };
    struct Job {
        std::string ticket;
        runtime::WaitScriptReceipt receipt;
        Kind kind{};
        core::SnapshotClock::time_point deadline;
        std::optional<Observation> observation;
        bool timed_out{};
    };
    struct Lease {
        runtime::RuntimeSocialVisitRequest request;
        bool installed{}, closing{}, explicit_release{};
        std::string failure;
        core::SnapshotClock::time_point refresh_at;
    };
    struct Live { bool eligible{};std::uint32_t package{}, expected{};double distance{}; };
    inline static std::mutex mutex_;
    inline static std::optional<Job> job_;
    inline static std::optional<Lease> lease_;
    inline static std::optional<runtime::RuntimeSocialVisitResult> last_;
    inline static std::optional<runtime::RuntimeSocialVisitRequest> last_request_;
    inline static bool initialized_{}, quarantined_{}, recovery_requested_{};
    inline static core::SnapshotClock::time_point sweep_{};

    static bool plugin_equal(std::string_view a,std::string_view b) {
        const auto fold=[](unsigned char ch) {return ch>='A' && ch<='Z'?ch+('a'-'A'):ch;};
        return std::ranges::equal(a,b,[&](auto x,auto y){return fold(x)==fold(y);});
    }
    static bool same_execution(const runtime::RuntimeSocialVisitRequest& a,const runtime::RuntimeSocialVisitRequest& b) {
        return a.same_execution(b);
    }
    static runtime::RuntimeSocialVisitResult result(const runtime::RuntimeSocialVisitRequest& request,
                                                   Status status,std::string code,double distance=0) {
        return {status,request.task_id,request.execution_id,std::move(code),distance};
    }
    inline static std::optional<core::SnapshotClock::time_point> paused_at_;
    static bool within_deadline_locked() {
        const auto now=core::SnapshotClock::now();
        return job_ && now<job_->deadline+(paused_at_?now-*paused_at_:core::SnapshotClock::duration{});
    }
    static bool current(std::monostate,std::string_view ticket) {
        std::scoped_lock lock{mutex_};
        return job_ && within_deadline_locked() && job_->receipt.current(ticket);
    }
    static void finished(std::monostate,std::string_view ticket,bool success) {
        std::scoped_lock lock{mutex_};
        if (job_ && job_->ticket==ticket) job_->receipt.finish(ticket,success && within_deadline_locked());
        else recovery_requested_=true;
    }
    static void observed(std::monostate,std::string_view ticket,std::int32_t package,bool actor,bool listener) {
        std::scoped_lock lock{mutex_};
        if (job_ && job_->receipt.current(ticket) && within_deadline_locked())
            job_->observation=Observation{std::bit_cast<std::uint32_t>(package),actor,listener};
    }
    static Live capture(const runtime::RuntimeSocialVisitRequest& request) {
        Live live;
        auto* player=RE::PlayerCharacter::GetSingleton();
        if (!player || menu_mode_active() || request.actor.playthrough_id!=playthrough_id() ||
            request.listener.playthrough_id!=request.actor.playthrough_id) return live;
        auto* actor=RE::TESForm::GetFormByID<RE::Actor>(request.actor.form_id);
        auto* listener=RE::TESForm::GetFormByID<RE::Actor>(request.listener.form_id);
        if (!actor || !listener || actor==listener || actor==player || listener==player ||
            !plugin_equal(origin_plugin(*actor),request.actor.origin_plugin) ||
            !plugin_equal(origin_plugin(*listener),request.listener.origin_plugin) ||
            (actor->niFlags.flags & (1u<<26))==0 || FlatWaitPackage::explicit_wait_owned(request.actor.form_id)) return live;
        for (auto* value:{actor,listener}) {
            if (!value->Get3D() || !value->currentProcess || value->IsDeleted() || value->IsDisabled() || value->IsDead(false) ||
                value->IsInCombat() || value->lifeState!=0 || value->knockState!=0 || value->inSyncAnim || value->talkingToPlayer ||
                value->DoGetSitSleepState()!=RE::SIT_SLEEP_STATE::kNormal ||
                (value->niFlags.flags & static_cast<std::uint32_t>(RE::Actor::BOOL_FLAGS::kScenePackage))!=0) return live;
        }
        if (actor->GetHostileToActor(listener) || listener->GetHostileToActor(actor) || listener->GetHostileToActor(player)) return live;
        const auto* a=actor->GetParentCell();const auto* b=listener->GetParentCell();
        if (!a || !b || (a!=b && (a->IsInterior() || b->IsInterior() || !a->worldSpace || a->worldSpace!=b->worldSpace))) return live;
        const auto p=point(actor->GetPosition()),q=point(listener->GetPosition());
        const auto dx=p.x-q.x,dy=p.y-q.y,dz=p.z-q.z;
        live.distance=std::sqrt(dx*dx+dy*dy+dz*dz);
        if (!std::isfinite(live.distance) || live.distance>2000) return live;
        auto* data=RE::TESDataHandler::GetSingleton();
        auto* expected=data?data->LookupForm<RE::TESPackage>(0xB40,"SYNTH.esp"):nullptr;
        if (!expected) return live;
        live.expected=expected->GetFormID();
        if (const auto active=current_package(*actor)) live.package=active->form_id;
        live.eligible=true;
        return live;
    }
    static void close(std::string code,bool explicit_release=false) {
        if (!lease_) return;
        lease_->closing=true;lease_->explicit_release=lease_->explicit_release || explicit_release;
        if (lease_->failure.empty()) lease_->failure=std::move(code);
        std::scoped_lock lock{mutex_};
        if (job_ && (job_->kind==Kind::acquire || job_->kind==Kind::refresh)) job_->receipt.revoke_install();
    }
    static void cleanup_complete() {
        if (!lease_) return;
        last_request_=lease_->request;
        last_=result(lease_->request,lease_->explicit_release?Status::released:Status::failed,
            lease_->explicit_release?"released":lease_->failure.empty()?"navigation_failed":lease_->failure);
        lease_.reset();
    }
public:
    static bool bind(RE::BSScript::IVirtualMachine& vm) {
        bool ok=vm.BindNativeMethod(new RE::BSScript::NativeFunction("SYNTHWait","CurrentVisit",current,false));
        ok &= vm.BindNativeMethod(new RE::BSScript::NativeFunction("SYNTHWait","FinishedVisit",finished,false));
        ok &= vm.BindNativeMethod(new RE::BSScript::NativeFunction("SYNTHWait","ObservedVisit",observed,false));
        return ok;
    }
    static void retire() {
        std::scoped_lock lock{mutex_};
        job_.reset();lease_.reset();last_.reset();last_request_.reset();initialized_=false;quarantined_=false;recovery_requested_=false;sweep_={};paused_at_.reset();
    }
    static void cancel_all() { close("cancelled"); }
    static std::optional<runtime::RuntimeActorIdentity> active_owner() {
        return lease_?std::optional{lease_->request.actor}:std::nullopt;
    }
    static void cancel_actor(const runtime::RuntimeActorIdentity& actor) {
        if (lease_ && lease_->request.actor.form_id==actor.form_id &&
            lease_->request.actor.playthrough_id==actor.playthrough_id &&
            plugin_equal(lease_->request.actor.origin_plugin,actor.origin_plugin)) close("player_command");
    }
    static bool actions_ready() {
        std::scoped_lock lock{mutex_};
        return !lease_ && !job_ && !recovery_requested_;
    }
    static runtime::RuntimeSocialVisitResult apply(const runtime::RuntimeSocialVisitRequest& request) {
        const auto now=core::SnapshotClock::now();
        if (!request.valid_ids()) return result(request,Status::unavailable,"invalid_identity");
        if (lease_ && !same_execution(lease_->request,request))
            return result(request,request.release?Status::unavailable:Status::pending,request.release?"execution_changed":"visit_busy");
        if (request.release) {
            if (lease_) { close("released",true);return result(request,Status::pending,"release_pending"); }
            if (last_ && last_->task_id==request.task_id && last_->execution_id==request.execution_id) {
                if (!last_request_ || !same_execution(*last_request_,request)) return result(request,Status::unavailable,"binding_changed");
                auto copy=*last_;copy.status=Status::released;copy.detail_code="released";return copy;
            }
            {
                std::scoped_lock lock{mutex_};
                if (!initialized_ || job_ || recovery_requested_) return result(request,Status::pending,"alias_recovery");
            }
            // No lease was acquired (for example Stop arrived between claim and loaded-pair admission).
            // A completed owned-quest recovery is the authoritative empty-alias receipt.
            return result(request,Status::released,"no_owned_visit");
        }
        if (menu_mode_active()) return result(request,Status::pending,"menu_paused");
        if (lease_) {
            if (lease_->request.cancellation.is_cancelled() || now>=lease_->request.deadline) close("cancelled_or_expired");
            if (lease_->closing) return result(request,Status::pending,"cleanup_pending");
            const auto live=capture(lease_->request);
            if (!live.eligible) { close("actor_unavailable");return result(request,Status::pending,"cleanup_pending"); }
            if (lease_->installed && live.expected && live.package==live.expected && live.distance<=256)
                return result(request,Status::arrived,"owned_arrival",live.distance);
            return result(request,Status::pending,"navigating",live.distance);
        }
        if (last_ && last_->task_id==request.task_id && last_->execution_id==request.execution_id)
            return last_request_ && same_execution(*last_request_,request) ? *last_ : result(request,Status::unavailable,"binding_changed");
        if (request.cancellation.is_cancelled() || now>=request.deadline) return result(request,Status::failed,"cancelled_or_expired");
        if (quarantined_) return result(request,Status::unavailable,"vm_recovery_required");
        if (!initialized_) return result(request,Status::pending,"alias_recovery");
        if (!capture(request).eligible) return result(request,Status::pending,"actor_unavailable");
        auto bounded=request;bounded.deadline=std::min(request.deadline,now+std::chrono::seconds{120});
        lease_=Lease{std::move(bounded),false,false,false,{},now};last_.reset();last_request_.reset();
        return result(request,Status::pending,"navigation_pending");
    }
    static void tick(core::SnapshotClock::time_point now) {
        if (menu_mode_active()) {
            std::scoped_lock lock{mutex_};if (!paused_at_) paused_at_=now;return;
        }
        {
            std::scoped_lock lock{mutex_};
            if (paused_at_) {if (job_) job_->deadline+=now-*paused_at_;paused_at_.reset();sweep_={};}
        }
        if (now<sweep_) return;
        sweep_=now+std::chrono::milliseconds{100};
        bool recover{};
        {
            std::scoped_lock lock{mutex_};
            if (recovery_requested_) {
                if (job_ && !job_->receipt.done()) job_->receipt.revoke_install();
                else { job_.reset();recovery_requested_=false;recover=true; }
            }
        }
        if (recover) { initialized_=false;close("stale_vm_recovery"); }
        if (lease_ && (lease_->request.cancellation.is_cancelled() || now>=lease_->request.deadline)) close("cancelled_or_expired");
        if (lease_ && !lease_->closing && !capture(lease_->request).eligible) close("actor_unavailable");
        std::optional<Job> verification;
        {
            std::scoped_lock lock{mutex_};
            if (job_ && job_->receipt.done() && job_->receipt.success() &&
                (job_->kind==Kind::acquire || job_->kind==Kind::refresh) && job_->observation &&
                job_->observation->actor && job_->observation->listener) verification=job_;
        }
        if (verification && lease_ && !lease_->closing && now<verification->deadline) {
            const auto live=capture(lease_->request);
            if (live.eligible && live.package!=live.expected) return;
        }
        std::optional<Job> completed;
        bool draining{},warn{};
        {
            std::scoped_lock lock{mutex_};
            if (job_) {
                if (!job_->receipt.done()) {
                    if (now<job_->deadline) return;
                    draining=true;
                    if (!job_->timed_out) {job_->timed_out=true;job_->receipt.revoke_install();quarantined_=true;warn=true;}
                } else {
                    completed=std::move(job_);job_.reset();
                }
            }
        }
        if (draining) {
            if (warn) REX::WARN("SYNTH visit: VM timeout; cleanup waits for owned stack to drain. Reload if it does not finish");
            return;
        }
        if (completed) {
            bool ok=completed->receipt.success();
            if (completed->kind==Kind::reset) {
                initialized_=ok;
                if (ok && lease_ && lease_->closing) cleanup_complete();
            } else if (completed->kind==Kind::release) {
                ok=ok && completed->observation && !completed->observation->actor && !completed->observation->listener;
                if (ok) cleanup_complete();
            } else {
                const auto live=lease_?capture(lease_->request):Live{};
                ok=ok && lease_ && !lease_->closing && completed->observation && completed->observation->actor &&
                    completed->observation->listener && live.eligible && live.expected && live.package==live.expected;
                if (ok) {lease_->installed=true;lease_->refresh_at=now+std::chrono::seconds{1};}
                else close("navigation_unconfirmed");
            }
            if (!ok) sweep_=now+std::chrono::seconds{2};
            if (completed->kind!=Kind::refresh || !ok)
                REX::INFO("SYNTH visit: owned phase {} {}",static_cast<unsigned>(completed->kind),ok?"confirmed":"not confirmed");
        }
        if (now<sweep_-std::chrono::milliseconds{100}) return;
        {
            std::scoped_lock lock{mutex_};if (recovery_requested_) return;
        }
        Kind kind=Kind::reset;
        if (initialized_) {
            if (!lease_) return;
            if (lease_->closing) kind=Kind::release;
            else if (!lease_->installed) kind=Kind::acquire;
            else if (now>=lease_->refresh_at) kind=Kind::refresh;
            else return;
        }
        auto* data=RE::TESDataHandler::GetSingleton();
        auto* owner=data?data->LookupForm<RE::TESQuest>(0xB30,"SYNTH.esp"):nullptr;
        auto* game=RE::GameVM::GetSingleton();const auto vm=game?game->GetVM():nullptr;
        if (!owner || !vm) {sweep_=now+std::chrono::seconds{5};return;}
        std::random_device entropy;std::string ticket;
        for(int index=0;index<4;++index) ticket+=std::to_string(entropy())+"-";
        const bool install=kind==Kind::acquire || kind==Kind::refresh;
        const auto token=lease_?lease_->request.cancellation:core::CancellationToken{};
        {
            std::scoped_lock lock{mutex_};
            job_=Job{ticket,runtime::WaitScriptReceipt{ticket,token,install},kind,now+std::chrono::seconds{5},{},false};
        }
        const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        const auto quest=std::bit_cast<std::int32_t>(owner->GetFormID());
        const bool queued=kind==Kind::reset ? vm->DispatchStaticCall("SYNTHWait","ResetVisitByFormId",callback,ticket,quest)
            : vm->DispatchStaticCall("SYNTHWait","VisitByFormIds",callback,ticket,quest,std::int32_t{0},
                std::bit_cast<std::int32_t>(lease_->request.actor.form_id),std::bit_cast<std::int32_t>(lease_->request.listener.form_id),install);
        if (!queued) finished({},ticket,false);
    }
};
} // namespace synth::adapters::commonlib
