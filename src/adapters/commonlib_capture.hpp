#pragma once

#include "adapters/detail/runtime_base.hpp"
#include "core/actor_discovery_scan.hpp"
#include "core/player_event_sampler.hpp"

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>
#if defined(SYNTH_WITH_F4SEVR)
#include <spdlog/spdlog.h>
#else
#include "adapters/flat_picked_reference.hpp"
#include <RE/Q/QuestFlag.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace synth::adapters::commonlib {

inline constexpr std::size_t maximum_nearby_actors = 64;
#if defined(SYNTH_WITH_F4SEVR)
inline constexpr std::size_t maximum_inventory_items = 32;
inline constexpr std::size_t maximum_inventory_copies = 128;
inline constexpr std::size_t maximum_inventory_entries = 128;
#else
inline constexpr std::size_t maximum_inventory_items = 512;
inline constexpr std::size_t maximum_inventory_copies = 512;
inline constexpr std::size_t maximum_inventory_entries = 1024;
#endif
inline constexpr std::size_t maximum_cell_references = 512;
inline constexpr std::size_t maximum_nearby_items = 32;
inline constexpr std::size_t maximum_points_of_interest = 16;
inline constexpr std::size_t maximum_factions_per_actor = 32;
inline constexpr std::size_t maximum_faction_source_entries = 512;
inline constexpr std::size_t maximum_extra_data_entries = 512;

class TrySpinLock final {
public:
    explicit TrySpinLock(RE::BSSpinLock& lock) noexcept : lock_{lock} {
#if defined(SYNTH_WITH_F4SEVR)
        owns_ = lock_.try_lock();
#else
        // Pinned CommonLibF4 maps BSSpinLock::try_lock to the RW write-lock routine
        // (ID 2267902), leaving 0x80000000 after spin unlock. Match the verified
        // flat engine's owner/count ABI with a single nonblocking 0 -> 1 attempt.
        static_assert(sizeof(RE::BSSpinLock) == 8 && alignof(RE::BSSpinLock) >= 4);
        auto* bytes = reinterpret_cast<std::byte*>(&lock_);
        auto& owner = *reinterpret_cast<std::uint32_t*>(bytes);
        auto& count = *reinterpret_cast<volatile std::uint32_t*>(bytes + 4);
        REX::TAtomicRef owner_ref{owner};
        REX::TAtomicRef count_ref{count};
        const auto thread = REX::W32::GetCurrentThreadId();
        if (owner_ref == thread) {
            ++count_ref;
            owns_ = true;
        } else {
            std::uint32_t expected{};
            owns_ = count_ref.compare_exchange_strong(expected, 1);
            if (owns_) owner_ref = thread;
        }
#endif
    }
    ~TrySpinLock() {
        if (owns_) lock_.unlock();
    }

    TrySpinLock(const TrySpinLock&) = delete;
    TrySpinLock& operator=(const TrySpinLock&) = delete;

    [[nodiscard]] bool owns_lock() const noexcept { return owns_; }

private:
    RE::BSSpinLock& lock_;
    bool owns_{};
};

class TryReadLock final {
public:
    explicit TryReadLock(RE::BSReadWriteLock& lock) noexcept
        : lock_{lock} {
#if defined(SYNTH_WITH_F4SEVR)
        owns_ = lock_.try_lock_read();
#else
        // The exact flat try_lock_read has an unbounded CAS retry under reader contention.
        // Match its owner/count ABI, but make only one attempt and defer on any contention.
        static_assert(sizeof(RE::BSReadWriteLock) == 8 && alignof(RE::BSReadWriteLock) >= 4);
        auto* bytes = reinterpret_cast<std::byte*>(&lock_);
        auto& owner = *reinterpret_cast<std::uint32_t*>(bytes);
        auto& count = *reinterpret_cast<volatile std::uint32_t*>(bytes + 4);
        REX::TAtomicRef owner_ref{owner};
        REX::TAtomicRef count_ref{count};
        auto expected = static_cast<std::uint32_t>(count_ref);
        const auto thread = REX::W32::GetCurrentThreadId();
        if ((expected & 0xF0000000U) != 0 && (expected & 0xF0000000U) != 0x80000000U) return;
        if ((expected & 0xF0000000U) != 0 && owner_ref != thread) return;
        if ((expected & 0x0FFFFFFFU) == 0x0FFFFFFFU) return;
        owns_ = count_ref.compare_exchange_strong(expected, expected + 1);
#endif
    }
    ~TryReadLock() {
        if (owns_) lock_.unlock_read();
    }

    TryReadLock(const TryReadLock&) = delete;
    TryReadLock& operator=(const TryReadLock&) = delete;

    [[nodiscard]] bool owns_lock() const noexcept { return owns_; }

private:
    RE::BSReadWriteLock& lock_;
    bool owns_{};
};

[[nodiscard]] inline bool menu_mode_active() noexcept {
    const auto* ui = RE::UI::GetSingleton();
    return ui != nullptr && ui->menuMode != 0;
}

[[nodiscard]] inline bool player_in_combat() noexcept {
    const auto* player = RE::PlayerCharacter::GetSingleton();
    return player != nullptr && player->IsInCombat();
}

// Deep context capture is safe only after the loaded save has published its world objects.
[[nodiscard]] inline bool world_ready_for_capture() noexcept {
    const auto* player = RE::PlayerCharacter::GetSingleton();
    const auto* camera = RE::PlayerCamera::GetSingleton();
    const auto* manager = RE::BGSSaveLoadManager::GetSingleton();
    return player != nullptr && player->GetParentCell() != nullptr && player->Get3D() != nullptr &&
           camera != nullptr && camera->cameraRoot != nullptr && manager != nullptr &&
           manager->currentPlayerID != 0;
}

[[nodiscard]] inline std::string bounded_label(const char* value) {
    if (value == nullptr) return {};
    std::string result{value};
    if (result.size() > 255) result.resize(255);
    result.erase(std::remove_if(result.begin(), result.end(), [](unsigned char character) {
                     return character < 0x20 && character != '\t';
                 }),
                 result.end());
    return result;
}

[[nodiscard]] inline std::string form_fallback(const RE::TESForm& form) {
    auto label = bounded_label(form.GetFormEditorID());
    if (!label.empty()) return label;
    std::ostringstream value;
    value << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
          << form.GetFormID();
    return value.str();
}

// A missing definition or invalid denominator is unavailable, not a full actor value.
[[nodiscard]] inline std::optional<double> actor_value_percent(const RE::Actor& actor,
                                                const RE::ActorValueInfo* value) {
    if (value == nullptr) return std::nullopt;
    const auto current = static_cast<double>(actor.GetActorValue(*value));
    const auto maximum = static_cast<double>(actor.GetPermanentActorValue(*value));
    if (!std::isfinite(current) || !std::isfinite(maximum) || maximum <= 0.0) return std::nullopt;
    return std::clamp((current / maximum) * 100.0, 0.0, 100.0);
}

[[nodiscard]] inline std::uint64_t game_time_ticks() noexcept {
    const auto* calendar = RE::Calendar::GetSingleton();
    if (calendar == nullptr) return 0;
    const auto hours = static_cast<double>(calendar->GetHoursPassed());
    if (!std::isfinite(hours) || hours <= 0.0) return 0;
    return static_cast<std::uint64_t>(std::llround(hours * (10'000'000.0 / 24.0)));
}

[[nodiscard]] inline std::optional<core::WorldState> world_state(
    RE::PlayerCharacter& player, std::uint64_t ticks) {
    auto* cell = player.GetParentCell();
    if (cell == nullptr || ticks == 0) return std::nullopt;
    const auto interior = cell->IsInterior();
    auto* current_location = player.GetCurrentLocation();
    auto location = current_location == nullptr
                        ? std::string{}
                        : bounded_label(current_location->GetFullName());
    auto cell_name = bounded_label(cell->GetFullName());
    std::string worldspace;
    if (!interior && cell->worldSpace != nullptr) {
        worldspace = bounded_label(cell->worldSpace->GetFullName());
        if (worldspace.empty()) worldspace = form_fallback(*cell->worldSpace);
    }
    if (cell_name.empty()) cell_name = form_fallback(*cell);
    if (location.empty()) location = !worldspace.empty() ? worldspace : cell_name;
    std::string weather;
    if (!interior) {
        if (const auto* sky = RE::Sky::GetSingleton(); sky != nullptr && sky->currentWeather != nullptr) {
            weather = form_fallback(*sky->currentWeather);
        }
    }
    const auto* cell_file = cell->GetFile(0);
    const auto* space = !interior ? cell->worldSpace : nullptr;
    const auto* space_file = space ? space->GetFile(0) : nullptr;
    core::SceneIdentity scene{cell->GetFormID(), space ? space->GetFormID() : 0,
        cell_file ? std::string{cell_file->GetFilename()} : std::string{},
        space_file ? std::string{space_file->GetFilename()} : std::string{}};
    return core::WorldState{std::move(location), std::move(cell_name), std::move(worldspace),
                            std::move(weather), interior, ticks,
                            scene.valid(interior) ? std::optional{std::move(scene)} : std::nullopt};
}

