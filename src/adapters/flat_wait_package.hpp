#pragma once

#include "adapters/commonlib_capture.hpp"
#include "runtime/wait_recovery.hpp"
#include "adapters/native_fault_recorder.hpp"
#include <RE/M/MiddleHighProcessData.h>
#include <array>
#include <bit>
#include <mutex>
#include <random>
#include <unordered_map>

namespace synth::adapters::commonlib {

// The game-thread coordinator owns actors; VM callbacks touch copied ticket metadata only.
class FlatWaitPackage final {
    using Status=runtime::RuntimeActionStatus;
    struct Observation {
        std::uint32_t active{},expected{};
        std::int32_t state{},sit_state{};
        bool operator==(const Observation&) const=default;
    };
    struct Command {
        runtime::RuntimeActionRequest request;
        std::optional<runtime::RuntimeActionResult> result;
        std::string failure;
    };
    struct Lease {
        RE::ActorHandle actor;
        runtime::RuntimeActorIdentity identity;
        std::uint32_t form{},cell{},package{};
        core::CancellationToken cancellation;
        RE::NiPoint3 position;
        std::uint64_t revision{};
        bool waiting{},dirty{},quarantined{},verified{},settling{},release_verifying{},conversational{};
        runtime::WaitHoldClock hold_clock;
        runtime::WaitPositionGuard position_guard;
        runtime::WaitSettleClock settle_clock;
        core::SnapshotClock::time_point next_verify{};
        std::optional<Observation> observation;
        std::string command;
    };
    struct Job {
        std::string ticket;
        runtime::WaitScriptReceipt receipt;
        std::size_t slot{};
        std::uint64_t revision{};
        bool reset{},waiting{},refresh{},timed_out{};
        core::SnapshotClock::time_point started,install_deadline;
        std::optional<Observation> observation;
    };
    inline static std::array<Lease,256> leases_{};
    inline static std::unordered_map<std::string,Command> commands_;
    inline static std::mutex job_mutex_;
    inline static std::optional<Job> job_;
    inline static bool initialized_{},recovery_requested_{};
    inline static std::uint64_t revision_{};
    inline static core::SnapshotClock::time_point next_sweep_{};
    inline static std::optional<core::SnapshotClock::time_point> paused_at_;
    inline static std::size_t refresh_cursor_{},dirty_cursor_{};
    struct AliasInventory {std::array<RE::ObjectRefHandle,512> actors{};std::size_t count{};};
    inline static AliasInventory recovery_actors_{};
    inline static bool reset_verifying_{};
    inline static runtime::WaitResetClock reset_clock_{};
    inline static core::SnapshotClock::time_point reset_verify_started_{};
    inline static std::uint32_t reset_refresh_actor_{};
    inline static std::size_t reset_refresh_cursor_{};
    inline static bool recovery_overflow_{};

