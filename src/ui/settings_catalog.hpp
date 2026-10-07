#pragma once

// Engine-free description of SYNTH's in-game settings pages.
//
// This table is the single description of the SYNTH pages that the F4SE Menu
// Framework adapter renders. It carries no state: every value is read from the
// authoritative `synth::config::Config` and every edit is written back to the
// authoritative INI override, so the catalog can never become a second settings
// store. Labels, help text, ranges and ordering are the ones SYNTH shipped
// previously, so the in-game surface is unchanged by the migration.

#include "config/config.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace synth::ui {

enum class SettingKind : unsigned char {
    section,   // non-interactive heading
    note,      // non-interactive informational row
    toggle,    // bool
    integer,   // std::uint32_t within [minimum, maximum]
    decimal,   // double within [minimum, maximum]
    keybind,   // DirectInput scan code within [minimum, maximum]
    action,    // momentary control that sets a bool the runtime re-arms
};

struct Setting final {
    // Literal pointers, not views: every one of these is handed straight to a
    // C UI API that needs a NUL terminator.
    const char* label;
    const char* help;
    std::string_view section;  // INI section; empty for section/note rows
    std::string_view key;      // INI key; empty for section/note rows
    SettingKind kind{SettingKind::section};
    double minimum{};
    double maximum{};
    double step{};

    [[nodiscard]] constexpr bool interactive() const noexcept {
        return kind != SettingKind::section && kind != SettingKind::note;
    }
};

struct Page final {
    const char* title;
    std::span<const Setting> settings;
};

// ------------------------------------------------------------ control menu ---
//
// The in-game Control menu's vocabulary, kept here for the same reason the
// settings pages are: one engine-free description that both the renderer and any
// consumer read, so a label and the value that travels to the game thread can
// never drift apart. These rows are deliberately not `Setting`s - none of them is
// a persisted INI override, and the Control menu is a separate surface from the
// settings pages. See `ui/control_menu.hpp`.
struct ControlOption final {
    // Literal pointers, not views: both are handed straight to a C UI API that
    // needs a NUL terminator, and `value` is compared against strings the game
    // thread published.
    const char* label;  // what the player reads
    const char* value;  // what travels to the game thread, verbatim
    const char* help;
};

inline constexpr std::array<ControlOption, 9> chat_modes{{
    {"Standard", "STANDARD", "Normal conversational replies."},
    {"Whisper", "WHISPER", "Private dialogue with the selected listener within 200 game units."},
    {"Close", "CLOSE", "Close-range replies for a private exchange."},
    {"Shout", "SHOUT", "Raised replies that carry further than normal speech."},
    {"Narrator", "NARRATOR", "Route the exchange through The Narrator."},
    {"Director", "DIRECTOR", "Direct the selected NPC using supported dialogue and actions. Multi-NPC scene orchestration is not available yet."},
    {"Injection Log", "INJECTION_LOG", "Record injected context without producing a reply."},
    {"Injection Chat", "INJECTION_CHAT", "Inject context and continue the conversation."},
    {"Cheat Mode", "CHEATMODE", "Give direct instructions using SYNTH's supported actions; unsupported commands remain blocked."},
}};

inline constexpr std::array<ControlOption, 4> llm_models{{
    {"Standard", "standard", "The balanced default model slot."},
    {"Fast", "fast", "Lower latency, shorter replies."},
    {"Powerful", "powerful", "Slower, for longer or more involved replies."},
    {"Experimental", "experimental", "Whatever the server currently has under test."},
}};

// Who a dynamic profile refresh is asked for. The Control menu maps each of
// these onto its own action, so the consumer never has to parse a scope string.
inline constexpr std::array<ControlOption, 3> profile_scopes{{
    {"Refresh This NPC", "target", "Rebuild the dynamic profile for the NPC you are talking to."},
    {"Refresh Nearby NPCs", "nearby", "Rebuild dynamic profiles for managed NPCs around you."},
    {"Refresh The Narrator", "narrator", "Rebuild The Narrator's dynamic profile."},
}};

