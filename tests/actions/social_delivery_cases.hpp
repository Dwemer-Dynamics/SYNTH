#pragma once
#include "client/social_task_delivery.hpp"
#include <stdexcept>

namespace synth::tests {
inline int social_delivery_contract_checks() {
    using namespace runtime;
    int count{};
    const auto check=[&](bool value) {++count;if(!value)throw std::runtime_error{"social delivery check failed at "+std::to_string(count)};};
    core::CancellationSource session{core::RuntimeGeneration::initial()};
    core::CancellationSource owner{session.token()};
    const protocol_native::Identity performer{"0x00000015","Fallout4.esm","save-a","Codsworth"};
    const protocol_native::Identity recipient{"0x00000016","Fallout4.esm","save-a","Jun"};
    SocialTaskDescriptor task{"social:0123456789abcdef0123456789abcdef",3,
        {0x15,"Fallout4.esm","save-a"},{0x16,"Fallout4.esm","save-a"},"the water pump","now","","conversing",{},"visit:session:1:1","Codsworth","Jun"};
    SocialTaskCommand command{SocialTaskCommandKind::arrived,task,"visit:session:1:1",core::SnapshotClock::now()+std::chrono::seconds{120},{},{}};
    const auto line=[&](std::string request,protocol_native::Identity speaker,protocol_native::Identity listener) {
        return protocol_native::Line{"line:reply",std::move(request),"turn:visit","flat",1,
            protocol_native::Dialogue{std::move(speaker),"I understand.",{},{},std::move(listener)},2,3};
    };
    const auto caption=[&](const protocol_native::Line& source) {
        presentation::DialogueCaption value;
        value.generation=source.generation;value.request_id=source.request_id;value.line_id=source.line_id;
        value.context_sequence=source.context_sequence;value.cancellation=owner.token();value.text="I understand.";
        return std::make_shared<const presentation::DialogueCaption>(std::move(value));
    };
    client::SocialTaskDelivery delivery;delivery.begin(command,1);delivery.root_request("arrived:1");
    auto first=line("arrived:1",performer,recipient);auto first_caption=caption(first);
    first_caption->mark_delivery(presentation::DialogueDelivery::spoken);
    delivery.track(first,first_caption);delivery.complete_request(first.request_id);
    check(!delivery.delivered_proof()); // A spoken follower request is not a recipient reply.
    auto reply=line("rechat:1",recipient,performer);auto reply_caption=caption(reply);
    reply_caption->mark_delivery(presentation::DialogueDelivery::spoken);
    delivery.child_request("unrelated:parent",reply.request_id);delivery.track(reply,reply_caption);delivery.complete_request(reply.request_id);
    check(!delivery.delivered_proof());
    delivery.child_request(first.request_id,reply.request_id);
    delivery.track(reply,reply_caption);check(!delivery.delivered_proof()); // terminal acknowledgement still missing
    delivery.complete_request(reply.request_id);
    auto proof=delivery.delivered_proof();check(proof&&proof->task_id==task.task_id&&proof->execution_id==command.execution_id&&proof->request_id==reply.request_id&&proof->line_id==reply.line_id);
    for(const auto state:{presentation::DialogueDelivery::pending,presentation::DialogueDelivery::reading,
        presentation::DialogueDelivery::playing,presentation::DialogueDelivery::discarded}) {
        reply_caption->mark_delivery(state);check(!delivery.delivered_proof());
    }
    reply_caption->mark_delivery(presentation::DialogueDelivery::text_complete);check(delivery.delivered_proof().has_value());
    for(int invalid=0;invalid<5;++invalid) {
        delivery.begin(command,1);delivery.root_request(first.request_id);delivery.child_request(first.request_id,reply.request_id);
        auto bad=reply;
        if(invalid==0)std::get<protocol_native::Dialogue>(bad.payload).listener=recipient;
        if(invalid==1)std::get<protocol_native::Identity>(std::get<protocol_native::Dialogue>(bad.payload).speaker).playthrough_id="other-save";
        if(invalid==2)bad.generation=2;
        if(invalid==3)bad.request_id="foreign:request";
        if(invalid==4)bad.line_id="foreign:line";
        delivery.track(bad,reply_caption);delivery.complete_request(bad.request_id);check(!delivery.delivered_proof());
    }
    delivery.begin(command,1);delivery.root_request(first.request_id);delivery.child_request(first.request_id,reply.request_id);
    delivery.track(reply,reply_caption);delivery.complete_request(reply.request_id);check(delivery.delivered_proof().has_value());
    owner.cancel();check(!delivery.delivered_proof());
    delivery.clear();check(!delivery.delivered_proof());
    return count;
}
} // namespace synth::tests
