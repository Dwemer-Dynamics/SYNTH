#include "adapters/adapter_descriptor.hpp"
#include "adapters/adapter_lifecycle.hpp"
#include "adapters/flat_fallout_runtime.hpp"
#include "adapters/vr_fallout_runtime.hpp"
#include "core/actor_discovery_scan.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

int assertions{};

#define CHECK(condition)                                                                        \
    do {                                                                                        \
        ++assertions;                                                                           \
        if (!(condition)) {                                                                     \
            throw std::runtime_error{std::string{"CHECK failed: "} + #condition + " at " +     \
                                     __FILE__ + ":" + std::to_string(__LINE__)};                \
        }                                                                                       \
    } while (false)

using synth::adapters::AdapterLifecycle;
using synth::adapters::DeferredLifecycleActions;
using synth::adapters::LifecycleAction;
using synth::adapters::LifecycleEvent;
using synth::adapters::LifecycleState;
using synth::core::RuntimeCapability;
using synth::core::RuntimeVariant;

void test_descriptors_and_capabilities() {
    using namespace synth::adapters;
    CHECK(flat_descriptor.plugin_name == "SYNTH");
    CHECK(flat_descriptor.adapter_name == "flat-f4se");
    CHECK(flat_descriptor.variant == RuntimeVariant::flat);
    CHECK(flat_descriptor.runtime_version == flat_runtime_version);
    CHECK(flat_descriptor.script_extender_version == flat_f4se_version);
    CHECK(flat_descriptor.capabilities.size() == 1);
    CHECK(has_capability(flat_descriptor, RuntimeCapability::notifications));
    CHECK(!has_capability(flat_descriptor, RuntimeCapability::hmd_pose));

    CHECK(vr_descriptor.plugin_name == "SYNTHVR");
    CHECK(vr_descriptor.variant == RuntimeVariant::vr);
    CHECK(vr_descriptor.runtime_version == vr_runtime_version);
    CHECK(vr_descriptor.script_extender_version == f4sevr_version);
    CHECK(vr_descriptor.capabilities.size() == 3);
    CHECK(has_capability(vr_descriptor, RuntimeCapability::hmd_pose));
    CHECK(has_capability(vr_descriptor, RuntimeCapability::controller_pose));
    CHECK(!has_capability(vr_descriptor, RuntimeCapability::semantic_input));
}

void test_exact_version_and_wrong_lane_rejection() {
    using namespace synth::adapters;
    CHECK(supports_exact_environment(
        flat_descriptor, RuntimeVariant::flat, flat_runtime_version, flat_f4se_version));
    CHECK(!supports_exact_environment(
        flat_descriptor, RuntimeVariant::vr, flat_runtime_version, flat_f4se_version));
    CHECK(!supports_exact_environment(
        flat_descriptor, RuntimeVariant::flat, {1, 11, 220, 0}, flat_f4se_version));
    CHECK(!supports_exact_environment(
        flat_descriptor, RuntimeVariant::flat, flat_runtime_version, {0, 7, 7, 0}));

    CHECK(supports_exact_environment(
        vr_descriptor, RuntimeVariant::vr, vr_runtime_version, f4sevr_version));
    CHECK(!supports_exact_environment(
        vr_descriptor, RuntimeVariant::flat, vr_runtime_version, f4sevr_version));
    CHECK(!supports_exact_environment(
        vr_descriptor, RuntimeVariant::vr, {1, 2, 71, 0}, f4sevr_version));
    CHECK(!supports_exact_environment(
        vr_descriptor, RuntimeVariant::vr, vr_runtime_version, {0, 6, 20, 0}));
}

void test_lifecycle_emits_runtime_actions() {
    using namespace synth::adapters;
    AdapterLifecycle wrong_lane;
    CHECK(!wrong_lane.load(
        flat_descriptor, RuntimeVariant::vr, flat_runtime_version, flat_f4se_version));
    CHECK(wrong_lane.state() == LifecycleState::unloaded);

    AdapterLifecycle lifecycle;
    CHECK(lifecycle.load(
        flat_descriptor, RuntimeVariant::flat, flat_runtime_version, flat_f4se_version));
    CHECK(!lifecycle.load(
        flat_descriptor, RuntimeVariant::flat, flat_runtime_version, flat_f4se_version));
    CHECK(lifecycle.handle(LifecycleEvent::plugin_loaded) == LifecycleAction::none);
    CHECK(lifecycle.handle(LifecycleEvent::game_data_ready) == LifecycleAction::none);
    CHECK(lifecycle.state() == LifecycleState::loaded);
    CHECK(lifecycle.handle(LifecycleEvent::post_load_game) == LifecycleAction::ready);
    CHECK(lifecycle.state() == LifecycleState::ready);
    CHECK(lifecycle.handle(LifecycleEvent::pre_load_game) == LifecycleAction::invalidate);
    CHECK(lifecycle.state() == LifecycleState::loaded);
    CHECK(lifecycle.handle(LifecycleEvent::post_load_game) == LifecycleAction::ready);
    CHECK(lifecycle.handle(LifecycleEvent::new_game) == LifecycleAction::invalidate_and_ready);
    CHECK(lifecycle.state() == LifecycleState::ready);
    CHECK(lifecycle.handle(LifecycleEvent::shutdown) == LifecycleAction::stop);
    CHECK(lifecycle.state() == LifecycleState::stopped);
    CHECK(lifecycle.handle(LifecycleEvent::game_data_ready) == LifecycleAction::rejected);
}

void test_deferred_lifecycle_actions_coalesce_load_boundaries() {
    DeferredLifecycleActions deferred;
    CHECK(deferred.take() == LifecycleAction::none);

    deferred.push(LifecycleAction::none);
    deferred.push(LifecycleAction::rejected);
    CHECK(deferred.take() == LifecycleAction::none);

    deferred.push(LifecycleAction::invalidate);
    deferred.push(LifecycleAction::ready);
    CHECK(deferred.take() == LifecycleAction::invalidate_and_ready);
    CHECK(deferred.take() == LifecycleAction::none);

    deferred.push(LifecycleAction::ready);
    CHECK(deferred.take() == LifecycleAction::ready);
    deferred.push(LifecycleAction::invalidate_and_ready);
    CHECK(deferred.take() == LifecycleAction::invalidate_and_ready);

    deferred.push(LifecycleAction::ready);
    deferred.push(LifecycleAction::stop);
    CHECK(deferred.take() == LifecycleAction::stop);
    CHECK(deferred.take() == LifecycleAction::none);
}