    static bool plugin_equal(std::string_view a,std::string_view b) {
        return std::ranges::equal(a,b,[](unsigned char x,unsigned char y){
            const auto fold=[](unsigned char c){return c>='A'&&c<='Z'?c+('a'-'A'):c;};return fold(x)==fold(y);
        });
    }
    static bool script_current_locked(std::string_view ticket) {
        if(!job_)return false;
        const auto now=core::SnapshotClock::now();
        const auto pause=paused_at_?now-*paused_at_:core::SnapshotClock::duration{};
        return now<job_->started+pause+std::chrono::seconds{30} &&
            (!job_->waiting || now<job_->install_deadline) && job_->receipt.current(ticket);
    }
    static bool current(std::monostate,std::string_view ticket) {
        std::scoped_lock lock{job_mutex_};return script_current_locked(ticket);
    }
    static void finished(std::monostate,std::string_view ticket,bool success) {
        std::scoped_lock lock{job_mutex_};
        if(job_&&job_->ticket==ticket)job_->receipt.finish(ticket,success&&script_current_locked(ticket));
        else recovery_requested_=true;
    }
    static void observed(std::monostate,std::string_view ticket,std::int32_t active,std::int32_t expected,
                         std::int32_t state,std::int32_t sit_state,float) {
        std::scoped_lock lock{job_mutex_};
        if(script_current_locked(ticket))job_->observation=Observation{
            std::bit_cast<std::uint32_t>(active),std::bit_cast<std::uint32_t>(expected),state,sit_state};
    }
    static runtime::RuntimeActionResult pending(std::string detail) {
        return {Status::unavailable,std::move(detail),{},{},true};
    }
    static std::optional<std::uint32_t> active_package_id(RE::Actor& actor) {
        auto* process=actor.currentProcess;if(!process)return {};
        std::optional<TrySpinLock> run;
        if(process->middleHigh) {
            auto& holder=process->middleHigh->runOncePackage;
            run.emplace(holder.packageLock);if(!run->owns_lock())return {};
            if(holder.package)return holder.package->GetFormID();
        }
        const TrySpinLock lock{process->currentPackage.packageLock};
        if(!lock.owns_lock())return {};
        return process->currentPackage.package?process->currentPackage.package->GetFormID():0;
    }
    static std::optional<std::uint32_t> active_owned_package(RE::Actor& actor) {
        const auto active=active_package_id(actor);if(!active)return {};
        auto* data=RE::TESDataHandler::GetSingleton();
        auto* first=data?data->LookupForm<RE::TESPackage>(0x900,"SYNTH.esp"):nullptr;
        auto* legacy=data?data->LookupForm<RE::TESPackage>(0x800,"SYNTH.esp"):nullptr;
        if(!first||!legacy)return {};
        return *active==legacy->GetFormID()||(*active>=first->GetFormID()&&*active-first->GetFormID()<256)?*active:0;
    }
    // Copy only this original quest's bounded alias storage. Never nest an engine getter under its lock.
    static std::optional<AliasInventory> alias_inventory() {
        auto* data=RE::TESDataHandler::GetSingleton();
        auto* owner=data?data->LookupForm<RE::TESQuest>(0x801,"SYNTH.esp"):nullptr;
        if(!owner)return {};
        const TryReadLock lock{owner->aliasAccessLock};if(!lock.owns_lock())return {};
        if(owner->aliases.size()!=512||owner->aliasedHandles.size()>512)return {};
        AliasInventory result;
        for(const auto& references:owner->aliasedHandles) {
            if(references.size()>1)return {}; // This quest has reference aliases, never collection aliases.
            for(const auto& reference:references)if(reference) {
                if(result.count==result.actors.size())return {};
                result.actors[result.count++]=reference;
            }
        }
        return result;
    }
    static void request_recovery(RE::Actor* actor=nullptr) {
        if(actor) {
            AliasInventory inventory;inventory.actors[0]=actor->GetHandle();inventory.count=1;
            remember_recovery_actors(inventory);
        }
        std::scoped_lock lock{job_mutex_};recovery_requested_=true;
    }
    static std::optional<bool> aliases_match_leases() {
        const auto inventory=alias_inventory();if(!inventory)return {};
        std::array<bool,256> seen{};
        for(std::size_t i=0;i<inventory->count;++i) {
            const auto lease=std::ranges::find_if(leases_,[&](const auto& value){
                return value.actor&&RE::ObjectRefHandle{value.actor}==inventory->actors[i];
            });
            if(lease==leases_.end())return false;
            const auto slot=static_cast<std::size_t>(lease-leases_.begin());
            if(seen[slot])return false;
            seen[slot]=true;
        }
        return true;
    }
    static void remember_recovery_actors(const AliasInventory& inventory) {
        for(std::size_t i=0;i<inventory.count;++i) {
            if(std::ranges::any_of(recovery_actors_.actors.begin(),recovery_actors_.actors.begin()+recovery_actors_.count,
                [&](const auto& actor){return actor==inventory.actors[i];}))continue;
            if(recovery_actors_.count==recovery_actors_.actors.size()){recovery_overflow_=true;continue;}
            recovery_actors_.actors[recovery_actors_.count++]=inventory.actors[i];
        }
    }
    static std::uint32_t next_recovery_actor() {
        for(std::size_t offset=0;offset<recovery_actors_.count;++offset) {
            const auto index=(reset_refresh_cursor_+offset)%recovery_actors_.count;
            const auto reference=recovery_actors_.actors[index].get();auto* actor=reference?reference->As<RE::Actor>():nullptr;
            if(!actor||!actor->currentProcess)continue;
            const auto owned=active_owned_package(*actor);
            if(owned&&*owned!=0){reset_refresh_cursor_=(index+1)%recovery_actors_.count;return actor->GetFormID();}
        }
        return 0;
    }
    static runtime::WaitResetProof reset_proof() {
        runtime::WaitResetProof value;
        const auto inventory=alias_inventory();if(!inventory)return value;
        value.aliases_observed=true;value.aliased_references=inventory->count;value.packages_observed=!recovery_overflow_;
        for(std::size_t i=0;i<recovery_actors_.count;++i) {
            const auto reference=recovery_actors_.actors[i].get();auto* actor=reference?reference->As<RE::Actor>():nullptr;
            if(!actor||!actor->currentProcess)continue;
            release_legacy(*actor);
            const auto owned=active_owned_package(*actor);
            if(!owned)value.packages_observed=false;
            else if(*owned!=0)value.owned_package=true;
        }
        return value;
    }
    static runtime::WaitPhysicalProof proof(const Lease& lease) {
        runtime::WaitPhysicalProof value;
        if(lease.observation) {
            const auto flags=lease.observation->state;
            value.target_owned=(flags&7)==7;
            value.aliases_clear=(flags&(256|512))==0;
        }
        const auto actor=lease.actor.get();
        if(!actor||actor->GetFormID()!=lease.form||lease.identity.playthrough_id!=playthrough_id()||
            !plugin_equal(origin_plugin(*actor),lease.identity.origin_plugin))return value;
        const auto aliases=alias_inventory();
        if(aliases) {
            const bool present=std::ranges::any_of(aliases->actors.begin(),aliases->actors.begin()+aliases->count,
                [&](const auto& reference){return reference==RE::ObjectRefHandle{lease.actor};});
            value.target_owned=value.target_owned&&present;
            value.aliases_clear=value.aliases_clear&&!present;
        } else {value.target_owned=false;value.aliases_clear=false;}
        value.restrained=static_cast<RE::ACTOR_LIFE_STATE>(actor->lifeState)==RE::ACTOR_LIFE_STATE::kRestrained;
        value.actor_available=!actor->IsDeleted()&&!actor->IsDisabled()&&!actor->IsDead(false)&&actor->Get3D()&&actor->currentProcess&&
            (actor->lifeState==0||value.restrained);
        const auto* cell=actor->GetParentCell();value.same_cell=cell&&cell->GetFormID()==lease.cell;
        const auto p=point(actor->GetPosition()),q=point(lease.position);
        const auto dx=p.x-q.x,dy=p.y-q.y,dz=p.z-q.z;
        value.distance=std::sqrt(dx*dx+dy*dy+dz*dz);value.speed=actor->DoGetCurrentSpeed();
        const auto owned=active_owned_package(*actor);
        value.package_observed=owned.has_value();value.owned_package=owned.value_or(0);
        value.package_owned=value.owned_package!=0&&value.owned_package==lease.package;
        return value;
    }
    static runtime::WaitPhysicalProof anchor_position(Lease& lease,runtime::WaitPhysicalProof state,bool owner_current) {
        if(lease.position_guard.observe_active_package(state,owner_current)) {
            const auto actor=lease.actor.get();
            if(actor) {
                const auto pending_distance=state.distance;
                lease.position=actor->GetPosition();state=proof(lease);
                REX::INFO("SYNTH wait: actor {:08X} owned package {:08X} start position observed; pending displacement {:.1f}",
                    lease.form,lease.package,pending_distance);
            }
        }
        return state;
    }
    static bool command_current(const Lease& lease,core::SnapshotClock::time_point now) {
        const auto it=commands_.find(lease.command);
        return it==commands_.end()||it->second.result||
            (!it->second.request.cancellation.is_cancelled()&&now<it->second.request.deadline);
    }
    static void close(Lease& lease,std::string reason) {
        if(!lease.actor)return;
        if(!lease.command.empty()) {
            const auto it=commands_.find(lease.command);
            if(it!=commands_.end()&&it->second.failure.empty())it->second.failure=std::move(reason);
        }
        if(lease.waiting||lease.settling)++lease.revision;
        lease.waiting=false;lease.verified=false;lease.settling=false;lease.release_verifying=false;lease.dirty=true;
        std::scoped_lock lock{job_mutex_};
        if(job_&&!job_->reset&&job_->slot==static_cast<std::size_t>(&lease-leases_.data()))job_->receipt.revoke_install();
    }
    static void complete_command(Lease& lease,bool held,const runtime::WaitPhysicalProof& state,std::string detail) {
        const auto it=commands_.find(lease.command);
        if(it==commands_.end()) {lease.command.clear();return;}
        auto& command=it->second;const auto& request=command.request;
        runtime::RuntimeActionResult result;
        const auto now=core::SnapshotClock::now();
        const bool success=request.name==runtime::RuntimeActionName::wait_here?held&&state.held():state.released();
        result.status=success?Status::succeeded:Status::failed;
        result.detail=std::move(detail);
        if(state.actor_available&&state.package_observed)result.wait=runtime::RuntimeWaitObservation{
            request.cancellation.generation(),request.actor,request.action_id,request.name,held,state.restrained,state.owned_package,held&&state.stationary()};
        if(now>=request.deadline) {result.status=Status::timed_out;result.detail="Wait action expired; owned cleanup completed";}
        else if(request.cancellation.is_cancelled()) {result.status=Status::unavailable;result.detail="Wait action cancelled; owned cleanup completed";}
        else if(!command.failure.empty()&&request.name==runtime::RuntimeActionName::wait_here) {
            result.status=Status::failed;result.detail=command.failure;
        }
        if(result.status==Status::succeeded&&(!result.wait||!result.wait->confirms_success(request)))
            result={Status::failed,"Wait physical postcondition was not observed"};
        command.result=std::move(result);lease.command.clear();
    }
    static void clear(Lease& lease) {
        const bool quarantined=lease.quarantined;lease={};lease.quarantined=quarantined;
    }
    // Migration only: clear the old local-800 run-once slot by exact identity, never another owner's package.
    static void release_legacy(RE::Actor& actor) {
        const auto module = REX::FModule::GetExecutingModule();
        if (module.GetFileVersion().pack() != REL::Version{1, 11, 240, 0}.pack()) return;
        static REL::Relocation<std::uintptr_t> clear{REL::ID(2232344)};
        constexpr unsigned char anchor[]{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20};
        if (clear.address() != module.GetBaseAddress() + 0xD34620 ||
            std::memcmp(reinterpret_cast<const void*>(clear.address()), anchor, sizeof(anchor)) != 0) return;
        auto* data = RE::TESDataHandler::GetSingleton();
        auto* old = data ? data->LookupForm<RE::TESPackage>(0x800, "SYNTH.esp") : nullptr;
        if (!old || !actor.currentProcess || !actor.currentProcess->middleHigh) return;
        bool owned{};
        {
            auto& holder = actor.currentProcess->middleHigh->runOncePackage;
            const TrySpinLock lock{holder.packageLock};
            if (lock.owns_lock()) owned = holder.package == old;
        }
        if (owned) actor.currentProcess->SetRunOncePackage(nullptr, &actor);
    }


public:
    static bool bind(RE::BSScript::IVirtualMachine& vm) {
        bool bound=vm.BindNativeMethod(new RE::BSScript::NativeFunction("SYNTHWait","Current",current,false));
        bound&=vm.BindNativeMethod(new RE::BSScript::NativeFunction("SYNTHWait","Finished",finished,false));
        bound&=vm.BindNativeMethod(new RE::BSScript::NativeFunction("SYNTHWait","Observed",observed,false));
        return bound;
    }
    static void retire() {
        std::scoped_lock lock{job_mutex_};job_.reset();leases_={};commands_.clear();initialized_=false;
        recovery_requested_=false;next_sweep_={};paused_at_.reset();refresh_cursor_=0;dirty_cursor_=0;
        recovery_actors_={};reset_verifying_=false;reset_clock_={};reset_verify_started_={};
        reset_refresh_actor_=0;reset_refresh_cursor_=0;recovery_overflow_=false;
    }
    static void cancel_all() {for(auto& lease:leases_)if(lease.actor)close(lease,"Wait stopped; owned hold released");}
    static void note_menu_pause(core::SnapshotClock::time_point now) {
        std::scoped_lock lock{job_mutex_};if(!paused_at_)paused_at_=now;
    }
    static bool explicit_wait_owned(std::uint32_t form) {
        return std::ranges::any_of(leases_,[&](const auto& lease){return lease.form==form&&lease.actor&&!lease.conversational;});
    }
    static bool action_actor_busy(std::uint32_t form) {
        {
            std::scoped_lock lock{job_mutex_};
            if(!initialized_||recovery_requested_||(job_&&job_->timed_out))return true;
        }
        const auto it=std::ranges::find_if(leases_,[&](const auto& lease){return lease.form==form&&lease.actor;});
        return it!=leases_.end()&&(!it->command.empty()||it->dirty||!it->waiting||it->settling);
    }
    static std::optional<bool> explicit_wait_observation(std::uint32_t form) {
        {
            std::scoped_lock lock{job_mutex_};
            if(!initialized_||recovery_requested_||(job_&&job_->timed_out))return {};
        }
        const auto it=std::ranges::find_if(leases_,[&](const auto& lease){return lease.form==form&&lease.actor&&!lease.conversational;});
        if(it==leases_.end()) {
            auto* actor=RE::TESForm::GetFormByID<RE::Actor>(form);if(!actor)return {};
            const auto aliases=aliases_match_leases();if(!aliases)return {};
            if(!*aliases){request_recovery();return {};}
            const auto owned=active_owned_package(*actor);
            if(owned&&*owned!=0){request_recovery(actor);return {};}
            return owned&&*owned==0?std::optional{false}:std::nullopt;
        }
        if(!it->waiting||it->dirty||!it->verified||it->cancellation.is_cancelled()||it->hold_clock.expired(core::SnapshotClock::now()))return {};
        return proof(*it).held()?std::optional{true}:std::nullopt;
    }
    static bool conversation_actions_ready() {
        return std::ranges::none_of(leases_,[](const auto& lease){return lease.conversational&&lease.actor;});
    }
    static bool conversation_held(std::uint32_t form) {
        const auto it=std::ranges::find_if(leases_,[&](const auto& lease){return lease.form==form&&lease.actor&&lease.conversational;});
        return it!=leases_.end()&&it->waiting&&it->verified&&!it->cancellation.is_cancelled()&&proof(*it).held();
    }
    static void update_conversation(std::span<const runtime::RuntimeConversationHold>,bool discard) {
        if(discard)return;
        for(auto& lease:leases_)if(lease.conversational&&lease.actor)close(lease,"Conversational hold retired");
    }
    static runtime::RuntimeActionResult apply(RE::Actor& actor,const runtime::RuntimeActionRequest& request) {
        const auto now=core::SnapshotClock::now();
        if(request.action_id.empty()||request.action_id.size()>128||
            (request.name!=runtime::RuntimeActionName::wait_here&&request.name!=runtime::RuntimeActionName::release_wait)||
            request.actor.form_id!=actor.GetFormID()||request.actor.playthrough_id!=playthrough_id()||
            !plugin_equal(origin_plugin(actor),request.actor.origin_plugin))return {Status::rejected,"Wait requires its exact canonical actor and action ID"};
        if(const auto command=commands_.find(request.action_id);command!=commands_.end()) {
            if(!runtime::same_wait_request(command->second.request,request))return {Status::rejected,"Wait action identity changed"};
            if(command->second.result)return *command->second.result;
            return pending("Wait action pending; physical ownership has not completed");
        }
        std::erase_if(commands_,[&](const auto& entry){return entry.second.result&&now>=entry.second.request.deadline+std::chrono::seconds{1};});
        if(request.cancellation.is_cancelled()||now>=request.deadline||
            (request.wait_session&&(request.wait_session->is_cancelled()||request.wait_session->generation()!=request.cancellation.generation())))
            return {now>=request.deadline?Status::timed_out:Status::unavailable,"Wait owner cancelled or expired before admission"};
        if(menu_mode_active())return pending("Wait action paused by menu");
        if(actor.GetFormID()==0x14||actor.IsDeleted()||actor.IsDisabled()||actor.IsDead(false)||!actor.Get3D()||!actor.currentProcess)
            return {Status::unavailable,"Wait requires the selected live, loaded NPC"};
        auto* data=RE::TESDataHandler::GetSingleton();
        if(!data||!data->LookupForm<RE::TESQuest>(0x801,"SYNTH.esp"))return {Status::unavailable,"Enable the updated SYNTH.esp wait controller"};
        {
            std::scoped_lock lock{job_mutex_};
            if(!initialized_||recovery_requested_||(job_&&job_->timed_out))return pending("Wait alias recovery pending");
        }
        const auto aliases=aliases_match_leases();
        if(!aliases)return pending("Wait alias observation pending");
        if(!*aliases){request_recovery();return pending("Unexpected saved Wait alias requires recovery");}
        const auto native_handle=actor.GetHandle();RE::ActorHandle handle;
        static_assert(sizeof(handle)==sizeof(native_handle));std::memcpy(static_cast<void*>(&handle),&native_handle,sizeof(handle));
        if(!handle)return {Status::unavailable,"NPC reference handle is unavailable"};
        auto lease=std::ranges::find_if(leases_,[&](const auto& value){return value.actor==handle;});
        const bool releasing=request.name==runtime::RuntimeActionName::release_wait;
        release_legacy(actor);
        if(lease==leases_.end()) {
            const auto owned=active_owned_package(actor);
            if(!owned)return pending("Wait package observation pending");
            if(*owned!=0){request_recovery(&actor);return pending("Untracked owned Wait package requires recovery");}
        }
        if(lease!=leases_.end()&&(!lease->command.empty()||!lease->waiting||lease->dirty||lease->settling))return pending("Previous wait ownership is still settling or releasing");
        if(!releasing&&lease!=leases_.end()) {
            std::scoped_lock lock{job_mutex_};
            if(job_&&!job_->reset&&job_->slot==static_cast<std::size_t>(lease-leases_.begin()))
                return pending("Previous wait verification must drain before renewal");
        }
        if(commands_.size()>=2048)return {Status::unavailable,"Wait action receipt capacity reached"};
        if(lease==leases_.end()&&releasing) {
            const auto owned=active_owned_package(actor);
            if(!owned||*owned!=0)return pending("Release Wait awaits fresh absence of the owned package");
            runtime::RuntimeActionResult result;
            const bool restrained=static_cast<RE::ACTOR_LIFE_STATE>(actor.lifeState)==RE::ACTOR_LIFE_STATE::kRestrained;
            result.status=Status::succeeded;
            result.detail="No SYNTH Wait package remains; unrelated restraint is unchanged";
            result.wait=runtime::RuntimeWaitObservation{request.cancellation.generation(),request.actor,request.action_id,request.name,false,restrained,0,false};
            commands_.emplace(request.action_id,Command{request,result,{}});return result;
        }
        if(lease==leases_.end()) {
            if(actor.lifeState!=0&&static_cast<RE::ACTOR_LIFE_STATE>(actor.lifeState)!=RE::ACTOR_LIFE_STATE::kRestrained)
                return {Status::unavailable,"NPC is unavailable; no Wait package ownership claimed"};
            lease=std::ranges::find_if(leases_,[](const auto& value){return !value.actor&&!value.quarantined;});
            if(lease==leases_.end())return {Status::unavailable,"Wait capacity reached"};
        }
        if(lease->quarantined&&!releasing)return {Status::unavailable,"Wait script timed out; reload before reusing its slot"};
        const auto* cell=actor.GetParentCell();if(!cell)return {Status::unavailable,"NPC cell is unavailable"};
        auto* package=data->LookupForm<RE::TESPackage>(0x900+static_cast<std::uint32_t>(lease-leases_.begin()),"SYNTH.esp");
        if(!package)return {Status::unavailable,"Owned Wait package record is unavailable"};
        const bool quarantined=lease->quarantined;
        *lease={};lease->actor=handle;lease->identity=request.actor;lease->form=actor.GetFormID();lease->cell=cell->GetFormID();
        lease->package=package->GetFormID();
        lease->cancellation=request.wait_session.value_or(request.cancellation);lease->position=actor.GetPosition();
        lease->revision=++revision_;lease->waiting=!releasing;lease->dirty=true;lease->quarantined=quarantined;lease->command=request.action_id;
        commands_.emplace(request.action_id,Command{request,{},{}});
        {
            std::scoped_lock lock{job_mutex_};
            if(job_&&!job_->reset&&job_->slot==static_cast<std::size_t>(lease-leases_.begin()))job_->receipt.revoke_install();
        }
        return pending(releasing?"Release Wait queued; awaiting alias and package removal":"Wait Here queued; awaiting active owned package and stopped position");
    }
    static void tick(core::SnapshotClock::time_point now) {
        if(menu_mode_active()) {note_menu_pause(now);return;}
        {
            std::scoped_lock lock{job_mutex_};
            if(paused_at_) {
                if(job_)job_->started+=now-*paused_at_;
                for(auto& lease:leases_)lease.hold_clock.pause(now-*paused_at_);
                paused_at_.reset();next_sweep_={};
            }
        }
        if(now<next_sweep_)return;next_sweep_=now+std::chrono::milliseconds{250};
        if(REX::FModule::GetExecutingModule().GetFileVersion().pack()!=REL::Version{1,11,240,0}.pack())return;
        bool recover{};
        {
            std::scoped_lock lock{job_mutex_};
            if(recovery_requested_) {
                if(job_&&!job_->receipt.done())job_->receipt.revoke_install();
                else {job_.reset();recovery_requested_=false;initialized_=false;reset_verifying_=false;reset_clock_={};reset_refresh_actor_=0;recover=true;}
            }
        }
        if(recover) {
            for(auto& lease:leases_)if(lease.actor)close(lease,"A stale saved stack required owned wait recovery");
            REX::WARN("SYNTH wait: late saved stack drained; recovering owned wait aliases");
        }
        if(reset_verifying_) {
            const auto state=reset_proof();
            if(reset_clock_.observe(state,now)) {
                initialized_=true;reset_verifying_=false;recovery_actors_={};reset_refresh_actor_=0;
                for(auto& lease:leases_)if(lease.actor) {
                    lease.observation=Observation{0,0,3,0};lease.release_verifying=true;
                    lease.next_verify=now+std::chrono::seconds{1};
                }
                REX::INFO("SYNTH wait: reset independently verified; owned aliases and observed packages absent");
            } else if((state.aliases_observed&&state.aliased_references!=0)||now-reset_verify_started_>=std::chrono::seconds{1}) {
                reset_refresh_actor_=state.aliases_observed&&state.aliased_references==0?next_recovery_actor():0;
                reset_verifying_=false;reset_clock_={};
            } else return;
        }
        if(initialized_) {
            const auto aliases=aliases_match_leases();if(!aliases)return;
            if(!*aliases){request_recovery();return;}
        }
        for(auto& lease:leases_)if(lease.actor) {
            auto state=proof(lease);
            if(!lease.waiting) {
                if(lease.release_verifying&&state.released()) {
                    complete_command(lease,false,state,"Release Wait verified: owned aliases and package removed; unrelated restraint unchanged");
                    REX::INFO("SYNTH wait: actor {:08X} package release verified; remaining restraint={}",lease.form,state.restrained);
                    clear(lease);
                } else if(lease.release_verifying&&now>=lease.next_verify) {
                    lease.release_verifying=false;lease.dirty=true;
                }
                continue;
            }
            const bool session_cancelled=lease.cancellation.is_cancelled();
            const bool lease_expired=lease.hold_clock.expired(now);
            const bool command_current_now=command_current(lease,now);
            const bool owner_current=!session_cancelled&&!lease_expired&&command_current_now;
            const bool position_interrupted=lease.position_guard.interrupted(state);
            if(!owner_current||position_interrupted) {
                const bool distance_finite=std::isfinite(state.distance),speed_finite=std::isfinite(state.speed);
                const char* reason=session_cancelled?"session_cancelled":lease_expired?"lease_expired":
                    !command_current_now?"command_inactive":!state.actor_available?"actor_unavailable":
                    !state.same_cell?"cell_changed":(!distance_finite||state.distance<0)?"invalid_position":"escaped";
                const auto actor=lease.actor.get();
                const auto native=actor?active_package_id(*actor):std::optional<std::uint32_t>{};
                const bool vm_observed=lease.observation.has_value();
                REX::WARN("SYNTH wait: interrupted reason={} session_cancelled={} lease_expired={} command_current={} actor_available={} same_cell={} anchor={} package_observed={} package_owned={} target_owned={} distance_finite={} distance={:.1f} speed_finite={} speed={:.1f} life_state={} vm_observed={} vm_package_owned={} native_observed={} native_package_owned={} vm_native_agree={}",
                    reason,session_cancelled,lease_expired,command_current_now,state.actor_available,state.same_cell,
                    lease.position_guard.anchored(),state.package_observed,state.package_owned,state.target_owned,
                    distance_finite,distance_finite?state.distance:0.0,speed_finite,speed_finite?state.speed:0.0,
                    actor?life_state(*actor):std::string{"unavailable"},vm_observed,
                    vm_observed&&lease.observation->active==lease.package,native.has_value(),native&&*native==lease.package,
                    vm_observed&&native&&lease.observation->active==*native);
                close(lease,"Wait interrupted or expired; owned hold cleanup required");
            } else {
                state=anchor_position(lease,state,owner_current);
                if(lease.settling&&lease.position_guard.anchored()&&lease.settle_clock.observe(state,now)) {
                    lease.settling=false;lease.verified=true;lease.hold_clock.start(now);
                    complete_command(lease,true,state,"Wait Here verified: active owned package and stopped near its package-start position");
                    REX::INFO("SYNTH wait: actor {:08X} owned package {:08X} verified; displacement {:.1f}, speed {:.1f}",lease.form,state.owned_package,state.distance,state.speed);
                }
            }
        }
        std::optional<Job> completed;bool draining{},warn{};
        {
            std::scoped_lock lock{job_mutex_};
            if(job_) {
                if(!job_->receipt.done()) {
                    if(now-job_->started<std::chrono::seconds{30})return;
                    draining=true;
                    if(!job_->timed_out){job_->timed_out=true;job_->receipt.revoke_install();warn=true;}
                } else {completed=std::move(job_);job_.reset();}
            }
        }
        if(draining) {
            if(warn) {
                for(auto& lease:leases_)if(lease.actor)close(lease,"Wait VM timed out; owned stack must drain before cleanup");
                REX::WARN("SYNTH wait: VM timeout; no alias release or reuse until matching Finished. Reload if it never drains");
            }
            return;
        }
        if(completed) {
            bool ok=completed->receipt.success();
            if(completed->reset) {
                initialized_=false;reset_verifying_=ok;reset_clock_={};reset_verify_started_=now;
                if(ok) {
                    for(auto& lease:leases_)if(lease.actor) {
                        lease.waiting=false;lease.verified=false;lease.settling=false;lease.dirty=false;
                        lease.release_verifying=false;
                    }
                }
                else next_sweep_=now+std::chrono::seconds{5};
                REX::INFO("SYNTH wait: alias controller reset {}",ok?"acknowledged; native verification pending":"failed; retry deferred");
            } else {
                auto& lease=leases_[completed->slot];
                if(completed->observation)lease.observation=completed->observation;
                const auto state=proof(lease);
                if(completed->waiting) {
                    ok=ok&&state.target_owned;
                    if(lease.revision==completed->revision&&lease.waiting&&ok) {
                        if(!completed->refresh) {
                            lease.settling=true;
                            const bool owner_current=!lease.cancellation.is_cancelled()&&command_current(lease,now);
                            const auto anchored=anchor_position(lease,state,owner_current);
                            if(lease.position_guard.anchored())(void)lease.settle_clock.observe(anchored,now);
                        }
                        else {lease.verified=state.held();lease.next_verify=now+std::chrono::seconds{1};}
                    } else if(lease.waiting)close(lease,"Wait alias ownership could not be verified");
                } else {
                    ok=ok&&state.aliases_clear;
                    if(ok) {
                        lease.release_verifying=true;lease.next_verify=now+std::chrono::seconds{1};
                        if(state.released()) {
                            complete_command(lease,false,state,"Release Wait verified: owned aliases and package removed; unrelated restraint unchanged");clear(lease);
                        }
                    }
                    else {lease.dirty=true;next_sweep_=now+std::chrono::seconds{2};}
                }
                if(completed->timed_out)lease.quarantined=true;
                if(!completed->refresh||!ok)REX::INFO("SYNTH wait: slot {} owned {} {}",completed->slot,
                    completed->waiting?"alias assignment":"alias release",ok?"confirmed":"not confirmed");
            }
            if(!initialized_||now<next_sweep_-std::chrono::milliseconds{250})return;
        }
        std::erase_if(commands_,[&](const auto& entry){return entry.second.result&&now>=entry.second.request.deadline+std::chrono::seconds{1};});
        {
            std::scoped_lock lock{job_mutex_};if(recovery_requested_)return;
        }
        std::size_t slot{};bool refresh{};
        if(initialized_) {
            slot=leases_.size();
            for(std::size_t offset=0;offset<leases_.size();++offset) {
                const auto index=(dirty_cursor_+offset)%leases_.size();
                if(leases_[index].actor&&leases_[index].dirty) {slot=index;dirty_cursor_=(index+1)%leases_.size();break;}
            }
            if(slot==leases_.size())for(std::size_t offset=0;offset<leases_.size();++offset) {
                const auto index=(refresh_cursor_+offset)%leases_.size();const auto& lease=leases_[index];
                if(lease.actor&&lease.waiting&&!lease.settling&&now>=lease.next_verify) {slot=index;refresh=true;refresh_cursor_=(index+1)%leases_.size();break;}
            }
            if(slot==leases_.size())return;
        }
        auto* game=RE::GameVM::GetSingleton();const auto vm=game?game->GetVM():nullptr;
        auto* data=RE::TESDataHandler::GetSingleton();auto* owner=data?data->LookupForm<RE::TESQuest>(0x801,"SYNTH.esp"):nullptr;
        if(!vm||!owner)return;
        auto& lease=leases_[slot];const bool reset=!initialized_;const bool waiting=!reset&&lease.waiting;
        if(reset) {
            const auto inventory=alias_inventory();if(!inventory)return;
            remember_recovery_actors(*inventory);
            if(inventory->count!=0)reset_refresh_actor_=0;
        }
        const auto actor=lease.actor.get();const auto* cell=actor?actor->GetParentCell():nullptr;
        if(waiting&&(!actor||!cell||!command_current(lease,now))) {close(lease,"Wait actor became unavailable before dispatch");return;}
        core::CancellationToken token=lease.cancellation;
        auto install_deadline=core::SnapshotClock::time_point::max();
        if(waiting&&!lease.command.empty())if(const auto it=commands_.find(lease.command);it!=commands_.end()) {
            token=it->second.request.cancellation;install_deadline=it->second.request.deadline;
        }
        std::random_device entropy;std::string ticket;for(int part=0;part<4;++part)ticket+=std::to_string(entropy())+"-";
        {
            std::scoped_lock lock{job_mutex_};
            job_=Job{ticket,runtime::WaitScriptReceipt{ticket,token,waiting},slot,lease.revision,reset,waiting,refresh,false,now,install_deadline,{}};
        }
        const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        const auto owner_id=std::bit_cast<std::int32_t>(owner->GetFormID());
        const auto actor_id=std::bit_cast<std::int32_t>(lease.form);
        const auto cell_id=cell?std::bit_cast<std::int32_t>(cell->GetFormID()):0;
        NativeFaultRecorder::phase(reset?"wait Reset dispatch":"wait Apply dispatch");
        bool queued{};
        if(reset&&reset_refresh_actor_!=0)queued=vm->DispatchStaticCall("SYNTHWait","ReleaseByFormIds",callback,ticket,std::int32_t{0},owner_id,
            std::bit_cast<std::int32_t>(reset_refresh_actor_));
        else if(reset)queued=vm->DispatchStaticCall("SYNTHWait","ResetByFormId",callback,ticket,owner_id);
        else if(!waiting)queued=vm->DispatchStaticCall("SYNTHWait","ReleaseByFormIds",callback,ticket,static_cast<std::int32_t>(slot),owner_id,actor_id);
        else if(refresh)queued=vm->DispatchStaticCall("SYNTHWait","RefreshByFormIds",callback,ticket,owner_id,static_cast<std::int32_t>(slot),actor_id);
        else queued=vm->DispatchStaticCall("SYNTHWait","ApplyByFormIds",callback,ticket,static_cast<std::int32_t>(slot),owner_id,actor_id,
            std::int32_t{0},cell_id,lease.position.x,lease.position.y,lease.position.z,true);
        NativeFaultRecorder::phase("wait dispatch returned");
        if(queued&&!reset)lease.dirty=false;
        if(!queued)finished({},ticket,false);
    }
};
} // namespace synth::adapters::commonlib
