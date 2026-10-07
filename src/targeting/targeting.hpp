#pragma once

#include "context/context.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace synth::targeting {

// Reject actors whose current native state cannot safely support a dialogue turn.
[[nodiscard]] inline bool dialogue_actor_available(const core::ActorSnapshot& actor) noexcept {
    if (!actor.alive() || actor.disabled()) return false;

    const auto& life_state = actor.life_state();
    if (life_state == "dying" || life_state == "dead" || life_state == "unconscious" ||
        life_state == "reanimate" || life_state == "recycle" ||
        life_state == "essential_down" || life_state == "bleedout") {
        return false;
    }

    // Restraint blocks locomotion, not conversation; Wait Here actors must remain targetable.
    const auto& posture = actor.posture();
    return posture != "want_to_sleep" && posture != "waiting_for_sleep" &&
           posture != "sleeping";
}

// Keep automatic activation conservative while allowing Fallout's conversational actor classes.
[[nodiscard]] inline bool automatic_actor_class_supported(std::string_view race_editor_id,
                                                          bool include_creatures) {
    if (include_creatures) return true;
    std::string race{race_editor_id};
    std::ranges::transform(race, race.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return race.find("human") != std::string::npos ||
           race.find("ghoul") != std::string::npos ||
           race.find("supermutant") != std::string::npos ||
           race.find("synth") != std::string::npos ||
           race.find("robot") != std::string::npos;
}

struct Ray final {
    core::Vec3 origin;
    core::UnitVector3 direction;
};

[[nodiscard]] inline core::Vec3 subtract(core::Vec3 left, core::Vec3 right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] inline double dot(core::Vec3 left, core::Vec3 right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

[[nodiscard]] inline double distance(core::Vec3 left, core::Vec3 right) noexcept {
    const auto delta = subtract(left, right);
    return std::hypot(delta.x, delta.y, delta.z);
}

struct RayGeometry final {
    double along{};
    double perpendicular{};
};

[[nodiscard]] inline RayGeometry geometry_to(const Ray& ray, core::Vec3 point) noexcept {
    const auto relative = subtract(point, ray.origin);
    const auto along = dot(relative, ray.direction.value());
    const auto squared = dot(relative, relative) - along * along;
    return {along, std::sqrt(std::max(0.0, squared))};
}

enum class TargetingStatus : unsigned char {
    selected,
    no_candidate,
    unavailable_pose,
    stale_pose,
    identity_mismatch,
};

struct TargetingResult final {
    TargetingStatus status{TargetingStatus::no_candidate};
    std::optional<context::TargetContext> target;
};

struct SelectionBounds final {
    double maximum_distance{4096.0};
    double maximum_ray_radius{75.0};

    void validate() const {
        if (!std::isfinite(maximum_distance) || maximum_distance <= 0.0 ||
            !std::isfinite(maximum_ray_radius) || maximum_ray_radius < 0.0) {
            throw std::invalid_argument{"target selection bounds are invalid"};
        }
    }
};

// Reuse SYNTH's established mode distances for both recipient selection and audience capture.
[[nodiscard]] inline double conversation_range(double base, std::string_view mode) noexcept {
    if (mode == "WHISPER" || mode == "CLOSE") return 200.0;
    return mode == "SHOUT" ? base * 2.0 : base;
}

// Unknown LOS is not proof of long-range hearing. Close proximity does not require looking.
[[nodiscard]] inline bool nearby_conversation_audible(double range, double proximity,
    double actor_distance, std::optional<bool> line_of_sight) noexcept {
    return std::isfinite(actor_distance) && actor_distance >= 0 && actor_distance <= range &&
        (actor_distance <= proximity || line_of_sight.value_or(false));
}

[[nodiscard]] inline bool snapshot_identity_matches(const core::RuntimeSnapshot& snapshot,
                                                    const context::PlaythroughIdentity& playthrough) {
    return std::ranges::all_of(snapshot.actors(), [&](const core::ActorSnapshot& actor) {
        return actor.playthrough_id() == playthrough.value();
    });
}

[[nodiscard]] inline TargetingResult select_by_ray(
    const core::RuntimeSnapshot& snapshot,
    const context::PlaythroughIdentity& playthrough,
    const Ray& ray,
    SelectionBounds bounds = {},
    const std::function<bool(const core::ActorSnapshot&)>& eligible = {}) {
    bounds.validate();
    if (!snapshot_identity_matches(snapshot, playthrough)) {
        return {.status = TargetingStatus::identity_mismatch, .target = std::nullopt};
    }

    const core::ActorSnapshot* best{};
    RayGeometry best_geometry{};
    for (const auto& actor : snapshot.actors()) {
        if (eligible && !eligible(actor)) {
            continue;
        }
        const auto geometry = geometry_to(ray, actor.position());
        if (geometry.along < 0.0 || geometry.along > bounds.maximum_distance ||
            geometry.perpendicular > bounds.maximum_ray_radius) {
            continue;
        }
        if (best == nullptr || geometry.perpendicular < best_geometry.perpendicular ||
            (geometry.perpendicular == best_geometry.perpendicular && geometry.along < best_geometry.along) ||
            (geometry.perpendicular == best_geometry.perpendicular && geometry.along == best_geometry.along &&
             context::identity_of(actor) < context::identity_of(*best))) {
            best = &actor;
            best_geometry = geometry;
        }
    }
    if (best == nullptr) {
        return {.status = TargetingStatus::no_candidate, .target = std::nullopt};
    }
    return {.status = TargetingStatus::selected,
            .target = context::TargetContext{context::identity_of(*best),
                                             best->name(),
                                             best->position(),
                                             distance(ray.origin, best->position()),
                                             snapshot.generation(),
                                             snapshot.frame()}};
}

[[nodiscard]] inline TargetingResult select_nearest(
    const core::RuntimeSnapshot& snapshot,
    const context::PlaythroughIdentity& playthrough,
    core::Vec3 origin,
    double maximum_distance,
    const std::function<bool(const core::ActorSnapshot&)>& eligible = {}) {
    if (!origin.finite() || !std::isfinite(maximum_distance) || maximum_distance <= 0.0) {
        throw std::invalid_argument{"nearest selection bounds are invalid"};
    }
    if (!snapshot_identity_matches(snapshot, playthrough)) {
        return {.status = TargetingStatus::identity_mismatch, .target = std::nullopt};
    }
    const core::ActorSnapshot* best{};
    double best_distance{};
    for (const auto& actor : snapshot.actors()) {
        if (eligible && !eligible(actor)) {
            continue;
        }
        const auto candidate_distance = distance(origin, actor.position());
        if (candidate_distance > maximum_distance) {
            continue;
        }
        if (best == nullptr || candidate_distance < best_distance ||
            (candidate_distance == best_distance && context::identity_of(actor) < context::identity_of(*best))) {
            best = &actor;
            best_distance = candidate_distance;
        }
    }
    if (best == nullptr) {
        return {.status = TargetingStatus::no_candidate, .target = std::nullopt};
    }
    return {.status = TargetingStatus::selected,
            .target = context::TargetContext{context::identity_of(*best), best->name(), best->position(),
                                             best_distance, snapshot.generation(), snapshot.frame()}};
}

[[nodiscard]] inline TargetingResult select_flat_crosshair(
    const core::RuntimeSnapshot& snapshot,
    const context::PlaythroughIdentity& playthrough,
    SelectionBounds bounds = {},
    const std::function<bool(const core::ActorSnapshot&)>& eligible = {}) {
    if (snapshot.variant() != core::RuntimeVariant::flat) {
        return {.status = TargetingStatus::unavailable_pose, .target = std::nullopt};
    }
    bounds.validate();
    if (!snapshot_identity_matches(snapshot, playthrough))
        return {.status = TargetingStatus::identity_mismatch, .target = std::nullopt};
    const auto picked = snapshot.picked_actor_form_id();
    if (!picked || *picked == 0) return {};
    const auto found = std::ranges::find_if(snapshot.actors(), [&](const auto& actor) {
        return actor.form_id() == *picked;
    });
    if (found == snapshot.actors().end() || (eligible && !eligible(*found))) return {};
    const auto range = distance(snapshot.player_pose().position(), found->position());
    if (range > bounds.maximum_distance) return {};
    return {.status = TargetingStatus::selected,
            .target = context::TargetContext{context::identity_of(*found), found->name(), found->position(),
                                             range, snapshot.generation(), snapshot.frame()}};
}

[[nodiscard]] inline TargetingResult select_vr_hmd_gaze(
    const core::RuntimeSnapshot& snapshot,
    const context::PlaythroughIdentity& playthrough,
    core::SnapshotClock::time_point now,
    SelectionBounds bounds = {},
    const std::function<bool(const core::ActorSnapshot&)>& eligible = {}) {
    if (snapshot.variant() != core::RuntimeVariant::vr || !snapshot.hmd_pose()) {
        return {.status = TargetingStatus::unavailable_pose, .target = std::nullopt};
    }
    if (!snapshot.hmd_pose()->fresh_at(now) || snapshot.hmd_pose()->frame() != snapshot.frame()) {
        return {.status = TargetingStatus::stale_pose, .target = std::nullopt};
    }
    const auto& pose = snapshot.hmd_pose()->pose();
    return select_by_ray(snapshot, playthrough, Ray{pose.position(), pose.forward()}, bounds, eligible);
}

enum class Hand : unsigned char { left, right };

class ControllerRay final {
public:
    ControllerRay(Hand hand, core::TimedPose pose, bool ray_capable)
        : hand_{hand}, pose_{std::move(pose)}, ray_capable_{ray_capable} {}

    [[nodiscard]] Hand hand() const noexcept { return hand_; }
    [[nodiscard]] const core::TimedPose& pose() const noexcept { return pose_; }
    [[nodiscard]] bool ray_capable() const noexcept { return ray_capable_; }

private:
    Hand hand_;
    core::TimedPose pose_;
    bool ray_capable_;
};

[[nodiscard]] inline TargetingResult select_vr_controller_ray(
    const core::RuntimeSnapshot& snapshot,
    const context::PlaythroughIdentity& playthrough,
    const std::optional<ControllerRay>& controller,
    core::SnapshotClock::time_point now,
    SelectionBounds bounds = {},
    const std::function<bool(const core::ActorSnapshot&)>& eligible = {}) {
    if (snapshot.variant() != core::RuntimeVariant::vr || !controller || !controller->ray_capable()) {
        return {.status = TargetingStatus::unavailable_pose, .target = std::nullopt};
    }
    if (!controller->pose().fresh_at(now) || controller->pose().frame() != snapshot.frame()) {
        return {.status = TargetingStatus::stale_pose, .target = std::nullopt};
    }
    const auto& pose = controller->pose().pose();
    return select_by_ray(snapshot, playthrough, Ray{pose.position(), pose.forward()}, bounds, eligible);
}

[[nodiscard]] inline TargetingResult select_vr_controller_ray(
    const core::RuntimeSnapshot& snapshot,
    const context::PlaythroughIdentity& playthrough,
    Hand hand,
    core::SnapshotClock::time_point now,
    SelectionBounds bounds = {},
    const std::function<bool(const core::ActorSnapshot&)>& eligible = {}) {
    const auto& pose = hand == Hand::left ? snapshot.left_controller_pose()
                                         : snapshot.right_controller_pose();
    if (!pose) {
        return {.status = TargetingStatus::unavailable_pose, .target = std::nullopt};
    }
    return select_vr_controller_ray(snapshot, playthrough,
                                    ControllerRay{hand, *pose, true}, now, bounds, eligible);
}

// Direct intent wins; only conversation input may fall back to a nearby listener.
[[nodiscard]] inline TargetingResult select_conversation(
    const core::RuntimeSnapshot& snapshot, const context::PlaythroughIdentity& playthrough,
    core::SnapshotClock::time_point now, SelectionBounds bounds,
    const std::function<bool(const core::ActorSnapshot&)>& direct_eligible,
    const std::function<bool(const core::ActorSnapshot&)>& nearby_eligible, bool* nearby = nullptr) {
    if (nearby) *nearby = false;
    auto origin = snapshot.player_pose().position();
    TargetingResult selected;
    if (snapshot.variant() == core::RuntimeVariant::flat) {
        selected = select_flat_crosshair(snapshot, playthrough, bounds, direct_eligible);
    } else {
        if (!snapshot.hmd_pose()) return {.status = TargetingStatus::unavailable_pose};
        if (!snapshot.hmd_pose()->fresh_at(now) || snapshot.hmd_pose()->frame() != snapshot.frame())
            return {.status = TargetingStatus::stale_pose};
        origin = snapshot.hmd_pose()->pose().position();
        selected = select_vr_controller_ray(snapshot, playthrough, Hand::right, now, bounds, direct_eligible);
        if (!selected.target) selected = select_vr_hmd_gaze(snapshot, playthrough, now, bounds, direct_eligible);
    }
    if (selected.target || selected.status == TargetingStatus::identity_mismatch) return selected;
    selected = select_nearest(snapshot, playthrough, origin, bounds.maximum_distance, nearby_eligible);
    if (nearby) *nearby = selected.target.has_value();
    return selected;
}

[[nodiscard]] inline context::AudienceContext build_audience(
    const core::RuntimeSnapshot& snapshot,
    const context::PlaythroughIdentity& playthrough,
    core::Vec3 origin,
    double maximum_distance,
    std::size_t capacity) {
    if (capacity == 0) {
        throw std::invalid_argument{"audience capacity must be positive"};
    }
    if (!snapshot_identity_matches(snapshot, playthrough)) {
        throw std::invalid_argument{"audience playthrough identity mismatch"};
    }
    std::vector<context::TargetContext> members;
    members.reserve(std::min(capacity, snapshot.actors().size()));
    for (const auto& actor : snapshot.actors()) {
        const auto candidate_distance = distance(origin, actor.position());
        if (candidate_distance <= maximum_distance) {
            members.emplace_back(context::identity_of(actor), actor.name(), actor.position(), candidate_distance,
                                 snapshot.generation(), snapshot.frame());
        }
    }
    std::ranges::sort(members, [](const auto& left, const auto& right) {
        return left.distance() < right.distance() ||
               (left.distance() == right.distance() && left.identity() < right.identity());
    });
    if (members.size() > capacity) {
        members.erase(members.begin() + static_cast<std::ptrdiff_t>(capacity), members.end());
    }
    std::ranges::sort(members, [](const auto& left, const auto& right) {
        return left.identity() < right.identity();
    });
    return context::AudienceContext{capacity, std::move(members)};
}

// Reserve packet capacity for the addressed actor before nearby bystanders. Pointers stay snapshot-owned.
[[nodiscard]] inline std::vector<const core::ActorSnapshot*> context_participants(
    const core::RuntimeSnapshot& snapshot, double hearing_distance,
    const std::function<bool(const core::ActorSnapshot&)>& selected = {}) {
    if (!std::isfinite(hearing_distance) || hearing_distance < 0.0)
        throw std::invalid_argument{"context hearing distance is invalid"};
    std::vector<const core::ActorSnapshot*> result;
    result.reserve(16);
    const auto eligible = [&](const core::ActorSnapshot& actor) {
        return actor.playthrough_id() == snapshot.player().playthrough_id() && dialogue_actor_available(actor);
    };
    if (selected && !selected(snapshot.player())) {
        const auto found = std::ranges::find_if(snapshot.actors(), [&](const auto& actor) {
            return selected(actor) && eligible(actor);
        });
        if (found == snapshot.actors().end())
            throw std::invalid_argument{"selected actor is absent or unavailable in the owned snapshot"};
        result.push_back(&*found);
    }
    for (const auto& actor : snapshot.actors()) {
        if (result.size() == 16) break;
        if (!eligible(actor) || (selected && selected(actor)) ||
            distance(snapshot.player_pose().position(), actor.position()) > hearing_distance) continue;
        result.push_back(&actor);
    }
    return result;
}

}  // namespace synth::targeting
