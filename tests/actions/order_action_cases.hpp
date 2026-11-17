#pragma once
// Follow, Stop Following and Take a Seat: reply-line binding, owned-package receipts and seat cooldown.
#include "client/order_action.hpp"
#include "runtime/seat_order.hpp"
#include <stdexcept>

namespace synth::tests {
inline int order_action_checks() {
    using namespace std::chrono_literals;
    int checks{};
    const auto check=[&](bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);};
    std::vector<std::string> ready,idle,vr;
    client::append_order_capabilities(ready,core::RuntimeVariant::flat,true);
    client::append_order_capabilities(idle,core::RuntimeVariant::flat,false);
    client::append_order_capabilities(vr,core::RuntimeVariant::vr,true);
    check(ready==std::vector<std::string>({"action.follow_player","action.stop_following","action.take_a_seat","action.order_verified"}),"flat ready lane did not advertise exactly the order capabilities");
    check(idle.empty()&&vr.empty(),"orders were advertised without a flat adapter that applies them");

    core::CancellationSource session{core::RuntimeGeneration::initial()};
    core::CancellationSource turn{session.token()};
    const auto now=core::SnapshotClock::now();
    const auto pose=core::WorldPose{{},core::UnitVector3::from({1,0,0}),core::UnitVector3::from({0,0,1})};
    const core::ActorSnapshot player{0x14,"Player",{},"Fallout4.esm","save-a"},npc{0x15,"Piper",{},"Fallout4.esm","save-a"};
    const core::RuntimeSnapshot snapshot{core::Game::fallout4,core::RuntimeVariant::flat,session.token().generation(),12,now,pose,
        std::nullopt,std::nullopt,std::nullopt,player,{npc},123};
    protocol_native::Identity piper{"0x00000015","Fallout4.esm","save-a","Piper"};
    protocol_native::Action follow{"action:f","idem:f","follow_player","action.follow_player",piper,std::nullopt,
        json::Value{json::Value::Object{}},5000,"line:1"};
    protocol_native::Line reply{"line:1","req:1","turn:1","flat",session.token().generation().value(),protocol_native::Status{},2,3};

    // Reply binding: exact line, speaker, request and live turn; one order per reply.
    client::OrderReplyBinding binding;
    binding.note_reply(reply,piper,turn.token());
    protocol_native::Line action_line=reply;action_line.line_id="line:2";
    check(binding.claim(action_line,follow,turn.token()),"exact reply line did not bind its order");
    check(!binding.claim(action_line,follow,turn.token()),"one reply line carried two orders");
    binding.note_reply(reply,piper,turn.token());
    auto other_line=follow;other_line.reply_line_id="line:9";
    check(!binding.claim(action_line,other_line,turn.token()),"an order bound to a reply line that was never accepted");
    auto other_actor=follow;other_actor.actor.form_id="0x00000016";
    check(!binding.claim(action_line,other_actor,turn.token()),"an order bound to a different speaker");
    auto no_line=follow;no_line.reply_line_id.clear();
    check(!binding.claim(action_line,no_line,turn.token()),"an order without a reply line was bound");
    auto other_turn=action_line;other_turn.turn_id="turn:2";
    check(!binding.claim(other_turn,follow,turn.token()),"an order bound across turns");
    core::CancellationSource cancelled{session.token()};
    client::OrderReplyBinding dropped;
    dropped.note_reply(reply,piper,cancelled.token());
    cancelled.cancel();
    check(!dropped.claim(action_line,follow,turn.token()),"a cancelled reply still applied its order");

    // Preparation: no target, no arguments, an NPC in this playthrough.
    const auto prepared=client::prepare_order_action(snapshot,follow,turn.token(),now+10s);
    check(prepared.name==runtime::RuntimeActionName::follow_player&&prepared.actor.form_id==0x15&&prepared.action_id=="action:f","order was not prepared for the exact actor");
    const auto refuses=[&](protocol_native::Action bad){
        try{(void)client::prepare_order_action(snapshot,bad,turn.token(),now+10s);}catch(const std::invalid_argument&){return true;}return false;};
    auto bad=follow;bad.target=piper;check(refuses(bad),"order accepted a target");
    bad=follow;bad.arguments=json::Value{json::Value::Object{{"item","x"}}};check(refuses(bad),"order accepted arguments");
    bad=follow;bad.reply_line_id.clear();check(refuses(bad),"order without reply binding was prepared");
    bad=follow;bad.actor.form_id="0x00000014";check(refuses(bad),"order was prepared for the player");
    bad=follow;bad.actor.form_id="0x00000016";check(refuses(bad),"order was prepared for an absent actor");
    bad=follow;bad.name="take_a_seat";check(refuses(bad),"order name and capability were allowed to disagree");
    bad=follow;bad.name="wait_here";bad.capability="action.wait_here";check(refuses(bad),"a non-order action was prepared as an order");
    turn.cancel();check(refuses(follow),"order was prepared for a cancelled turn");
    core::CancellationSource live{session.token()};
    const auto request=client::prepare_order_action(snapshot,follow,live.token(),now+10s);

