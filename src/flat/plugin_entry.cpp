#include "adapters/adapter_descriptor.hpp"
#include "adapters/adapter_lifecycle.hpp"

#if defined(SYNTH_WITH_F4SE) && defined(SYNTH_WITH_F4SEVR)
#error "flat and VR ABI macros are mutually exclusive"
#endif

#if defined(SYNTH_WITH_F4SE)
#include "adapters/flat_fallout_runtime.hpp"
#include "adapters/flat_dialogue_subtitles.hpp"
#include "adapters/flat_rest_events.hpp"
#include "adapters/flat_quest_events.hpp" // Compile the guarded observer; activation waits for owned event transport.
#include "adapters/flat_actor_events.hpp"
#include "adapters/flat_inventory_changes.hpp"
#include "adapters/flat_pickup_package.hpp" // Compile preparation only; pickup installation/dispatch remains disabled.
#include "adapters/flat_pickup_inspection.hpp" // Read-only preparation; not a pickup capability.
#include "adapters/flat_actor_event_capture.hpp"
#include "adapters/commonlib_actions.hpp"
#include "adapters/commonlib_capture.hpp"
#include "adapters/commonlib_external_requests.hpp"
#include "adapters/commonlib_papyrus_api.hpp"
#include "adapters/commonlib_save_context.hpp"
#include "runtime/game_thread_dispatcher.hpp"
#include "client/plugin_session_host.hpp"
#include "core/player_inventory_refresh.hpp"
#include "input/press_gesture.hpp"
#include "presentation/commonlib_face_animation.hpp"
#include "adapters/flat_speech_animation.hpp"
#include "adapters/commonlib_speech_facing.hpp"
#include "adapters/flat_menu_pause.hpp"
#include "adapters/native_fault_recorder.hpp"
#include "ui/menu_framework_ui.hpp"
#include "ui/settings_store.hpp"

#include <F4SE/F4SE.h>

#include <atomic>
#include <chrono>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

namespace {

using synth::adapters::LifecycleAction;

synth::adapters::AdapterLifecycle lifecycle;
synth::adapters::DeferredLifecycleActions deferred_lifecycle;
std::unique_ptr<synth::adapters::FlatFalloutRuntime> runtime;
std::unique_ptr<synth::runtime::GameThreadDispatcher> dispatcher;
std::unique_ptr<synth::client::PluginSession> session;
std::unique_ptr<synth::client::PluginSessionHost> session_host;
std::unique_ptr<synth::client::ActorEventSceneBinder> actor_event_binder;
std::uint64_t actor_event_epoch{};
std::uint64_t actor_event_binding_losses{};
synth::core::PlayerInventoryRefresh player_inventory_refresh;
std::uint64_t inventory_change_revision{};
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
synth::ui::ConfigSnapshot local_configuration;
bool configuration_reconnect_pending{};
std::optional<synth::integration::PromptTarget> control_target;
std::optional<synth::ui::ControlRequest> deferred_control_action;
std::uint64_t control_status_revision{};
bool trace_first_background_cycle{};
synth::input::PressGesture pip_vision_gesture;
synth::presentation::CommonlibFaceAnimation face_animation;
synth::presentation::SpeechFacingOnce speech_facing;
synth::adapters::FlatDialogueSubtitles dialogue_subtitles;

void pump_runtime() noexcept;

// Hook the only 1.11.240 call to Main::Update. This executes in Fallout's
// ordinary engine-update phase without relying on migrating F4SE task queues
// or re-entrant Windows message dispatch.
class EngineMainUpdatePump final {
public:
    [[nodiscard]] static bool install() noexcept {
        if (installed_) return true;
        const auto executable = REX::FModule::GetExecutingModule();
        if (executable.GetFileVersion().pack() != supported_runtime_.pack()) {
            REX::WARN("SYNTH runtime: engine-update pump requires Fallout 4 1.11.240");
            return false;
        }

        const auto call_site = executable.GetBaseAddress() + main_update_call_site_rva_;
        const auto main_update = REL::ID(main_update_id_).address();
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(call_site);
        if (bytes[0] != 0xE8) {
            REX::WARN("SYNTH runtime: engine-update hook site is not a CALL (0x{:02x})",
                      bytes[0]);
            return false;
        }

        std::int32_t displacement{};
        std::memcpy(&displacement, bytes + 1, sizeof(displacement));
        const auto existing_target =
            call_site + 5 + static_cast<std::int64_t>(displacement);
        if (existing_target != main_update) {
            REX::WARN(
                "SYNTH runtime: engine-update hook target 0x{:x} != Main::Update 0x{:x}",
                existing_target, main_update);
            return false;
        }

        original_update_ = reinterpret_cast<MainUpdateFn>(
            REL::GetTrampoline().write_call<5>(call_site, thunk));
        installed_ = original_update_ != nullptr;
        if (!installed_) {
            REX::WARN("SYNTH runtime: failed to install engine-update pump");
            return false;
        }
        REX::INFO(
            "SYNTH runtime: engine-update pump installed at RVA 0x{:x}; Main::Update 0x{:x}",
            main_update_call_site_rva_, main_update);
        return true;
    }

    static void request_stop() noexcept { stopping_.store(true, std::memory_order_release); }

private:
    using MainUpdateFn = void (*)(void*);

    static void thunk(void* argument) noexcept {
        // Let Fallout finish its own update first. Deep reads before Main::Update
        // returned successfully but left the loaded-save frame hung inside the
        // original update on 1.11.240.
        synth::adapters::NativeFaultRecorder::phase("engine Main::Update");
        original_update_(argument);

        const auto current_thread_id = GetCurrentThreadId();
        const auto* engine = RE::Main::GetSingleton();
        if (!stopping_.load(std::memory_order_acquire) && engine != nullptr &&
            engine->threadID != 0 && current_thread_id == engine->threadID) {
            if (!first_entry_logged_) {
                REX::INFO("SYNTH runtime: entered Fallout engine-update pump on thread {}",
                          current_thread_id);
                first_entry_logged_ = true;
            }
            if (!callback_active_) {
                callback_active_ = true;
                pump_runtime();
                callback_active_ = false;
            }
        } else if (!stopping_.load(std::memory_order_acquire) && !wrong_thread_reported_) {
            const auto expected_thread_id = engine == nullptr ? 0 : engine->threadID;
            REX::WARN("SYNTH runtime: rejected engine-update pump thread {}; expected {}",
                      current_thread_id, expected_thread_id);
            wrong_thread_reported_ = true;
        }
    }