void test_product_owned_pump_seams_and_invalidation() {
    using namespace synth::adapters;
    using synth::runtime::IFalloutRuntime;
    static_assert(std::is_base_of_v<IFalloutRuntime, FlatFalloutRuntime>);
    static_assert(std::is_base_of_v<IFalloutRuntime, VrFalloutRuntime>);

    const auto now = synth::core::SnapshotClock::now();
    std::vector<std::string> notifications;
    int flat_captures{};
    FlatFalloutRuntime flat{
        [&](synth::core::SnapshotClock::time_point,
            synth::runtime::RuntimeCapturePurpose) {
            ++flat_captures;
            return detail::CapturedRuntimeValues{
                synth::core::WorldPose{
                    {4.0, 5.0, 6.0},
                    synth::core::UnitVector3::from({0.0, 1.0, 0.0}),
                    synth::core::UnitVector3::from({0.0, 0.0, 1.0})},
                std::nullopt,
                std::nullopt,
                std::nullopt,
                synth::core::ActorSnapshot{0x14, "Sole Survivor", {}, "Fallout4.esm", "save-a"},
                {},
                123,
                synth::core::WorldState{"Sanctuary Hills", "SanctuaryExt", "Commonwealth",
                                        "CommonwealthClear", false, 123},
                {synth::core::LoadedPlugin{"Fallout4.esm", false, 0, 0, 0, "00"}},
                {synth::core::QuestSnapshot{
                    0x000229E5, "Fallout4.esm", "When Freedom Calls", "Min00", 45, 1,
                    {"Join Preston Garvey in Sanctuary"}}},
                {synth::core::NearbyItemSnapshot{
                    0xFF001234, 0x00004822, 0x00018AA2, std::nullopt, "Fallout4.esm",
                    "Fallout4.esm", "10mm Pistol", {12.0, 22.0, 30.0}, 14.14, 4.2,
                    1, 50, 43, false, true, false}},
                {synth::core::PointOfInterestSnapshot{
                    0x0001A6D8, 0x0001A6D7, 0x00018AA2, std::string{"Fallout4.esm"},
                    "Fallout4.esm", "Fallout4.esm", "Sanctuary workshop door", "door",
                    {40.0, 20.0, 30.0}, 40.0, false, false}}};
        },
        [&](std::string_view message) { notifications.emplace_back(message); },
        [](const synth::runtime::RuntimeActionRequest& request) {
            CHECK(request.name == synth::runtime::RuntimeActionName::sheathe_weapon);
            CHECK(request.actor.form_id == 0x0001A4D7);
            return synth::runtime::RuntimeActionResult{
                synth::runtime::RuntimeActionStatus::succeeded, "flat action verified"};
        }};

    auto flat_snapshot = flat.capture_snapshot(now);
    CHECK(flat_captures == 1);
    CHECK(flat_snapshot->variant() == RuntimeVariant::flat);
    CHECK(flat_snapshot->player_pose().position() == (synth::core::Vec3{4.0, 5.0, 6.0}));
    CHECK(!flat_snapshot->hmd_pose());
    CHECK(flat_snapshot->world() && flat_snapshot->world()->location == "Sanctuary Hills");
    CHECK(flat_snapshot->loaded_plugins().size() == 1);
    CHECK(flat_snapshot->active_quests().size() == 1);
    CHECK(flat_snapshot->active_quests()[0].objectives.size() == 1);
    CHECK(flat_snapshot->nearby_items().size() == 1);
    CHECK(flat_snapshot->points_of_interest().size() == 1);
    flat.present_notification("flat");
    CHECK(notifications == std::vector<std::string>{"flat"});
    const auto flat_action = flat.execute_action({
        synth::runtime::RuntimeActionName::sheathe_weapon,
        {0x0001A4D7, "Fallout4.esm", "save-a"}});
    CHECK(flat_action.status == synth::runtime::RuntimeActionStatus::succeeded);
    CHECK(flat_action.detail == "flat action verified");
    const auto first_generation = flat_snapshot->generation();
    CHECK(flat.invalidate().value() == first_generation.value() + 1);
    auto invalidated_snapshot = flat.capture_snapshot(now);
    CHECK(invalidated_snapshot->generation() != first_generation);
    CHECK(invalidated_snapshot->frame() == 1);

    const synth::core::WorldPose hmd{
        {1.0, 2.0, 3.0},
        synth::core::UnitVector3::from({0.0, 1.0, 0.0}),
        synth::core::UnitVector3::from({0.0, 0.0, 1.0}),
    };
    VrFalloutRuntime vr{
        [=](synth::core::SnapshotClock::time_point,
            synth::runtime::RuntimeCapturePurpose) {
            return detail::CapturedRuntimeValues{
                synth::core::WorldPose{
                    {},
                    synth::core::UnitVector3::from({0.0, 1.0, 0.0}),
                    synth::core::UnitVector3::from({0.0, 0.0, 1.0})},
                hmd,
                hmd,
                hmd,
                synth::core::ActorSnapshot{0x14, "Sole Survivor", {}, "Fallout4.esm", "save-a"},
                {},
                456,
                synth::core::WorldState{"Vault 111", "Vault111Cryo", "", "", true, 456},
                {synth::core::LoadedPlugin{"Fallout4.esm", false, 0, 0, 0, "00"}},
                {synth::core::QuestSnapshot{
                    0x000229E5, "Fallout4.esm", "When Freedom Calls", "Min00", 45, 1,
                    {"Join Preston Garvey in Sanctuary"}}},
                {},
                {}};
        },
        [](std::string_view) {},
        [](const synth::runtime::RuntimeActionRequest& request) {
            CHECK(request.name == synth::runtime::RuntimeActionName::sheathe_weapon);
            return synth::runtime::RuntimeActionResult{
                synth::runtime::RuntimeActionStatus::succeeded, "vr action verified"};
        }};
    auto vr_snapshot = vr.capture_snapshot(now);
    CHECK(vr_snapshot->variant() == RuntimeVariant::vr);
    CHECK(vr_snapshot->hmd_pose().has_value());
    CHECK(vr_snapshot->hmd_pose()->pose().position() == (synth::core::Vec3{1.0, 2.0, 3.0}));
    CHECK(vr_snapshot->world() && vr_snapshot->world()->interior);
    CHECK(vr_snapshot->active_quests().size() == 1);
    const auto vr_action = vr.execute_action({
        synth::runtime::RuntimeActionName::sheathe_weapon,
        {0x0001A4D7, "Fallout4.esm", "save-a"}});
    CHECK(vr_action.status == synth::runtime::RuntimeActionStatus::succeeded);
    CHECK(vr_action.detail == "vr action verified");

    bool flat_rejected{};
    bool vr_rejected{};
    std::thread worker{[&] {
        try {
            (void)flat.capture_snapshot(now);
        } catch (const std::logic_error&) {
            flat_rejected = true;
        }
        try {
            vr.present_notification("worker");
        } catch (const std::logic_error&) {
            vr_rejected = true;
        }
    }};
    worker.join();
    CHECK(flat_rejected);
    CHECK(vr_rejected);

    bool rebound_capture_succeeded{};
    std::thread game_task_worker{[&] {
        flat.rebind_game_thread();
        rebound_capture_succeeded = flat.capture_snapshot(now) != nullptr;
    }};
    game_task_worker.join();
    CHECK(rebound_capture_succeeded);
    CHECK(!flat.is_game_thread());
}

