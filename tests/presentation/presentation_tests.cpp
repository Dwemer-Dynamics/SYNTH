#include "presentation/dialogue_presentation.hpp"
#include "presentation/speech_facing.hpp"
#include "client/speech_facing_binding.hpp"
#include "client/conversation_holds.hpp"
#include "adapters/detail/runtime_base.hpp"
#include <iostream>
#include <limits>
#include <thread>
#include <stdexcept>
using namespace synth::presentation;
#define CHECK(x) do { if (!(x)) throw std::runtime_error{#x}; } while(false)

static void test_scene_admission_movement(const synth::adapters::AdapterDescriptor& descriptor) {
    using namespace synth;
    using Stage = diagnostics::ConversationStage;
    const core::WorldPose pose{{},core::UnitVector3::from({0,1,0}),core::UnitVector3::from({0,0,1})};
    bool interior = false, present = true, alive = true, disabled = false;
    std::uint32_t cell = 0x200, worldspace = 0x3C, base = 0x179FF;
    std::string cell_plugin = "Fallout4.esm", worldspace_plugin = "Fallout4.esm";
    std::string actor_plugin = "Fallout4.esm", player_save = "save:a", posture = "normal";
    core::Vec3 position{100,0,0};
    adapters::detail::RuntimeBase runtime{descriptor,[&](auto,auto) {
        std::vector<core::ActorSnapshot> actors;
        if (present) actors.emplace_back(0x101,"Codsworth",position,actor_plugin,"save:a",alive,disabled,
            false,false,false,std::int16_t{1},"RobotRace","male","RobotMrHandy",false,100.0,100.0,
            std::vector<core::InventoryItemSnapshot>{},std::nullopt,std::vector<core::FactionMembershipSnapshot>{},
            "unavailable","alive",posture,false,150.0,false,false,false,std::nullopt,base,"Fallout4.esm");
        const auto vr = descriptor.variant == core::RuntimeVariant::vr;
        return adapters::detail::CapturedRuntimeValues{pose,vr?std::optional{pose}:std::nullopt,{}, {},
            core::ActorSnapshot{0x14,"Player",{},"Fallout4.esm",player_save},std::move(actors),100,
            core::WorldState{"Commonwealth","Outdoors","Commonwealth","",interior,100,
                core::SceneIdentity{cell,worldspace,cell_plugin,worldspace_plugin}}};
    },[](auto){},[](auto){return runtime::RuntimeActionResult{};}};
    const auto scene = runtime.capture_snapshot(core::SnapshotClock::now());
    core::CancellationSource owner{scene->generation()};
    SceneAdmission admission;
    admission.original = scene;
    const auto& speaker = scene->actors().front();
    admission.actors.push_back({owner.token(),speaker.form_id(),speaker.base_form_id(),speaker.origin_plugin(),
        speaker.base_origin_plugin(),speaker.playthrough_id(),speaker.name(),true});
    const auto reason = [&] {
        const auto current = runtime.capture_snapshot(core::SnapshotClock::now());
        return admission.rejection_reason(current.get(),core::SnapshotClock::now());
    };
    CHECK(!reason()); // Moving speech is allowed without taking over vanilla AI packages.
    cell = 0x201; position = {300,0,0}; CHECK(!reason());
    cell_plugin = "Settlement.esp"; CHECK(!reason()); // One verified exterior worldspace spans cells/plugins.
    worldspace = 0x3D; CHECK(reason() == Stage::presentation_scene_changed); worldspace = 0x3C;
    worldspace_plugin = "Other.esm"; CHECK(reason() == Stage::presentation_scene_changed);
    worldspace_plugin = "Fallout4.esm";
    interior = true; worldspace = 0; worldspace_plugin.clear();
    CHECK(reason() == Stage::presentation_scene_changed);
    interior = false; worldspace = 0x3C; worldspace_plugin = "Fallout4.esm";
    position = {4096,0,0}; CHECK(!reason());
    position.x = 4096.01; CHECK(reason() == Stage::presentation_actor_out_of_range); position.x = 300;
    present = false; CHECK(reason() == Stage::presentation_actor_missing); present = true;
    base = 0x179FE; CHECK(reason() == Stage::presentation_actor_identity_changed); base = 0x179FF;
    actor_plugin = "Other.esm"; CHECK(reason() == Stage::presentation_actor_identity_changed);
    actor_plugin = "Fallout4.esm";
    disabled = true; CHECK(reason() == Stage::presentation_actor_unavailable); disabled = false;
    alive = false; CHECK(reason() == Stage::presentation_actor_unavailable); alive = true;
    posture = "sleeping"; CHECK(reason() == Stage::presentation_actor_unavailable); posture = "normal";
    player_save = "save:b"; CHECK(reason() == Stage::presentation_actor_identity_changed); player_save = "save:a";
    CHECK(admission.rejection_reason(nullptr,core::SnapshotClock::now()) == Stage::presentation_missing_capture);
    CHECK(admission.rejection_reason(scene.get(),scene->captured_at()+std::chrono::milliseconds{250}) ==
        Stage::presentation_stale_capture);
    CHECK(admission.rejection_reason(scene.get(),scene->captured_at()-std::chrono::milliseconds{1}) ==
        Stage::presentation_stale_capture);
    CHECK(!reason());
    owner.cancel(); CHECK(reason() == Stage::presentation_actor_cancelled);
    (void)runtime.invalidate(); CHECK(reason() == Stage::presentation_owner_changed);
}

