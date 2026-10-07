#include "context/context.hpp"
#include "client/quest_event_delivery.hpp"
#include "client/actor_event_binding.hpp"
#include "client/actor_event_delivery.hpp"
#include "core/player_event_sampler.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
int checks{};
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error{std::string{"check failed: "} + #x}; } while (false)

template<class E, class F> void throws(F f) { ++checks; try { f(); } catch (const E&) { return; } throw std::runtime_error{"missing exception"}; }

synth::context::TargetContext target(std::uint32_t id, std::uint64_t generation, std::uint64_t frame) {
    using namespace synth;
    return context::TargetContext{context::ActorIdentity{context::FormIdentity{id, "Actors.esm"},
                                                         context::PlaythroughIdentity{"save-a"}},
                                  "actor", {double(id), 0, 0}, double(id),
                                  core::RuntimeGeneration::from_value(generation), frame};
}

// The callback is historical evidence; a later scene and nearby/visible actors cannot manufacture witnesses.
void test_actor_event_binding() {
    using namespace synth;
    using Binder=client::ActorEventSceneBinder;
    using Knowledge=client::ActorEventKnowledge;
    const auto start=core::SnapshotClock::time_point{std::chrono::seconds{10}};
    const auto pose=core::WorldPose{{},core::UnitVector3::from({1,0,0}),core::UnitVector3::from({0,0,1})};
    const auto npc=[](std::string owner="save-a",bool visible=false) {
        return core::ActorSnapshot{0x1234,"Settler",{},"Actors.esm",std::move(owner),false,false,false,
            false,false,1,{},{},{},false,100,100,{},visible};
    };
    const auto scene=[&](int elapsed=1,std::uint64_t generation=1,std::string owner="save-a",
                         std::vector<core::ActorSnapshot> actors={},core::RuntimeVariant variant=core::RuntimeVariant::flat,
                         std::uint64_t ticks=42) {
        return std::make_shared<const core::RuntimeSnapshot>(core::Game::fallout4,variant,
            core::RuntimeGeneration::from_value(generation),2,start+std::chrono::milliseconds{elapsed*100},pose,
            std::optional<core::TimedPose>{},std::optional<core::TimedPose>{},std::optional<core::TimedPose>{},
            core::ActorSnapshot{0x14,"Player",{},"Fallout4.esm",std::move(owner)},std::move(actors),ticks);
    };
    auto arm=scene(0);Binder binder{"session-a",3,arm};
    auto current=scene(2,1,"save-a",{npc()});
    const client::ActorEventItem weapon{0x4321,"Items.esm","Rifle",0x2B};
    client::ActorEventMetadata metadata{current,{weapon}};
    core::ActorEvent death{core::ActorEventKind::death,0x1234,0x14,0,0,0,start+std::chrono::milliseconds{100}};
    core::ActorEvent equip{core::ActorEventKind::equipped,0x14,0,0x4321,0xFF001234,65535,death.observed_at};
    std::array events{death,equip};
    const auto now=start+std::chrono::milliseconds{250};
    auto bound=binder.bind("session-a",3,current,events,metadata,now);
    CHECK(bound && bound->events.size()==2 && bound->rejected==0 && bound->scene==current);
    CHECK(bound->session_id=="session-a" && bound->native_epoch==3);
    CHECK(bound->events[0].evidence==death && bound->events[0].subject.form().plugin()=="Actors.esm");
    CHECK(bound->events[0].other->form().form_id()==0x14 && !bound->events[0].item);
    CHECK(bound->events[0].player_knowledge==Knowledge::unavailable);
    CHECK(bound->events[1].player_knowledge==Knowledge::player_equipment && bound->events[1].item->name=="Rifle");
    CHECK(bound->events[1].evidence.original_reference==0xFF001234 && bound->events[1].evidence.unique_id==65535);
    client::ActorEventDelivery delivery{"session-a",core::RuntimeGeneration::initial()};
    CHECK(delivery.enqueue(bound) && !delivery.enqueue(bound));
    const auto first=delivery.prepare();
    CHECK(first && first->capture->scene==current && first->capture->events.size()==2 && first->actors.size()==2);
    const auto retained_states=first->resolve_actor_states();
    CHECK(retained_states.size()==2 && retained_states[0]==&current->player() && retained_states[1]==&current->actors()[0]);
    auto wrong_fragment=*first;wrong_fragment.actors.clear();
    throws<std::invalid_argument>([&]{(void)wrong_fragment.resolve_actor_states();});
    wrong_fragment=*first;wrong_fragment.actors.push_back(wrong_fragment.actors.back());
    throws<std::invalid_argument>([&]{(void)wrong_fragment.resolve_actor_states();});
    auto absent=std::make_shared<client::BoundActorEventScene>(*bound);absent->scene=scene(2);
    wrong_fragment=*first;wrong_fragment.capture=absent;
    throws<std::invalid_argument>([&]{(void)wrong_fragment.resolve_actor_states();});
    absent->scene=scene(2,1,"save-a",{npc(),npc()});
    throws<std::invalid_argument>([&]{(void)wrong_fragment.resolve_actor_states();});
    CHECK(first->packet(now).age_ms==50 && first->packet(now+std::chrono::seconds{1}).age_ms==1050);
    CHECK(delivery.prepare()==first && first->capture->events[0].evidence.observed_at==death.observed_at);
    throws<std::invalid_argument>([&]{(void)first->packet(start);});
    CHECK(delivery.finish(first,true) && !delivery.pending() && !delivery.finish(first,true));
    auto wrong=std::make_shared<client::BoundActorEventScene>(*bound);wrong->session_id="other";
    CHECK(!delivery.enqueue(wrong));wrong->session_id="session-a";wrong->native_epoch=2;
    CHECK(!delivery.enqueue(wrong));wrong->native_epoch=3;
    for (std::size_t i=0;i<client::ActorEventDelivery::capacity;++i)
        CHECK(delivery.enqueue(std::make_shared<const client::BoundActorEventScene>(*bound)));
    CHECK(delivery.full() && !delivery.enqueue(wrong) && delivery.dropped()==2);
    const auto rejected=delivery.prepare();CHECK(delivery.finish(rejected,false) && delivery.dropped()==4);
    const auto retained=delivery.prepare();delivery.stop();
    CHECK(!delivery.prepare() && !delivery.finish(retained,true) && !delivery.enqueue(wrong));
    std::vector<core::ActorSnapshot> crowd;
    std::vector<core::ActorEvent> crowd_events;
    for (std::uint32_t i=0;i<32;++i) {
        crowd.emplace_back(0x1000+i,"Settler",core::Vec3{},"Actors.esm","save-a");
        crowd_events.push_back({core::ActorEventKind::death,0x1000+i,0x1000+(i+1)%32,0,0,0,death.observed_at});
    }
    const auto crowded=scene(2,1,"save-a",std::move(crowd));
    auto crowded_bound=binder.bind("session-a",3,crowded,crowd_events,{crowded,{}},now);
    CHECK(crowded_bound && crowded_bound->events.size()==32 && crowded_bound->rejected==0);
    client::ActorEventDelivery split{"session-a",core::RuntimeGeneration::initial()};CHECK(split.enqueue(crowded_bound));
    std::size_t delivered{};std::uint64_t previous_serial{};
    while (const auto part=split.prepare()) {
        CHECK(part->capture->scene==crowded && part->actors.size()<=16 && part->serial>previous_serial);
        CHECK(part->resolve_actor_states().size()==part->actors.size());
        CHECK(split.prepare()==part);previous_serial=part->serial;
        for (const auto& event:part->capture->events) {
            CHECK(event.evidence==crowd_events[delivered++] && event.other);
            CHECK(std::ranges::find(part->actors,event.subject)!=part->actors.end());
            CHECK(std::ranges::find(part->actors,*event.other)!=part->actors.end());
        }
        CHECK(split.finish(part,true));
    }
    CHECK(delivered==32 && !split.pending() && split.dropped()==0);
    protocol_native::EventContext wire{};wire.session_id="session-a";wire.generation=1;wire.protocol_version=2;
    wire.runtime_variant="flat";wire.capabilities={"context.actor_events"};
    protocol_native::Identity player{"0x00000014","Fallout4.esm","save-a","Player"};
    protocol_native::NativeActorBatch packet{1,50,bound};
    const auto encoded=protocol_native::actor_events_value(packet,wire,player);
    const auto text=json::write(encoded);
    CHECK(text.find("\"batch_id\":\"actor:1\"")!=std::string::npos && text.find("\"capture_delay_ms\":100")!=std::string::npos);
    CHECK(text.find("player_equipment")!=std::string::npos && text.find("witnesses")==std::string::npos);
    CHECK(protocol_native::encode_context(wire,"save-a",1,player,std::nullopt,{}, {},std::nullopt,{},{},{},
        std::nullopt,"","","","",std::nullopt,std::nullopt,packet).find("actor_events")!=std::string::npos);
    for (int invalid=0;invalid<8;++invalid) {
        auto transport=wire;auto bad=packet;auto recipient=player;
        if(invalid==0)transport.session_id="other";
        if(invalid==1)transport.generation=2;
        if(invalid==2)transport.protocol_version=1;
        if(invalid==3)transport.runtime_variant="vr";
        if(invalid==4)transport.capabilities.clear();
        if(invalid==5)bad.serial=0;
        if(invalid==6)bad.capture.reset();
        if(invalid==7)recipient.playthrough_id="other";
        throws<std::invalid_argument>([&] {(void)protocol_native::actor_events_value(bad,transport,recipient);});
    }
    auto forged=std::make_shared<client::BoundActorEventScene>(*bound);packet.capture=forged;
    forged->events[0].player_knowledge=Knowledge::player_equipment;
    throws<std::invalid_argument>([&] {(void)protocol_native::actor_events_value(packet,wire,player);});
    metadata.items[0].name="Later name";events[1].kind=core::ActorEventKind::unequipped;
    CHECK(bound->events[1].item->name=="Rifle" && bound->events[1].evidence.kind==core::ActorEventKind::equipped);
    CHECK(binder.bind("session-a",3,current,events,metadata,now)->events[1].evidence.kind==core::ActorEventKind::unequipped);
    CHECK(!binder.bind("session-b",3,current,events,metadata,now));
    CHECK(!binder.bind("session-a",5,current,events,metadata,now));
    CHECK(!binder.bind("session-a",2,current,events,metadata,now));
    CHECK(!binder.bind("session-a",3,{},events,metadata,now));
    CHECK(!binder.bind("session-a",3,current,events,metadata,start));
    CHECK(!binder.bind("session-a",3,current,events,metadata,start+std::chrono::seconds{3}));
    auto aged=binder.bind("session-a",3,current,events,metadata,start+std::chrono::milliseconds{2200});
    CHECK(aged && aged->events.empty() && aged->rejected==2); // A fresh scene cannot refresh the original event age.
    CHECK(binder.bind("session-a",3,current,events,metadata,start+std::chrono::milliseconds{2100})->events.size()==2);
    for (auto wrong_scene:{scene(2,2),scene(2,1,"save-b"),scene(2,1,"save-a",{},core::RuntimeVariant::vr),
                    scene(-1),scene(2,1,"save-a",{},core::RuntimeVariant::flat,41)}) {
        client::ActorEventMetadata wrong_metadata{wrong_scene,{weapon}};
        CHECK(!binder.bind("session-a",3,wrong_scene,events,wrong_metadata,now));
    }
    auto copied_scene=scene(2,1,"save-a",{npc()});
    CHECK(!binder.bind("session-a",3,copied_scene,events,metadata,now)); // Metadata must belong to the exact immutable capture.
    for (auto bad_time:{core::SnapshotClock::time_point{},start-std::chrono::milliseconds{1},now}) {
        auto invalid=equip;invalid.observed_at=bad_time;
        auto result=binder.bind("session-a",3,current,std::span{&invalid,1},metadata,now);
        CHECK(result && result->events.empty() && result->rejected==1);
    }
    for (const auto& actors:std::vector<std::vector<core::ActorSnapshot>>{{},{npc("save-b")},{npc(),npc()}}) {
        auto missing=scene(2,1,"save-a",actors);client::ActorEventMetadata missing_metadata{missing,{weapon}};
        auto result=binder.bind("session-a",3,missing,events,missing_metadata,now);
        CHECK(result && result->events.size()==1 && result->rejected==1);
    }
    auto visible=scene(2,1,"save-a",{npc("save-a",true)});client::ActorEventMetadata visible_metadata{visible,{weapon}};
    CHECK(binder.bind("session-a",3,visible,events,visible_metadata,now)->events[0].player_knowledge==Knowledge::unavailable);
    auto npc_equip=equip;npc_equip.actor=0x1234;
    CHECK(binder.bind("session-a",3,current,std::span{&npc_equip,1},metadata,now)->events[0].player_knowledge==Knowledge::unavailable);
    for (const auto other:{0U,0x9999U}) {
        auto secondary=death;secondary.other_actor=other;
        auto result=binder.bind("session-a",3,current,std::span{&secondary,1},metadata,now);
        CHECK(result->events.size()==1 && !result->events[0].other && result->events[0].evidence.other_actor==other);
    }
    for (const auto& items:std::vector<std::vector<client::ActorEventItem>>{{},{weapon,weapon},{{0x4321,"../Items.esm","Rifle",43}}}) {
        auto invalid_metadata=metadata;invalid_metadata.items=items;
        auto result=binder.bind("session-a",3,current,events,invalid_metadata,now);
        CHECK(result->events.size()==1 && result->rejected==1);
    }
    std::array<core::ActorEvent,33> overflow{};
    CHECK(!binder.bind("session-a",3,current,overflow,metadata,now));
    auto overflow_metadata=metadata;overflow_metadata.items.resize(33);
    CHECK(!binder.bind("session-a",3,current,events,overflow_metadata,now));
    CHECK(!binder.bind("session-a",3,current,{},metadata,now));
    Binder frontier{"session-a",3,arm};
    auto advanced=scene(3,1,"save-a",{npc()},core::RuntimeVariant::flat,50);
    CHECK(frontier.bind("session-a",3,advanced,events,{advanced,{weapon}},start+std::chrono::milliseconds{350}));
    CHECK(!frontier.bind("session-a",3,current,events,metadata,now));
    auto rollback=scene(4,1,"save-a",{npc()},core::RuntimeVariant::flat,49);
    CHECK(!frontier.bind("session-a",3,rollback,events,{rollback,{weapon}},start+std::chrono::milliseconds{450}));
    throws<std::invalid_argument>([&] { Binder{"",3,arm}; });
    throws<std::invalid_argument>([&] { Binder{"session-a",2,arm}; });
    throws<std::invalid_argument>([&] { Binder{"session-a",3,{}}; });
    throws<std::invalid_argument>([&] { Binder{"session-a",3,scene(0,1,"unknown")}; });
}
}