// Exercise the same adapter gate in both lanes without touching an engine or provider.
void test_selected_actor_enrichment_ownership() {
    using namespace synth;
    for (const auto* descriptor : {&adapters::flat_descriptor, &adapters::vr_descriptor}) {
        const core::WorldPose pose{{},core::UnitVector3::from({1,0,0}),core::UnitVector3::from({0,0,1})};
        const core::ActorSnapshot near{1,"Near",{1,0,0},"Actors.esm","save-a"};
        const core::ActorSnapshot selected{2,"Selected",{10,0,0},"Actors.esm","save-a"};
        adapters::detail::RuntimeBase runtime{*descriptor,
            [&](auto,auto) { return adapters::detail::CapturedRuntimeValues{pose,{},{},{},
                core::ActorSnapshot{0x14,"Player",{},"Fallout4.esm","save-a"},{near,selected},42,
                core::WorldState{"Sanctuary","Cell","Commonwealth","Clear",false,42}}; },
            [](auto){},[](const auto&){return synth::runtime::RuntimeActionResult{};}};
        auto snapshot=runtime.capture_snapshot(core::SnapshotClock::now());
        CHECK(!runtime.enrich_actor_snapshot(snapshot,2));
        int calls{},mode{};
        runtime.set_actor_detail_pump([&](const core::ActorSnapshot& actor)->std::optional<core::ActorSnapshot> {
            ++calls; CHECK(actor.form_id()==2);
            if(mode==1)return {};
            if(mode==3)(void)runtime.invalidate();
            return core::ActorSnapshot{mode==2?1U:2U,"Live renamed",{99,0,0},"Actors.esm","save-a",
                true,false,false,false,false,9,"Human","female","Voice",false,50,40,
                {{0x42,"Fallout4.esm","Item",3,1,0.5,41,false}},std::nullopt,
                {{0x43,"Fallout4.esm","Faction","Faction",1}},"effective","alive","normal",false,0,false,false,false,
                core::PackageSnapshot{0x44,"Fallout4.esm","Package","Package"},0,"","complete","complete",true,true};
        });
        auto enriched=runtime.enrich_actor_snapshot(snapshot,2);
        CHECK(enriched && calls==1);
        CHECK(enriched->actors()[1].name()=="Selected");
        CHECK(enriched->actors()[1].position()==selected.position());
        CHECK(enriched->actors()[1].health_percent()==selected.health_percent());
        CHECK(enriched->actors()[1].health_percent_available()==selected.health_percent_available());
        CHECK(enriched->actors()[1].action_points_percent_available()==selected.action_points_percent_available());
        CHECK(enriched->actors()[1].inventory_observation()=="complete");
        CHECK(enriched->actors()[1].inventory()[0].count==3);
        CHECK(enriched->actors()[1].factions().size()==1);
        CHECK(enriched->actors()[1].faction_completeness()=="complete");
        CHECK(snapshot->actors()[1].faction_completeness()=="unavailable");
        CHECK(enriched->actors()[1].current_package()->form_id==0x44);
        CHECK(enriched->actors()[0].inventory_observation()=="unavailable");
        CHECK(enriched->actors()[0].faction_completeness()=="unavailable");
        CHECK(snapshot->actors()[1].inventory().empty());
        CHECK(enriched->generation()==snapshot->generation() && enriched->frame()==snapshot->frame());
        CHECK(enriched->captured_at()==snapshot->captured_at());
        CHECK(enriched->world()->location==snapshot->world()->location);
        CHECK(!runtime.enrich_actor_snapshot(snapshot,99));
        CHECK(!runtime.enrich_actor_snapshot(snapshot,0x14));
        CHECK(calls==1);
        bool worker_rejected{};
        std::thread worker{[&]{try{(void)runtime.enrich_actor_snapshot(snapshot,2);}catch(const std::logic_error&){worker_rejected=true;}}};
        worker.join(); CHECK(worker_rejected && calls==1);
        mode=1; CHECK(!runtime.enrich_actor_snapshot(snapshot,2));
        mode=2; bool wrong_identity{};
        try{(void)runtime.enrich_actor_snapshot(snapshot,2);}catch(const std::invalid_argument&){wrong_identity=true;}
        CHECK(wrong_identity);
        auto fresh=runtime.capture_snapshot(core::SnapshotClock::now());
        const auto before=calls; CHECK(!runtime.enrich_actor_snapshot(snapshot,2)); CHECK(calls==before);
        mode=3; CHECK(!runtime.enrich_actor_snapshot(fresh,2));
        CHECK(!runtime.enrich_actor_snapshot(fresh,2)); CHECK(calls==before+1);
    }
}