[[nodiscard]] inline std::vector<core::LoadedPlugin> loaded_plugins() {
    auto* handler = RE::TESDataHandler::GetSingleton();
    if (handler == nullptr) return {};
#ifdef SYNTH_WITH_F4SEVR
    const auto* collection = handler->GetCompiledFileCollection();
#else
    const auto* collection = &handler->compiledFileCollection;
#endif
    if (collection == nullptr) return {};
    std::vector<core::LoadedPlugin> result;
    result.reserve(std::min<std::size_t>(
        collection->files.size() + collection->smallFiles.size(), 512));
    const auto append = [&](const auto& files, bool light) {
        for (const auto* file : files) {
            if (file == nullptr || result.size() == 512) break;
            auto name = bounded_label(file->GetFilename().data());
            if (name.empty() || name.find_first_of("/\\") != std::string::npos) continue;
            const auto compile_index = static_cast<std::uint16_t>(file->GetCompileIndex());
            const auto small_index = file->GetSmallFileCompileIndex();
            std::ostringstream prefix;
            prefix << std::uppercase << std::hex << std::setfill('0');
            if (light) {
                prefix << "FE" << std::setw(3) << (small_index & 0x0FFF);
            } else {
                prefix << std::setw(2) << (compile_index & 0x00FF);
            }
            result.push_back(core::LoadedPlugin{
                std::move(name), light, compile_index, small_index,
                light ? static_cast<std::uint16_t>(small_index & 0x0FFF) : compile_index,
                prefix.str()});
        }
    };
    append(collection->files, false);
    append(collection->smallFiles, true);
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        if (left.light != right.light) return !left.light;
        if (left.partial_index != right.partial_index) return left.partial_index < right.partial_index;
        return left.name < right.name;
    });
    return result;
}

[[nodiscard]] inline core::Vec3 point(const RE::NiPoint3& value) noexcept {
    return {value.x, value.y, value.z};
}

[[nodiscard]] inline core::WorldPose pose(const RE::NiTransform& transform) {
    const auto forward = transform.rotate * RE::NiPoint3{0.0F, 1.0F, 0.0F};
    const auto up = transform.rotate * RE::NiPoint3{0.0F, 0.0F, 1.0F};
    return {point(transform.translate), core::UnitVector3::from(point(forward)),
            core::UnitVector3::from(point(up))};
}

[[nodiscard]] inline core::WorldPose actor_pose(const RE::Actor& actor) {
    const auto position = actor.GetPosition();
    const auto yaw = static_cast<double>(actor.data.angle.z);
    return {point(position), core::UnitVector3::from({std::sin(yaw), std::cos(yaw), 0.0}),
            core::UnitVector3::from({0.0, 0.0, 1.0})};
}

[[nodiscard]] inline std::string playthrough_id() {
    const auto* manager = RE::BGSSaveLoadManager::GetSingleton();
    if (manager == nullptr || manager->currentPlayerID == 0) {
        throw std::runtime_error{"Fallout 4 has not published a stable player/save identity yet"};
    }
    std::ostringstream value;
    value << "fo4-player-" << std::hex << std::setw(16) << std::setfill('0')
          << manager->currentPlayerID;
    return value.str();
}

[[nodiscard]] inline std::string origin_plugin(const RE::TESObjectREFR& reference) {
    const auto* file = reference.GetFile(0);
    if (file == nullptr) {
        const auto* base = reference.GetObjectReference();
        file = base == nullptr ? nullptr : base->GetFile(0);
    }
    return file == nullptr ? std::string{} : std::string{file->GetFilename()};
}

[[nodiscard]] inline std::string origin_plugin(const RE::TESForm& form) {
    const auto* file = form.GetFile(0);
    return file == nullptr ? std::string{} : std::string{file->GetFilename()};
}

// One guarded scalar read; do not add discovery, inventory, quests, camera or deep actor capture here.
[[nodiscard]] inline std::optional<core::PlayerEventSample> player_event_sample(
    const runtime::IFalloutRuntime& runtime, core::SnapshotClock::time_point now) {
    runtime.assert_game_thread();
    const auto generation = runtime.generation();
    auto* player = RE::PlayerCharacter::GetSingleton();
    const auto* manager = RE::BGSSaveLoadManager::GetSingleton();
    if (player == nullptr || player->GetFormID() != 0x14 || manager == nullptr ||
        manager->currentPlayerID == 0 || player->IsDead(false) || player->IsDisabled()) return {};
    const auto level = player->GetLevel();
    const auto ticks = game_time_ticks();
    if (level < 1 || ticks == 0) return {};
    auto plugin = origin_plugin(*player);
    if (plugin.empty()) plugin = "Fallout4.esm";
    core::PlayerEventSample sample{generation, runtime.variant(), player->GetFormID(), std::move(plugin),
        playthrough_id(), now, ticks, level, player->IsInCombat()};
    return runtime.generation() == generation ? std::optional{std::move(sample)} : std::nullopt;
}

struct FactionCapture {
    std::vector<core::FactionMembershipSnapshot> factions;
    std::string observation{"unavailable"};
    std::string completeness{"unavailable"};
};

// Copy ranks without waiting on actor extra data or repeatedly querying live membership.
[[nodiscard]] inline FactionCapture
faction_memberships(const RE::Actor& actor, const RE::TESNPC* npc) {
    if (npc == nullptr) return {{}, "unavailable"};
    if (npc->factions.size() > maximum_faction_source_entries) return {{}, "unavailable"};

    std::vector<std::pair<RE::TESFaction*, std::int16_t>> changes_copy;
#if !defined(SYNTH_WITH_F4SEVR)
    if (actor.extraList != nullptr) {
        // GetByType takes a blocking lock and releases it before callers read its result.
        // Hold one nonblocking read lease while copying; resolve names only after release.
        const TryReadLock lock{actor.extraList->extraRWLock};
        if (!lock.owns_lock()) return {{}, "unavailable"};
        if (actor.extraList->extraData.HasType(RE::ExtraFactionChanges::TYPE)) {
            // The pinned flat BaseExtraList is standard-layout: head, tail, flags.
            // Copy its private first pointer without aliasing or importing a VR layout.
            static_assert(std::is_standard_layout_v<RE::BaseExtraList> &&
                          std::is_trivially_copyable_v<RE::BaseExtraList>);
            static_assert(sizeof(RE::BaseExtraList) == 3 * sizeof(void*));
            RE::BSExtraData* entry{};
            std::memcpy(&entry, &actor.extraList->extraData, sizeof(entry));
            const RE::ExtraFactionChanges* changes{};
            for (std::size_t inspected = 0; entry != nullptr && inspected < maximum_extra_data_entries;
                 entry = entry->next, ++inspected) {
                if (entry->GetExtraType() == RE::ExtraFactionChanges::TYPE) {
                    changes = static_cast<const RE::ExtraFactionChanges*>(entry);
                    break;
                }
            }
            // A flagged-but-missing record or a capped/cyclic chain cannot attest membership.
            if (changes == nullptr) return {{}, "unavailable"};
            if (changes->factionChanges.size() > maximum_faction_source_entries) return {{}, "unavailable"};
            changes_copy.reserve(changes->factionChanges.size());
            for (const auto& membership : changes->factionChanges) {
                changes_copy.emplace_back(membership.faction, static_cast<std::int16_t>(membership.rank));
            }
        }
    }
#else
    (void)actor;
#endif

    std::unordered_map<std::uint32_t, core::FactionMembershipSnapshot> memberships;
    bool complete = true;
    const auto assign = [&](RE::TESFaction* faction, std::int16_t rank) {
        if (faction == nullptr) { complete = false; return; }
        if (rank < 0) {
            memberships.erase(faction->GetFormID());
            return;
        }
        auto plugin = origin_plugin(*faction);
        if (plugin.empty()) { complete = false; return; }
        auto name = bounded_label(faction->GetFullName());
        if (name.empty()) name = form_fallback(*faction);
        memberships.insert_or_assign(
            faction->GetFormID(),
            core::FactionMembershipSnapshot{
                faction->GetFormID(), std::move(plugin), std::move(name),
                bounded_label(faction->GetFormEditorID()), rank});
    };

    for (const auto& membership : npc->factions) {
        assign(membership.faction, static_cast<std::int16_t>(membership.rank));
    }
    // Runtime rank changes override the base record, including negative-rank removals.
    for (const auto& [faction, rank] : changes_copy) {
        assign(faction, rank);
    }

    std::vector<core::FactionMembershipSnapshot> result;
    result.reserve(std::min(memberships.size(), maximum_factions_per_actor));
    for (auto& [form_id, membership] : memberships) {
        (void)form_id;
        result.push_back(std::move(membership));
    }
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        if (left.rank != right.rank) return left.rank > right.rank;
        if (left.origin_plugin != right.origin_plugin) {
            return left.origin_plugin < right.origin_plugin;
        }
        return left.form_id < right.form_id;
    });
    if (result.size() > maximum_factions_per_actor) {
        result.resize(maximum_factions_per_actor);
        complete = false;
    }
#if defined(SYNTH_WITH_F4SEVR)
    return {std::move(result), "base_only", complete ? "complete" : "partial"};
#else
    return {std::move(result), "effective", complete ? "complete" : "partial"};
