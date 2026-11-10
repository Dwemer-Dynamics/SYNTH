#!/usr/bin/env python3
"""Portable static checks for the Windows build gates."""

from __future__ import annotations

import re
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
EXPECTED_SUITES = (
    "native",
    "transport",
    "json",
    "fake-server",
    "config",
    "diagnostics",
    "tasks",
    "lifecycle",
    "context",
    "targeting",
    "actions",
    "input",
    "audio",
    "presentation",
    "media-fetch",
    "radio",
    "adapters",
)
OPTIONAL_PROTOCOL_SUITE = "protocol-native"


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


class BuildDefinitionTests(unittest.TestCase):
    def test_voice_import_revision_is_retired_with_the_session(self) -> None:
        host = read("src/flat/plugin_entry.cpp")
        for entry in ("void retire_session(", "void invalidate_sessions("):
            body = host.split(entry, 1)[1].split("\n}", 1)[0]
            self.assertIn("Chatbox::set_voice_session_available(false)", body)
        adoption = host.split("session = std::move(result->session);", 1)[1]
        self.assertTrue(adoption.lstrip().startswith("synth::ui::Chatbox::set_voice_session_available(true);"))
        ui = read("src/ui/menu_framework_ui.hpp")
        self.assertIn("voice_import.request(VoiceAction::import_all,ticket)", ui)
        self.assertIn("voice_import.request(VoiceAction::cancel,ticket)", ui)

    def test_presentation_scene_capture_precedes_delivery_without_native_callbacks_in_queues(self) -> None:
        for lane in ("flat", "vr"):
            host = read(f"src/{lane}/plugin_entry.cpp")
            handoff = host.split("session->presentation_capture_candidate(now)", 1)[1].split("session->pump(", 1)[0]
            self.assertIn("runtime->capture_actor_snapshot(now,actors.front().form_id)", handoff)
            self.assertLess(handoff.index("session.get() != owner"), handoff.index("session->submit_presentation_capture"))
        audio = read("src/audio/native_playback.hpp")
        pump = audio.split("bool pump(", 1)[1].split("void halt()", 1)[0]
        self.assertLess(pump.index("queued_.front().caption->scene_ready"), pump.index("start(std::move(clip)"))
        self.assertIn("active_->clip.caption->scene_rejected()", pump)
        for path in ("src/audio/native_playback.hpp", "src/presentation/dialogue_presentation.hpp"):
            self.assertNotIn("capture_actor_snapshot", read(path))
        session = read("src/client/plugin_session.hpp")
        self.assertIn("admission.rejection_reason(snapshot.get(),now)", session)
        self.assertIn("trace_->record(*reason,caption->generation", session)
        self.assertIn(".admission = std::move(admission)", session)

    def test_flat_spin_lock_avoids_rw_lock_relocation(self) -> None:
        capture = read("src/adapters/commonlib_capture.hpp")
        guard = capture.split("class TrySpinLock final", 1)[1].split("class TryReadLock", 1)[0]
        package = capture.split("current_package(RE::Actor& actor)", 1)[1].split("// Read an already-owned flat string", 1)[0]
        override = package.split("// Flat's running-package selector", 1)[1].split("#endif", 1)[0]
        self.assertIn("run_once_lock.emplace(run_once.packageLock)", override)
        self.assertIn("if (!run_once_lock->owns_lock()) return std::nullopt;", override)
        self.assertLess(package.index("package = run_once.package"), package.index("if (package == nullptr)"))
        self.assertEqual(package.count("process->currentPackage.packageLock"), 1)
        self.assertNotIn("GetPackageThatIsRunning(", package)
        flat = guard.split("#else", 1)[1].split("#endif", 1)[0]
        self.assertNotIn("lock_.try_lock()", flat)
        self.assertIn("count_ref.compare_exchange_strong(expected, 1)", flat)
        self.assertIn("owner_ref == thread", flat)
        self.assertIn("if (owns_) lock_.unlock()", guard)

    def test_flat_package_metadata_holds_a_native_lifetime_lease(self) -> None:
        capture = read("src/adapters/commonlib_capture.hpp")
        lease = capture.split("class FlatPackageReadLease final", 1)[1].split("#endif", 1)[0]
        self.assertIn("offsetof(RE::TESPackage, refCount) == 0xC4", lease)
        self.assertIn("offsetof(RE::TESPackage, data) == 0x20", lease)
        self.assertIn("expected == 0 || expected == std::numeric_limits<std::uint32_t>::max()", lease)
        self.assertEqual(lease.count("compare_exchange_strong("), 1)
        self.assertIn("if (--count == 0 && (package_->data.packFlags & 0x800U) != 0) delete package_;", lease)
        package = capture.split("current_package(RE::Actor& actor)", 1)[1].split("// Read an already-owned flat string", 1)[0]
        self.assertLess(package.index("FlatPackageReadLease lease"), package.index("std::optional<TrySpinLock> run_once_lock"))
        self.assertLess(package.index("std::optional<TrySpinLock> current_lock"), package.index("if (package == nullptr)"))
        self.assertLess(package.index("package = process->currentPackage.package"), package.index("lease.try_acquire(package)"))
        self.assertIn("if (!lease.try_acquire(package)) return std::nullopt;\n#endif\n    }", package)
        self.assertLess(package.index("lease.try_acquire(package)"), package.index("origin_plugin(*package)"))

    def test_cmake_registers_every_test_suite(self) -> None:
        cmake = read("CMakeLists.txt")
        for suite in EXPECTED_SUITES:
            with self.subTest(suite=suite):
                self.assertRegex(cmake, rf"(?m)^\s*{re.escape(suite)}\)?\s*$")
                self.assertIn(f'tests/{suite.replace("-", "_")}/', cmake)
        self.assertIn('list(APPEND _synth_test_suites protocol-native)', cmake)
        self.assertIn('if(EXISTS "${_synth_protocol_native_source}")', cmake)
        self.assertIn(
            'add_test(NAME "${_synth_test_target}" COMMAND "${_synth_test_target}")',
            cmake,
        )

    def test_cmake_links_threads_only_to_threaded_suites(self) -> None:
        cmake = read("CMakeLists.txt")
        self.assertIn("find_package(Threads REQUIRED)", cmake)
        threaded_condition = re.search(
            r'if\(_synth_test_suite STREQUAL "native" OR\s*'
            r'_synth_test_suite STREQUAL "tasks" OR\s*'
            r'_synth_test_suite STREQUAL "lifecycle" OR\s*'
            r'_synth_test_suite STREQUAL "radio" OR\s*'
            r'_synth_test_suite STREQUAL "adapters"\)'
            r'.*?target_link_libraries\("\$\{_synth_test_target\}" PRIVATE Threads::Threads\)',
            cmake,
            re.DOTALL,
        )
        self.assertIsNotNone(threaded_condition)
        self.assertEqual(cmake.count("Threads::Threads"), 1)

    def test_xmake_registers_runnable_test_targets(self) -> None:
        xmake = read("xmake.lua")
        for suite in EXPECTED_SUITES:
            with self.subTest(suite=suite):
                self.assertIn(f'{{name = "{suite}", source = ', xmake)
        self.assertIn('name = "protocol-native"', xmake)
        self.assertIn("if os.isfile(protocol_native_source) then", xmake)
        self.assertIn('target("synth-" .. suite.name .. "-tests")', xmake)
        self.assertIn("os.execv(target:targetfile())", xmake)

    def test_vr_options_precede_dependency_population(self) -> None:
        cmake = read("CMakeLists.txt")
        first_dependency_action = min(
            cmake.index("FetchContent_MakeAvailable(commonlibf4vr)"),
            cmake.index('add_subdirectory("${_synth_commonlibf4vr}/CommonLibF4"'),
        )
        for setting in (
            "set(ENABLE_FALLOUT_F4 OFF",
            "set(ENABLE_FALLOUT_NG OFF",
            "set(ENABLE_FALLOUT_VR ON",
            "set(BUILD_TESTS OFF",
        ):
            self.assertLess(cmake.index(setting), first_dependency_action)

    def test_vr_gate_names_actionable_vcpkg_requirements(self) -> None:
        cmake = read("CMakeLists.txt")
        self.assertIn("CMAKE_TOOLCHAIN_FILE", cmake)
        self.assertIn("rsm-mmio:x64-windows-static-md", cmake)
        self.assertIn("spdlog:x64-windows-static-md", cmake)
        self.assertIn("mmio::mmio", cmake)
        self.assertIn("spdlog::spdlog", cmake)

    def test_flat_target_is_an_opt_in_default_target(self) -> None:
        xmake = read("xmake.lua")
        flat = xmake[xmake.index('target("SYNTH")') :]
        self.assertIn("set_default(true)", flat)
        self.assertNotIn('os.isdir("external/CommonLibF4")', xmake)
        self.assertIn("--commonlibf4_dir", xmake)

    def test_plugin_targets_compile_entries_and_adapter_headers(self) -> None:
        cmake = read("CMakeLists.txt")
        xmake = read("xmake.lua")
        self.assertIn('src/adapters/vr_fallout_runtime.hpp', cmake)
        self.assertIn(
            'target_compile_definitions(SYNTHVR PRIVATE SYNTH_WITH_F4SEVR=1',
            ' '.join(cmake.split()),
        )
        self.assertIn('add_headerfiles("src/adapters/**.hpp")', xmake)
        self.assertIn('add_defines("SYNTH_WITH_F4SE=1"', ' '.join(xmake.split()))

    def test_each_lane_has_one_source_owned_version_export(self) -> None:
        xmake = read("xmake.lua")
        flat = read("src/flat/plugin_entry.cpp")
        vr = read("src/vr/plugin_entry.cpp")
        self.assertNotIn('add_rules("commonlibf4.plugin"', xmake)
        self.assertEqual(flat.count("F4SE_PLUGIN_VERSION"), 1)
        self.assertNotIn("F4SEPlugin_Version", flat)
        self.assertEqual(vr.count("F4SEPlugin_Version"), 1)
        self.assertNotIn("F4SE_PLUGIN_VERSION", vr)

    def test_entries_instantiate_runtime_and_connect_invalidation(self) -> None:
        for lane, runtime in (("flat", "FlatFalloutRuntime"), ("vr", "VrFalloutRuntime")):
            entry = read(f"src/{lane}/plugin_entry.cpp")
            with self.subTest(lane=lane):
                self.assertIn(runtime, entry)
                self.assertIn("GameThreadDispatcher", entry)
                self.assertIn("runtime->invalidate()", entry)
                self.assertIn("capture_snapshot", entry)

    def test_flat_runtime_pump_uses_guarded_engine_update_hook(self) -> None:
        entry = read("src/flat/plugin_entry.cpp")
        # F4SE queues migrate across workers and hold their global locks while
        # callbacks run. Hook the exact 1.11.240 Main::Update call site, verify
        # its original target, and reject any entry outside RE::Main's thread.
        self.assertNotIn("AddTaskPermanent", entry)
        self.assertNotIn("AddTask(", entry)
        self.assertNotIn("AddUITask", entry)
        self.assertNotIn("write_vfunc", entry)
        self.assertNotIn("SetTimer", entry)
        self.assertNotIn("WM_TIMER", entry)
        self.assertNotIn("PlayerCharacterPumpHook", entry)
        self.assertNotIn("PlayerControlsPumpHook", entry)
        self.assertIn("class EngineMainUpdatePump final", entry)
        self.assertIn("supported_runtime_{1, 11, 240, 0}", entry)
        self.assertIn("main_update_id_{2228917}", entry)
        self.assertIn("main_update_call_site_rva_{0x00C315BB}", entry)
        self.assertIn("bytes[0] != 0xE8", entry)
        self.assertIn("existing_target != main_update", entry)
        self.assertIn("REL::GetTrampoline().write_call<5>(call_site, thunk)", entry)
        self.assertIn("RE::Main::GetSingleton()", entry)
        self.assertIn("engine->threadID", entry)
        self.assertIn("current_thread_id == engine->threadID", entry)
        self.assertIn("rejected engine-update pump thread", entry)
        self.assertIn("entered Fallout engine-update pump", entry)
        self.assertIn("if (!callback_active_)", entry)
        self.assertIn("F4SE::Init(load, {.trampoline = true, .trampolineSize = 64})", entry)
        thunk = entry[entry.index("static void thunk(void* argument)") : entry.index(
            "static inline constexpr REL::Version", entry.index("static void thunk(void* argument)")
        )]
        self.assertLess(thunk.index("original_update_(argument);"), thunk.index("pump_runtime();"))

    def test_missing_entry_points_fail_without_placeholder_sources(self) -> None:
        cmake = read("CMakeLists.txt")
        xmake = read("xmake.lua")
        self.assertIn("src/vr/plugin_entry.cpp does not exist", cmake)
        self.assertIn("src/flat/plugin_entry.cpp does not exist", xmake)
        self.assertNotRegex(cmake + xmake, r"file\s*\(\s*WRITE")