// Automatic requests enrich only missing player inventory and cannot cross a native observation boundary.
void test_request_player_inventory_ownership() {
    using namespace synth;
    const core::WorldPose pose{{},core::UnitVector3::from({1,0,0}),core::UnitVector3::from({0,0,1})};
    const core::ActorSnapshot npc{2,"Settler",{10,0,0},"Actors.esm","save-a"};
    const auto player = [](std::string quality, std::string owner = "save-a", unsigned form = 0x14) {
        const auto available = quality != "unavailable";
        return core::ActorSnapshot{form,"Player observed",{90,0,0},"Fallout4.esm",std::move(owner),
            true,false,false,false,false,12,"HumanRace","female",{},false,55,66,
            !available ? std::vector<core::InventoryItemSnapshot>{} :
                std::vector<core::InventoryItemSnapshot>{{0x456,"Items.esm","Equipped rifle",1,20,3,43,true}},
            std::nullopt,{},"unavailable","alive","normal",false,0,false,false,false,std::nullopt,
            0,"",std::move(quality)};
    };
    const core::ActorSnapshot original{0x14,"Original player",{},"Fallout4.esm","save-a"};
    std::uint64_t epoch = 1;
    int mode{}, captures{}, actor_calls{}, player_calls{};
    adapters::detail::RuntimeBase runtime{adapters::flat_descriptor,
        [&](auto,auto) {
            ++captures;
            if (mode == 1) ++epoch;
            return adapters::detail::CapturedRuntimeValues{pose,{},{},{},original,{npc},42};
        }, [](auto){}, [](const auto&){return synth::runtime::RuntimeActionResult{};}};
    runtime.set_capture_epoch_reader([&]{return epoch;});
    runtime.set_actor_detail_pump([&](const auto& actor)->std::optional<core::ActorSnapshot> {
        ++actor_calls;
        if (mode == 2) ++epoch;
        return actor;
    });
    runtime.set_player_inventory_pump([&](const auto& selected)->std::optional<core::ActorSnapshot> {
        ++player_calls;
        CHECK(selected.form_id() == 0x14);
        if (mode == 3) return {};
        if (mode == 4) ++epoch;
        if (mode == 5) (void)runtime.invalidate();
        return player(mode == 8 ? "unavailable" : "complete", mode == 6 ? "other-save" : "save-a", mode == 7 ? 99 : 0x14);
    });
    const auto now = core::SnapshotClock::now();
    auto scene = runtime.capture_snapshot(now, runtime::RuntimeCapturePurpose::background);
    auto enriched = runtime.enrich_actor_snapshot(scene,2);
    CHECK(enriched && actor_calls == 1 && player_calls == 1);
    CHECK(enriched->player().inventory().size() == 1 && enriched->player().inventory()[0].equipped);
    CHECK(enriched->player().inventory_observation() == "complete");
    CHECK(enriched->player().name() == original.name() && enriched->player().position() == original.position());
    CHECK(enriched->player().health_percent() == original.health_percent() && enriched->player().level() == original.level());
    CHECK(enriched->player().factions().empty() && !enriched->player().current_package());
    CHECK(enriched->frame() == scene->frame() && enriched->captured_at() == scene->captured_at());
    CHECK(scene->player().inventory().empty() && scene->player().inventory_observation() == "unavailable");
    CHECK(runtime.enrich_actor_snapshot(enriched,2) && player_calls == 1); // Already observed dialogue inventory is not recaptured.
    ++epoch;
    const auto before = actor_calls;
    CHECK(!runtime.enrich_actor_snapshot(scene,2) && actor_calls == before);
    for (mode = 2; mode <= 8; ++mode) {
        scene = runtime.capture_snapshot(now, runtime::RuntimeCapturePurpose::background);
        bool invalid_identity = false;
        std::shared_ptr<const core::RuntimeSnapshot> result;
        try { result = runtime.enrich_actor_snapshot(scene,2); }
        catch (const std::invalid_argument&) { invalid_identity = true; }
        if (mode == 6 || mode == 7) CHECK(invalid_identity);
        else if (mode == 8) CHECK(result && result->player().inventory_observation() == "unavailable");
        else CHECK(!result);
    }
    mode = 0;
    {
        const auto batch = runtime.capture_batch();
        const auto first = runtime.capture_snapshot(now);
        CHECK(runtime.capture_snapshot(now) == first);
        ++epoch;
        const auto refreshed = runtime.capture_snapshot(now);
        CHECK(refreshed != first && refreshed->frame() > first->frame());
        CHECK(!runtime.enrich_actor_snapshot(first,2));
    }
    mode = 1;
    bool crossed = false;
    try { (void)runtime.capture_snapshot(now); } catch (const std::runtime_error&) { crossed = true; }
    CHECK(crossed);
    mode = 0;
    scene = runtime.capture_snapshot(now);
    CHECK(runtime.enrich_actor_snapshot(scene,2) != nullptr);
    runtime.set_requested_actor_pump([&](unsigned,const std::string&)->std::optional<core::ActorSnapshot> {
        ++epoch; return npc;
    });
    CHECK(!runtime.capture_actor_snapshot(now,2));
}