#endif
}

[[nodiscard]] inline std::string life_state(const RE::Actor& actor) {
#if defined(SYNTH_WITH_F4SEVR)
    switch (static_cast<RE::LIFE_STATE>(actor.lifeState)) {
        case RE::LIFE_STATE::kAlive: return "alive";
        case RE::LIFE_STATE::kDying: return "dying";
        case RE::LIFE_STATE::kDead: return "dead";
        case RE::LIFE_STATE::kUnconscious: return "unconscious";
        case RE::LIFE_STATE::kReanimate: return "reanimate";
        case RE::LIFE_STATE::kRecycle: return "recycle";
        case RE::LIFE_STATE::kRestrained: return "restrained";
        case RE::LIFE_STATE::kEssentialDown: return "essential_down";
        case RE::LIFE_STATE::kBleedout: return "bleedout";
    }
#else
    switch (static_cast<RE::ACTOR_LIFE_STATE>(actor.lifeState)) {
        case RE::ACTOR_LIFE_STATE::kAlive: return "alive";
        case RE::ACTOR_LIFE_STATE::kDying: return "dying";
        case RE::ACTOR_LIFE_STATE::kDead: return "dead";
        case RE::ACTOR_LIFE_STATE::kUnconscious: return "unconscious";
        case RE::ACTOR_LIFE_STATE::kReanimate: return "reanimate";
        case RE::ACTOR_LIFE_STATE::kRecycle: return "recycle";
        case RE::ACTOR_LIFE_STATE::kRestrained: return "restrained";
        case RE::ACTOR_LIFE_STATE::kEssentialDown: return "essential_down";
        case RE::ACTOR_LIFE_STATE::kBleedout: return "bleedout";
    }
#endif
    return "unknown";
}

[[nodiscard]] inline std::string posture(const RE::Actor& actor) {
    switch (actor.DoGetSitSleepState()) {
        case RE::SIT_SLEEP_STATE::kNormal: return "normal";
        case RE::SIT_SLEEP_STATE::kWantToSit: return "want_to_sit";
        case RE::SIT_SLEEP_STATE::kWaitingForSitAnim: return "waiting_for_sit";
        case RE::SIT_SLEEP_STATE::kIsSitting: return "sitting";
        case RE::SIT_SLEEP_STATE::kWantToStand: return "want_to_stand";
        case RE::SIT_SLEEP_STATE::kWantToSleep: return "want_to_sleep";
        case RE::SIT_SLEEP_STATE::kWaitingForSleepAnim: return "waiting_for_sleep";
        case RE::SIT_SLEEP_STATE::kIsSleeping: return "sleeping";
        case RE::SIT_SLEEP_STATE::kWantToWake: return "want_to_wake";
    }
    return "unknown";
}

#if !defined(SYNTH_WITH_F4SEVR)
// Pin under the owning ActorPackage lock; metadata and final native deletion run after unlock.
class FlatPackageReadLease final {
public:
    FlatPackageReadLease() = default;
    ~FlatPackageReadLease() {
        if (package_ == nullptr) return;
        REX::TAtomicRef count{package_->refCount};
        // Flat 1.11.240 releases +0xC4 and deletes only runtime-created packages (flag 0x800).
        // IsCreated() tests the form ID instead and is not the native package deletion rule.
        if (--count == 0 && (package_->data.packFlags & 0x800U) != 0) delete package_;
    }
    FlatPackageReadLease(const FlatPackageReadLease&) = delete;
    FlatPackageReadLease& operator=(const FlatPackageReadLease&) = delete;

    [[nodiscard]] bool try_acquire(RE::TESPackage* package) noexcept {
        static_assert(offsetof(RE::TESPackage, refCount) == 0xC4);
        static_assert(offsetof(RE::TESPackage, data) == 0x20);
        if (package == nullptr || package_ != nullptr) return false;
        REX::TAtomicRef count{package->refCount};
        auto expected = static_cast<std::uint32_t>(count);
        if (expected == 0 || expected == std::numeric_limits<std::uint32_t>::max()) return false;
        if (!count.compare_exchange_strong(expected, expected + 1)) return false;
        package_ = package;
        return true;
    }

private:
    RE::TESPackage* package_{};
};
#endif

[[nodiscard]] inline std::optional<core::PackageSnapshot> current_package(RE::Actor& actor) {
    auto* process = actor.currentProcess;
    if (process == nullptr) return std::nullopt;
    RE::TESPackage* package{};
#if !defined(SYNTH_WITH_F4SEVR)
    FlatPackageReadLease lease;
#endif
    {
#if !defined(SYNTH_WITH_F4SEVR)
        // Flat's running-package selector gives a non-null run-once package precedence.
        // Keep that observation locked through fallback; a busy override is unknown, not absent.
        std::optional<TrySpinLock> run_once_lock;
        if (process->middleHigh != nullptr) {
            auto& run_once = process->middleHigh->runOncePackage;
            run_once_lock.emplace(run_once.packageLock);
            if (!run_once_lock->owns_lock()) return std::nullopt;
            package = run_once.package;
        }
#endif
        std::optional<TrySpinLock> current_lock;
        if (package == nullptr) {
            current_lock.emplace(process->currentPackage.packageLock);
            if (!current_lock->owns_lock()) return std::nullopt;
            package = process->currentPackage.package;
        }
#if !defined(SYNTH_WITH_F4SEVR)
        if (!lease.try_acquire(package)) return std::nullopt;
#endif
    }
    if (package == nullptr) return std::nullopt;
    auto plugin = origin_plugin(*package);
    if (plugin.empty()) return std::nullopt;
    auto editor = bounded_label(package->GetFormEditorID());
    auto name = editor;
    if (name.empty()) name = form_fallback(*package);
    return core::PackageSnapshot{
        package->GetFormID(), std::move(plugin), std::move(name), std::move(editor)};
}

#if !defined(SYNTH_WITH_F4SEVR)
// Read an already-owned flat string without the engine's unbounded shallow-entry walk or pool mutation.
template <class String>
[[nodiscard]] inline std::optional<std::string> inventory_label(const String& text) {
    static_assert(std::is_standard_layout_v<String> && sizeof(String) == sizeof(void*));
    const RE::BSStringPool::Entry* entry{};
    std::memcpy(&entry, &text, sizeof(entry));
    for (std::size_t inspected = 0; entry != nullptr && inspected < 8; ++inspected) {
        if (entry->shallow()) {
            entry = entry->_right;
            if (entry == nullptr) return std::nullopt;
            continue;
        }
        if (entry->wide()) return std::nullopt;
        std::array<char, 256> copy{};
        std::copy_n(reinterpret_cast<const char*>(entry + 1),
                    std::min<std::size_t>(entry->_length, copy.size() - 1), copy.data());
        return bounded_label(copy.data());
    }
    if (entry != nullptr) return std::nullopt;
    return std::string{};
}

struct InventoryMetadata final {
    std::string name;
    RE::BSTSmartPointer<RE::TBO_InstanceData> instance;
    std::uint32_t reference_count{1};
    std::optional<std::uint32_t> reference_loaded_ammo;
};

