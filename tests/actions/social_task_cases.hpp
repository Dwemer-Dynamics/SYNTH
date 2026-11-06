#pragma once
#include "runtime/social_task_codec.hpp"
#include <stdexcept>

namespace synth::tests {
inline int social_task_contract_checks() {
    using namespace runtime;
    int count{};
    const auto check=[&](bool value) {++count;if(!value)throw std::runtime_error{"social task contract check failed at "+std::to_string(count)};};
    const auto rejects=[&](auto operation) {try{operation();return false;}catch(const std::invalid_argument&){return true;}};
    const RuntimeActorIdentity player{0x14,"Fallout4.esm","save-a"},performer{0x15,"Fallout4.esm","save-a"},recipient{0x16,"Fallout4.esm","save-a"};
    SocialTaskDescriptor task{"social:0123456789abcdef0123456789abcdef",1,performer,recipient,"the water pump","now","","ready",{},{},"Codsworth","Jun"};
    check(social_task_valid(task));
    for(int invalid=0;invalid<9;++invalid) {
        auto bad=task;
        if(invalid==0)bad.task_id.back()='g';
        if(invalid==1)bad.performer=player;
        if(invalid==2)bad.recipient=performer;
        if(invalid==3)bad.recipient.playthrough_id="other-save";
        if(invalid==4)bad.topic=std::string(513,'x');
        if(invalid==5)bad.topic=std::string{"x\0x",3};
        if(invalid==6)bad.status="promise";
        if(invalid==7)bad.when="on_return";
        if(invalid==8)bad.performer.origin_plugin="../Fallout4.esm";
        check(!social_task_valid(bad));
    }
    check(social_same_actor(performer,{0x15,"fallout4.esm","save-a"}));
    auto encoded=encode_social_task(task);check(decode_social_task(encoded).recipient_name=="Jun");
    auto fields=encoded.as_object();fields.emplace_back("pretend_completed",true);encoded=json::Value{std::move(fields)};check(rejects([&]{(void)decode_social_task(encoded);}));
    const auto bytes=encode_social_task_reminders(std::span{&task,1});const auto restored=decode_social_task_reminders(bytes);
    check(restored&&restored->size()==1&&restored->front().topic==task.topic);
    auto corrupt=bytes;corrupt[0]='X';check(!decode_social_task_reminders(corrupt));
    std::vector<SocialTaskDescriptor> duplicate{task,task};check(!decode_social_task_reminders(encode_social_task_reminders(duplicate)));
    const auto now=core::SnapshotClock::now();
    SocialTaskFrame frame{1,player,{},{{performer,{0,0,0},true,true,false,false,false,true,true},{recipient,{500,0,0},true,true,false,false,false,false,true}},false};
    SocialTaskController controller;controller.bind_session("session-one");controller.restore_reminders(*restored);
    check(!controller.poll(frame,now)); // saved reminder text is never authority
    controller.hydrate({task},1,player);
    auto command=controller.poll(frame,now);check(command&&command->kind==SocialTaskCommandKind::claim);
    const auto execution=command->execution_id;check(execution=="visit:session-one:1:1");
    check(!controller.poll(frame,now)); // no physical movement before server claim
    auto stale=task;stale.execution_id="another-execution";stale.status="travelling";stale.revision=2;
    controller.hydrate({stale},1,player);check(!controller.poll(frame,now));
    auto travelling=task;travelling.revision=3;travelling.status="travelling";travelling.execution_id=execution;
    controller.hydrate({travelling},1,player);command=controller.poll(frame,now);check(command&&command->kind==SocialTaskCommandKind::visit&&command->deadline==now+std::chrono::seconds{120});
    controller.visit_feedback(task.task_id,"wrong-exec",SocialTaskVisitFeedback::arrived);command=controller.poll(frame,now);check(command&&command->kind==SocialTaskCommandKind::visit);
    controller.visit_feedback(task.task_id,execution,SocialTaskVisitFeedback::arrived);command=controller.poll(frame,now);check(command&&command->kind==SocialTaskCommandKind::arrived);
    check(!controller.poll(frame,now));
    auto arrived=travelling;arrived.status="arrived";arrived.revision=4;controller.hydrate({arrived},1,player);
    controller.note_spoken(task.task_id,execution,"request-recipient","line-recipient");command=controller.poll(frame,now);
    check(command&&command->kind==SocialTaskCommandKind::spoken&&command->proof_line_id=="line-recipient");
    controller.hydrate({arrived},1,player);command=controller.poll(frame,now);check(command&&command->kind==SocialTaskCommandKind::visit); // repeated ack cannot re-open completion report
    auto complete=arrived;complete.status="completed";complete.revision=5;controller.hydrate({complete},1,player);
    command=controller.poll(frame,now);check(command&&command->kind==SocialTaskCommandKind::release);
    controller.visit_feedback(task.task_id,execution,SocialTaskVisitFeedback::released);check(!controller.active_task());
    controller.hydrate({task},1,player);check(!controller.poll(frame,now)); // older same-generation offers cannot reopen completed work
    controller.reset();controller.hydrate({task},1,player);command=controller.poll(frame,now);check(command&&command->execution_id!=execution);
    controller.claim_failed();check(!controller.active_task());check(!controller.poll(frame,now));
    auto retry=task;retry.revision=2;controller.hydrate({retry},1,player);check(controller.poll(frame,now).has_value());
    controller.stop();command=controller.poll(frame,now);check(command&&command->kind==SocialTaskCommandKind::cancelled&&!controller.active_task());
    controller.bind_session("session-two");controller.hydrate({task},1,player);command=controller.poll(frame,now);check(command&&command->execution_id=="visit:session-two:1:1");
    auto visit=task;visit.status="travelling";visit.revision=2;visit.execution_id=command->execution_id;
    controller.hydrate({visit},1,player);check(controller.poll(frame,now)->kind==SocialTaskCommandKind::visit);frame.actors[0].in_combat=true;command=controller.poll(frame,now);
    check(command&&command->kind==SocialTaskCommandKind::release); // cleanup precedes failed status
    controller.visit_feedback(task.task_id,*visit.execution_id,SocialTaskVisitFeedback::released);command=controller.poll(frame,now);check(command&&command->kind==SocialTaskCommandKind::failed);
    frame.actors[0].in_combat=false;auto deferred=task;deferred.task_id="social:fedcba9876543210fedcba9876543210";deferred.when="on_return";deferred.location_ref=SocialLocationRef{0x20,"Fallout4.esm"};deferred.location_name="Sanctuary";
    controller.hydrate({deferred},1,player);check(!controller.poll(frame,now));
    frame.location=SocialLocationRef{0x20,"fallout4.esm"};frame.paused=true;check(!controller.poll(frame,now));frame.paused=false;
    check(controller.poll(frame,now).has_value());
    controller.reset();controller.hydrate({deferred},2,player);check(!controller.poll(frame,now)); // wrong generation cannot trigger restored work
    return count;
}
} // namespace synth::tests