void test_requested_actor_capture_outside_discovery() {
    using namespace synth;
    for (const auto* descriptor : {&adapters::flat_descriptor, &adapters::vr_descriptor}) {
        const core::WorldPose pose{{},core::UnitVector3::from({1,0,0}),core::UnitVector3::from({0,0,1})};
        std::vector<core::ActorSnapshot> crowd;
        for (std::uint32_t id=100; id<164; ++id) crowd.emplace_back(id,"Nearby",core::Vec3{},"Actors.esm","save-a");
        int captures{},calls{},mode{};
        adapters::detail::RuntimeBase runtime{*descriptor,
            [&](auto,auto) { ++captures; return adapters::detail::CapturedRuntimeValues{pose,
                descriptor->variant==RuntimeVariant::vr ? std::optional{pose} : std::nullopt,{},{},
                core::ActorSnapshot{0x14,"Player",{},"Fallout4.esm","save-a"},crowd,42,
                core::WorldState{"Sanctuary","Cell","Commonwealth","Clear",false,42}}; },
            [](auto){},[](const auto&){return synth::runtime::RuntimeActionResult{};}};
        const auto now=core::SnapshotClock::now();
        CHECK(!runtime.capture_actor_snapshot(now,9999)); CHECK(captures==0);
        runtime.set_requested_actor_pump([&](std::uint32_t id,const std::string& playthrough)->std::optional<core::ActorSnapshot> {
            ++calls; CHECK(playthrough=="save-a");
            if (mode==1) return {};
            if (mode==4) (void)runtime.invalidate();
            if (mode==5) (void)runtime.capture_snapshot(now);
            return core::ActorSnapshot{mode==2 ? 9998U : id,"Requested",{99,0,0},
                mode==6 ? "Other.esm" : "Actors.esm", mode==3 ? "other-save" : playthrough};
        });
        CHECK(!runtime.capture_actor_snapshot(now,0)); CHECK(!runtime.capture_actor_snapshot(now,0x14));
        CHECK(captures==0 && calls==0);
        const auto targeted=runtime.capture_actor_snapshot(now,9999);
        CHECK(targeted && calls==1 && captures==1); CHECK(targeted->actors().size()==64);
        CHECK(targeted->actors().front().form_id()==9999); CHECK(targeted->actors()[1].form_id()==100);
        CHECK(targeted->actors().back().form_id()==162); CHECK(targeted->captured_at()==now);
        CHECK(targeted->world()->location=="Sanctuary" && targeted->game_time_ticks()==42);
        CHECK(targeted->variant()==descriptor->variant);
        if (descriptor->variant==RuntimeVariant::vr) CHECK(targeted->hmd_pose()->frame()==targeted->frame());
        const auto ordinary=runtime.capture_snapshot(now);
        CHECK(ordinary->actors().size()==64 && ordinary->actors().front().form_id()==100 && ordinary->actors().back().form_id()==163);
        const auto present=runtime.capture_actor_snapshot(now,163);
        CHECK(present->actors().size()==64 && present->actors().front().form_id()==163 && present->actors().back().form_id()==162);
        CHECK(targeted->actors().front().form_id()==9999); // Later captures cannot rewrite an admitted turn.
        runtime.set_actor_detail_pump([](const core::ActorSnapshot& actor){return std::optional{actor};});
        CHECK(runtime.enrich_actor_snapshot(present,163));
        bool worker_rejected{};
        std::thread worker{[&]{try{(void)runtime.capture_actor_snapshot(now,9999);}catch(const std::logic_error&){worker_rejected=true;}}};
        worker.join(); CHECK(worker_rejected);
        mode=1; CHECK(!runtime.capture_actor_snapshot(now,9999));
        mode=2; CHECK(!runtime.capture_actor_snapshot(now,9999));
        for (const auto invalid : {3,6}) {
            mode=invalid; bool rejected{};
            try {(void)runtime.capture_actor_snapshot(now,100);} catch(const std::invalid_argument&) {rejected=true;}
            CHECK(rejected);
        }
        mode=4; CHECK(!runtime.capture_actor_snapshot(now,9999));
        mode=5; CHECK(!runtime.capture_actor_snapshot(now,9999));
    }
}

