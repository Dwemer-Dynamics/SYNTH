#include "adapters/adapter_descriptor.hpp"
#include "adapters/adapter_lifecycle.hpp"

#if defined(SYNTH_WITH_F4SE) && defined(SYNTH_WITH_F4SEVR)
#error "flat and VR ABI macros are mutually exclusive"
#endif

#if defined(SYNTH_WITH_F4SEVR)
#include "adapters/vr_fallout_runtime.hpp"
#include "adapters/commonlib_actions.hpp"
#include "adapters/commonlib_capture.hpp"
#include "adapters/commonlib_external_requests.hpp"
#include "adapters/commonlib_papyrus_api.hpp"
#include "adapters/commonlib_save_context.hpp"
#include "runtime/game_thread_dispatcher.hpp"
#include "client/plugin_session.hpp"
#include "client/plugin_session_host.hpp"
#include "input/press_gesture.hpp"
#include "presentation/commonlib_face_animation.hpp"
#include "adapters/commonlib_speech_facing.hpp"
#include "ui/settings_store.hpp"

#include <F4SE/F4SE.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <array>
#include <chrono>
#include <filesystem>
#include <string>

namespace {

using synth::adapters::LifecycleAction;

synth::adapters::AdapterLifecycle lifecycle;
synth::adapters::DeferredLifecycleActions deferred_lifecycle;
std::unique_ptr<synth::adapters::VrFalloutRuntime> runtime;
std::unique_ptr<synth::runtime::GameThreadDispatcher> dispatcher;
std::unique_ptr<synth::client::PluginSession> session;
std::unique_ptr<synth::client::PluginSessionHost> session_host;
std::uint64_t pending_session_request{};
bool runtime_disabled{};
std::array<bool, 256> hotkey_down{};
bool voice_capture_down{};
bool player_was_in_combat{};
auto next_context_publish = synth::core::SnapshotClock::time_point{};
auto next_profile_refresh = synth::core::SnapshotClock::time_point{};
auto profile_timer_paused_at = synth::core::SnapshotClock::time_point{};
auto runtime_ready_after = synth::core::SnapshotClock::time_point::max();
synth::config::FileRevision settings_revision{};
bool settings_revision_initialized{};
synth::input::PressGesture pip_vision_gesture;
synth::presentation::CommonlibFaceAnimation face_animation;
synth::presentation::SpeechFacingOnce speech_facing;

void pump_runtime() noexcept;
void apply_lifecycle(LifecycleAction action);

// CommonLibF4VR exposes one-shot tasks, so each completed pump schedules the next frame.
void queue_runtime_pump() {
    if (const auto* tasks = F4SE::GetTaskInterface(); tasks != nullptr) {
        tasks->AddTask(pump_runtime);
    }
}

[[nodiscard]] synth::adapters::Version version_of(const REL::Version& version) noexcept {
    return {version.major(), version.minor(), version.patch(), version.build()};
}

[[nodiscard]] bool environment_supported(const F4SE::QueryInterface* query) noexcept {
    return query != nullptr && !query->IsEditor() &&
           synth::adapters::supports_exact_environment(
               synth::adapters::vr_descriptor,
               synth::core::RuntimeVariant::vr,
               version_of(query->RuntimeVersion()),
               version_of(query->F4SEVersion()));
}

[[nodiscard]] synth::adapters::detail::CapturedRuntimeValues capture_values(
    synth::core::SnapshotClock::time_point now,
    synth::runtime::RuntimeCapturePurpose purpose) {
    return synth::adapters::commonlib::capture_vr(now, purpose);
}

void present_notification(std::string_view message) {
    const std::string owned{message};
    RE::SendHUDMessage::ShowHUDMessage(owned.c_str(), nullptr, false, false);
}

[[nodiscard]] synth::runtime::RuntimeActionResult execute_runtime_action(
    const synth::runtime::RuntimeActionRequest& request) {
    return synth::adapters::commonlib::execute_action(request);
}

// SYNTH persists DirectInput scan codes. DIK marks extended keys with bit 0x80
// while Windows spells the same keys with an 0xE0 scan-code prefix, so restore
// that prefix before asking the keyboard layout for a virtual key.
[[nodiscard]] UINT dik_to_virtual_key(std::uint32_t scan_code) {
    if (scan_code == 0 || scan_code > 0xFF) return 0;
    const UINT windows_scan_code =
        (scan_code & 0x80U) != 0 ? 0xE000U | (scan_code & 0x7FU) : scan_code;
    return MapVirtualKeyW(windows_scan_code, MAPVK_VSC_TO_VK_EX);
}

[[nodiscard]] bool hotkey_pressed(std::uint32_t scan_code) {
    if (scan_code == 0 || scan_code >= hotkey_down.size()) return false;
    const auto virtual_key = dik_to_virtual_key(scan_code);
    if (virtual_key == 0) return false;
    const auto down = (GetAsyncKeyState(static_cast<int>(virtual_key)) & 0x8000) != 0;
    const auto pressed = down && !hotkey_down[scan_code];
    hotkey_down[scan_code] = down;
    return pressed;
}

[[nodiscard]] bool hotkey_held(std::uint32_t scan_code) {
    if (scan_code == 0 || scan_code >= hotkey_down.size()) return false;
    const auto virtual_key = dik_to_virtual_key(scan_code);
    return virtual_key != 0 && (GetAsyncKeyState(static_cast<int>(virtual_key)) & 0x8000) != 0;
}

// "false", not "0": the configuration parser only accepts the boolean spelling
// it emits, so re-arming with a numeric literal would break the next reload.
void rearm_initialize_control() {
    if (!synth::ui::write_setting("Tools", "InitializeSYNTH", "false")) {
        present_notification("SYNTHVR could not reset the initialization control");
        return;
    }
    settings_revision = synth::config::inspect_file_revision(synth::ui::settings_override_path());
    settings_revision_initialized = true;
}

// Drain a bounded diagnostic batch without creating any in-game notifications.
void log_conversation_trace() noexcept {
    if (!session) return;
    try {
        for (unsigned i = 0; i < 8; ++i) {
            const auto record = session->take_conversation_trace();
            if (!record) break;
            spdlog::info("SYNTH conversation: ms={} gen={} context={} request={} line={} event={} dropped={}",
                record->milliseconds,record->generation,record->context_sequence,record->request_id,record->line_id,
                synth::diagnostics::stage_name(record->stage),session->dropped_conversation_traces());
        }
    } catch (...) {}
}

// Retire voice/network workers on the shared coordinator, never during a VR frame or save load.
void invalidate_sessions() noexcept {
    synth::integration::external_requests.set_available(false);
    pending_session_request = 0;
    runtime_disabled = false;
    if (session_host) session_host->invalidate();
    if (dispatcher) dispatcher->discard_pending();
    if (session) {
        session->request_stop();
        log_conversation_trace();
        session_host->retire(std::move(session));
    }
}

void initialize_session(const std::shared_ptr<const synth::core::RuntimeSnapshot>& snapshot) {
    if (snapshot && session_host && !pending_session_request && !runtime_disabled)
        pending_session_request = session_host->request_initialize(snapshot, *dispatcher);
}

void adopt_session_result(synth::core::SnapshotClock::time_point now) {
    if (!session_host) return;
    auto result = session_host->take_result();
    if (!result) return;
    if (result->request_id != pending_session_request ||
        lifecycle.state() != synth::adapters::LifecycleState::ready) {
        session_host->retire(std::move(result->session));
        return;
    }
    pending_session_request = 0;
    runtime_disabled = result->runtime_disabled;
    if (!result->error.empty()) {
        present_notification("SYNTHVR configuration: " + result->error);
        runtime_ready_after = now + std::chrono::seconds{2};
    }
    session = std::move(result->session);
    if (!session) return;
    session->set_actor_enricher([](const auto& snapshot, std::uint32_t form_id) {
        return runtime ? runtime->enrich_actor_snapshot(snapshot, form_id) : synth::client::PluginSession::Snapshot{};
    });
    next_profile_refresh = now + std::chrono::minutes{session->configuration().behavior.dynamic_profile_minutes};
    next_context_publish = now + std::chrono::seconds{2};
    profile_timer_paused_at = {};
    if (session->configuration().tools.initialize_requested) {
        rearm_initialize_control();
        present_notification("SYNTHVR initialization requested");
    }
}

// Recreates only SYNTHVR's in-memory session when the authoritative settings
// override changes. Fallout 4 VR has no F4SE Menu Framework, so a hand-edited
// SYNTH_custom.ini or external tooling is the VR editing surface.
void reload_settings_if_changed(synth::core::SnapshotClock::time_point now) {
    if (!session_host) return;
    const auto observed = session_host->take_settings_revision();
    if (!observed) return;
    const auto revision = *observed;
    if (!settings_revision_initialized) {
        settings_revision = revision;
        settings_revision_initialized = true;
        return;
    }
    if (revision == settings_revision) return;
    settings_revision = revision;
    invalidate_sessions();
    (void)runtime->invalidate();
    if (runtime) face_animation.reset(*runtime);
    voice_capture_down = false;
    pip_vision_gesture.reset();
    initialize_session(runtime->capture_snapshot(
        now, synth::runtime::RuntimeCapturePurpose::bootstrap));
    present_notification("SYNTHVR settings reloaded");
}

void pump_runtime_impl() {
    queue_runtime_pump();
    if (runtime == nullptr || dispatcher == nullptr) {
        return;
    }
    apply_lifecycle(deferred_lifecycle.take());
    if (lifecycle.state() != synth::adapters::LifecycleState::ready) {
        synth::integration::external_requests.set_available(false);
        return;
    }
    const auto now = synth::core::SnapshotClock::now();
    if (now < runtime_ready_after || !synth::adapters::commonlib::world_ready_for_capture()) {
        synth::integration::external_requests.set_available(false);
        return;
    }
    (void)dispatcher->drain();
    const auto menu_mode = synth::adapters::commonlib::menu_mode_active();
    const auto player_in_combat = synth::adapters::commonlib::player_in_combat();
    if (!session && menu_mode) {
        runtime_ready_after = now + std::chrono::seconds{2};
        return;
    }
    reload_settings_if_changed(now);
    adopt_session_result(now);
    if (session && session->initialization_failed()) {
        session->drain_notifications(present_notification);
        invalidate_sessions();
        (void)runtime->invalidate();
        runtime_ready_after = now + std::chrono::seconds{5};
        return;
    }
    if (session) {
        const auto& configuration = session->configuration();
        const auto runtime_paused = configuration.behavior.pause_dialogue_in_menus && menu_mode;
        session->set_runtime_paused(runtime_paused);
        if (runtime_paused && profile_timer_paused_at.time_since_epoch().count() == 0) {
            profile_timer_paused_at = now;
        } else if (!runtime_paused && profile_timer_paused_at.time_since_epoch().count() != 0) {
            next_profile_refresh += now - profile_timer_paused_at;
            profile_timer_paused_at = {};
        }
        if (player_in_combat && !player_was_in_combat &&
            configuration.behavior.cancel_dialogue_on_combat) {
            session->interrupt_dialogue_for_combat();
            voice_capture_down = false;
        }
        player_was_in_combat = player_in_combat;
        if (const auto caption = session->presentation_capture_candidate(now)) {
            const auto* owner = session.get();
            const auto generation = runtime->generation();
            const auto& actors = caption->admission->actors;
            auto snapshot = actors.empty() ? runtime->capture_snapshot(now)
                : runtime->capture_actor_snapshot(now,actors.front().form_id);
            if (session.get() != owner || runtime->generation() != generation) return;
            session->submit_presentation_capture(caption,snapshot);
        }
        session->pump(runtime->generation().value());
        log_conversation_trace();
        if (const auto target=session->rechat_capture_target()) {
            const auto* owner=session.get();
            const auto generation=runtime->generation();
            auto snapshot=runtime->capture_actor_snapshot(now,target->form_id);
            if (session.get()!=owner || runtime->generation()!=generation) return;
            session->submit_rechat_capture(std::move(snapshot),*target);
        }
        const auto playback_frame = session->playback_frame();
        // Advance the shared fallback reading window; VR retains its existing notification display.
        (void)session->subtitle_frame(runtime->generation().value(), playback_frame,
                                      runtime_paused || menu_mode, now);
        if (const auto result = speech_facing.pump(playback_frame ? playback_frame->facing : nullptr, *runtime)) {
            const auto& facing = *playback_frame->facing;
            spdlog::info("SYNTHVR facing: request={} utterance={} speaker={:08X} listener={:08X}: {}",
                facing.request_id, facing.utterance_id, facing.speaker.form_id, facing.listener.form_id, result->detail);
        }
        face_animation.pump(playback_frame,
                            configuration.audio.lip_animation_resolution,
                            configuration.audio.lip_animation_intensity,
                            now, *runtime);
        session->drain_notifications(present_notification);
        if (session->halt_complete()) {
            invalidate_sessions();
            if (runtime) face_animation.reset(*runtime);
            voice_capture_down = false;
            pip_vision_gesture.reset();
            return;
        }
        if (session->halting()) {
            synth::integration::external_requests.set_available(false);
            return;
        }
    }
    try {
        if (!session) {
            if (pending_session_request || runtime_disabled) return;
            spdlog::info("SYNTHVR runtime: beginning first stable world capture");
            auto snapshot = runtime->capture_snapshot(
                now, synth::runtime::RuntimeCapturePurpose::bootstrap);
            spdlog::info("SYNTHVR runtime: first stable world capture complete");
            initialize_session(snapshot);
            spdlog::info("SYNTHVR runtime: session initialization complete");
            player_was_in_combat = player_in_combat;
            next_context_publish = now + std::chrono::seconds{2};
            return;
        }
        const auto capture_batch = runtime->capture_batch();
        const auto& configuration = session->configuration();
        const auto regular_dialogue_blocked = session->runtime_paused() ||
            (player_in_combat && !configuration.behavior.enable_combat_dialogue);
        const auto* external_owner = session.get();
        const auto external_generation = runtime->generation();
        if (const auto external=synth::adapters::commonlib::pump_external_requests(
                *runtime,*session,!menu_mode && !regular_dialogue_blocked)) {
            spdlog::info("SYNTH external API: kind {}, actor {:08X}, admitted {}",external->kind,external->form_id,external->admitted);
        }
        if (session.get() != external_owner || runtime->generation() != external_generation) return;
        if (!session->runtime_paused() && now >= next_profile_refresh) {
            const auto* owner = session.get();
            const auto generation = runtime->generation();
            auto snapshot = runtime->capture_snapshot(now);
            if (session.get() != owner || runtime->generation() != generation) return;
            (void)session->refresh_dynamic_profiles(std::move(snapshot));
            next_profile_refresh = now +
                std::chrono::minutes{configuration.behavior.dynamic_profile_minutes};
        }
        if (!regular_dialogue_blocked && session->diary_checkpoint_needed()) {
            const auto* owner = session.get();
            const auto generation = runtime->generation();
            auto snapshot = runtime->capture_snapshot(now);
            if (session.get() != owner || runtime->generation() != generation) return;
            (void)session->checkpoint_diary(std::move(snapshot));
        }
        // Capture only speech boundaries, retaining the admitted actor even after looking away.
        const auto capture_voice_scene = [&](bool retain_actor) -> synth::client::PluginSession::Snapshot {
            const auto* owner = session.get();
            const auto generation = runtime->generation();
            const auto actor = retain_actor ? session->voice_target_form_id() : 0;
            auto snapshot = actor ? runtime->capture_actor_snapshot(now, actor)
                                  : runtime->capture_snapshot(now, synth::runtime::RuntimeCapturePurpose::dialogue);
            if (session.get() != owner || runtime->generation() != generation) return {};
            if (!snapshot) {
                session->cancel_voice_capture();
                voice_capture_down = false;
            }
            return snapshot;
        };
        const auto voice_held = configuration.input.push_to_talk_enabled &&
                                !regular_dialogue_blocked &&
                                hotkey_held(configuration.hotkeys.toggle_voice);
        if (voice_held && !voice_capture_down) {
            auto snapshot = capture_voice_scene(false);
            if (!snapshot) return;
            (void)session->begin_voice_capture(snapshot);
        } else if (!voice_held && voice_capture_down) {
            auto snapshot = capture_voice_scene(true);
            if (!snapshot) return;
            (void)session->end_voice_capture(std::move(snapshot));
        }
        voice_capture_down = voice_held;
        if (!regular_dialogue_blocked && !voice_capture_down &&
            configuration.input.open_microphone) {
            const auto event = session->pump_open_microphone(now);
            using MicrophoneEvent = synth::client::PluginSession::MicrophoneEvent;
            if (event != MicrophoneEvent::none) {
                auto snapshot = capture_voice_scene(event == MicrophoneEvent::speech_finished);
                if (!snapshot) return;
                if (event == MicrophoneEvent::speech_started) session->bind_voice_target(snapshot);
                else (void)session->end_voice_capture(std::move(snapshot));
            }
        }
        if (hotkey_pressed(configuration.hotkeys.open_mic_mute)) {
            session->toggle_open_mic_mute();
        }
        if (!regular_dialogue_blocked && hotkey_pressed(configuration.hotkeys.talk_to_npc)) {
            auto text = synth::client::PluginSession::clipboard_text();
            if (text) {
                const auto* owner = session.get();
                const auto generation = runtime->generation();
                auto snapshot = runtime->capture_snapshot(now);
                if (session.get() != owner || runtime->generation() != generation) return;
                (void)session->submit_text(std::move(snapshot), std::move(*text));
            } else {
                present_notification("SYNTHVR: copy 1-4096 characters to the clipboard first");
            }
        }
        if (hotkey_pressed(configuration.hotkeys.synth_control)) {
            session->open_control_panel();
        }
        if (session && hotkey_pressed(configuration.hotkeys.hard_halt)) {
            session->hard_halt();
            voice_capture_down = false;
            pip_vision_gesture.reset();
            return;
        }
        const auto pip_vision = pip_vision_gesture.update(
            hotkey_held(configuration.hotkeys.pip_vision), now);
        if (!regular_dialogue_blocked && pip_vision != synth::input::PressGestureEvent::none) {
            const auto mode = pip_vision == synth::input::PressGestureEvent::double_tap
                                  ? "npc_portrait"
                                  : pip_vision == synth::input::PressGestureEvent::hold
                                        ? "describe"
                                        : "store_only";
            const auto* owner = session.get();
            const auto generation = runtime->generation();
            auto snapshot = runtime->capture_snapshot(now);
            if (session.get() != owner || runtime->generation() != generation) return;
            (void)session->submit_visual_capture(std::move(snapshot), mode);
        }
        const auto manual = session && !regular_dialogue_blocked &&
                            hotkey_pressed(configuration.hotkeys.manual_activate);
        const auto periodic = session && !session->runtime_paused() && now >= next_context_publish;
        const auto automatic = periodic && !regular_dialogue_blocked &&
                               configuration.activation.enabled;
        if (manual || periodic) {
            const auto* owner = session.get();
            const auto generation = runtime->generation();
            auto snapshot = runtime->capture_snapshot(
                now, manual ? synth::runtime::RuntimeCapturePurpose::dialogue
                            : synth::runtime::RuntimeCapturePurpose::background);
            if (session.get() != owner || runtime->generation() != generation) return;
            const auto combat_bark = periodic && !manual &&
                                     session->maybe_trigger_combat_bark(snapshot, now);
            const auto bored = periodic && !regular_dialogue_blocked && !manual && !combat_bark &&
                               session->maybe_trigger_bored(snapshot, now);
            if (manual || (automatic && !combat_bark && !bored)) {
                (void)session->activate_target(snapshot, manual);
            }
            if (periodic && !manual) (void)session->observe_world_context(std::move(snapshot));
            next_context_publish = now + std::chrono::seconds{2};
        }
    } catch (const std::exception& error) {
        present_notification(std::string{"SYNTHVR runtime: "} + error.what());
        synth::integration::external_requests.set_available(false);
        next_context_publish = now + std::chrono::seconds{2};
    }
}

// Prevents exceptions from escaping the F4SE-owned one-shot task callback.
void pump_runtime() noexcept {
    try {
        pump_runtime_impl();
    } catch (const std::exception& error) {
        spdlog::warn("SYNTHVR runtime pump failed: {}", error.what());
        synth::integration::external_requests.set_available(false);
    } catch (...) {
        spdlog::warn("SYNTHVR runtime pump failed with an unknown exception");
        synth::integration::external_requests.set_available(false);
    }
}

void apply_lifecycle(LifecycleAction action) {
    if (runtime == nullptr) {
        return;
    }
    switch (action) {
    case LifecycleAction::ready:
        runtime_ready_after = synth::core::SnapshotClock::now() + std::chrono::seconds{2};
        break;
    case LifecycleAction::invalidate:
        invalidate_sessions();
        if (runtime) face_animation.reset(*runtime);
        pip_vision_gesture.reset();
        profile_timer_paused_at = {};
        runtime_ready_after = synth::core::SnapshotClock::time_point::max();
        (void)runtime->invalidate();
        break;
    case LifecycleAction::invalidate_and_ready:
        invalidate_sessions();
        if (runtime) face_animation.reset(*runtime);
        pip_vision_gesture.reset();
        profile_timer_paused_at = {};
        runtime_ready_after = synth::core::SnapshotClock::now() + std::chrono::seconds{2};
        (void)runtime->invalidate();
        break;
    case LifecycleAction::stop:
        if (dispatcher != nullptr) {
            dispatcher->stop_accepting(true);
        }
        invalidate_sessions();
        if (runtime) face_animation.reset(*runtime);
        pip_vision_gesture.reset();
        profile_timer_paused_at = {};
        runtime_ready_after = synth::core::SnapshotClock::time_point::max();
        (void)runtime->invalidate();
        break;
    case LifecycleAction::none:
    case LifecycleAction::rejected:
        break;
    }
}

void on_f4se_message(F4SE::MessagingInterface::Message* message) noexcept {
    if (message == nullptr) {
        return;
    }
    LifecycleAction action{LifecycleAction::none};
    switch (message->type) {
    case F4SE::MessagingInterface::kPreLoadGame:
        (void)synth::adapters::commonlib::save_context_store.reset();
        action = lifecycle.handle(synth::adapters::LifecycleEvent::pre_load_game);
        break;
    case F4SE::MessagingInterface::kPostLoadGame:
        if (message->data == nullptr) {
            (void)synth::adapters::commonlib::save_context_store.reset();
            return;
        }
        action = lifecycle.handle(synth::adapters::LifecycleEvent::post_load_game);
        break;
    case F4SE::MessagingInterface::kNewGame:
        (void)synth::adapters::commonlib::save_context_store.reset();
        action = lifecycle.handle(synth::adapters::LifecycleEvent::new_game);
        break;
    case F4SE::MessagingInterface::kGameDataReady:
        action = lifecycle.handle(synth::adapters::LifecycleEvent::game_data_ready);
        break;
    default:
        return;
    }
    if (action==LifecycleAction::invalidate || action==LifecycleAction::invalidate_and_ready || action==LifecycleAction::stop)
        synth::integration::external_requests.set_available(false);
    deferred_lifecycle.push(action);
}

}  // namespace

