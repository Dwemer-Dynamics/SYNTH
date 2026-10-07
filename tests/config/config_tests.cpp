#include "config/config.hpp"
#include "ui/menu_framework_ui.hpp"
#include "ui/settings_catalog.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace synth::config;
int assertions{};
#define CHECK(value) do { ++assertions; if (!(value)) throw std::runtime_error{std::string{"CHECK failed: "} + #value}; } while (false)

template <class Function>
void rejects(Function&& function) {
    ++assertions;
    try { std::forward<Function>(function)(); } catch (const ConfigError&) { return; }
    throw std::runtime_error{"expected ConfigError"};
}

constexpr std::string_view defaults = R"ini(
[Server]
BaseUrl=http://127.0.0.1:8087/Synthserver/
ConnectTimeoutMs=2000
RequestTimeoutMs=30000
AllowRemote=false
[Runtime]
Enabled=true
LogLevel=info
DiagnosticsIncludePayloads=false
[Limits]
MaxResponseBytes=1048576
MaxPendingRequests=8
[Input]
PushToTalkEnabled=true
OpenMicrophone=false
[Rechat]
EndConversationCooldown=60
)ini";

void test_defaults_and_precedence() {
    const auto config = parse(defaults, R"ini(
[Server]
RequestTimeoutMs=45000
[Runtime]
Enabled=false
LogLevel=warning
[Limits]
MaxPendingRequests=12
)ini");
    CHECK(config.server.connect_timeout_ms == 2000);
    CHECK(config.server.request_timeout_ms == 45000);
    CHECK(!config.runtime.enabled);
    CHECK(config.runtime.log_level == LogLevel::warning);
    CHECK(config.limits.max_pending_requests == 12);
    CHECK(!config.server.allow_remote);
    CHECK(config.behavior.end_conversation_cooldown_seconds == 60);
}

void test_bad_input() {
    rejects([] { (void)parse("[Runtime]\nEnabled=perhaps\n"); });
    rejects([] { (void)parse("[Runtime]\nNoSuchOption=true\n"); });
    rejects([] { (void)parse("[Server]\nConnectTimeoutMs=0\n"); });
    rejects([] { (void)parse("[Limits]\nMaxResponseBytes=999999999\n"); });
    rejects([] { (void)parse("[Runtime\nEnabled=true\n"); });
    rejects([] { (void)parse("[Runtime]\nLogLevel=verbose\n"); });
    rejects([] { (void)parse("[Rechat]\nEndConversationCooldown=301\n"); });
}

void test_endpoint_policy() {
    const auto loopback = parse("[Server]\nBaseUrl=http://localhost:8087/api\n");
    CHECK(loopback.server.base_url == "http://localhost:8087/api/");
    rejects([] { (void)parse("[Server]\nBaseUrl=https://example.com/api\n"); });
    rejects([] { (void)parse("[Server]\nBaseUrl=http://example.com/api\nAllowRemote=true\n"); });
    rejects([] { (void)parse("[Server]\nBaseUrl=https://user:secret@example.com/api\nAllowRemote=true\n"); });
    rejects([] { (void)parse("[Server]\nBaseUrl=http://localhost:notaport/api\n"); });
    rejects([] { (void)parse("[Server]\nBaseUrl=http://localhost:/api\n"); });
    rejects([] { (void)parse("[Server]\nBaseUrl=http://localhost:70000/api\n"); });
    const auto remote = parse("[Server]\nBaseUrl=https://example.com/api\nAllowRemote=true\n");
    CHECK(remote.server.allow_remote);
}

void test_paths_and_missing_override() {
    CHECK(normalize_safe_relative_path("logs/../SYNTH.log").generic_string() == "SYNTH.log");
    rejects([] { (void)normalize_safe_relative_path("../../secret"); });
    rejects([] { (void)normalize_safe_relative_path("/absolute/file"); });

    const auto temporary = std::filesystem::temp_directory_path() / "synth-config-tests.ini";
    {
        std::ofstream output{temporary};
        output << defaults;
    }
    const auto config = load(temporary, temporary.parent_path() / "missing-SYNTH-custom.ini");
    CHECK(config.runtime.enabled);
    std::filesystem::remove(temporary);
}