// Stable identifiers for the hotkeys SYNTH publishes to the F4SE Menu Framework
// hotkey registry. The framework namespaces bindings by this string, so the
// prefix must stay unique across mods and stable across releases.
struct HotkeyBinding final {
    const char* framework_id;
    std::string_view key;  // INI key inside [Hotkeys]
    const char* label;
};

inline constexpr std::array<HotkeyBinding, 7> hotkey_bindings{{
    {"SYNTH.Chatbox", "TalkToNPC", "Chatbox"},
    {"SYNTH.VoiceChat", "ToggleVoice", "Voice Chat"},
    {"SYNTH.Control", "SYNTHControl", "SYNTH Control"},
    {"SYNTH.ManualActivate", "ManualActivate", "Manual AI Activate"},
    {"SYNTH.HaltActions", "StopTalking", "Halt AI Actions"},
    {"SYNTH.PipVision", "PipVision", "Use PipVision"},
    {"SYNTH.OpenMicMute", "OpenMicMute", "Open Mic Mute"},
}};

namespace detail {

inline constexpr Setting hotkeys_page[] = {
    {"Hotkeys", "", "", "", SettingKind::section},
    {"Chatbox", "Open text dialogue for the NPC in your crosshair. Unbound by default.",
     "Hotkeys", "TalkToNPC", SettingKind::keybind, 0, 255, 1},
    {"Voice Chat", "Hold to talk to the selected NPC. Unbound by default.",
     "Hotkeys", "ToggleVoice", SettingKind::keybind, 0, 255, 1},
    {"SYNTH Control",
     "Open SYNTH's in-game control menu: chat mode, LLM mode, dynamic profiles, and wait here. "
     "Unbound by default.",
     "Hotkeys", "SYNTHControl", SettingKind::keybind, 0, 255, 1},
    {"Manual AI Activate", "Activate the targeted NPC as a SYNTH AI agent. Unbound by default.",
     "Hotkeys", "ManualActivate", SettingKind::keybind, 0, 255, 1},
    {"Halt AI Actions",
     "Hard stop dialogue, requests, media, playback, rechat, and active actions. Unbound by default.",
     "Hotkeys", "StopTalking", SettingKind::keybind, 0, 255, 1},
    {"Use PipVision",
     "Tap to capture the scene, double-tap to set the targeted NPC portrait, or hold to ask the "
     "nearest NPC to describe the scene. Unbound by default.",
     "Hotkeys", "PipVision", SettingKind::keybind, 0, 255, 1},
};

inline constexpr Setting auto_activate_page[] = {
    {"Activation", "", "", "", SettingKind::section},
    {"Enable Auto Activate", "Automatically activate nearby eligible NPCs as SYNTH AI agents.",
     "AutoActivate", "Enabled", SettingKind::toggle},
    {"Interior Auto Activate Distance",
     "AI actors within this distance in interiors are auto activated.",
     "Distance", "ActivatingNpcInterior", SettingKind::integer, 200, 4000, 100},
    {"Exterior Auto Activate Distance", "AI actors within this distance outside are auto activated.",
     "Distance", "ActivatingNpcExterior", SettingKind::integer, 200, 6000, 100},
    {"Spatial Hearing", "", "", "", SettingKind::section},
    {"Interior Spatial Hearing Distance",
     "How far AI actors can hear spoken dialogue in interiors.",
     "SpatialAudio", "InteriorHearingDistance", SettingKind::integer, 100, 4000, 50},
    {"Exterior Spatial Hearing Distance", "How far AI actors can hear spoken dialogue outdoors.",
     "SpatialAudio", "ExteriorHearingDistance", SettingKind::integer, 100, 6000, 50},
    {"Auto Hearing Radius",
     "Default hearing radius used by automatic listener and rechat selection.",
     "SpatialAudio", "AutoHearingDistance", SettingKind::integer, 100, 4000, 50},
    {"Policy", "", "", "", SettingKind::section},
    {"Add Hostile NPCs", "Allow hostile NPCs to be auto activated.",
     "AutoActivate", "AutoAddHostile", SettingKind::toggle},
    {"Add All Races", "Allow auto activation for all actor categories, including creatures.",
     "AutoActivate", "AutoAddCreatures", SettingKind::toggle},
    {"Combat", "", "", "", SettingKind::section},
    {"Allow Combat Dialogue", "Allow regular AI dialogue requests while the player is in combat.",
     "Behavior", "EnableCombatDialogue", SettingKind::toggle},
    {"Clear Dialogue Entering Combat",
     "Cancel active AI dialogue and queued responses when combat begins.",
     "Behavior", "CancelDialogueOnCombat", SettingKind::toggle},
    {"Enable Combat Barks", "Allow nearby AI agents to make combat bark comments.",
     "RpgEvents", "CombatBarksEnabled", SettingKind::toggle},
    {"Combat Bark Cooldown", "Seconds between combat bark attempts during combat.",
     "RpgEvents", "CombatBarkPeriodSeconds", SettingKind::integer, 5, 120, 5},
};

inline constexpr Setting behavior_page[] = {
    {"Dialogue", "", "", "", SettingKind::section},
    {"Connection Timeout", "Maximum response wait in seconds.",
     "Behavior", "AIResponseTimeout", SettingKind::integer, 5, 180, 5},
    {"Pause Dialogue in Menus",
     "Pause active AI dialogue audio while menus or the Pip-Boy are open.",
     "Behavior", "PauseDialogueOnMenu", SettingKind::toggle},
    {"Show AI Subtitles", "Display passive Fallout-style subtitles while AI audio plays.",
     "Behavior", "ShowAISubtitles", SettingKind::toggle},
    {"Scene Safety", "Avoid actors currently controlled by active scene or dialogue packages.",
     "Behavior", "SceneSafety", SettingKind::toggle},
    {"Dynamic Profiles", "", "", "", SettingKind::section},
    {"Dynamic Profile Timer", "Minutes between automatic dynamic profile refreshes.",
     "DynamicProfile", "TimerMinutes", SettingKind::integer, 5, 240, 5},
    {"Include Narrator", "Include The Narrator in timer-triggered profile refreshes.",
     "DynamicProfile", "IncludeNarrator", SettingKind::toggle},
    {"Bored Events", "", "", "", SettingKind::section},
    {"Bored Event Timer",
     "Seconds of quiet time before SYNTH may ask a nearby managed NPC for an idle comment.",
     "BoredEvents", "TimerSeconds", SettingKind::integer, 5, 600, 5},
    {"End Conversation Cooldown",
     "Seconds before an actor who ended a SYNTH conversation can be activated again.",
     "Rechat", "EndConversationCooldown", SettingKind::integer, 0, 300, 5},
};

inline constexpr Setting sound_page[] = {
    {"Basic", "", "", "", SettingKind::section},
    {"AI Voice Volume", "Set AI NPC speech volume.",
     "Audio", "VoiceVolume", SettingKind::integer, 0, 100, 5},
    {"Narrator / Player TTS Volume",
     "Adjust narrator and player TTS relative to AI voice volume. 100 keeps the current level.",
     "Audio", "HeadVoiceVolume", SettingKind::integer, 0, 200, 5},
    {"Enable 3D Audio Playback", "Control player-heard spatial voice playback.",
     "Audio", "Enable3DPlayback", SettingKind::toggle},
    {"AI Voice Distance Scale", "Adjust AI NPC voice volume at distance.",
     "Audio", "DistanceScale", SettingKind::decimal, 0.1, 20.0, 0.1},
    {"Interior Playback Dropoff",
     "Indoor playback dropoff aggressiveness. Lower values are less aggressive.",
     "Audio", "PlaybackDropoffInteriorPercent", SettingKind::integer, 25, 200, 5},
    {"Exterior Playback Dropoff",
     "Outdoor playback dropoff aggressiveness. Lower values are less aggressive.",
     "Audio", "PlaybackDropoffExteriorPercent", SettingKind::integer, 25, 200, 5},
    {"3D Sound Invert Heading", "Try this if 3D audio left and right are reversed.",
     "Audio", "InvertHeading", SettingKind::toggle},
    {"Open Mic", "", "", "", SettingKind::section},
    {"Current Device: Detected automatically",
     "The first in-game recording containing speech selects the microphone.",
     "", "", SettingKind::note},
    {"Enable Open Mic",
     "Listen for speech during normal gameplay and start voice input for the current target.",
     "OpenMic", "Enabled", SettingKind::toggle},
    {"Mute Open Mic", "Temporarily mute open mic detection without disabling it.",
     "OpenMic", "Muted", SettingKind::toggle},
    {"Open Mic Mute Hotkey", "Toggle open mic mute. Unbound by default.",
     "Hotkeys", "OpenMicMute", SettingKind::keybind, 0, 255, 1},
    {"Open Mic Sensitivity",
     "RMS threshold for starting a recording. Higher values require louder speech.",
     "OpenMic", "Sensitivity", SettingKind::integer, 100, 10000, 100},
    {"Open Mic End Delay", "Seconds of silence before open mic recording is submitted.",
     "OpenMic", "EndDelaySeconds", SettingKind::decimal, 0.1, 5.0, 0.1},
    {"Advanced", "", "", "", SettingKind::section},
    {"Skip Milliseconds at Beginning", "Skip silence at the beginning of generated voice clips.",
     "Audio", "PreClipMs", SettingKind::integer, 0, 1000, 25},
    {"Skip Milliseconds at End", "Skip silence at the end of generated voice clips.",
     "Audio", "PostClipMs", SettingKind::integer, 0, 1000, 25},
    {"Resolution of Lip Animations", "Lower values update more often and use more CPU.",
     "Audio", "AnimationResolution", SettingKind::integer, 50, 1500, 50},
    {"Intensity of Lip Animations", "Lower this if mouths open too much.",
     "Audio", "AnimationIntensity", SettingKind::decimal, 0.0, 3.0, 0.1},
};

inline constexpr Setting tools_page[] = {
    {"Initialization", "", "", "", SettingKind::section},
    {"Initialize SYNTH",
     "Reconnect SYNTH and republish the current Fallout runtime context to Synthserver.",
     "Tools", "InitializeSYNTH", SettingKind::action},
};

}  // namespace detail