class MenuFrameworkSurfaceTests(unittest.TestCase):
    """SYNTH registers native F4SE Menu Framework pages, never MCM JSON."""

    def test_no_legacy_mcm_assets_are_tracked_or_packaged(self) -> None:
        self.assertFalse((ROOT / "packaging/Data/MCM").exists())
        packaging = read("scripts/package_release.py")
        self.assertNotIn("Data/MCM", packaging)
        audit = read("tools/audit_release_tree.py")
        self.assertNotIn('"Data/MCM/Config/SYNTH/config.json"', audit)
        self.assertIn("legacy MCM asset in release", audit)

    def test_registration_uses_the_official_consumer_api_at_post_load(self) -> None:
        adapter = read("src/ui/menu_framework_ui.hpp")
        for symbol in (
            "F4SEMenuFramework::IsInstalled",
            "F4SEMenuFramework::SetSection",
            "F4SEMenuFramework::AddSectionItem",
            "F4SEMenuFramework::Hotkeys::Register",
            "F4SEMenuFramework::Hotkeys::GetBinding",
            "F4SEMenuFramework::Hotkeys::SetBinding",
            "F4SEMenuFramework::IsAnyBlockingWindowOpened",
        ):
            with self.subTest(symbol=symbol):
                self.assertIn(symbol, adapter)
        # Absent framework must be a no-op, never a hard dependency.
        self.assertIn('__has_include("F4SEMenuFramework.h")', adapter)
        self.assertIn("SYNTH_HAS_MENU_FRAMEWORK", adapter)

        entry = read("src/flat/plugin_entry.cpp")
        self.assertIn("case F4SE::MessagingInterface::kPostLoad:", entry)
        self.assertLess(
            entry.index("case F4SE::MessagingInterface::kPostLoad:"),
            entry.index("case F4SE::MessagingInterface::kPreLoadGame:"),
        )
        self.assertIn("synth::ui::MenuFramework::register_surface();", entry)

        # Session publication must not activate a registry walk on the next
        # render frame. Registry access is limited to an explicit page edit.
        self.assertIn("F4SEMenuFramework::Events::kBeforeRender", adapter)
        self.assertNotIn("reconcile_hotkeys", adapter + entry)
        lifecycle = adapter.index("inline void __stdcall on_framework_lifecycle(")
        lifecycle_end = adapter.index("\n}\n\n}  // namespace detail", lifecycle)
        lifecycle_body = adapter[lifecycle:lifecycle_end]
        self.assertNotIn("published_configuration.load", lifecycle_body)
        self.assertNotIn("Hotkeys::GetBinding", lifecycle_body)
        self.assertNotIn("Hotkeys::SetBinding", lifecycle_body)

        explicit_edit = adapter.index("inline void commit_hotkey_selection(")
        draw_keybind = adapter.index("inline void draw_keybind(")
        self.assertLess(explicit_edit, draw_keybind)
        self.assertIn("Hotkeys::HasConflict", adapter[explicit_edit:draw_keybind])
        self.assertIn("Hotkeys::SetBinding", adapter[explicit_edit:draw_keybind])
        self.assertIn("Hotkeys::GetBinding", adapter[explicit_edit:draw_keybind])
        self.assertIn("note_pending(setting, selected)", adapter[explicit_edit:draw_keybind])
        self.assertIn("commit(setting, selected)", adapter[explicit_edit:draw_keybind])
        self.assertEqual(adapter.count("Hotkeys::HasConflict"), 1)
        self.assertEqual(adapter.count("Hotkeys::GetBinding"), 1)
        self.assertEqual(adapter.count("Hotkeys::SetBinding"), 2)

    def test_gameplay_thread_makes_no_framework_call(self) -> None:
        """Framework entry points are render-thread owned; the pump reads atomics."""
        adapter = read("src/ui/menu_framework_ui.hpp")
        entry = read("src/flat/plugin_entry.cpp")

        # IsAnyBlockingWindowOpened resolves a framework export behind a
        # function-local static and then reads renderer-owned window state, so it
        # belongs to kBeforeRender for the same reason the hotkey reconcile does.
        # Calling it from the game task hung the frame after a load.
        self.assertEqual(adapter.count("F4SEMenuFramework::IsAnyBlockingWindowOpened()"), 1)
        lifecycle = adapter.index("inline void __stdcall on_framework_lifecycle(")
        capture_probe = adapter.index("F4SEMenuFramework::IsAnyBlockingWindowOpened()")
        facade = adapter.index("static bool input_captured() noexcept")
        self.assertLess(lifecycle, capture_probe)
        self.assertLess(capture_probe, facade)

        # The gameplay thread reads only the state SYNTH itself published.
        self.assertIn("inline std::atomic<bool> framework_input_captured{false};", adapter)
        self.assertIn("framework_input_captured.store(", adapter)
        self.assertIn("return detail::framework_input_captured.load(", adapter)

        # Chatbox availability answers from the published window pointer alone.
        chatbox_available = adapter.index("static bool available() noexcept")
        self.assertNotIn(
            "F4SEMenuFramework",
            adapter[chatbox_available : adapter.index("static bool request_open() noexcept")],
        )
        # IsInstalled is only the module-handle probe, and only on the two paths
        # that never run per frame: kPostLoad registration and the presence facade.
        self.assertEqual(adapter.count("F4SEMenuFramework::IsInstalled()"), 2)

        # The gameplay entry point never names the framework at all.
        self.assertNotIn("F4SEMenuFramework", entry)
        self.assertIn("synth::ui::MenuFramework::input_captured()", entry)

    def test_first_post_session_pump_logs_bounded_phase_markers(self) -> None:
        entry = read("src/flat/plugin_entry.cpp")
        # Armed once per process, from the frame that logged session init, so the
        # per-frame pump cannot turn these markers into per-frame log spam.
        self.assertEqual(entry.count("pump_trace = PumpTrace::armed;"), 1)
        self.assertLess(
            entry.index("queue_session_initialization(snapshot);"),
            entry.index("if (pump_trace == PumpTrace::idle) pump_trace = PumpTrace::armed;"),
        )
        # Later startup phases can arrive after the first successful update.
        # Each phase is logged once within a bounded observation window.
        self.assertIn('std::array<const char*, 24> traced_pump_phases', entry)
        self.assertIn('now() < pump_trace_deadline', entry)
        self.assertIn('NativeFaultRecorder::phase(phase)', entry)
        self.assertIn("pump_trace = PumpTrace::done;", entry)
        self.assertIn("phase markers disabled", entry)
        self.assertLess(entry.index("pump_runtime_impl();"), entry.index("finish_pump_trace();"))
        self.assertIn("first post-session engine-update pump entered", entry)
        for phase in (
            "dispatcher drain",
            "settings revision check",
            "session adoption",
            "session pump",
            "chatbox drain",
            "hotkey polling",
            "context publish",
        ):
            with self.subTest(phase=phase):
                self.assertIn(f'trace_pump_phase("{phase}");', entry)

    def test_session_workers_start_and_stop_outside_game_task(self) -> None:
        entry = read("src/flat/plugin_entry.cpp")
        host = read("src/client/plugin_session_host.hpp")
        playback = read("src/audio/native_playback.hpp")

        self.assertIn("PluginSessionHost", entry)
        self.assertIn("session initialization queued off the game thread", entry)
        self.assertIn("adopted initialized session from coordinator", entry)
        self.assertNotIn("std::make_unique<synth::client::PluginSession>(", entry)
        self.assertIn("std::make_unique<PluginSession>(", host)
        self.assertIn("worker_{[this] { worker_loop(); }}", host)
        self.assertIn("session_host->retire(std::move(session));", entry)
        constructor = playback[
            playback.index("explicit NativeAudioPlayback") : playback.index(
                "NativeAudioPlayback(const NativeAudioPlayback&)"
            )
        ]
        self.assertNotIn("XAudio2Create", constructor)
        self.assertIn("engine_thread_ = std::thread", playback)
        self.assertIn("CoInitializeEx(nullptr, COINIT_MULTITHREADED)", playback)

        pump = entry.index("void pump_runtime_impl()")
        lifecycle = entry.index("void apply_lifecycle(", pump)
        pump_body = entry[pump:lifecycle]
        self.assertNotIn("PluginSession(", pump_body)
        self.assertNotIn("load_configuration", pump_body)
        self.assertNotIn("session.reset()", pump_body)

        session = read("src/client/plugin_session.hpp")
        delivery = read("src/client/speech_delivery.hpp")
        # Queue rejection/expiry/discard must release all admission-time activity.
        for counter, source in (("speech_jobs_", delivery), ("active_actions_", session), ("halt_requests_", session)):
            self.assertIn(f"tasks::TaskLanes::track_activity({counter},", source)
            self.assertNotIn(f"{counter}.fetch_add(", source)
            self.assertNotIn(f"{counter}.fetch_sub(", source)
        destructor = session[session.index("~PluginSession()") : session.index(
            "[[nodiscard]] bool initialize(")]
        self.assertIn("network_.shutdown(tasks::ShutdownMode::cancel_pending);", destructor)
        # The network lane, which carries receipt drains into the speech owner, is joined first.
        teardown = [destructor.index(step) for step in (
            "request_stop();", "dialogue_.cancel_microphone();", "transport_.cancel_all();", "network_.shutdown(",
            "speech_.join_jobs();", "control_.shutdown(", "speech_.halt_playback();")]
        self.assertEqual(teardown, sorted(teardown))
        fallback = delivery.split("~SpeechDelivery() {", 1)[1].split("}", 1)[0]
        self.assertLess(fallback.index("join_jobs();"), fallback.index("halt_playback();"))
        self.assertIn("void join_jobs() noexcept { speech_.shutdown(tasks::ShutdownMode::cancel_pending); }", delivery)
        self.assertIn('tasks::TaskLanes speech_{{tasks::LaneConfig{"speech", 16, 4, 3}}, 1};', delivery)
        self.assertIn("SpeechDelivery speech_;", session)
        self.assertLess(session.index("std::deque<std::string> notifications_;"), session.index("SpeechDelivery speech_;"))
        speech = delivery.split("void queue_speech(", 1)[1].split("void enqueue_prepared_speech(", 1)[0]
        self.assertIn("speech_.try_submit(", speech)
        self.assertNotIn("network_.try_submit(", speech)
        speculative = delivery.split("void prepare_rechat_media(", 1)[1].split("private:", 1)[0]
        self.assertIn("speech_.try_submit(", speculative)
        self.assertNotIn("playback_.enqueue(", speculative)
        self.assertNotIn("captions_.enqueue(", speculative)
        self.assertIn("prefetch->finish_media(std::move(bytes))", speculative)
        self.assertIn("prefetch->prepared_media(line.line_id)", speech)
        self.assertIn("{ return speech_.discard_pending(); }", delivery)
        self.assertIn("{ return speech_.discard_cancelled(); }", delivery)
        hard_halt = session.split("void hard_halt() noexcept", 1)[1].split("const auto submitted", 1)[0]
        halt_order = [hard_halt.index(step) for step in (
            "speech_.halt_playback();", "dispatcher_.discard_pending();", "(void)network_.discard_pending();",
            "(void)speech_.discard_pending_jobs();")]
        self.assertEqual(halt_order, sorted(halt_order))
        turn = session.split("core::CancellationToken begin_player_turn(", 1)[1].split("return turn;", 1)[0]
        turn_order = [turn.index(step) for step in (
            "dialogue_.replace_turn();", "(void)network_.discard_cancelled();", "(void)speech_.discard_cancelled_jobs();",
            "dialogue_.begin_external(std::move(actor));",
            "dialogue_.cancel_interrupted_turn();", "speech_.interrupt_playback();")]
        self.assertEqual(turn_order, sorted(turn_order))
        self.assertLess(session.index("std::deque<std::string> notifications_;"), session.index("PlayerDialogue dialogue_;"))
        self.assertLess(session.index("PlayerDialogue dialogue_;"), session.index("SpeechDelivery speech_;"))
        self.assertNotIn("retire_remote_session", destructor)
        host = read("src/client/plugin_session_host.hpp")
        self.assertIn("retire_and_destroy(retired)", host)
        self.assertIn("retire_and_destroy(replaced)", host)
        stop = session.split("void request_stop() noexcept", 1)[1].split("retire_remote_session()", 1)[0]
        self.assertNotIn("client_.", stop)
        self.assertNotIn("shutdown(", stop)

    def test_session_start_waits_for_stable_menu_free_gameplay(self) -> None:
        entry = read("src/flat/plugin_entry.cpp")
        menu_query = entry.index(
            "const auto menu_mode = synth::adapters::commonlib::menu_mode_active();"
        )
        readiness_gate = entry.index("if (!session && menu_mode)", menu_query)
        session_start = entry.index("if (!session) {", readiness_gate)
        self.assertLess(menu_query, readiness_gate)
        self.assertLess(readiness_gate, session_start)
        gate_body = entry[readiness_gate:session_start]
        self.assertIn("runtime_ready_after = now + std::chrono::seconds{2};", gate_body)
        self.assertIn("return;", gate_body)
        bootstrap = entry[session_start : entry.index("player_was_in_combat", session_start)]
        self.assertIn("RuntimeCapturePurpose::bootstrap", bootstrap)

    def test_settings_watcher_does_not_query_filesystem_on_game_thread(self) -> None:
        entry = read("src/flat/plugin_entry.cpp")
        host = read("src/client/plugin_session_host.hpp")
        watcher = entry[entry.index("void reload_settings_if_changed(") : entry.index(
            "// One-time phase markers", entry.index("void reload_settings_if_changed(")
        )]
        self.assertIn("take_settings()", watcher)
        self.assertIn("session_settings.hotkeys = local_configuration->hotkeys", watcher)
        self.assertIn("!allow_reconnect", watcher)
        self.assertIn("session_settings != *local_configuration", watcher)
        self.assertNotIn("inspect_file_revision", watcher)
        self.assertNotIn("settings_override_path", watcher)
        self.assertIn("condition_.wait_until(lock, next_settings_probe", host)
        self.assertIn("now + std::chrono::seconds{1}", host)
        self.assertIn("config::inspect_file_revision(", host)

    def test_capture_cost_is_split_from_connection_and_open_mic_frames(self) -> None:
        entry = read("src/flat/plugin_entry.cpp")
        capture = read("src/adapters/commonlib_capture.hpp")
        session = read("src/client/plugin_session.hpp")

        self.assertIn("struct FlatCaptureCache final", capture)
        self.assertIn("RuntimeCapturePurpose::background", entry)
        self.assertIn("maximum_cell_references = 512", capture)
        self.assertNotIn("HasLOSToTarget", capture)
        flat = capture.split("inline detail::CapturedRuntimeValues capture_flat(", 1)[1].split(
            "inline detail::CapturedRuntimeValues capture_vr(", 1)[0]
        self.assertIn("actor_snapshot(*player, playthrough, player, (dialogue || player_inventory) && !menu_mode_active(), false)", flat)
        self.assertIn("const auto capture_owner = FlatPickedReference::stamp();", flat)
        self.assertIn("FlatPickedReference::stamp() != capture_owner", flat)
        self.assertIn("capture crossed a native load boundary", flat)
        inventory = capture.split("inventory_snapshot(", 1)[1].split("struct QuestCapture", 1)[0]
        limits = capture.split("maximum_nearby_actors = 64;", 1)[1].split("maximum_cell_references", 1)[0]
        vr_limits, flat_limits = limits.split("#else", 1)
        self.assertIn("maximum_inventory_items = 32", vr_limits)
        self.assertIn("maximum_inventory_copies = 128", vr_limits)
        self.assertIn("maximum_inventory_items = 512", flat_limits)
        self.assertIn("maximum_inventory_copies = 512", flat_limits)
        self.assertIn("maximum_inventory_entries = 1024", flat_limits)
        self.assertIn("started + std::chrono::microseconds{500}", inventory)
        self.assertIn("started + std::chrono::milliseconds{2}", inventory)
        self.assertIn('if (started >= deadline) return {{}, "unavailable"}', inventory)
        self.assertIn("std::min(started + std::chrono::milliseconds{2}, deadline)", inventory)
        self.assertEqual(inventory.count("Clock::now() >= structure_deadline"), 2)
        self.assertLess(inventory.index("copies.reserve(maximum_inventory_copies)"), inventory.index("const TryReadLock lock{actor.inventoryList->rwLock}"))
        self.assertLess(inventory.index("Clock::now() >= metadata_deadline"), inventory.index("inventory_metadata(*item.object, copy.extra)"))
        self.assertIn("observed.stackData=stack;", inventory)
        self.assertIn("copies.resize(first_copy)", inventory)
        self.assertLess(inventory.index("std::stable_sort(copies.begin()"), inventory.index("GetDisplayFullName"))
        self.assertIn("copies.back().extra = stack->extra;", inventory)
        self.assertIn("inventory_metadata(*item.object, copy.extra)", inventory)
        metadata = capture.split("inventory_metadata(", 1)[1].split("// Copy bounded inventory structure", 1)[0]
        self.assertIn("const TryReadLock lock{extra->extraRWLock}", metadata)
        self.assertIn("inspected < maximum_extra_data_entries", metadata)
        self.assertIn("metadata.instance = instance->data;", metadata)
        self.assertNotIn("GetBaseInstanceData", metadata)
        read_lock = capture.split("class TryReadLock final", 1)[1].split("inline bool menu_mode_active", 1)[0]
        flat_read_lock = read_lock.split("#else", 1)[1].split("#endif", 1)[0]
        self.assertEqual(flat_read_lock.count("compare_exchange_strong"), 1)
        self.assertNotIn("try_lock_read()", flat_read_lock)
        self.assertNotIn("while (", flat_read_lock)
        for blocking_getter in ("GetDisplayFullName", "GetDisplayName", "GetInstanceData", "GetByType"):
            self.assertNotIn(blocking_getter, metadata)
        references = capture.split("inline NearbyReferenceSnapshots nearby_reference_snapshots(", 1)[1].split("struct NearbyActorDiscovery", 1)[0]
        self.assertIn("is_nearby_item_type(type) ? std::string{} : point_of_interest_kind(*reference, type)", references)
        flat_items = references.split("#if !defined(SYNTH_WITH_F4SEVR)", 1)[1].split("#else", 1)[0]
        self.assertIn("inventory_metadata(*base, reference->extraList, count_vtable.address())", flat_items)
        self.assertIn("inventory_statistics(*base, metadata->instance.get())", flat_items)
        self.assertIn("metadata->reference_count", flat_items)
        self.assertEqual(flat_items.count('result.items_observation = "partial"; continue;'), 2)
        for fabricated_or_blocking in ("GetDisplayFullName", "GetBaseInstanceData", "GetByType", "count = 1"):
            self.assertNotIn(fabricated_or_blocking, flat_items)
        self.assertIn("observed_vtable != count_vtable", metadata)
        self.assertIn("if (count != nullptr) return std::nullopt;", metadata)
        self.assertIn("if (value == 0) return std::nullopt;", metadata)
        discovery = capture.split("inline NearbyActorSnapshots nearby_actors(", 1)[1].split("return captured;", 1)[0]
        self.assertRegex(discovery, r"actor_snapshot\(\*candidate\.reference, playthrough,\s*"
                         r"const_cast<RE::PlayerCharacter\*>\(&player\),\s*false, false\)")
        selected = capture.split("selected_actor_details(", 1)[1].split("struct NearbyActorSnapshots", 1)[0]
        self.assertIn("actor_snapshot(*actor, selected.playthrough_id(), player, true, true)", selected)
        self.assertIn("if (!bootstrap && !isolated)", flat)
        self.assertIn("const auto actor_events = purpose == runtime::RuntimeCapturePurpose::actor_events;", flat)
        self.assertIn("const auto isolated = actor_events || player_inventory;", flat)
        self.assertIn("const auto refresh_quests = !isolated && (", flat)
        self.assertIn("if (!bootstrap && !isolated && !menu_mode_active())", flat)
        self.assertIn("nearby_actors(*player, playthrough, &flat_capture_cache.actor_discovery)", flat)
        self.assertIn("if (bootstrap) flat_capture_cache.next_quest_capture = now + std::chrono::seconds{5};", flat)
        self.assertIn("dialogue || (!bootstrap && now >= flat_capture_cache.next_quest_capture)", flat)
        quest_refresh = flat.split("if (refresh_quests) {", 1)[1].split("\n    }", 1)[0]
        self.assertIn("active_quest_snapshot(*player)", quest_refresh)
        self.assertIn("next_quest_capture = now + std::chrono::seconds{5}", quest_refresh)
        self.assertNotIn("nearby_reference_snapshots", quest_refresh)
        self.assertNotIn("actor_snapshot", quest_refresh)
        self.assertIn("if (purpose == runtime::RuntimeCapturePurpose::dialogue) {", flat)
        dialogue_refresh = flat.split("if (purpose == runtime::RuntimeCapturePurpose::dialogue) {", 1)[1].split("\n    }", 1)[0]
        self.assertIn("nearby_reference_snapshots(*player, camera_pose)", dialogue_refresh)
        self.assertEqual(flat.count("nearby_reference_snapshots("), 1)
        self.assertIn("auto values = detail::CapturedRuntimeValues", flat)
        self.assertLess(
            flat.index("auto values = detail::CapturedRuntimeValues"),
            flat.index("const auto elapsed"),
        )

        open_mic = session[session.index("MicrophoneEvent pump_open_microphone(") : session.index(
            "void toggle_open_mic_mute", session.index("MicrophoneEvent pump_open_microphone(")
        )]
        self.assertNotIn("Snapshot snapshot", open_mic)
        self.assertNotIn("capture_snapshot", open_mic)
        self.assertIn("const auto event = session->pump_open_microphone(now)", entry)
        self.assertIn("if (event != MicrophoneEvent::none)", entry)
        self.assertIn("session->bind_voice_target(snapshot)", entry)
        self.assertIn("session->voice_target_form_id()", entry)

        context = read("src/client/context_publication.hpp")
        initialize = context[context.index("if (initialize_first)") : context.index(
            "const auto outcome = publish(snapshot, target, cancellation);", context.index("if (initialize_first)")
        )]
        self.assertIn('notify_("SYNTH connected")', initialize)
        self.assertIn("return;", initialize)
        self.assertIn("[[nodiscard]] bool initialize(Snapshot snapshot) { return context_.initialize(std::move(snapshot)); }", session)

    def test_chatbox_is_one_framework_window_that_synth_alone_owns(self) -> None:
        adapter = read("src/ui/menu_framework_ui.hpp")
        # The official window API. Creation is deferred to the framework's own
        # render lifecycle, because AddWindow mutates the framework's window
        # registry and the pages register at kPostLoad while it is still starting.
        self.assertIn("F4SEMenuFramework::AddWindow(render_chatbox", adapter)
        self.assertEqual(adapter.count("F4SEMenuFramework::AddWindow("), 1)
        self.assertIn("InputTextMultiline", adapter)
        # Closing is SYNTH clearing its own window flag, never a framework
        # close call: the framework keeps ownership of its main menu.
        self.assertNotIn("F4SEMenuFramework::CloseMenu", adapter)
        self.assertIn("IsOpen.store(false)", adapter)

        entry = read("src/flat/plugin_entry.cpp")
        # Keep the target attached to the submission; the legacy string drain is not a flat-pump API.
        self.assertIn("synth::ui::Chatbox::take_submission()", entry)
        self.assertNotIn("Chatbox::take_message()", entry)
        drain = entry[entry.index("if (auto submission = menu_mode ? std::nullopt : synth::ui::Chatbox::take_submission())")
                      : entry.index("if (!session->runtime_paused() && now >= next_profile_refresh)")]
        self.assertIn("if (submission->target) {", drain)
        self.assertIn("!target.session.is_cancelled()", drain)
        self.assertIn("target.session.generation()==runtime->generation()", drain)
        self.assertIn("runtime->capture_actor_snapshot(now,target.form_id)", drain)
        self.assertIn("session->submit_prompt(snapshot, target,", drain)
        self.assertIn("SYNTH targeted prompt is no longer valid; no message was sent", drain)
        self.assertEqual(drain.count("submit_text("), 2)
        self.assertIn('target.manual_chat', drain)
        self.assertIn('session->submit_text(std::move(snapshot), std::move(submission->text))', drain)
        self.assertIn('if (session.get() != owner || runtime->generation() != generation) return;', drain)
        self.assertNotIn('session->submit_text(runtime->capture_snapshot', drain)
        session = read("src/client/plugin_session.hpp")
        prompt = session[session.index("bool submit_prompt(") : session.index("private:", session.index("bool submit_prompt("))]
        self.assertNotIn("selected_target(", prompt)
        self.assertIn("actor.form_id()==target.form_id", prompt)
        for field in ("origin_plugin", "base_form_id", "base_origin_plugin", "playthrough_id"):
            self.assertIn(f"current->{field}!=target.{field}", prompt)
        self.assertIn("synth::ui::Chatbox::request_open()", entry)
        self.assertNotIn("session->submit_text(runtime->capture_snapshot(now)", entry)

    def test_pages_and_hotkeys_are_preserved(self) -> None:
        catalog = read("src/ui/settings_catalog.hpp")
        for title in ("Hotkeys", "Auto Activate", "Behavior", "Sound", "Tools"):
            with self.subTest(page=title):
                self.assertIn(f'{{"{title}", detail::', catalog)
        for key in (
            "TalkToNPC",
            "ToggleVoice",
            "SYNTHControl",
            "ManualActivate",
            "StopTalking",
            "PipVision",
            "OpenMicMute",
            "WriteDiary",
        ):
            with self.subTest(hotkey=key):
                self.assertIn(f'"{key}"', catalog)
        # Help text carried over from the previous settings surface.
        self.assertIn("Hold to talk to the selected NPC. Unbound by default.", catalog)
        self.assertIn("Reconnect SYNTH and republish the current Fallout runtime context", catalog)

    def test_one_authoritative_persisted_settings_source(self) -> None:
        session = read("src/client/plugin_session.hpp")
        self.assertIn("Data/F4SE/Plugins/SYNTH.ini", session)
        self.assertIn("Data/F4SE/Plugins/SYNTH_custom.ini", session)
        self.assertNotIn("Data/MCM", session)

        store = read("src/ui/settings_store.hpp")
        self.assertIn("Data/F4SE/Plugins/SYNTH_custom.ini", store)

        shipped = read("config/SYNTH.ini")
        for section in ("[Hotkeys]", "[AutoActivate]", "[Behavior]", "[Audio]", "[Tools]"):
            with self.subTest(section=section):
                self.assertIn(section, shipped)
        # Unbound is the shipped default for every hotkey.
        hotkeys = shipped.split("[Hotkeys]", 1)[1].split("[AutoActivate]", 1)[0]
        bindings = [line for line in hotkeys.splitlines() if "=" in line]
        self.assertEqual(len(bindings), 8)
        for line in bindings:
            with self.subTest(line=line):
                self.assertTrue(line.strip().endswith("=0"))

        for lane in ("flat", "vr"):
            entry = read(f"src/{lane}/plugin_entry.cpp")
            with self.subTest(lane=lane):
                self.assertNotIn("Data/MCM", entry)
                self.assertIn("settings_override_path()", entry)
                # The parser only accepts true/false, so re-arming must not write "0".
                self.assertIn('write_setting("Tools", "InitializeSYNTH", "false")', entry)

    def test_flat_lane_pins_the_consumer_header(self) -> None:
        pins = read("cmake/DependencyPins.cmake")
        self.assertIn("SYNTH_MENU_FRAMEWORK_REPOSITORY", pins)
        self.assertIn("SYNTH_MENU_FRAMEWORK_REVISION", pins)
        revision = re.search(r'SYNTH_MENU_FRAMEWORK_REVISION "([0-9a-f]{40})"', pins)
        self.assertIsNotNone(revision)

        xmake = read("xmake.lua")
        self.assertIn('option("menuframework_dir")', xmake)
        self.assertIn("--menuframework_dir", xmake)

        script = read("scripts/build-flat.ps1")
        self.assertIn("F4SEMenuFramework", script)
        self.assertIn(revision.group(1), script)
        self.assertIn("F4SEMenuFramework revision mismatch", script)
        self.assertIn("--menuframework_dir=$MenuFrameworkHeaders", script)

        provenance = read("provenance/F4SEMENUFRAMEWORK.md")
        self.assertIn(revision.group(1), provenance)
        self.assertIn("DCCStudios/F4SEMenuFramework", provenance)
        self.assertIn("LICENSE", provenance)


