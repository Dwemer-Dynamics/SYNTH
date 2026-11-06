#pragma once
#include "runtime/wait_recovery.hpp"
#include <limits>
#include <stdexcept>

namespace synth::tests {
inline int wait_verification_checks() {
    int checks{};
    const auto check=[&](bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);};
    core::CancellationSource turn{core::RuntimeGeneration::initial()};
    const auto now=core::SnapshotClock::now();
    runtime::WaitResetProof reset{true,true,false,0};
    check(reset.cleared(),"fresh empty aliases and package absence did not clear recovery");
    for(int field=0;field<4;++field) {
        auto unknown=reset;
        if(field==0)unknown.aliases_observed=false;
        if(field==1)unknown.packages_observed=false;
        if(field==2)unknown.owned_package=true;
        if(field==3)unknown.aliased_references=1;
        check(!unknown.cleared(),"Reset acknowledgement hid unknown ownership or a saved ghost hold");
    }
    runtime::WaitResetClock reset_clock;
    check(!reset_clock.observe(reset,now),"one clean reset sample enabled new Wait admission");
    auto ghost=reset;ghost.aliased_references=1;
    check(!reset_clock.observe(ghost,now+std::chrono::milliseconds{100}),"late saved alias retained reset proof");
    check(!reset_clock.observe(reset,now+std::chrono::milliseconds{200}),"late saved mutation did not restart recovery verification");
    check(reset_clock.observe(reset,now+std::chrono::milliseconds{300}),"separated clean reset observations did not finish recovery");
    runtime::RuntimeActionRequest request{runtime::RuntimeActionName::wait_here,{0x15,"Actors.esm","save-a"},{},turn.token(),now+std::chrono::seconds{10},"wait:exact",{},1,{}};
    const runtime::RuntimeWaitObservation hold{turn.token().generation(),request.actor,request.action_id,request.name,true,false,0x01000900,true};
    check(hold.valid_for(request)&&hold.confirms_success(request),"exact held observation was not accepted");
    runtime::RuntimeActionResult old{runtime::RuntimeActionStatus::succeeded,"Wait Here queued",{},{},false,{}};
    check(!old.verified_wait_for(request),"old queued success became physical proof");
    old.wait=hold;old.pending=true;
    check(!old.verified_wait_for(request),"pending admission became terminal success");
    old.pending=false;
    check(old.verified_wait_for(request),"completed owned hold was rejected");
    for(int field=0;field<8;++field) {
        auto other=hold;
        if(field==0)other.action_id="wait:other";
        if(field==1)other.actor.form_id=0x16;
        if(field==2)other.actor.origin_plugin="Other.esm";
        if(field==3)other.actor.playthrough_id="save-b";
        if(field==4)other.generation=core::RuntimeGeneration::from_value(2);
        if(field==5)other.name=runtime::RuntimeActionName::release_wait;
        if(field==6)other.actor.form_id=0x14;
        if(field==7)other.action_id.clear();
        check(!other.valid_for(request),"foreign wait observation matched an action");
    }
    for(int mask=0;mask<4;++mask) {
        auto other=hold;other.owned_hold=(mask&1)!=0;other.stationary=(mask&2)!=0;
        check(other.confirms_success(request)==(mask==3),"unowned or moving package became a successful wait");
    }
    auto no_package=hold;no_package.owned_package=0;
    check(!no_package.confirms_success(request),"missing package became a successful wait");
    auto release=request;release.name=runtime::RuntimeActionName::release_wait;release.action_id="release:exact";
    runtime::RuntimeWaitObservation freed{turn.token().generation(),release.actor,release.action_id,release.name,false,false,0,false};
    check(freed.confirms_success(release),"verified package release was rejected");
    freed.restrained=true;
    check(freed.confirms_success(release),"foreign restraint prevented release of SYNTH's own package");
    freed.owned_package=0x01000900;
    check(!freed.confirms_success(release),"active Wait package became release proof");
    freed.owned_package=0;
    freed.restrained=false;freed.owned_hold=true;
    check(!freed.confirms_success(release),"owned hold became release proof");
    check(runtime::same_wait_request(request,request),"idempotent wait poll changed identity");
    auto extended=request;extended.deadline+=std::chrono::hours{1};
    check(runtime::same_wait_request(request,extended),"poll deadline renewal changed action identity instead of retaining original deadline");
    for(int field=0;field<5;++field) {
        auto other=request;
        if(field==0)other.action_id="wait:other";
        if(field==1)other.actor.form_id=0x16;
        if(field==2)other.actor.playthrough_id="save-b";
        if(field==3)other.actor.origin_plugin="Other.esm";
        if(field==4)other.name=runtime::RuntimeActionName::release_wait;
        check(!runtime::same_wait_request(request,other),"another wait command reused an owned receipt");
    }
    runtime::WaitPhysicalProof physical{true,true,true,false,false,true,0,0,true,0x01000900};
    check(physical.held(),"owned stopped actor was rejected");
    runtime::WaitPositionGuard delayed_apply;
    auto pending_walk=physical;
    pending_walk.target_owned=false;pending_walk.package_owned=false;pending_walk.owned_package=0;
    pending_walk.speed=120;
    for(double position: {32.0,128.0,720.0,1320.0}) {
        pending_walk.distance=position;
        check(!delayed_apply.interrupted(pending_walk),"walking during delayed Apply was mistaken for escape from an active hold");
        check(!delayed_apply.observe_active_package(pending_walk,true),"pending Apply established a hold anchor without owned alias/package");
    }
    auto active_start=physical;active_start.distance=1320;active_start.speed=120;
    check(delayed_apply.observe_active_package(active_start,true),"first exact owned package could not establish its package-start anchor");
    check(!active_start.held(),"active package plus movement became physical waiting");
    active_start.distance=0; // Native anchor transition copies the actor's current position once.
    runtime::WaitSettleClock delayed_settle;
    check(!delayed_settle.observe(active_start,now+std::chrono::seconds{11}),"moving package start completed waiting");
    active_start.speed=0;
    check(!delayed_settle.observe(active_start,now+std::chrono::milliseconds{11100}),"first stopped sample completed waiting");
    check(!delayed_settle.observe(active_start,now+std::chrono::milliseconds{11199}),"delayed Apply weakened the 100ms stationary proof");
    check(delayed_settle.observe(active_start,now+std::chrono::milliseconds{11200}),"delayed Apply could not finish after two strict stopped samples");
    auto escaped=active_start;escaped.distance=65;
    check(delayed_apply.interrupted(escaped),"verified hold could escape its fixed package-start position");
    check(!delayed_apply.observe_active_package(escaped,true),"an active hold silently re-anchored after escaping");
    for(int field=0;field<4;++field) {
        runtime::WaitPositionGuard unknown;
        auto other=physical;
        if(field==0)other.actor_available=false;
        if(field==1)other.same_cell=false;
        if(field==2)other.distance=std::numeric_limits<double>::infinity();
        if(field==3)other.distance=-1;
        check(unknown.interrupted(other),"pending installation relaxed actor/cell/valid-position guards");
    }
    for(int field=0;field<4;++field) {
        runtime::WaitPositionGuard unowned;
        auto other=physical;
        if(field==0)other.target_owned=false;
        if(field==1)other.package_owned=false;
        if(field==2)other.package_observed=false;
        if(field==3)other.owned_package=0;
        check(!unowned.observe_active_package(other,true),"foreign or unknown package established the hold anchor");
    }
    core::CancellationSource cancelled_install{core::RuntimeGeneration::initial()};
    runtime::WaitScriptReceipt cancelled_receipt{"cancelled-install",cancelled_install.token(),true};
    runtime::WaitPositionGuard cancelled_anchor;
    cancelled_install.cancel();
    check(!cancelled_anchor.observe_active_package(physical,!cancelled_install.token().is_cancelled()),"cancelled pending work established a late hold anchor");
    cancelled_receipt.finish("cancelled-install",true);
    check(!cancelled_receipt.success()&&!cancelled_anchor.anchored(),"late cancelled Apply became successful physical waiting");
    runtime::WaitPositionGuard after_reload;
    const auto fresh_generation=core::RuntimeGeneration::from_value(2);
    check(!after_reload.observe_active_package(physical,!cancelled_install.token().is_cancelled()&&
        cancelled_install.token().generation()==fresh_generation),"retired generation's Apply anchored a hold after reload");
    check(!after_reload.anchored(),"reload retained the old hold's position ownership");
    runtime::WaitSettleClock settle;
    check(!settle.observe(physical,now),"one stopped sample became completed waiting");
    check(!settle.observe(physical,now+std::chrono::milliseconds{99}),"settle window was shortened");
    check(settle.observe(physical,now+std::chrono::milliseconds{100}),"two separated stopped samples were rejected");
    auto moving=physical;moving.speed=20;
    check(!settle.observe(moving,now+std::chrono::milliseconds{110}),"movement retained stale stopped proof");
    check(!settle.observe(physical,now+std::chrono::milliseconds{120}),"movement interruption did not restart settling");
    check(settle.observe(physical,now+std::chrono::milliseconds{220}),"fresh settling after interruption was rejected");
    for(int field=0;field<10;++field) {
        auto other=physical;
        if(field==0)other.actor_available=false;
        if(field==1)other.target_owned=false;
        if(field==2)other.package_owned=false;
        if(field==3)other.package_observed=false;
        if(field==4)other.same_cell=false;
        if(field==5)other.distance=65;
        if(field==6)other.speed=6;
        if(field==7)other.distance=std::numeric_limits<double>::quiet_NaN();
        if(field==8)other.speed=std::numeric_limits<double>::infinity();
        if(field==9)other.distance=-1;
        check(!other.held(),"missing physical wait postcondition was accepted");
    }
    physical.aliases_clear=true;physical.target_owned=false;physical.package_owned=false;physical.restrained=false;physical.owned_package=0;
    check(physical.released(),"empty aliases and native package absence did not prove release");
    physical.restrained=true;check(physical.released(),"release tried to own or remove foreign restraint");
    physical.owned_package=0x01000900;check(!physical.released(),"alias clearing alone proved package removal");
    physical.owned_package=0;physical.aliases_clear=false;
    check(!physical.released(),"native package absence alone cleared somebody else's alias receipt");
    physical.aliases_clear=true;physical.package_observed=false;
    check(!physical.released(),"unknown package state became known absence");
    turn.cancel();
    check(!hold.valid_for(request),"cancelled wait observation remained valid");
    check(!old.verified_wait_for(request),"cancelled completed proof remained successful");
    return checks;
}
} // namespace synth::tests
