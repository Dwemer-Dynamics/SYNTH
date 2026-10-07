#pragma once

// Persistence bridge for the in-game settings surface.
//
// SYNTH keeps exactly one authoritative persisted settings source: the shipped
// `Data/F4SE/Plugins/SYNTH.ini` defaults plus the user override
// `Data/F4SE/Plugins/SYNTH_custom.ini`. Every in-game edit is a single keyed
// write into that override, after which the plugin's existing revision watcher
// reloads the whole chain and rebuilds the session. Nothing in the UI holds
// settings state of its own; hand edits and external tooling use the same file.

#include "ui/settings_catalog.hpp"

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

#include <windows.h>

namespace synth::ui {

// The user override is the only file the in-game surface writes. The shipped
// defaults file stays read-only so a package reinstall never loses user edits.
[[nodiscard]] inline std::filesystem::path settings_override_path() {
    return std::filesystem::current_path() / "Data/F4SE/Plugins/SYNTH_custom.ini";
}

namespace detail {

// Serialises writes from the render thread (settings pages) against writes from
// the game thread (re-arming the Tools control).
inline std::mutex settings_write_mutex;

[[nodiscard]] inline std::wstring widen(std::string_view value) {
    if (value.empty()) return {};
    const auto length = ::MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                              static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
                          length);
    return result;
}

}  // namespace detail

// Writes one key into the user override. Returns false when the file could not
// be updated so the caller can surface a notification instead of silently
// dropping the edit.
[[nodiscard]] inline bool write_setting(std::string_view section, std::string_view key,
                                        std::string_view value) {
    const auto path = settings_override_path();
    const auto wide_section = detail::widen(section);
    const auto wide_key = detail::widen(key);
    const auto wide_value = detail::widen(value);
    if (wide_section.empty() || wide_key.empty() || wide_value.empty()) return false;

    std::scoped_lock lock{detail::settings_write_mutex};
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (!::WritePrivateProfileStringW(wide_section.c_str(), wide_key.c_str(), wide_value.c_str(),
                                      path.c_str())) {
        return false;
    }
    // Windows buffers profile writes; flush so the revision watcher observes the
    // change on the very next pump instead of an arbitrary number of frames later.
    ::WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return true;
}

[[nodiscard]] inline bool write_setting(const Setting& setting, double value) {
    return write_setting(setting.section, setting.key, format_value(setting, value));
}

}  // namespace synth::ui
