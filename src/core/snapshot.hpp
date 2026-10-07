#pragma once

#include "generation.hpp"
#include "runtime_identity.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace synth::core {

struct Vec3 final {
    double x{};
    double y{};
    double z{};

    [[nodiscard]] bool finite() const noexcept {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }

    auto operator<=>(const Vec3&) const = default;
};

class UnitVector3 final {
public:
    [[nodiscard]] static UnitVector3 from(Vec3 value) {
        if (!value.finite()) {
            throw std::invalid_argument{"direction must be finite"};
        }
        const auto magnitude = std::hypot(value.x, value.y, value.z);
        if (!std::isfinite(magnitude) || magnitude <= 0.0) {
            throw std::invalid_argument{"direction must be nonzero"};
        }
        return UnitVector3{{value.x / magnitude, value.y / magnitude, value.z / magnitude}};
    }

    [[nodiscard]] constexpr const Vec3& value() const noexcept { return value_; }

    auto operator<=>(const UnitVector3&) const = default;

private:
    explicit constexpr UnitVector3(Vec3 value) noexcept : value_{value} {}
    Vec3 value_;
};

class WorldPose final {
public:
    WorldPose(Vec3 position, UnitVector3 forward, UnitVector3 up)
        : position_{position}, forward_{forward}, up_{up} {
        if (!position.finite()) {
            throw std::invalid_argument{"position must be finite"};
        }
    }

    [[nodiscard]] const Vec3& position() const noexcept { return position_; }
    [[nodiscard]] const UnitVector3& forward() const noexcept { return forward_; }
    [[nodiscard]] const UnitVector3& up() const noexcept { return up_; }

private:
    Vec3 position_;
    UnitVector3 forward_;
    UnitVector3 up_;
};

using SnapshotClock = std::chrono::steady_clock;

class TimedPose final {
public:
    TimedPose(WorldPose pose,
              SnapshotClock::time_point captured_at,
              SnapshotClock::duration maximum_age,
              std::uint64_t frame)
        : pose_{std::move(pose)},
          captured_at_{captured_at},
          maximum_age_{maximum_age},
          frame_{frame} {
        if (maximum_age < SnapshotClock::duration::zero()) {
            throw std::invalid_argument{"pose maximum age cannot be negative"};
        }
    }

    [[nodiscard]] const WorldPose& pose() const noexcept { return pose_; }
    [[nodiscard]] SnapshotClock::time_point captured_at() const noexcept { return captured_at_; }
    [[nodiscard]] SnapshotClock::duration maximum_age() const noexcept { return maximum_age_; }
    [[nodiscard]] std::uint64_t frame() const noexcept { return frame_; }

    [[nodiscard]] bool fresh_at(SnapshotClock::time_point now) const noexcept {
        return now >= captured_at_ && now - captured_at_ <= maximum_age_;
    }

private:
    WorldPose pose_;
    SnapshotClock::time_point captured_at_;
    SnapshotClock::duration maximum_age_;
    std::uint64_t frame_;
};

enum class PoseCapability : unsigned char {
    unavailable,
    fresh,
    stale,
};

enum class RuntimeCapability : unsigned char {
    notifications,
    hmd_pose,
    controller_pose,
    semantic_input,
};

// Physical scene identity is copied on the game thread; display labels never grant scene ownership.
struct SceneIdentity final {
    std::uint32_t cell_form_id{}, worldspace_form_id{};
    std::string cell_origin_plugin, worldspace_origin_plugin;

    [[nodiscard]] bool valid(bool interior) const noexcept {
        return cell_form_id != 0 && !cell_origin_plugin.empty() &&
            (interior ? worldspace_form_id == 0 && worldspace_origin_plugin.empty()
                      : worldspace_form_id != 0 && !worldspace_origin_plugin.empty());
    }
    bool operator==(const SceneIdentity&) const = default;
};

struct WorldState final {
    std::string location;
    std::string cell;
    std::string worldspace;
    std::string weather;
    bool interior{};
    std::uint64_t game_time_ticks{};
    std::optional<SceneIdentity> scene;
};

struct LoadedPlugin final {
    std::string name;
    bool light{};
    std::uint16_t compile_index{};
    std::uint16_t small_file_compile_index{};
    std::uint16_t partial_index{};
    std::string form_id_prefix;
};

struct InventoryItemSnapshot final {
    std::uint32_t form_id{};
    std::string origin_plugin;
    std::string display_name;
    std::uint32_t count{};
    std::int32_t value{};
    double weight{};
    std::uint16_t form_type{};
    bool equipped{};