// Model coincident text/voice/maintenance requests on one serialized pump in both lanes.
void test_scoped_scene_capture_reuse() {
    using namespace synth;
    using Purpose = runtime::RuntimeCapturePurpose;
    for (const auto* descriptor : {&adapters::flat_descriptor, &adapters::vr_descriptor}) {
        const core::WorldPose pose{{},core::UnitVector3::from({1,0,0}),core::UnitVector3::from({0,0,1})};
        int captures{}, detail_calls{};
        bool fail_capture{}, fail_action{};
        std::function<void()> during_capture;
        std::vector<Purpose> purposes;
        adapters::detail::RuntimeBase runtime{*descriptor,
            [&](auto, Purpose purpose) {
                ++captures; purposes.push_back(purpose);
                if (fail_capture) throw std::runtime_error{"capture failed"};
                if (during_capture) during_capture();
                auto values = adapters::detail::CapturedRuntimeValues{pose,
                    descriptor->variant==RuntimeVariant::vr ? std::optional{pose} : std::nullopt,{},{},
                    core::ActorSnapshot{0x14,"Player",{},"Fallout4.esm","save-a"},
                    {core::ActorSnapshot{100,"Near",{1,0,0},"Actors.esm","save-a"}},42,
                    core::WorldState{"Sanctuary","Cell","Commonwealth","Clear",false,42}};
                if (descriptor->variant==RuntimeVariant::flat && purpose!=Purpose::bootstrap) {
                    values.picked_observed=true;
                    values.picked_actor=values.actors.front();
                }
                return values;
            }, [](auto){}, [&](const auto&) {
                if (fail_action) throw std::runtime_error{"action failed"};
                return runtime::RuntimeActionResult{};
            }};
        runtime.set_requested_actor_pump([](std::uint32_t id,const std::string& playthrough) {
            return std::optional{core::ActorSnapshot{id,"Requested",{99,0,0},"Actors.esm",playthrough}};
        });
        runtime.set_actor_detail_pump([&](const core::ActorSnapshot& actor) {
            ++detail_calls;
            return std::optional{core::ActorSnapshot{actor.form_id(),actor.name(),actor.position(),
                actor.origin_plugin(),actor.playthrough_id(),true,false,false,false,false,1,
                "Human","female","Voice",false,100,100,
                {{0x42,"Fallout4.esm","Selected inventory",static_cast<std::uint32_t>(detail_calls),1,0.5,41,false}},
                std::nullopt,{},"unavailable","alive","normal",false,0,false,false,false,std::nullopt,0,"","complete"}};
        });
        const auto now=core::SnapshotClock::now();
        std::shared_ptr<const core::RuntimeSnapshot> first;
        {
            const auto batch=runtime.capture_batch();
            first=runtime.capture_snapshot(now);
            for (int input=0; input<6; ++input) CHECK(runtime.capture_snapshot(now)==first);
            CHECK(runtime.capture_snapshot(now+std::chrono::milliseconds{100},Purpose::background)==first);
            CHECK(captures==1 && first->captured_at()==now);
            CHECK(first->picked_actor_form_id()==(descriptor->variant==RuntimeVariant::flat
                ? std::optional<std::uint32_t>{100} : std::nullopt));
            if (first->hmd_pose()) CHECK(!first->hmd_pose()->fresh_at(now+std::chrono::milliseconds{100}));
            const auto a=runtime.capture_actor_snapshot(now,9998);
            const auto b=runtime.capture_actor_snapshot(now,9999);
            CHECK(captures==1 && a->frame()==first->frame() && b->frame()==first->frame());
            CHECK(a->actors().front().form_id()==9998 && b->actors().front().form_id()==9999);
            CHECK(a->picked_actor_form_id()==first->picked_actor_form_id());
            CHECK(b->picked_actor_form_id()==first->picked_actor_form_id());
            CHECK(first->actors().size()==1 && first->actors().front().form_id()==100);
            const auto detailed_a=runtime.enrich_actor_snapshot(a,9998);
            const auto detailed_b=runtime.enrich_actor_snapshot(b,9999);
            CHECK(detailed_a && detailed_a->actors().front().inventory().front().count==1);
            CHECK(detailed_b && detailed_b->actors().front().inventory().front().count==2);
            CHECK(detailed_a->picked_actor_form_id()==first->picked_actor_form_id());
            CHECK(detail_calls==2 && runtime.capture_snapshot(now)==first);
            CHECK(a->actors().front().inventory().empty() && b->actors().front().inventory().empty());
            CHECK(first->actors().front().inventory().empty());
            CHECK(!runtime.enrich_actor_snapshot(first,9998));
            bool nested_rejected{}, worker_rejected{};
            try { const auto nested=runtime.capture_batch(); } catch(const std::logic_error&) {nested_rejected=true;}
            std::thread worker{[&] {
                try {(void)runtime.capture_snapshot(now);} catch(const std::logic_error&) {worker_rejected=true;}
            }};
            worker.join(); CHECK(nested_rejected && worker_rejected && captures==1);
        }
        {
            const auto batch=runtime.capture_batch();
            auto next=runtime.capture_snapshot(now);
            CHECK(captures==2 && next!=first && next->frame()>first->frame());
            CHECK(!runtime.enrich_actor_snapshot(first,100));
            (void)runtime.invalidate();
            auto loaded=runtime.capture_snapshot(now);
            CHECK(captures==3 && loaded->generation()!=next->generation());
            CHECK(runtime.capture_snapshot(now)==loaded);
            synth::runtime::RuntimeActionRequest cache_action{}; cache_action.actor.form_id = 100;
            (void)runtime.execute_action(cache_action);
            CHECK(runtime.capture_snapshot(now)!=loaded && captures==4);
            fail_action=true;
            try {(void)runtime.execute_action(cache_action);} catch(const std::runtime_error&) {}
            CHECK(runtime.capture_snapshot(now)!=loaded && captures==5);
        }
        // Background's cached environmental domains must not satisfy a dialogue refresh.
        {
            const auto batch=runtime.capture_batch();
            auto background=runtime.capture_snapshot(now,Purpose::background);
            CHECK(runtime.capture_snapshot(now,Purpose::background)==background);
            auto dialogue=runtime.capture_snapshot(now,Purpose::dialogue);
            CHECK(dialogue!=background && captures==7 && purposes.back()==Purpose::dialogue);
            CHECK(runtime.capture_snapshot(now,Purpose::background)==dialogue);
            auto bootstrap=runtime.capture_snapshot(now,Purpose::bootstrap);
            CHECK(!bootstrap->picked_actor_form_id());
            CHECK(runtime.capture_snapshot(now,Purpose::bootstrap)!=bootstrap && captures==9);
            CHECK(runtime.capture_snapshot(now)!=dialogue && captures==10);
        }
        // No cache survives a throwing scope or a failed source capture.
        try {
            const auto batch=runtime.capture_batch();
            (void)runtime.capture_snapshot(now);
            fail_capture=true;
            (void)runtime.capture_snapshot(now,Purpose::bootstrap);
        } catch(const std::runtime_error&) {}
        fail_capture=false;
        const auto unscoped=runtime.capture_snapshot(now);
        CHECK(runtime.capture_snapshot(now)!=unscoped && captures==14);
        CHECK(first->generation()!=runtime.generation() && first->actors().size()==1);
        // A source read interrupted by load invalidation cannot be relabelled as a new generation.
        {
            const auto batch=runtime.capture_batch();
            const auto before=runtime.generation();
            during_capture=[&] {(void)runtime.invalidate();};
            bool refused=false;
            try {(void)runtime.capture_snapshot(now);} catch(const std::runtime_error&) {refused=true;}
            CHECK(refused && runtime.generation()!=before);
            during_capture={};
            const auto after=runtime.capture_snapshot(now);
            CHECK(after->generation()==runtime.generation() && after->frame()==1);
            CHECK(runtime.capture_snapshot(now)==after);
        }
        // Actor evidence cannot reuse an earlier dialogue frame or populate the cache used by ordinary dialogue.
        {
            const auto batch=runtime.capture_batch();
            const auto prior=runtime.capture_snapshot(now);
            const auto count=captures;
            if (descriptor->variant==RuntimeVariant::flat) {
                const auto observed_at=now+std::chrono::milliseconds{1};
                const auto actor_scene=runtime.capture_snapshot(observed_at,Purpose::actor_events);
                CHECK(actor_scene!=prior && actor_scene->captured_at()==observed_at && actor_scene->frame()>prior->frame());
                CHECK(captures==count+1 && purposes.back()==Purpose::actor_events);
                CHECK(runtime.capture_snapshot(observed_at,Purpose::background)!=actor_scene && captures==count+2);
                const auto subset=actor_scene->with_event_actors({core::ActorSnapshot{999,"Corpse",{},"Actors.esm","save-a",false}});
                CHECK(subset.actors().size()==1 && subset.actors()[0].form_id()==999 && !subset.actors()[0].alive());
                CHECK(subset.frame()==actor_scene->frame() && subset.captured_at()==actor_scene->captured_at());
                CHECK(subset.actors_observation()=="partial" && !subset.picked_actor_form_id());
                CHECK(actor_scene->actors().size()==1 && actor_scene->actors()[0].form_id()==100);
                for (auto invalid:std::vector<std::vector<core::ActorSnapshot>>{
                    {subset.player()},{core::ActorSnapshot{999,"Other",{},"Actors.esm","other-save"}},
                    {subset.actors()[0],subset.actors()[0]}}) {
                    bool refused=false;try{(void)actor_scene->with_event_actors(std::move(invalid));}catch(const std::invalid_argument&){refused=true;}
                    CHECK(refused);
                }
            } else {
                bool refused=false;try{(void)runtime.capture_snapshot(now,Purpose::actor_events);}catch(const std::invalid_argument&){refused=true;}
                CHECK(refused && captures==count && runtime.capture_snapshot(now)==prior);
            }
            const auto before_inventory = captures;
            if (descriptor->variant == RuntimeVariant::flat) {
                const auto inventory = runtime.capture_snapshot(now, Purpose::player_inventory);
                CHECK(captures == before_inventory + 1 && purposes.back() == Purpose::player_inventory);
                CHECK(inventory != prior);
                CHECK(runtime.capture_snapshot(now, Purpose::player_inventory) != inventory && captures == before_inventory + 2);
                CHECK(runtime.capture_snapshot(now, Purpose::background) != inventory && captures == before_inventory + 3);
            } else {
                bool refused = false;
                try { (void)runtime.capture_snapshot(now, Purpose::player_inventory); }
                catch (const std::invalid_argument&) { refused = true; }
                CHECK(refused && captures == before_inventory);
            }
        }
    }
}