class PowerShellBuildScriptTests(unittest.TestCase):
    scripts = ("scripts/build-flat.ps1", "scripts/build-vr.ps1")

    def test_player_event_sampling_is_scalar_and_independent_of_response_work(self) -> None:
        capture = read("src/adapters/commonlib_capture.hpp").split("player_event_sample(", 1)[1].split("struct FactionCapture", 1)[0]
        self.assertLess(capture.index("runtime.assert_game_thread()"), capture.index("RE::PlayerCharacter"))
        for expected in ("player->GetLevel()", "player->IsInCombat()", "player->IsDead(false)", "game_time_ticks()",
                         "runtime.generation() == generation"):
            self.assertIn(expected, capture)
        for forbidden in ("actor_snapshot(", "nearby_actors(", "active_quest_snapshot(", "inventory_snapshot(", "Get3D(", "PlayerCamera"):
            self.assertNotIn(forbidden, capture)
        entry = read("src/flat/plugin_entry.cpp")
        unavailable = entry.split("if (now < runtime_ready_after || !synth::adapters::commonlib::world_ready_for_capture())", 1)[1].split("return;", 1)[0]
        self.assertIn("session->observe_player_events(std::nullopt)", unavailable)
        sample = entry.split("if (!menu_mode && session->player_event_sample_due(now))", 1)[1].split("if (!menu_mode && session->quest_event_capture_ready())", 1)[0]
        self.assertIn("session.get() == owner_session && runtime->generation() == owner_generation", sample)
        due = read("src/client/plugin_session.hpp").split("bool player_event_sample_due(", 1)[1].split("void observe_player_events", 1)[0]
        for forbidden in ("active_responses_", "active_actions_", "playback_", "microphone_", "try_submit", "health()"):
            self.assertNotIn(forbidden, due)
        self.assertIn("std::chrono::seconds{1}", read("src/core/player_event_sampler.hpp"))

    def test_player_event_delivery_uses_fresh_context_and_game_thread_receipts(self) -> None:
        session = read("src/client/plugin_session.hpp")
        delivery = session.split("bool publish_player_event(Snapshot snapshot)", 1)[1].split("bool activate_target", 1)[0]
        self.assertIn("player_event_receipt_.exchange", delivery)
        self.assertIn("player_events_.acknowledge", delivery)
        worker = delivery.split("[this, snapshot=", 1)[1]
        self.assertNotIn("player_events_.", worker)
        for expected in ("client_.begin_context_turn()", "snapshot->captured_at() > std::chrono::seconds{2}",
                         "std::nullopt, event)", "player_event_receipt_.store", "cancellation.is_cancelled()"):
            self.assertIn(expected, worker)
        entry = read("src/flat/plugin_entry.cpp")
        self.assertLess(entry.index("const auto player_history ="), entry.index("const auto combat_bark ="))

    def test_player_reactions_are_receipt_gated_fresh_and_one_shot(self) -> None:
        session = read("src/client/plugin_session.hpp")
        reaction = session.split("bool maybe_trigger_player_reaction(", 1)[1].split("private:", 1)[0]
        for expected in ("player_events_.reaction(now)", "client_.player_reactions_ready()", "snapshot->player().in_combat()",
                         "select_observation_reactor", "std::chrono::seconds{60}", "player_events_.consume_reaction(serial)"):
            self.assertIn(expected, reaction)
        for forbidden in (".get()", "begin_context_turn", "publish_snapshot", "RE::", "REL::"):
            self.assertNotIn(forbidden, reaction)
        worker = session.split("bool queue_context_trigger(", 1)[1].split("static std::string make_runtime_session_id", 1)[0]
        self.assertIn("snapshot->captured_at() > std::chrono::seconds{2}", worker)
        self.assertIn('kind=="player_reaction"', worker)
        self.assertIn('handle_line(line, snapshot, cancellation, !one_shot, {}, combat_bark)', worker)
        entry = read("src/flat/plugin_entry.cpp")
        self.assertLess(entry.index("const auto player_history ="), entry.index("const auto player_reaction ="))
        self.assertLess(entry.index("const auto player_reaction ="), entry.index("const auto quest_reaction ="))

    def test_combat_interrupts_a_turn_without_retiring_the_runtime_session(self) -> None:
        session = read("src/client/plugin_session.hpp")
        interruption = session.split("void interrupt_dialogue_for_combat()", 1)[1].split("void hard_halt()", 1)[0]
        for expected in ("cancel_voice_capture()", "dialogue_.external_owner()",
                         'begin_player_turn(actor.value_or(""))', "dialogue_.begin_external(std::move(actor))", "mark_activity()"):
            self.assertIn(expected, interruption)
        for forbidden in ("terminal_.store", "halting_.store", "cancel_all", "hard_halt(", "retire", "invalidate",
                          "active_quest_actors_.clear", "conversation_cooldowns_.clear", "dispatcher_.discard_pending"):
            self.assertNotIn(forbidden, interruption)
        for lane in ("flat", "vr"):
            entry = read(f"src/{lane}/plugin_entry.cpp")
            combat = entry.split("if (player_in_combat && !player_was_in_combat &&", 1)[1].split("player_was_in_combat =", 1)[0]
            self.assertNotIn("enable_combat_dialogue", combat)
            self.assertIn("configuration.behavior.cancel_dialogue_on_combat", combat)
            self.assertIn("session->interrupt_dialogue_for_combat();", combat)
            self.assertNotIn("session->hard_halt()", combat)
            self.assertIn("voice_capture_down = false", combat)
            binding = "hotkeys.hard_halt" if lane == "flat" else "configuration.hotkeys.hard_halt"
            manual_halt = entry.split(f"hotkey_pressed({binding})", 1)[1].split("return;", 1)[0]
            self.assertIn("session->hard_halt();", manual_halt)

    def test_microphone_turn_publishes_owned_context_before_dialogue(self) -> None:
        session = read("src/client/plugin_session.hpp")
        voice = session.split("bool end_voice_capture(", 1)[1].split("prepare_chat_target(const Snapshot& snapshot)", 1)[0]
        self.assertIn("const auto controls = voice->controls;", voice)
        self.assertNotIn("control_state().selection", voice)
        self.assertLess(voice.index("enrich_selected_actor(snapshot, target)"), voice.index("requests_.submit_voice("))
        self.assertLess(voice.index("requests_.submit_voice("), voice.index("dialogue_.own_external_work(actor_key(target))"))
        requests = read("src/client/dialogue_requests.hpp")
        voice_request = requests.split("bool submit_voice(", 1)[1].split("private:", 1)[0]
        self.assertLess(voice_request.index("client_.upload_media("), voice_request.index("begin_context_turn()"))
        self.assertLess(voice_request.index("begin_context_turn()"), voice_request.index("publish_(snapshot,"))
        self.assertLess(voice_request.index("publish_(snapshot,"), voice_request.index("client_.send_audio("))
        self.assertIn("context_outcome.context_binding, controls", voice_request)
        self.assertIn("SYNTH voice context failed:", voice_request)
        self.assertIn("SYNTH network queue is full; voice input discarded", voice_request)
        self.assertIn("holds_.suspend(cancellation);", voice_request)
        self.assertIn("deadline, std::move(turn),", voice_request)
        dialogue = read("src/client/player_dialogue.hpp")
        recording = dialogue.split("std::optional<VoiceTurn> end_voice_capture(", 1)[1].split("pump_open_microphone(", 1)[0]
        self.assertIn("std::exchange(voice_target_, std::nullopt)", recording)
        self.assertIn("conversation_target(*snapshot, controls, &*retained,", recording)
        self.assertIn("const auto controls = voice_controls_", recording)
        self.assertIn("voice_turn_};", recording)
        self.assertNotIn("current_controls()", recording)
        for lane in ("flat", "vr"):
            entry = read(f"src/{lane}/plugin_entry.cpp")
            self.assertIn("session->begin_voice_capture(snapshot)", entry)
            self.assertIn("session->bind_voice_target(snapshot)", entry)
            self.assertIn("session->voice_target_form_id()", entry)
            self.assertIn("runtime->capture_actor_snapshot(now, actor)", entry)
            self.assertIn("session.get() != owner || runtime->generation() != generation", entry)
            capture = entry.split("const auto capture_voice_scene =", 1)[1].split("const auto voice_held", 1)[0]
            self.assertIn("if (!snapshot)", capture)
            self.assertIn("session->cancel_voice_capture()", capture)
            self.assertIn("voice_capture_down = false", capture)
        text_admission = session.split("bool submit_text(", 1)[1].split("bool external_ready()", 1)[0]
        self.assertIn("return queue_text_for_actor(std::move(snapshot), std::move(turn->text), target, turn->controls, turn->intent);", text_admission)
        self.assertIn("dialogue_.admit_text(*snapshot, std::move(text), retained, client_.native_turn_ready(),", text_admission)
        text_turn = session.split("bool queue_text_for_actor(", 1)[1].split("static std::optional<std::string> clipboard_text", 1)[0]
        self.assertLess(text_turn.index("enrich_selected_actor(snapshot, target)"), text_turn.index("requests_.submit_text("))
        self.assertLess(text_turn.index("requests_.submit_text("), text_turn.index("claim_external_work(actor_key(target))"))
        text_request = requests.split("bool submit_text(", 1)[1].split("bool submit_voice(", 1)[0]
        self.assertLess(text_request.index("begin_context_turn()"), text_request.index("publish_(snapshot,"))
        self.assertLess(text_request.index("publish_(snapshot,"), text_request.index("client_.send_text("))
        self.assertIn("context_outcome.context_binding, controls, intent", text_request)
        self.assertIn("SYNTH network queue is full; text input discarded", text_request)
        self.assertIn("deadline, std::move(turn),", text_request)
        wiring = session.split("requests_{{client_, network_, terminal_, conversation_holds_,", 1)[1].split("speech_{", 1)[0]
        for expected in ("publish_snapshot(snapshot, target, std::move(cancellation), nullptr, {}, {}, {}, controls)",
                         "handle_line(line, snapshot, cancellation); }", "return notify_social_input(outcome);"):
            self.assertIn(expected, wiring)
        for forbidden in ("RE::", "REL::", "PluginSession", "IFalloutRuntime"):
            self.assertNotIn(forbidden, requests)
        self.assertIn(".protocol_version = 2", session)
        context = read("src/client/context_publication.hpp")
        publication = context.split("RequestOutcome publish(", 1)[1].split("private:", 1)[0]
        self.assertIn("client_.publish_context(0,", publication)
        self.assertNotIn("selected_target(", publication)
        # Social hydration and response rollback stay session-owned; the publisher reaches them only by hooks.
        self.assertLess(publication.index("*published_audience = std::move(audience)"), publication.index("hydrate_social_(outcome, snapshot);"))
        self.assertLess(publication.index("hydrate_social_(outcome, snapshot);"), publication.index("invalidate_response_(cancellation);"))
        self.assertLess(publication.index("save_context_->acknowledge(save_binding_.epoch,"), publication.index("history_checkpoint_acknowledged_.compare_exchange_weak("))
        self.assertLess(publication.index("history_checkpoint_acknowledged_.compare_exchange_weak("), publication.index("acknowledged_inventory_frame_.compare_exchange_weak("))
        for forbidden in ("social_mutex_", "social_tasks_.", "PluginSession", "RE::", "REL::", "IFalloutRuntime"):
            self.assertNotIn(forbidden, context)
        forwarder = session.split("RequestOutcome publish_snapshot(", 1)[1].split("void hydrate_social_tasks(", 1)[0]
        self.assertIn("return context_.publish(owned_snapshot, target, std::move(cancellation), published_audience, quest_events,", forwarder)
        self.assertIn("[this](core::CancellationToken cancellation) { invalidate_response(std::move(cancellation)); }", session)
        self.assertIn("hydrate_social_tasks(outcome, snapshot);", session)
        self.assertLess(session.index("    voice::SampleWorker voice_samples_;"), session.index("    ContextPublication context_;"))
        for moved in ("world_context_mutex_", "history_checkpoint_", "acknowledged_world_", "save_binding_", "save_context_"):
            self.assertNotIn(moved, session)
        self.assertIn('}, pending.binding, std::move(inventory), std::move(transfer),', session)
        self.assertIn('action.name=="give_caps_to" || action.name=="take_caps_from_player", action.name=="take_caps_from_player", action.name=="take_item_from_player", action.name=="open_inventory")', session)
        self.assertIn("handle_line(continuation, continuation_snapshot, cancellation)", session)
        self.assertIn("pending_rechat_ = PendingRechat{*actor, snapshot,", session)
        self.assertIn("response_binding(line, cancellation)", session)
        self.assertIn("{line.generation, line.context_sequence, cancellation, line.turn_id, {},", session)
        self.assertIn("invalidate_response(cancellation)", session)
        self.assertIn("{line.request_id, line.line_id}", session)
        self.assertIn("client_.uses_rechat_parent_contract() ? std::optional{rechat.parent}", session)
        self.assertIn("deadline, begin_player_turn(actor_key(target)))", text_turn)
        self.assertIn("deadline, voice->turn))", voice)
        self.assertEqual(dialogue.count("voice_turn_ = begin_turn();"), 2)
        self.assertEqual(session.count("[this] { return begin_player_turn(); }"), 2)
        # Includes deferred prefetch delivery and the separately owned social-task progress response.
        self.assertEqual(session.count("requests_.track_response("), 6)
        self.assertEqual(context.count("requests_.track_response("), 1)
        self.assertEqual(requests.count("track_activity(responses_,"), 3)
        self.assertIn("std::size_t responses() const noexcept { return responses_.load(std::memory_order_acquire); }", requests)
        self.assertNotIn("std::atomic_size_t& responses()", requests)
        self.assertNotIn("requests_.responses().", session)
        self.assertNotIn("active_responses_", session)
        self.assertIn('playing->caption == pending_rechat_->caption && pending_rechat_->response_complete', session)
        self.assertIn('pending_actions_.empty() && !rechat_capture_', session)
        self.assertIn('prefetch->append(line);', session)
        self.assertIn('if (prefetch->claim_media(line)) speech_.prepare_rechat_media(line, prefetch);', session)
        capture_target = session.split('rechat_capture_target() {', 1)[1].split('void submit_rechat_capture(', 1)[0]
        self.assertIn('if (runtime_paused() || player_voice_active()) return {};', capture_target)
        self.assertIn('bool player_voice_active() const { return dialogue_.voice_active(); }', session)
        voice_priority = dialogue.split('bool voice_active() const {', 1)[1].split('}', 1)[0]
        self.assertIn('ambient_player_voice_active(microphone_.recording(), open_mic_passive_recording_, open_mic_voice_detected_)', voice_priority)
        ambient_voice = read('src/client/ambient_exchange.hpp').split('bool ambient_player_voice_active(', 1)[1].split('bool ambient_actor_class_supported(', 1)[0]
        self.assertIn('return recording && (!passive_open_mic || voice_detected);', ambient_voice)
        manual_voice = dialogue.split('bool begin_voice_capture(', 1)[1].split('void cancel_voice_capture()', 1)[0]
        self.assertIn('open_mic_passive_recording_ = false;', manual_voice)
        passive_voice = dialogue.split('OpenMicrophonePump pump_open_microphone(', 1)[1].split('[[nodiscard]]', 1)[0]
        self.assertIn('open_mic_passive_recording_ = true;', passive_voice)
        self.assertNotIn('is_cancelled() || runtime_paused()', capture_target)
        self.assertIn('diagnostics::ConversationStage::prefetch_received', session)
        deferred = session.split('else if (auto lines = deferred.prefetch->take(now))', 1)[1].split('if (runtime_paused_', 1)[0]
        self.assertIn('rechat_capture_ = std::move(deferred)', deferred)
        self.assertNotIn('handle_line(', deferred)
        release = session.split('if (rechat->prefetched_lines) {', 1)[1].split('rechat->snapshot=std::move(snapshot)', 1)[0]
        self.assertIn('selected_target(*snapshot,false,&retained).has_value()', release)
        self.assertLess(release.index('snapshot->captured_at()>std::chrono::seconds{2}'), release.index('prefetch->release('))
        self.assertLess(release.index('prefetch->release('), release.index('handle_line('))
        self.assertIn('!available(action->actor)', release)
        self.assertIn('!available(*action->target)', release)
        self.assertIn('prefetch->cancel(); return;', release)
        self.assertIn("audio_.post_clip_ms, cancellation", read("src/client/speech_delivery.hpp"))
        admission = session.split("core::CancellationToken begin_player_turn(std::string actor = {})", 1)[1].split(
            "request_timeout_ms()", 1)[0]
        self.assertNotIn("transport_", admission)
        self.assertIn("dialogue_.cancel_interrupted_turn();", admission)
        cancel_dispatch = dialogue.split("void cancel_interrupted_turn() {", 1)[1].split("void begin_external(", 1)[0]
        self.assertIn("client_.take_interrupted_turn()", cancel_dispatch)
        self.assertNotIn("transport_", cancel_dispatch)
        self.assertNotIn("shutdown", cancel_dispatch)
        capabilities = read("src/client/context_publication.hpp").split("static std::vector<std::string> capabilities(", 1)[1].split("return result;", 1)[0]
        self.assertIn('"dialogue.rechat.ownership"', capabilities)
        self.assertLess(capabilities.index('"dialogue.rechat.ownership"'), capabilities.index("if (variant"))
        flat_capabilities = capabilities.split("if (variant == core::RuntimeVariant::flat) {", 1)[1].split("}", 1)[0]
        self.assertIn('"action.inventory_observation"', flat_capabilities)
        self.assertEqual(capabilities.count('"action.inventory_observation"'), 1)
        execution = session.split("void execute_action(PendingAction pending)", 1)[1].split(
            "static protocol_native::Identity identity(", 1)[0]
        self.assertLess(execution.index("!client_.equipment_actions_ready()"), execution.index("dispatcher_.try_enqueue("))
        self.assertIn('control_.try_submit("control"', cancel_dispatch)
        self.assertLess(cancel_dispatch.index("[this, anchor = *anchor]"), cancel_dispatch.index("client_.cancel("))
        client = read("src/client/synth_client.hpp")
        control_rpc = client.split("RequestOutcome cancel(", 1)[1].split("RequestOutcome send_action_result", 1)[0]
        self.assertNotIn("request_mutex_", control_rpc)
        self.assertIn("expected_generation != generation_", control_rpc)
        self.assertNotIn("shutdown", admission)
        self.assertIn("network_.discard_cancelled()", admission)
        transport = read("src/client/winhttp_transport.hpp")
        self.assertIn("WINHTTP_FLAG_ASYNC", transport)
        cancel = transport.split("void cancel_all()", 1)[1].split("private:", 1)[0]
        self.assertIn("cancellation_epoch_.fetch_add", cancel)
        self.assertNotIn("WinHttpCloseHandle", cancel)
        self.assertNotIn("mutex", cancel)
        self.assertIn("WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING", transport)
        self.assertIn("now >= deadline", transport)
        self.assertIn("publish_snapshot(snapshot, subject, cancellation)", session)

    def test_windows_detection_is_powershell_51_safe(self) -> None:
        for relative in self.scripts:
            with self.subTest(script=relative):
                script = read(relative)
                self.assertIn("Get-Variable -Name IsWindows", script)
                self.assertIn("$env:OS -eq 'Windows_NT'", script)
                self.assertNotRegex(script, r"if \(-not \$IsWindows\)")

    def test_consume_is_flat_only_and_requires_its_acknowledged_receipt_contract(self) -> None:
        session = read("src/client/plugin_session.hpp")
        capabilities = read("src/client/context_publication.hpp").split("static std::vector<std::string> capabilities(", 1)[1].split("return result;", 1)[0]
        flat_capabilities = capabilities.split("if (variant == core::RuntimeVariant::flat) {", 1)[1].split("}", 1)[0]
        for capability in ('"action.consume"', '"action.consume_inventory"'):
            self.assertIn(capability, flat_capabilities)
            self.assertEqual(capabilities.count(capability), 1)
        execution = session.split("void execute_action(PendingAction pending)", 1)[1].split(
            "static protocol_native::Identity identity(", 1)[0]
        self.assertLess(execution.index("!client_.consumption_ready()"), execution.index("dispatcher_.try_enqueue("))
        self.assertIn("RuntimeActionName::consume", execution)
        self.assertIn('(action.name=="consume" && client_.consumption_ready())', execution)
        native = read("src/adapters/commonlib_actions.hpp")
        self.assertIn("case runtime::RuntimeActionName::consume:", native)
        self.assertNotIn("actor->DrinkPotion(", native)
        self.assertNotIn("REL::ID(2698330)", native)
        self.assertIn("main->threadID != REX::W32::GetCurrentThreadId()", native)
        self.assertIn("REL::ID(2231345)", native)
        self.assertIn("REL::ID(2226295)", native)
        self.assertLess(native.index("main->threadID == 0"), native.index("cast(consumable_caster, &parameters)"))
        self.assertLess(native.index("now() >= request.deadline"), native.index("cast(consumable_caster, &parameters)"))
        self.assertIn("actions::consumption_postcondition(before->items, after->items, item)", native)
        self.assertIn("return finish(std::move(result))", native)

    def test_diary_menu_uses_the_retained_target_and_existing_owned_request(self) -> None:
        menu = read("src/ui/control_menu.hpp")
        self.assertIn('Write Diary  [', menu)
        self.assertIn('control_submit(ControlAction::diary_target, target)', menu)
        entry = read("src/flat/plugin_entry.cpp")
        execution = entry.split("if (deferred_control_action && !menu_mode", 1)[1].split("if(pending_control_wait", 1)[0]
        self.assertIn("request->action == synth::ui::ControlAction::diary_target", execution)
        self.assertIn("target->session.generation() == generation", execution)
        self.assertLess(execution.index("targeted && !actor"), execution.index("session->request_npc_diary(snapshot, &*target)"))
        self.assertIn("request->generation != generation.value()", execution)

    def test_diary_hotkey_keeps_crosshair_identity_and_blocks_menu_input(self) -> None:
        entry = read("src/flat/plugin_entry.cpp")
        execution = entry.split("if (hotkey_pressed(hotkeys.write_diary)", 1)[1].split(
            "if (hotkey_pressed(hotkeys.synth_control)", 1)[0]
        for guard in ("!menu_mode", "!regular_dialogue_blocked", "!synth::ui::MenuFramework::input_captured()",
                      "session.get() != owner || runtime->generation() != generation",
                      "!snapshot->picked_actor_form_id()", "target->form_id != *snapshot->picked_actor_form_id()"):
            self.assertIn(guard, execution)
        self.assertLess(execution.index("target->form_id !="),
                        execution.index("session->request_npc_diary(snapshot, &*target)"))
        self.assertIn("RuntimeCapturePurpose::dialogue", execution)

    def test_all_native_commands_use_checked_wrapper(self) -> None:
        direct_command = re.compile(r"(?m)^\s*(?:&\s*)?(git|cmake|ctest|xmake)\b")
        for relative in self.scripts:
            with self.subTest(script=relative):
                script = read(relative)
                self.assertIn("function Invoke-NativeCommand", script)
                self.assertIn("$LASTEXITCODE", script)
                self.assertIsNone(direct_command.search(script))

    def test_transfer_requires_paired_native_evidence_and_flat_advertisement(self) -> None:
        session = read("src/client/plugin_session.hpp")
        capabilities = read("src/client/context_publication.hpp").split("static std::vector<std::string> capabilities(", 1)[1].split("return result;", 1)[0]
        self.assertNotIn('"action.give_item"', capabilities)
        flat = capabilities.split("if (variant == core::RuntimeVariant::flat) {", 1)[1].split("if (variant == core::RuntimeVariant::vr)", 1)[0]
        for capability in ('"action.give_item_to"', '"action.transfer_inventory"',
                           '"action.give_caps_to"', '"action.caps_inventory"',
                           '"action.take_caps_from_player"', '"action.player_caps_inventory"'):
            self.assertIn(capability, flat)
            self.assertEqual(capabilities.count(capability), 1)
        self.assertIn("(void)dialogue_.cancel_turn_if_owner(cancellation);", session)
        self.assertIn("turn_cancellation_.cancel_if_owner(cancellation)", read("src/client/player_dialogue.hpp"))
        self.assertIn("client_.cancel(pending.binding.context_request_id,pending.binding.generation)", session)
        self.assertLess(session.index("!client_.transfer_ready()"), session.index("prepare_transfer_action(*snapshot"))
        self.assertLess(session.index("!client_.caps_ready()"), session.index("prepare_transfer_action(*snapshot"))
        self.assertLess(session.index("!client_.player_caps_ready()"), session.index("prepare_transfer_action(*snapshot"))
        self.assertIn("if (request_valid) {", session)
        self.assertIn("completion->result->transfer->valid_for(request)", session)
        self.assertIn("prepare_transfer_inventory(snapshot,action,cancellation,transfer_observation,client_.extended_inventory_ready())", session)
        self.assertIn("continuation_snapshot=std::move(prepared.snapshot);transfer=std::move(prepared.transfer)", session)
        self.assertIn("handle_line(continuation, continuation_snapshot, cancellation)", session)
        self.assertIn("pending.binding, std::move(inventory), std::move(transfer)", session)
        native = read("src/adapters/commonlib_actions.hpp")
        transfer = native.split("case runtime::RuntimeActionName::give_item: {", 1)[1].split(
            "case runtime::RuntimeActionName::equip_item:", 1)[0]
        self.assertIn("#if defined(SYNTH_WITH_F4SEVR)", transfer)
        self.assertIn("remove.stackData.push_back(stack.index)", transfer)
        self.assertIn("remove.otherContainer = recipient", transfer)
        self.assertLess(transfer.index("now() >= request.deadline"), transfer.index("actor->RemoveItem(remove)"))
        self.assertIn("actions::transfer_postcondition", transfer)
        self.assertNotIn("BGSDefaultObjectManager::GetSingleton()", transfer)
        self.assertIn("REL::ID(4796209)", transfer)
        self.assertIn("bytes[0xC83] != 1", transfer)
        self.assertIn("bytes + 0x38", transfer)
        self.assertIn("currency != object", transfer)
        self.assertIn("const bool taking_caps = request.name == runtime::RuntimeActionName::take_caps", transfer)
        self.assertIn("taking ? actor != player : actor == player", transfer)
        self.assertIn("RuntimeActionName::give_caps || taking", transfer)
        self.assertLess(transfer.index("currency != object"), transfer.index("actor->RemoveItem(remove)"))
        self.assertIn("result.transfer->donor", transfer)
        self.assertIn("result.transfer->recipient", transfer)
        self.assertNotIn("AddObjectToContainer(", transfer)

    def test_equipped_transfer_has_one_immediate_owned_unequip_before_remove(self) -> None:
        native = read("src/adapters/commonlib_actions.hpp")
        transfer = native.split("case runtime::RuntimeActionName::give_item: {", 1)[1].split(
            "case runtime::RuntimeActionName::equip_item:", 1)[0]
        unequip = transfer.index("equipment->UnequipObject(")
        verify = transfer.index("actions::transfer_unequip_postcondition(")
        remove = transfer.index("actor->RemoveItem(remove)")
        self.assertLess(unequip, verify)
        self.assertLess(verify, remove)
        self.assertIn("stack.index, false, false, true, true, nullptr", transfer)
        self.assertIn("unequipped->stacks[*refreshed].extra != stack.extra", transfer)
        self.assertIn("candidate->IsEquipped() == item.equipped", transfer)
        self.assertIn("request.name != runtime::RuntimeActionName::give_item", transfer)
        self.assertLess(transfer.index("quest-alias inventory cannot be transferred"), unequip)
        resumed = transfer[verify:remove]
        self.assertIn("before = std::move(unequipped)", resumed)
        self.assertIn("validate_stack()", resumed)
        self.assertIn("return finish(*invalid)", resumed)
        self.assertIn("validate_owner()", transfer[unequip:verify])
        self.assertIn("now() >= request.deadline", transfer[unequip:verify])
        self.assertIn("request.cancellation.is_cancelled()", transfer[unequip:verify])
        self.assertIn("epoch != FlatPickedReference::stamp()", transfer[unequip:verify])
        self.assertIn("return finish(std::move(result))", transfer)
        self.assertIn("const auto finish =", transfer)

    def test_native_inventory_postconditions_recheck_load_boundary(self) -> None:
        native = read("src/adapters/commonlib_actions.hpp")
        transfer = native.split("(void)actor->RemoveItem(remove);", 1)[1].split("return result;", 1)[0]
        equipment = native.split("bool accepted{};", 1)[1].split("return finish(std::move(result));", 1)[0]
        for body, reader in ((transfer, "actor"), (transfer, "recipient"), (equipment, "actor")):
            with self.subTest(reader=reader, equipment=body is equipment):
                self.assertRegex(body, r"epoch == FlatPickedReference::stamp\(\) && !menu_mode_active\(\)\s*"
                                      r"\? equipment_inventory\(\*" + reader + r", \*object\) : std::nullopt")
        self.assertIn("if (epoch == FlatPickedReference::stamp() && !menu_mode_active() &&", transfer)
        self.assertIn("const bool confirmed = epoch == FlatPickedReference::stamp() && !menu_mode_active() &&", equipment)
        self.assertEqual(native.count("if (!request.cancellation.is_cancelled() && epoch == FlatPickedReference::stamp() && !menu_mode_active())"), 2)

    def test_pickup_package_preparation_stays_flat_and_uninstalled(self) -> None:
        package = read("src/adapters/flat_pickup_package.hpp")
        self.assertIn("#if defined(SYNTH_WITH_F4SEVR)", package)
        self.assertIn("runtime.assert_game_thread()", package)
        self.assertIn("REL::Version{1, 11, 240, 0}", package)
        for binding in (2211661, 2211916, 4480239, 2211917, 2211735):
            self.assertIn(f"REL::ID({binding})", package)
        self.assertIn("auto* created = create(6)", package)
        self.assertIn("initialize(package.get(), nullptr)", package)
        self.assertIn("(void)package.release()", package)
        self.assertLess(package.index("if (package->packLoc == nullptr)"),
                        package.index("location + 0x18"))
        for actor_call in ("PutCreatedPackage(", "SetRunOncePackage(", "PickupObject(", "PickUpObject("):
            self.assertNotIn(actor_call, package)
        self.assertNotIn("flat_pickup_package.hpp", read("src/vr/plugin_entry.cpp"))

    def test_pickup_inspection_stays_read_only_and_flat(self) -> None:
        inspection = read("src/adapters/flat_pickup_inspection.hpp")
        self.assertIn("#if defined(SYNTH_WITH_F4SEVR)", inspection)
        self.assertIn("runtime.assert_game_thread()", inspection)
        self.assertIn("REL::Version{1,11,240,0}", inspection)
        self.assertIn("epoch != FlatPickedReference::stamp()", inspection)
        self.assertIn("actions::pickup_reference_matches(request.item, current)", inspection)
        self.assertIn("point(actor->GetPosition())", inspection)
        self.assertIn("distance <= 128.0", inspection)
        self.assertIn("inventory_metadata(*base, reference->extraList", inspection)
        self.assertIn("RE::VTABLE::ExtraAmmo[0]", inspection)
        self.assertIn("struct PickupInspection final", inspection)
        self.assertIn("std::optional<core::InventoryItemSnapshot> ammunition", inspection)
        self.assertIn("metadata->instance ? metadata->instance.get() : &weapon->weaponData", inspection)
        self.assertIn("RE::VTABLE::TESObjectWEAP__InstanceData[0]", inspection)
        self.assertIn("RE::VTABLE::TESObjectWEAP__Data[0]", inspection)
        self.assertIn("ammo->GetFormID(), origin_plugin(*ammo), ammo_metadata->name", inspection)
        self.assertIn("std::move(ammunition)", inspection)
        self.assertIn('if (!middle_high) return {Inspection::unavailable}', inspection)
        self.assertIn("middle_high->currentFurniture || middle_high->occupiedFurniture || middle_high->reservationSlot >= 0", inspection)
        self.assertIn("std::optional<std::vector<core::InventoryItemSnapshot>> inventory", inspection)
        self.assertIn("inventory_snapshot(*actor, std::min(request.deadline, started + std::chrono::milliseconds{2}))", inspection)
        self.assertIn('if (captured.second != "complete") return {Inspection::unavailable}', inspection)
        self.assertIn("count_vtable.address(), ammo_vtable.address()", inspection)
        self.assertIn("if (!metadata->reference_loaded_ammo)", inspection)
        self.assertIn("static_cast<std::uint64_t>(*metadata->reference_loaded_ammo) * metadata->reference_count > 2147483647U", inspection)
        self.assertIn("request.actor.playthrough_id != playthrough_id()", inspection)
        self.assertIn("manager->currentPlayerID == 0", inspection)
        for mutation in ("PickupObject(", "PickUpObject(", "PutCreatedPackage(", "SetRunOncePackage(", "RemoveItem(", "GetHandle("):
            self.assertNotIn(mutation, inspection)
        self.assertNotIn("flat_pickup_inspection.hpp", read("src/vr/plugin_entry.cpp"))

    def test_full_suite_runs_before_plugin_entry_gate(self) -> None:
        flat = read("scripts/build-flat.ps1")
        for suite in EXPECTED_SUITES:
            with self.subTest(script="flat", suite=suite):
                target = f"synth-{suite}-tests"
                self.assertIn(target, flat)
                self.assertLess(flat.index(target), flat.index("$Entry ="))
        self.assertIn("synth-protocol-native-tests", flat)
        self.assertIn("foreach ($TestSuite in $TestSuites)", flat)
        self.assertLess(
            flat.index("@('run', $TestSuite)"),
            flat.index("$Entry ="),
        )

        vr = read("scripts/build-vr.ps1")
        self.assertLess(
            vr.index("@('--build', $BuildDirectory, '--config', $Configuration)"),
            vr.index("$Entry ="),
        )
        self.assertLess(vr.index("-FilePath ctest"), vr.index("$Entry ="))

    def test_vr_script_passes_vcpkg_toolchain(self) -> None:
        vr = read("scripts/build-vr.ps1")
        self.assertIn("VCPKG_ROOT", vr)
        self.assertIn("scripts\\buildsystems\\vcpkg.cmake", vr)
        self.assertIn("-DCMAKE_TOOLCHAIN_FILE=", vr)
        # The VR package ships no spdlog.dll/fmt.dll, so they must link statically.
        self.assertEqual(vr.count("'-DVCPKG_TARGET_TRIPLET=x64-windows-static-md'"), 2)
        self.assertNotIn("'-DVCPKG_TARGET_TRIPLET=x64-windows'", vr)

    def test_vr_ctest_runs_after_vr_target_build(self) -> None:
        vr = read("scripts/build-vr.ps1")
        final_configure = vr.rindex("-DSYNTH_BUILD_VR=ON")
        final_build = vr.rindex("'--target', 'SYNTHVR'")
        final_ctest = vr.rindex("-FilePath ctest")
        self.assertLess(final_configure, final_build)
        self.assertLess(final_build, final_ctest)


