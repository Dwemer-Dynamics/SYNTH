#pragma once
#include "runtime/fake_fallout_runtime.hpp"
#include <stdexcept>
#include <thread>

namespace synth::tests {
inline int social_visit_contract_checks() {
    int checks{};
    const auto check=[&](bool value,const char* reason) {++checks;if(!value)throw std::runtime_error(reason);};
    core::CancellationSource lifetime{core::RuntimeGeneration::initial()};
    const runtime::RuntimeSocialVisitRequest owned{"task:1","execution:1",{0x1234,"Actors.esm","save-a"},
        {0x1235,"Actors.esm","save-a"},lifetime.token(),core::SnapshotClock::now()+std::chrono::seconds{20},false};
    check(owned.valid_ids(),"valid social visit was rejected");
    auto dotted=owned;dotted.execution_id="visit:session.1:2:3";
    check(dotted.valid_ids(),"wire-valid dotted session identity was rejected");
    auto release=owned;release.release=true;
    check(owned.same_execution(release),"release lost exact execution ownership");
    auto folded=owned;folded.actor.origin_plugin="ACTORS.ESM";
    check(owned.same_execution(folded),"canonical plugin case changed identity");
    for(int field=0;field<8;++field) {
        auto foreign=owned;
        if(field==0)foreign.task_id="task:2";
        if(field==1)foreign.execution_id="execution:2";
        if(field==2)foreign.actor.form_id=0x1236;
        if(field==3)foreign.listener.form_id=0x1236;
        if(field==4)foreign.actor.origin_plugin="Other.esm";
        if(field==5)foreign.listener.origin_plugin="Other.esm";
        if(field==6)foreign.actor.playthrough_id="other-save";
        if(field==7)foreign.listener.playthrough_id="other-save";
        check(!owned.same_execution(foreign),"different task/actor/save could reuse visit ownership");
    }
    core::GenerationClock clock;(void)clock.advance();core::CancellationSource loaded{clock.current()};
    auto stale=owned;stale.cancellation=loaded.token();
    check(!owned.same_execution(stale),"load generation could reuse a previous visit execution");
    for(int field=0;field<10;++field) {
        auto malformed=owned;
        if(field==0)malformed.task_id.clear();
        if(field==1)malformed.execution_id=std::string(129,'x');
        if(field==2)malformed.task_id="task/other";
        if(field==3)malformed.actor.form_id=0x14;
        if(field==4)malformed.listener.form_id=0x14;
        if(field==5)malformed.listener=malformed.actor;
        if(field==6)malformed.actor.origin_plugin="../Actors.esm";
        if(field==7)malformed.listener.playthrough_id="other-save";
        if(field==8)malformed.actor.form_id=0;
        if(field==9)malformed.execution_id=".session";
        check(!malformed.valid_ids(),"malformed visit could enter native ownership");
    }
    runtime::FakeFalloutRuntime unsupported{core::RuntimeVariant::vr};
    const auto result=unsupported.social_visit(owned);
    check(result.status==runtime::RuntimeSocialVisitStatus::unavailable,"unimplemented adapter claimed a visit");
    check(result.task_id==owned.task_id && result.execution_id==owned.execution_id,"unsupported adapter lost request correlation");
    bool worker_rejected{};
    std::thread worker{[&]{try{(void)unsupported.social_visit(owned);}catch(const std::logic_error&){worker_rejected=true;}}};
    worker.join();check(worker_rejected,"social visit accessed runtime from a worker");
    return checks;
}
} // namespace synth::tests