    [[nodiscard]] bool valid() const noexcept {
        return form_id != 0 && !origin_plugin.empty() && origin_plugin.size() <= 255 &&
               origin_plugin.find_first_of("/\\") == std::string::npos &&
               (origin_plugin.ends_with(".esm") || origin_plugin.ends_with(".esp") ||
                origin_plugin.ends_with(".esl")) &&
               !display_name.empty() && display_name.size() <= 255 && count > 0 &&
               std::isfinite(weight) && weight >= 0.0 && weight <= 100000.0;
    }
};

struct QuestSnapshot final {
    std::uint32_t form_id{};
    std::string origin_plugin;
    std::string display_name;
    std::string editor_id;
    std::uint16_t current_stage{};
    std::uint16_t active_objectives{};
    std::vector<std::string> objectives;
    std::string objectives_observation{"partial"};
    std::optional<bool> tracked;  // Unavailable is distinct from explicitly untracked.

    bool operator==(const QuestSnapshot&) const = default;

    [[nodiscard]] bool valid() const noexcept {
        return form_id != 0 && !origin_plugin.empty() && origin_plugin.size() <= 255 &&
               origin_plugin.find_first_of("/\\") == std::string::npos &&
               (origin_plugin.ends_with(".esm") || origin_plugin.ends_with(".esp") ||
                origin_plugin.ends_with(".esl")) &&
               !display_name.empty() && display_name.size() <= 255 && editor_id.size() <= 255 &&
               objectives.size() <= 8 &&
               (objectives_observation == "complete" || objectives_observation == "partial" ||
                objectives_observation == "unavailable") &&
               (objectives_observation != "unavailable" || (objectives.empty() && active_objectives == 0)) &&
               std::ranges::all_of(objectives, [](const auto& objective) {
                   return !objective.empty() && objective.size() <= 255;
               });
    }
};

struct NearbyItemSnapshot final {
    std::uint32_t reference_id{}, base_form_id{}, cell_form_id{};
    std::optional<std::string> reference_origin_plugin;
    std::string base_origin_plugin, cell_origin_plugin, display_name;
    Vec3 position;
    double distance{}, weight{};
    std::uint32_t count{1};
    std::int32_t value{};
    std::uint16_t form_type{};
    bool stealing{}, looking_at{}, held{};

    [[nodiscard]] bool valid() const noexcept {
        const auto valid_plugin = [](const std::string& plugin) {
            return !plugin.empty() && plugin.size() <= 255 &&
                   plugin.find_first_of("/\\") == std::string::npos &&
                   (plugin.ends_with(".esm") || plugin.ends_with(".esp") ||
                    plugin.ends_with(".esl"));
        };
        return reference_id != 0 && base_form_id != 0 && cell_form_id != 0 &&
               (!reference_origin_plugin || valid_plugin(*reference_origin_plugin)) &&
               valid_plugin(base_origin_plugin) && valid_plugin(cell_origin_plugin) &&
               !display_name.empty() && display_name.size() <= 255 && position.finite() &&
               std::isfinite(distance) && distance >= 0.0 && distance <= 1000000.0 &&
               count > 0 && std::isfinite(weight) && weight >= 0.0 && weight <= 100000.0;
    }
};

struct PointOfInterestSnapshot final {
    std::uint32_t reference_id{}, base_form_id{}, cell_form_id{};
    std::optional<std::string> reference_origin_plugin;
    std::string base_origin_plugin, cell_origin_plugin, display_name, kind;
    Vec3 position;
    double distance{};
    bool locked{}, looking_at{};

    [[nodiscard]] bool valid() const noexcept {
        const auto valid_plugin = [](const std::string& plugin) {
            return !plugin.empty() && plugin.size() <= 255 &&
                   plugin.find_first_of("/\\") == std::string::npos &&
                   (plugin.ends_with(".esm") || plugin.ends_with(".esp") ||
                    plugin.ends_with(".esl"));
        };
        static constexpr std::array kinds{
            std::string_view{"door"}, std::string_view{"container"},
            std::string_view{"activator"}, std::string_view{"flora"},
            std::string_view{"furniture"}, std::string_view{"terminal"},
            std::string_view{"map_marker"},
        };
        return reference_id != 0 && base_form_id != 0 && cell_form_id != 0 &&
               (!reference_origin_plugin || valid_plugin(*reference_origin_plugin)) &&
               valid_plugin(base_origin_plugin) && valid_plugin(cell_origin_plugin) &&
               !display_name.empty() && display_name.size() <= 255 &&
               std::ranges::find(kinds, kind) != kinds.end() && position.finite() &&
               std::isfinite(distance) && distance >= 0.0 && distance <= 1000000.0;
    }
};