inline constexpr std::array<Page, 5> pages{{
    {"Hotkeys", detail::hotkeys_page},
    {"Auto Activate", detail::auto_activate_page},
    {"Behavior", detail::behavior_page},
    {"Sound", detail::sound_page},
    {"Tools", detail::tools_page},
}};

// Reads the live value of a setting out of the authoritative configuration.
// Booleans come back as 0 or 1 so every widget can share one numeric path.
[[nodiscard]] inline double current_value(const config::Config& configuration,
                                          const Setting& setting) {
    const auto section = setting.section;
    const auto key = setting.key;
    const auto boolean = [](bool value) { return value ? 1.0 : 0.0; };
    if (section == "Hotkeys") {
        if (key == "TalkToNPC") return configuration.hotkeys.talk_to_npc;
        if (key == "ToggleVoice") return configuration.hotkeys.toggle_voice;
        if (key == "SYNTHControl") return configuration.hotkeys.synth_control;
        if (key == "ManualActivate") return configuration.hotkeys.manual_activate;
        if (key == "StopTalking") return configuration.hotkeys.hard_halt;
        if (key == "PipVision") return configuration.hotkeys.pip_vision;
        if (key == "OpenMicMute") return configuration.hotkeys.open_mic_mute;
    } else if (section == "AutoActivate") {
        if (key == "Enabled") return boolean(configuration.activation.enabled);
        if (key == "AutoAddHostile") return boolean(configuration.activation.include_hostile);
        if (key == "AutoAddCreatures") return boolean(configuration.activation.include_creatures);
    } else if (section == "Distance") {
        if (key == "ActivatingNpcInterior") return configuration.activation.interior_distance;
        if (key == "ActivatingNpcExterior") return configuration.activation.exterior_distance;
    } else if (section == "SpatialAudio") {
        if (key == "InteriorHearingDistance")
            return configuration.activation.interior_hearing_distance;
        if (key == "ExteriorHearingDistance")
            return configuration.activation.exterior_hearing_distance;
        if (key == "AutoHearingDistance")
            return configuration.activation.automatic_hearing_distance;
    } else if (section == "Behavior") {
        if (key == "AIResponseTimeout") return configuration.behavior.ai_response_timeout_seconds;
        if (key == "PauseDialogueOnMenu")
            return boolean(configuration.behavior.pause_dialogue_in_menus);
        if (key == "ShowAISubtitles") return boolean(configuration.behavior.show_subtitles);
        if (key == "SceneSafety") return boolean(configuration.behavior.scene_safety);
        if (key == "EnableCombatDialogue")
            return boolean(configuration.behavior.enable_combat_dialogue);
        if (key == "CancelDialogueOnCombat")
            return boolean(configuration.behavior.cancel_dialogue_on_combat);
    } else if (section == "RpgEvents") {
        if (key == "CombatBarksEnabled") return boolean(configuration.behavior.combat_barks_enabled);
        if (key == "CombatBarkPeriodSeconds")
            return configuration.behavior.combat_bark_period_seconds;
    } else if (section == "DynamicProfile") {
        if (key == "TimerMinutes") return configuration.behavior.dynamic_profile_minutes;
        if (key == "IncludeNarrator")
            return boolean(configuration.behavior.dynamic_profile_include_narrator);
    } else if (section == "BoredEvents") {
        if (key == "TimerSeconds") return configuration.behavior.bored_event_seconds;
    } else if (section == "Rechat") {
        if (key == "EndConversationCooldown")
            return configuration.behavior.end_conversation_cooldown_seconds;
    } else if (section == "Audio") {
        if (key == "VoiceVolume") return configuration.audio.voice_volume_percent;
        if (key == "HeadVoiceVolume") return configuration.audio.head_voice_volume_percent;
        if (key == "Enable3DPlayback") return boolean(configuration.audio.spatial_playback);
        if (key == "DistanceScale") return configuration.audio.distance_scale;
        if (key == "PlaybackDropoffInteriorPercent")
            return configuration.audio.interior_dropoff_percent;
        if (key == "PlaybackDropoffExteriorPercent")
            return configuration.audio.exterior_dropoff_percent;
        if (key == "InvertHeading") return boolean(configuration.audio.invert_heading);
        if (key == "PreClipMs") return configuration.audio.pre_clip_ms;
        if (key == "PostClipMs") return configuration.audio.post_clip_ms;
        if (key == "AnimationResolution") return configuration.audio.lip_animation_resolution;
        if (key == "AnimationIntensity") return configuration.audio.lip_animation_intensity;
    } else if (section == "OpenMic") {
        if (key == "Enabled") return boolean(configuration.input.open_microphone);
        if (key == "Muted") return boolean(configuration.audio.open_mic_muted);
        if (key == "Sensitivity") return configuration.audio.open_mic_sensitivity;
        if (key == "EndDelaySeconds") return configuration.audio.open_mic_end_delay_seconds;
    } else if (section == "Tools") {
        if (key == "InitializeSYNTH") return boolean(configuration.tools.initialize_requested);
    }
    throw config::ConfigError{"setting catalog references an unknown key: " +
                             std::string{section} + "." + std::string{key}};
}

