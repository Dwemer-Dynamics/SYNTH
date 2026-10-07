#pragma once

#include "core/generation.hpp"
#include "core/snapshot.hpp"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace synth::context {

inline constexpr std::size_t maximum_plugin_name_bytes = 255;
inline constexpr std::size_t maximum_playthrough_id_bytes = 128;
inline constexpr std::size_t maximum_actor_name_bytes = 255;
inline constexpr std::size_t maximum_worldspace_name_bytes = 255;
inline constexpr std::size_t maximum_location_name_bytes = 255;

// Change detection uses observed labels and scope, not advancing game time or NPC selection.
struct WorldObservation final {
    core::RuntimeVariant variant;
    core::RuntimeGeneration generation;
    std::string playthrough, location, cell, worldspace, weather;
    bool interior{};
    bool operator==(const WorldObservation&) const = default;
};

[[nodiscard]] inline std::optional<WorldObservation> world_observation(const core::RuntimeSnapshot& snapshot) {
    if (!snapshot.world()) return std::nullopt;
    const auto& world = *snapshot.world();
    return WorldObservation{snapshot.variant(), snapshot.generation(), snapshot.player().playthrough_id(),
                            world.location, world.cell, world.worldspace, world.weather, world.interior};
}

[[nodiscard]] inline bool valid_plugin_name(std::string_view value) noexcept {
    return !value.empty() && value.size() <= maximum_plugin_name_bytes &&
           value.find_first_of("/\\") == std::string_view::npos &&
           (value.ends_with(".esm") || value.ends_with(".esp") || value.ends_with(".esl"));
}

struct QuestObservation final {
    core::RuntimeVariant variant;
    core::RuntimeGeneration generation;
    std::string playthrough, quality;
    std::vector<core::QuestSnapshot> quests;
    bool operator==(const QuestObservation&) const = default;
};

// Only fresh journal observations may cause passive publication; enumeration order is not a change.
[[nodiscard]] inline std::optional<QuestObservation> quest_observation(const core::RuntimeSnapshot& snapshot) {
    const auto& quality = snapshot.active_quests_observation();
    if (quality != "complete" && quality != "partial") return std::nullopt;
    QuestObservation result{snapshot.variant(), snapshot.generation(), snapshot.player().playthrough_id(),
                            quality, snapshot.active_quests()};
    std::ranges::sort(result.quests, [](const auto& left, const auto& right) {
        if (left.origin_plugin != right.origin_plugin) return left.origin_plugin < right.origin_plugin;
        return left.form_id < right.form_id;
    });
    return result;
}

// Both journals must describe the same live owner; unknown or cached evidence cannot imply progress.
[[nodiscard]] inline bool tracked_quest_changed(const QuestObservation& previous, const QuestObservation& current) {
    if (previous.variant != core::RuntimeVariant::flat || current.variant != previous.variant ||
        current.generation != previous.generation || current.playthrough != previous.playthrough ||
        (previous.quality != "complete" && previous.quality != "partial") ||
        (current.quality != "complete" && current.quality != "partial")) return false;
    const auto same_plugin = [](std::string_view a, std::string_view b) {
        return std::ranges::equal(a, b, [](unsigned char x, unsigned char y) {
            return (x >= 'A' && x <= 'Z' ? x + 32 : x) == (y >= 'A' && y <= 'Z' ? y + 32 : y);
        });
    };
    const auto complete = previous.quality == "complete" && std::ranges::all_of(previous.quests,
        [](const auto& quest) { return quest.tracked.has_value(); });
    for (const auto& quest : current.quests) {
        if (quest.tracked != true || quest.active_objectives == 0 ||
            (quest.objectives_observation != "complete" && quest.objectives_observation != "partial")) continue;
        const auto old = std::ranges::find_if(previous.quests, [&](const auto& candidate) {
            return candidate.form_id == quest.form_id && same_plugin(candidate.origin_plugin, quest.origin_plugin);
        });
        if (old == previous.quests.end()) { if (complete) return true; continue; }
        if (old->tracked == false) return true;
        if (old->tracked == true && old->objectives_observation == "complete" && quest.objectives_observation == "complete") {
            auto a = old->objectives, b = quest.objectives;
            std::ranges::sort(a); std::ranges::sort(b);
            if (a != b || old->active_objectives != quest.active_objectives) return true;
        }
    }
    return false;
}

