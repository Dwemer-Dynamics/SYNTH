#pragma once

#include "protocol_native/v1_codec.hpp"

namespace synth::client {

// Event scenes retain dead actors. Visual evidence belongs only to a living NPC observer;
// an empty contact array still claims that observer and must not be attached to a death event.
inline void append_actor_combat_observations(protocol_native::ActorState& state,
    const core::ActorSnapshot& actor, bool contacts_ready, bool awareness_ready) {
    if (!actor.alive() || actor.disabled() || actor.form_id()==0x14) return;
    const auto copied=[](const core::CombatantIdentity& value) {
        char form[11]{};std::snprintf(form,sizeof(form),"0x%08X",value.form_id);
        return protocol_native::Identity{form,value.origin_plugin,value.playthrough_id,value.display_name};
    };
    if (contacts_ready) {
        state.combat_contacts=std::vector<protocol_native::CombatContactState>{};
        for (const auto& contact:actor.combat_contacts()) state.combat_contacts->push_back({
            copied(contact.actor),copied(contact.target),contact.actor.race,
            contact.distance,contact.hostile,contact.in_combat,contact.visible ? "visible" : "unknown"});
    }
    if (awareness_ready && actor.enemy_awareness().available) {
        state.enemy_awareness=protocol_native::EnemyAwarenessState{};
        for (const auto& item:actor.enemy_awareness().current) state.enemy_awareness->current.push_back({
            copied(item.actor),item.actor.race,item.distance,item.hostile,item.in_combat,item.visible ? "visible" : "unknown"});
        for (const auto& item:actor.enemy_awareness().recent) state.enemy_awareness->recent.push_back({
            copied(item.actor),item.actor.race,item.observation,item.age_ms});
    }
}

} // namespace synth::client