    static inline constexpr REL::Version supported_runtime_{1, 11, 240, 0};
    static inline constexpr std::uint64_t main_update_id_{2228917};
    static inline constexpr std::uintptr_t main_update_call_site_rva_{0x00C315BB};
    static inline MainUpdateFn original_update_{};
    static inline std::atomic_bool stopping_{};
    static inline bool installed_{};
    static inline bool first_entry_logged_{};
    static inline bool wrong_thread_reported_{};
    static inline bool callback_active_{};
};

void apply_lifecycle(LifecycleAction action);

[[nodiscard]] synth::adapters::Version version_of(const REL::Version& version) noexcept {
    return {version.major(), version.minor(), version.patch(), version.build()};
}

[[nodiscard]] bool environment_supported(const F4SE::QueryInterface* query) noexcept {
    return query != nullptr && !query->IsEditor() &&
           synth::adapters::supports_exact_environment(
               synth::adapters::flat_descriptor,
               synth::core::RuntimeVariant::flat,
               version_of(query->RuntimeVersion()),
               version_of(query->F4SEVersion()));
}

[[nodiscard]] synth::adapters::detail::CapturedRuntimeValues capture_values(
    synth::core::SnapshotClock::time_point now,
    synth::runtime::RuntimeCapturePurpose purpose) {
    return synth::adapters::commonlib::capture_flat(now, purpose);
}

void present_notification(std::string_view message) {
    const std::string owned{message};
    REX::INFO("SYNTH notification: {}", owned);
    RE::SendHUDMessage::ShowHUDMessage(owned.c_str(), nullptr, false, false);
}

[[nodiscard]] synth::runtime::RuntimeActionResult execute_runtime_action(
    const synth::runtime::RuntimeActionRequest& request) {
    return synth::adapters::commonlib::execute_action(request);
}

// F4SE plugin load and rendering can use different threads on Fallout 4
// 1.11.240. Construct the runtime only inside Main::Update so its affinity
// guard records the engine main thread that owns Fallout state.
void bind_runtime_to_game_thread() {
    if (runtime != nullptr) return;
    runtime = std::make_unique<synth::adapters::FlatFalloutRuntime>(
        capture_values, present_notification, execute_runtime_action);
    runtime->set_actor_detail_pump(synth::adapters::commonlib::selected_actor_details);
    runtime->set_player_inventory_pump(synth::adapters::commonlib::selected_player_inventory);
    runtime->set_capture_epoch_reader([] { return synth::adapters::FlatPickedReference::stamp(); });
    runtime->set_requested_actor_pump(synth::adapters::commonlib::requested_actor_snapshot);
    runtime->set_facing_pump(synth::adapters::commonlib::face_speech_listener);
    runtime->set_speech_pump(synth::adapters::FlatSpeechAnimation::update);
    dispatcher = std::make_unique<synth::runtime::GameThreadDispatcher>(*runtime, 256);
    REX::INFO("SYNTH runtime: bound adapter to Fallout main thread");
}

// SYNTH persists DirectInput scan codes, which is also what the F4SE Menu
// Framework hotkey registry hands back. DIK marks extended keys with bit 0x80
// while Windows spells the same keys with an 0xE0 scan-code prefix, so restore
// that prefix before asking the keyboard layout for a virtual key.
[[nodiscard]] UINT dik_to_virtual_key(std::uint32_t scan_code) {
    if (scan_code == 0 || scan_code > 0xFF) return 0;
    const UINT windows_scan_code =
        (scan_code & 0x80U) != 0 ? 0xE000U | (scan_code & 0x7FU) : scan_code;
    return MapVirtualKeyW(windows_scan_code, MAPVK_VSC_TO_VK_EX);
}

// Reads as up while a framework window has input, so a key held inside the
// settings menu can never leak a press or a gesture into gameplay.
[[nodiscard]] bool hotkey_is_down(std::uint32_t scan_code) {
    if (synth::ui::MenuFramework::input_captured()) return false;
    const auto virtual_key = dik_to_virtual_key(scan_code);
    return virtual_key != 0 && (GetAsyncKeyState(static_cast<int>(virtual_key)) & 0x8000) != 0;
}

[[nodiscard]] bool hotkey_pressed(std::uint32_t scan_code) {
    if (scan_code == 0 || scan_code >= hotkey_down.size()) return false;
    if (synth::ui::MenuFramework::input_captured()) {
        const auto key = dik_to_virtual_key(scan_code);
        hotkey_down[scan_code] = key != 0 && (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
        return false;
    }
    const auto down = hotkey_is_down(scan_code);
    const auto pressed = down && !hotkey_down[scan_code];
    hotkey_down[scan_code] = down;
    return pressed;
}

[[nodiscard]] bool hotkey_held(std::uint32_t scan_code) {
    if (scan_code == 0 || scan_code >= hotkey_down.size()) return false;
    return hotkey_is_down(scan_code);
}

// "false", not "0": the configuration parser only accepts the boolean spelling
// it emits, so re-arming with a numeric literal would break the next reload.
void rearm_initialize_control() {
    if (!synth::ui::write_setting("Tools", "InitializeSYNTH", "false")) {
        present_notification("SYNTH could not reset the initialization control");
        return;
    }
    settings_revision = synth::config::inspect_file_revision(synth::ui::settings_override_path());
    settings_revision_initialized = true;
}

// Hands the settings pages an immutable copy of the authoritative configuration.
// The render thread never touches the live session.
void publish_ui_configuration() {
    if (!local_configuration && session)
        local_configuration = std::make_shared<const synth::config::Config>(session->configuration());
    synth::ui::MenuFramework::publish(local_configuration);
}

// Drain a bounded diagnostic batch; never send trace events to the in-game notification UI.
void log_conversation_trace() noexcept {
    if (!session) return;
    try {
        for (unsigned i = 0; i < 8; ++i) {
            const auto record = session->take_conversation_trace();
            if (!record) break;
            REX::INFO("SYNTH conversation: ms={} gen={} context={} request={} line={} event={} dropped={}",
                record->milliseconds,record->generation,record->context_sequence,record->request_id,record->line_id,
                synth::diagnostics::stage_name(record->stage),session->dropped_conversation_traces());
        }
    } catch (...) {}
}

// Session teardown cancels WinHTTP and joins task workers, so hand ownership to
// the coordinator instead of performing that work inside the main-thread pump.
void retire_session() noexcept {
    synth::ui::Chatbox::set_voice_session_available(false);
    synth::ui::Chatbox::close();
    synth::ui::ControlMenu::invalidate();
    control_target.reset();
    deferred_control_action.reset();
    control_status_revision = 0;
    synth::adapters::FlatMenuPause::sync(false);
    synth::adapters::FlatInventoryChanges::invalidate();
    inventory_change_revision = 0;
    player_inventory_refresh = {};
    synth::adapters::FlatActorEvents::invalidate();
    actor_event_binder.reset();actor_event_epoch=0;
    synth::adapters::FlatQuestEvents::invalidate();
    synth::adapters::FlatPickedReference::invalidate();
    synth::integration::external_requests.set_available(false);
    synth::adapters::FlatRestEvents::invalidate();
    if (!session) return;
    session->request_stop();
    log_conversation_trace();
    if (session_host) {
        session_host->retire(std::move(session));
    } else {
        session.reset();
    }
}

void invalidate_sessions() noexcept {
    synth::ui::Chatbox::set_voice_session_available(false);
    synth::adapters::commonlib::FlatWaitPackage::retire();
    synth::ui::Chatbox::close();
    synth::ui::ControlMenu::invalidate();
    (void)synth::ui::Chatbox::take_voice_action();
    control_target.reset();
    deferred_control_action.reset();
    control_status_revision = 0;
    synth::adapters::FlatQuestEvents::invalidate();
    synth::adapters::FlatPickedReference::invalidate();
    synth::integration::external_requests.set_available(false);
    synth::adapters::FlatRestEvents::invalidate();
    pending_session_request = 0;
    runtime_disabled = false;
    synth::adapters::commonlib::reset_flat_capture_cache();
    if (session_host) session_host->invalidate();
    // Clear at the load boundary, never from an old session's asynchronous
    // destructor after the replacement has started queuing new work.
    if (dispatcher) dispatcher->discard_pending();
    retire_session();
}

void queue_session_initialization(
    const std::shared_ptr<const synth::core::RuntimeSnapshot>& snapshot) {
    if (!snapshot || pending_session_request != 0 || runtime_disabled || !session_host ||
        !dispatcher) {
        return;
    }
    pending_session_request = session_host->request_initialize(snapshot, *dispatcher,
        synth::adapters::FlatRestEvents::available(), synth::adapters::FlatQuestEvents::available(),
        synth::adapters::FlatActorEvents::available());
    if (pending_session_request != 0) {
        REX::INFO("SYNTH runtime: session initialization queued off the game thread");
    }
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
        present_notification("SYNTH configuration: " + result->error);
        REX::WARN("SYNTH runtime: asynchronous session initialization failed: {}",
                  result->error);
        runtime_ready_after = now + std::chrono::seconds{2};
        return;
    }
    if (!result->session) {
        if (runtime_disabled) {
            REX::INFO("SYNTH runtime: disabled by configuration");
        }
        return;
    }

    session = std::move(result->session);
    synth::ui::Chatbox::set_voice_session_available(true);
    if (local_configuration) {
        auto effective = session->configuration();
        effective.hotkeys = local_configuration->hotkeys;
        configuration_reconnect_pending = effective != *local_configuration;
    }
    player_inventory_refresh.reset(runtime->generation().value(), now);
    session->set_actor_enricher([](const auto& snapshot, std::uint32_t form_id) {
        return runtime ? runtime->enrich_actor_snapshot(snapshot, form_id) : synth::client::PluginSession::Snapshot{};
    });
    next_profile_refresh = now +
        std::chrono::minutes{session->configuration().behavior.dynamic_profile_minutes};
    next_context_publish = now + std::chrono::seconds{2};
    profile_timer_paused_at = {};
    trace_first_background_cycle = true;
    if (session->configuration().tools.initialize_requested) {
        rearm_initialize_control();
        present_notification("SYNTH initialization requested");
    }
    publish_ui_configuration();
    (void)session->request_control();
    REX::INFO("SYNTH runtime: adopted initialized session from coordinator");
}

// Recreates only SYNTH's in-memory session when the authoritative settings
// override changes, whether that came from the in-game pages, a hand edit, or
// external tooling.
void reload_settings_if_changed(synth::core::SnapshotClock::time_point now, bool allow_reconnect = true) {
    if (!session_host) return;
    if (auto loaded = session_host->take_settings()) {
        if (!loaded->configuration) {
            REX::WARN("SYNTH configuration reload rejected: {}", loaded->error);
            return;
        }
        local_configuration = std::move(loaded->configuration);
        if (!session && runtime_disabled && local_configuration->runtime.enabled) runtime_disabled = false;
        if (session) {
            session->cancel_voice_capture();
            voice_capture_down = false;
            auto session_settings = session->configuration();
            // Hotkeys are game-thread-owned; active workers retain their immutable session config.
            session_settings.hotkeys = local_configuration->hotkeys;
            configuration_reconnect_pending = session_settings != *local_configuration;
        }
        publish_ui_configuration();
        // Treat every held key as already pressed until physically released after a rebind.
        hotkey_down.fill(true);
    }
    if (!allow_reconnect || !configuration_reconnect_pending || !session ||
        synth::ui::MenuFramework::input_captured() || synth::adapters::commonlib::menu_mode_active() || !session->external_ready()) return;
    configuration_reconnect_pending = false;
    face_animation.reset(*runtime, false); // Settings reconnect keeps the same world: release live mouth ownership first.
    invalidate_sessions();
    (void)runtime->invalidate();
    publish_ui_configuration();
    if (runtime) face_animation.reset(*runtime);
    voice_capture_down = false;
    pip_vision_gesture.reset();
    queue_session_initialization(runtime->capture_snapshot(
        now, synth::runtime::RuntimeCapturePurpose::bootstrap));
    present_notification("SYNTH settings reload queued");
}

// One-time phase markers across the initial 30-second gameplay startup window.
// Session readiness can arrive after the first frame; record each distinct phase
// once without producing a per-frame log stream.
enum class PumpTrace : unsigned char {
    idle,     // never armed
    armed,    // arm requested; the next pump starts tracing
    tracing,  // recording each distinct startup phase once
    done,     // startup observation window elapsed; log markers stay off
};
PumpTrace pump_trace{PumpTrace::idle};
int pump_trace_phases{};
std::array<const char*, 24> traced_pump_phases{};
auto pump_trace_deadline = synth::core::SnapshotClock::time_point{};

void trace_pump_phase(const char* phase) noexcept {
    synth::adapters::NativeFaultRecorder::phase(phase);
    if (pump_trace != PumpTrace::tracing) return;
    for (int i = 0; i < pump_trace_phases; ++i)
        if (std::string_view{traced_pump_phases[i]} == phase) return;
    if (pump_trace_phases == static_cast<int>(traced_pump_phases.size())) return;
    traced_pump_phases[pump_trace_phases] = phase;
    ++pump_trace_phases;
    REX::INFO("SYNTH runtime: startup first reached {}", phase);
}

// Stop log markers at the deadline; fault phase tracking remains active without logging.
void finish_pump_trace() noexcept {
    if (pump_trace != PumpTrace::tracing || pump_trace_phases == 0) return;
    if (synth::core::SnapshotClock::now() < pump_trace_deadline) return;
    REX::INFO("SYNTH runtime: startup observation complete after {} distinct phases; "
              "phase markers disabled",
              pump_trace_phases);
    pump_trace = PumpTrace::done;
}

void pump_runtime_impl() {
    if (pump_trace == PumpTrace::armed) {
        REX::INFO("SYNTH runtime: first post-session engine-update pump entered");
        pump_trace = PumpTrace::tracing;
        pump_trace_deadline = synth::core::SnapshotClock::now() + std::chrono::seconds{30};
    }
    synth::adapters::NativeFaultRecorder::phase("apply deferred lifecycle");
    bind_runtime_to_game_thread();
    apply_lifecycle(deferred_lifecycle.take());
    synth::adapters::FlatMenuPause::sync(lifecycle.state() == synth::adapters::LifecycleState::ready &&
        (synth::ui::Chatbox::is_open() || synth::ui::ControlMenu::is_open()));
    // Local options are readable even before a world/session is ready. No engine capture here.
    reload_settings_if_changed(synth::core::SnapshotClock::now(), false);
    if (lifecycle.state() != synth::adapters::LifecycleState::ready) {
        dialogue_subtitles.present({});
        synth::integration::external_requests.set_available(false);
        return;
    }
    const auto now = synth::core::SnapshotClock::now();
    if (now < runtime_ready_after || !synth::adapters::commonlib::world_ready_for_capture()) {
        dialogue_subtitles.present({});
        synth::adapters::FlatInventoryChanges::invalidate();
        synth::adapters::FlatActorEvents::invalidate();actor_event_binder.reset();actor_event_epoch=0;
        if (session) session->observe_player_events(std::nullopt);
        synth::integration::external_requests.set_available(false);
        return;
    }
    trace_pump_phase("dispatcher drain");
    (void)dispatcher->drain();
    trace_pump_phase("menu and combat query");
    const auto menu_mode = synth::adapters::commonlib::menu_mode_active();
    if (!session || menu_mode) dialogue_subtitles.present({});
    if (menu_mode) synth::adapters::FlatPickedReference::invalidate();
    else synth::adapters::FlatPickedReference::arm();
    const auto player_in_combat = synth::adapters::commonlib::player_in_combat();
    // kPostLoadGame can arrive while Fallout still owns a loading/menu transition.
    // Player, cell and camera pointers are already valid then, but deep capture and
    // network-session startup must wait until ordinary gameplay has remained active.
    // Re-arm the existing dwell deadline on every menu frame so one transient
    // menu-free update cannot start SYNTH in the middle of a load.
    if (!session && menu_mode) {
        runtime_ready_after = now + std::chrono::seconds{2};
        return;
    }
    trace_pump_phase("settings revision check");
    reload_settings_if_changed(now);
    trace_pump_phase("session adoption");
    adopt_session_result(now);
    if (session && session->initialization_failed()) {
        session->drain_notifications(present_notification);
        invalidate_sessions();
        (void)runtime->invalidate();
        publish_ui_configuration();
        runtime_ready_after = now + std::chrono::seconds{5};
        present_notification("SYNTH connection failed; retrying in 5 seconds");
        return;
    }
    if (session) {
        // Wait-controller VM work belongs to an initialized gameplay session,
        // never the first post-load frame before stable bootstrap/adoption.
        if (session->external_ready()) {
            trace_pump_phase("wait controller");
            synth::adapters::commonlib::FlatWaitPackage::tick(now);
        }
        const auto& configuration = session->configuration();
        const auto runtime_paused = synth::ui::Chatbox::is_open() || synth::ui::ControlMenu::is_open() ||
            (configuration.behavior.pause_dialogue_in_menus && menu_mode);
        if (synth::ui::Chatbox::take_stop_request()) session->stop_dialogue();
        session->set_composing(synth::ui::Chatbox::has_submission() || synth::ui::Chatbox::is_open());
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
        trace_pump_phase("session pump");
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
        trace_pump_phase("face animation");
        const auto playback_frame = session->playback_frame();
        const auto caption = session->subtitle_frame(
            runtime->generation().value(), playback_frame, runtime_paused || menu_mode, now);
        dialogue_subtitles.present(configuration.behavior.show_subtitles ? caption : nullptr);
        const auto facing_request = playback_frame ? playback_frame->facing : caption ? caption->facing : nullptr;
        if (const auto result = speech_facing.pump(facing_request, *runtime)) {
            const auto& facing = *facing_request;
            REX::INFO("SYNTH facing: request={} utterance={} speaker={:08X} listener={:08X}: {}",
                facing.request_id, facing.utterance_id, facing.speaker.form_id, facing.listener.form_id, result->detail);
        }
        face_animation.pump(playback_frame,
                            configuration.audio.lip_animation_resolution,
                            configuration.audio.lip_animation_intensity,
                            now, *runtime);
        session->drain_notifications(present_notification);
        if (session->halt_complete()) {
            retire_session();
            publish_ui_configuration();
            if (runtime) face_animation.reset(*runtime);
            voice_capture_down = false;
            pip_vision_gesture.reset();
            return;
        }
        if (session->halting()) {
            synth::adapters::FlatInventoryChanges::invalidate();
            synth::adapters::FlatActorEvents::invalidate();actor_event_binder.reset();actor_event_epoch=0;
            synth::adapters::FlatQuestEvents::invalidate();
            synth::integration::external_requests.set_available(false);
            return;
        }
        if (session->automatic_diary_ready() && synth::adapters::FlatRestEvents::available()) {
            synth::adapters::FlatRestEvents::arm();
        } else {
            synth::adapters::FlatRestEvents::invalidate();
        }
        if (session->quest_events_ready()) synth::adapters::FlatQuestEvents::arm();
        else synth::adapters::FlatQuestEvents::invalidate();
        if (!menu_mode) {
            static std::uint64_t last_dropped_total{};
            const auto dropped_total = synth::adapters::FlatRestEvents::dropped_total();
            if (dropped_total != last_dropped_total) {
                last_dropped_total = dropped_total;
                REX::WARN("SYNTH rest observation: process-wide buffer/contended callback drops {}; these are not rest events", dropped_total);
            }
        }
        switch (synth::ui::Chatbox::take_voice_action()) {
        case synth::ui::VoiceAction::import_all: session->import_voice_samples(); break;
        case synth::ui::VoiceAction::cancel: session->cancel_voice_samples(); break;
        default: break;
        }
        synth::ui::Chatbox::publish_voice_status(session->voice_sample_status());
    }
    try {
        if (!session) {
            if (pending_session_request != 0 || runtime_disabled) return;
            REX::INFO("SYNTH runtime: beginning stable menu-free bootstrap capture");
            auto snapshot = runtime->capture_snapshot(
                now, synth::runtime::RuntimeCapturePurpose::bootstrap);
            REX::INFO("SYNTH runtime: bootstrap capture complete");
            queue_session_initialization(snapshot);
            // Trace the next pump after the handoff that previously froze. The
            // coordinator now owns configuration I/O, worker startup and HTTP.
            if (pump_trace == PumpTrace::idle) pump_trace = PumpTrace::armed;
            player_was_in_combat = player_in_combat;
            return;
        }
        synth::adapters::FlatInventoryChanges::arm();
        if (const auto revision = synth::adapters::FlatInventoryChanges::revision();
            revision && *revision != inventory_change_revision) {
            inventory_change_revision = *revision;
            player_inventory_refresh.mark_dirty(runtime->generation().value(), now);
        }
        const auto capture_batch = runtime->capture_batch();
        trace_pump_phase("actor event activation");
        if (!session->actor_events_ready() || !synth::adapters::FlatActorEvents::available()) {
            synth::adapters::FlatActorEvents::invalidate();actor_event_binder.reset();actor_event_epoch=0;
        } else {
            if (!synth::adapters::FlatActorEvents::is_current(actor_event_epoch)) actor_event_binder.reset();
            if (!actor_event_binder) {
                const auto* owner_session=session.get();const auto generation=runtime->generation();
                const auto before_arm=synth::adapters::FlatActorEvents::stamp();
                auto arm_scene=runtime->capture_snapshot(synth::core::SnapshotClock::now(),synth::runtime::RuntimeCapturePurpose::actor_events);
                if (session.get()!=owner_session || runtime->generation()!=generation || !arm_scene ||
                    lifecycle.state()!=synth::adapters::LifecycleState::ready ||
                    synth::adapters::FlatActorEvents::stamp()!=before_arm || !synth::adapters::commonlib::world_ready_for_capture()) return;
                synth::adapters::FlatActorEvents::arm();actor_event_epoch=synth::adapters::FlatActorEvents::stamp();
                if (synth::adapters::FlatActorEvents::is_current(actor_event_epoch)) {
                    actor_event_binder=std::make_unique<synth::client::ActorEventSceneBinder>(
                        session->actor_event_session_id(),actor_event_epoch,std::move(arm_scene));
                    REX::INFO("SYNTH actor observation: armed owned epoch {}, generation {}",actor_event_epoch,generation.value());
                }
            }
            // Pip-Boy equipment changes remain observable; menu pause only defers delivery, not shallow source capture.
            if (actor_event_binder && session->actor_event_capture_ready()) {
                const auto events=synth::adapters::FlatActorEvents::take();
                if (events.size) {
                    const auto* owner_session=session.get();const auto generation=runtime->generation();
                    const auto capture_started=synth::core::SnapshotClock::now();
                    auto scene=runtime->capture_snapshot(capture_started,synth::runtime::RuntimeCapturePurpose::actor_events);
                    const auto records=std::span<const synth::core::ActorEvent>{events.events.data(),events.size};
                    scene=synth::adapters::commonlib::actor_event_scene(*runtime,std::move(scene),records);
                    const auto metadata=synth::adapters::commonlib::actor_event_metadata(*runtime,scene,records);
                    if (session.get()!=owner_session || runtime->generation()!=generation ||
                        !synth::adapters::FlatActorEvents::is_current(events.owner) || !actor_event_binder) return;
                    for (const auto& event : records) {
                        if (event.actor == 0x14 && (event.kind == synth::core::ActorEventKind::equipped ||
                            event.kind == synth::core::ActorEventKind::unequipped))
                            player_inventory_refresh.mark_dirty(generation.value(), now);
                    }
                    const auto bound=actor_event_binder->bind(session->actor_event_session_id(),events.owner,scene,records,metadata,
                        synth::core::SnapshotClock::now());
                    const auto accepted=bound && session->observe_actor_events(bound);
                    actor_event_binding_losses+=bound?bound->rejected:events.size;
                    REX::DEBUG("SYNTH actor observation: {} scalars, {} bound, {} rejected, admitted {}, {} ms, generation {}",
                        events.size,bound?bound->events.size():0,bound?bound->rejected:events.size,accepted,
                        std::chrono::duration_cast<std::chrono::milliseconds>(synth::core::SnapshotClock::now()-capture_started).count(),generation.value());
                }
            }
        }
        session->pump_actor_events();
        {
            static std::array<std::uint64_t,4> previous{};
            static auto next_health=synth::core::SnapshotClock::time_point{};
            const std::array current{synth::adapters::FlatActorEvents::dropped_total(),
                synth::adapters::FlatActorEvents::unresolved_total(),actor_event_binding_losses,session->dropped_actor_events()};
            if (current!=previous && now>=next_health) {
                previous=current;next_health=now+std::chrono::seconds{5};
                REX::WARN("SYNTH actor observation losses: native {}, unresolved {}, binding {}, delivery {}",current[0],current[1],current[2],current[3]);
            }
        }
        if (!menu_mode && session->player_event_sample_due(now)) {
            trace_pump_phase("player event sample");
            const auto* owner_session = session.get();
            const auto owner_generation = runtime->generation();
            const auto previous = session->player_event_health();
            const auto sample = synth::adapters::commonlib::player_event_sample(*runtime, now);
            if (session.get() == owner_session && runtime->generation() == owner_generation) {
                session->observe_player_events(sample);
                const auto current = session->player_event_health();
                if (current != previous)
                        REX::INFO("SYNTH player observation: {} retained transitions, {} dropped; acknowledged recent events may react",
                        current[0], current[1]);
            }
        }
        if (!menu_mode && session->quest_event_capture_ready()) {
            trace_pump_phase("quest event capture");
            const auto events = synth::adapters::FlatQuestEvents::take();
            if (events.size != 0) {
                const auto* owner_session = session.get();
                const auto owner_generation = runtime->generation();
                auto snapshot = runtime->capture_snapshot(now);
                // Capture can re-enter lifecycle callbacks: validate both the native epoch and session afterward.
                if (session.get() == owner_session && runtime->generation() == owner_generation &&
                    snapshot && snapshot->generation() == owner_generation &&
                    synth::adapters::FlatQuestEvents::is_current(events.owner)) {
                    const auto accepted = session->observe_quest_events(std::move(snapshot),
                        std::span<const synth::core::QuestEvent>{events.events.data(), events.size});
                    REX::DEBUG("SYNTH quest observation: {} scalars, capture admitted {}, generation {}",
                        events.size, accepted, owner_generation.value());
                }
            }
        }
        session->pump_quest_events();
        trace_pump_phase("control state");
        const auto controls = session->control_state();
        if (controls.revision == 0) (void)session->request_control();
        const auto& model_names = synth::ui::llm_models;
        const std::string model{model_names[std::clamp<std::uint64_t>(
            controls.selection.model_slot, 1, model_names.size()) - 1].value};
        if (controls.revision != control_status_revision && !controls.pending) {
            synth::ui::ControlMenu::publish_status(controls.selection.mode, model, controls.status);
            control_status_revision = controls.revision;
        }
        if (const auto request = synth::ui::ControlMenu::take_request()) {
            if (request->generation != runtime->generation().value()) {
                synth::ui::ControlMenu::invalidate();
            } else if (request->action == synth::ui::ControlAction::chat_mode || request->action == synth::ui::ControlAction::model_slot) {
                const bool mode_change = request->action == synth::ui::ControlAction::chat_mode;
                std::string value = request->value;
                if (!mode_change) {
                    const auto found = std::ranges::find(model_names, value, &synth::ui::ControlOption::value);
                    value = found == std::end(model_names) ? "" : std::to_string(found - std::begin(model_names) + 1);
                }
                if (value.empty() || !session->request_control(mode_change ? "mode" : "model_slot", value))
                    synth::ui::ControlMenu::publish_status(controls.selection.mode, model, "Server settings are busy or unavailable; try again");
            } else if (request->action == synth::ui::ControlAction::open_dashboard) {
                session->open_control_panel();
                synth::ui::ControlMenu::publish_status(controls.selection.mode, model, "Server dashboard opened in your browser");
            } else if (!deferred_control_action) {
                deferred_control_action = request;
                synth::ui::ControlMenu::publish_status(controls.selection.mode, model,
                    request->action == synth::ui::ControlAction::wait_here || request->action == synth::ui::ControlAction::release_wait
                    ? "NPC wait action queued" : "Close the control menu to apply this action");
            } else {
                synth::ui::ControlMenu::publish_status(controls.selection.mode, model, "An action is already queued; close this menu first");
            }
        }
        if (deferred_control_action && !menu_mode && !synth::ui::ControlMenu::is_open()) {
            const auto request = std::exchange(deferred_control_action, std::nullopt);
            const auto* owner = session.get();
            const auto generation = runtime->generation();
            const auto target = control_target;
            const auto waiting = request->action == synth::ui::ControlAction::wait_here ||
                request->action == synth::ui::ControlAction::release_wait;
            const auto targeted = waiting ||
                request->action == synth::ui::ControlAction::profile_target;
            auto snapshot = targeted && target ? runtime->capture_actor_snapshot(now, target->form_id) : runtime->capture_snapshot(now);
            if (session.get() != owner || runtime->generation() != generation || request->generation != generation.value()) return;
            const synth::core::ActorSnapshot* actor{};
            if (target && snapshot && !target->session.is_cancelled() && target->session.generation() == generation) {
                for (const auto& candidate : snapshot->actors()) {
                    if (candidate.form_id() == target->form_id && candidate.base_form_id() == target->base_form_id &&
                        candidate.origin_plugin() == target->origin_plugin && candidate.base_origin_plugin() == target->base_origin_plugin &&
                        candidate.playthrough_id() == target->playthrough_id && candidate.alive() && !candidate.disabled() &&
                        (waiting || (synth::targeting::dialogue_actor_available(candidate) &&
                        !candidate.in_combat() && !candidate.hostile_to_player()))) { actor = &candidate; break; }
                }
            }
            if (targeted && !actor) present_notification("SYNTH control target is no longer available; no action performed");
            else if (waiting) {
                const auto result = runtime->execute_action({request->action == synth::ui::ControlAction::release_wait
                    ? synth::runtime::RuntimeActionName::release_wait : synth::runtime::RuntimeActionName::wait_here,
                    {target->form_id, target->origin_plugin, target->playthrough_id}, {}, target->session,
                    now + std::chrono::seconds{2}, "control-wait"});
                present_notification("SYNTH: " + result.detail);
            } else {
                const auto scope = request->action == synth::ui::ControlAction::profile_target ? "target" :
                    request->action == synth::ui::ControlAction::profile_narrator ? "narrator" : "nearby";
                const auto accepted = session->refresh_dynamic_profiles(snapshot, scope, actor ? actor->form_id() : 0);
                present_notification(accepted ? "SYNTH profile refresh queued" : "SYNTH profile refresh unavailable");
            }
        }
        if (!menu_mode) {
            static std::array<std::uint64_t, 3> previous{};
            static auto next_health = synth::core::SnapshotClock::time_point{};
            const std::array current{synth::adapters::FlatQuestEvents::dropped_total(),
                session->dropped_quest_events(), session->unobserved_quest_events()};
            if (current != previous && now >= next_health) {
                previous = current;
                next_health = now + std::chrono::seconds{5};
                REX::INFO("SYNTH quest observation: native pressure drops {}, session delivery drops {}, unobserved filtered {}",
                    current[0], current[1], current[2]);
            }
        }
        const auto& configuration = session->configuration();
        const auto& hotkeys = local_configuration ? local_configuration->hotkeys : configuration.hotkeys;
        const auto regular_dialogue_blocked = session->runtime_paused() ||
            (player_in_combat && !configuration.behavior.enable_combat_dialogue);
        const auto* external_owner = session.get();
        const auto external_generation = runtime->generation();
        if (const auto external=synth::adapters::commonlib::pump_external_requests(
                *runtime,*session,!menu_mode && !regular_dialogue_blocked,
                [](synth::integration::PromptTarget target) { return synth::ui::Chatbox::request_open(std::move(target)); })) {
            REX::INFO("SYNTH external API: kind {}, actor {:08X}, admitted {}",external->kind,external->form_id,external->admitted);
        }
        if (session.get() != external_owner || runtime->generation() != external_generation) return;
        if (!menu_mode && !regular_dialogue_blocked && session->automatic_diary_ready()) {
            const auto starts = synth::adapters::FlatRestEvents::take();
            if (starts.size != 0) {
                // Capture at this safe pump, never inside the engine callback or from a previous cached frame.
                const auto* owner = session.get();
                const auto generation = runtime->generation();
                auto snapshot = runtime->capture_snapshot(now);
                if (session.get() != owner || runtime->generation() != generation) return;
                const auto queued = session->queue_automatic_diaries(std::move(snapshot), starts);
                REX::INFO("SYNTH rest observation: {} starts, {} automatic diaries queued from safe-pump capture, generation {}",
                    starts.size, queued, runtime->generation().value());
            }
        }
        trace_pump_phase("chatbox drain");
        // Native menu hide is queued: leave Send pending until the engine has removed its pause.
        if (auto submission = menu_mode ? std::nullopt : synth::ui::Chatbox::take_submission()) {
            REX::INFO("SYNTH runtime: received chatbox message; checking dialogue gate");
            if (regular_dialogue_blocked) {
                present_notification("SYNTH text dialogue is unavailable while dialogue is paused");
            } else {
                REX::INFO("SYNTH runtime: beginning text dialogue capture");
                const auto* owner = session.get();
                const auto generation = runtime->generation();
                if (submission->target) {
                    const auto& target=*submission->target;
                    const auto valid=!target.session.is_cancelled() && target.session.generation()==runtime->generation();
                    const auto snapshot = valid ? runtime->capture_actor_snapshot(now,target.form_id) : nullptr;
                    if (session.get() != owner || runtime->generation() != generation) return;
                    if (!valid || !(target.manual_chat
                        ? session->submit_text(snapshot, std::move(submission->text), &target)
                        : session->submit_prompt(snapshot, target, std::move(submission->text))))
                        present_notification("SYNTH targeted prompt is no longer valid; no message was sent");
                } else {
                    auto snapshot = runtime->capture_snapshot(now);
                    if (session.get() != owner || runtime->generation() != generation) return;
                    (void)session->submit_text(std::move(snapshot), std::move(submission->text));
                }
                REX::INFO("SYNTH runtime: text dialogue submission returned");
            }
            session->set_composing(synth::ui::Chatbox::is_open());
        }
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
        trace_pump_phase("hotkey polling");
        if (synth::ui::MenuFramework::input_captured()) {
            session->cancel_voice_capture();
            voice_capture_down = false;
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
                                hotkey_held(hotkeys.toggle_voice);
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
        if (!regular_dialogue_blocked && !synth::ui::MenuFramework::input_captured() && !voice_capture_down &&
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
        if (hotkey_pressed(hotkeys.open_mic_mute)) {
            session->toggle_open_mic_mute();
        }
        if (hotkey_pressed(hotkeys.talk_to_npc) && !regular_dialogue_blocked) {
            const auto* owner = session.get();
            const auto generation = runtime->generation();
            const auto snapshot = runtime->capture_snapshot(now);
            if (session.get() != owner || runtime->generation() != generation) return;
            const auto target = session->prepare_chat_target(snapshot);
            const auto untargeted = controls.selection.mode == "NARRATOR" || controls.selection.mode == "INJECTION_LOG";
            if (!target && !untargeted) {
                present_notification("SYNTH: no eligible NPC within conversation range");
            } else if (!(untargeted ? synth::ui::Chatbox::request_open() : synth::ui::Chatbox::request_open(*target))) {
                if (synth::ui::MenuFramework::available()) {
                    present_notification("SYNTH chat is busy or still loading; no message was sent");
                } else {
                    auto text = synth::client::PluginSession::clipboard_text();
                    if (session.get() != owner || runtime->generation() != generation) return;
                    if (text) {
                        (void)session->submit_text(snapshot, std::move(*text), target ? &*target : nullptr);
                    } else {
                        present_notification(
                            "SYNTH: install F4SE Menu Framework or copy 1-4096 characters first");
                    }
                }
            }
        }
        if (hotkey_pressed(hotkeys.synth_control) && !menu_mode) {
            const auto* owner = session.get();
            const auto generation = runtime->generation();
            const auto snapshot = runtime->capture_snapshot(now);
            if (session.get() != owner || runtime->generation() != generation) return;
            control_target = session->prepare_control_target(snapshot);
            if (synth::ui::ControlMenu::request_open(generation.value(),
                    control_target ? control_target->display_name : "", controls.selection.mode, model)) {
                (void)session->request_control();
            } else present_notification("SYNTH control menu unavailable; check F4SE Menu Framework");
        }
        if (session && hotkey_pressed(hotkeys.hard_halt)) {
            session->hard_halt();
            voice_capture_down = false;
            pip_vision_gesture.reset();
            return;
        }
        const auto pip_vision = pip_vision_gesture.update(
            hotkey_held(hotkeys.pip_vision), now);
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
        if (!menu_mode && player_inventory_refresh.due(runtime->generation().value(), now,
                session->acknowledged_inventory_frame()) && session->context_observation_ready()) {
            const auto* owner_session = session.get();
            const auto generation = runtime->generation();
            const auto native_epoch = synth::adapters::FlatPickedReference::stamp();
            const auto revision = player_inventory_refresh.attempt(now);
            auto snapshot = runtime->capture_snapshot(synth::core::SnapshotClock::now(),
                synth::runtime::RuntimeCapturePurpose::player_inventory);
            if (session.get() != owner_session || runtime->generation() != generation ||
                synth::adapters::FlatPickedReference::stamp() != native_epoch ||
                lifecycle.state() != synth::adapters::LifecycleState::ready) return;
            const auto accepted = session->observe_world_context(snapshot, true);
            if (accepted) player_inventory_refresh.submitted(revision, snapshot->frame());
            REX::DEBUG("SYNTH player inventory: {} rows, {}, admitted {}, frame {}, generation {}",
                snapshot->player().inventory().size(), snapshot->player().inventory_observation(),
                accepted, snapshot->frame(), generation.value());
        }
        const auto manual = session && !regular_dialogue_blocked &&
                            hotkey_pressed(hotkeys.manual_activate);
        const auto periodic = session && !session->runtime_paused() && now >= next_context_publish;
        const auto automatic = periodic && !regular_dialogue_blocked &&
                               configuration.activation.enabled;
        trace_pump_phase("context publish");
        if (manual || periodic) {
            const auto trace_background = periodic && !manual && trace_first_background_cycle;
            if (trace_background) {
                REX::INFO("SYNTH runtime: first background cycle beginning shallow capture");
            }
            const auto* owner = session.get();
            const auto generation = runtime->generation();
            auto snapshot = runtime->capture_snapshot(
                now, manual ? synth::runtime::RuntimeCapturePurpose::dialogue
                            : synth::runtime::RuntimeCapturePurpose::background);
            if (session.get() != owner || runtime->generation() != generation) return;
            if (trace_background) {
                REX::INFO("SYNTH runtime: first background shallow capture returned");
            }
            const auto player_history = periodic && !manual && session->publish_player_event(snapshot);
            const auto combat_bark = periodic && !manual && !player_history &&
                                     session->maybe_trigger_combat_bark(snapshot, now);
            if (trace_background) {
                REX::INFO("SYNTH runtime: first background combat gate complete");
            }
            const auto player_reaction = periodic && !regular_dialogue_blocked && !manual && !player_history && !combat_bark &&
                                         session->maybe_trigger_player_reaction(snapshot, now);
            const auto quest_reaction = periodic && !regular_dialogue_blocked && !manual && !player_history && !combat_bark && !player_reaction &&
                                        session->maybe_trigger_quest_reaction(snapshot, now);
            const auto bored = periodic && !regular_dialogue_blocked && !manual && !player_history && !combat_bark && !player_reaction && !quest_reaction &&
                               session->maybe_trigger_bored(snapshot, now);
            if (trace_background) {
                REX::INFO("SYNTH runtime: first background boredom gate complete");
            }
            if (manual || (automatic && !player_history && !combat_bark && !bored && !quest_reaction && !player_reaction)) {
                (void)session->activate_target(snapshot, manual);
            }
            if (periodic && !manual && !player_history)
                (void)session->observe_world_context(std::move(snapshot));
            next_context_publish = now + std::chrono::seconds{2};
            if (trace_background) {
                trace_first_background_cycle = false;
                REX::INFO("SYNTH runtime: first background cycle complete");
            }
        }
    } catch (const std::exception& error) {
        present_notification(std::string{"SYNTH runtime: "} + error.what());
        synth::integration::external_requests.set_available(false);
        next_context_publish = now + std::chrono::seconds{2};
    }
}

// Prevents exceptions from escaping the engine-owned Main::Update callback.
void pump_runtime() noexcept {
    try {
        pump_runtime_impl();
    } catch (const std::exception& error) {
        REX::WARN("SYNTH runtime callback failed: {}", error.what());
        synth::integration::external_requests.set_available(false);
    } catch (...) {
        REX::WARN("SYNTH runtime callback failed with an unknown exception");
        synth::integration::external_requests.set_available(false);
    }
    // After the catch: a pump that threw still returned, so the frame is not the
    // hang the markers are looking for.
    finish_pump_trace();
}

void apply_lifecycle(LifecycleAction action) {
    if (action == LifecycleAction::invalidate || action == LifecycleAction::invalidate_and_ready || action == LifecycleAction::stop)
        synth::adapters::FlatMenuPause::sync(false);
    if (action == LifecycleAction::invalidate || action == LifecycleAction::invalidate_and_ready || action == LifecycleAction::stop)
        synth::adapters::FlatRestEvents::invalidate();
    if (runtime == nullptr) {
        return;
    }
    switch (action) {
    case LifecycleAction::ready:
        runtime_ready_after = synth::core::SnapshotClock::now() + std::chrono::seconds{2};
        break;
    case LifecycleAction::invalidate:
        invalidate_sessions();
        publish_ui_configuration();
        if (runtime) face_animation.reset(*runtime);
        pip_vision_gesture.reset();
        profile_timer_paused_at = {};
        runtime_ready_after = synth::core::SnapshotClock::time_point::max();
        (void)runtime->invalidate();
        break;
    case LifecycleAction::invalidate_and_ready:
        invalidate_sessions();
        publish_ui_configuration();
        if (runtime) face_animation.reset(*runtime);
        pip_vision_gesture.reset();
        profile_timer_paused_at = {};
        runtime_ready_after = synth::core::SnapshotClock::now() + std::chrono::seconds{2};
        (void)runtime->invalidate();
        break;
    case LifecycleAction::stop:
        EngineMainUpdatePump::request_stop();
        if (dispatcher != nullptr) {
            dispatcher->stop_accepting(true);
        }
        invalidate_sessions();
        publish_ui_configuration();
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
    case F4SE::MessagingInterface::kPostLoad:
        (void)synth::adapters::FlatPickedReference::install();
        (void)synth::adapters::FlatRestEvents::install();
        (void)synth::adapters::FlatQuestEvents::install();
        (void)synth::adapters::FlatActorEvents::install();
        (void)synth::adapters::FlatInventoryChanges::install();
        // F4SE maps every plugin before dispatching this, so the optional
        // F4SE Menu Framework module resolves regardless of plugins.txt order.
        // Registration is a no-op when the framework is not installed.
        synth::ui::MenuFramework::register_surface();
        return;
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
    if (action == LifecycleAction::invalidate || action == LifecycleAction::invalidate_and_ready || action == LifecycleAction::stop) {
        synth::adapters::FlatInventoryChanges::invalidate();
        synth::adapters::FlatActorEvents::invalidate();
        synth::adapters::FlatPickedReference::invalidate();
        synth::adapters::FlatRestEvents::invalidate();
        synth::adapters::FlatQuestEvents::invalidate();
    }
    if (action==LifecycleAction::invalidate || action==LifecycleAction::invalidate_and_ready || action==LifecycleAction::stop) {
        synth::integration::external_requests.set_available(false);
    }
    deferred_lifecycle.push(action);
}

}  // namespace

F4SE_PLUGIN_VERSION = []() noexcept {
    F4SE::PluginVersionData version{};
    version.PluginVersion({0, 1, 0, 0});
    version.PluginName("SYNTH");
    version.AuthorName("SYNTH contributors");
    version.UsesAddressLibrary(true);
    version.UsesSigScanning(false);
    version.IsLayoutDependent(true);
    version.HasNoStructUse(false);
    version.CompatibleVersions({F4SE::RUNTIME_1_11_240});
    version.MinimumRequiredXSEVersion({0, 7, 9, 0});
    return version;
}();

F4SE_EXPORT bool F4SEPlugin_Query(const F4SE::QueryInterface* query, F4SE::PluginInfo* info) {
    if (info == nullptr) {
        return false;
    }
    info->infoVersion = F4SE::PluginInfo::kVersion;
    info->name = "SYNTH";
    info->version = REL::Version{0, 1, 0, 0}.pack();
    return environment_supported(query);
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* load) {
    if (!environment_supported(load) ||
        !lifecycle.load(synth::adapters::flat_descriptor,
                        synth::core::RuntimeVariant::flat,
                        version_of(load->RuntimeVersion()),
                        version_of(load->F4SEVersion()))) {
        return false;
    }

    F4SE::Init(load, {.trampoline = true, .trampolineSize = 64});
    bool recorder_ready{};
    try {
        recorder_ready = synth::adapters::NativeFaultRecorder::install_game_log();
    } catch (...) {} // Diagnostics must not prevent the plugin from loading.
    if (recorder_ready) {
        REX::INFO("SYNTH native fault recorder armed in the F4SE log directory");
    } else {
        REX::WARN("SYNTH native fault recorder could not open its diagnostic output");
    }
    if (!synth::adapters::commonlib::register_save_context()) return false;
    if (!synth::adapters::commonlib::register_papyrus_api())
        REX::WARN("SYNTH Papyrus registration unavailable; native dialogue remains enabled");
    const auto* messaging = F4SE::GetMessagingInterface();
    if (messaging == nullptr || messaging->Version() != F4SE::MessagingInterface::kVersion ||
        !messaging->RegisterListener(on_f4se_message)) {
        apply_lifecycle(lifecycle.handle(synth::adapters::LifecycleEvent::shutdown));
        return false;
    }
    try {
        // CHIM keeps its long-lived manager/worker infrastructure outside frame
        // callbacks. SYNTH follows that boundary: the host exists before any save
        // can load, and the engine-update pump only exchanges bounded work.
        session_host = std::make_unique<synth::client::PluginSessionHost>(
            synth::core::RuntimeVariant::flat, "1.11.240", [](const synth::client::RequestOutcome& outcome) {
                if (outcome.status == synth::client::RequestStatus::complete)
                    REX::INFO("SYNTH retired server session: {} acknowledged", outcome.request_id);
                else
                    REX::WARN("SYNTH retired locally; server Halt {} not acknowledged (status {})",
                              outcome.request_id, static_cast<int>(outcome.status));
            }, &synth::adapters::commonlib::save_context_store);
    } catch (...) {
        apply_lifecycle(lifecycle.handle(synth::adapters::LifecycleEvent::shutdown));
        return false;
    }
    if (lifecycle.handle(synth::adapters::LifecycleEvent::plugin_loaded) ==
            LifecycleAction::rejected ||
        !EngineMainUpdatePump::install()) {
        apply_lifecycle(lifecycle.handle(synth::adapters::LifecycleEvent::shutdown));
        return false;
    }
    return true;
}
#endif
