#pragma once

#include "integration/prompt_target.hpp"
#include "targeting/targeting.hpp"
#include "diagnostics/conversation_trace.hpp"
#include <atomic>
#include <memory>
#include <vector>

namespace synth::presentation {

// Exterior cells partition one physical worldspace. Interior cell boundaries are actual scene exits.
[[nodiscard]] inline bool same_presentation_scene(const core::WorldState& original,
                                                 const core::WorldState& current) noexcept {
    if (original.interior != current.interior || !original.scene || !current.scene ||
        !original.scene->valid(original.interior) || !current.scene->valid(current.interior)) return false;
    if (original.interior) return original.scene == current.scene;
    return original.scene->worldspace_form_id == current.scene->worldspace_form_id &&
        original.scene->worldspace_origin_plugin == current.scene->worldspace_origin_plugin;
}

// Copied utterance ownership plus a short-lived game-thread proof shared by audio and text.
struct SceneAdmission final {
    using Clock = core::SnapshotClock;
    std::shared_ptr<const core::RuntimeSnapshot> original;
    std::vector<integration::PromptTarget> actors;
    std::atomic_bool rejected{};
    std::atomic<std::int64_t> verified_until{};

    // Presentation checks availability and identity, not input cooldown or crosshair selection.
    [[nodiscard]] std::optional<diagnostics::ConversationStage> rejection_reason(
        const core::RuntimeSnapshot* snapshot, Clock::time_point now) const {
        using Stage = diagnostics::ConversationStage;
        if (!snapshot) return Stage::presentation_missing_capture;
        if (!original || rejected.load(std::memory_order_acquire) || snapshot->generation() != original->generation() ||
            snapshot->variant() != original->variant()) return Stage::presentation_owner_changed;
        if (now < snapshot->captured_at() || now - snapshot->captured_at() >= std::chrono::milliseconds{250} ||
            snapshot->captured_at() < original->captured_at()) return Stage::presentation_stale_capture;
        if (!snapshot->world() || !original->world() ||
            !same_presentation_scene(*original->world(), *snapshot->world())) return Stage::presentation_scene_changed;
        const auto& player = original->player();
        const integration::PromptTarget retained_player{{},player.form_id(),player.base_form_id(),player.origin_plugin(),
            player.base_origin_plugin(),player.playthrough_id(),player.name(),true};
        if (!retained_player.matches(snapshot->player())) return Stage::presentation_actor_identity_changed;
        for (const auto& retained : actors) {
            if (retained.session.is_cancelled()) return Stage::presentation_actor_cancelled;
            const auto found = std::ranges::find_if(snapshot->actors(),[&](const auto& actor) {
                return actor.form_id() == retained.form_id;
            });
            if (found == snapshot->actors().end()) return Stage::presentation_actor_missing;
            if (!retained.matches(*found)) return Stage::presentation_actor_identity_changed;
            if (!targeting::dialogue_actor_available(*found))
                return Stage::presentation_actor_unavailable;
            if (targeting::distance(snapshot->player_pose().position(),found->position()) > targeting::SelectionBounds{}.maximum_distance)
                return Stage::presentation_actor_out_of_range;
        }
        return std::nullopt;
    }

    [[nodiscard]] bool matches(const core::RuntimeSnapshot& snapshot, Clock::time_point now) const {
        return !rejection_reason(&snapshot, now);
    }

    [[nodiscard]] bool ready(Clock::time_point now) const noexcept {
        return !rejected.load(std::memory_order_acquire) &&
            std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count() <
                verified_until.load(std::memory_order_acquire);
    }
    void accept(Clock::time_point captured) noexcept {
        verified_until.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
            (captured + std::chrono::milliseconds{250}).time_since_epoch()).count(),std::memory_order_release);
    }
};

} // namespace synth::presentation