int main() try {
    auto generation = synth::core::RuntimeGeneration::initial();
    {
        using namespace synth;
        client::ConversationHolds holds;
        core::CancellationSource turn{generation};
        const auto now=core::SnapshotClock::now();
        runtime::RuntimeActorIdentity codsworth{0x1CA7D,"Fallout4.esm","save:a"};
        runtime::RuntimeActorIdentity jun{0x1A4DB,"Fallout4.esm","save:a"};
        holds.add(codsworth,turn.token(),now);holds.add(jun,turn.token(),now);
        holds.add({0x14,"Fallout4.esm","save:a"},turn.token(),now);
        holds.add(codsworth,turn.token(),now);
        CHECK(holds.frame(true,generation,now).size()==2);
        CHECK(holds.frame(true,generation,now+std::chrono::seconds{90}).empty());
        holds.add(codsworth,turn.token(),now);holds.suspend(turn.token());
        holds.add(jun,turn.token(),now);CHECK(holds.frame(true,generation,now).empty());
        holds.clear();holds.add(codsworth,turn.token(),now);
        CHECK(holds.frame(false,generation,now).empty());
        CHECK(holds.frame(true,generation,now).empty());
        holds.add(codsworth,turn.token(),now);turn.cancel();
        CHECK(holds.frame(true,generation,now).empty());
        core::CancellationSource replacement{generation};holds.clear();holds.add(jun,replacement.token(),now);
        holds.suspend(turn.token());CHECK(holds.frame(true,generation,now).size()==1);
        core::GenerationClock clock; (void)clock.advance();
        CHECK(holds.frame(true,clock.current(),now).empty());
    }
    {
        synth::core::CancellationSource turn{generation};
        DialogueCaptions captions;
        auto proof = std::make_shared<SceneAdmission>();
        auto value = DialogueCaption{generation.value(), "Danse", "Validated fallback", turn.token()};
        value.admission = proof;
        const auto caption = std::make_shared<const DialogueCaption>(std::move(value));
        const auto now = DialogueCaptions::Clock::now();
        captions.enqueue(caption);
        CHECK(captions.validation_candidate(now) == caption);
        CHECK(!captions.frame(generation.value(), {}, false, now));
        CHECK(caption->delivery->load() == DialogueDelivery::pending);
        proof->accept(now);
        CHECK(captions.frame(generation.value(), {}, false, now) == caption);
        CHECK(!captions.validation_candidate(now));
        CHECK(!captions.frame(generation.value(), {}, false, now + std::chrono::seconds{1}));
        CHECK(!caption->delivered());
        CHECK(captions.validation_candidate(now + std::chrono::seconds{1}) == caption);
        const auto spoken = std::make_shared<const DialogueCaption>(DialogueCaption{generation.value(),"Danse","Audio",turn.token()});
        CHECK(captions.frame(generation.value(),spoken,false,now + std::chrono::seconds{1}) == spoken);
        proof->accept(now + std::chrono::seconds{1});
        CHECK(captions.frame(generation.value(), {}, false, now + std::chrono::seconds{1}) == caption);
        CHECK(!captions.frame(generation.value(), {}, true, now + std::chrono::seconds{5}));
        CHECK(captions.validation_candidate(now + std::chrono::seconds{5}) == caption);
        proof->rejected.store(true);
        proof->accept(now + std::chrono::seconds{5});
        CHECK(!caption->scene_ready(now + std::chrono::seconds{5}));
        CHECK(!captions.frame(generation.value(), {}, false, now + std::chrono::seconds{5}));
        CHECK(caption->delivery->load() == DialogueDelivery::discarded);
        CHECK(captions.idle() && !caption->delivered());
        caption->mark_delivery(DialogueDelivery::spoken);
        CHECK(!caption->delivered()); // Late completion cannot authorize continuation after rejection.
    }
    {
        synth::core::CancellationSource turn{generation};
        DialogueCaptions captions;
        const auto text = std::make_shared<const DialogueCaption>(DialogueCaption{
            generation.value(), "Danse", "Text only.", turn.token()});
        const auto spoken = std::make_shared<const DialogueCaption>(DialogueCaption{
            generation.value(), "Danse", "Speaking.", turn.token()});
        const auto now = DialogueCaptions::Clock::now();
        CHECK(captions.idle() && !text->delivered());
        captions.enqueue(text);
        CHECK(!captions.idle());
        CHECK(captions.frame(generation.value(), {}, false, now) == text);
        CHECK(text->delivery->load() == DialogueDelivery::reading && !text->delivered());
        CHECK(captions.frame(generation.value(), spoken, false, now + std::chrono::seconds{2}) == spoken);
        CHECK(!captions.frame(generation.value(), {}, true, now + std::chrono::seconds{12}));
        CHECK(captions.frame(generation.value(), {}, false, now + std::chrono::seconds{13}) == text);
        CHECK(!text->delivered()); // Neither speech overlap nor a paused menu consumes the reading window.
        CHECK(!captions.frame(generation.value(), {}, false, now + std::chrono::seconds{16}));
        CHECK(text->delivered() && captions.idle());
        captions.enqueue(text);
        CHECK(!captions.frame(generation.value() + 1, spoken, false, now));
        captions.enqueue(text);
        turn.cancel();
        CHECK(!captions.frame(generation.value(), spoken, false, now));
        captions.enqueue(text);
        CHECK(!captions.frame(generation.value(), {}, false, now));
        CHECK(!text->delivered());
        spoken->mark_delivery(DialogueDelivery::spoken);
        CHECK(!spoken->delivered()); // Cancellation wins even over a late playback-completion observation.
    }
    {
        synth::core::CancellationSource turn{generation};
        DialogueCaptions captions;
        auto caption = std::make_shared<const DialogueCaption>(DialogueCaption{
            generation.value(), "Preston", "Fallback", turn.token()});
        auto alias = caption;
        caption->mark_delivery(DialogueDelivery::playing);
        CHECK(!alias->delivered());
        caption->mark_delivery(DialogueDelivery::spoken);
        CHECK(alias->delivered());
        caption->mark_delivery(DialogueDelivery::discarded);
        CHECK(!alias->delivered());
        captions.enqueue(caption);
        for (int i = 0; i < 16; ++i) captions.enqueue(std::make_shared<const DialogueCaption>(DialogueCaption{
            generation.value(), "Preston", "Next", turn.token()}));
        CHECK(caption->delivery->load() == DialogueDelivery::discarded && !caption->delivered());
    }
    PassiveSubtitles subtitles{generation, 2, 5};
    CHECK(subtitles.show({generation, "a", "one"}) == SubtitleResult::accepted);
    CHECK(subtitles.show({generation, "b", "two"}) == SubtitleResult::accepted);
    CHECK(subtitles.show({generation, "c", "tri"}) == SubtitleResult::accepted);
    CHECK(subtitles.entries().size() == 2 && subtitles.entries().front().speaker_id == "b");
    CHECK(subtitles.show({generation, "x", "longer"}) == SubtitleResult::too_long);
    auto next = synth::core::RuntimeGeneration::from_value(2); subtitles.bind_generation(next);
    CHECK(subtitles.entries().empty()); CHECK(subtitles.show({generation, "x", "old"}) == SubtitleResult::stale_generation);

    OptionalPerformance optional; optional.fail(PresentationCapability::lipsync, "hook unavailable");
    CHECK(optional.status().lipsync == CapabilityState::failed); CHECK(!optional.status().lipsync_error.empty());
    CHECK(!optional.attempt(PresentationCapability::lipsync)); CHECK(!optional.audio_blocked());
    optional.ready(PresentationCapability::facing); CHECK(optional.attempt(PresentationCapability::facing));

    PresentationState state{generation}; CHECK(state.subtitles().show({generation, "a", "line"}) == SubtitleResult::accepted);
    state.optional().fail(PresentationCapability::facing, "adapter missing"); auto report = state.hard_halt();
    CHECK(report.subtitles_cleared == 1 && report.optional_state_cleared && report.terminal_cancellation);
    CHECK(state.subtitles().entries().empty()); CHECK(state.subtitles().show({generation, "a", "late"}) == SubtitleResult::terminal);
    CHECK(facing_yaw({}, {0,10,0}).value() == 0.0F);
    CHECK(std::abs(facing_yaw({}, {10,0,0}).value()-std::numbers::pi_v<float>/2) < 0.00001F);
    CHECK(std::abs(facing_yaw({}, {-10,0,0}).value()-3*std::numbers::pi_v<float>/2) < 0.00001F);
    CHECK(!facing_yaw({}, {0.5,0,0}));
    CHECK(!facing_yaw({}, {std::numeric_limits<double>::quiet_NaN(),0,0}));
    CHECK(!facing_yaw({}, {0,10,std::numeric_limits<double>::infinity()}));
    for (const auto* descriptor : {&synth::adapters::flat_descriptor,&synth::adapters::vr_descriptor}) {
        test_scene_admission_movement(*descriptor);
        using namespace synth;
        const core::WorldPose pose{{},core::UnitVector3::from({0,1,0}),core::UnitVector3::from({0,0,1})};
        std::uint32_t cell = 0x200, speaker_form = 0x101;
        bool disabled = false;
        std::string player_save = "save:a";
        adapters::detail::RuntimeBase runtime{*descriptor,[&](auto,auto) {
            const auto vr=descriptor->variant==core::RuntimeVariant::vr;
            return adapters::detail::CapturedRuntimeValues{pose,vr?std::optional{pose}:std::nullopt,{}, {},
                core::ActorSnapshot{0x14,"Player",{},"Fallout4.esm",player_save},
                {core::ActorSnapshot{speaker_form,"Settler",{},"Fallout4.esm","save:a",true,disabled},
                 core::ActorSnapshot{0x102,"Settler",{},"Fallout4.esm","save:a"}},100,
                core::WorldState{"Room","Room","","",true,100,core::SceneIdentity{cell,0,"Fallout4.esm",""}}};
        },[](auto){},[](auto){return runtime::RuntimeActionResult{};}};
        const auto scene=runtime.capture_snapshot(core::SnapshotClock::now());
        core::CancellationSource owner{scene->generation()};
        SceneAdmission admission;
        admission.original = scene;
        const auto& speaker = scene->actors().front();
        admission.actors.push_back({owner.token(),speaker.form_id(),speaker.base_form_id(),speaker.origin_plugin(),
            speaker.base_origin_plugin(),speaker.playthrough_id(),speaker.name(),true});
        const auto check_scene = [&] {
            const auto observed = runtime.capture_snapshot(core::SnapshotClock::now());
            return admission.matches(*observed,core::SnapshotClock::now());
        };
        CHECK(check_scene());
        cell = 0x201; CHECK(!check_scene()); cell = 0x200; // Identical room label is insufficient.
        speaker_form = 0x103; CHECK(!check_scene()); speaker_form = 0x101;
        disabled = true; CHECK(!check_scene()); disabled = false;
        CHECK(!admission.matches(*scene,scene->captured_at() + std::chrono::seconds{1}));
        CHECK(!admission.matches(*scene,scene->captured_at() - std::chrono::milliseconds{1}));
        CHECK(check_scene());
        protocol_native::Dialogue dialogue{protocol_native::Identity{"0x00000101","Fallout4.esm","save:a","Settler"},
            "Hello",{},protocol_native::Speech{"utterance:1","cache","ready",1000,{}},
            protocol_native::Identity{"0x00000102","Fallout4.esm","save:a","Settler"}};
        protocol_native::Line line{"line:1","request:1","turn:1",std::string{core::to_string(descriptor->variant)},
            scene->generation().value(),dialogue,2,7};
        const auto bound=client::bind_speech_facing(line,dialogue,*scene,owner.token());
        CHECK(bound && bound->speaker.form_id==0x101 && bound->listener.form_id==0x102);
        CHECK(bound->context_sequence==7 && bound->utterance_id=="utterance:1");
        auto text_dialogue=dialogue; text_dialogue.speech.reset();
        const auto text_facing=client::bind_speech_facing(line,text_dialogue,*scene,owner.token());
        CHECK(text_facing && text_facing->listener.form_id==0x102 && text_facing->utterance_id==line.line_id);
        for(int invalid=0;invalid<9;++invalid) {
            auto bad=dialogue; auto bad_line=line;
            if(invalid==0) bad.listener.reset();
            if(invalid==1) bad.listener->form_id="0x00000999";
            if(invalid==2) bad.listener->origin_plugin="Other.esm";
            if(invalid==3) bad.listener->playthrough_id="Save:a";
            if(invalid==4) bad.listener=std::get<protocol_native::Identity>(bad.speaker);
            if(invalid==5) bad.speaker=protocol_native::Narrator{"narrator","The Narrator"};
            if(invalid==6) std::get<protocol_native::Identity>(bad.speaker).form_id="0x00000014";
            if(invalid==7) ++bad_line.generation;
            if(invalid==8) bad_line.runtime_variant=descriptor->variant==core::RuntimeVariant::vr?"flat":"vr";
            CHECK(!client::bind_speech_facing(bad_line,bad,*scene,owner.token()));
        }
        auto player_dialogue=dialogue; player_dialogue.listener->form_id="0x00000014";
        CHECK(client::bind_speech_facing(line,player_dialogue,*scene,owner.token())->listener.form_id==0x14);
        CHECK(runtime.face_speech_listener(*bound).status==runtime::RuntimeActionStatus::unsupported_runtime);
        int attempts{};
        runtime.set_facing_pump([&](const auto& request) {
            ++attempts;
            CHECK(request.listener.form_id==0x102);
            return runtime::RuntimeActionResult{attempts==1 ? runtime::RuntimeActionStatus::rejected : runtime::RuntimeActionStatus::succeeded,"fixture"};
        });
        SpeechFacingOnce once;
        CHECK(once.pump(bound,runtime)->status==runtime::RuntimeActionStatus::rejected);
        CHECK(!once.pump(bound,runtime));
        CHECK(!once.pump({},runtime)); // Pausing/completion does not relinquish one-shot ownership.
        CHECK(!once.pump(std::make_shared<const runtime::RuntimeFacingRequest>(*bound),runtime));
        CHECK(attempts==1);
        bool worker_rejected{};
        std::thread worker{[&] {try{(void)runtime.face_speech_listener(*bound);}catch(const std::logic_error&){worker_rejected=true;}}};
        worker.join(); CHECK(worker_rejected && attempts==1);
        auto next_request=*bound; next_request.utterance_id="utterance:2";
        CHECK(once.pump(std::make_shared<const runtime::RuntimeFacingRequest>(next_request),runtime)->status==runtime::RuntimeActionStatus::succeeded);
        CHECK(attempts==2);
        int speech_calls{}; bool speech_discard{};
        runtime.set_speech_pump([&](const auto& frame, bool discard) {
            ++speech_calls; speech_discard=discard;
            if (frame) CHECK(frame->actor_id==0x101);
        });
        runtime::RuntimeSpeechFrame speech{runtime.generation().value(),0x101,"speech:1",owner.token()};
        runtime.animate_speech(speech);
        CHECK(speech_calls==1 && !speech_discard);
        runtime::RuntimeSpeechOwner clip_owner{runtime.generation().value(),"request","turn","clip",
            {0x101,"Fallout4.esm","save:a"},owner.token()};
        CHECK(!runtime.set_speech_owner(clip_owner)); // Unsupported adapters fail to text, never claim exclusivity.
        CHECK(runtime.set_speech_owner(std::nullopt));
        int owner_calls{}; bool owner_discard{}; bool owner_admit = true;
        runtime.set_speech_owner_pump([&](const auto& value, bool discard) {
            ++owner_calls; owner_discard=discard;
            if (value) CHECK(value->speaker.playthrough_id=="save:a");
            return !value || owner_admit;
        });
        CHECK(runtime.set_speech_owner(clip_owner));
        CHECK(runtime.set_speech_owner(clip_owner)); // Including console/menu-paused clip maintenance.
        for (int invalid=0;invalid<8;++invalid) {
            auto bad=clip_owner;
            if(invalid==0) ++bad.generation;
            if(invalid==1) bad.speaker.form_id=0;
            if(invalid==2) bad.speaker.origin_plugin.clear();
            if(invalid==3) bad.speaker.playthrough_id.clear();
            if(invalid==4) bad.request_id.clear();
            if(invalid==5) bad.turn_id.clear();
            if(invalid==6) bad.utterance_id.clear();
            if(invalid==7) bad.cancellation={};
            CHECK(!runtime.set_speech_owner(bad));
        }
        CHECK(owner_calls==2);
        bool owner_worker_rejected{};
        std::thread owner_worker{[&] {try {(void)runtime.set_speech_owner(clip_owner);}
            catch(const std::logic_error&) {owner_worker_rejected=true;}}};
        owner_worker.join(); CHECK(owner_worker_rejected && owner_calls==2);
        owner_admit=false; CHECK(!runtime.set_speech_owner(clip_owner)); CHECK(owner_calls==3);
        bool speech_worker_rejected{};
        std::thread speech_worker{[&] {try {runtime.animate_speech(speech);} catch(const std::logic_error&) {speech_worker_rejected=true;}}};
        speech_worker.join(); CHECK(speech_worker_rejected && speech_calls==1);
        owner.cancel(); next_request.utterance_id="utterance:3";
        CHECK(!runtime.set_speech_owner(clip_owner) && owner_calls==3);
        CHECK(runtime.set_speech_owner(std::nullopt) && owner_calls==4); // Release ignores canceled owner tokens.
        runtime.animate_speech(speech);
        CHECK(speech_calls==1 && !speech_discard);
        CHECK(!once.pump(std::make_shared<const runtime::RuntimeFacingRequest>(next_request),runtime));
        CHECK(!client::bind_speech_facing(line,dialogue,*scene,owner.token()));
        CHECK(runtime.face_speech_listener(next_request).status==runtime::RuntimeActionStatus::unavailable);
        (void)runtime.invalidate();
        CHECK(owner_calls==5 && owner_discard);
        CHECK(!runtime.set_speech_owner(clip_owner) && owner_calls==5);
        CHECK(speech_calls==2 && speech_discard);
        runtime.animate_speech(speech);
        CHECK(speech_calls==2); // Old generation cannot clear a new runtime's face owner.
        CHECK(!once.pump(bound,runtime)); CHECK(attempts==2);
    }
    std::cout << "presentation tests passed: subtitles, facing binding, yaw, ownership, pause and cancellation\n";
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