struct FactionMembershipSnapshot final {
    std::uint32_t form_id{};
    std::string origin_plugin;
    std::string display_name;
    std::string editor_id;
    std::int16_t rank{};

    [[nodiscard]] bool valid() const noexcept {
        const auto valid_plugin = !origin_plugin.empty() && origin_plugin.size() <= 255 &&
                                  origin_plugin.find_first_of("/\\") == std::string::npos &&
                                  (origin_plugin.ends_with(".esm") ||
                                   origin_plugin.ends_with(".esp") ||
                                   origin_plugin.ends_with(".esl"));
        return form_id != 0 && valid_plugin && !display_name.empty() &&
               display_name.size() <= 255 && editor_id.size() <= 255 &&
               rank >= 0 && rank <= 127;
    }
};

struct PackageSnapshot final {
    std::uint32_t form_id{};
    std::string origin_plugin;
    std::string display_name;
    std::string editor_id;

    [[nodiscard]] bool valid() const noexcept {
        const auto valid_plugin = !origin_plugin.empty() && origin_plugin.size() <= 255 &&
                                  origin_plugin.find_first_of("/\\") == std::string::npos &&
                                  (origin_plugin.ends_with(".esm") ||
                                   origin_plugin.ends_with(".esp") ||
                                   origin_plugin.ends_with(".esl"));
        return form_id != 0 && valid_plugin && !display_name.empty() &&
               display_name.size() <= 255 && editor_id.size() <= 255;
    }
};

class ActorSnapshot final {
public:
    ActorSnapshot(std::uint32_t form_id,
                  std::string name,
                  Vec3 position,
                  std::string origin_plugin = "Fallout4.esm",
                  std::string playthrough_id = "unknown",
                  bool alive = true,
                  bool disabled = false,
                  bool in_combat = false,
                  bool hostile_to_player = false,
                  bool sneaking = false,
                  std::int16_t level = 1,
                  std::string race = {},
                  std::string sex = {},
                  std::string voice_type = {},
                  bool teammate = false,
                  double health_percent = 100.0,
                  double action_points_percent = 100.0,
                  std::vector<InventoryItemSnapshot> inventory = {},
                  std::optional<bool> line_of_sight = std::nullopt,
                  std::vector<FactionMembershipSnapshot> factions = {},
                  std::string faction_observation = "unavailable",
                  std::string life_state = "unknown",
                  std::string posture = "unknown",
                  bool weapon_drawn = false,
                  double movement_speed = 0.0,
                  bool sprinting = false,
                  bool talking_to_player = false,
                  bool in_power_armor = false,
                  std::optional<PackageSnapshot> current_package = std::nullopt,
                  std::uint32_t base_form_id = 0,
                  std::string base_origin_plugin = {},
                  std::string inventory_observation = "unavailable",
                  std::string faction_completeness = {},
                  bool health_percent_available = false,
                  bool action_points_percent_available = false,
                  bool conversation_area_available = false)
        : form_id_{form_id},
          name_{std::move(name)},
          position_{position},
          origin_plugin_{std::move(origin_plugin)},
          playthrough_id_{std::move(playthrough_id)},
          alive_{alive},
          disabled_{disabled},
          in_combat_{in_combat},
          hostile_to_player_{hostile_to_player},
          sneaking_{sneaking},
          level_{level},
          race_{std::move(race)},
          sex_{std::move(sex)},
          voice_type_{std::move(voice_type)},
          teammate_{teammate},
          health_percent_{health_percent},
          action_points_percent_{action_points_percent},
          inventory_{std::move(inventory)},
          line_of_sight_{line_of_sight},
          factions_{std::move(factions)},
          faction_observation_{std::move(faction_observation)},
          life_state_{std::move(life_state)},
          posture_{std::move(posture)},
          weapon_drawn_{weapon_drawn},
          movement_speed_{movement_speed},
          sprinting_{sprinting},
          talking_to_player_{talking_to_player},
          in_power_armor_{in_power_armor},
          current_package_{std::move(current_package)},
          base_form_id_{base_form_id == 0 ? form_id : base_form_id},
          base_origin_plugin_{base_origin_plugin.empty() ? origin_plugin_ : std::move(base_origin_plugin)},
          inventory_observation_{std::move(inventory_observation)},
          faction_completeness_{faction_completeness.empty()
              ? (faction_observation_ == "unavailable" ? "unavailable" : "partial")
              : std::move(faction_completeness)},
          health_percent_available_{health_percent_available},
          action_points_percent_available_{action_points_percent_available},
          conversation_area_available_{conversation_area_available} {
        const auto valid_plugin = !origin_plugin_.empty() && origin_plugin_.size() <= 255 &&
                                  origin_plugin_.find_first_of("/\\") == std::string::npos &&
                                  (origin_plugin_.ends_with(".esm") || origin_plugin_.ends_with(".esp") ||
                                   origin_plugin_.ends_with(".esl"));
        const auto valid_base_plugin = !base_origin_plugin_.empty() && base_origin_plugin_.size() <= 255 &&
                                       base_origin_plugin_.find_first_of("/\\") == std::string::npos &&
                                       (base_origin_plugin_.ends_with(".esm") ||
                                        base_origin_plugin_.ends_with(".esp") ||
                                        base_origin_plugin_.ends_with(".esl"));
        if (form_id == 0 || name_.empty() || name_.size() > 255 || !position_.finite() ||
            !valid_plugin || base_form_id_ == 0 || !valid_base_plugin ||
            playthrough_id_.empty() || playthrough_id_.size() > 128 || level_ < 1 ||
            race_.size() > 255 || sex_.size() > 16 || voice_type_.size() > 255 ||
            !std::isfinite(health_percent_) || health_percent_ < 0.0 || health_percent_ > 100.0 ||
            !std::isfinite(action_points_percent_) || action_points_percent_ < 0.0 ||
            action_points_percent_ > 100.0 || inventory_.size() > 512 ||
            !std::ranges::all_of(inventory_, &InventoryItemSnapshot::valid) ||
            (inventory_observation_ != "complete" && inventory_observation_ != "partial" &&
             inventory_observation_ != "unavailable") ||
            (inventory_observation_ == "unavailable" && !inventory_.empty()) ||
            factions_.size() > 32 ||
            !std::ranges::all_of(factions_, &FactionMembershipSnapshot::valid) ||
            (faction_observation_ != "effective" &&
             faction_observation_ != "base_only" &&
             faction_observation_ != "unavailable") ||
            (faction_completeness_ != "complete" && faction_completeness_ != "partial" &&
             faction_completeness_ != "unavailable") ||
            ((faction_observation_ == "unavailable") != (faction_completeness_ == "unavailable")) ||
            (faction_completeness_ == "unavailable" && !factions_.empty()) ||
            !valid_life_state(life_state_) || !valid_posture(posture_) ||
            !std::isfinite(movement_speed_) || movement_speed_ < 0.0 ||
            movement_speed_ > 100000.0 ||
            (current_package_ && !current_package_->valid())) {
            throw std::invalid_argument{"actor snapshot fields are invalid"};
        }
    }