// The shipped defaults carry every setting the in-game pages expose, and the
// single user override wins. This is the whole persisted settings chain.
void test_shipped_defaults_and_override_precedence() {
    const auto root = std::filesystem::current_path();
    const auto custom = std::filesystem::temp_directory_path() / "synth-override.ini";
    {
        std::ofstream output{custom};
        output << "[Hotkeys]\nPipVision=43\n[Audio]\nDistanceScale=20.0\n"
                  "[BoredEvents]\nTimerSeconds=5\n";
    }

    const auto shipped = load(root / "config/SYNTH.ini");
    CHECK(shipped.hotkeys.pip_vision == 0);
    CHECK(shipped.activation.automatic_hearing_distance == 784);
    CHECK(shipped.behavior.dynamic_profile_minutes == 30);
    CHECK(shipped.behavior.bored_event_seconds == 60);
    CHECK(shipped.audio.distance_scale == 2.0);
    CHECK(shipped.audio.pre_clip_ms == 100);
    CHECK(shipped.tools.initialize_requested == false);

    const auto config = load(root / "config/SYNTH.ini", custom);
    CHECK(config.hotkeys.pip_vision == 43);
    CHECK(config.behavior.bored_event_seconds == 5);
    CHECK(config.audio.distance_scale == 20.0);
    CHECK(config.activation.automatic_hearing_distance == 784);

    std::filesystem::remove(custom);
}

// Contract between the in-game settings catalog and the configuration parser.
// Every catalog row must name a real INI key, read back the value the parser
// stored, and format values the parser accepts at both range ends. Without this
// the two tables could drift and a page would silently write an override that
// fails to load.
void test_settings_catalog_matches_configuration() {
    using namespace synth::ui;
    std::size_t interactive{};
    for (const auto& page : pages) {
        CHECK(page.title != nullptr && *page.title != 0);
        for (const auto& setting : page.settings) {
            CHECK(setting.label != nullptr && *setting.label != 0);
            if (!setting.interactive()) {
                CHECK(setting.section.empty() && setting.key.empty());
                continue;
            }
            ++interactive;
            CHECK(!setting.section.empty() && !setting.key.empty());
            CHECK(setting.help != nullptr && *setting.help != 0);
            CHECK(find_setting(setting.section, setting.key) != nullptr);

            for (const auto probe : {setting.minimum, setting.maximum}) {
                Config config;
                const auto text = format_value(setting, probe);
                synth::config::detail::apply(config, setting.section, setting.key, text);
                const auto observed = current_value(config, setting);
                const auto expected =
                    setting.kind == SettingKind::toggle || setting.kind == SettingKind::action
                        ? (probe != 0.0 ? 1.0 : 0.0)
                        : probe;
                CHECK(std::fabs(observed - expected) < 1e-6);
                // Re-formatting what was read back must be stable, so a page
                // that renders a value cannot rewrite it into a different one.
                CHECK(format_value(setting, observed) == text);
            }
        }
    }
    CHECK(interactive == 43);

    // Every framework hotkey identifier must resolve to a catalog keybind row,
    // otherwise the reconcile pass would drop a rebind on the floor.
    for (const auto& binding : hotkey_bindings) {
        const auto* setting = find_setting("Hotkeys", binding.key);
        CHECK(setting != nullptr);
        CHECK(setting->kind == SettingKind::keybind);
        CHECK(setting->minimum == 0 && setting->maximum == 255);
        CHECK(std::string_view{binding.framework_id}.starts_with("SYNTH."));
    }
}

// Contract between the chatbox window and the game thread. The render thread
// owns the draft, the game thread owns everything that can act on it, and this
// is the only thing they share: one bounded message, handed over once.
void test_chatbox_bridge_hands_over_one_bounded_message() {
    using synth::ui::detail::ChatboxBridge;
    using synth::ui::detail::ChatboxSubmit;
    ChatboxBridge bridge;

    // The bound the transport already enforces on submitted dialogue text.
    static_assert(ChatboxBridge::message_limit == 4096);

    // One press opens one empty box.
    CHECK(!bridge.consume_open_request());
    bridge.request_open();
    bridge.request_open();
    const auto draft = bridge.consume_open_request();
    CHECK(draft);
    CHECK(!bridge.consume_open_request());

    // Nothing outside 1-4096 characters reaches the game thread.
    CHECK(bridge.submit("", draft->revision) == ChatboxSubmit::empty);
    CHECK(bridge.submit(" \t\r\n ", draft->revision) == ChatboxSubmit::empty);
    CHECK(bridge.submit(std::string(ChatboxBridge::message_limit + 1, 'a'), draft->revision) ==
          ChatboxSubmit::too_long);
    CHECK(!bridge.has_message());
    CHECK(!bridge.take_message().has_value());

    CHECK(bridge.submit("  hello there\n", draft->revision) == ChatboxSubmit::accepted);
    // An unread message is never overwritten and never doubled.
    CHECK(bridge.submit("second", draft->revision) == ChatboxSubmit::busy);
    const auto message = bridge.take_message();
    CHECK(message.has_value() && *message == "hello there");
    CHECK(!bridge.take_message().has_value());

    // A session that goes away takes the queued message with it.
    CHECK(bridge.request_open());
    const auto next = bridge.consume_open_request();
    CHECK(next);
    CHECK(bridge.submit(std::string(ChatboxBridge::message_limit, 'a'), next->revision) == ChatboxSubmit::accepted);
    bridge.request_open();
    bridge.discard();
    CHECK(!bridge.has_message());
    CHECK(!bridge.take_message().has_value());
    CHECK(!bridge.consume_open_request());
}