    // Receipts: only the game's owned package proves an order; Stop proves it is gone.
    runtime::RuntimeOrderObservation obs{live.token().generation(),request.actor,request.action_id,request.name,true,false,false,0x01000C00,0};
    check(obs.confirms_success(request),"owned follow package was not accepted");
    auto bad_obs=obs;bad_obs.owned_package=0;check(!bad_obs.confirms_success(request),"follow succeeded without an owned package");
    bad_obs=obs;bad_obs.follows_player=false;check(!bad_obs.confirms_success(request),"follow succeeded without following");
    bad_obs=obs;bad_obs.action_id="other";check(!bad_obs.confirms_success(request),"another action's observation confirmed this order");
    bad_obs=obs;bad_obs.actor.form_id=0x16;check(!bad_obs.confirms_success(request),"another actor's observation confirmed this order");
    const auto receipt=client::verified_order_result(follow,request,obs);
    check(receipt.find("synth.action.order.result.v1")!=std::string::npos&&receipt.find("line:1")!=std::string::npos&&receipt.size()<=1024,"order receipt is not the bounded owned receipt");
    bool refused{};try{(void)client::verified_order_result(follow,request,bad_obs);}catch(const std::invalid_argument&){refused=true;}
    check(refused,"an unverified order produced a receipt");
    auto sit=request;sit.name=runtime::RuntimeActionName::take_a_seat;
    runtime::RuntimeOrderObservation seated{live.token().generation(),sit.actor,sit.action_id,sit.name,false,true,false,0x01000C01,0x00012345};
    check(seated.confirms_success(sit),"seated observation was not accepted");
    seated.seat_form=0;check(!seated.confirms_success(sit),"sit succeeded without a seat");
    auto stop=request;stop.name=runtime::RuntimeActionName::stop_following;
    runtime::RuntimeOrderObservation released{live.token().generation(),stop.actor,stop.action_id,stop.name,false,false,true,0,0};
    check(released.confirms_success(stop),"released observation was not accepted");
    released.owned_package=0x01000C00;check(!released.confirms_success(stop),"release succeeded while a package remained");

    // Seat watch: cooldown for unreachable seats, as in CHIM 4.
    using runtime::SeatKind;using runtime::SeatReason;using runtime::SeatStep;
    runtime::SeatOrderWatch watch;SeatReason reason{};const auto t0=runtime::SeatOrderWatch::Clock::now();
    watch.begin(t0);watch.target(SeatKind::furniture,0xA1,1000.0F,t0);
    runtime::SeatSample sample{false,true,false,1,1000.0F};
    check(watch.check(sample,t0+2s,reason)==SeatStep::keep,"a walk was cut short");
    check(watch.check(sample,t0+6s,reason)==SeatStep::retry&&reason==SeatReason::no_progress,"a stuck walk did not retry");
    watch.mark_failed(reason,t0+6s);
    auto skip=watch.skipped(t0+7s);
    check(std::ranges::find(skip,0xA1U)!=skip.end(),"an unreachable seat was not skipped");
    watch.begin(t0+8s);
    skip=watch.skipped(t0+9s);
    check(std::ranges::find(skip,0xA1U)!=skip.end(),"the cooldown did not survive a new order");
    skip=watch.skipped(t0+7min);
    check(std::ranges::find(skip,0xA1U)!=skip.end(),"the cooldown ended before ten minutes");
    skip=watch.skipped(t0+12min);
    check(skip.empty(),"the cooldown never expired");
    watch.begin(t0+13min);watch.target(SeatKind::furniture,0xA2,500.0F,t0+13min);
    sample=runtime::SeatSample{false,true,false,0,500.0F};
    check(watch.check(sample,t0+13min+1s,reason)==SeatStep::retry&&reason==SeatReason::seat_taken,"an occupied seat did not retry");
    watch.mark_failed(reason,t0+13min+1s);
    skip=watch.skipped(t0+13min+2s);
    check(std::ranges::find(skip,0xA2U)!=skip.end(),"seats tried in this order were retried");
    watch.begin(t0+14min);
    check(watch.skipped(t0+14min+1s).empty(),"a taken seat was treated as unreachable");
    watch.target(SeatKind::furniture,0xA3,100.0F,t0+14min);
    sample=runtime::SeatSample{false,true,false,1,100.0F};
    check(watch.check(sample,t0+14min+1s,reason)==SeatStep::keep,"arrival was not allowed time to sit");
    check(watch.check(sample,t0+14min+7s,reason)==SeatStep::retry&&reason==SeatReason::not_seated,"an NPC that never sat did not retry");
    sample.seated_on_seat=true;
    check(watch.check(sample,t0+14min+8s,reason)==SeatStep::keep&&reason==SeatReason::seated,"sitting was not noticed");
    sample.player_left=true;
    check(watch.check(sample,t0+14min+9s,reason)==SeatStep::finish&&reason==SeatReason::player_left,"the order outlived the player");
    sample.player_left=false;
    check(watch.check(sample,t0+14min+6min,reason)==SeatStep::finish&&reason==SeatReason::duration,"the order never expired");
    // After max attempts the NPC sits on the ground, then waits there.
    runtime::SeatOrderWatch ladder;ladder.begin(t0);
    check(ladder.next_kind()==SeatKind::furniture,"first attempt was not furniture");
    for(std::uint32_t seat=1;seat<=3;++seat)ladder.target(SeatKind::furniture,seat,1000.0F,t0);
    check(ladder.next_kind()==SeatKind::ground,"ground fallback did not follow the last seat");
    ladder.target(SeatKind::ground,0xB1,0.0F,t0);
    check(ladder.next_kind()==SeatKind::wait,"waiting did not follow the ground fallback");
    return checks;
}
}  // namespace synth::tests