    [[nodiscard]] std::uint32_t form_id() const noexcept { return form_id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const Vec3& position() const noexcept { return position_; }
    [[nodiscard]] const std::string& origin_plugin() const noexcept { return origin_plugin_; }
    [[nodiscard]] std::uint32_t base_form_id() const noexcept { return base_form_id_; }
    [[nodiscard]] const std::string& base_origin_plugin() const noexcept { return base_origin_plugin_; }
    [[nodiscard]] const std::string& playthrough_id() const noexcept { return playthrough_id_; }
    [[nodiscard]] bool alive() const noexcept { return alive_; }
    [[nodiscard]] bool disabled() const noexcept { return disabled_; }
    [[nodiscard]] bool in_combat() const noexcept { return in_combat_; }
    [[nodiscard]] bool hostile_to_player() const noexcept { return hostile_to_player_; }
    [[nodiscard]] bool sneaking() const noexcept { return sneaking_; }
    [[nodiscard]] std::int16_t level() const noexcept { return level_; }
    [[nodiscard]] const std::string& race() const noexcept { return race_; }
    [[nodiscard]] const std::string& sex() const noexcept { return sex_; }
    [[nodiscard]] const std::string& voice_type() const noexcept { return voice_type_; }
    [[nodiscard]] bool teammate() const noexcept { return teammate_; }
    [[nodiscard]] double health_percent() const noexcept { return health_percent_; }
    [[nodiscard]] double action_points_percent() const noexcept { return action_points_percent_; }
    [[nodiscard]] bool health_percent_available() const noexcept { return health_percent_available_; }
    [[nodiscard]] bool action_points_percent_available() const noexcept { return action_points_percent_available_; }
    [[nodiscard]] const std::vector<InventoryItemSnapshot>& inventory() const noexcept {
        return inventory_;
    }
    [[nodiscard]] const std::string& inventory_observation() const noexcept {
        return inventory_observation_;
    }
    [[nodiscard]] const std::optional<bool>& line_of_sight() const noexcept {
        return line_of_sight_;
    }
    [[nodiscard]] bool conversation_area_available() const noexcept { return conversation_area_available_; }
    [[nodiscard]] const std::vector<FactionMembershipSnapshot>& factions() const noexcept {
        return factions_;
    }
    [[nodiscard]] const std::string& faction_observation() const noexcept {
        return faction_observation_;
    }
    [[nodiscard]] const std::string& faction_completeness() const noexcept {
        return faction_completeness_;
    }
    [[nodiscard]] const std::string& life_state() const noexcept { return life_state_; }
    [[nodiscard]] const std::string& posture() const noexcept { return posture_; }
    [[nodiscard]] bool weapon_drawn() const noexcept { return weapon_drawn_; }
    [[nodiscard]] double movement_speed() const noexcept { return movement_speed_; }
    [[nodiscard]] bool sprinting() const noexcept { return sprinting_; }
    [[nodiscard]] bool talking_to_player() const noexcept { return talking_to_player_; }
    [[nodiscard]] bool in_power_armor() const noexcept { return in_power_armor_; }
    [[nodiscard]] const std::optional<PackageSnapshot>& current_package() const noexcept {
        return current_package_;
    }

    // Copy only validated inventory rows and quality; retain every original identity and non-inventory fact.
    [[nodiscard]] ActorSnapshot with_inventory(std::vector<InventoryItemSnapshot> items, std::string observation) const {
        if (items.size()>512 || !std::ranges::all_of(items,&InventoryItemSnapshot::valid) ||
            (observation!="complete" && observation!="partial" && observation!="unavailable") ||
            (observation=="unavailable" && !items.empty())) throw std::invalid_argument{"invalid inventory observation"};
        auto result=*this;
        result.inventory_=std::move(items);result.inventory_observation_=std::move(observation);
        return result;
    }

    // Inventory enrichment preserves every other fact and rejects a different actor/base/playthrough.
    [[nodiscard]] ActorSnapshot with_inventory_from(const ActorSnapshot& observed) const {
        if (form_id_ != observed.form_id_ || origin_plugin_ != observed.origin_plugin_ ||
            playthrough_id_ != observed.playthrough_id_ || base_form_id_ != observed.base_form_id_ ||
            base_origin_plugin_ != observed.base_origin_plugin_)
            throw std::invalid_argument{"actor detail identity changed"};
        auto result = *this;
        result.inventory_ = observed.inventory_;
        result.inventory_observation_ = observed.inventory_observation_;
        return result;
    }

    // Merge bounded deep evidence without changing the selection facts frozen earlier in this frame.
    [[nodiscard]] ActorSnapshot with_details_from(const ActorSnapshot& observed) const {
        auto result = with_inventory_from(observed);
        result.factions_ = observed.factions_;
        result.faction_observation_ = observed.faction_observation_;
        result.faction_completeness_ = observed.faction_completeness_;
        result.current_package_ = observed.current_package_;
        return result;
    }

private:
    [[nodiscard]] static bool valid_life_state(const std::string& value) noexcept {
        static constexpr std::array states{
            std::string_view{"alive"}, std::string_view{"dying"},
            std::string_view{"dead"}, std::string_view{"unconscious"},
            std::string_view{"reanimate"}, std::string_view{"recycle"},
            std::string_view{"restrained"}, std::string_view{"essential_down"},
            std::string_view{"bleedout"}, std::string_view{"unknown"},
        };
        return std::ranges::find(states, value) != states.end();
    }

    [[nodiscard]] static bool valid_posture(const std::string& value) noexcept {
        static constexpr std::array states{
            std::string_view{"normal"}, std::string_view{"want_to_sit"},
            std::string_view{"waiting_for_sit"}, std::string_view{"sitting"},
            std::string_view{"want_to_stand"}, std::string_view{"want_to_sleep"},
            std::string_view{"waiting_for_sleep"}, std::string_view{"sleeping"},
            std::string_view{"want_to_wake"}, std::string_view{"unknown"},
        };
        return std::ranges::find(states, value) != states.end();
    }

    std::uint32_t form_id_;
    std::string name_;
    Vec3 position_;
    std::string origin_plugin_;
    std::string playthrough_id_;
    bool alive_;
    bool disabled_;
    bool in_combat_;
    bool hostile_to_player_;
    bool sneaking_;
    std::int16_t level_;
    std::string race_;
    std::string sex_;
    std::string voice_type_;
    bool teammate_;
    double health_percent_;
    double action_points_percent_;
    std::vector<InventoryItemSnapshot> inventory_;
    std::optional<bool> line_of_sight_;
    std::vector<FactionMembershipSnapshot> factions_;
    std::string faction_observation_;
    std::string life_state_;
    std::string posture_;
    bool weapon_drawn_;
    double movement_speed_;
    bool sprinting_;
    bool talking_to_player_;
    bool in_power_armor_;
    std::optional<PackageSnapshot> current_package_;
    std::uint32_t base_form_id_;
    std::string base_origin_plugin_;
    std::string inventory_observation_;
    std::string faction_completeness_;
    bool health_percent_available_;
    bool action_points_percent_available_;
    bool conversation_area_available_;
};

class RuntimeSnapshot final {
public:
    RuntimeSnapshot(Game game,
                    RuntimeVariant variant,
                    RuntimeGeneration generation,
                    std::uint64_t frame,
                    SnapshotClock::time_point captured_at,
                    WorldPose player_pose,
                    std::optional<TimedPose> hmd_pose,
                    std::optional<TimedPose> left_controller_pose,
                    std::optional<TimedPose> right_controller_pose,
                    ActorSnapshot player,
                    std::vector<ActorSnapshot> actors,
                    std::uint64_t game_time_ticks = 0,
                    std::optional<WorldState> world = std::nullopt,
                    std::vector<LoadedPlugin> loaded_plugins = {},
                    std::vector<QuestSnapshot> active_quests = {},
                    std::vector<NearbyItemSnapshot> nearby_items = {},
                    std::vector<PointOfInterestSnapshot> points_of_interest = {},
                    std::string active_quests_observation = {},
                    std::string nearby_items_observation = {},
                    std::string points_of_interest_observation = {},
                    std::string actors_observation = {})
        : game_{game},
          variant_{variant},
          generation_{generation},
          frame_{frame},
          captured_at_{captured_at},
          player_pose_{std::move(player_pose)},
          hmd_pose_{std::move(hmd_pose)},
          left_controller_pose_{std::move(left_controller_pose)},
          right_controller_pose_{std::move(right_controller_pose)},
          player_{std::move(player)},
          actors_{std::move(actors)},
          game_time_ticks_{game_time_ticks},
          world_{std::move(world)},
          loaded_plugins_{std::move(loaded_plugins)},
          active_quests_{std::move(active_quests)},
          nearby_items_{std::move(nearby_items)},
          points_of_interest_{std::move(points_of_interest)},
          active_quests_observation_{active_quests_observation.empty()
              ? (active_quests_.empty() ? "unavailable" : "partial") : std::move(active_quests_observation)},
          nearby_items_observation_{nearby_items_observation.empty()
              ? (nearby_items_.empty() ? "unavailable" : "partial") : std::move(nearby_items_observation)},
          points_of_interest_observation_{points_of_interest_observation.empty()
              ? (points_of_interest_.empty() ? "unavailable" : "partial") : std::move(points_of_interest_observation)},
          actors_observation_{actors_observation.empty()
              ? (actors_.empty() ? "unavailable" : "partial") : std::move(actors_observation)} {
        if (!valid_collection_observation(actors_observation_, actors_.empty()) || actors_observation_ == "cached")
            throw std::invalid_argument{"snapshot actor observation is invalid"};
        if (!generation.valid()) {
            throw std::invalid_argument{"snapshot generation must be valid"};
        }
        if (variant == RuntimeVariant::flat && hmd_pose_) {
            throw std::invalid_argument{"flat runtime snapshot cannot contain an HMD pose"};
        }
        if (variant == RuntimeVariant::flat && (left_controller_pose_ || right_controller_pose_)) {
            throw std::invalid_argument{"flat runtime snapshot cannot contain controller poses"};
        }
        if (active_quests_.size() > 16 ||
            !std::ranges::all_of(active_quests_, &QuestSnapshot::valid) ||
            (active_quests_observation_ != "complete" && active_quests_observation_ != "partial" &&
             active_quests_observation_ != "cached" && active_quests_observation_ != "unavailable") ||
            (active_quests_observation_ == "unavailable" && !active_quests_.empty())) {
            throw std::invalid_argument{"snapshot active quests are invalid"};
        }
        if (nearby_items_.size() > 32 ||
            !std::ranges::all_of(nearby_items_, &NearbyItemSnapshot::valid) ||
            !valid_collection_observation(nearby_items_observation_, nearby_items_.empty())) {
            throw std::invalid_argument{"snapshot nearby items are invalid"};
        }
        if (points_of_interest_.size() > 16 ||
            !std::ranges::all_of(points_of_interest_, &PointOfInterestSnapshot::valid) ||
            !valid_collection_observation(points_of_interest_observation_, points_of_interest_.empty())) {
            throw std::invalid_argument{"snapshot points of interest are invalid"};
        }
    }

