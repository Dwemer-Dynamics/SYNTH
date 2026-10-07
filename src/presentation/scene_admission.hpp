#pragma once

#include "integration/prompt_target.hpp"
#include "targeting/targeting.hpp"
#include <atomic>
#include <memory>
#include <vector>

namespace synth::presentation {

// Copied utterance ownership plus a short-lived game-thread proof shared by audio and text.
struct SceneAdmission final {
    using Clock = core::SnapshotClock;
    std::shared_ptr<const core::RuntimeSnapshot> original;
    std::vector<integration::PromptTarget> actors;
    std::atomic_bool rejected{};
    std::atomic<std::int64_t> verified_until{};

    // Presentation checks availability and identity, not input cooldown or crosshair selection.
    [[nodiscard]] bool matches(const core::RuntimeSnapshot& snapshot, Clock::time_point now) const {
        if (!original || rejected.load(std::memory_order_acquire) || snapshot.generation() != original->generation() ||
            snapshot.variant() != original->variant() || now < snapshot.captured_at() ||
            now - snapshot.captured_at() >= std::chrono::milliseconds{250} ||
            snapshot.captured_at() < original->captured_at() || !snapshot.world() || !original->world() ||
            !snapshot.world()->scene || !original->world()->scene ||
            !snapshot.world()->scene->valid(snapshot.world()->interior) ||
            snapshot.world()->interior != original->world()->interior ||
            snapshot.world()->scene != original->world()->scene) return false;
        const auto& player = original->player();
        const integration::PromptTarget retained_player{{},player.form_id(),player.base_form_id(),player.origin_plugin(),
            player.base_origin_plugin(),player.playthrough_id(),player.name(),true};
        if (!retained_player.matches(snapshot.player())) return false;
        for (const auto& retained : actors) {
            if (retained.session.is_cancelled()) return false;
            const auto found = std::ranges::find_if(snapshot.actors(),[&](const auto& actor) { return retained.matches(actor); });
            if (found == snapshot.actors().end() || !targeting::dialogue_actor_available(*found) ||
                targeting::distance(snapshot.player_pose().position(),found->position()) > targeting::SelectionBounds{}.maximum_distance)
                return false;
        }
        return true;
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
