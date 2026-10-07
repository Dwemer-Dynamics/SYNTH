#include "input/dialogue_input.hpp"
#include "ui/menu_framework_ui.hpp"
#include "input/press_gesture.hpp"
#include "integration/external_requests.hpp"
#include "integration/papyrus_requests.hpp"
#include <thread>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace synth::input;
static_assert(sizeof(SynthExternalRequestV1)==1032 && offsetof(SynthExternalRequestV1,epoch)==16 &&
              offsetof(SynthExternalRequestV1,text)==24 && sizeof(SynthExternalAPIV1)==24);
#define CHECK(x) do { if (!(x)) throw std::runtime_error{#x}; } while(false)
int main() try {
    {
        using synth::ui::VoiceAction;
        synth::ui::detail::VoiceImportBridge bridge;
        CHECK(bridge.ticket() == 0 && bridge.take() == VoiceAction::none);
        CHECK(!bridge.request(VoiceAction::import_all,0));
        bridge.set_available(true);
        const auto old_frame = bridge.ticket();
        CHECK(bridge.request(VoiceAction::import_all,old_frame));
        CHECK(bridge.request(VoiceAction::cancel,old_frame));
        CHECK(bridge.take() == VoiceAction::cancel && bridge.take() == VoiceAction::none);
        CHECK(bridge.request(VoiceAction::import_all,old_frame));
        bridge.set_available(false);
        CHECK(bridge.take() == VoiceAction::none);
        CHECK(!bridge.request(VoiceAction::import_all,old_frame));
        bridge.set_available(true); // Same game generation, different session.
        const auto new_frame = bridge.ticket();
        CHECK(new_frame != old_frame);
        CHECK(bridge.request(VoiceAction::cancel,new_frame));
        CHECK(!bridge.request(VoiceAction::import_all,old_frame));
        CHECK(bridge.take() == VoiceAction::cancel);
        bridge.set_available(true); // Ordinary publication does not reset the live revision.
        CHECK(bridge.ticket() == new_frame);
        CHECK(bridge.request(VoiceAction::import_all,new_frame));
        CHECK(bridge.take() == VoiceAction::import_all);
    }
    {
        // Voice and typed drafts revalidate the same copied identity, never a matching name.
        synth::integration::PromptTarget target{{}, 1, 1, "Fallout4.esm", "Fallout4.esm", "save", "Preston"};
        using synth::core::ActorSnapshot;
        CHECK(target.matches(ActorSnapshot{1, "Renamed Preston", {1, 2, 3}, "Fallout4.esm", "save"}));
        CHECK(!target.matches(ActorSnapshot{2, "Preston", {}, "Fallout4.esm", "save"}));
        CHECK(!target.matches(ActorSnapshot{1, "Preston", {}, "Other.esp", "save"}));
        CHECK(!target.matches(ActorSnapshot{1, "Preston", {}, "Fallout4.esm", "other-save"}));
        const ActorSnapshot original{1, "Preston", {}, "Fallout4.esm", "save"};
        target.base_form_id = 2;
        CHECK(!target.matches(original));
        target.base_form_id = 1;
        target.base_origin_plugin = "Other.esp";
        CHECK(!target.matches(original));
    }
    synth::integration::ExternalConversationOwner owner;
    const std::string piper="save|Fallout4.esm|0x00000001", nick="save|Fallout4.esm|0x00000002";
    CHECK(owner.allows_ask(piper,true));
    CHECK(!owner.allows_ask(piper,false)); // Unknown busy work is never assumed to be this actor's.
    owner.begin(piper); owner.claim(piper);
    CHECK(owner.allows_ask(piper,false)); CHECK(owner.allows_ask(piper,true));
    CHECK(!owner.allows_ask(nick,false)); CHECK(!owner.allows_ask(nick,true));
    owner.claim(nick);
    CHECK(!owner.allows_ask(piper,false)); CHECK(!owner.allows_ask(nick,false));
    owner.idle();
    CHECK(owner.allows_ask(piper,true)); CHECK(!owner.allows_ask(nick,true));
    owner.end(nick); CHECK(!owner.allows_ask(nick,true));
    owner.end(piper); CHECK(owner.allows_ask(nick,true));
    owner.claim(std::nullopt); owner.claim(nick); CHECK(!owner.allows_ask(nick,false));
    owner.begin(nick); owner.claim(nick); CHECK(owner.allows_ask(nick,false));
    CHECK(!owner.allows_ask("other-save|Fallout4.esm|0x00000002",true));
    owner.begin(std::string{}); owner.claim(nick);
    CHECK(!owner.allows_ask(nick,true)); CHECK(!owner.allows_ask(nick,false));
    owner.end(""); CHECK(owner.allows_ask(nick,true)); // Failed recording releases its unresolved owner only.
    owner.begin(piper); owner.end(""); CHECK(!owner.allows_ask(nick,true));

    synth::integration::ExternalRequests external;
    CHECK(synth::integration::submit_papyrus_request(external,SYNTH_EXTERNAL_ASK,0x1234,"question")==SYNTH_EXTERNAL_UNAVAILABLE);
    external.set_available(true);
    for (const auto kind : {SYNTH_EXTERNAL_SPEAK_EXACT,SYNTH_EXTERNAL_COMMENT,SYNTH_EXTERNAL_REACT,SYNTH_EXTERNAL_ASK,SYNTH_EXTERNAL_OPEN_PROMPT}) {
        const auto no_text=kind==SYNTH_EXTERNAL_COMMENT || kind==SYNTH_EXTERNAL_OPEN_PROMPT;
        const auto text=no_text ? std::string_view{} : std::string_view{"  exact  text  "};
        CHECK(synth::integration::submit_papyrus_request(external,kind,-1,text)==SYNTH_EXTERNAL_ACCEPTED);
        const auto copy=external.take();
        CHECK(copy && copy->actor_form_id==UINT32_MAX && copy->kind==kind);
        CHECK(std::string_view{copy->text}==(no_text ? "" : "exact  text"));
    }
    CHECK(synth::integration::submit_papyrus_request(external,SYNTH_EXTERNAL_ASK,0x14,"question")==SYNTH_EXTERNAL_INVALID);
    CHECK(synth::integration::submit_papyrus_request(external,SYNTH_EXTERNAL_ASK,0,"question")==SYNTH_EXTERNAL_INVALID);
    CHECK(synth::integration::submit_papyrus_request(external,SYNTH_EXTERNAL_ASK,1,std::string(1001,'x'))==SYNTH_EXTERNAL_INVALID);
    CHECK(synth::integration::submit_papyrus_request(external,SYNTH_EXTERNAL_ASK,1,std::string_view{"a\0b",3})==SYNTH_EXTERNAL_INVALID);
    CHECK(synth::integration::submit_papyrus_request(external,SYNTH_EXTERNAL_ASK,1,"\xC0")==SYNTH_EXTERNAL_INVALID);
    external.set_available(false);
    const auto api_now=synth::integration::ExternalRequests::Clock::time_point{};
    SynthExternalRequestV1 request{};
    request.size=sizeof(request); request.kind=SYNTH_EXTERNAL_ASK; request.actor_form_id=0x1234;
    std::memcpy(request.text,"  literal /diary  ",sizeof("  literal /diary  "));
    CHECK(external.get_epoch()==0);
    CHECK(external.submit(&request,api_now)==SYNTH_EXTERNAL_UNAVAILABLE);
    external.set_available(true);request.epoch=external.get_epoch();
    CHECK(request.epoch!=0);
    CHECK(external.submit(&request,api_now)==SYNTH_EXTERNAL_ACCEPTED);
    request.text[2]='X';const auto copied=external.take(api_now);
    CHECK(copied && std::string{copied->text}=="literal /diary" && copied->actor_form_id==0x1234);
    CHECK(!external.take(api_now));
    CHECK(external.submit(nullptr,api_now)==SYNTH_EXTERNAL_INVALID);
    auto invalid=request;invalid.size=0;CHECK(external.submit(&invalid,api_now)==SYNTH_EXTERNAL_INVALID);
    invalid=request;invalid.reserved=1;CHECK(external.submit(&invalid,api_now)==SYNTH_EXTERNAL_INVALID);
    for(auto actor:{0u,0x14u}) { invalid=request;invalid.actor_form_id=actor;CHECK(external.submit(&invalid,api_now)==SYNTH_EXTERNAL_INVALID); }
    for(auto kind:{0u,6u,UINT32_MAX}) { invalid=request;invalid.kind=kind;CHECK(external.submit(&invalid,api_now)==SYNTH_EXTERNAL_INVALID); }
    invalid=request;std::memset(invalid.text,'a',sizeof(invalid.text));CHECK(external.submit(&invalid,api_now)==SYNTH_EXTERNAL_INVALID);
    invalid.text[1000]=0;CHECK(external.submit(&invalid,api_now)==SYNTH_EXTERNAL_ACCEPTED);
    CHECK(std::strlen(external.take(api_now)->text)==1000);
    for(const char* text:{""," \t\r\n","\xC0","\xED\xA0\x80"}) {
        invalid=request;std::memcpy(invalid.text,text,std::strlen(text)+1);CHECK(external.submit(&invalid,api_now)==SYNTH_EXTERNAL_INVALID);
    }
    invalid=request;invalid.kind=SYNTH_EXTERNAL_COMMENT;
    CHECK(external.submit(&invalid,api_now)==SYNTH_EXTERNAL_INVALID);
    invalid.text[0]=0;CHECK(external.submit(&invalid,api_now)==SYNTH_EXTERNAL_ACCEPTED);
    CHECK(external.take(api_now)->kind==SYNTH_EXTERNAL_COMMENT);
    invalid.kind=SYNTH_EXTERNAL_OPEN_PROMPT;CHECK(external.submit(&invalid,api_now)==SYNTH_EXTERNAL_ACCEPTED);
    CHECK(external.take(api_now)->kind==SYNTH_EXTERNAL_OPEN_PROMPT);
    invalid.text[0]='x';CHECK(external.submit(&invalid,api_now)==SYNTH_EXTERNAL_INVALID);
    for(auto kind:{SYNTH_EXTERNAL_SPEAK_EXACT,SYNTH_EXTERNAL_REACT,SYNTH_EXTERNAL_ASK}) {
        request.kind=kind;std::memcpy(request.text,"Keep  this spacing.\n",sizeof("Keep  this spacing.\n"));
        CHECK(external.submit(&request,api_now)==SYNTH_EXTERNAL_ACCEPTED);
        CHECK(std::string{external.take(api_now)->text}=="Keep  this spacing.");
    }
    CHECK(external.submit(&request,api_now)==SYNTH_EXTERNAL_ACCEPTED);
    CHECK(!external.take(api_now+synth::integration::ExternalRequests::lifetime));
    for(std::size_t i=0;i<external.capacity;++i) CHECK(external.submit(&request,api_now)==SYNTH_EXTERNAL_ACCEPTED);
    CHECK(external.submit(&request,api_now)==SYNTH_EXTERNAL_BUSY);
    external.set_available(false);external.set_available(true);
    CHECK(external.submit(&request,api_now)==SYNTH_EXTERNAL_STALE);
    CHECK(!external.take(api_now));request.epoch=external.get_epoch();
    std::atomic_uint accepted{};
    std::vector<std::thread> producers;
    for(int i=0;i<4;++i) producers.emplace_back([&] {
        for(int j=0;j<100;++j) if(external.submit(&request,api_now)==SYNTH_EXTERNAL_ACCEPTED) ++accepted;
    });
    for(auto& producer:producers)producer.join();
    CHECK(accepted==external.capacity);
    std::size_t drained{};while(external.take(api_now))++drained;CHECK(drained==accepted);

    TypedTextBuffer text{{5, 3}};
    CHECK(text.append("a") == TextEditResult::accepted);
    CHECK(text.append("\xC3\xA9") == TextEditResult::accepted);
    CHECK(text.code_points() == 2);
    CHECK(text.append("bc") == TextEditResult::code_point_limit);
    CHECK(text.append("\xC0") == TextEditResult::malformed_utf8);
    text.erase_last_code_point(); CHECK(text.text() == "a"); CHECK(text.take() == "a");

    PushToTalkState ptt{{Hand::left, false}};
    CHECK(ptt.apply({SemanticAction::push_to_talk, ButtonTransition::pressed, InputDevice::vr_controller, Hand::right}) == PttEventResult::wrong_hand);
    CHECK(ptt.apply({SemanticAction::push_to_talk, ButtonTransition::pressed, InputDevice::keyboard, Hand::none}) == PttEventResult::fallback_disabled);
    CHECK(ptt.apply({SemanticAction::push_to_talk, ButtonTransition::pressed, InputDevice::vr_controller, Hand::left}) == PttEventResult::accepted && ptt.active());

    VoiceCaptureGate gate; CHECK(!gate.options().open_microphone); CHECK(!gate.should_capture(true));
    gate.set_push_to_talk(true); CHECK(gate.should_capture());
    gate.set_suspended(CaptureSuspension::menu, true); CHECK(!gate.should_capture());
    gate.set_suspended(CaptureSuspension::menu, false); CHECK(gate.should_capture());
    gate.hard_halt(); CHECK(gate.terminal() && !gate.should_capture());

    SttUploadState uploads{2, 10}; auto generation = synth::core::RuntimeGeneration::initial();
    CHECK(uploads.add("capture_1", generation, 6) == UploadResult::accepted);
    CHECK(uploads.add("https://bad", generation, 1) == UploadResult::invalid_reference);
    CHECK(uploads.add("capture_2", generation, 5) == UploadResult::byte_limit);
    CHECK(uploads.mark_uploading("capture_1"));
    auto report = uploads.hard_halt(); CHECK(report.cancelled == 1 && report.released_bytes == 6 && report.terminal);
    CHECK(uploads.add("late", generation, 1) == UploadResult::terminal);

    using namespace std::chrono_literals;
    PressGesture gesture;
    const auto start = PressGesture::Clock::time_point{};
    CHECK(gesture.update(true, start) == PressGestureEvent::none);
    CHECK(gesture.update(false, start + 50ms) == PressGestureEvent::none);
    CHECK(gesture.update(false, start + 351ms) == PressGestureEvent::tap);
    CHECK(gesture.update(true, start + 1s) == PressGestureEvent::none);
    CHECK(gesture.update(false, start + 1050ms) == PressGestureEvent::none);
    CHECK(gesture.update(true, start + 1100ms) == PressGestureEvent::none);
    CHECK(gesture.update(false, start + 1150ms) == PressGestureEvent::double_tap);
    CHECK(gesture.update(true, start + 2s) == PressGestureEvent::none);
    CHECK(gesture.update(true, start + 2600ms) == PressGestureEvent::hold);
    CHECK(gesture.update(false, start + 2700ms) == PressGestureEvent::none);
    gesture.reset();
    CHECK(gesture.update(false, start + 4s) == PressGestureEvent::none);
    // Chatbox Stop control. The render thread queues a stop against the receipt
    // that drew the button; the game thread takes it once. Composing state is a
    // cached flag so the pump never reads renderer-owned state to pause on it.
    {
        using synth::ui::detail::ChatboxBridge;
        ChatboxBridge bridge;
        CHECK(!bridge.is_open()); CHECK(!bridge.take_stop_request());

        CHECK(bridge.request_open());
        const auto draft = bridge.consume_open_request();
        CHECK(draft.has_value()); CHECK(bridge.is_open());
        // A receipt that is not the live draft's stops nothing.
        CHECK(!bridge.request_stop(draft->revision + 1)); CHECK(!bridge.has_stop_request());
        CHECK(bridge.request_stop(draft->revision)); CHECK(bridge.has_stop_request());
        // Stop is not a second Cancel: the window stays up and the draft lives.
        CHECK(bridge.is_open()); CHECK(bridge.has_draft());
        CHECK(bridge.take_stop_request()); CHECK(!bridge.take_stop_request());

        // Closing alone neither queues a stop nor retracts one already pressed.
        CHECK(bridge.request_stop(draft->revision));
        bridge.cancel(draft->revision);
        CHECK(!bridge.is_open()); CHECK(!bridge.has_draft());
        CHECK(bridge.take_stop_request());
        CHECK(bridge.request_open());
        const auto quiet = bridge.consume_open_request();
        CHECK(quiet.has_value());
        bridge.cancel(quiet->revision);
        CHECK(!bridge.take_stop_request()); CHECK(!bridge.is_open());
    }
    {
        using synth::ui::detail::ChatboxBridge;
        ChatboxBridge bridge;
        synth::core::CancellationSource session{generation};
        synth::integration::PromptTarget target{};
        target.session = session.token();
        target.display_name = "Piper";
        CHECK(bridge.request_open(target));
        const auto draft = bridge.consume_open_request();
        CHECK(draft.has_value()); CHECK(bridge.is_open());

        // A retired session stops nothing, and the cached flag converges on the
        // next `has_draft()` the render lifecycle runs.
        session.cancel();
        CHECK(!bridge.request_stop(draft->revision)); CHECK(!bridge.has_stop_request());
        CHECK(!bridge.has_draft()); CHECK(!bridge.is_open());

        // Session cleanup drops a stop queued while the session was alive, and
        // retires the receipt so the renderer cannot re-queue it afterwards.
        synth::core::CancellationSource live{generation};
        synth::integration::PromptTarget next{};
        next.session = live.token();
        bridge.discard();
        CHECK(bridge.request_open(next));
        const auto pending = bridge.consume_open_request();
        CHECK(pending.has_value()); CHECK(bridge.request_stop(pending->revision));
        bridge.discard();
        CHECK(!bridge.take_stop_request()); CHECK(!bridge.is_open());
        CHECK(!bridge.request_stop(pending->revision)); CHECK(!bridge.take_stop_request());
    }
    // Without the framework header the Stop control is inert, like the window.
    CHECK(!synth::ui::Chatbox::is_open());
    CHECK(!synth::ui::Chatbox::take_stop_request());

    {
        synth::ui::detail::ControlMenuBridge bridge;
        CHECK(bridge.request_open(7, "Preston", "STANDARD", "standard"));
        const auto draft = bridge.consume_open_request();
        CHECK(draft.has_value());
        CHECK(bridge.submit(synth::ui::ControlAction::chat_mode, "WHISPER", draft->revision) == synth::ui::detail::ControlSubmit::accepted);
        const auto control_request = bridge.take_request();
        CHECK(control_request && control_request->generation == 7 && control_request->value == "WHISPER");
        CHECK(bridge.status().mode == "STANDARD");
        CHECK(bridge.status().pending_mode == "WHISPER");
        bridge.publish_status("WHISPER", "standard", "confirmed");
        CHECK(bridge.status().mode == "WHISPER");
        CHECK(bridge.status().pending_mode.empty());
        bridge.invalidate();
        CHECK(!bridge.is_open());
        CHECK(bridge.submit(synth::ui::ControlAction::wait_here, "Preston", draft->revision) == synth::ui::detail::ControlSubmit::stale);
        CHECK(!bridge.take_request());
    }
    // Auto-close must preserve exactly one owned wait/release request; load invalidation must not.
    for (auto action : {synth::ui::ControlAction::wait_here, synth::ui::ControlAction::release_wait}) {
        synth::ui::detail::ControlMenuBridge bridge;
        CHECK(bridge.request_open(9, "Raider", "STANDARD", "standard"));
        const auto draft = bridge.consume_open_request();
        CHECK(bridge.submit(action, "Raider", draft->revision) == synth::ui::detail::ControlSubmit::accepted);
        bridge.cancel(draft->revision);
        CHECK(!bridge.is_open());
        const auto wait_request = bridge.take_request();
        CHECK(wait_request && wait_request->generation == 9 && wait_request->action == action);
        CHECK(!bridge.take_request());
        CHECK(bridge.request_open(10, "Preston", "STANDARD", "standard"));
        const auto next = bridge.consume_open_request();
        CHECK(bridge.submit(action, "Preston", next->revision) == synth::ui::detail::ControlSubmit::accepted);
        bridge.invalidate();
        CHECK(!bridge.take_request());
    }
    // Normal chat retains its selected actor and manual admission policy through native menu pause.
    {
        synth::ui::detail::ChatboxBridge bridge;
        synth::core::CancellationSource chat_owner{generation};
        synth::integration::PromptTarget target{chat_owner.token(), 0x1A4D7, 0x19FD9,
            "Fallout4.esm", "Fallout4.esm", "test-playthrough", "Preston", true};
        CHECK(bridge.request_open(target));
        auto draft = bridge.consume_open_request();
        CHECK(draft && draft->target && draft->target->display_name == "Preston");
        CHECK(bridge.submit("hello", draft->revision) == synth::ui::detail::ChatboxSubmit::accepted);
        auto message = bridge.take_submission();
        CHECK(message && message->target && message->target->form_id == 0x1A4D7);
        CHECK(message->target->manual_chat);
        CHECK(!bridge.take_submission());
        CHECK(bridge.request_open(target));
        draft = bridge.consume_open_request();
        chat_owner.cancel();
        CHECK(bridge.submit("stale", draft->revision) == synth::ui::detail::ChatboxSubmit::stale);
        CHECK(!bridge.take_submission());
    }
    std::cout << "input tests passed\n";
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