    [[nodiscard]] Game game() const noexcept { return game_; }
    [[nodiscard]] RuntimeVariant variant() const noexcept { return variant_; }
    [[nodiscard]] RuntimeGeneration generation() const noexcept { return generation_; }
    [[nodiscard]] std::uint64_t frame() const noexcept { return frame_; }
    [[nodiscard]] SnapshotClock::time_point captured_at() const noexcept { return captured_at_; }
    [[nodiscard]] const WorldPose& player_pose() const noexcept { return player_pose_; }
    [[nodiscard]] const std::optional<TimedPose>& hmd_pose() const noexcept { return hmd_pose_; }
    [[nodiscard]] const std::optional<TimedPose>& left_controller_pose() const noexcept {
        return left_controller_pose_;
    }
    [[nodiscard]] const std::optional<TimedPose>& right_controller_pose() const noexcept {
        return right_controller_pose_;
    }
    [[nodiscard]] const ActorSnapshot& player() const noexcept { return player_; }
    [[nodiscard]] const std::vector<ActorSnapshot>& actors() const noexcept { return actors_; }
    [[nodiscard]] const std::string& actors_observation() const noexcept { return actors_observation_; }
    // nullopt: unavailable; zero: observed without an eligible actor; otherwise exact captured form.
    [[nodiscard]] std::optional<std::uint32_t> picked_actor_form_id() const noexcept { return picked_actor_form_id_; }
    [[nodiscard]] std::uint64_t game_time_ticks() const noexcept { return game_time_ticks_; }
    [[nodiscard]] const std::optional<WorldState>& world() const noexcept { return world_; }
    [[nodiscard]] const std::vector<LoadedPlugin>& loaded_plugins() const noexcept {
        return loaded_plugins_;
    }
    [[nodiscard]] const std::vector<QuestSnapshot>& active_quests() const noexcept {
        return active_quests_;
    }
    [[nodiscard]] const std::string& active_quests_observation() const noexcept {
        return active_quests_observation_;
    }
    [[nodiscard]] const std::vector<NearbyItemSnapshot>& nearby_items() const noexcept {
        return nearby_items_;
    }
    [[nodiscard]] const std::string& nearby_items_observation() const noexcept { return nearby_items_observation_; }
    [[nodiscard]] const std::string& points_of_interest_observation() const noexcept { return points_of_interest_observation_; }

