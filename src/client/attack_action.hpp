#pragma once
#include "client/wait_action.hpp"
#include <cstdio>

namespace synth::client {
// Attack never resolves names on the client: only this NPC's captured current visible hostile is eligible.
[[nodiscard]] inline runtime::RuntimeActionRequest prepare_attack_action(const core::RuntimeSnapshot& snapshot,
    const protocol_native::Action& action, const core::CancellationToken& turn,
    core::SnapshotClock::time_point deadline) {
    if (snapshot.variant()!=core::RuntimeVariant::flat || snapshot.game()!=core::Game::fallout4 ||
        turn.is_cancelled() || snapshot.generation()!=turn.generation() || snapshot.frame()==0 ||
        deadline<=core::SnapshotClock::now() || action.name!="attack" || action.capability!="action.attack" ||
        action.action_id.empty() || action.action_id.size()>128 || !action.target ||
        !action.arguments.is_object() || !action.arguments.as_object().empty())
        throw std::invalid_argument{"invalid attack ownership or arguments"};
    (void)protocol_native::identity(protocol_native::identity_value(action.actor));
    (void)protocol_native::identity(protocol_native::identity_value(*action.target));
    const auto matches=[](const auto& captured,const auto& wire) {
        char form[11]{}; std::snprintf(form,sizeof(form),"0x%08X",captured.form_id);
        return std::string_view{form}==wire.form_id && captured.origin_plugin==wire.origin_plugin &&
            captured.playthrough_id==wire.playthrough_id;
    };
    const core::ActorSnapshot* performer{};
    for (const auto& actor:snapshot.actors()) {
        core::CombatantIdentity id{actor.form_id(),actor.origin_plugin(),actor.playthrough_id(),actor.name(),actor.race()};
        if (!matches(id,action.actor)) continue;
        if (performer || !actor.alive() || actor.disabled() || !actor.teammate() || actor.hostile_to_player())
            throw std::invalid_argument{"attack performer is not an available follower"};
        performer=&actor;
    }
    if (!performer || performer->form_id()==0x14 || !performer->enemy_awareness().available)
        throw std::invalid_argument{"attack performer has no current visual contacts"};
    const core::VisibleEnemySnapshot* selected{};
    for (const auto& enemy:performer->enemy_awareness().current) if (matches(enemy.actor,*action.target)) {
        if (selected || !enemy.hostile || !enemy.visible || enemy.distance>2000 || enemy.actor.form_id==0x14 ||
            enemy.actor.form_id==performer->form_id()) throw std::invalid_argument{"invalid attack target"};
        selected=&enemy;
    }
    if (!selected) throw std::invalid_argument{"attack target is not this follower's current visible hostile"};
    runtime::RuntimeActionRequest request{runtime::RuntimeActionName::attack,
        {performer->form_id(),performer->origin_plugin(),performer->playthrough_id()}, {}, turn, deadline, action.action_id};
    request.recipient=runtime::RuntimeActorIdentity{selected->actor.form_id,selected->actor.origin_plugin,selected->actor.playthrough_id};
    return request;
}
} // namespace synth::client