class WaitPackageGates(unittest.TestCase):
    def test_native_menu_pause_is_owned_and_released_before_capture(self):
        adapter = read('src/adapters/flat_menu_pause.hpp')
        host = read('src/flat/plugin_entry.cpp')
        self.assertIn('UI_MENU_FLAGS::kPausesGame', adapter)
        self.assertIn('UI_MESSAGE_TYPE::kForceHide', adapter)
        self.assertNotIn('freezeTime', adapter)
        self.assertNotIn('menuMode =', adapter)
        start = host.index('void pump_runtime_impl()')
        pump = host[start:host.index('void pump_runtime() noexcept', start)]
        self.assertLess(pump.index('FlatMenuPause::sync'), pump.index('world_ready_for_capture'))
        self.assertIn('menu_mode ? std::nullopt : synth::ui::Chatbox::take_submission()', pump)
        self.assertIn('caption ? caption->facing', pump)

    def test_wait_requires_owned_alias_package_and_physical_completion(self):
        adapter=read('src/adapters/flat_wait_package.hpp')
        script=read('scripts/papyrus/SYNTHWait.psc')
        for forbidden in ('PutCreatedPackage','SetPlayerTeammate','ProcessLists::GetSingleton()'):
            self.assertNotIn(forbidden,adapter)
        self.assertIn('return pending(releasing?',adapter)
        self.assertIn('same_wait_request(command->second.request,request)',adapter)
        self.assertIn('value.package_owned=value.owned_package!=0&&value.owned_package==lease.package',adapter)
        self.assertIn('const auto active=active_package_id(actor)',adapter)
        self.assertIn('lease.settling&&lease.position_guard.anchored()&&lease.settle_clock.observe(state,now)',adapter)
        self.assertIn('lease.position_guard.interrupted(state)',adapter)
        self.assertIn('lease.position_guard.observe_active_package(state,owner_current)',adapter)
        self.assertNotIn('!state.actor_available||!state.same_cell||!std::isfinite(state.distance)||state.distance>64',adapter)
        self.assertIn('result.wait->confirms_success(request)',adapter)
        self.assertIn('state.released()',adapter)
        self.assertIn('lease.hold_clock.expired(now)',adapter)
        self.assertIn('lease.hold_clock.pause(now-*paused_at_)',adapter)
        self.assertIn('holder.package == old',adapter)
        self.assertIn('if (owned) actor.currentProcess->SetRunOncePackage(nullptr, &actor)',adapter)
        hold=script.split('Bool Function HoldActor(',1)[1].split('EndFunction',1)[0]
        self.assertIn('Return False',hold)
        self.assertNotIn('SetRestrained',hold)
        self.assertNotIn('SetRestrained(True)',script)
        for method in ('Apply','ReleaseByFormIds','RefreshByFormIds'):
            function=script.split('Function '+method+'(',1)[1].split('EndFunction',1)[0]
            self.assertNotIn('SetRestrained',function)
            self.assertNotIn('ReleaseRestraint(',function)
        self.assertEqual(script.count('ReleaseRestraint(ticket, restraintOwner)'),1)
        for forbidden in ('subject.MoveTo','anchor.MoveTo','SetPlayerTeammate','SetFactionRank'):
            self.assertNotIn(forbidden,script)

    def test_wait_dispatch_is_scalar_and_release_preserves_original_identity(self):
        adapter=read('src/adapters/flat_wait_package.hpp')
        dispatch=adapter.split('bool queued{};',1)[1].split('NativeFaultRecorder::phase',1)[0]
        for method in ('ResetByFormId','ApplyByFormIds','RefreshByFormIds','ReleaseByFormIds'):
            self.assertIn('"'+method+'",callback,ticket',dispatch)
        self.assertIn('std::bit_cast<std::int32_t>(lease.form)',adapter)
        for borrowed in ('actor.get()', 'callback,ticket,owner)', 'owner,actor', 'anchor,cell'):
            self.assertNotIn(borrowed,dispatch)
        script=read('scripts/papyrus/SYNTHWait.psc')
        for method in ('ApplyByFormIds','ResetByFormId','ReleaseByFormIds'):
            function=script.split('Function '+method+'(',1)[1].split('EndFunction',1)[0]
            signature=function.split(') Global',1)[0]
            for borrowed in ('Quest ','Actor ','ObjectReference ','Cell '):
                self.assertNotIn(borrowed,signature)
            self.assertLess(function.index('If Current(ticket)'),function.index('Game.GetForm'))
        release=script.split('Function ReleaseByFormIds(',1)[1].split('EndFunction',1)[0]
        self.assertIn('previous.GetFormID() == subjectId',release)
        self.assertIn('legacyOwner.GetReference() == None',release)
        self.assertIn('target.GetReference() == None && legacyOwner.GetReference() == None',release)
        self.assertIn('Function Reset(String ticket, Quest owner) Global',script)
        self.assertIn('Function Apply(String ticket, Int slot, Quest owner, Actor subject, ObjectReference anchor, Cell startCell,',script)

    def test_wait_vm_timeout_cannot_release_or_reuse_an_undrained_stack(self):
        source=read('src/adapters/flat_wait_package.hpp')
        timeout=source.split('std::optional<Job> completed;bool draining{},warn{};',1)[1].split('if(completed)',1)[0]
        self.assertIn('if(!job_->receipt.done())',timeout)
        self.assertIn('job_->receipt.revoke_install()',timeout)
        self.assertIn('else {completed=std::move(job_);job_.reset();}',timeout)
        self.assertIn('if(draining)',timeout)
        self.assertIn('return;',timeout)
        callback=source.split('static void finished(',1)[1].split('static void observed(',1)[0]
        self.assertIn('job_->ticket==ticket',callback)
        self.assertIn('else recovery_requested_=true',callback)
        self.assertNotIn('job_.reset()',callback)
        self.assertIn('static void cancel_all()',source)
        self.assertIn('static void note_menu_pause(',source)

    def test_stale_wait_stack_cannot_clear_restraint_without_a_current_receipt(self):
        script=read('scripts/papyrus/SYNTHWait.psc')
        hold=script.split('Bool Function HoldActor(',1)[1].split('EndFunction',1)[0]
        for branch in hold.split('If !Current(ticket)\n')[1:]:
            self.assertNotIn('SetRestrained(False)',branch.split('EndIf',1)[0],
                'Known blocking race: a restored/cancelled stack can clear newer or foreign restraint')
        legacy=script.split('Bool Function ReleaseRestraint(',1)[1].split('EndFunction',1)[0]
        before=legacy.split('previous.SetRestrained(False)',1)[0]
        self.assertIn('If !Current(ticket) || restraintOwner.GetReference() != previous',before)
        self.assertEqual(script.count('SetRestrained(False)'),1)

    def test_wait_reset_is_native_verified_before_admission(self):
        source=read('src/adapters/flat_wait_package.hpp')
        inventory=source.split('static std::optional<AliasInventory> alias_inventory()',1)[1].split('static void request_recovery(',1)[0]
        self.assertIn('const TryReadLock lock{owner->aliasAccessLock}',inventory)
        self.assertIn('owner->aliasedHandles.size()>512',inventory)
        self.assertIn('references.size()>1',inventory)
        self.assertNotIn('GetAliasedRef',inventory)
        reset=source.split('if(completed->reset)',1)[1].split('} else {',1)[0]
        self.assertIn('initialized_=false;reset_verifying_=ok',reset)
        self.assertNotIn('initialized_=ok',source)
        self.assertIn('reset_clock_.observe(state,now)',source)
        self.assertIn('remember_recovery_actors(*inventory)',source)
        self.assertIn('if(owned&&*owned!=0){request_recovery(actor);return {};}',source)
        self.assertIn('if(!*aliases){request_recovery();return {};}',source)
        self.assertIn('reset&&reset_refresh_actor_!=0',source)
        self.assertIn('Unexpected saved Wait alias requires recovery',source)

    def test_manual_chat_captures_identity_before_pause_and_revalidates_on_send(self):
        for lane in ('flat', 'vr'):
            source = read(f'src/{lane}/plugin_entry.cpp')
            for method in ('refresh_dynamic_profiles', 'checkpoint_diary', 'submit_visual_capture'):
                self.assertNotIn(f'session->{method}(runtime->capture_', source)
                call = source.index(f'session->{method}(std::move(snapshot)')
                capture = source.rindex('auto snapshot = runtime->capture_snapshot(now);', 0, call)
                self.assertIn('const auto generation = runtime->generation();', source[capture-120:capture])
                self.assertIn('if (session.get() != owner || runtime->generation() != generation) return;', source[capture:call])
        host = read('src/flat/plugin_entry.cpp')
        hotkey = host.split('hotkey_pressed(hotkeys.talk_to_npc)', 1)[1].split('hotkey_pressed(hotkeys.synth_control)', 1)[0]
        self.assertLess(hotkey.index('prepare_chat_target(snapshot)'), hotkey.index('Chatbox::request_open'))
        self.assertIn('Chatbox::request_open(*target,session->prepare_chat_targets(snapshot))', hotkey)
        self.assertIn('target.manual_chat', host)
        self.assertIn('session->submit_text(snapshot, std::move(submission->text), &target)', host)
        self.assertIn('capture_actor_snapshot(now,target.form_id)', host)
        session = read('src/client/plugin_session.hpp')
        retained = session.split('if (retained) {', 1)[1].split('if (snapshot.variant()', 1)[0]
        for guard in ('session.is_cancelled()', 'retained->matches(actor)', 'eligible(actor)', 'maximum_distance'):
            self.assertIn(guard, retained)
        self.assertIn('return {}; // Never silently retarget', retained)
        self.assertIn('request_npc_diary(std::move(snapshot), retained)', session)

    def test_chat_dispatch_and_control_retirement_have_one_owner(self):
        menu = read('src/ui/menu_framework_ui.hpp')
        self.assertNotIn('on_chatbox_hotkey', menu)
        self.assertIn('Register(binding.framework_id, 0, detail::on_hotkey)', menu)
        host = read('src/flat/plugin_entry.cpp')
        hotkey = host.split('hotkey_pressed(hotkeys.talk_to_npc)', 1)[1].split('hotkey_pressed(hotkeys.synth_control)', 1)[0]
        self.assertLess(hotkey.index('MenuFramework::available()'), hotkey.index('clipboard_text()'))
        retirement = host.split('void retire_session(', 1)[1].split('void invalidate_sessions(', 1)[0]
        for reset in ('control_target.reset()', 'deferred_control_action.reset()', 'control_status_revision = 0'):
            self.assertIn(reset, retirement)

    def test_menu_pause_keeps_behavior_without_player_notifications(self):
        session = read('src/client/plugin_session.hpp')
        method = session[session.index('void set_runtime_paused(bool paused)'):session.index('bool runtime_paused() const')]
        self.assertNotIn('push_notification', method)
        self.assertIn('speech_.set_paused', method)
        self.assertIn('cancel_voice_capture()', method)
        self.assertIn('void cancel_voice_capture() { dialogue_.cancel_voice_capture(); }', session)
        cancellation = read('src/client/player_dialogue.hpp').split('void cancel_voice_capture()', 1)[1].split('void bind_voice_target(', 1)[0]
        for operation in ('microphone_.cancel()', 'open_mic_voice_detected_ = false',
                          'voice_target_.reset()', 'external_owner_.end("")'):
            self.assertIn(operation, cancellation)

    def test_conversation_does_not_acquire_wait_package(self):
        package = read('src/adapters/flat_wait_package.hpp')
        speech = package.split('static void update_conversation(', 1)[1].split('static runtime::RuntimeActionResult apply(', 1)[0]
        self.assertNotIn('RuntimeActionName::wait_here', speech)
        self.assertNotIn('apply(*actor', speech)
        self.assertIn('if(lease.conversational&&lease.actor)close(lease,', speech)
        cleanup=package.split('static void close(',1)[1].split('static void complete_command(',1)[0]
        self.assertIn('lease.waiting=false', cleanup)
        self.assertIn('job_->receipt.revoke_install()', cleanup)

    def test_conversation_fallback_does_not_change_command_targeting(self):
        session = read('src/client/plugin_session.hpp')
        dialogue = read('src/client/player_dialogue.hpp')
        self.assertNotIn('conversation_target(', session)
        prepare = dialogue.split('std::optional<integration::PromptTarget> prepare_chat_target(', 1)[1].split('prepare_chat_targets(', 1)[0]
        self.assertIn('conversation_target(*snapshot, controls', prepare)
        voice = dialogue.split('std::optional<VoiceTurn> end_voice_capture(', 1)[1].split('pump_open_microphone(', 1)[0]
        self.assertIn('conversation_target(*snapshot, controls, &*retained,', voice)
        conversation = dialogue.split('std::optional<context::TargetContext> conversation_target(', 1)[1].split(
            'const config::Config& configuration_;', 1)[0]
        for guard in ('conversation_area_available()', 'retained->matches(actor)',
                      'retained->session.is_cancelled()', 'player_speech_audible', 'distance > range'):
            self.assertIn(guard, conversation)
        command = session.split('std::optional<context::TargetContext> selected_target(', 1)[1].split(
            'std::optional<context::TargetContext> pointed_subject(', 1)[0]
        self.assertIn('selected.target || !automatic', command)
        self.assertIn('retained->conversation_mode', dialogue)