    // Event identities replace discovery once; this copied subset never claims a complete nearby audience.
    [[nodiscard]] RuntimeSnapshot with_event_actors(std::vector<ActorSnapshot> actors) const {
        if (variant_!=RuntimeVariant::flat || actors.size()>64) throw std::invalid_argument{"invalid actor event scene"};
        for (std::size_t i=0;i<actors.size();++i) {
            if (actors[i].form_id()==player_.form_id() || actors[i].playthrough_id()!=player_.playthrough_id())
                throw std::invalid_argument{"actor event scene owner mismatch"};
            for (std::size_t j=0;j<i;++j) if (actors[j].form_id()==actors[i].form_id())
                throw std::invalid_argument{"duplicate actor event identity"};
        }
        auto result=*this;
        result.actors_=std::move(actors);result.actors_observation_="partial";result.picked_actor_form_id_.reset();
        return result;
    }

    // Reserve an exact caller-named NPC without expanding the bounded scene or mutating its original owner.
    [[nodiscard]] RuntimeSnapshot with_requested_actor(const ActorSnapshot& observed) const {
        if (observed.form_id() == player_.form_id() || observed.playthrough_id() != player_.playthrough_id())
            throw std::invalid_argument{"requested actor has a different scene owner"};
        auto result = *this;
        const auto found = std::ranges::find_if(result.actors_, [&](const auto& actor) {
            return actor.form_id() == observed.form_id();
        });
        if (found != result.actors_.end()) {
            if (found->origin_plugin() != observed.origin_plugin() || found->playthrough_id() != observed.playthrough_id() ||
                found->base_form_id() != observed.base_form_id() || found->base_origin_plugin() != observed.base_origin_plugin())
                throw std::invalid_argument{"requested actor identity changed during capture"};
            result.actors_.erase(found);
        }
        else result.actors_observation_ = "partial";
        if (result.actors_.size() >= 64) {
            result.actors_observation_ = "partial";
            result.actors_.erase(result.actors_.begin() + 63, result.actors_.end());
        }
        result.actors_.insert(result.actors_.begin(), observed);
        return result;
    }