void test_bounded_discovery_windows() {
    using synth::core::ActorDiscoveryScan;
    ActorDiscoveryScan scan;
    for (const auto slice : scan.next({0,0,0,0})) CHECK(slice.begin == 0 && slice.count == 0);
    const std::array<std::size_t,4> small{1,3,7,245};
    auto slices = scan.next(small);
    for (std::size_t tier = 0; tier < 4; ++tier) CHECK(slices[tier].count == small[tier]);
    // Exhausted/empty tiers donate their budget; no low tier is starved.
    slices = scan.next({0,0,0,1000});
    CHECK(slices[3].count == 256 && slices[0].count == 0);
    scan = {};
    std::array<std::array<bool,257>,4> seen{};
    for (int capture = 0; capture < 5; ++capture) {
        slices = scan.next({257,257,257,257});
        for (std::size_t tier = 0; tier < 4; ++tier) {
            CHECK(slices[tier].count == 64 && slices[tier].begin < 257);
            for (std::size_t i = 0; i < slices[tier].count; ++i)
                seen[tier][(slices[tier].begin+i)%257] = true;
        }
    }
    for (const auto& tier : seen) CHECK(std::ranges::all_of(tier, [](bool read){return read;}));
    // Mutable list sizes and maximal sizes must preserve the bounded numeric windows.
    for (const auto sizes : {std::array<std::size_t,4>{2,0,1,3}, {0,9000,5,1},
                            {std::numeric_limits<std::size_t>::max(),0,0,0}, {1,2,3,4}}) {
        slices = scan.next(sizes);
        std::size_t reads{};
        for (std::size_t tier = 0; tier < 4; ++tier) {
            CHECK(slices[tier].count <= sizes[tier]);
            CHECK(sizes[tier] ? slices[tier].begin < sizes[tier] : slices[tier].begin == 0);
            reads += slices[tier].count;
        }
        CHECK(reads <= ActorDiscoveryScan::budget);
    }
    scan = {};
    for (const auto slice : scan.next({1000,1000,1000,1000})) CHECK(slice.begin == 0);
}