// Both native item getters take blocking extra-data locks; copy cached names and pin instances instead.
[[nodiscard]] inline std::optional<InventoryMetadata> inventory_metadata(
    RE::TESBoundObject& object, const RE::BSTSmartPointer<RE::ExtraDataList>& extra,
    std::uintptr_t count_vtable = 0, std::uintptr_t ammo_vtable = 0) {
    InventoryMetadata metadata;
    if (ammo_vtable != 0) metadata.reference_loaded_ammo = 0;
    if (extra != nullptr) {
        const TryReadLock lock{extra->extraRWLock};
        if (!lock.owns_lock()) return std::nullopt;
        static_assert(std::is_standard_layout_v<RE::BaseExtraList> &&
                      std::is_trivially_copyable_v<RE::BaseExtraList>);
        static_assert(sizeof(RE::BaseExtraList) == 3 * sizeof(void*));
        RE::BSExtraData* entry{};
        std::memcpy(&entry, &extra->extraData, sizeof(entry));
        const RE::ExtraTextDisplayData* text{};
        const RE::ExtraInstanceData* instance{};
        const RE::BSExtraData* count{};
        const RE::BSExtraData* ammo{};
        for (std::size_t inspected = 0; entry != nullptr && inspected < maximum_extra_data_entries;
             entry = entry->next, ++inspected) {
            if (entry->GetExtraType() == RE::ExtraTextDisplayData::TYPE) {
                if (text != nullptr) return std::nullopt;
                text = static_cast<const RE::ExtraTextDisplayData*>(entry);
            } else if (entry->GetExtraType() == RE::ExtraInstanceData::TYPE) {
                if (instance != nullptr) return std::nullopt;
                instance = static_cast<const RE::ExtraInstanceData*>(entry);
            } else if (count_vtable != 0 && entry->GetExtraType() == RE::EXTRA_DATA_TYPE::kCount) {
                if (count != nullptr) return std::nullopt;
                count = entry;
            } else if (ammo_vtable != 0 && entry->GetExtraType() == RE::EXTRA_DATA_TYPE::kAmmo) {
                if (ammo != nullptr) return std::nullopt;
                ammo = entry;
            }
        }
        if (entry != nullptr) return std::nullopt;
        if (extra->extraData.HasType(RE::ExtraTextDisplayData::TYPE) != (text != nullptr) ||
            extra->extraData.HasType(RE::ExtraInstanceData::TYPE) != (instance != nullptr)) return std::nullopt;
        if (count_vtable != 0) {
            if (extra->extraData.HasType(RE::EXTRA_DATA_TYPE::kCount) != (count != nullptr)) return std::nullopt;
            if (count != nullptr) {
                // Flat 1.11.240 count getter reads unsigned WORD at +0x18; an absent count means one.
                // Validate the native type before copying under the same read lease. See WORLD-ITEM-CAPTURE.md.
                static_assert(sizeof(RE::BSExtraData) == 0x18);
                std::uintptr_t observed_vtable{};
                std::memcpy(&observed_vtable, count, sizeof(observed_vtable));
                if (observed_vtable != count_vtable) return std::nullopt;
                std::uint16_t value{};
                std::memcpy(&value, reinterpret_cast<const std::byte*>(count) + 0x18, sizeof(value));
                if (value == 0) return std::nullopt;
                metadata.reference_count = value;
            }
        }
        if (ammo_vtable != 0) {
            if (extra->extraData.HasType(RE::EXTRA_DATA_TYPE::kAmmo) != (ammo != nullptr)) return std::nullopt;
            if (ammo != nullptr) {
                // Flat 1.11.240 getter28DAF0 reads DWORD+18. Absence is zero, unlike a failed read.
                std::uintptr_t observed_vtable{};
                std::memcpy(&observed_vtable, ammo, sizeof(observed_vtable));
                if (observed_vtable != ammo_vtable) return std::nullopt;
                std::uint32_t value{};
                std::memcpy(&value, reinterpret_cast<const std::byte*>(ammo) + 0x18, sizeof(value));
                metadata.reference_loaded_ammo = value;
            }
        }
        if (text != nullptr) {
            auto name = inventory_label(text->displayName);
            // An unresolved generated/quest name is not the base name. Let a later capture retry it.
            if (!name || name->empty()) return std::nullopt;
            metadata.name = std::move(*name);
        }
        if (instance != nullptr) {
            if (instance->base != nullptr && instance->base != &object) return std::nullopt;
            metadata.instance = instance->data;
        }
    }
    if (metadata.name.empty()) {
        if (const auto* full = object.As<RE::TESFullName>()) {
            auto name = inventory_label(full->fullName);
            if (!name) return std::nullopt;
            metadata.name = std::move(*name);
        }
        if (metadata.name.empty()) metadata.name = form_fallback(object);
    }
    return metadata;
}

struct InventoryStatistics final {
    double weight{};
    std::int32_t value{};
};

// Preserve effective engine stats (including Survival weight) without admitting recursive recipe valuation.
[[nodiscard]] inline std::optional<InventoryStatistics> inventory_statistics(
    RE::TESBoundObject& object, const RE::TBO_InstanceData* instance) {
    const auto type = object.GetFormType();
    if (type == RE::ENUM_FORM_ID::kCOBJ) return std::nullopt;
    if (const auto* magic = object.As<RE::MagicItem>()) {
        // These inventory magic classes have audited no-actor cost paths on flat 1.11.240.
        if (type != RE::ENUM_FORM_ID::kALCH && type != RE::ENUM_FORM_ID::kINGR &&
            type != RE::ENUM_FORM_ID::kSCRL) return std::nullopt;
        const auto* data = magic->GetData();
        if (data == nullptr) return std::nullopt;
        if ((data->flags & 1U) == 0) {
            if (magic->listOfEffects.size() > 128 ||
                (!magic->listOfEffects.empty() && magic->listOfEffects.data() == nullptr)) return std::nullopt;
            for (const auto* effect : magic->listOfEffects) {
                if (effect == nullptr || effect->effectSetting == nullptr || !std::isfinite(effect->rawCost))
                    return std::nullopt;
            }
        }
    }
    const auto weight = static_cast<double>(RE::TESWeightForm::GetFormWeight(&object, instance));
    // CommonLib exposes an unsigned return although the native routine uses signed -1 for no value component.
    const auto value = std::bit_cast<std::int32_t>(RE::TESValueForm::GetFormValue(&object, instance));
    if (!std::isfinite(weight) || weight > 100000.0 || (weight < 0.0 && weight != -1.0) || value < -1)
        return std::nullopt;
    // Only the engine's actual no-component sentinel becomes zero, never a skipped query.
    return InventoryStatistics{std::max(0.0, weight), std::max<std::int32_t>(0, value)};
}
#endif

// Copy bounded inventory structure under one try-lock, then resolve metadata without holding that engine lock.
template <class Clock = core::SnapshotClock>
[[nodiscard]] inline std::pair<std::vector<core::InventoryItemSnapshot>, std::string> inventory_snapshot(
    RE::Actor& actor, [[maybe_unused]] typename Clock::time_point deadline = Clock::time_point::max()) {
    if (actor.inventoryList == nullptr) return {{}, "unavailable"};
#if !defined(SYNTH_WITH_F4SEVR)
    const auto started = Clock::now();
    if (started >= deadline) return {{}, "unavailable"};
    const auto structure_deadline = std::min(started + std::chrono::microseconds{500}, deadline);
    const auto metadata_deadline = std::min(started + std::chrono::milliseconds{2}, deadline);
#endif
    struct ItemCopy final {
        RE::BGSInventoryItem item;
        std::uint32_t count{};
        bool equipped{};
#if !defined(SYNTH_WITH_F4SEVR)
        RE::BSTSmartPointer<RE::ExtraDataList> extra;
#endif
    };
    // Each copied item retains the particular stack supplying its count/equipped/name/instance state.
    // Never retain references into the inventory vector or cache these copies across frames.
    static_assert(std::is_copy_constructible_v<RE::BGSInventoryItem>);
    std::vector<ItemCopy> copies;
    copies.reserve(maximum_inventory_copies);
    std::string observation{"complete"};
    {
        const TryReadLock lock{actor.inventoryList->rwLock};
        if (!lock.owns_lock()) return {{}, "unavailable"};
        std::size_t inspected{};
        for (const auto& item : actor.inventoryList->data) {
            if (inspected++ == maximum_inventory_entries) { observation = "partial"; break; }
#if !defined(SYNTH_WITH_F4SEVR)
            if (Clock::now() >= structure_deadline) { observation = "partial"; break; }
#endif
            if (item.object == nullptr || item.stackData == nullptr) {
                observation = "partial";
                continue;
            }
            // Stack accessors are inline field reads in both pinned runtime libraries.
            // Do not call item/name/instance engine routines inside this critical section.
            const auto first_copy=copies.size();
            std::size_t stacks{};
            auto stack = item.stackData;
            for (; stack != nullptr && stacks < 64; stack = stack->nextStack, ++stacks) {
#if !defined(SYNTH_WITH_F4SEVR)
                if (Clock::now() >= structure_deadline) { observation = "partial"; break; }
#endif
                const auto count=stack->GetCount();
                if (count==0) continue;
                if (count>2147483647U) { observation="partial";continue; }
                if (copies.size()==maximum_inventory_copies) { observation="partial";break; }
                auto observed=item;
                // Metadata index zero now refers to this retained stack, not the original first stack.
                observed.stackData=stack;
                copies.push_back({std::move(observed),count,stack->IsEquipped()});
#if !defined(SYNTH_WITH_F4SEVR)
                copies.back().extra = stack->extra;
#endif
            }
            if (stack != nullptr) {
                observation="partial";
                copies.resize(first_copy); // A truncated/cyclic chain must not manufacture repeated instance rows.
                if (stacks<64) break; // The shared row/time budget was exhausted, not the per-item stack limit.
            }
#if defined(SYNTH_WITH_F4SEVR)
            if (copies.size()==128) { observation="partial";break; }
#endif
        }
    }
    // Prioritize the actual equipped stacks before the wire cap, without calling metadata under the list lock.
    std::stable_sort(copies.begin(),copies.end(),[](const auto& left,const auto& right) {return left.equipped && !right.equipped;});
    std::vector<core::InventoryItemSnapshot> result;
    result.reserve(std::min(copies.size(), maximum_inventory_items));
    for (auto& copy : copies) {
        if (result.size() == maximum_inventory_items) { observation = "partial"; break; }
#if !defined(SYNTH_WITH_F4SEVR)
        // Work-admission deadline, not preemption: finish an admitted bounded row, then stop.
        // Retained native copies live only in this call; never join rows captured in different frames.
        if (Clock::now() >= metadata_deadline) { observation = "partial"; break; }
#endif
        auto& item = copy.item;
        auto plugin = origin_plugin(*item.object);
        if (plugin.empty()) { observation = "partial"; continue; }
#if !defined(SYNTH_WITH_F4SEVR)
        auto metadata = inventory_metadata(*item.object, copy.extra);
        if (!metadata) { observation = "partial"; continue; }
        auto name = std::move(metadata->name);
        auto* instance = metadata->instance.get();
#else
        auto name = bounded_label(item.GetDisplayFullName(static_cast<std::uint32_t>(0)));
        if (name.empty()) name = form_fallback(*item.object);
        auto* instance = item.GetInstanceData(0);
#endif
        // Base instances are embedded in the form, not separately owned heap objects.
        // Only ExtraInstanceData::data is retained by an intrusive smart pointer.
        if (instance == nullptr) instance = const_cast<RE::TBO_InstanceData*>(item.object->GetBaseInstanceData());
#if !defined(SYNTH_WITH_F4SEVR)
        const auto statistics = inventory_statistics(*item.object, instance);
        if (!statistics) { observation = "partial"; continue; }
        const auto raw_weight = statistics->weight;
        const auto raw_value = statistics->value;
#else
        const auto raw_weight = instance == nullptr ? 0.0 : static_cast<double>(instance->GetWeight());
        const auto raw_value = instance == nullptr ? 0 : instance->GetValue();
#endif
        core::InventoryItemSnapshot captured{
            item.object->GetFormID(), std::move(plugin), std::move(name), copy.count,
            std::max<std::int32_t>(0, raw_value),
            std::isfinite(raw_weight) && raw_weight > 0.0 ? raw_weight : 0.0,
            static_cast<std::uint16_t>(item.object->GetFormType()), copy.equipped};
        if (!captured.valid()) { observation = "partial"; continue; }
        result.push_back(std::move(captured));
    }
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        if (left.equipped != right.equipped) return left.equipped;
        if (left.display_name != right.display_name) return left.display_name < right.display_name;
        return left.form_id < right.form_id;
    });
    return {std::move(result), std::move(observation)};
}