class FormIdentity final {
public:
    FormIdentity(std::uint32_t form_id, std::string plugin)
        : form_id_{form_id}, plugin_{std::move(plugin)} {
        if (form_id_ == 0 || !valid_plugin_name(plugin_)) {
            throw std::invalid_argument{"form identity is invalid"};
        }
    }

    [[nodiscard]] std::uint32_t form_id() const noexcept { return form_id_; }
    [[nodiscard]] const std::string& plugin() const noexcept { return plugin_; }

    auto operator<=>(const FormIdentity&) const = default;

private:
    std::uint32_t form_id_;
    std::string plugin_;
};

class PlaythroughIdentity final {
public:
    explicit PlaythroughIdentity(std::string value) : value_{std::move(value)} {
        if (value_.empty() || value_.size() > maximum_playthrough_id_bytes) {
            throw std::invalid_argument{"playthrough identity is invalid"};
        }
    }

    [[nodiscard]] const std::string& value() const noexcept { return value_; }
    auto operator<=>(const PlaythroughIdentity&) const = default;

private:
    std::string value_;
};

class ActorIdentity final {
public:
    ActorIdentity(FormIdentity form, PlaythroughIdentity playthrough)
        : form_{std::move(form)}, playthrough_{std::move(playthrough)} {}

    [[nodiscard]] const FormIdentity& form() const noexcept { return form_; }
    [[nodiscard]] const PlaythroughIdentity& playthrough() const noexcept { return playthrough_; }

    auto operator<=>(const ActorIdentity&) const = default;

private:
    FormIdentity form_;
    PlaythroughIdentity playthrough_;
};

