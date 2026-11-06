#pragma once

#include "combat_target.hpp"
#include <cmath>
#include <vector>

namespace synth::core {

// A visual incoming-attacker observation owned by one NPC, not a conversation participant.
struct CombatContactSnapshot final {
    CombatantIdentity actor, target;
    double distance{};
    bool hostile{}, in_combat{}, visible{};

    [[nodiscard]] bool valid_for(std::uint32_t owner_id, std::string_view owner_plugin,
                                std::string_view playthrough, bool owner_alive, bool owner_disabled) const noexcept {
        return owner_id != 0x14 && owner_alive && !owner_disabled && actor.valid() && target.valid() &&
            actor.playthrough_id == playthrough && target.playthrough_id == playthrough &&
            actor.form_id != owner_id && actor.form_id != 0x14 && actor.form_id != target.form_id &&
            ((target.form_id == owner_id && target.origin_plugin == owner_plugin) ||
             (target.form_id == 0x14 && target.origin_plugin == "Fallout4.esm")) &&
            std::isfinite(distance) && distance >= 0.0 && distance <= 2000.0 && hostile && in_combat && visible;
    }
    bool operator==(const CombatContactSnapshot&) const = default;
};

inline bool valid_combat_contacts(const std::vector<CombatContactSnapshot>& contacts,
                                 std::uint32_t owner_id, std::string_view owner_plugin,
                                 std::string_view playthrough, bool alive, bool disabled) noexcept {
    if (contacts.size() > 8) return false;
    for (std::size_t i=0; i<contacts.size(); ++i) {
        if (!contacts[i].valid_for(owner_id,owner_plugin,playthrough,alive,disabled)) return false;
        for (std::size_t j=0; j<i; ++j)
            if (contacts[i].actor.form_id == contacts[j].actor.form_id &&
                contacts[i].actor.origin_plugin == contacts[j].actor.origin_plugin) return false;
    }
    return true;
}

} // namespace synth::core