int main() {
    try {
        using namespace synth;
        test_actor_event_binding();
        throws<std::invalid_argument>([] { context::FormIdentity{0, "Actors.esm"}; });
        throws<std::invalid_argument>([] { context::FormIdentity{1, "../Actors.esm"}; });
        CHECK((context::FormIdentity{1, "Actors.esm"}.plugin() == "Actors.esm"));
        context::ActiveAgentRegistry registry{3};
        CHECK(registry.register_agent(target(3, 1, 7), core::RuntimeGeneration::initial(), 7) ==
              context::ActiveAgentRegistry::RegisterResult::inserted);
        CHECK(registry.register_agent(target(1, 1, 7), core::RuntimeGeneration::initial(), 7) ==
              context::ActiveAgentRegistry::RegisterResult::inserted);
        CHECK(registry.register_agent(target(2, 1, 6), core::RuntimeGeneration::initial(), 7) ==
              context::ActiveAgentRegistry::RegisterResult::rejected_stale);
        const auto current = registry.current(core::RuntimeGeneration::initial(), 7);
        CHECK(current.size() == 2);
        CHECK(current[0].identity().form().form_id() == 1);
        CHECK(current[1].identity().form().form_id() == 3);
        CHECK(registry.contains_current(current[0].identity(), core::RuntimeGeneration::initial(), 7));
        throws<std::invalid_argument>([&] { context::AudienceContext{1, current}; });
        // Passive context changes are scoped observations, not ticks, frame IDs or NPC selection.
        const core::WorldPose pose{{},core::UnitVector3::from({1,0,0}),core::UnitVector3::from({0,0,1})};
        const auto scene = [&](core::RuntimeVariant variant, std::uint64_t generation, std::uint64_t frame,
                               std::string playthrough, std::optional<core::WorldState> world,
                               std::vector<core::QuestSnapshot> quests = {}, std::string quality = {}) {
            return core::RuntimeSnapshot{core::Game::fallout4, variant, core::RuntimeGeneration::from_value(generation),
                frame, core::SnapshotClock::now(), pose,{},{},{},
                core::ActorSnapshot{0x14,"Player",{},"Fallout4.esm",std::move(playthrough)},{},42,std::move(world),
                {},std::move(quests),{},{},std::move(quality)};
        };
        core::WorldState world{"Sanctuary","SanctuaryExt","Commonwealth","Clear",false,42};
        auto observed = context::world_observation(scene(core::RuntimeVariant::flat,1,1,"save-a",world));
        CHECK(observed);
        world.game_time_ticks=43;
        CHECK(observed==context::world_observation(scene(core::RuntimeVariant::flat,1,2,"save-a",world)));
        CHECK(observed!=context::world_observation(scene(core::RuntimeVariant::flat,2,1,"save-a",world)));
        CHECK(observed!=context::world_observation(scene(core::RuntimeVariant::flat,1,3,"save-b",world)));
        CHECK(observed!=context::world_observation(scene(core::RuntimeVariant::vr,1,3,"save-a",world)));
        CHECK(!context::world_observation(scene(core::RuntimeVariant::flat,1,4,"save-a",std::nullopt)));
        for (int field=0;field<5;++field) {
            auto changed=world;
            if(field==0)changed.location="Red Rocket";
            if(field==1)changed.cell="RedRocketExt";
            if(field==2)changed.worldspace="OtherWorld";
            if(field==3)changed.weather="Rain";
            if(field==4)changed.interior=true;
            CHECK(observed!=context::world_observation(scene(core::RuntimeVariant::flat,1,5,"save-a",changed)));
        }
        core::QuestSnapshot quest{1,"Fallout4.esm","Quest","QuestEditor",10,1,{"Find the settler"},"complete"};
        const auto journal = [&](std::vector<core::QuestSnapshot> quests, std::string quality = "complete") {
            return context::quest_observation(scene(core::RuntimeVariant::flat,1,1,"save-a",std::nullopt,
                                                    std::move(quests),std::move(quality)));
        };
        const auto first = journal({quest});
        CHECK(first && first->quests.front().current_stage == 10);
        quest.tracked=false; CHECK(journal({quest}) != first);
        const auto untracked = journal({quest});
        quest.tracked=true; CHECK(journal({quest}) != untracked);
        quest.tracked.reset(); CHECK(journal({quest}) == first);
        CHECK(!journal({quest},"cached"));
        CHECK(!journal({},"unavailable"));
        CHECK(journal({quest},"partial") != first);
        CHECK(journal({}) && journal({})->quests.empty());
        quest.current_stage=20; CHECK(journal({quest}) != first);
        quest.current_stage=10; quest.objectives={"Return to the settler"}; CHECK(journal({quest}) != first);
        auto other=quest; other.form_id=2;
        CHECK(journal({quest,other}) == journal({other,quest}));
        CHECK(first != context::quest_observation(scene(core::RuntimeVariant::flat,2,1,"save-a",std::nullopt,{quest},"complete")));
        CHECK(first != context::quest_observation(scene(core::RuntimeVariant::vr,1,1,"save-b",std::nullopt,{quest},"complete")));
        auto tracked=quest; tracked.tracked=true; tracked.objectives={"One", "Two"};
        auto prior=*journal({tracked}); auto next=prior;
        CHECK(!context::tracked_quest_changed(prior,next));
        next.quests[0].current_stage++;
        CHECK(!context::tracked_quest_changed(prior,next));
        next.quests[0].objectives={"Two","One"};
        CHECK(!context::tracked_quest_changed(prior,next));
        next.quests[0].objectives={"Return"};
        CHECK(context::tracked_quest_changed(prior,next));
        next.quests[0].objectives_observation="partial";
        CHECK(!context::tracked_quest_changed(prior,next));
        next=prior; next.quests[0].tracked=false;
        CHECK(!context::tracked_quest_changed(prior,next));
        CHECK(context::tracked_quest_changed(next,prior));
        next.quests[0].tracked.reset();
        CHECK(!context::tracked_quest_changed(next,prior));
        next.quests.clear();
        CHECK(context::tracked_quest_changed(next,prior));
        next.quality="partial";
        CHECK(!context::tracked_quest_changed(next,prior));
        next.quality="cached";
        CHECK(!context::tracked_quest_changed(next,prior));
        next=prior; next.playthrough="save-other"; next.quests[0].objectives={"Return"};
        CHECK(!context::tracked_quest_changed(prior,next));
        next=prior; next.generation=core::RuntimeGeneration::from_value(2); next.quests[0].tracked=false;
        CHECK(!context::tracked_quest_changed(next,prior));
        next=prior; next.quests[0].origin_plugin="FALLOUT4.ESM";
        CHECK(!context::tracked_quest_changed(prior,next));
        // Native delivery retains the captured scene and ordered batch through ambiguous network failures.
        using Delivery = client::QuestEventDelivery;
        const core::QuestSnapshot visible_quest{0x229E5,"Fallout4.esm","Quest","QuestEditor",45,1,{"Find the settler"},"complete"};
        const auto capture = [&](std::vector<core::QuestSnapshot> quests, std::string quality = "complete",
                                 std::uint64_t generation = 1, std::string save = "save-a",
                                 core::RuntimeVariant lane = core::RuntimeVariant::flat) {
            return std::make_shared<const core::RuntimeSnapshot>(scene(lane,generation,10,std::move(save),world,std::move(quests),std::move(quality)));
        };
        const auto original = capture({visible_quest});
        const std::array transitions{core::QuestEvent{core::QuestEventKind::stage,0x229E5,20,1,false},
                                    core::QuestEvent{core::QuestEventKind::stopped,0x229E5,0,0,true}};
        Delivery delivery{core::RuntimeGeneration::initial()};
        CHECK(delivery.enqueue(original,transitions));
        auto prepared = delivery.prepare();
        CHECK(prepared && prepared->snapshot == original && prepared->batch.events.size() == 2);
        CHECK(prepared->batch.events[0].event == transitions[0] && prepared->batch.events[1].event == transitions[1]);
        CHECK(prepared->batch.events[0].visibility_sequence == 0);
        delivery.acknowledge_journal(capture({}),1);
        CHECK(delivery.prepare() == prepared); // Later scenes cannot replace retry evidence.
        CHECK(delivery.finish(prepared,true) && !delivery.pending());
        CHECK(!delivery.finish(prepared,true));
        delivery.acknowledge_journal(original,2);
        CHECK(delivery.enqueue(capture({}),transitions));
        auto stopped = delivery.prepare();
        CHECK(stopped && stopped->batch.batch_id != prepared->batch.batch_id);
        CHECK(stopped->batch.events[1].visibility_sequence == 2);
        CHECK(!delivery.finish(prepared,true) && delivery.prepare() == stopped);
        CHECK(delivery.finish(stopped,false) && delivery.dropped() == 2);
        CHECK(!delivery.enqueue(capture({},"complete",2),transitions));
        CHECK(!delivery.enqueue(capture({},"complete",1,"save-b"),transitions));
        CHECK(!delivery.enqueue(capture({},"complete",1,"save-a",core::RuntimeVariant::vr),transitions));
        delivery.stop();
        CHECK(!delivery.enqueue(original,transitions) && !delivery.prepare());

        Delivery bounded{core::RuntimeGeneration::initial()};
        for (std::size_t i=0;i<Delivery::capacity;++i) CHECK(bounded.enqueue(original,transitions));
        CHECK(!bounded.enqueue(original,transitions) && bounded.dropped() == 2);
        auto retained = bounded.prepare();
        bounded.stop();
        CHECK(!bounded.prepare() && !bounded.finish(retained,true));

        Delivery unseen{core::RuntimeGeneration::initial()};
        unseen.acknowledge_journal(capture({visible_quest},"cached"),1);
        CHECK(unseen.enqueue(capture({}),transitions));
        CHECK(!unseen.prepare() && !unseen.pending() && unseen.unobserved() == 2);
        auto hidden = visible_quest; hidden.active_objectives=0; hidden.objectives.clear();
        unseen.acknowledge_journal(capture({hidden}),2);
        CHECK(unseen.enqueue(capture({hidden}),transitions));
        CHECK(!unseen.prepare() && unseen.unobserved() == 4);
        unseen.acknowledge_journal(original,3);
        const std::array mixed{core::QuestEvent{core::QuestEventKind::stage,0,0,0,false},
            transitions[0],core::QuestEvent{core::QuestEventKind::stage,0x999,1,0,false},transitions[0]};
        CHECK(unseen.enqueue(original,mixed));
        const auto filtered=unseen.prepare();
        CHECK(filtered && filtered->batch.events.size()==2 && filtered->batch.events[0]==filtered->batch.events[1]);
        CHECK(unseen.dropped()==1 && unseen.unobserved()==5);

        Delivery history{core::RuntimeGeneration::initial()};
        for (std::size_t i=0;i<=Delivery::witness_capacity;++i) {
            auto entry=visible_quest; entry.form_id=static_cast<std::uint32_t>(i+1);
            history.acknowledge_journal(capture({entry}),i+1);
        }
        const std::array evicted{core::QuestEvent{core::QuestEventKind::stopped,1,0,0,false}};
        CHECK(history.enqueue(capture({}),evicted));
        CHECK(!history.prepare() && history.unobserved()==1);
        const std::array newest{core::QuestEvent{core::QuestEventKind::stopped,129,0,0,false}};
        CHECK(history.enqueue(capture({}),newest));
        CHECK(history.prepare()->batch.events[0].visibility_sequence==129);
        using Sampler = core::PlayerEventSampler;
        using Kind = core::PlayerEventKind;
        const auto epoch = core::SnapshotClock::time_point{};
        const auto player_sample = [&](int tick, int level, bool combat) {
            return core::PlayerEventSample{core::RuntimeGeneration::initial(), core::RuntimeVariant::flat,
                0x14, "Fallout4.esm", "save-a", epoch + std::chrono::seconds{tick},
                static_cast<std::uint64_t>(tick + 100), static_cast<std::int16_t>(level), combat};
        };
        throws<std::invalid_argument>([] { Sampler{{},core::RuntimeVariant::flat}; });
        Sampler sampled{core::RuntimeGeneration::initial(),core::RuntimeVariant::flat};
        CHECK(sampled.due(epoch));
        CHECK(!sampled.due(epoch + std::chrono::milliseconds{999}));
        CHECK(sampled.due(epoch + std::chrono::seconds{1}));
        CHECK(sampled.due(epoch + std::chrono::seconds{100}));
        CHECK(!sampled.due(epoch + std::chrono::seconds{100})); // No catch-up storm.
        CHECK(sampled.observe(player_sample(0,10,true)) == 0 && !sampled.front()); // Quiet initial load in combat.
        CHECK(sampled.observe(player_sample(1,11,false)) == 2);
        const auto first_transition = *sampled.front();
        CHECK(first_transition.kind == Kind::level_up && first_transition.before.level == 10 && first_transition.after.level == 11);
        CHECK(first_transition.before.in_combat && !first_transition.after.in_combat);
        CHECK(sampled.observe(player_sample(2,11,true)) == 0 && *sampled.front() == first_transition);
        CHECK(!sampled.acknowledge(first_transition.serial + 1));
        CHECK(sampled.acknowledge(first_transition.serial));
        CHECK(!sampled.acknowledge(first_transition.serial));
        CHECK(sampled.front()->kind == Kind::combat_end && sampled.front()->before == first_transition.before &&
              sampled.front()->after == first_transition.after);
        CHECK(sampled.acknowledge(sampled.front()->serial) && !sampled.front());
        CHECK(sampled.observe(player_sample(3,11,false)) == 1);
        auto old = player_sample(2,100,false);
        CHECK(sampled.observe(old) == 0 && sampled.pending() == 1); // Out of order cannot invent a level-up.
        old.playthrough_id="older-save";
        CHECK(sampled.observe(old) == 0 && sampled.pending() == 1);
        auto wrong = player_sample(4,100,false);wrong.generation=core::RuntimeGeneration::from_value(2);
        CHECK(sampled.observe(wrong) == 0 && sampled.pending() == 1);
        wrong.generation=core::RuntimeGeneration::initial();wrong.variant=core::RuntimeVariant::vr;
        CHECK(sampled.observe(wrong) == 0 && sampled.pending() == 1);
        sampled.unavailable();
        CHECK(sampled.observe(player_sample(5,20,false)) == 0 && sampled.pending() == 1); // Missing observations are not events.
        const auto old_serial=sampled.front()->serial;
        auto other_player=player_sample(6,30,true);other_player.playthrough_id="save-b";
        CHECK(sampled.observe(other_player) == 0 && sampled.pending() == 0 && sampled.dropped() == 1);
        other_player.observed_at+=std::chrono::seconds{1};++other_player.game_time_ticks;other_player.in_combat=false;
        CHECK(sampled.observe(other_player) == 1 && sampled.front()->serial > old_serial);
        CHECK(!sampled.acknowledge(old_serial));
        sampled.unavailable();other_player.observed_at+=std::chrono::seconds{1};--other_player.game_time_ticks;
        CHECK(sampled.observe(other_player) == 0 && sampled.pending() == 0 && sampled.dropped() == 2); // Rollback after missing frame.
        other_player.observed_at+=std::chrono::seconds{1};++other_player.game_time_ticks;other_player.level=31;
        CHECK(sampled.observe(other_player) == 1);
        other_player.observed_at+=std::chrono::seconds{1};++other_player.game_time_ticks;other_player.level=10;other_player.in_combat=false;
        CHECK(sampled.observe(other_player) == 0 && sampled.pending() == 0); // Level decrease is a discontinuity.
        other_player.observed_at+=std::chrono::seconds{1};other_player.game_time_ticks=0;
        CHECK(sampled.observe(other_player) == 0);
        other_player.observed_at+=std::chrono::seconds{1};other_player.game_time_ticks=200;other_player.level=20;
        CHECK(sampled.observe(other_player) == 0); // Invalid scalar broke the comparison baseline.

        Sampler pressure{core::RuntimeGeneration::initial(),core::RuntimeVariant::flat};
        CHECK(pressure.observe(player_sample(0,1,false)) == 0);
        for (int tick=1;tick<=16;++tick) CHECK(pressure.observe(player_sample(tick,tick+1,false)) == 1);
        const auto immutable=*pressure.front();
        CHECK(pressure.observe(player_sample(17,18,false)) == 0 && pressure.pending() == Sampler::capacity && pressure.dropped() == 1);
        CHECK(*pressure.front() == immutable);
        CHECK(pressure.acknowledge(immutable.serial));
        CHECK(pressure.observe(player_sample(18,18,false)) == 0); // Overflow advances baseline, not repeated false events.
        CHECK(pressure.observe(player_sample(19,19,false)) == 1 && pressure.pending() == Sampler::capacity);
        for (std::size_t i=0;i<Sampler::capacity;++i) CHECK(pressure.acknowledge(pressure.front()->serial));
        CHECK(!pressure.front() && !pressure.acknowledge(0));

        // Menu pause retains continuity, including a level earned while the level-up menu was open.
        Sampler menu{core::RuntimeGeneration::initial(),core::RuntimeVariant::flat};
        CHECK(menu.observe(player_sample(1,10,false)) == 0);
        CHECK(menu.observe(player_sample(300,11,false)) == 1);
        CHECK(menu.front()->before.level == 10 && menu.front()->after.level == 11);
        CHECK(menu.acknowledge(menu.front()->serial, false) && menu.dropped() == 1 && !menu.front());
        Sampler reactions{core::RuntimeGeneration::initial(),core::RuntimeVariant::flat};
        CHECK(reactions.observe(player_sample(0,1,true))==0);
        CHECK(reactions.observe(player_sample(1,2,false))==2);
        CHECK(!reactions.reaction(epoch+std::chrono::seconds{1})); // No speculation before history ACK.
        const auto serial=reactions.front()->serial;
        CHECK(!reactions.acknowledge(serial+1,true,true));
        CHECK(reactions.acknowledge(serial,true,true));
        CHECK(reactions.reaction(epoch+std::chrono::seconds{61})->serial==serial); // Inclusive 60-second boundary.
        CHECK(!reactions.consume_reaction(serial+1));
        CHECK(reactions.consume_reaction(serial));
        CHECK(reactions.acknowledge(reactions.front()->serial,false,true));
        CHECK(!reactions.reaction(epoch+std::chrono::seconds{61})); // Rejection creates no reaction.
        CHECK(reactions.observe(player_sample(2,3,false))==1);
        CHECK(reactions.acknowledge(reactions.front()->serial,true,true));
        CHECK(!reactions.reaction(epoch+std::chrono::seconds{63}));
        for (int tick=3;tick<22;++tick) {
            CHECK(reactions.observe(player_sample(tick,tick+1,false))==1);
            CHECK(reactions.acknowledge(reactions.front()->serial,true,true));
        }
        unsigned retained_reactions=0;
        while (const auto* reaction=reactions.reaction(epoch+std::chrono::seconds{22})) {
            ++retained_reactions;CHECK(reactions.consume_reaction(reaction->serial));
        }
        CHECK(retained_reactions==Sampler::capacity && reactions.pending()==0); // Full reaction queue cannot block history.
        CHECK(reactions.observe(player_sample(23,30,false))==1);
        CHECK(reactions.acknowledge(reactions.front()->serial,true,true));
        reactions.unavailable();
        auto switched=player_sample(24,30,false);switched.playthrough_id="different-save";
        CHECK(reactions.observe(switched)==0 && !reactions.reaction(switched.observed_at));
        switched.observed_at+=std::chrono::seconds{1};++switched.game_time_ticks;++switched.level;
        CHECK(reactions.observe(switched)==1);
        CHECK(reactions.acknowledge(reactions.front()->serial,true,true));
        switched.observed_at+=std::chrono::seconds{1};--switched.game_time_ticks;
        CHECK(reactions.observe(switched)==0 && !reactions.reaction(switched.observed_at));
        std::cout << "context tests passed (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