struct ActorIdentityHash final {
    [[nodiscard]] std::size_t operator()(const ActorIdentity& identity) const noexcept {
        auto seed = std::hash<std::string>{}(identity.form().plugin());
        seed ^= std::hash<std::uint32_t>{}(identity.form().form_id()) + 0x9e3779b9U + (seed << 6U) +
                (seed >> 2U);
        seed ^= std::hash<std::string>{}(identity.playthrough().value()) + 0x9e3779b9U +
                (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

class PlayerContext final {
public:
    PlayerContext(ActorIdentity identity,
                  std::string name,
                  core::Vec3 position,
                  core::RuntimeGeneration generation,
                  std::uint64_t snapshot_frame)
        : identity_{std::move(identity)},
          name_{std::move(name)},
          position_{position},
          generation_{generation},
          snapshot_frame_{snapshot_frame} {
        if (name_.empty() || name_.size() > maximum_actor_name_bytes || !position_.finite() ||
            !generation_.valid()) {
            throw std::invalid_argument{"player context is invalid"};
        }
    }

    [[nodiscard]] const ActorIdentity& identity() const noexcept { return identity_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const core::Vec3& position() const noexcept { return position_; }
    [[nodiscard]] core::RuntimeGeneration generation() const noexcept { return generation_; }
    [[nodiscard]] std::uint64_t snapshot_frame() const noexcept { return snapshot_frame_; }

private:
    ActorIdentity identity_;
    std::string name_;
    core::Vec3 position_;
    core::RuntimeGeneration generation_;
    std::uint64_t snapshot_frame_;
};

class WorldContext final {
public:
    WorldContext(std::string worldspace,
                 std::string location,
                 core::RuntimeGeneration generation,
                 std::uint64_t snapshot_frame)
        : worldspace_{std::move(worldspace)},
          location_{std::move(location)},
          generation_{generation},
          snapshot_frame_{snapshot_frame} {
        if (worldspace_.empty() || worldspace_.size() > maximum_worldspace_name_bytes ||
            location_.size() > maximum_location_name_bytes || !generation_.valid()) {
            throw std::invalid_argument{"world context is invalid"};
        }
    }

    [[nodiscard]] const std::string& worldspace() const noexcept { return worldspace_; }
    [[nodiscard]] const std::string& location() const noexcept { return location_; }
    [[nodiscard]] core::RuntimeGeneration generation() const noexcept { return generation_; }
    [[nodiscard]] std::uint64_t snapshot_frame() const noexcept { return snapshot_frame_; }

private:
    std::string worldspace_;
    std::string location_;
    core::RuntimeGeneration generation_;
    std::uint64_t snapshot_frame_;
};

class TargetContext final {
public:
    TargetContext(ActorIdentity identity,
                  std::string name,
                  core::Vec3 position,
                  double distance,
                  core::RuntimeGeneration generation,
                  std::uint64_t snapshot_frame)
        : identity_{std::move(identity)},
          name_{std::move(name)},
          position_{position},
          distance_{distance},
          generation_{generation},
          snapshot_frame_{snapshot_frame} {
        if (name_.empty() || name_.size() > maximum_actor_name_bytes || !position_.finite() ||
            !std::isfinite(distance_) || distance_ < 0.0 || !generation_.valid()) {
            throw std::invalid_argument{"target context is invalid"};
        }
    }

    [[nodiscard]] const ActorIdentity& identity() const noexcept { return identity_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const core::Vec3& position() const noexcept { return position_; }
    [[nodiscard]] double distance() const noexcept { return distance_; }
    [[nodiscard]] core::RuntimeGeneration generation() const noexcept { return generation_; }
    [[nodiscard]] std::uint64_t snapshot_frame() const noexcept { return snapshot_frame_; }

private:
    ActorIdentity identity_;
    std::string name_;
    core::Vec3 position_;
    double distance_;
    core::RuntimeGeneration generation_;
    std::uint64_t snapshot_frame_;
};

class AudienceContext final {
public:
    AudienceContext(std::size_t capacity, std::vector<TargetContext> members)
        : capacity_{capacity}, members_{std::move(members)} {
        if (capacity_ == 0 || members_.size() > capacity_) {
            throw std::invalid_argument{"audience exceeds its bound"};
        }
        std::ranges::sort(members_, [](const TargetContext& left, const TargetContext& right) {
            return left.identity() < right.identity();
        });
        for (std::size_t index = 1; index < members_.size(); ++index) {
            if (members_[index - 1].identity() == members_[index].identity()) {
                throw std::invalid_argument{"audience contains a duplicate actor"};
            }
        }
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] const std::vector<TargetContext>& members() const noexcept { return members_; }

private:
    std::size_t capacity_;
    std::vector<TargetContext> members_;
};

[[nodiscard]] inline ActorIdentity identity_of(const core::ActorSnapshot& actor) {
    return ActorIdentity{FormIdentity{actor.form_id(), actor.origin_plugin()},
                         PlaythroughIdentity{actor.playthrough_id()}};
}

class ActiveAgentRegistry final {
public:
    enum class RegisterResult : unsigned char { inserted, updated, rejected_stale };

    explicit ActiveAgentRegistry(std::size_t capacity) : capacity_{capacity} {
        if (capacity_ == 0) {
            throw std::invalid_argument{"agent registry capacity must be positive"};
        }
    }

    [[nodiscard]] RegisterResult register_agent(TargetContext agent,
                                                core::RuntimeGeneration current_generation,
                                                std::uint64_t current_frame) {
        if (agent.generation() != current_generation || agent.snapshot_frame() != current_frame) {
            return RegisterResult::rejected_stale;
        }
        std::erase_if(agents_, [current_generation](const auto& entry) {
            return entry.second.generation() != current_generation;
        });
        const auto found = agents_.find(agent.identity());
        if (found != agents_.end()) {
            found->second = std::move(agent);
            return RegisterResult::updated;
        }
        if (agents_.size() >= capacity_) {
            throw std::length_error{"active agent registry is full"};
        }
        const auto identity = agent.identity();
        agents_.emplace(identity, std::move(agent));
        return RegisterResult::inserted;
    }

    [[nodiscard]] bool erase(const ActorIdentity& identity) { return agents_.erase(identity) != 0; }

    [[nodiscard]] bool contains_current(const ActorIdentity& identity,
                                        core::RuntimeGeneration generation,
                                        std::uint64_t frame) const noexcept {
        const auto found = agents_.find(identity);
        return found != agents_.end() && found->second.generation() == generation &&
               found->second.snapshot_frame() == frame;
    }

    [[nodiscard]] std::vector<TargetContext> current(core::RuntimeGeneration generation,
                                                     std::uint64_t frame) const {
        std::vector<TargetContext> result;
        result.reserve(agents_.size());
        for (const auto& [identity, agent] : agents_) {
            (void)identity;
            if (agent.generation() == generation && agent.snapshot_frame() == frame) {
                result.push_back(agent);
            }
        }
        std::ranges::sort(result, [](const TargetContext& left, const TargetContext& right) {
            return left.identity() < right.identity();
        });
        return result;
    }

    [[nodiscard]] std::size_t size() const noexcept { return agents_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    std::size_t capacity_;
    std::unordered_map<ActorIdentity, TargetContext, ActorIdentityHash> agents_;
};

}  // namespace synth::context
