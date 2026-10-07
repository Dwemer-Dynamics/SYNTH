#include "targeting/targeting.hpp"
#include "core/automatic_diary_queue.hpp"
#include "core/picked_reference_mailbox.hpp"
#include "integration/external_requests.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;
int checks{};
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error{std::string{"check failed: "} + #x}; } while (false)

synth::core::WorldPose pose(synth::core::Vec3 position, synth::core::Vec3 forward = {1, 0, 0}) {
    return {position, synth::core::UnitVector3::from(forward), synth::core::UnitVector3::from({0, 0, 1})};
}

synth::core::ActorSnapshot actor_with_state(std::string life_state, std::string posture) {
    return synth::core::ActorSnapshot{
        0x1234, "Piper", {}, "Fallout4.esm", "save-a", true, false, false, false,
        false, 1, "HumanRace", "female", "FemaleBoston", false, 100.0, 100.0, {},
        std::nullopt, {}, "effective", std::move(life_state), std::move(posture)};
}

synth::core::RuntimeSnapshot snapshot(synth::core::RuntimeVariant variant,
                                      synth::core::SnapshotClock::time_point now,
                                      std::optional<synth::core::TimedPose> hmd = std::nullopt) {
    using namespace synth::core;
    std::vector<ActorSnapshot> actors;
    actors.emplace_back(2, "side", Vec3{5, 2, 0}, "Actors.esm", "save-a");
    actors.emplace_back(1, "center", Vec3{10, 0.25, 0}, "Actors.esm", "save-a");
    actors.emplace_back(3, "near", Vec3{2, 5, 0}, "Actors.esm", "save-a");
    return RuntimeSnapshot{Game::fallout4, variant, RuntimeGeneration::initial(), 4, now, pose({}),
                           std::move(hmd),
                           std::nullopt,
                           std::nullopt,
                           ActorSnapshot{0x14, "Sole Survivor", {}, "Fallout4.esm", "save-a"},
                           std::move(actors)};
}
}

