#include "radio/probe_host.hpp"
#include "adapters/flat_fallout_runtime.hpp"
#include "adapters/vr_fallout_runtime.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
int assertions{};
#define CHECK(value) do { ++assertions; if (!(value)) throw std::runtime_error{#value}; } while(false)
using namespace synth;

adapters::FlatFalloutRuntime make_runtime() {
    return {[](auto, auto) -> adapters::detail::CapturedRuntimeValues { throw std::logic_error{"capture unused"}; },
            [](std::string_view) {}, [](const auto&) { return runtime::RuntimeActionResult{}; }};
}

void test_mailbox_bounds_and_generation() {
    radio::ProbeMailbox mailbox;
    const auto one = core::RuntimeGeneration::initial();
    const auto two = core::RuntimeGeneration::from_value(2);
    CHECK(!mailbox.submit({one, radio::ProbeOperation::enable, 0}));
    mailbox.invalidate(one);
    CHECK(!mailbox.submit({one, radio::ProbeOperation::play, 0}));
    CHECK(!mailbox.submit({two, radio::ProbeOperation::enable, 0}));
    CHECK(!mailbox.submit({one, radio::ProbeOperation::enable, 2}));
    CHECK(!mailbox.submit({one, static_cast<radio::ProbeOperation>(255), 0}));
    CHECK(mailbox.submit({one, radio::ProbeOperation::enable, 0}));
    CHECK(!mailbox.submit({one, radio::ProbeOperation::enable, 0}));
    CHECK(!mailbox.take(two));
    CHECK(mailbox.take(one).has_value());
    CHECK(!mailbox.take(one));
    mailbox.publish(one, true, "copied");
    CHECK(mailbox.enabled());
    CHECK(mailbox.submit({one, radio::ProbeOperation::play, 0}));
    mailbox.invalidate(two);
    CHECK(!mailbox.enabled());
    CHECK(!mailbox.take(two));
    mailbox.publish(one, true, "stale");
    CHECK(!mailbox.enabled());
    CHECK(mailbox.diagnostic().find("stale") == std::string::npos);
}

void test_disabled_probe_and_reload_cancel() {
    auto runtime = make_runtime();
    int captures{}, commands{}, cleanup{};
    runtime.set_radio_probe_pumps([&](auto generation) {
        ++captures;
        radio::ProbeObservation result{generation};
        result.supported = result.records_available = true;
        return result;
    }, [&](const auto& request) {
        ++commands;
        if (request.operation == radio::ProbeOperation::disable) ++cleanup;
        return radio::ProbeResult{true, "accepted"};
    });
    radio::ProbeHost host;
    const auto now = core::SnapshotClock::now();
    host.tick(runtime, now);
    CHECK(cleanup == 1);
    CHECK(captures == 0);
    CHECK(!radio::probe_mailbox.enabled());
    CHECK(radio::probe_mailbox.submit({runtime.generation(), radio::ProbeOperation::enable, 0}));
    host.tick(runtime, now);
    CHECK(radio::probe_mailbox.enabled());
    CHECK(captures == 1);
    CHECK(radio::probe_mailbox.diagnostic().find("\"live_rewrite_authorized\":false") != std::string::npos);
    CHECK(radio::probe_mailbox.submit({runtime.generation(), radio::ProbeOperation::play, 0}));
    host.invalidate();
    (void)runtime.invalidate();
    const auto before = commands;
    host.tick(runtime, now);
    CHECK(commands == before + 1);
    CHECK(cleanup == 2);
    CHECK(!radio::probe_mailbox.enabled());
    CHECK(!radio::probe_mailbox.submit({runtime.generation(), radio::ProbeOperation::play, 0}));
    CHECK(captures == 1);
}

void test_runtime_affinity_and_unsupported() {
    auto runtime = make_runtime();
    CHECK(!runtime.capture_radio_probe().supported);
    CHECK(!runtime.execute_radio_probe({runtime.generation(), radio::ProbeOperation::enable, 0}).succeeded);
    CHECK(runtime.execute_radio_probe({core::RuntimeGeneration::from_value(2), radio::ProbeOperation::enable, 0}).detail == "stale_generation");
    bool rejected{};
    std::thread worker([&] {
        try { (void)runtime.capture_radio_probe(); }
        catch (const std::logic_error&) { rejected = true; }
    });
    worker.join();
    CHECK(rejected);
    adapters::VrFalloutRuntime vr{[](auto, auto) -> adapters::detail::CapturedRuntimeValues { throw std::logic_error{"capture unused"}; },
        [](std::string_view) {}, [](const auto&) { return runtime::RuntimeActionResult{}; }};
    CHECK(!vr.capture_radio_probe().supported);
    CHECK(!vr.execute_radio_probe({vr.generation(), radio::ProbeOperation::enable, 0}).succeeded);
}

