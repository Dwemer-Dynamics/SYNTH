#pragma once
#include "adapters/flat_wait_package.hpp"
#include "runtime/conversation_approach.hpp"
#include <RE/P/PTYPE.h>
#include <bit>
#include <cmath>
#include <mutex>
#include <random>

namespace synth::adapters::commonlib {
// Only the engine-thread pump owns actors. VM callbacks mutate copied receipt metadata under a short lock.
class FlatConversationApproach final {
    struct Observation { std::uint32_t package{};bool actor{}, listener{}; };
    struct Job {
        std::string ticket;
        runtime::WaitScriptReceipt receipt;
        std::optional<runtime::ConversationApproachOperation> operation;
        core::SnapshotClock::time_point deadline;
        std::optional<Observation> observation;
        bool timed_out{};
    };
    inline static runtime::ConversationApproachController controller_;
    inline static std::mutex mutex_;
    inline static std::optional<Job> job_;
    inline static bool initialized_{}, quarantined_{}, recovery_requested_{};
    inline static core::SnapshotClock::time_point next_sweep_{};
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
        else recovery_requested_=true; // A restored old stack may have completed one mutation after its last saved Current().
    }
    static void observed(std::monostate,std::string_view ticket,std::int32_t package,bool actor,bool listener) {
        std::scoped_lock lock{mutex_};
        if (job_ && job_->receipt.current(ticket) && within_deadline_locked())
            job_->observation=Observation{std::bit_cast<std::uint32_t>(package),actor,listener};
    }
    // Read only the active holder's copied package type while its nonblocking lock owns the pointer.
    static std::optional<std::int8_t> package_type(RE::Actor& actor) {
        auto* process=actor.currentProcess;if (!process) return {};
        std::optional<TrySpinLock> run;
        if (process->middleHigh) {
            auto& holder=process->middleHigh->runOncePackage;
            run.emplace(holder.packageLock);if (!run->owns_lock()) return {};
            if (holder.package) return holder.package->data.packType;
        }
        const TrySpinLock current{process->currentPackage.packageLock};
        if (!current.owns_lock() || !process->currentPackage.package) return {};
        return process->currentPackage.package->data.packType;
    }
    static runtime::ConversationApproachCandidate capture(const runtime::RuntimeConversationHold& hold) {
        runtime::ConversationApproachCandidate result;result.hold=hold;
        auto* player=RE::PlayerCharacter::GetSingleton();
        const auto save=playthrough_id();
        result.listener={0x14,"Fallout4.esm",save};
        if (!player || hold.actor.form_id==0 || hold.actor.form_id==0x14 || hold.actor.playthrough_id!=save ||
            hold.cancellation.is_cancelled() || menu_mode_active()) return result;
        auto* actor=RE::TESForm::GetFormByID<RE::Actor>(hold.actor.form_id);
        if (!actor || origin_plugin(*actor)!=hold.actor.origin_plugin) return result;
        const auto* a=actor->GetParentCell();const auto* b=player->GetParentCell();
        result.same_area=a && b && (a==b || (!a->IsInterior() && !b->IsInterior() && a->worldSpace && a->worldSpace==b->worldSpace));
        result.loaded=actor->Get3D() && actor->currentProcess && !actor->IsDeleted() && !actor->IsDisabled() && !actor->IsDead(false) &&
            player->Get3D() && !player->IsDead(false) && !player->IsDisabled();
        const auto kind=package_type(*actor);
        result.loaded=result.loaded && kind.has_value(); // Unknown ownership never grants a temporary override.
        result.native_follower=(actor->niFlags.flags & (1u<<26))!=0 || (kind &&
            (*kind==static_cast<int>(RE::PTYPE::kFollow) || *kind==static_cast<int>(RE::PTYPE::kEscore) ||
             *kind==static_cast<int>(RE::PTYPE::kAccompany)));
        result.explicit_wait=FlatWaitPackage::explicit_wait_owned(hold.actor.form_id);
        result.calm=!actor->IsInCombat() && !player->IsInCombat() && !actor->GetHostileToActor(player) &&
            actor->lifeState==0 && actor->knockState==0 && !actor->inSyncAnim && !actor->talkingToPlayer &&
            actor->DoGetSitSleepState()==RE::SIT_SLEEP_STATE::kNormal &&
            (actor->niFlags.flags & static_cast<std::uint32_t>(RE::Actor::BOOL_FLAGS::kScenePackage))==0;
        const auto p=point(actor->GetPosition()),q=point(player->GetPosition());
        const auto dx=p.x-q.x,dy=p.y-q.y,dz=p.z-q.z;
        result.distance=std::sqrt(dx*dx+dy*dy+dz*dz);
        return result;
    }
    static bool navigation_observed(const runtime::ConversationApproachOperation& operation) {
        const auto live=capture({operation.actor,operation.cancellation,true});
        if (!live.eligible()) return false;
        auto* data=RE::TESDataHandler::GetSingleton();
        auto* expected=data?data->LookupForm<RE::TESPackage>(0xB10,"SYNTH.esp"):nullptr;
        auto* actor=RE::TESForm::GetFormByID<RE::Actor>(operation.actor.form_id);
        if (!expected || !actor) return false;
        const auto active=current_package(*actor); // Existing bounded try-lock observation, never a borrowed package on a worker.
        return active && active->form_id==expected->GetFormID();
    }
public:
    static bool bind(RE::BSScript::IVirtualMachine& vm) {
        bool ok=vm.BindNativeMethod(new RE::BSScript::NativeFunction("SYNTHWait","CurrentApproach",current,false));
        ok &= vm.BindNativeMethod(new RE::BSScript::NativeFunction("SYNTHWait","FinishedApproach",finished,false));
        ok &= vm.BindNativeMethod(new RE::BSScript::NativeFunction("SYNTHWait","ObservedApproach",observed,false));
        return ok;
    }
    static void retire() {
        std::scoped_lock lock{mutex_};job_.reset();controller_.reset();initialized_=false;quarantined_=false;recovery_requested_=false;next_sweep_={};paused_at_.reset();
    }
    static bool actions_ready() {
        std::scoped_lock lock{mutex_};
        return !recovery_requested_ && !job_ && controller_.idle() && FlatWaitPackage::conversation_actions_ready();
    }
    static void update(std::span<const runtime::RuntimeConversationHold> holds,bool discard) {
        FlatWaitPackage::update_conversation(holds,discard); // Drain older conversational restraints by their original ownership.
        if (discard) {retire();return;}
        if (menu_mode_active() && !holds.empty()) return;
        std::vector<runtime::ConversationApproachCandidate> candidates;
        for (const auto& hold:holds) if (hold.approach_player && candidates.size()<16) candidates.push_back(capture(hold));
        if (quarantined_) candidates.clear();
        const auto now=core::SnapshotClock::now();controller_.update(candidates,now);
        std::scoped_lock lock{mutex_};
        if (job_ && job_->operation && job_->operation->kind!=runtime::ConversationApproachKind::release &&
            !controller_.install_current(*job_->operation,now)) job_->receipt.revoke_install();
    }
    static void tick(core::SnapshotClock::time_point now) {
        if (menu_mode_active()) {
            std::scoped_lock lock{mutex_};if (!paused_at_) paused_at_=now;return;
        }
        {
            std::scoped_lock lock{mutex_};
            if (paused_at_) {if (job_) job_->deadline+=now-*paused_at_;paused_at_.reset();next_sweep_={};}
        }
        if (now<next_sweep_) return;
        next_sweep_=now+std::chrono::milliseconds{100};
        bool recover_stale{};
        {
            std::scoped_lock lock{mutex_};
            if (recovery_requested_) {
                if (job_ && !job_->receipt.done()) {
                    job_->receipt.revoke_install();
                    // The current stack must drain before any alias reset, including a foreign old callback.
                    // The ordinary timeout path below still reports a VM that does not finish.
                } else {
                    job_.reset();recovery_requested_=false;recover_stale=true;
                }
            }
        }
        if (recover_stale) {
            controller_.reset();initialized_=false; // Navigation only: never cancel the new dialogue owner.
            REX::WARN("SYNTH approach: late saved stack drained; recovering original approach aliases");
        }
        // EvaluatePackage may finish before the engine selects the package on its next frame.
        // Keep the completed VM receipt while polling native evidence; no new VM mutation is queued.
        std::optional<Job> verification;
        {
            std::scoped_lock lock{mutex_};
            if (job_ && job_->receipt.done() && job_->receipt.success() && job_->operation &&
                job_->operation->kind!=runtime::ConversationApproachKind::release && job_->observation &&
                job_->observation->actor && job_->observation->listener) verification=job_;
        }
        if (verification && now<verification->deadline && controller_.install_current(*verification->operation,now) &&
            capture({verification->operation->actor,verification->operation->cancellation,true}).eligible() &&
            !navigation_observed(*verification->operation)) return;
        std::optional<Job> completed;
        bool draining{}, report_timeout{};
        {
            std::scoped_lock lock{mutex_};
            if (job_) {
                if (!job_->receipt.done()) {
                    if (now<job_->deadline) return;
                    // Current() and ForceRefTo are distinct VM operations. Revoke further mutation,
                    // but retain this exact stack's ticket until Finished acknowledges it has drained.
                    // Starting cleanup now could race a previously admitted ForceRefTo on this alias.
                    draining=true;
                    if (!job_->timed_out) {
                        job_->timed_out=true;job_->receipt.revoke_install();quarantined_=true;report_timeout=true;
                    }
                } else {completed=std::move(job_);job_.reset();}
            }
        }
        if (draining) {
            if (report_timeout) REX::WARN("SYNTH approach: VM timeout; cleanup waits for owned stack to drain. Reload if it does not finish");
            return; // No alias release or reuse until the matching Finished callback, even after Stop.
        }
        if (completed) {
            bool ok=completed->receipt.success();
            if (completed->operation) {
                const auto& operation=*completed->operation;
                if (operation.kind!=runtime::ConversationApproachKind::release) {
                    ok=ok && completed->observation && completed->observation->actor && completed->observation->listener &&
                        navigation_observed(operation);
                } else ok=ok && completed->observation && !completed->observation->actor && !completed->observation->listener;
                controller_.complete(operation,ok,now);
                if (!ok) next_sweep_=now+std::chrono::seconds{2};
                if (operation.kind!=runtime::ConversationApproachKind::refresh || !ok)
                {
                    const auto live=capture({operation.actor,operation.cancellation,true});
                    auto* actor=RE::TESForm::GetFormByID<RE::Actor>(operation.actor.form_id);
                    const auto active=actor?current_package(*actor):std::optional<core::PackageSnapshot>{};
                    auto* data=RE::TESDataHandler::GetSingleton();
                    auto* expected=data?data->LookupForm<RE::TESPackage>(0xB10,"SYNTH.esp"):nullptr;
                    REX::INFO("SYNTH approach: actor {:08X} owned {} {} package {:08X} expected {:08X} distance {:.1f} eligible={}",
                        operation.actor.form_id,operation.kind==runtime::ConversationApproachKind::release ? "release" : "navigation",
                        ok?"confirmed":"not confirmed",active?active->form_id:0,expected?expected->GetFormID():0,live.distance,live.eligible());
                }
            } else {
                initialized_=ok;
                if (!ok) next_sweep_=now+std::chrono::seconds{5};
                REX::INFO("SYNTH approach: saved alias recovery {}",ok?"complete":"not confirmed");
            }
        }
        if (now<next_sweep_-std::chrono::milliseconds{100}) return;
        {
            std::scoped_lock lock{mutex_};
            if (recovery_requested_) return; // Next safe tick resets only after the live stack's Finished acknowledgment.
        }
        std::optional<runtime::ConversationApproachOperation> operation;
        if (initialized_) operation=controller_.next(now);
        if (initialized_ && !operation) return;
        if (operation && operation->kind!=runtime::ConversationApproachKind::release &&
            (!controller_.install_current(*operation,now) || !capture({operation->actor,operation->cancellation,true}).eligible())) {
            controller_.complete(*operation,false,now);return;
        }
        auto* data=RE::TESDataHandler::GetSingleton();
        auto* owner=data?data->LookupForm<RE::TESQuest>(0xB00,"SYNTH.esp"):nullptr;
        auto* game=RE::GameVM::GetSingleton();const auto vm=game?game->GetVM():nullptr;
        if (!owner || !vm) {
            if (operation) controller_.complete(*operation,false,now);
            next_sweep_=now+std::chrono::seconds{5};return;
        }
        std::random_device entropy;std::string ticket;
        for(int i=0;i<4;++i)ticket+=std::to_string(entropy())+"-";
        const bool acquire=operation && operation->kind!=runtime::ConversationApproachKind::release;
        {
            std::scoped_lock lock{mutex_};
            job_=Job{ticket,runtime::WaitScriptReceipt{ticket,operation?operation->cancellation:core::CancellationToken{},acquire},
                operation,now+std::chrono::seconds{5},{}};
        }
        const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        const auto quest=std::bit_cast<std::int32_t>(owner->GetFormID());
        const bool queued=operation ? vm->DispatchStaticCall("SYNTHWait","ApproachByFormIds",callback,ticket,quest,std::int32_t{0},
            std::bit_cast<std::int32_t>(operation->actor.form_id),std::bit_cast<std::int32_t>(operation->listener.form_id),acquire)
            : vm->DispatchStaticCall("SYNTHWait","ResetApproachByFormId",callback,ticket,quest);
        if (!queued) finished({},ticket,false);
    }
};
} // namespace synth::adapters::commonlib
