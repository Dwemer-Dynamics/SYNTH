#pragma once
#include "adapters/commonlib_capture.hpp"
#include "runtime/fallout_runtime.hpp"
#include <bit>
#include <cmath>
#include <mutex>
#include <random>

namespace synth::adapters::commonlib {
// VM workers see only a random ticket and copied cancellation/deadline, never a game object.
class FlatAttackAction final {
    struct Job { std::string ticket, action; core::CancellationToken cancellation;
        core::SnapshotClock::time_point deadline; bool done{}, success{}, consumed{}; };
    inline static std::mutex mutex_;
    inline static std::optional<Job> job_;
    static bool current(std::monostate,std::string_view ticket) {
        std::scoped_lock lock{mutex_};
        return job_ && job_->ticket==ticket && !job_->done && !job_->cancellation.is_cancelled() &&
            core::SnapshotClock::now()<job_->deadline;
    }
    static void finished(std::monostate,std::string_view ticket,bool success) {
        std::scoped_lock lock{mutex_};
        if (job_ && job_->ticket==ticket && !job_->cancellation.is_cancelled() &&
            core::SnapshotClock::now()<job_->deadline) { job_->done=true;job_->success=success; }
    }
public:
    static bool bind(RE::BSScript::IVirtualMachine& vm) {
        bool ok=vm.BindNativeMethod(new RE::BSScript::NativeFunction("SYNTHWait","CurrentAttack",current,false));
        ok &= vm.BindNativeMethod(new RE::BSScript::NativeFunction("SYNTHWait","FinishedAttack",finished,false));
        return ok;
    }
    static void retire() { std::scoped_lock lock{mutex_};job_.reset(); }
    static runtime::RuntimeActionResult apply(RE::Actor& actor,const runtime::RuntimeActionRequest& request) {
        using Status=runtime::RuntimeActionStatus;
        auto* player=RE::PlayerCharacter::GetSingleton();
        if (!player || &actor==player || !request.recipient || request.recipient->form_id==actor.GetFormID() ||
            request.recipient->form_id==0x14 || request.recipient->playthrough_id!=request.actor.playthrough_id ||
            request.action_id.empty() || request.cancellation.is_cancelled() || menu_mode_active())
            return {Status::rejected,"Attack requires its owned follower and distinct target"};
        if (core::SnapshotClock::now()>=request.deadline) return {Status::timed_out,"Attack expired before execution"};
        auto* target=RE::TESForm::GetFormByID<RE::Actor>(request.recipient->form_id);
        if (!target || origin_plugin(*target)!=request.recipient->origin_plugin)
            return {Status::unavailable,"Exact attack target is no longer loaded"};
        const auto* cell=actor.GetParentCell();const auto* other=target->GetParentCell();
        if (!cell || !other || (cell!=other && (cell->IsInterior() || other->IsInterior() || !cell->worldSpace || cell->worldSpace!=other->worldSpace)))
            return {Status::unavailable,"Attack actors are no longer in the same loaded area"};
        for (auto* value:{&actor,target}) if (value->IsDead(false) || value->IsDeleted() || value->IsDisabled() ||
            !value->Get3D() || !value->currentProcess || value->lifeState!=0 || value->knockState!=0 || value->inSyncAnim ||
            (value->niFlags.flags & static_cast<std::uint32_t>(RE::Actor::BOOL_FLAGS::kScenePackage))!=0)
            return {Status::unavailable,"Attack actor is unavailable, restrained or in a scene"};
        if ((actor.niFlags.flags & (1u<<26))==0 || actor.GetHostileToActor(player) ||
            !(target->GetHostileToActor(&actor) || target->GetHostileToActor(player)))
            return {Status::rejected,"Attack requires a follower and a currently hostile target"};
        const auto a=point(actor.GetPosition()),b=point(target->GetPosition());
        const auto dx=a.x-b.x,dy=a.y-b.y,dz=a.z-b.z;
        RE::DetectionData detection{};actor.CalculateDetectionFormula(target,&detection);
        if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz) || dx*dx+dy*dy+dz*dz>2000.0*2000.0 || !detection.LOS || detection.detectionLevel.visualLevel<=0 || detection.targetInvisible)
            return {Status::unavailable,"Attack target is outside this follower's current sight or range"};
        std::string ticket;
        {
            std::scoped_lock lock{mutex_};
            if (job_ && job_->action==request.action_id) {
                if (job_->cancellation.is_cancelled() || core::SnapshotClock::now()>=job_->deadline)
                    return {Status::timed_out,"Attack script ownership expired"};
                if (job_->done && !job_->success) { job_->consumed=true;return {Status::failed,"Attack script rejected its live target"}; }
                const auto active=actor.currentCombatTarget.get();
                if (job_->done && actor.IsInCombat() && active && active.get()==target) {
                    job_->consumed=true;
                    return {Status::succeeded,"Follower's native combat target now matches the requested actor"};
                }
                return {Status::unavailable,"Attack dispatched; awaiting native combat target",{},{},true};
            }
            if (job_ && !job_->consumed && !job_->cancellation.is_cancelled() && core::SnapshotClock::now()<job_->deadline)
                return {Status::unavailable,"Another owned attack is still pending"};
            std::random_device entropy;for(int i=0;i<4;++i)ticket+=std::to_string(entropy())+"-";
            job_=Job{ticket,request.action_id,request.cancellation,request.deadline};
        }
        auto* game=RE::GameVM::GetSingleton();const auto vm=game?game->GetVM():nullptr;
        const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        const bool queued=vm && vm->DispatchStaticCall("SYNTHWait","AttackByFormIds",callback,ticket,
            std::bit_cast<std::int32_t>(actor.GetFormID()),std::bit_cast<std::int32_t>(target->GetFormID()));
        if (!queued) { finished({},ticket,false);return {Status::failed,"Attack script dispatch rejected; check SYNTHWait.pex"}; }
        return {Status::unavailable,"Attack dispatched; awaiting native combat target",{},{},true};
    }
};
} // namespace synth::adapters::commonlib