F4SE_EXPORT constinit auto F4SEPlugin_Version = []() noexcept {
    F4SE::PluginVersionData version{};
    version.PluginVersion({0, 1, 0, 0});
    version.PluginName("SYNTHVR");
    version.AuthorName("SYNTH contributors");
    version.UsesAddressLibrary(true);
    version.UsesSigScanning(false);
    version.IsLayoutDependent(true);
    version.HasNoStructUse(false);
    version.CompatibleVersions({F4SE::RUNTIME_VR_1_2_72});
    version.MinimumRequiredXSEVersion({0, 6, 21, 0});
    return version;
}();

F4SE_EXPORT bool F4SEPlugin_Query(const F4SE::QueryInterface* query, F4SE::PluginInfo* info) {
    if (info == nullptr) {
        return false;
    }
    info->infoVersion = F4SE::PluginInfo::kVersion;
    info->name = "SYNTHVR";
    info->version = REL::Version{0, 1, 0, 0}.pack();
    return environment_supported(query);
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* load) {
    if (!environment_supported(load) ||
        !lifecycle.load(synth::adapters::vr_descriptor,
                        synth::core::RuntimeVariant::vr,
                        version_of(load->RuntimeVersion()),
                        version_of(load->F4SEVersion()))) {
        return false;
    }

    F4SE::Init(load);
    if (!synth::adapters::commonlib::register_save_context()) return false;
    if (!synth::adapters::commonlib::register_papyrus_api())
        spdlog::warn("SYNTH Papyrus registration unavailable; native dialogue remains enabled");
    runtime = std::make_unique<synth::adapters::VrFalloutRuntime>(
        capture_values, present_notification, execute_runtime_action);
    runtime->set_actor_detail_pump(synth::adapters::commonlib::selected_actor_details);
    runtime->set_requested_actor_pump(synth::adapters::commonlib::requested_actor_snapshot);
    runtime->set_facing_pump(synth::adapters::commonlib::face_speech_listener);
    spdlog::warn("SYNTH lipsync unavailable: VR facial ABI has not been independently verified; audio remains enabled");
    dispatcher = std::make_unique<synth::runtime::GameThreadDispatcher>(*runtime, 256);

    const auto* messaging = F4SE::GetMessagingInterface();
    const auto* tasks = F4SE::GetTaskInterface();
    if (messaging == nullptr || messaging->Version() != F4SE::MessagingInterface::kVersion ||
        tasks == nullptr || tasks->Version() < F4SE::TaskInterface::kVersion ||
        !messaging->RegisterListener(on_f4se_message)) {
        apply_lifecycle(lifecycle.handle(synth::adapters::LifecycleEvent::shutdown));
        return false;
    }
    try {
        session_host = std::make_unique<synth::client::PluginSessionHost>(
            synth::core::RuntimeVariant::vr, "1.2.72", [](const synth::client::RequestOutcome& outcome) {
                if (outcome.status == synth::client::RequestStatus::complete)
                    spdlog::info("SYNTHVR retired server session: {} acknowledged", outcome.request_id);
                else
                    spdlog::warn("SYNTHVR retired locally; server Halt {} not acknowledged (status {})",
                                 outcome.request_id, static_cast<int>(outcome.status));
            }, &synth::adapters::commonlib::save_context_store);
    } catch (...) {
        apply_lifecycle(lifecycle.handle(synth::adapters::LifecycleEvent::shutdown));
        return false;
    }
    tasks->AddTask(pump_runtime);
    return lifecycle.handle(synth::adapters::LifecycleEvent::plugin_loaded) !=
           LifecycleAction::rejected;
}
#endif