// Stale render receipts and string-only consumers must never rebind an actor-targeted draft.
void test_targeted_chatbox_identity_and_retirement() {
    using namespace synth::ui;
    namespace detail = synth::ui::detail;
    detail::ChatboxBridge bridge;
    synth::core::CancellationSource session{synth::core::RuntimeGeneration::initial()};
    synth::integration::PromptTarget target{session.token(), 42, 12, "Actor.esp", "Base.esm", "save", "%s NPC"};
    CHECK(bridge.request_open(target));
    CHECK(!bridge.request_open());
    CHECK(!bridge.request_open(target));
    const auto old = bridge.consume_open_request();
    CHECK(old && old->target && old->target->form_id == 42);
    bridge.discard();
    CHECK(bridge.request_open());
    const auto replacement = bridge.consume_open_request();
    CHECK(replacement && !replacement->target);
    CHECK(bridge.submit("old text", old->revision) == detail::ChatboxSubmit::stale);
    bridge.cancel(old->revision);
    CHECK(bridge.submit("new text", replacement->revision) == detail::ChatboxSubmit::accepted);
    bridge.cancel(replacement->revision);
    CHECK(bridge.take_message() == "new text");
    CHECK(bridge.request_open(target));
    const auto exact = bridge.consume_open_request();
    CHECK(exact);
    CHECK(bridge.submit("hello", exact->revision) == detail::ChatboxSubmit::accepted);
    CHECK(!bridge.take_message());
    CHECK(!bridge.request_open());
    const auto submitted = bridge.take_submission();
    CHECK(submitted && submitted->text == "hello" && submitted->target);
    CHECK(submitted->target->form_id == 42 && submitted->target->base_form_id == 12);
    CHECK(submitted->target->origin_plugin == "Actor.esp" && submitted->target->playthrough_id == "save");
    CHECK(!bridge.take_submission());
    CHECK(bridge.request_open(target));
    const auto retiring = bridge.consume_open_request();
    CHECK(retiring);
    session.cancel();
    CHECK(bridge.submit("late", retiring->revision) == detail::ChatboxSubmit::stale);
    bridge.discard();
    CHECK(!bridge.request_open(target));
    CHECK(!Chatbox::request_open(target));
    CHECK(!Chatbox::take_submission());
}

// Without the framework header the chatbox is inert, exactly like the pages.
void test_chatbox_is_a_no_op_without_the_framework() {
    CHECK(!synth::ui::Chatbox::available());
    CHECK(!synth::ui::Chatbox::visible());
    CHECK(!synth::ui::Chatbox::request_open());
    CHECK(!synth::ui::Chatbox::take_message().has_value());
    synth::ui::Chatbox::close();
    static_assert(synth::ui::Chatbox::message_limit == 4096);
}

void test_open_mic_keeps_push_to_talk_available() {
    const auto config = parse(
        "[Input]\nPushToTalkEnabled=true\nOpenMicrophone=false\n",
        "[OpenMic]\nEnabled=true\n");
    CHECK(config.input.open_microphone);
    CHECK(config.input.push_to_talk_enabled);
}
}  // namespace

int main() {
    try {
        test_defaults_and_precedence();
        test_bad_input();
        test_endpoint_policy();
        test_paths_and_missing_override();
        test_shipped_defaults_and_override_precedence();
        test_settings_catalog_matches_configuration();
        test_chatbox_bridge_hands_over_one_bounded_message();
        test_targeted_chatbox_identity_and_retirement();
        test_chatbox_is_a_no_op_without_the_framework();
        test_open_mic_keeps_push_to_talk_available();
        std::cout << "config tests passed (" << assertions << " assertions)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
