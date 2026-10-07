#include "audio/dialogue_audio.hpp"
#include "audio/speech_animation.hpp"
#include <limits>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace synth::audio;
#define CHECK(x) do { if (!(x)) throw std::runtime_error{#x}; } while(false)
int main() try {
    {
        const auto now = SpeechMergePermit::Clock::now();
        synth::core::CancellationSource owner{synth::core::RuntimeGeneration::initial()};
        SpeechMergePermit permit{42, owner.token()};
        CHECK(!permit.allows(42, now));
        permit.publish(now);
        bool worker_allowed{};
        std::thread worker{[&] { worker_allowed = permit.allows(42, now); }};
        worker.join(); CHECK(worker_allowed); // Native animation workers can merge, not write objects.
        CHECK(!permit.allows(43, now));
        CHECK(!permit.allows(42, now + std::chrono::milliseconds{250}));
        permit.revoke(); CHECK(!permit.allows(42, now));
        permit.publish(now); owner.cancel(); CHECK(!permit.allows(42, now));
    }
    {
        const SpeechAnimation lips{"A M F OO EE TH"};
        CHECK(lips.size() == 11);
        CHECK(lips.sample(0.1, 11.0, 1.0F)[0] > 0.9F);
        CHECK(lips.sample(2.1, 11.0, 1.0F)[2] > 0.9F); // closed lips, not jaw-open
        CHECK(lips.sample(4.1, 11.0, 1.0F)[3] > 0.9F);
        CHECK(lips.sample(6.1, 11.0, 1.0F)[5] > 0.9F);
        CHECK(lips.sample(8.1, 11.0, 1.0F)[1] > 0.9F);
        CHECK(lips.sample(10.1, 11.0, 1.0F)[6] > 0.9F);
        CHECK(lips.sample(1.1, 11.0, 1.0F) == MouthWeights{});
        CHECK(lips.sample(0.1, 11.0, 0.0F) == MouthWeights{}); // WAV silence overrides text
        CHECK(lips.sample(11.0, 11.0, 1.0F) == MouthWeights{});
        CHECK(lips.sample(0.1, 0.0, 1.0F) == MouthWeights{});
        CHECK(lips.sample(-1, 11.0, 1.0F) == MouthWeights{});
        CHECK(lips.sample(std::numeric_limits<double>::quiet_NaN(), 11.0, 1.0F) == MouthWeights{});
        CHECK(lips.sample(0.8,11.0,1.0F)[0] > 0 && lips.sample(0.8,11.0,1.0F)[0] < 1); // blend into rest
        CHECK(SpeechAnimation{""}.sample(0,1,1) == MouthWeights{});
        CHECK(SpeechAnimation{std::string(8193,'A')}.size() == 0);
        CHECK(speech_animation_interval(500) == std::chrono::microseconds{500});
    }
    auto generation = synth::core::RuntimeGeneration::initial();
    TtsQueue queue{generation, 3};
    CHECK(queue.enqueue({generation, 1, "npc_1", MediaId::same_origin("media_1")}) == EnqueueResult::accepted);
    CHECK(queue.enqueue({generation, 3, "npc_2", MediaId::same_origin("media_2")}) == EnqueueResult::accepted);
    CHECK(queue.enqueue({generation, 2, "npc_3", MediaId::same_origin("media_3")}) == EnqueueResult::invalid_sequence);
    CHECK(queue.start_next()->speaker_id == "npc_1"); CHECK(queue.finish_playing());
    CHECK(queue.cached(MediaId::same_origin("media_1")));
    auto next = synth::core::RuntimeGeneration::from_value(2); CHECK(queue.bind_generation(next) == 1);
    CHECK(queue.enqueue({generation, 4, "npc", MediaId::same_origin("old")}) == EnqueueResult::stale_generation);
    CHECK(queue.enqueue({next, 1, "npc", MediaId::same_origin("new")}) == EnqueueResult::accepted);
    auto halt = queue.hard_halt(); CHECK(halt.cancelled == 1 && halt.cache_cleared && halt.terminal);
    CHECK(queue.enqueue({next, 2, "npc", MediaId::same_origin("late")}) == EnqueueResult::terminal);
    bool rejected{}; try { (void)MediaId::same_origin("https://server/media"); } catch (const std::invalid_argument&) { rejected = true; } CHECK(rejected);

    SpatialScene scene;
    Pose flat{{0,0,0},{0,1,0},{0,0,1},1}; Pose stale_hmd{{10,0,0},{0,1,0},{0,0,1},4};
    CHECK(scene.update_listener(RuntimeView::flat, flat, std::nullopt, 1) == ListenerResult::accepted);
    CHECK(scene.set_source("npc", {3,4,0})); CHECK(std::abs(*scene.distance_to("npc") - 5.0) < 1e-9);
    CHECK(scene.update_listener(RuntimeView::vr, flat, stale_hmd, 5) == ListenerResult::stale_hmd);
    CHECK(!scene.has_listener()); CHECK(!scene.distance_to("npc"));
    stale_hmd.sample = 5; CHECK(scene.update_listener(RuntimeView::vr, flat, stale_hmd, 5) == ListenerResult::accepted);
    CHECK(std::abs(*scene.distance_to("npc") - std::sqrt(65.0)) < 1e-9);
    std::cout << "audio tests passed\n";
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