// Reservations cover paired mutation, reentrant callbacks and late releases without retaining engine objects.
void test_action_actor_reservations() {
    using namespace synth;
    for (const auto* descriptor : {&adapters::flat_descriptor, &adapters::vr_descriptor}) {
        adapters::detail::RuntimeBase* active{};
        bool reenter{}, fail{}, reload{};
        int calls{};
        std::optional<std::uint64_t> replacement;
        const std::array<std::uint32_t, 1> one{100};
        adapters::detail::RuntimeBase adapter{*descriptor,
            [](core::SnapshotClock::time_point, runtime::RuntimeCapturePurpose) -> adapters::detail::CapturedRuntimeValues {
                throw std::logic_error{"capture not used"};
            }, [](std::string_view) {}, [&](const runtime::RuntimeActionRequest& request) {
                ++calls;
                if (reenter) CHECK(active->execute_action(request).status == runtime::RuntimeActionStatus::rejected);
                if (fail) throw std::runtime_error{"native failure"};
                if (reload) replacement = active->reserve_action_actors(one, active->invalidate());
                return runtime::RuntimeActionResult{runtime::RuntimeActionStatus::succeeded, "observed"};
            }};
        active = &adapter;
        auto generation = adapter.generation();
        const std::array<std::uint32_t, 2> pair{100, 200}, overlap{300, 200}, duplicate{100, 100};
        const auto held = adapter.reserve_action_actors(pair, generation);
        CHECK(held && !adapter.reserve_action_actors(one, generation));
        CHECK(!adapter.reserve_action_actors(overlap, generation));
        const std::array<std::uint32_t, 1> independent{300}, invalid{0};
        const auto unrelated = adapter.reserve_action_actors(independent, generation);
        CHECK(unrelated && adapter.release_action_actors(*unrelated, generation));
        CHECK(!adapter.reserve_action_actors(duplicate, generation));
        CHECK(!adapter.reserve_action_actors(invalid, generation));
        CHECK(!adapter.reserve_action_actors({}, generation));
        runtime::RuntimeActionRequest request{}; request.actor.form_id = 100;
        CHECK(adapter.execute_action(request).status == runtime::RuntimeActionStatus::rejected && calls == 0);
        CHECK(adapter.release_action_actors(*held, generation));
        CHECK(!adapter.release_action_actors(*held, generation));
        reenter = true;
        CHECK(adapter.execute_action(request).status == runtime::RuntimeActionStatus::succeeded && calls == 1);
        reenter = false; fail = true;
        bool threw{}; try {(void)adapter.execute_action(request);} catch(const std::runtime_error&) {threw = true;}
        CHECK(threw); fail = false;
        CHECK(adapter.execute_action(request).status == runtime::RuntimeActionStatus::succeeded);
        // Both actors are reserved during a paired action, with all-or-none acquisition.
        const auto recipient_hold = adapter.reserve_action_actors(std::span{pair}.subspan(1), generation);
        request.recipient = runtime::RuntimeActorIdentity{200, "Fallout4.esm", "save-a"};
        const auto prior_calls = calls;
        CHECK(adapter.execute_action(request).status == runtime::RuntimeActionStatus::rejected && calls == prior_calls);
        const auto donor_hold = adapter.reserve_action_actors(one, generation);
        CHECK(donor_hold && adapter.release_action_actors(*donor_hold, generation));
        CHECK(adapter.release_action_actors(*recipient_hold, generation)); request.recipient.reset();
        reload = true;
        CHECK(adapter.execute_action(request).status == runtime::RuntimeActionStatus::succeeded && replacement);
        CHECK(!adapter.release_action_actors(*replacement, generation));
        CHECK(!adapter.reserve_action_actors(one, adapter.generation()));
        CHECK(adapter.release_action_actors(*replacement, adapter.generation()));
        CHECK(!adapter.reserve_action_actors(one, generation)); generation = adapter.generation();
        bool worker_rejected{};
        std::thread worker{[&] { try {(void)adapter.reserve_action_actors(one, generation);}
            catch(const std::logic_error&) {worker_rejected = true;} }};
        worker.join(); CHECK(worker_rejected);
        std::vector<std::uint64_t> reservations;
        for (std::uint32_t id = 1000; id < 1064; ++id) {
            const std::array actor{id}; const auto slot = adapter.reserve_action_actors(actor, generation);
            CHECK(slot.has_value()); reservations.push_back(*slot);
        }
        CHECK(!adapter.reserve_action_actors(one, generation));
        for (const auto id : reservations) CHECK(adapter.release_action_actors(id, generation));
        const auto recovered = adapter.reserve_action_actors(one, generation);
        CHECK(recovered && adapter.release_action_actors(*recovered, generation));
        // Optional speech rotation shares speaker ownership, without unnecessarily reserving its listener.
        core::CancellationSource facing_owner{generation};
        runtime::RuntimeFacingRequest facing{generation.value(), 1, "request", "turn", "utterance",
            {100, "Fallout4.esm", "save-a"}, {200, "Fallout4.esm", "save-a"}, facing_owner.token()};
        int facing_calls{};
        bool fail_facing{};
        adapter.set_facing_pump([&](const auto& same_facing) {
            ++facing_calls;
            CHECK(adapter.execute_action(request).status == runtime::RuntimeActionStatus::rejected);
            CHECK(adapter.face_speech_listener(same_facing).status == runtime::RuntimeActionStatus::rejected);
            if (fail_facing) throw std::runtime_error{"facing failure"};
            return runtime::RuntimeActionResult{runtime::RuntimeActionStatus::succeeded, "rotated"};
        });
        const auto occupied_speaker = adapter.reserve_action_actors(one, generation);
        CHECK(adapter.face_speech_listener(facing).status == runtime::RuntimeActionStatus::rejected && facing_calls == 0);
        CHECK(adapter.release_action_actors(*occupied_speaker, generation));
        const auto occupied_listener = adapter.reserve_action_actors(std::span{pair}.subspan(1), generation);
        CHECK(adapter.face_speech_listener(facing).status == runtime::RuntimeActionStatus::succeeded && facing_calls == 1);
        CHECK(adapter.release_action_actors(*occupied_listener, generation));
        fail_facing = true; threw = false;
        try {(void)adapter.face_speech_listener(facing);} catch(const std::runtime_error&) {threw = true;}
        CHECK(threw && facing_calls == 2);
        const auto released_speaker = adapter.reserve_action_actors(one, generation);
        CHECK(released_speaker && adapter.release_action_actors(*released_speaker, generation));
        facing_owner.cancel();
        CHECK(adapter.face_speech_listener(facing).status == runtime::RuntimeActionStatus::unavailable && facing_calls == 2);
    }
}

}  // namespace

int main() {
    try {
        test_descriptors_and_capabilities();
        test_exact_version_and_wrong_lane_rejection();
        test_lifecycle_emits_runtime_actions();
        test_deferred_lifecycle_actions_coalesce_load_boundaries();
        test_product_owned_pump_seams_and_invalidation();
        test_selected_actor_enrichment_ownership();
        test_request_player_inventory_ownership();
        test_requested_actor_capture_outside_discovery();
        test_scoped_scene_capture_reuse();
        test_bounded_discovery_windows();
        test_action_actor_reservations();
        std::cout << "adapter tests passed (" << assertions << " assertions)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