    // Native flat picking is independent of discovery, including targets outside its retained audience.
    [[nodiscard]] RuntimeSnapshot with_picked_actor(const std::optional<ActorSnapshot>& observed) const {
        if (variant_ != RuntimeVariant::flat) throw std::invalid_argument{"flat pick on a VR snapshot"};
        auto result = observed ? with_requested_actor(*observed) : *this;
        result.picked_actor_form_id_ = observed ? observed->form_id() : 0;
        return result;
    }

    // The runtime adapter must bind this inventory-only observation to the same player and capture frame.
    [[nodiscard]] RuntimeSnapshot with_player_inventory(const ActorSnapshot& observed) const {
        auto result = *this;
        result.player_ = player_.with_inventory_from(observed);
        return result;
    }

    // An action continuation may replace inventory only for one exact NPC already in its original scene.
    [[nodiscard]] RuntimeSnapshot with_actor_inventory(std::uint32_t form_id, const std::string& plugin,
        const std::string& playthrough, std::vector<InventoryItemSnapshot> items, std::string observation) const {
        auto result=*this;
        ActorSnapshot* found=nullptr;
        for (auto& actor:result.actors_) {
            if (actor.form_id()!=form_id || actor.origin_plugin()!=plugin || actor.playthrough_id()!=playthrough) continue;
            if (found) throw std::invalid_argument{"ambiguous inventory actor"};
            found=&actor;
        }
        if (!found || form_id==player_.form_id() || playthrough!=player_.playthrough_id())
            throw std::invalid_argument{"inventory actor is not in the original NPC scene"};
        *found=found->with_inventory(std::move(items),std::move(observation));
        return result;
    }

