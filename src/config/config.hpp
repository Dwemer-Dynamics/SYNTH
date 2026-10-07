#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace synth::config {

enum class LogLevel : unsigned char { trace, debug, info, warning, error };

struct ServerConfig {
    bool operator==(const ServerConfig&) const = default;
    std::string base_url{"http://127.0.0.1:8087/Synthserver/"};
    std::uint32_t connect_timeout_ms{2'000};
    std::uint32_t request_timeout_ms{30'000};
    bool allow_remote{false};
};

struct RuntimeConfig {
    bool operator==(const RuntimeConfig&) const = default;
    bool enabled{true};
    LogLevel log_level{LogLevel::info};
    bool diagnostics_include_payloads{false};
};

struct LimitsConfig {
    bool operator==(const LimitsConfig&) const = default;
    std::uint32_t max_response_bytes{1'048'576};
    std::uint32_t max_pending_requests{8};
};

struct InputConfig {
    bool operator==(const InputConfig&) const = default;
    bool push_to_talk_enabled{true};
    bool open_microphone{false};
};

struct HotkeysConfig {
    bool operator==(const HotkeysConfig&) const = default;
    std::uint32_t talk_to_npc{};
    std::uint32_t toggle_voice{};
    std::uint32_t synth_control{};
    std::uint32_t manual_activate{};
    std::uint32_t hard_halt{};
    std::uint32_t pip_vision{};
    std::uint32_t open_mic_mute{};
};

struct ActivationConfig {
    bool operator==(const ActivationConfig&) const = default;
    bool enabled{true};
    std::uint32_t interior_distance{1200};
    std::uint32_t exterior_distance{2400};
    std::uint32_t interior_hearing_distance{1050};
    std::uint32_t exterior_hearing_distance{1750};
    std::uint32_t automatic_hearing_distance{784};
    bool include_hostile{};
    bool include_creatures{};
};

struct BehaviorConfig {
    bool operator==(const BehaviorConfig&) const = default;
    std::uint32_t ai_response_timeout_seconds{30};
    bool pause_dialogue_in_menus{true};
    bool show_subtitles{true};
    bool scene_safety{true};
    bool enable_combat_dialogue{};
    bool cancel_dialogue_on_combat{true};
    bool combat_barks_enabled{true};
    std::uint32_t combat_bark_period_seconds{30};
    std::uint32_t dynamic_profile_minutes{30};
    bool dynamic_profile_include_narrator{true};
    std::uint32_t bored_event_seconds{60};
    std::uint32_t end_conversation_cooldown_seconds{60};
};

struct AudioConfig {
    bool operator==(const AudioConfig&) const = default;
    std::uint32_t voice_volume_percent{75};
    std::uint32_t head_voice_volume_percent{100};
    bool spatial_playback{true};
    bool camera_listener{};
    double distance_scale{2.0};
    std::uint32_t interior_dropoff_percent{70};
    std::uint32_t exterior_dropoff_percent{70};
    bool invert_heading{};
    bool open_mic_muted{};
    std::uint32_t open_mic_sensitivity{1000};
    double open_mic_end_delay_seconds{1.0};
    std::uint32_t pre_clip_ms{100};
    std::uint32_t post_clip_ms{100};
    std::uint32_t lip_animation_resolution{500};
    double lip_animation_intensity{1.0};
};

struct ToolsConfig {
    bool operator==(const ToolsConfig&) const = default;
    bool initialize_requested{};
};

struct Config {
    bool operator==(const Config&) const = default;
    ServerConfig server{};
    RuntimeConfig runtime{};
    LimitsConfig limits{};
    InputConfig input{};
    HotkeysConfig hotkeys{};
    ActivationConfig activation{};
    BehaviorConfig behavior{};
    AudioConfig audio{};
    ToolsConfig tools{};
};

struct FileRevision {
    bool exists{};
    std::filesystem::file_time_type written{};
    std::uintmax_t size{};

    auto operator<=>(const FileRevision&) const = default;
};

class ConfigError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

namespace detail {

inline std::string trim(std::string_view value) {
    const auto space = [](unsigned char character) { return std::isspace(character) != 0; };
    while (!value.empty() && space(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && space(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return std::string{value};
}

inline std::string lower(std::string_view value) {
    std::string result{value};
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

inline bool parse_bool(std::string_view value, std::string_view key) {
    const auto normalized = lower(trim(value));
    if (normalized == "true") {
        return true;
    }
    if (normalized == "false") {
        return false;
    }
    throw ConfigError{"invalid boolean for " + std::string{key}};
}

inline std::uint32_t parse_range(std::string_view value,
                                 std::uint32_t minimum,
                                 std::uint32_t maximum,
                                 std::string_view key) {
    const auto text = trim(value);
    std::uint32_t result{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size() || result < minimum ||
        result > maximum) {
        throw ConfigError{"value out of range for " + std::string{key}};
    }
    return result;
}

inline double parse_decimal(std::string_view value,
                            double minimum,
                            double maximum,
                            std::string_view key) {
    const auto text = trim(value);
    std::size_t consumed{};
    double result{};
    try {
        result = std::stod(text, &consumed);
    } catch (...) {
        throw ConfigError{"invalid decimal for " + std::string{key}};
    }
    if (consumed != text.size() || !std::isfinite(result) || result < minimum || result > maximum) {
        throw ConfigError{"value out of range for " + std::string{key}};
    }
    return result;
}

inline LogLevel parse_log_level(std::string_view value) {
    const auto normalized = lower(trim(value));
    if (normalized == "trace") return LogLevel::trace;
    if (normalized == "debug") return LogLevel::debug;
    if (normalized == "info") return LogLevel::info;
    if (normalized == "warning" || normalized == "warn") return LogLevel::warning;
    if (normalized == "error") return LogLevel::error;
    throw ConfigError{"invalid enum value for Runtime.LogLevel"};
}

struct UrlParts {
    std::string scheme;
    std::string host;
    std::string path;
};

inline UrlParts parse_url(std::string_view input) {
    const auto scheme_end = input.find("://");
    if (scheme_end == std::string_view::npos) {
        throw ConfigError{"Server.BaseUrl must be an absolute HTTP URL"};
    }
    UrlParts result{lower(input.substr(0, scheme_end)), {}, {}};
    if (result.scheme != "http" && result.scheme != "https") {
        throw ConfigError{"Server.BaseUrl must use HTTP or HTTPS"};
    }
    auto remainder = input.substr(scheme_end + 3);
    const auto authority_end = remainder.find_first_of("/?#");
    const auto authority = remainder.substr(0, authority_end);
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        throw ConfigError{"Server.BaseUrl must not contain credentials"};
    }
    if (remainder.find_first_of("?#") != std::string_view::npos) {
        throw ConfigError{"Server.BaseUrl must not contain a query or fragment"};
    }
    auto validate_port = [](std::string_view port) {
        unsigned value{};
        const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), value);
        if (port.empty() || error != std::errc{} || end != port.data() + port.size() ||
            value == 0 || value > 65535) {
            throw ConfigError{"invalid Server.BaseUrl port"};
        }
    };
    auto host_port = authority;
    if (host_port.front() == '[') {
        const auto closing = host_port.find(']');
        if (closing == std::string_view::npos) throw ConfigError{"invalid Server.BaseUrl host"};
        result.host = lower(host_port.substr(1, closing - 1));
        const auto suffix = host_port.substr(closing + 1);
        if (!suffix.empty()) {
            if (suffix.front() != ':') throw ConfigError{"invalid Server.BaseUrl port"};
            validate_port(suffix.substr(1));
        }
    } else {
        const auto colon = host_port.rfind(':');
        const auto host_text = colon == std::string_view::npos ? host_port : host_port.substr(0, colon);
        if (host_text.find(':') != std::string_view::npos) {
            throw ConfigError{"IPv6 Server.BaseUrl hosts must use brackets"};
        }
        result.host = lower(host_text);
        if (colon != std::string_view::npos) validate_port(host_port.substr(colon + 1));
    }
    if (result.host.empty()) throw ConfigError{"invalid Server.BaseUrl host"};
    result.path = authority_end == std::string_view::npos ? "/" : std::string{remainder.substr(authority_end)};
    return result;
}

inline bool is_loopback_host(std::string_view host) {
    if (host == "localhost" || host == "::1") return true;
    if (!host.starts_with("127.")) return false;
    unsigned parts = 0;
    while (!host.empty()) {
        const auto dot = host.find('.');
        const auto part = host.substr(0, dot);
        unsigned value{};
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (error != std::errc{} || end != part.data() + part.size() || value > 255) return false;
        ++parts;
        if (dot == std::string_view::npos) break;
        host.remove_prefix(dot + 1);
    }
    return parts == 4;
}

inline void validate(Config& config) {
    const auto url = parse_url(config.server.base_url);
    const bool loopback = is_loopback_host(url.host);
    if (!loopback && !config.server.allow_remote) {
        throw ConfigError{"remote Server.BaseUrl requires Server.AllowRemote=true"};
    }
    if (!loopback && url.scheme != "https") {
        throw ConfigError{"remote Server.BaseUrl requires HTTPS"};
    }
    if (config.server.base_url.back() != '/') config.server.base_url.push_back('/');
}

inline void apply(Config& config,
                  std::string_view section,
                  std::string_view key,
                  std::string_view value) {
    const auto qualified = std::string{section} + "." + std::string{key};
    if (section == "Server" && key == "BaseUrl") config.server.base_url = trim(value);
    else if (section == "Server" && key == "ConnectTimeoutMs")
        config.server.connect_timeout_ms = parse_range(value, 1, 60'000, qualified);
    else if (section == "Server" && key == "RequestTimeoutMs")
        config.server.request_timeout_ms = parse_range(value, 1, 300'000, qualified);
    else if (section == "Server" && key == "AllowRemote")
        config.server.allow_remote = parse_bool(value, qualified);
    else if (section == "Runtime" && key == "Enabled")
        config.runtime.enabled = parse_bool(value, qualified);
    else if (section == "Runtime" && key == "LogLevel")
        config.runtime.log_level = parse_log_level(value);
    else if (section == "Runtime" && key == "DiagnosticsIncludePayloads")
        config.runtime.diagnostics_include_payloads = parse_bool(value, qualified);
    else if (section == "Limits" && key == "MaxResponseBytes")
        config.limits.max_response_bytes = parse_range(value, 1, 16'777'216, qualified);
    else if (section == "Limits" && key == "MaxPendingRequests")
        config.limits.max_pending_requests = parse_range(value, 1, 256, qualified);
    else if (section == "Input" && key == "PushToTalkEnabled")
        config.input.push_to_talk_enabled = parse_bool(value, qualified);
    else if (section == "Input" && key == "OpenMicrophone")
        config.input.open_microphone = parse_bool(value, qualified);
    else if (section == "Hotkeys" && key == "TalkToNPC")
        config.hotkeys.talk_to_npc = parse_range(value, 0, 255, qualified);
    else if (section == "Hotkeys" && key == "ToggleVoice")
        config.hotkeys.toggle_voice = parse_range(value, 0, 255, qualified);
    else if (section == "Hotkeys" && key == "SYNTHControl")
        config.hotkeys.synth_control = parse_range(value, 0, 255, qualified);
    else if (section == "Hotkeys" && key == "ManualActivate")
        config.hotkeys.manual_activate = parse_range(value, 0, 255, qualified);
    else if (section == "Hotkeys" && key == "StopTalking")
        config.hotkeys.hard_halt = parse_range(value, 0, 255, qualified);
    else if (section == "Hotkeys" && key == "PipVision")
        config.hotkeys.pip_vision = parse_range(value, 0, 255, qualified);
    else if (section == "Hotkeys" && key == "OpenMicMute")
        config.hotkeys.open_mic_mute = parse_range(value, 0, 255, qualified);
    else if (section == "AutoActivate" && key == "Enabled")
        config.activation.enabled = parse_bool(value, qualified);
    else if (section == "AutoActivate" && key == "AutoAddHostile")
        config.activation.include_hostile = parse_bool(value, qualified);
    else if (section == "AutoActivate" && key == "AutoAddCreatures")
        config.activation.include_creatures = parse_bool(value, qualified);
    else if (section == "Distance" && key == "ActivatingNpcInterior")
        config.activation.interior_distance = parse_range(value, 200, 4000, qualified);
    else if (section == "Distance" && key == "ActivatingNpcExterior")
        config.activation.exterior_distance = parse_range(value, 200, 6000, qualified);
    else if (section == "SpatialAudio" && key == "InteriorHearingDistance")
        config.activation.interior_hearing_distance = parse_range(value, 100, 4000, qualified);
    else if (section == "SpatialAudio" && key == "ExteriorHearingDistance")
        config.activation.exterior_hearing_distance = parse_range(value, 100, 6000, qualified);
    else if (section == "SpatialAudio" && key == "AutoHearingDistance")
        config.activation.automatic_hearing_distance = parse_range(value, 100, 4000, qualified);
    else if (section == "Behavior" && key == "AIResponseTimeout")
        config.behavior.ai_response_timeout_seconds = parse_range(value, 5, 180, qualified);
    else if (section == "Behavior" && key == "PauseDialogueOnMenu")
        config.behavior.pause_dialogue_in_menus = parse_bool(value, qualified);
    else if (section == "Behavior" && key == "ShowAISubtitles")
        config.behavior.show_subtitles = parse_bool(value, qualified);
    else if (section == "Behavior" && key == "SceneSafety")
        config.behavior.scene_safety = parse_bool(value, qualified);
    else if (section == "Behavior" && key == "EnableCombatDialogue")
        config.behavior.enable_combat_dialogue = parse_bool(value, qualified);
    else if (section == "Behavior" && key == "CancelDialogueOnCombat")
        config.behavior.cancel_dialogue_on_combat = parse_bool(value, qualified);
    else if (section == "RpgEvents" && key == "CombatBarksEnabled")
        config.behavior.combat_barks_enabled = parse_bool(value, qualified);
    else if (section == "RpgEvents" && key == "CombatBarkPeriodSeconds")
        config.behavior.combat_bark_period_seconds = parse_range(value, 5, 120, qualified);
    else if (section == "DynamicProfile" && key == "TimerMinutes")
        config.behavior.dynamic_profile_minutes = parse_range(value, 5, 240, qualified);
    else if (section == "DynamicProfile" && key == "IncludeNarrator")
        config.behavior.dynamic_profile_include_narrator = parse_bool(value, qualified);
    else if (section == "BoredEvents" && key == "TimerSeconds")
        config.behavior.bored_event_seconds = parse_range(value, 5, 600, qualified);
    else if (section == "Rechat" && key == "EndConversationCooldown")
        config.behavior.end_conversation_cooldown_seconds = parse_range(value, 0, 300, qualified);
    else if (section == "Audio" && key == "VoiceVolume")
        config.audio.voice_volume_percent = parse_range(value, 0, 100, qualified);
    else if (section == "Audio" && key == "HeadVoiceVolume")
        config.audio.head_voice_volume_percent = parse_range(value, 0, 200, qualified);
    else if (section == "Audio" && key == "Enable3DPlayback")
        config.audio.spatial_playback = parse_bool(value, qualified);
    else if (section == "Audio" && key == "CameraBasedAudio")
        config.audio.camera_listener = parse_bool(value, qualified);
    else if (section == "Audio" && key == "DistanceScale")
        config.audio.distance_scale = parse_decimal(value, 0.1, 20.0, qualified);
    else if (section == "Audio" && key == "PlaybackDropoffInteriorPercent")
        config.audio.interior_dropoff_percent = parse_range(value, 25, 200, qualified);
    else if (section == "Audio" && key == "PlaybackDropoffExteriorPercent")
        config.audio.exterior_dropoff_percent = parse_range(value, 25, 200, qualified);
    else if (section == "Audio" && key == "InvertHeading")
        config.audio.invert_heading = parse_bool(value, qualified);
    else if (section == "Audio" && key == "PreClipMs")
        config.audio.pre_clip_ms = parse_range(value, 0, 1000, qualified);
    else if (section == "Audio" && key == "PostClipMs")
        config.audio.post_clip_ms = parse_range(value, 0, 1000, qualified);
    else if (section == "Audio" && key == "AnimationResolution")
        config.audio.lip_animation_resolution = parse_range(value, 50, 1500, qualified);
    else if (section == "Audio" && key == "AnimationIntensity")
        config.audio.lip_animation_intensity = parse_decimal(value, 0.0, 3.0, qualified);
    else if (section == "OpenMic" && key == "Enabled")
        config.input.open_microphone = parse_bool(value, qualified);
    else if (section == "OpenMic" && key == "Muted")
        config.audio.open_mic_muted = parse_bool(value, qualified);
    else if (section == "OpenMic" && key == "Sensitivity")
        config.audio.open_mic_sensitivity = parse_range(value, 100, 10000, qualified);
    else if (section == "OpenMic" && key == "EndDelaySeconds")
        config.audio.open_mic_end_delay_seconds = parse_decimal(value, 0.1, 5.0, qualified);
    else if (section == "Tools" && key == "InitializeSYNTH")
        config.tools.initialize_requested = parse_bool(value, qualified);
    else
        throw ConfigError{"unknown configuration key: " + qualified};
}

inline void merge_text(Config& config, std::string_view text, std::string_view source) {
    std::string section;
    std::unordered_set<std::string> seen;
    std::size_t line_number = 0;
    while (!text.empty()) {
        ++line_number;
        const auto newline = text.find('\n');
        auto line = text.substr(0, newline);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (newline == std::string_view::npos) text = {};
        else text.remove_prefix(newline + 1);
        const auto cleaned = trim(line);
        if (cleaned.empty() || cleaned.front() == ';' || cleaned.front() == '#') continue;
        if (cleaned.front() == '[' && cleaned.back() == ']') {
            section = trim(std::string_view{cleaned}.substr(1, cleaned.size() - 2));
            if (section != "Server" && section != "Runtime" && section != "Limits" &&
                section != "Input" && section != "Hotkeys" && section != "AutoActivate" &&
                section != "Distance" && section != "SpatialAudio" && section != "Behavior" &&
                section != "RpgEvents" && section != "DynamicProfile" && section != "BoredEvents" &&
                section != "Rechat" &&
                section != "Audio" && section != "OpenMic" && section != "Tools") {
                throw ConfigError{"unknown configuration section at " + std::string{source} + ":" +
                                  std::to_string(line_number)};
            }
            continue;
        }
        const auto equals = cleaned.find('=');
        if (equals == std::string::npos || section.empty()) {
            throw ConfigError{"malformed configuration at " + std::string{source} + ":" +
                              std::to_string(line_number)};
        }
        const auto key = trim(std::string_view{cleaned}.substr(0, equals));
        const auto value = trim(std::string_view{cleaned}.substr(equals + 1));
        const auto qualified = section + "." + key;
        if (!seen.insert(qualified).second) {
            throw ConfigError{"duplicate configuration key: " + qualified};
        }
        apply(config, section, key, value);
    }
}

inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) throw ConfigError{"cannot read configuration file: " + path.string()};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

}  // namespace detail

[[nodiscard]] inline std::filesystem::path normalize_safe_relative_path(std::string_view value) {
    if (value.empty()) throw ConfigError{"path must not be empty"};
    std::filesystem::path path{value};
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        throw ConfigError{"path must be relative"};
    }
    path = path.lexically_normal();
    for (const auto& component : path) {
        if (component == "..") throw ConfigError{"path traversal is not allowed"};
    }
    if (path.empty() || path == ".") throw ConfigError{"path must name a file"};
    return path;
}

// Captures enough file state to detect a settings reload without opening a partially written file.
[[nodiscard]] inline FileRevision inspect_file_revision(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error || !std::filesystem::is_regular_file(status)) return {};
    const auto written = std::filesystem::last_write_time(path, error);
    if (error) return {};
    const auto size = std::filesystem::file_size(path, error);
    if (error) return {};
    return FileRevision{true, written, size};
}

[[nodiscard]] inline Config parse(std::string_view shipped_defaults,
                                  std::optional<std::string_view> custom_override = std::nullopt) {
    Config result;
    detail::merge_text(result, shipped_defaults, "shipped defaults");
    if (custom_override) detail::merge_text(result, *custom_override, "custom override");
    detail::validate(result);
    return result;
}

// The shipped defaults plus one user override are the whole persisted settings
// chain. The in-game pages, hand edits, and external tools may edit that
// override, so no other file can hold SYNTH settings state.
[[nodiscard]] inline Config load(const std::filesystem::path& shipped_defaults,
                                 const std::optional<std::filesystem::path>& custom_override =
                                     std::nullopt) {
    Config result;
    detail::merge_text(result, detail::read_file(shipped_defaults), shipped_defaults.string());
    if (custom_override && std::filesystem::exists(*custom_override)) {
        detail::merge_text(result, detail::read_file(*custom_override), custom_override->string());
    }
    detail::validate(result);
    return result;
}

}  // namespace synth::config
