#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace synth::core {

// Copied from one actor's current native target; this never grants conversation audience membership.
struct CombatantIdentity final {
    std::uint32_t form_id{};
    std::string origin_plugin, playthrough_id, display_name, race;

    [[nodiscard]] bool valid() const noexcept {
        return form_id != 0 && !origin_plugin.empty() && origin_plugin.size() <= 255 &&
            origin_plugin.find_first_of("/\\") == std::string::npos &&
            (origin_plugin.ends_with(".esm") || origin_plugin.ends_with(".esp") || origin_plugin.ends_with(".esl")) &&
            !playthrough_id.empty() && playthrough_id.size() <= 128 &&
            display_name.find_first_not_of(" \t\r\n\v\f") != std::string::npos && display_name.size() <= 255 && race.size() <= 255;
    }

    bool operator==(const CombatantIdentity&) const = default;
};

struct CombatTargetSnapshot final {
    // none attests no current native target; unavailable includes a target handle that could not be resolved safely.
    std::string observation{"unavailable"};
    std::optional<CombatantIdentity> actor;

    [[nodiscard]] bool valid_for(std::uint32_t owner_form_id, std::string_view owner_plugin,
                                std::string_view playthrough, bool alive, bool disabled, bool in_combat) const noexcept {
        if (observation == "none" || observation == "unavailable") return !actor;
        const auto lower = [](unsigned char value) { return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value; };
        const bool same_plugin = actor && actor->origin_plugin.size() == owner_plugin.size() &&
            std::equal(actor->origin_plugin.begin(), actor->origin_plugin.end(), owner_plugin.begin(),
                [&](unsigned char left, unsigned char right) { return lower(left) == lower(right); });
        return observation == "identified" && actor && actor->valid() && alive && !disabled && in_combat &&
            actor->playthrough_id == playthrough &&
            (actor->form_id != owner_form_id || !same_plugin);
    }

    bool operator==(const CombatTargetSnapshot&) const = default;
};

}  // namespace synth::core