    // Only an existing NPC can be enriched; player/deep-load capture remains a separate guarded path.
    [[nodiscard]] RuntimeSnapshot with_actor_details(std::uint32_t selected_form_id, const ActorSnapshot& observed) const {
        auto result = *this;
        const auto found = std::ranges::find_if(result.actors_, [&](const auto& actor) {
            return actor.form_id() == selected_form_id;
        });
        if (found == result.actors_.end()) throw std::invalid_argument{"actor detail owner is absent"};
        *found = found->with_details_from(observed);
        return result;
    }
    [[nodiscard]] const std::vector<PointOfInterestSnapshot>& points_of_interest() const noexcept {
        return points_of_interest_;
    }

    [[nodiscard]] PoseCapability hmd_pose_capability(SnapshotClock::time_point now) const noexcept {
        if (!hmd_pose_) {
            return PoseCapability::unavailable;
        }
        return hmd_pose_->fresh_at(now) ? PoseCapability::fresh : PoseCapability::stale;
    }

    [[nodiscard]] const WorldPose* effective_listener_pose(
        SnapshotClock::time_point now) const noexcept {
        if (variant_ == RuntimeVariant::vr) {
            if (!hmd_pose_ || !hmd_pose_->fresh_at(now)) {
                return nullptr;
            }
            return &hmd_pose_->pose();
        }
        return &player_pose_;
    }

private:
    Game game_;
    RuntimeVariant variant_;
    RuntimeGeneration generation_;
    std::uint64_t frame_;
    SnapshotClock::time_point captured_at_;
    WorldPose player_pose_;
    std::optional<TimedPose> hmd_pose_;
    std::optional<TimedPose> left_controller_pose_;
    std::optional<TimedPose> right_controller_pose_;
    ActorSnapshot player_;
    std::vector<ActorSnapshot> actors_;
    std::uint64_t game_time_ticks_{};
    std::optional<WorldState> world_;
    std::vector<LoadedPlugin> loaded_plugins_;
    std::vector<QuestSnapshot> active_quests_;
    std::vector<NearbyItemSnapshot> nearby_items_;
    std::vector<PointOfInterestSnapshot> points_of_interest_;
    std::string active_quests_observation_;
    std::string nearby_items_observation_;
    std::string points_of_interest_observation_;
    std::string actors_observation_;
    std::optional<std::uint32_t> picked_actor_form_id_;

    // A missing collection is unknown unless its producer attests an observed empty list.
    static bool valid_collection_observation(std::string_view quality, bool empty) noexcept {
        return quality == "complete" || quality == "partial" || quality == "cached" ||
               (quality == "unavailable" && empty);
    }
};

}  // namespace synth::core