void test_sound_receipt_does_not_infer_silence() {
    radio::ProbeObservation value{core::RuntimeGeneration::initial()};
    value.slots[0].scene_active = false;
    value.slots[0].sound_valid = value.slots[0].sound_busy = true;
    value.slots[0].sound_state_available = true;
    value.slots[0].native_descriptor_id = 0x08000808;
    value.slots[0].expected_descriptor_id = 0x08000808;
    value.slots[0].sound_id = 0x08000808;
    value.slots[0].action_status = 33;
    value.slots[0].handle_duration = 22000;
    value.slots[0].sound_done = false;
    value.slots[0].action_duration = 5;
    const auto text = radio::probe_json(value, true, 7, "stop_requested");
    CHECK(text.find("\"scene_active\":false") != std::string::npos);
    const auto parsed = json::parse(text);
    const auto& slot = parsed.find("slots")->as_array()[0];
    CHECK(slot.find("sound_playing")->is_null());
    CHECK(slot.find("sound_busy")->as_bool());
    CHECK(slot.find("sound_state_available")->as_bool());
    CHECK(slot.find("sound_id")->as_uint64() == 0x08000808);
    CHECK(slot.find("native_descriptor_id")->as_uint64() == 0x08000808);
    CHECK(slot.find("expected_descriptor_id")->as_uint64() == 0x08000808);
    CHECK(slot.find("native_descriptor_owned")->as_bool());
    CHECK(slot.find("action_status")->as_uint64() == 33);
    CHECK(slot.find("action_base_status")->as_uint64() == 1);
    CHECK(slot.find("action_status_flags")->as_uint64() == 32);
    CHECK(slot.find("native_duration_milliseconds")->as_uint64() == 22000);
    CHECK(text.find("stop_requested") != std::string::npos);
    CHECK(text.find("\"audible_cache_refresh_verified\":false") != std::string::npos);
}

void test_owned_radio_state_admission() {
    constexpr std::uint32_t owned = 0x08000808;
    radio::SlotObservation slot;
    slot.available = slot.sound_state_available = true;
    slot.expected_descriptor_id = owned;
    slot.phase = 0xFFFFFFFFU;
    slot.action_status = 32; // opaque upper flags with stopped base state
    slot.sound_done = false; // EndAction resets this field; it is not a busy witness
    CHECK(radio::action_base_status(32) == 0);
    CHECK(radio::action_status_flags(33) == 32);
    CHECK(radio::slot_readiness(slot, owned) == radio::SlotReadiness::ready);
    slot.phase = 0; // inactive scene with a live phase remains a transition
    CHECK(radio::slot_readiness(slot, owned) == radio::SlotReadiness::busy);
    slot.phase = 0xFFFFFFFFU;

    // A still-valid, finished engine handle may retain an owned descriptor.
    slot.sound_valid = true;
    slot.sound_id = slot.native_descriptor_id = owned;
    slot.handle_duration = 22000;
    CHECK(radio::slot_readiness(slot, owned) == radio::SlotReadiness::ready);
    slot.action_status = 34;
    CHECK(radio::slot_readiness(slot, owned) == radio::SlotReadiness::ready);
    slot.action_status = 33;
    CHECK(radio::slot_readiness(slot, owned) == radio::SlotReadiness::busy);
    slot.action_status = 35; // unrecognized low status must not authorize replay
    CHECK(radio::slot_readiness(slot, owned) == radio::SlotReadiness::busy);
    slot.action_status = 32;
    slot.scene_active = true;
    CHECK(radio::slot_readiness(slot, owned) == radio::SlotReadiness::busy);
    slot.scene_active = false;
    slot.sound_busy = true;
    CHECK(radio::slot_readiness(slot, owned) == radio::SlotReadiness::busy);
    slot.sound_busy = false;
    slot.sound_paused = true;
    CHECK(radio::slot_readiness(slot, owned) == radio::SlotReadiness::busy);
    slot.sound_paused = false;

    // A queued radio entry is busy even before it has a valid handle/descriptor.
    slot.sound_valid = false;
    slot.sound_id = slot.native_descriptor_id = 0xFFFFFFFFU;
    slot.sound_busy = true;
    CHECK(radio::slot_readiness(slot, owned) == radio::SlotReadiness::busy);
    slot.sound_busy = false;
    slot.sound_state_available = false;
    CHECK(radio::slot_readiness(slot, owned) == radio::SlotReadiness::state_unavailable);
    slot.sound_state_available = true;
    slot.sound_valid = true; // inconsistent copied snapshot
    CHECK(radio::slot_readiness(slot, owned) == radio::SlotReadiness::state_unavailable);
    slot.native_descriptor_id = owned + 1;
    CHECK(radio::slot_readiness(slot, owned) == radio::SlotReadiness::descriptor_mismatch);
    slot.native_descriptor_id = owned;
    slot.sound_id = owned + 1;
    CHECK(radio::slot_readiness(slot, owned) == radio::SlotReadiness::descriptor_mismatch);
    slot.sound_id = owned;
    CHECK(radio::slot_readiness(slot, 0) == radio::SlotReadiness::state_unavailable);
    CHECK(radio::slot_readiness(slot, 0xFFFFFFFFU) == radio::SlotReadiness::state_unavailable);
    slot.expected_descriptor_id = owned + 1;
    CHECK(radio::slot_readiness(slot, owned) == radio::SlotReadiness::state_unavailable);
    slot.expected_descriptor_id = owned;
    slot.available = false;
    CHECK(radio::slot_readiness(slot, owned) == radio::SlotReadiness::state_unavailable);
}
}
int main() {
    try {
        test_mailbox_bounds_and_generation();
        test_disabled_probe_and_reload_cancel();
        test_runtime_affinity_and_unsupported();
        test_sound_receipt_does_not_infer_silence();
        test_owned_radio_state_admission();
        std::cout << "Radio probe tests passed: " << assertions << " assertions\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