// Renders a value in exactly the syntax `synth::config` accepts, clamped to the
// declared range so a widget can never persist an unparseable override.
[[nodiscard]] inline std::string format_value(const Setting& setting, double value) {
    if (setting.kind == SettingKind::toggle || setting.kind == SettingKind::action) {
        return value != 0.0 ? "true" : "false";
    }
    const auto clamped = value < setting.minimum   ? setting.minimum
                         : value > setting.maximum ? setting.maximum
                                                   : value;
    if (setting.kind == SettingKind::decimal) {
        // Two fraction digits cover every 0.1-step decimal SYNTH exposes and
        // stay inside the parser range checks after truncation.
        auto text = std::to_string(clamped);
        const auto dot = text.find('.');
        if (dot != std::string::npos && text.size() > dot + 3) {
            text.resize(dot + 3);
        }
        return text;
    }
    return std::to_string(static_cast<std::uint32_t>(clamped + 0.5));
}

// Locates the catalog row backing an INI key so the hotkey bridge and the pages
// can never disagree about a range or a label.
[[nodiscard]] inline const Setting* find_setting(std::string_view section, std::string_view key) {
    for (const auto& page : pages) {
        for (const auto& setting : page.settings) {
            if (setting.interactive() && setting.section == section && setting.key == key) {
                return &setting;
            }
        }
    }
    return nullptr;
}

}  // namespace synth::ui