if __name__ == "__main__":
    unittest.main()


class ConversationApproachGates(unittest.TestCase):
    def test_owned_cleanup_pumps_without_a_network_session_after_world_dwell(self):
        source=read('src/flat/plugin_entry.cpp')
        retirement=source[source.index('void retire_session('):source.index('void invalidate_sessions(')]
        self.assertLess(retirement.index('FlatConversationApproach::update({},false)'),retirement.index('if (!session) return;'))
        pump=source[source.index('void pump_runtime_impl()'):source.index('void pump_runtime() noexcept',source.index('void pump_runtime_impl()'))]
        cleanup=pump.index('FlatConversationApproach::tick(now)')
        self.assertEqual(1,pump.count('FlatConversationApproach::tick(now)'))
        self.assertLess(pump.index('world_ready_for_capture()'),cleanup)
        self.assertLess(pump.index('if (!session && menu_mode)'),cleanup)
        self.assertLess(cleanup,pump.index('adopt_session_result(now)'))
        self.assertIn('!session || !session->connection_ready() || session->halting()',pump[:cleanup])

    def test_approach_is_player_input_only_and_actions_revoke_before_queueing(self):
        source=read('src/client/plugin_session.hpp')
        requests=read('src/client/dialogue_requests.hpp')
        self.assertEqual(2,requests.count('hold_conversation_actor(target,cancellation,controls.mode)'))
        self.assertIn('conversation_approach_mode(mode)',requests)
        self.assertNotIn('hold_conversation_actor',source)
        actions=source[source.index('const auto* action = std::get_if<protocol_native::Action>'):source.index('// Continuations retain the same rollback owner')]
        self.assertLess(actions.index('conversation_holds_.suspend(cancellation)'),actions.index('pending_actions_.push_back'))
        modes=read('src/client/conversation_holds.hpp')
        self.assertIn('entry.hold.approach_player=entry.hold.approach_player || approach_player',modes)

    def test_vm_timeout_does_not_drop_undrained_alias_ownership(self):
        source=read('src/adapters/flat_conversation_approach.hpp')
        timeout=source[source.index('bool draining{}, report_timeout{}'):source.index('if (completed) {')]
        self.assertIn('if (!job_->receipt.done())',timeout)
        self.assertIn('job_->receipt.revoke_install()',timeout)
        self.assertIn('else {completed=std::move(job_);job_.reset();}',timeout)
        self.assertIn('if (draining)',timeout)
        self.assertIn('return; // No alias release or reuse',timeout)

    def test_unknown_saved_completion_requests_recovery_without_replacing_a_live_owner(self):
        source=read('src/adapters/flat_conversation_approach.hpp')
        callback=source[source.index('static void finished('):source.index('static void observed(')]
        self.assertIn('job_->ticket==ticket',callback)
        self.assertIn('else recovery_requested_=true',callback)
        self.assertNotIn('job_.reset()',callback)
        recovery=source[source.index('if (recovery_requested_) {'):source.index('// EvaluatePackage may finish')]
        self.assertIn('if (job_ && !job_->receipt.done())',recovery)
        self.assertIn('job_->receipt.revoke_install()',recovery)
        self.assertIn('controller_.reset();initialized_=false',recovery)
        self.assertIn('if (recovery_requested_) return;',source)
