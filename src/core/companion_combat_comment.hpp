#pragma once

#include "core/player_event_sampler.hpp"
#include "core/snapshot.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace synth::core {

// Engine-free postcombat companion remark policy over copied scenes and acknowledged combat_end
// evidence. Game thread only; the session owns admission, the shared claim and the owned request.
inline constexpr auto companion_comment_quiet = std::chrono::seconds{3};
inline constexpr auto companion_comment_cooldown = std::chrono::seconds{30};
inline constexpr auto companion_comment_start_budget = std::chrono::seconds{15};

enum class CompanionCommentStep : unsigned char { wait, drop, decide };

// Quiet restarts for each newer end and whenever a recorded participant is still seen fighting.
struct CompanionCommentQuiet final {
    std::uint64_t serial{};
    SnapshotClock::time_point since{};
};

[[nodiscard]] inline bool same_participant(const CombatParticipant& participant, const ActorSnapshot& actor) noexcept {
    return participant.form_id == actor.form_id() && participant.origin_plugin == actor.origin_plugin() &&
        participant.playthrough_id == actor.playthrough_id();
}

[[nodiscard]] inline CompanionCommentStep companion_comment_step(CompanionCommentQuiet& quiet, const PlayerEventTransition& end,
    const RuntimeSnapshot& scene, bool player_in_combat, SnapshotClock::time_point now) {
    // Renewed player combat or an end without original participant evidence is never a remark.
    if (end.kind != PlayerEventKind::combat_end || end.participants.empty() || player_in_combat ||
        scene.player().in_combat()) return CompanionCommentStep::drop;
    if (quiet.serial != end.serial) quiet = {end.serial, end.after.observed_at};
    for (const auto& actor : scene.actors())
        for (const auto& participant : end.participants)
            if (actor.in_combat() && same_participant(participant, actor) && scene.captured_at() > quiet.since)
                quiet.since = scene.captured_at();
    return now >= quiet.since && now - quiet.since >= companion_comment_quiet
        ? CompanionCommentStep::decide : CompanionCommentStep::wait;
}

// A recorded participant still following now: teammate, not SYNTH-held, alive, calm and within hearing.
// Vanilla wait/dismissal is visible only through the copied teammate bit.
[[nodiscard]] inline bool companion_still_following(const ActorSnapshot& actor, const RuntimeSnapshot& scene, double hearing) {
    const auto& position = scene.player_pose().position();
    const auto dx = actor.position().x - position.x, dy = actor.position().y - position.y, dz = actor.position().z - position.z;
    return actor.teammate() && actor.wait_held() != true && actor.alive() && !actor.disabled() &&
        actor.life_state() == "alive" && !actor.in_combat() && !actor.hostile_to_player() &&
        !scene.player().in_combat() && dx * dx + dy * dy + dz * dz <= hearing * hearing;
}

// Nearest eligible participant, ties broken by reference ID; no owner or crosshair preference.
template <class Eligible>
[[nodiscard]] const ActorSnapshot* select_companion_speaker(const RuntimeSnapshot& scene, const PlayerEventTransition& end,
                                                            double hearing, Eligible&& eligible) {
    const ActorSnapshot* selected{};
    double selected_distance{};
    const auto& position = scene.player_pose().position();
    for (const auto& actor : scene.actors()) {
        if (actor.playthrough_id() != scene.player().playthrough_id() || !companion_still_following(actor, scene, hearing) ||
            actor.talking_to_player() || !std::ranges::any_of(end.participants, [&](const auto& participant) {
                return same_participant(participant, actor); }) || !eligible(actor)) continue;
        const auto dx = actor.position().x - position.x, dy = actor.position().y - position.y, dz = actor.position().z - position.z;
        const auto distance = dx * dx + dy * dy + dz * dz;
        if (!selected || distance < selected_distance ||
            (distance == selected_distance && actor.form_id() < selected->form_id())) {
            selected = &actor;
            selected_distance = distance;
        }
    }
    return selected;
}

}  // namespace synth::core