struct QuestCapture final {
    std::vector<core::QuestSnapshot> quests;
    std::string observation{"unavailable"};
};

// Copy bounded quest evidence under a try-lock; skipped/opaque state never means observed empty.
[[nodiscard]] inline QuestCapture active_quest_snapshot(RE::PlayerCharacter& player) {
    QuestCapture captured;
    const TrySpinLock lock{player.questTargetLock};
    if (!lock.owns_lock()) return captured;
    captured.observation = "complete";
    std::vector<RE::TESQuest*> quests;
    quests.reserve(16);
    const auto remember = [&](RE::TESQuest* quest) {
        if (quest == nullptr) { captured.observation = "partial"; return; }
        if (std::ranges::find(quests, quest) != quests.end()) return;
        if (quests.size() == 16) { captured.observation = "partial"; return; }
        quests.push_back(quest);
    };
    std::size_t inspected{};
    for (const auto& entry : player.questTargets) {
        if (inspected++ == 128) { captured.observation = "partial"; break; }
        remember(entry.first);
    }
#if !defined(SYNTH_WITH_F4SEVR)
    inspected = 0;
    for (const auto& instance : player.objectives) {
        if (inspected++ == 256) { captured.observation = "partial"; break; }
        if (instance.enstanceState == RE::QUEST_OBJECTIVE_STATE::kDisplayed) {
            if (instance.objective == nullptr) { captured.observation = "partial"; continue; }
            remember(instance.objective->ownerQuest);
        }
    }
#else
    // VR headers do not attest displayed-objective state, text or non-target quest discovery.
    captured.observation = "partial";
#endif
    for (auto* quest : quests) {
        auto plugin = origin_plugin(*quest);
        if (plugin.empty()) { captured.observation = "partial"; continue; }
        auto editor_id = bounded_label(quest->formEditorID.c_str());
        auto name = bounded_label(quest->TESFullName::GetFullName());
        if (name.empty()) name = editor_id;
        if (name.empty()) name = form_fallback(*quest);
        std::uint16_t active_objectives{};
        std::vector<std::string> objective_texts;
        std::string objective_observation{"unavailable"};
#if !defined(SYNTH_WITH_F4SEVR)
        objective_observation = "complete";
        inspected = 0;
        for (const auto& instance : player.objectives) {
            if (inspected++ == 256) { objective_observation = "partial"; break; }
            if (instance.enstanceState != RE::QUEST_OBJECTIVE_STATE::kDisplayed) continue;
            if (instance.objective == nullptr) { objective_observation = "partial"; continue; }
            if (instance.objective->ownerQuest != quest) continue;
            ++active_objectives;
            auto text = bounded_label(instance.objective->displayText.c_str());
            if (text.empty()) { objective_observation = "partial"; continue; }
            if (std::ranges::find(objective_texts, text) != objective_texts.end()) continue;
            if (objective_texts.size() == 8) { objective_observation = "partial"; continue; }
            if (text.size() == 255) objective_observation = "partial";
            objective_texts.push_back(std::move(text));
        }
#endif
        core::QuestSnapshot value{
            quest->GetFormID(), std::move(plugin), std::move(name), std::move(editor_id),
            quest->currentStage, active_objectives, std::move(objective_texts),
            std::move(objective_observation)};
#if !defined(SYNTH_WITH_F4SEVR)
        // Pinned flat QUEST_DATA::flags kActive (0x800) means tracked, not merely displayed.
        static_assert(offsetof(RE::QUEST_DATA, flags) == 4);
        static_assert(static_cast<std::uint16_t>(RE::QuestFlag::kActive) == 0x800);
        value.tracked = (quest->data.flags & static_cast<std::uint16_t>(RE::QuestFlag::kActive)) != 0;
#endif
        if (!value.valid()) { captured.observation = "partial"; continue; }
        captured.quests.push_back(std::move(value));
    }
    std::ranges::sort(captured.quests, [](const auto& left, const auto& right) {
        if (left.display_name != right.display_name) return left.display_name < right.display_name;
        return left.form_id < right.form_id;
    });
    return captured;
}

struct NearbyReferenceSnapshots final {
    std::vector<core::NearbyItemSnapshot> items;
    std::vector<core::PointOfInterestSnapshot> points_of_interest;
    std::string items_observation{"unavailable"};
    std::string points_of_interest_observation{"unavailable"};
};