int main() {
    try {
        using namespace synth;
        const auto api_now=core::SnapshotClock::now();
        const auto api_flat=snapshot(core::RuntimeVariant::flat,api_now);
        CHECK(integration::external_actor(api_flat,2,api_now,100)->form_id()==2);
        CHECK(!integration::external_actor(api_flat,0x1234,api_now,100));
        CHECK(!integration::external_actor(api_flat,0x14,api_now,100));
        CHECK(!integration::external_actor(api_flat,2,api_now,1));
        CHECK(!integration::external_actor(api_flat,2,api_now-1ms,100));
        CHECK(!integration::external_actor(api_flat,2,api_now+101ms,100));
        CHECK(!integration::external_actor(api_flat,2,api_now,0));
        CHECK(!integration::external_actor(snapshot(core::RuntimeVariant::vr,api_now),2,api_now,100));
        const auto api_vr=snapshot(core::RuntimeVariant::vr,api_now,core::TimedPose{pose({5,2,0}),api_now,50ms,4});
        CHECK(integration::external_actor(api_vr,2,api_now,1)->form_id()==2);
        CHECK(!integration::external_actor(api_vr,2,api_now+51ms,1));
        const auto api_stale=snapshot(core::RuntimeVariant::vr,api_now,core::TimedPose{pose({5,2,0}),api_now,50ms,3});
        CHECK(!integration::external_actor(api_stale,2,api_now,1));
        std::vector<core::ActorSnapshot> crowded_actors;
        for (std::uint32_t id=1; id<=20; ++id)
            crowded_actors.emplace_back(id+100,"NPC",core::Vec3{double(id),0,0},"Actors.esm","save-a");
        const core::RuntimeSnapshot crowded{core::Game::fallout4,core::RuntimeVariant::flat,
            core::RuntimeGeneration::initial(),1,core::SnapshotClock::now(),pose({}),std::nullopt,
            std::nullopt,std::nullopt,core::ActorSnapshot{0x14,"Player",{},"Fallout4.esm","save-a"},crowded_actors};
        const auto selected=[](const core::ActorSnapshot& actor) {
            return actor.form_id()==120 && actor.origin_plugin()=="Actors.esm" && actor.playthrough_id()=="save-a";
        };
        const auto crowded_packet=targeting::context_participants(crowded,100,selected);
        CHECK(crowded_packet.size()==16);
        CHECK(crowded_packet.front()==&crowded.actors().back());
        CHECK(crowded_packet[1]->form_id()==101);
        CHECK(crowded_packet[14]->form_id()==114); // Player plus these15 states still includes selected NPC.
        CHECK(targeting::context_participants(crowded,0,selected).size()==1);
        CHECK(targeting::context_participants(crowded,0).empty());
        CHECK(targeting::context_participants(crowded,100).front()->form_id()==101);
        CHECK(crowded.actors().front().form_id()==101); // Ordering never mutates the owned snapshot.
        bool missing_rejected{};
        try { (void)targeting::context_participants(crowded,100,[](const auto& actor){return actor.form_id()==999;}); }
        catch(const std::invalid_argument&) {missing_rejected=true;}
        CHECK(missing_rejected);
        for (const auto& unavailable : {
                core::ActorSnapshot{120,"NPC",{},"Actors.esm","save-a",false},
                core::ActorSnapshot{120,"NPC",{},"Actors.esm","other-save"},
                core::ActorSnapshot{120,"NPC",{},"Other.esm","save-a"}}) {
            const core::RuntimeSnapshot invalid_target{core::Game::fallout4,core::RuntimeVariant::flat,
                core::RuntimeGeneration::initial(),1,core::SnapshotClock::now(),pose({}),std::nullopt,
                std::nullopt,std::nullopt,core::ActorSnapshot{0x14,"Player",{},"Fallout4.esm","save-a"},{unavailable}};
            bool rejected{};
            try { (void)targeting::context_participants(invalid_target,100,selected); }
            catch(const std::invalid_argument&) {rejected=true;}
            CHECK(rejected);
        }
        CHECK(targeting::context_participants(crowded,100,[](const auto& actor){return actor.form_id()==0x14;}).size()==16);
        CHECK(targeting::automatic_actor_class_supported("HumanRace", false));
        CHECK(targeting::automatic_actor_class_supported("GhoulRace", false));
        CHECK(targeting::automatic_actor_class_supported("SuperMutantRace", false));
        CHECK(targeting::automatic_actor_class_supported("SynthGen2Race", false));
        CHECK(targeting::automatic_actor_class_supported("HandyRace", true));
        CHECK(!targeting::automatic_actor_class_supported("DeathclawRace", false));
        CHECK(!targeting::automatic_actor_class_supported("", false));
        CHECK(targeting::dialogue_actor_available(actor_with_state("alive", "normal")));
        CHECK(targeting::dialogue_actor_available(actor_with_state("unknown", "unknown")));
        CHECK(!targeting::dialogue_actor_available(actor_with_state("unconscious", "normal")));
        CHECK(targeting::dialogue_actor_available(actor_with_state("restrained", "normal")));
        CHECK(!targeting::dialogue_actor_available(actor_with_state("bleedout", "normal")));
        CHECK(!targeting::dialogue_actor_available(actor_with_state("alive", "sleeping")));
        CHECK(!targeting::dialogue_actor_available(actor_with_state("alive", "want_to_sleep")));
        const auto now = core::SnapshotClock::now();
        const context::PlaythroughIdentity save{"save-a"};
        auto flat = snapshot(core::RuntimeVariant::flat, now);
        const auto copied = std::make_shared<const core::RuntimeSnapshot>(flat);
        core::AutomaticDiaryQueue diaries{core::RuntimeGeneration::initial()};
        core::AutomaticDiaryQueue other_generation{core::RuntimeGeneration::from_value(2)};
        CHECK(!other_generation.push(copied, core::RestStart::sleep, now));
        CHECK(!diaries.push(nullptr, core::RestStart::sleep, now));
        CHECK(!diaries.push(copied, static_cast<core::RestStart>(0), now));
        CHECK(!diaries.push(copied, core::RestStart::sleep, now - 1ms));
        CHECK(!diaries.push(copied, core::RestStart::sleep, now + 2min));
        for (std::size_t i = 0; i < core::AutomaticDiaryQueue::capacity; ++i)
            CHECK(diaries.push(copied, i % 2 ? core::RestStart::wait : core::RestStart::sleep, now));
        CHECK(!diaries.push(copied, core::RestStart::sleep, now));
        CHECK(diaries.front()->snapshot == copied);
        CHECK(diaries.front()->reason == core::RestStart::sleep);
        diaries.pop();
        CHECK(diaries.front()->reason == core::RestStart::wait);
        CHECK(diaries.expire(now + 2min - 1ms) == 0);
        CHECK(diaries.expire(now + 2min) == 15);
        CHECK(diaries.front() == nullptr);
        diaries.pop();
        CHECK(diaries.push(copied, core::RestStart::wait, now));
        diaries.clear();
        CHECK(diaries.size() == 0);
        CHECK(!targeting::select_flat_crosshair(flat, save, {100, 3}).target);
        const auto picked = flat.with_picked_actor(flat.actors()[1]);
        const auto ray = targeting::select_flat_crosshair(picked, save, {100, 3});
        CHECK(ray.status == targeting::TargetingStatus::selected);
        CHECK(ray.target->identity().form().form_id() == 1);
        CHECK(picked.picked_actor_form_id() == 1U);
        CHECK(!flat.picked_actor_form_id());
        CHECK(!targeting::select_flat_crosshair(picked.with_picked_actor(std::nullopt), save).target);
        CHECK(!targeting::select_flat_crosshair(picked, save, {1, 3}).target);
        CHECK(!targeting::select_flat_crosshair(picked, save, {}, [](const auto&) { return false; }).target);
        const auto eligible = [](const auto&) { return true; };
        bool nearby{};
        CHECK(targeting::select_conversation(flat, save, now, {100, 0}, eligible, eligible, &nearby)
            .target->identity().form().form_id() == 2);
        CHECK(nearby); // A crosshair miss no longer prevents ordinary conversation.
        CHECK(targeting::select_conversation(picked, save, now, {100, 0}, eligible, eligible, &nearby)
            .target->identity().form().form_id() == picked.picked_actor_form_id());
        CHECK(!nearby);
        CHECK(!targeting::select_conversation(flat, save, now, {1, 0}, eligible, eligible).target);
        CHECK(!targeting::select_conversation(flat, save, now, {100, 0}, eligible,
            [](const auto&) { return false; }).target);
        CHECK(targeting::conversation_range(1050, "STANDARD") == 1050);
        CHECK(targeting::conversation_range(1050, "SHOUT") == 2100);
        CHECK(targeting::conversation_range(1050, "WHISPER") == 200);
        CHECK(targeting::conversation_range(1750, "CLOSE") == 200);
        CHECK(targeting::nearby_conversation_audible(1050, 784, 784, std::nullopt));
        CHECK(!targeting::nearby_conversation_audible(1050, 784, 785, std::nullopt));
        CHECK(targeting::nearby_conversation_audible(1050, 784, 900, true));
        CHECK(!targeting::nearby_conversation_audible(1050, 784, 900, false));
        CHECK(!targeting::nearby_conversation_audible(200, 784, 201, true));
        CHECK(!flat.actors()[0].conversation_area_available()); // Missing native area proof fails closed.
        const std::vector<core::ActorSnapshot> behind_actors{
            {8, "Behind", {-20, 0, 0}, "Actors.esm", "save-a"},
            {7, "Beside", {0, 20, 0}, "Actors.esm", "save-a"},
            {9, "Ahead", {40, 0, 0}, "Actors.esm", "save-a"}};
        const core::RuntimeSnapshot behind{core::Game::fallout4, core::RuntimeVariant::flat,
            core::RuntimeGeneration::initial(), 4, now, pose({}), {}, {}, {}, flat.player(), behind_actors};
        CHECK(targeting::select_conversation(behind, save, now, {100, 0}, eligible, eligible)
            .target->identity().form().form_id() == 7); // Stable FormID tie, not gaze direction.
        CHECK(targeting::select_conversation(behind, save, now, {100, 0}, eligible,
            [](const auto& actor) { return actor.form_id() != 7; })
            .target->identity().form().form_id() == 8);
        CHECK(targeting::select_conversation(behind.with_picked_actor(behind_actors[2]), save, now,
            {100, 0}, eligible, eligible).target->identity().form().form_id() == 9);
        const core::ActorSnapshot outside{900, "picked outside discovery", {10, 20, 0}, "Actors.esm", "save-a"};
        const auto reserved = flat.with_picked_actor(outside);
        CHECK(targeting::select_flat_crosshair(reserved, save, {100, 0}).target->identity().form().form_id() == 900);
        CHECK(reserved.actors_observation() == "partial");
        CHECK(reserved.with_actor_details(900, outside).picked_actor_form_id() == 900U);
        CHECK(reserved.with_requested_actor(flat.actors()[0]).picked_actor_form_id() == 900U);
        auto full_crowd=crowded_actors;
        for (std::uint32_t id=121; id<=164; ++id)
            full_crowd.emplace_back(id,"Crowd",core::Vec3{1,0,0},"Actors.esm","save-a");
        const core::RuntimeSnapshot full{core::Game::fallout4,core::RuntimeVariant::flat,
            core::RuntimeGeneration::initial(),4,now,pose({}),{},{},{},flat.player(),full_crowd};
        const auto exact=full.with_picked_actor(outside);
        CHECK(exact.actors().size()==64);
        CHECK(exact.actors().front().form_id()==900);
        CHECK(targeting::select_flat_crosshair(exact,save).target->identity().form().form_id()==900);
        CHECK(full.actors().size()==64 && !full.picked_actor_form_id());
        bool foreign_pick_rejected{};
        try { (void)flat.with_picked_actor(core::ActorSnapshot{901,"Foreign",{},"Actors.esm","other"}); }
        catch(const std::invalid_argument&) { foreign_pick_rejected=true; }
        CHECK(foreign_pick_rejected);
        core::PickedReferenceMailbox mailbox;
        CHECK(!mailbox.read());
        CHECK(!mailbox.record(mailbox.stamp(), 42));
        mailbox.arm();
        CHECK(!mailbox.read());
        const auto old = mailbox.stamp();
        CHECK(mailbox.record(old, 42));
        CHECK(mailbox.read() == 42U);
        CHECK(!mailbox.record(old, 99));
        CHECK(mailbox.read() == 42U);
        CHECK(mailbox.record(mailbox.stamp(), 0));
        CHECK(mailbox.read() == 0U);
        const auto prior_save = mailbox.stamp();
        mailbox.invalidate();
        CHECK(!mailbox.read());
        mailbox.arm();
        CHECK(!mailbox.record(prior_save, 42));
        CHECK(!mailbox.read());
        CHECK(mailbox.record(mailbox.stamp(), 77));
        mailbox.arm();
        CHECK(mailbox.read() == 77U);
        const auto before_repeat=mailbox.stamp();
        CHECK(mailbox.record(before_repeat,77));
        CHECK(mailbox.stamp()!=before_repeat);
        CHECK(!mailbox.record(before_repeat,99));
        CHECK(mailbox.read()==77U);
        const auto nearest = targeting::select_nearest(flat, save, {}, 20);
        CHECK(nearest.target->identity().form().form_id() == 2);
        const auto audience = targeting::build_audience(flat, save, {}, 20, 2);
        CHECK(audience.members().size() == 2);

        auto fresh_vr = snapshot(core::RuntimeVariant::vr, now,
            core::TimedPose{pose({0, 0, 0}), now, 20ms, 4});
        CHECK(targeting::select_vr_hmd_gaze(fresh_vr, save, now + 10ms, {100, 3}).status ==
              targeting::TargetingStatus::selected);
        CHECK(targeting::select_vr_hmd_gaze(fresh_vr, save, now + 21ms, {100, 3}).status ==
              targeting::TargetingStatus::stale_pose);
        CHECK(targeting::select_conversation(fresh_vr, save, now + 21ms, {100, 3}, eligible, eligible).status ==
              targeting::TargetingStatus::stale_pose);
        targeting::ControllerRay controller{targeting::Hand::right,
            core::TimedPose{pose({}, {0, 1, 0}), now, 20ms, 4}, true};
        CHECK(targeting::select_vr_controller_ray(fresh_vr, save, controller, now + 5ms, {100, 3})
                  .target->identity().form().form_id() == 3);
        targeting::ControllerRay old_frame{targeting::Hand::right,
            core::TimedPose{pose({}), now, 20ms, 3}, true};
        CHECK(targeting::select_vr_controller_ray(fresh_vr, save, old_frame, now + 5ms).status ==
              targeting::TargetingStatus::stale_pose);
        CHECK(targeting::select_flat_crosshair(flat, context::PlaythroughIdentity{"other"}).status ==
              targeting::TargetingStatus::identity_mismatch);
        std::cout << "targeting tests passed (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
