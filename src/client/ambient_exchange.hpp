#pragma once

#include "targeting/targeting.hpp"
#include <span>

namespace synth::client {

[[nodiscard]] inline bool ambient_player_voice_active(bool recording, bool passive_open_mic,
                                                       bool voice_detected) noexcept {
    return recording && (!passive_open_mic || voice_detected);
}

[[nodiscard]] inline bool ambient_actor_class_supported(std::string_view label) {
    std::string race{label};
    std::ranges::transform(race, race.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (race.find("feral") != std::string::npos) return false;
    return targeting::automatic_actor_class_supported(race, false) || race.find("handy") != std::string::npos ||
        race.find("protectron") != std::string::npos || race.find("assaultron") != std::string::npos ||
        race.find("eyebot") != std::string::npos || race.find("sentrybot") != std::string::npos;
}

struct AmbientPair final {
    const core::ActorSnapshot* speaker{};
    const core::ActorSnapshot* listener{};
};

// Copied scene only. Close proximity can be heard without a known LOS; unknown
// LOS never authorizes a farther actor. No engine lookup or new audience claim.
[[nodiscard]] inline std::optional<AmbientPair> select_ambient_pair(
    const core::RuntimeSnapshot& snapshot, core::SnapshotClock::time_point now,
    core::RuntimeGeneration generation, double hearing_range, double proximity,
    const std::function<bool(const core::ActorSnapshot&)>& eligible = {}) {
    if (snapshot.game() != core::Game::fallout4 || snapshot.generation() != generation || snapshot.frame() == 0 ||
        now < snapshot.captured_at() || now - snapshot.captured_at() > std::chrono::seconds{2} ||
        !snapshot.player().alive() || snapshot.player().disabled() || snapshot.player().in_combat() ||
        !std::isfinite(hearing_range) || hearing_range <= 0 || !std::isfinite(proximity) || proximity <= 0 ||
        (snapshot.actors_observation() != "complete" && snapshot.actors_observation() != "partial")) return {};
    if (snapshot.variant() == core::RuntimeVariant::vr && (!snapshot.hmd_pose() ||
        !snapshot.hmd_pose()->fresh_at(now) || snapshot.hmd_pose()->frame() != snapshot.frame())) return {};
    const auto* listener = snapshot.effective_listener_pose(now);
    if (!listener) return {};
    const auto range = std::min(2000.0, hearing_range);
    const auto close = std::min({784.0, range, proximity});
    const context::PlaythroughIdentity playthrough{snapshot.player().playthrough_id()};
    if (!targeting::snapshot_identity_matches(snapshot, playthrough)) return {};
    // The wire has room for player plus15 actors. Pick from that exact nearest
    // set so selecting the speaker cannot push the prospective listener out.
    const auto wire = targeting::context_participants(snapshot, range);
    std::vector<const core::ActorSnapshot*> candidates;
    for (const auto* actor : std::span{wire}.first(std::min<std::size_t>(15, wire.size()))) {
        if (!actor || actor->form_id() == snapshot.player().form_id() || actor->form_id() == 0x14 ||
            !actor->conversation_area_available() || !targeting::dialogue_actor_available(*actor) ||
            actor->in_combat() || actor->hostile_to_player() || actor->talking_to_player() ||
            actor->life_state() != "alive" ||
            (actor->posture() != "normal" && actor->posture() != "sitting") || actor->line_of_sight() == false ||
            !ambient_actor_class_supported(actor->race()) || (eligible && !eligible(*actor)) ||
            std::ranges::count_if(snapshot.actors(), [&](const auto& other) { return other.form_id() == actor->form_id(); }) != 1 ||
            !targeting::nearby_conversation_audible(range, close,
                targeting::distance(listener->position(), actor->position()), actor->line_of_sight())) continue;
        candidates.push_back(actor);
    }
    std::ranges::sort(candidates, [&](const auto* left, const auto* right) {
        const auto ld = targeting::distance(listener->position(), left->position());
        const auto rd = targeting::distance(listener->position(), right->position());
        return ld != rd ? ld < rd : context::identity_of(*left) < context::identity_of(*right);
    });
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        for (std::size_t j = i + 1; j < candidates.size(); ++j) {
            // LOS is observed from the player, not between NPCs. Require close
            // mutual proximity rather than fabricating an NPC-to-NPC sight ray.
            if (targeting::distance(candidates[i]->position(), candidates[j]->position()) <= close)
                return AmbientPair{candidates[i], candidates[j]};
        }
    }
    return {};
}

} // namespace synth::client