[[nodiscard]] inline bool is_nearby_item_type(RE::ENUM_FORM_ID type) noexcept {
    switch (type) {
        case RE::ENUM_FORM_ID::kARMO:
        case RE::ENUM_FORM_ID::kBOOK:
        case RE::ENUM_FORM_ID::kINGR:
        case RE::ENUM_FORM_ID::kMISC:
        case RE::ENUM_FORM_ID::kWEAP:
        case RE::ENUM_FORM_ID::kAMMO:
        case RE::ENUM_FORM_ID::kKEYM:
        case RE::ENUM_FORM_ID::kALCH:
        case RE::ENUM_FORM_ID::kNOTE:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] inline std::string point_of_interest_kind(const RE::TESObjectREFR& reference,
                                                        RE::ENUM_FORM_ID type) {
    if (reference.extraList != nullptr && reference.extraList->HasType<RE::ExtraMapMarker>()) {
        return "map_marker";
    }
    switch (type) {
        case RE::ENUM_FORM_ID::kDOOR: return "door";
        case RE::ENUM_FORM_ID::kCONT: return "container";
        case RE::ENUM_FORM_ID::kACTI: return "activator";
        case RE::ENUM_FORM_ID::kFLOR: return "flora";
        case RE::ENUM_FORM_ID::kFURN: return "furniture";
        case RE::ENUM_FORM_ID::kTERM: return "terminal";
        default: return {};
    }
}

[[nodiscard]] inline NearbyReferenceSnapshots nearby_reference_snapshots(
    RE::PlayerCharacter& player,
    const core::WorldPose& observer) {
    auto* cell = player.GetParentCell();
    if (cell == nullptr || cell->GetFormID() == 0) return {};
    auto cell_plugin = origin_plugin(*cell);
    if (cell_plugin.empty()) return {};

    std::vector<RE::NiPointer<RE::TESObjectREFR>> references;
    NearbyReferenceSnapshots result;
    {
        const TrySpinLock lock{cell->spinLock};
        if (!lock.owns_lock()) return {};
        result.items_observation = result.points_of_interest_observation = "complete";
        references.reserve(std::min<std::size_t>(cell->references.size(), maximum_cell_references));
        std::size_t inspected{};
        for (const auto& reference : cell->references) {
            if (inspected++ == maximum_cell_references) {
                result.items_observation = result.points_of_interest_observation = "partial";
                break;
            }
            if (reference != nullptr) references.push_back(reference);
        }
    }

    struct Candidate final {
        double distance{};
        std::optional<core::NearbyItemSnapshot> item;
        std::optional<core::PointOfInterestSnapshot> point_of_interest;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(references.size());
    const auto grabbed = player.grabbedObject.get();
    for (auto& reference : references) {
        if (reference == nullptr || reference.get() == &player || reference->GetFormID() == 0 ||
            reference->IsDeleted() || reference->IsDisabled()) {
            continue;
        }
        auto* base = reference->GetObjectReference();
        if (base == nullptr || base->GetFormID() == 0) {
            result.items_observation = result.points_of_interest_observation = "partial";
            continue;
        }
        const auto type = base->GetFormType();
        // Item rows take precedence and need no map-marker query (that getter takes a blocking extra-data lock).
        const auto poi_kind = is_nearby_item_type(type) ? std::string{} : point_of_interest_kind(*reference, type);
        if (!is_nearby_item_type(type) && poi_kind.empty()) continue;
        auto base_plugin = origin_plugin(*base);
        if (base_plugin.empty()) {
            if (is_nearby_item_type(type)) result.items_observation = "partial";
            else result.points_of_interest_observation = "partial";
            continue;
        }
        std::optional<std::string> reference_plugin;
        if (const auto* file = reference->GetFile(0); file != nullptr) {
            auto candidate = bounded_label(file->GetFilename().data());
            if (!candidate.empty()) reference_plugin = std::move(candidate);
        }
        const auto position = point(reference->GetPosition());
        const auto delta = core::Vec3{position.x - observer.position().x,
                                      position.y - observer.position().y,
                                      position.z - observer.position().z};
        const auto distance = std::hypot(delta.x, delta.y, delta.z);
        if (!std::isfinite(distance)) {
            if (is_nearby_item_type(type)) result.items_observation = "partial";
            else result.points_of_interest_observation = "partial";
            continue;
        }
        if (distance > 4096.0) continue;
        const auto along = delta.x * observer.forward().value().x +
                           delta.y * observer.forward().value().y +
                           delta.z * observer.forward().value().z;
        const auto perpendicular = std::sqrt(std::max(0.0, distance * distance - along * along));
        const auto looking_at = along >= 0.0 && perpendicular <= 75.0;

        if (is_nearby_item_type(type)) {
#if !defined(SYNTH_WITH_F4SEVR)
            static REL::Relocation<std::uintptr_t> count_vtable{RE::VTABLE::ExtraCount[0]};
            auto metadata = inventory_metadata(*base, reference->extraList, count_vtable.address());
            if (!metadata) { result.items_observation = "partial"; continue; }
            auto statistics = inventory_statistics(*base, metadata->instance.get());
            if (!statistics) { result.items_observation = "partial"; continue; }
            auto name = std::move(metadata->name);
            const auto count = metadata->reference_count;
            const auto weight = statistics->weight;
            const auto value = statistics->value;
#else
            auto name = bounded_label(reference->GetDisplayFullName());
            if (name.empty()) name = form_fallback(*base);
            auto* instance = const_cast<RE::TBO_InstanceData*>(base->GetBaseInstanceData());
            const auto raw_weight = instance == nullptr ? 0.0 : static_cast<double>(instance->GetWeight());
            const auto raw_value = instance == nullptr ? 0 : instance->GetValue();
            const std::uint32_t count = 1;
            const auto weight = std::isfinite(raw_weight) && raw_weight > 0.0 ? raw_weight : 0.0;
            const auto value = std::max<std::int32_t>(0, raw_value);
#endif
            candidates.push_back(Candidate{
                distance,
                core::NearbyItemSnapshot{
                    reference->GetFormID(), base->GetFormID(), cell->GetFormID(),
                    std::move(reference_plugin), std::move(base_plugin), cell_plugin,
                    std::move(name), position, distance,
                    weight, count, value,
                    static_cast<std::uint16_t>(type), reference->IsCrimeToActivate(),
                    looking_at, grabbed != nullptr && grabbed.get() == reference.get()},
                std::nullopt});
        } else {
            auto name = bounded_label(reference->GetDisplayFullName());
            if (name.empty()) name = form_fallback(*base);
            auto* lock = reference->GetLock();
            const auto locked = lock != nullptr &&
                                lock->GetLockLevel(reference.get()) != RE::LOCK_LEVEL::kUnlocked;
            candidates.push_back(Candidate{
                distance, std::nullopt,
                core::PointOfInterestSnapshot{
                    reference->GetFormID(), base->GetFormID(), cell->GetFormID(),
                    std::move(reference_plugin), std::move(base_plugin), cell_plugin,
                    std::move(name), poi_kind, position, distance, locked, looking_at}});
        }
    }
    std::ranges::sort(candidates, [](const Candidate& left, const Candidate& right) {
        return left.distance < right.distance;
    });
    for (auto& candidate : candidates) {
        if (candidate.item) {
            if (result.items.size() < maximum_nearby_items && candidate.item->valid())
                result.items.push_back(std::move(*candidate.item));
            else result.items_observation = "partial";
        } else if (candidate.point_of_interest) {
            if (result.points_of_interest.size() < maximum_points_of_interest && candidate.point_of_interest->valid())
                result.points_of_interest.push_back(std::move(*candidate.point_of_interest));
            else result.points_of_interest_observation = "partial";
        }
    }
    return result;
}

[[nodiscard]] inline core::ActorSnapshot actor_snapshot(const RE::Actor& actor,
                                                        const std::string& playthrough,
                                                        RE::PlayerCharacter* player,
                                                        bool include_inventory = false,
                                                        bool include_details = true) {
    const auto* display = const_cast<RE::Actor&>(actor).GetDisplayFullName();
    const auto name = display == nullptr || *display == '\0' ? std::string{"Unnamed actor"}
                                                              : std::string{display};
    auto plugin = origin_plugin(actor);
    if (plugin.empty() && actor.GetFormID() == 0x14) {
        plugin = "Fallout4.esm";
    }
    auto& mutable_actor = const_cast<RE::Actor&>(actor);
    auto race = actor.race == nullptr ? std::string{}
                                      : bounded_label(actor.race->formEditorID.c_str());
    if (race.empty() && actor.race != nullptr) race = bounded_label(actor.race->GetFullName());
    if (race.empty() && actor.race != nullptr) race = form_fallback(*actor.race);
    auto* npc = actor.GetNPC();
    const auto base_form_id = npc == nullptr ? actor.GetFormID() : npc->GetFormID();
    auto base_plugin = npc == nullptr ? plugin : origin_plugin(*npc);
    if (base_plugin.empty()) base_plugin = plugin;
    std::string sex;
    std::string voice_type;
    if (npc != nullptr) {
        const auto actor_sex = npc->GetSex();
        sex = actor_sex == RE::SEX::kFemale ? "female"
              : actor_sex == RE::SEX::kMale ? "male"
                                             : "unknown";
        if (npc->voiceType != nullptr) {
            voice_type = bounded_label(npc->voiceType->formEditorID.c_str());
            if (voice_type.empty()) voice_type = form_fallback(*npc->voiceType);
        }
    }
    std::vector<core::FactionMembershipSnapshot> factions;
    std::string faction_observation{"unavailable"};
    std::string faction_completeness{"unavailable"};
    if (include_details) {
        auto captured_factions = faction_memberships(actor, npc);
        factions = std::move(captured_factions.factions);
        faction_observation = std::move(captured_factions.observation);
        faction_completeness = std::move(captured_factions.completeness);
    }
    const auto actor_life_state = life_state(actor);
    const auto actor_posture = posture(actor);
    const auto raw_movement_speed = static_cast<double>(actor.DoGetCurrentSpeed());
    const auto movement_speed = std::isfinite(raw_movement_speed)
                                    ? std::clamp(raw_movement_speed, 0.0, 100000.0)
                                    : 0.0;
    auto package = include_details ? current_package(mutable_actor)
                                   : std::optional<core::PackageSnapshot>{};
    const auto* actor_values = RE::ActorValue::GetSingleton();
    const auto health = actor_value_percent(actor, actor_values == nullptr ? nullptr : actor_values->health);
    const auto action_points = actor_value_percent(actor, actor_values == nullptr ? nullptr : actor_values->actionPoints);
    auto inventory = include_inventory ? inventory_snapshot(mutable_actor)
        : std::pair<std::vector<core::InventoryItemSnapshot>, std::string>{{}, "unavailable"};
    // Copied conversation admission only; use the same area rule as requested_actor_snapshot.
    const auto* player_cell = player ? player->GetParentCell() : nullptr;
    const auto* actor_cell = actor.GetParentCell();
    const bool conversation_area = player_cell && actor_cell && mutable_actor.Get3D() &&
        !mutable_actor.IsDeleted() && (player_cell == actor_cell ||
        (!player_cell->IsInterior() && !actor_cell->IsInterior() && player_cell->worldSpace &&
         player_cell->worldSpace == actor_cell->worldSpace));
    return {actor.GetFormID(), name, point(actor.GetPosition()), std::move(plugin), playthrough,
            !actor.IsDead(false), mutable_actor.IsDisabled(), actor.IsInCombat(),
            player != nullptr && &actor != player && mutable_actor.GetHostileToActor(player),
            mutable_actor.IsSneaking(), std::max<std::int16_t>(1, mutable_actor.GetLevel()),
            std::move(race), std::move(sex), std::move(voice_type),
            (actor.niFlags.flags & (1u << 26)) != 0,
            health.value_or(0.0), action_points.value_or(0.0),
            std::move(inventory.first),
            std::nullopt, std::move(factions), std::move(faction_observation),
            actor_life_state, actor_posture, actor.GetWeaponMagicDrawn(), movement_speed,
            actor.DoGetSprinting(), actor.talkingToPlayer != 0,
            RE::PowerArmor::ActorInPowerArmor(actor), std::move(package), base_form_id,
            std::move(base_plugin), std::move(inventory.second), std::move(faction_completeness),
            health.has_value(), action_points.has_value(), conversation_area};
}

// Resolve a caller-named loaded actor even when process-list discovery omitted it from the nearest 64.
[[nodiscard]] inline std::optional<core::ActorSnapshot> requested_actor_snapshot(
    std::uint32_t form_id, const std::string& playthrough) {
    const auto* manager = RE::BGSSaveLoadManager::GetSingleton();
    if (manager == nullptr || manager->currentPlayerID == 0 || playthrough != playthrough_id()) return {};
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* actor = RE::TESForm::GetFormByID<RE::Actor>(form_id);
    if (player == nullptr || actor == nullptr || actor == player || !actor->Get3D()) return {};
    const auto* player_cell = player->GetParentCell();
    const auto* actor_cell = actor->GetParentCell();
    if (!player_cell || !actor_cell) return {};
    if (player_cell != actor_cell && (player_cell->IsInterior() || actor_cell->IsInterior() ||
        !player_cell->worldSpace || player_cell->worldSpace != actor_cell->worldSpace)) return {};
    return actor_snapshot(*actor, playthrough, player, false, false);
}

#if !defined(SYNTH_WITH_F4SEVR)
// Request-only player inventory capture; the caller preserves all original player facts and fences the native epoch.
[[nodiscard]] inline std::optional<core::ActorSnapshot> selected_player_inventory(const core::ActorSnapshot& selected) {
    const auto* manager = RE::BGSSaveLoadManager::GetSingleton();
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (manager == nullptr || manager->currentPlayerID == 0 || player == nullptr ||
        selected.form_id() != 0x14 || selected.playthrough_id() != playthrough_id() || menu_mode_active()) return {};
    return actor_snapshot(*player, selected.playthrough_id(), player, true, false);
}
#endif

// Re-resolve only the selected same-frame NPC. No discovery scan or nearest-actor substitution.
[[nodiscard]] inline std::optional<core::ActorSnapshot> selected_actor_details(const core::ActorSnapshot& selected) {
    const auto* manager = RE::BGSSaveLoadManager::GetSingleton();
    if (manager == nullptr || manager->currentPlayerID == 0 || selected.playthrough_id() != playthrough_id()) return {};
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* actor = RE::TESForm::GetFormByID<RE::Actor>(selected.form_id());
    if (player == nullptr || actor == nullptr || actor == player || actor->IsDead(false) || actor->IsDisabled() ||
        origin_plugin(*actor) != selected.origin_plugin()) return {};
    auto* npc = actor->GetNPC();
    if (npc == nullptr || npc->GetFormID() != selected.base_form_id() || origin_plugin(*npc) != selected.base_origin_plugin()) return {};
    const auto started = core::SnapshotClock::now();
    auto observed = actor_snapshot(*actor, selected.playthrough_id(), player, true, true);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(core::SnapshotClock::now() - started);
#if defined(SYNTH_WITH_F4SEVR)
    spdlog::info("SYNTH selected actor capture: form {:08X}, inventory {}, factions {} / {}, {} ms",
#else
    REX::INFO("SYNTH selected actor capture: form {:08X}, inventory {}, factions {} / {}, {} ms",
#endif
        selected.form_id(), observed.inventory_observation(), observed.faction_observation(),
        observed.faction_completeness(), elapsed.count());
    return observed;
}

struct NearbyActorSnapshots final {
    std::vector<core::ActorSnapshot> actors;
    std::string observation{"unavailable"};
};

// Flat-only rolling discovery retains numeric aged handles, never engine references.
struct NearbyActorDiscovery final {
    core::ActorDiscoveryScan scan;
    std::array<std::uint32_t, maximum_nearby_actors> handles{};
    std::size_t count{};
};

[[nodiscard]] inline NearbyActorSnapshots nearby_actors(
    const RE::PlayerCharacter& player,
    const std::string& playthrough,
    NearbyActorDiscovery* discovery = nullptr) {
    const auto* lists = RE::ProcessLists::GetSingleton();
    if (lists == nullptr) {
        if (discovery) *discovery = {};
        return {};
    }
    NearbyActorSnapshots captured{{}, "complete"};

    struct Candidate final {
        double distance_squared{};
        std::uint32_t form_id{};
        RE::NiPointer<RE::Actor> reference;
        std::uint32_t handle{};
    };
    std::vector<Candidate> candidates;
    candidates.reserve(maximum_nearby_actors);
    std::unordered_set<std::uint32_t> retained;
    retained.reserve(maximum_nearby_actors);
    const auto nearer = [](const Candidate& left, const Candidate& right) {
        if (left.distance_squared != right.distance_squared) {
            return left.distance_squared < right.distance_squared;
        }
        return left.form_id < right.form_id;
    };
    const auto player_position = point(player.GetPosition());
    const auto consider = [&](const auto& handle) {
        auto actor = handle.get();
        if (!actor) { captured.observation = "partial"; return; }
        const auto form_id = actor->GetFormID();
        if (form_id == 0) { captured.observation = "partial"; return; }
        if (form_id == player.GetFormID() || retained.contains(form_id)) return;
        const auto actor_position = point(actor->GetPosition());
        const auto delta = core::Vec3{actor_position.x - player_position.x,
                                      actor_position.y - player_position.y,
                                      actor_position.z - player_position.z};
        const auto distance_squared = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
        if (!std::isfinite(distance_squared)) { captured.observation = "partial"; return; }
        Candidate candidate{distance_squared, form_id, std::move(actor)};
#if !defined(SYNTH_WITH_F4SEVR)
        auto copied_handle = handle;
        candidate.handle = copied_handle.get_handle();
#endif
        // Keep the farthest retained actor at the heap root; all positions
        // are read again on this capture, including previously discovered actors.
        if (candidates.size() == maximum_nearby_actors) {
            captured.observation = "partial";
            if (!nearer(candidate, candidates.front())) return;
            auto identity = retained.extract(candidates.front().form_id);
            identity.value() = form_id;
            retained.insert(std::move(identity));
            std::pop_heap(candidates.begin(), candidates.end(), nearer);
            candidates.pop_back();
        } else {
            retained.insert(form_id);
        }
        candidates.push_back(std::move(candidate));
        std::push_heap(candidates.begin(), candidates.end(), nearer);
    };
    const auto append = [&](const auto& handles) { for (const auto& handle : handles) consider(handle); };
#if !defined(SYNTH_WITH_F4SEVR)
    if (discovery) {
        const std::array<std::size_t, 4> sizes{lists->highActorHandles.size(), lists->middleHighActorHandles.size(),
                                             lists->middleLowActorHandles.size(), lists->lowActorHandles.size()};
        const auto slices = discovery->scan.next(sizes);
        bool complete = true;
        for (std::size_t tier = 0; tier < 4; ++tier) if (slices[tier].count != sizes[tier]) complete = false;
        if (!complete) {
            captured.observation = "partial";
            for (std::size_t i = 0; i < discovery->count; ++i) {
                RE::ActorHandle handle;
                static_assert(sizeof(handle) == sizeof(std::uint32_t));
                std::memcpy(static_cast<void*>(&handle), &discovery->handles[i], sizeof(handle));
                consider(handle);
            }
        }
        discovery->count = 0;
        const auto scan = [&](const auto& handles, const core::ActorDiscoverySlice slice) {
            auto index = slice.begin;
            for (std::size_t i = 0; i < slice.count; ++i) {
                consider(handles[static_cast<decltype(handles.size())>(index)]);
                if (++index == handles.size()) index = 0;
            }
        };
        scan(lists->highActorHandles, slices[0]);
        scan(lists->middleHighActorHandles, slices[1]);
        scan(lists->middleLowActorHandles, slices[2]);
        scan(lists->lowActorHandles, slices[3]);
    } else
#endif
    {
        // VR pointing still depends on exhaustive current discovery; do not truncate it.
        append(lists->highActorHandles);
        append(lists->middleHighActorHandles);
        append(lists->middleLowActorHandles);
        append(lists->lowActorHandles);
    }

    std::sort_heap(candidates.begin(), candidates.end(), nearer);
    auto& result = captured.actors;
    result.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        auto& candidate = candidates[index];
        if (candidate.reference == nullptr) { captured.observation = "partial"; continue; }
        try {
            result.push_back(actor_snapshot(*candidate.reference, playthrough,
                                            const_cast<RE::PlayerCharacter*>(&player),
                                            false, false));
#if !defined(SYNTH_WITH_F4SEVR)
            if (discovery) discovery->handles[discovery->count++] = candidate.handle;
#endif
        } catch (const std::invalid_argument&) {
            // Dynamic references without a safe canonical plugin identity are not exposed.
            captured.observation = "partial";
        }
    }
    return captured;
}

[[nodiscard]] inline RE::NiAVObject* named_node(RE::NiAVObject* root,
                                                std::initializer_list<const char*> names) {
    if (root == nullptr) {
        return nullptr;
    }
    for (const auto* name : names) {
        if (auto* found = root->GetObjectByName(RE::BSFixedString{name}); found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

#if !defined(SYNTH_WITH_F4SEVR)
struct FlatCaptureCache final {
    std::string playthrough;
    std::uint32_t cell_form_id{};
    std::vector<core::LoadedPlugin> loaded_plugins;
    QuestCapture active_quests;
    core::SnapshotClock::time_point next_quest_capture{};
    NearbyReferenceSnapshots nearby_references;
    NearbyActorDiscovery actor_discovery;
};

inline FlatCaptureCache flat_capture_cache;

// A load boundary invalidates every pointer-derived context domain. The next
// bootstrap capture rebuilds only immutable identity before networking starts.
inline void reset_flat_capture_cache() noexcept {
    flat_capture_cache = {};
}

[[nodiscard]] inline const char* capture_purpose_name(
    runtime::RuntimeCapturePurpose purpose) noexcept {
    switch (purpose) {
        case runtime::RuntimeCapturePurpose::bootstrap: return "bootstrap";
        case runtime::RuntimeCapturePurpose::dialogue: return "dialogue";
        case runtime::RuntimeCapturePurpose::background: return "background";
        case runtime::RuntimeCapturePurpose::actor_events: return "actor_events";
        case runtime::RuntimeCapturePurpose::player_inventory: return "player_inventory";
    }
    return "unknown";
}

// Build a cheap connection snapshot first. Automatic background cadence stays
// on shallow actor data; deep player/inventory reads during the first cadence
// after a save load can wait on engine-owned state and freeze Fallout. Explicit
// dialogue captures refresh the bounded environment cache before a request.
[[nodiscard]] inline detail::CapturedRuntimeValues capture_flat(
    core::SnapshotClock::time_point now,
    runtime::RuntimeCapturePurpose purpose = runtime::RuntimeCapturePurpose::dialogue) {
    const auto started_at = std::chrono::steady_clock::now();
    const auto capture_owner = FlatPickedReference::stamp();
    const auto actor_events = purpose == runtime::RuntimeCapturePurpose::actor_events;
    const auto player_inventory = purpose == runtime::RuntimeCapturePurpose::player_inventory;
    const auto isolated = actor_events || player_inventory;
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* camera = RE::PlayerCamera::GetSingleton();
    if (player == nullptr || camera == nullptr || !camera->cameraRoot) {
        throw std::runtime_error{"Fallout 4 player camera is unavailable"};
    }
    const auto playthrough = playthrough_id();
    const auto ticks = game_time_ticks();
    const auto camera_pose = pose(camera->cameraRoot->world);
    if (flat_capture_cache.playthrough != playthrough) {
        reset_flat_capture_cache();
        flat_capture_cache.playthrough = playthrough;
    }
    if (flat_capture_cache.loaded_plugins.empty()) {
        flat_capture_cache.loaded_plugins = loaded_plugins();
    }
    const auto* cell = player->GetParentCell();
    const auto cell_form_id = cell == nullptr ? 0 : cell->GetFormID();
    if (flat_capture_cache.cell_form_id != cell_form_id) {
        flat_capture_cache.cell_form_id = cell_form_id;
        flat_capture_cache.nearby_references = {};
        flat_capture_cache.actor_discovery = {};
    }

    NearbyActorSnapshots actors;
    const auto bootstrap = purpose == runtime::RuntimeCapturePurpose::bootstrap;
    const auto dialogue = purpose == runtime::RuntimeCapturePurpose::dialogue;
    if (dialogue) REX::INFO("SYNTH capture: dialogue beginning nearby actors");
    if (!bootstrap && !isolated) {
        actors = nearby_actors(*player, playthrough, &flat_capture_cache.actor_discovery);
    }

    const char* refreshed_domain = "none";
    if (bootstrap) flat_capture_cache.next_quest_capture = now + std::chrono::seconds{5};
    const auto refresh_quests = !isolated && (dialogue || (!bootstrap && now >= flat_capture_cache.next_quest_capture));
    if (refresh_quests) {
        // Existing bounded, try-lock collector only; never add deep player or nearby-reference reads to cadence.
        flat_capture_cache.active_quests = active_quest_snapshot(*player);
        flat_capture_cache.next_quest_capture = now + std::chrono::seconds{5};
        refreshed_domain = "quest_journal";
    }
    if (purpose == runtime::RuntimeCapturePurpose::dialogue) {
        REX::INFO("SYNTH capture: dialogue quests returned; beginning nearby references");
        flat_capture_cache.nearby_references =
            nearby_reference_snapshots(*player, camera_pose);
        refreshed_domain = "dialogue_context";
    }

    if (dialogue) REX::INFO("SYNTH capture: dialogue references returned; beginning player/world");

    auto values = detail::CapturedRuntimeValues{
        camera_pose, std::nullopt, std::nullopt, std::nullopt,
        // Only the audited inventory collector is enabled, never deep faction/package reads.
        // No cross-frame inventory cache: every dialogue owns a fresh observation or explicit unavailability.
        actor_snapshot(*player, playthrough, player, (dialogue || player_inventory) && !menu_mode_active(), false),
        std::move(actors.actors), ticks, world_state(*player, ticks),
        flat_capture_cache.loaded_plugins, flat_capture_cache.active_quests.quests,
        flat_capture_cache.nearby_references.items,
        flat_capture_cache.nearby_references.points_of_interest,
        refresh_quests || flat_capture_cache.active_quests.observation == "unavailable"
            ? flat_capture_cache.active_quests.observation : "cached",
        dialogue || flat_capture_cache.nearby_references.items_observation == "unavailable"
            ? flat_capture_cache.nearby_references.items_observation : "cached",
        dialogue || flat_capture_cache.nearby_references.points_of_interest_observation == "unavailable"
            ? flat_capture_cache.nearby_references.points_of_interest_observation : "cached",
        std::move(actors.observation)};
    if (!bootstrap && !isolated && !menu_mode_active()) {
        const auto pick_owner = FlatPickedReference::stamp();
        const auto raw_handle = FlatPickedReference::read();
        values.picked_observed = raw_handle.has_value();
        if (raw_handle && *raw_handle != 0) {
            // CommonLib's handle has no raw-value constructor. Its sole 32-bit field
            // is copied into a local handle, then resolved only on this game-pump lane.
            RE::ObjectRefHandle handle;
            static_assert(sizeof(handle) == sizeof(std::uint32_t));
            std::memcpy(static_cast<void*>(&handle), &*raw_handle, sizeof(handle));
            auto reference = handle.get();
            if (reference && reference->As<RE::Actor>()) {
                try { values.picked_actor = requested_actor_snapshot(reference->GetFormID(), playthrough); }
                catch (const std::invalid_argument&) { values.picked_actor.reset(); }
            }
        }
        if (pick_owner != FlatPickedReference::stamp()) {
            values.picked_observed = false;
            values.picked_actor.reset();
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);
    if (dialogue) REX::INFO("SYNTH capture: dialogue complete in {} ms", elapsed.count());
    if (elapsed >= std::chrono::milliseconds{33}) {
        REX::WARN("SYNTH capture: {} took {} ms; refreshed {}", capture_purpose_name(purpose),
                  elapsed.count(), refreshed_domain);
    } else if (bootstrap) {
        REX::INFO("SYNTH capture: bootstrap complete in {} ms", elapsed.count());
    }

    // F4SE invalidates this epoch immediately, before its deferred runtime-generation update.
    if (FlatPickedReference::stamp() != capture_owner) {
        reset_flat_capture_cache();
        throw std::runtime_error{"Fallout 4 capture crossed a native load boundary"};
    }
    return values;
}
#endif

[[nodiscard]] inline detail::CapturedRuntimeValues capture_vr(
    core::SnapshotClock::time_point,
    runtime::RuntimeCapturePurpose purpose = runtime::RuntimeCapturePurpose::dialogue) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* camera = RE::PlayerCamera::GetSingleton();
    if (player == nullptr || camera == nullptr || !camera->cameraRoot) {
        throw std::runtime_error{"Fallout 4 VR HMD camera is unavailable"};
    }
    const auto playthrough = playthrough_id();
    const auto ticks = game_time_ticks();
    auto* root = player->Get3D();
    auto* left = named_node(root, {"SecondaryWandNode", "SecondaryWand"});
    auto* right = named_node(root, {"PrimaryWandNode", "PrimaryWand"});
    const auto hmd_pose = pose(camera->cameraRoot->world);
    const auto bootstrap = purpose == runtime::RuntimeCapturePurpose::bootstrap;
    auto references = bootstrap ? NearbyReferenceSnapshots{}
                                : nearby_reference_snapshots(*player, hmd_pose);
    auto quests = bootstrap ? QuestCapture{} : active_quest_snapshot(*player);
    auto actors = bootstrap ? NearbyActorSnapshots{} : nearby_actors(*player, playthrough);
    return {actor_pose(*player), hmd_pose,
            left == nullptr ? std::nullopt : std::optional{pose(left->world)},
            right == nullptr ? std::nullopt : std::optional{pose(right->world)},
            actor_snapshot(*player, playthrough, player, !bootstrap, !bootstrap),
            std::move(actors.actors),
            ticks, world_state(*player, ticks), loaded_plugins(),
            std::move(quests.quests),
            std::move(references.items), std::move(references.points_of_interest),
            std::move(quests.observation), std::move(references.items_observation),
            std::move(references.points_of_interest_observation), std::move(actors.observation)};
}

}  // namespace synth::adapters::commonlib
