#pragma once
#include "adapters/commonlib_capture.hpp"
#include "actions/gift_menu.hpp"
#include "runtime/fallout_runtime.hpp"
#include <bit>
#include <mutex>
#include <random>

namespace synth::adapters::commonlib {
// Only copied ticket/ownership state crosses VM threads. All engine and menu reads occur in apply().
class FlatGiftMenu final {
    struct Job {
        std::string ticket,action;
        core::CancellationToken cancellation;
        core::SnapshotClock::time_point deadline;
        bool dispatched{},admitted{},opened{},consumed{};
        std::vector<core::InventoryItemSnapshot> player_before,npc_before;
    };
    inline static std::mutex mutex_;
    inline static std::optional<Job> job_;
    static bool current(std::monostate,std::string_view ticket) {
        std::scoped_lock lock{mutex_};
        return job_ && job_->ticket==ticket && !job_->consumed && !job_->cancellation.is_cancelled() && core::SnapshotClock::now()<job_->deadline;
    }
    static void finished(std::monostate,std::string_view ticket,bool admitted) {
        std::scoped_lock lock{mutex_};
        if(job_ && job_->ticket==ticket && !job_->cancellation.is_cancelled()) {job_->dispatched=true;job_->admitted=admitted;}
    }
public:
    static bool bind(RE::BSScript::IVirtualMachine& vm) {
        bool ok=vm.BindNativeMethod(new RE::BSScript::NativeFunction("SYNTHWait","CurrentGiftMenu",current,false));
        ok &=vm.BindNativeMethod(new RE::BSScript::NativeFunction("SYNTHWait","FinishedGiftMenu",finished,false));
        return ok;
    }
    static void retire(){std::scoped_lock lock{mutex_};job_.reset();}
    static runtime::RuntimeActionResult apply(RE::Actor& actor,const runtime::RuntimeActionRequest& request) {
        using Status=runtime::RuntimeActionStatus;
        auto* player=RE::PlayerCharacter::GetSingleton();auto* ui=RE::UI::GetSingleton();
        if(!player || !ui || &actor==player || !request.recipient || request.recipient->form_id!=0x14 ||
            request.recipient->origin_plugin!="Fallout4.esm" || request.recipient->playthrough_id!=request.actor.playthrough_id ||
            request.item || request.action_id.empty() || request.cancellation.is_cancelled())
            return {Status::rejected,"Gift menu requires its exact NPC and Player owner"};
        if(core::SnapshotClock::now()>=request.deadline)return {Status::timed_out,"Gift menu ownership expired"};
        for(auto* participant:{&actor,static_cast<RE::Actor*>(player)}) {
            if(participant->IsDeleted() || participant->IsDisabled() || participant->IsDead(false) || !participant->Get3D() ||
                !participant->currentProcess || !participant->inventoryList || !participant->GetParentCell() ||
                participant->GetParentCell()!=player->GetParentCell() || participant->IsInCombat())
                return {Status::unavailable,"Gift menu participants are no longer available in the same cell"};
        }
        std::string ticket;
        {
            std::scoped_lock lock{mutex_};
            if(job_ && job_->action==request.action_id) {
                if(job_->cancellation.is_cancelled() || job_->consumed)return {Status::rejected,"Gift menu owner retired"};
                if(ui->GetMenuOpen<RE::ContainerMenu>()) {
                    const auto menu=ui->GetMenu<RE::ContainerMenu>();const auto owner=menu?menu->containerRef.get():RE::NiPointer<RE::TESObjectREFR>{};
                    if(!owner || owner.get()!=&actor){job_->consumed=true;return {Status::failed,"Opened inventory belongs to another actor"};}
                    job_->opened=true;
                    return {Status::unavailable,"Gift inventory is open; awaiting the player's choices",{},{},true};
                }
                if(job_->opened) {
                    if(menu_mode_active())return {Status::unavailable,"Awaiting gift inventory close",{},{},true};
                    auto [player_items,player_quality]=inventory_snapshot(*player);
                    auto [npc_items,npc_quality]=inventory_snapshot(actor);job_->consumed=true;
                    runtime::RuntimeActionResult result{Status::failed,"Gift menu closed; inventory changes could not be verified"};
                    result.transfer=runtime::RuntimeTransferObservation{
                        {request.cancellation.generation(),*request.recipient,std::move(player_items),std::move(player_quality),request.action_id},
                        {request.cancellation.generation(),request.actor,std::move(npc_items),std::move(npc_quality),request.action_id}};
                    if(result.transfer->donor.observation=="complete" && result.transfer->recipient.observation=="complete" &&
                        actions::gift_menu_exchange_conserved(job_->player_before,job_->npc_before,result.transfer->donor.items,result.transfer->recipient.items)) {
                        result.status=Status::succeeded;result.detail="Gift menu opened for the exact NPC and closed; paired inventory conservation verified";
                    }
                    return result;
                }
                if(job_->dispatched && !job_->admitted){job_->consumed=true;return {Status::failed,"Gift menu script rejected its actor or menu state"};}
                return {Status::unavailable,"Gift menu dispatched; awaiting the exact actor's inventory",{},{},true};
            }
            if(job_ && !job_->consumed && !job_->cancellation.is_cancelled() && core::SnapshotClock::now()<job_->deadline)
                return {Status::unavailable,"Another owned gift menu is pending"};
            if(menu_mode_active())return {Status::unavailable,"Close the current menu before offering a gift"};
            const auto a=point(actor.GetPosition()),b=point(player->GetPosition());
            const auto dx=a.x-b.x,dy=a.y-b.y,dz=a.z-b.z;
            if(!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz) || dx*dx+dy*dy+dz*dz>512.0*512.0)
                return {Status::unavailable,"Gift recipient is too far away"};
            auto [player_items,player_quality]=inventory_snapshot(*player);auto [npc_items,npc_quality]=inventory_snapshot(actor);
            if(player_quality!="complete" || npc_quality!="complete")return {Status::unavailable,"Gift menu requires complete paired inventory observations"};
            std::random_device entropy;for(int i=0;i<4;++i)ticket+=std::to_string(entropy())+"-";
            job_=Job{ticket,request.action_id,request.cancellation,request.deadline,false,false,false,false,std::move(player_items),std::move(npc_items)};
        }
        auto* game=RE::GameVM::GetSingleton();const auto vm=game?game->GetVM():nullptr;
        const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        const bool queued=vm && vm->DispatchStaticCall("SYNTHWait","GiftMenuByFormIds",callback,ticket,
            std::bit_cast<std::int32_t>(actor.GetFormID()),std::bit_cast<std::int32_t>(player->GetParentCell()->GetFormID()));
        if(!queued){finished({},ticket,false);return {Status::failed,"Gift menu dispatch rejected; check SYNTHWait.pex"};}
        return {Status::unavailable,"Gift menu dispatched; awaiting the exact actor's inventory",{},{},true};
    }
};
} // namespace synth::adapters::commonlib
