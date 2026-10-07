#include "core/cancellation.hpp"
#include "core/priority_event_queue.hpp"
#include "core/runtime_identity.hpp"
#include "core/snapshot.hpp"
#include "runtime/fake_fallout_runtime.hpp"
#include "runtime/game_thread_dispatcher.hpp"
#include "voice/voice_sample_resolver.hpp"

#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using synth::core::EventPriority;
using synth::core::QueuePushResult;
using synth::core::RuntimeGeneration;
using synth::core::RuntimeVariant;
using synth::core::SnapshotClock;
using synth::core::UnitVector3;
using synth::core::Vec3;
using synth::core::WorldPose;

int assertions = 0;

#define CHECK(condition)                                                                        \
    do {                                                                                        \
        ++assertions;                                                                           \
        if (!(condition)) {                                                                     \
            throw std::runtime_error{std::string{"CHECK failed: "} + #condition + " at " +     \
                                     __FILE__ + ":" + std::to_string(__LINE__)};                \
        }                                                                                       \
    } while (false)

template <class Exception, class Function>
void check_throws(Function&& function) {
    ++assertions;
    try {
        std::forward<Function>(function)();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error{"expected exception was not thrown"};
}

WorldPose pose(Vec3 position = {}) {
    return WorldPose{position,
                     UnitVector3::from({0.0, 2.0, 0.0}),
                     UnitVector3::from({0.0, 0.0, 3.0})};
}

void test_runtime_identity() {
    using namespace synth::core;
    CHECK(to_string(Game::fallout4) == "fo4");
    CHECK(to_string(RuntimeVariant::flat) == "flat");
    CHECK(to_string(RuntimeVariant::vr) == "vr");
    CHECK(parse_game("fo4") == Game::fallout4);
    CHECK(!parse_game("fallout4"));
    CHECK(parse_runtime_variant("flat") == RuntimeVariant::flat);
    CHECK(parse_runtime_variant("vr") == RuntimeVariant::vr);
    CHECK(!parse_runtime_variant("VR"));
}

void test_generation_and_cancellation() {
    using namespace synth::core;
    GenerationClock clock;
    const auto first = clock.current();
    CHECK(first == RuntimeGeneration::initial());
    CHECK(clock.is_current(first));
    check_throws<std::invalid_argument>([] { (void)RuntimeGeneration::from_value(0); });

    CancellationSource source{first};
    auto token = source.token();
    CancellationSource turn_a{token};
    CancellationSource turn_b{token};
    CHECK(turn_a.token().generation() == first);
    CancellationSource child{turn_a.token()};
    CHECK(!turn_a.cancel_if_owner(turn_b.token()));
    CHECK(!turn_a.cancel_if_owner(token));
    CHECK(!turn_a.cancel_if_owner(child.token()));
    CHECK(!turn_a.cancel_if_owner(CancellationToken{}));
    CHECK(!turn_a.is_cancelled());
    CHECK(turn_a.cancel_if_owner(turn_a.token()));
    CHECK(child.is_cancelled());
    CHECK(!turn_b.cancel_if_owner(turn_a.token()));
    CHECK(turn_a.token().is_cancelled());
    CHECK(!turn_b.is_cancelled());
    CHECK(!source.is_cancelled());
    check_throws<std::invalid_argument>([] { (void)CancellationSource{CancellationToken{}}; });
    CHECK(!token.is_cancelled());
    CHECK(token.is_current(clock));

    const auto second = clock.advance();
    CHECK(second.value() == first.value() + 1);
    CHECK(!token.is_current(clock));
    source.cancel();
    CHECK(source.is_cancelled());
    CHECK(token.is_cancelled());
    CHECK(turn_b.is_cancelled());
    CHECK(synth::core::CancellationToken{}.is_cancelled());
}

void test_snapshot_values_and_immutability() {
    using namespace synth::core;
    check_throws<std::invalid_argument>([] { (void)UnitVector3::from({0.0, 0.0, 0.0}); });
    check_throws<std::invalid_argument>([] {
        (void)UnitVector3::from({std::numeric_limits<double>::infinity(), 0.0, 0.0});
    });
    const auto unit = UnitVector3::from({0.0, 3.0, 4.0});
    CHECK(std::abs(unit.value().y - 0.6) < 1e-12);
    CHECK(std::abs(unit.value().z - 0.8) < 1e-12);
    check_throws<std::invalid_argument>([] { (void)synth::core::ActorSnapshot{0, "npc", {}}; });
    check_throws<std::invalid_argument>([] { (void)synth::core::ActorSnapshot{1, "", {}}; });
    CHECK((ActorSnapshot{1,"Unobserved",{}}.inventory_observation() == "unavailable"));
    const auto inventory_actor = [](std::size_t count) {
        return ActorSnapshot{1,"Inventory",{},"Fallout4.esm","save-a",true,false,false,false,false,
            1,"","","",false,100,100,
            std::vector<InventoryItemSnapshot>(count,{0x4822,"Fallout4.esm","Pistol",1,50,4.2,43,false}),
            std::nullopt,{},"unavailable","unknown","unknown",false,0,false,false,false,std::nullopt,0,"","complete"};
    };
    CHECK(inventory_actor(512).inventory().size()==512);
    check_throws<std::invalid_argument>([&] { (void)inventory_actor(513); });
    CHECK((ActorSnapshot{1,"Unobserved",{}}.faction_completeness() == "unavailable"));
    CHECK((!ActorSnapshot{1,"Unobserved",{}}.health_percent_available()));
    CHECK((!ActorSnapshot{1,"Unobserved",{}}.action_points_percent_available()));
    const ActorSnapshot measured{1,"Measured",{},"Fallout4.esm","save-a",true,false,false,false,false,
        1,"","","",false,0,40,{},std::nullopt,{},"unavailable","unknown","unknown",false,0,
        false,false,false,std::nullopt,0,"","unavailable","unavailable",true,false};
    CHECK(measured.health_percent()==0 && measured.health_percent_available());
    CHECK(measured.action_points_percent()==40 && !measured.action_points_percent_available());
    const FactionMembershipSnapshot valid_faction{
        0x5DE41, "Fallout4.esm", "MinutemenFaction", "MinutemenFaction", 0};
    const FactionMembershipSnapshot invalid_faction{
        0x5DE41, "../unsafe.esm", "Unsafe", "", 0};
    CHECK(valid_faction.valid());
    CHECK(!invalid_faction.valid());
    const auto faction_actor = [&](const std::string& mode, const std::string& quality,
                                   std::vector<FactionMembershipSnapshot> factions) {
        return ActorSnapshot{1,"NPC",{},"Fallout4.esm","save-a",true,false,false,false,false,
            1,"","","",false,100,100,{},std::nullopt,std::move(factions),mode,
            "unknown","unknown",false,0,false,false,false,std::nullopt,0,"","unavailable",quality};
    };
    for (const auto mode : {"effective","base_only"}) {
        CHECK(faction_actor(mode,"complete",{}).faction_completeness()=="complete");
        CHECK(faction_actor(mode,"partial",{valid_faction}).faction_completeness()=="partial");
        CHECK(faction_actor(mode,"",{}).faction_completeness()=="partial");
        check_throws<std::invalid_argument>([&]{(void)faction_actor(mode,"unavailable",{});});
    }
    CHECK(faction_actor("unavailable","unavailable",{}).faction_completeness()=="unavailable");
    check_throws<std::invalid_argument>([&]{(void)faction_actor("unavailable","complete",{});});
    check_throws<std::invalid_argument>([&]{(void)faction_actor("unavailable","unavailable",{valid_faction});});
    check_throws<std::invalid_argument>([&]{(void)faction_actor("effective","invented",{});});
    const PackageSnapshot valid_package{
        0xA2420, "Fallout4.esm", "SandboxPackage", "SandboxPackage"};
    CHECK(valid_package.valid());

    std::vector<ActorSnapshot> actors;
    actors.emplace_back(0x1234, "Piper", Vec3{1.0, 2.0, 3.0});
    const auto now = SnapshotClock::now();
    RuntimeSnapshot snapshot{Game::fallout4,
                             RuntimeVariant::flat,
                             RuntimeGeneration::initial(),
                             9,
                             now,
                             pose(),
                             std::nullopt,
                             std::nullopt,
                             std::nullopt,
                             ActorSnapshot{0x14, "Sole Survivor", {}, "Fallout4.esm", "unknown"},
                             std::move(actors)};
    CHECK(snapshot.actors().size() == 1);
    CHECK(snapshot.actors_observation() == "partial");
    for (const auto quality : {"complete", "partial", "unavailable"}) {
        const RuntimeSnapshot observed{Game::fallout4, RuntimeVariant::flat, RuntimeGeneration::initial(),
            1, now, pose(), std::nullopt, std::nullopt, std::nullopt,
            ActorSnapshot{0x14,"Player",{}}, {}, 0, std::nullopt, {}, {}, {}, {}, {}, {}, {}, quality};
        CHECK(observed.actors_observation() == quality);
        CHECK(observed.with_requested_actor(snapshot.actors().front()).actors_observation() == "partial");
        CHECK(observed.actors().empty());
    }
    for (const auto quality : {"invented", "cached", "unavailable"}) {
        check_throws<std::invalid_argument>([&] {
            (void)RuntimeSnapshot{Game::fallout4, RuntimeVariant::flat, RuntimeGeneration::initial(),
                1, now, pose(), std::nullopt, std::nullopt, std::nullopt,
                snapshot.player(), snapshot.actors(), 0, std::nullopt, {}, {}, {}, {}, {}, {}, {}, quality};
        });
    }
    CHECK(snapshot.nearby_items_observation() == "unavailable");
    CHECK(snapshot.points_of_interest_observation() == "unavailable");
    for (const auto quality : {"complete", "partial", "cached", "unavailable"}) {
        const RuntimeSnapshot observed{Game::fallout4, RuntimeVariant::flat, RuntimeGeneration::initial(),
            1, now, pose(), std::nullopt, std::nullopt, std::nullopt,
            ActorSnapshot{0x14,"Player",{}}, {}, 0, std::nullopt, {}, {}, {}, {}, {}, quality, quality};
        CHECK(observed.nearby_items_observation() == quality);
        CHECK(observed.points_of_interest_observation() == quality);
    }
    check_throws<std::invalid_argument>([&] {
        (void)RuntimeSnapshot{Game::fallout4, RuntimeVariant::flat, RuntimeGeneration::initial(),
            1, now, pose(), std::nullopt, std::nullopt, std::nullopt,
            ActorSnapshot{0x14,"Player",{}}, {}, 0, std::nullopt, {}, {}, {}, {}, {}, "invented", "complete"};
    });
    CHECK(snapshot.actors()[0].name() == "Piper");
    CHECK(snapshot.actors()[0].origin_plugin() == "Fallout4.esm");
    CHECK(snapshot.actors()[0].playthrough_id() == "unknown");
    CHECK(snapshot.effective_listener_pose(now) == &snapshot.player_pose());
    CHECK(snapshot.hmd_pose_capability(now) == PoseCapability::unavailable);

    static_assert(std::is_same_v<decltype(std::declval<const RuntimeSnapshot&>().actors()),
                                 const std::vector<ActorSnapshot>&>);
    check_throws<std::invalid_argument>([&] {
        (void)RuntimeSnapshot{Game::fallout4,
                              RuntimeVariant::flat,
                              RuntimeGeneration::initial(),
                              1,
                              now,
                              pose(),
                              TimedPose{pose(), now, 5ms, 1},
                              std::nullopt,
                              std::nullopt,
                              ActorSnapshot{0x14, "Sole Survivor", {}, "Fallout4.esm", "unknown"},
                              {}};
    });
}

void test_priority_queue_reservations_and_halt() {
    synth::core::BoundedPriorityEventQueue<std::string> queue{5, 2, 1};
    CHECK(queue.try_push("background", EventPriority::background) == QueuePushResult::accepted);
    CHECK(queue.try_push("normal", EventPriority::normal) == QueuePushResult::accepted);
    CHECK(queue.try_push("background-2", EventPriority::background) == QueuePushResult::saturated);
    CHECK(queue.try_push("player-1", EventPriority::player) == QueuePushResult::accepted);
    CHECK(queue.try_push("player-2", EventPriority::player) == QueuePushResult::accepted);
    CHECK(queue.try_push("player-3", EventPriority::player) == QueuePushResult::saturated);

    CHECK(queue.try_pop() == "player-1");
    CHECK(queue.try_pop() == "player-2");
    CHECK(queue.try_pop() == "normal");
    CHECK(queue.try_pop() == "background");
    CHECK(!queue.try_pop());

    CHECK(queue.try_push("old", EventPriority::normal) == QueuePushResult::accepted);
    CHECK(queue.try_push("stop", EventPriority::halt) == QueuePushResult::accepted);
    const auto halted_health = queue.health();
    CHECK(halted_health.halted);
    CHECK(halted_health.discarded_on_halt == 1);
    CHECK(halted_health.size == 1);
    CHECK(queue.try_push("late", EventPriority::player) == QueuePushResult::halted);
    CHECK(queue.try_pop() == "stop");
    CHECK(!queue.try_pop());
    CHECK(queue.health().rejected_after_halt == 1);

    check_throws<std::invalid_argument>([] {
        synth::core::BoundedPriorityEventQueue<int> invalid{2, 2, 1};
    });
}

void test_priority_queue_thread_safety() {
    synth::core::BoundedPriorityEventQueue<int> queue{102, 1, 1};
    std::vector<std::thread> producers;
    for (int producer = 0; producer < 4; ++producer) {
        producers.emplace_back([&queue, producer] {
            for (int item = 0; item < 25; ++item) {
                const auto result = queue.try_push(producer * 25 + item, EventPriority::normal);
                if (result != QueuePushResult::accepted) {
                    throw std::runtime_error{"unexpected queue saturation"};
                }
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    CHECK(queue.health().size == 100);
    std::size_t popped{};
    while (queue.try_pop()) {
        ++popped;
    }
    CHECK(popped == 100);
}

void test_fake_runtime_flat_and_vr_pose_freshness() {
    using namespace synth::core;
    using synth::runtime::FakeFalloutRuntime;

    const auto now = SnapshotClock::now();
    FakeFalloutRuntime flat{RuntimeVariant::flat};
    flat.set_player_pose(pose({1.0, 2.0, 3.0}));
    flat.set_actors({ActorSnapshot{0x42, "Nick", {4.0, 5.0, 6.0}}});
    const auto flat_snapshot = flat.capture_snapshot(now);
    CHECK(flat_snapshot->game() == Game::fallout4);
    CHECK(flat_snapshot->variant() == RuntimeVariant::flat);
    CHECK(flat_snapshot->effective_listener_pose(now)->position() == (Vec3{1.0, 2.0, 3.0}));
    CHECK(flat_snapshot->hmd_pose_capability(now) == PoseCapability::unavailable);
    check_throws<std::logic_error>([&] { flat.set_hmd_pose(pose(), now, 10ms); });

    FakeFalloutRuntime vr{RuntimeVariant::vr};
    vr.set_player_pose(pose({100.0, 100.0, 100.0}));
    vr.set_hmd_pose(pose({7.0, 8.0, 9.0}), now, 50ms);
    const auto fresh = vr.capture_snapshot(now + 25ms);
    CHECK(fresh->hmd_pose_capability(now + 25ms) == PoseCapability::fresh);
    CHECK(fresh->effective_listener_pose(now + 25ms)->position() == (Vec3{7.0, 8.0, 9.0}));
    CHECK(fresh->hmd_pose()->frame() == 0);
    CHECK(fresh->hmd_pose_capability(now + 51ms) == PoseCapability::stale);
    CHECK(fresh->effective_listener_pose(now + 51ms) == nullptr);

    vr.clear_hmd_pose();
    const auto missing = vr.capture_snapshot(now + 60ms);
    CHECK(missing->hmd_pose_capability(now + 60ms) == PoseCapability::unavailable);
    CHECK(missing->effective_listener_pose(now + 60ms) == nullptr);
    CHECK(vr.capabilities().size() == 2);
}

void test_game_thread_dispatcher() {
    using synth::runtime::DispatchResult;
    using synth::runtime::FakeFalloutRuntime;
    using synth::runtime::GameThreadDispatcher;

    FakeFalloutRuntime runtime{RuntimeVariant::flat};
    GameThreadDispatcher dispatcher{runtime, 2};
    const auto generation = runtime.generation();

    std::thread producer{[&] {
        CHECK(dispatcher.try_enqueue(generation, [](synth::runtime::IFalloutRuntime& target) {
                  target.present_notification("worker command");
              }) == DispatchResult::accepted);
    }};
    producer.join();
    CHECK(dispatcher.drain() == 1);
    CHECK(runtime.notifications() == std::vector<std::string>{"worker command"});

    CHECK(dispatcher.try_enqueue(generation, [](synth::runtime::IFalloutRuntime& target) {
              target.present_notification("stale");
          }) == DispatchResult::accepted);
    const auto next_generation = runtime.advance_generation();
    CHECK(next_generation != generation);
    CHECK(dispatcher.drain() == 0);
    CHECK(dispatcher.health().stale == 1);
    CHECK(runtime.notifications().size() == 1);

    CHECK(dispatcher.try_enqueue(next_generation, [](auto&) {}) == DispatchResult::accepted);
    CHECK(dispatcher.try_enqueue(next_generation, [](auto&) {}) == DispatchResult::accepted);
    CHECK(dispatcher.try_enqueue(next_generation, [](auto&) {}) == DispatchResult::saturated);
    CHECK(dispatcher.health().saturated == 1);
    CHECK(dispatcher.drain() == 2);

    CHECK(dispatcher.try_enqueue(next_generation, [](auto&) {}) == DispatchResult::accepted);
    dispatcher.discard_pending();
    CHECK(dispatcher.health().pending == 0);
    CHECK(dispatcher.health().discarded == 1);

    bool runtime_asserted = false;
    bool dispatcher_asserted = false;
    std::thread wrong_thread{[&] {
        try {
            runtime.present_notification("wrong");
        } catch (const std::logic_error&) {
            runtime_asserted = true;
        }
        try {
            (void)dispatcher.drain();
        } catch (const std::logic_error&) {
            dispatcher_asserted = true;
        }
    }};
    wrong_thread.join();
    CHECK(runtime_asserted);
    CHECK(dispatcher_asserted);

    dispatcher.stop_accepting();
    CHECK(dispatcher.try_enqueue(next_generation, [](auto&) {}) == DispatchResult::stopped);
    CHECK(!dispatcher.health().accepting);
}

// Synthetic archive bytes exercise the parser without distributing any game audio.
void test_voice_archive_bounds_and_cancellation() {
    using namespace synth::voice;
    const auto root = std::filesystem::temp_directory_path() /
        ("synth-voice-unit-" + std::to_string(SnapshotClock::now().time_since_epoch().count()));
    CHECK(std::filesystem::create_directory(root));
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    } cleanup{root};
    const std::string name = "sound/voice/Fallout4.esm/NPCMPaladinDanse/test.fuz";
    const auto archive = root / "Fallout4 - Voices.ba2";
    const auto write_archive = [&](std::uint64_t offset, std::uint32_t version = 1) {
        std::ofstream output{archive, std::ios::binary | std::ios::trunc};
        const auto number = [&](std::uint64_t value, std::size_t size) {
            for (std::size_t i = 0; i < size; ++i) output.put(static_cast<char>((value >> (8 * i)) & 255));
        };
        output.write("BTDX", 4); number(version, 4); output.write("GNRL", 4);
        number(1, 4); number(60 + 80'000, 8);
        number(0, 8); number(0, 8); number(offset, 8);
        number(0, 4); number(80'000, 4); number(0, 4);
        output << std::string(80'000, 'x');
        number(name.size(), 2); output << name;
    };
    write_archive(60);
    SampleIndex index;
    index.scan(root, {"Fallout4.esm"}, [] { return false; });
    CHECK(index.samples().size() == 1);
    CHECK(SampleIndex::read(index.samples().at("npcmpaladindanse")).size() == 80'000);
    index.scan(root, {"Other.esp"}, [] { return false; });
    CHECK(index.samples().empty());
    bool cancelled{};
    try { index.scan(root, {"Fallout4.esm"}, [] { return true; }); }
    catch (const std::runtime_error&) { cancelled = true; }
    CHECK(cancelled);
    write_archive(std::numeric_limits<std::uint64_t>::max());
    index.scan(root, {"Fallout4.esm"}, [] { return false; });
    CHECK(index.samples().empty());
    write_archive(60, 99);
    index.scan(root, {"Fallout4.esm"}, [] { return false; });
    CHECK(index.unsupported_archives() == 1);
    CHECK(index.samples().empty());
    write_archive(60);
    const auto loose = root / "Sound/Voice/Fallout4.esm/NPCMPaladinDanse/override.fuz";
    std::filesystem::create_directories(loose.parent_path());
    { std::ofstream output{loose, std::ios::binary}; output << std::string(60'000, 'y'); }
    index.scan(root, {"Fallout4.esm"}, [] { return false; });
    CHECK(index.samples().at("npcmpaladindanse").loose);
    CHECK(SampleIndex::read(index.samples().at("npcmpaladindanse")).front() == 'y');
    CHECK(!safe_component("../outside"));
    CHECK(!safe_component(std::string(129, 'a')));
}

}  // namespace

int main() {
    try {
        test_runtime_identity();
        test_generation_and_cancellation();
        test_snapshot_values_and_immutability();
        test_priority_queue_reservations_and_halt();
        test_priority_queue_thread_safety();
        test_fake_runtime_flat_and_vr_pose_freshness();
        test_game_thread_dispatcher();
        test_voice_archive_bounds_and_cancellation();
        std::cout << "native tests passed (" << assertions << " assertions)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
