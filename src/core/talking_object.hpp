#pragma once

#include "core/snapshot.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace synth::core {

// Talking objects (Synthserver docs/TALKING-OBJECTS.md): choosing the one world object a private narrator is bound to
// or addressed through. Engine-free; it reads only the copied snapshot, so flat and VR share it. Actors are never
// candidates (a robot speaks through the NPC path), stacks are refused, and nothing is ever matched by name,
// base or position alone.
inline constexpr double talking_object_look_range = 300.0;
inline constexpr double talking_object_address_range = 350.0;
inline constexpr double talking_object_ambiguity_margin = 40.0;

enum class TalkingObjectKind : unsigned char { terminal, activator, furniture, container, item };

// Exact object identity as it travels: a plugin reference by plugin and plugin-local ID, a runtime-created (0xFF)
// reference by its full ID with no plugin; the base always comes from a plugin and is plugin-local.
struct TalkingObjectIdentity final {
    std::optional<std::string> ref_plugin;
    std::uint32_t ref_id{};
    std::string base_plugin;
    std::uint32_t base_id{};
    [[nodiscard]] bool operator==(const TalkingObjectIdentity&) const = default;
    [[nodiscard]] bool runtime_created() const noexcept { return !ref_plugin.has_value(); }
};

struct TalkingObjectCandidate final {
    TalkingObjectIdentity identity;
    TalkingObjectKind kind{TalkingObjectKind::item};
    std::string display_name;
    double distance{};
    bool looking_at{}, held{};
};

// Fixed-width wire form of an ID: "0x" followed by eight upper-case hexadecimal digits.
[[nodiscard]] inline std::string talking_object_wire_id(std::uint32_t value) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out{"0x00000000"};
    for (int index = 9; index >= 2; --index, value >>= 4) out[static_cast<std::size_t>(index)] = digits[value & 0xFU];
    return out;
}

// A plugin-origin reference is decoded through the loaded plugin that defines it; a runtime-created reference has
// no origin plugin and must carry the 0xFF prefix. Anything else (an unresolved prefix, a runtime-created base, a
// null ID) is not an identity.
[[nodiscard]] inline std::optional<TalkingObjectIdentity> talking_object_identity(
    std::uint32_t reference_id, const std::optional<std::string>& reference_plugin, std::uint32_t base_id,
    const std::string& base_plugin, const std::vector<LoadedPlugin>& plugins) {
    const auto base = plugin_local_form_id(base_id, base_plugin, plugins);
    if (!base) return std::nullopt;
    TalkingObjectIdentity out{std::nullopt, 0, base_plugin, *base};
    if (!reference_plugin) {
        if ((reference_id >> 24) != 0xFFU || reference_id == 0xFF000000U) return std::nullopt;
        out.ref_id = reference_id;
        return out;
    }
    const auto local = plugin_local_form_id(reference_id, *reference_plugin, plugins);
    if (!local) return std::nullopt;
    out.ref_plugin = *reference_plugin;
    out.ref_id = *local;
    return out;
}

// Every eligible reference the snapshot copied: terminals, activators (radios), furniture, containers and single
// items. Actors never appear in these lists.
[[nodiscard]] inline std::vector<TalkingObjectCandidate> talking_object_candidates(const RuntimeSnapshot& snapshot) {
    std::vector<TalkingObjectCandidate> out;
    const auto& plugins = snapshot.loaded_plugins();
    for (const auto& item : snapshot.nearby_items()) {
        if (item.count != 1 || !item.valid()) continue;
        auto identity = talking_object_identity(item.reference_id, item.reference_origin_plugin, item.base_form_id,
                                                item.base_origin_plugin, plugins);
        if (!identity) continue;
        out.push_back({std::move(*identity), TalkingObjectKind::item, item.display_name, item.distance, item.looking_at, item.held});
    }
    for (const auto& point : snapshot.points_of_interest()) {
        const auto kind = point.kind == "terminal" ? std::optional{TalkingObjectKind::terminal}
            : point.kind == "activator" ? std::optional{TalkingObjectKind::activator}
            : point.kind == "furniture" ? std::optional{TalkingObjectKind::furniture}
            : point.kind == "container" ? std::optional{TalkingObjectKind::container} : std::nullopt;
        if (!kind || !point.valid()) continue;
        auto identity = talking_object_identity(point.reference_id, point.reference_origin_plugin, point.base_form_id,
                                                point.base_origin_plugin, plugins);
        if (!identity) continue;
        out.push_back({std::move(*identity), *kind, point.display_name, point.distance, point.looking_at, false});
    }
    return out;
}

enum class TalkingObjectScanStatus : unsigned char { found, none, ambiguous };
struct TalkingObjectScan final {
    TalkingObjectScanStatus status{TalkingObjectScanStatus::none};
    std::optional<TalkingObjectCandidate> object;
};

// The object the player is aiming at or holding. A held object wins; otherwise the nearest candidate in the look
// cone within range. A second candidate nearly as close refuses the choice instead of guessing.
[[nodiscard]] inline TalkingObjectScan scan_talking_object(const RuntimeSnapshot& snapshot) {
    auto candidates = talking_object_candidates(snapshot);
    std::erase_if(candidates, [](const TalkingObjectCandidate& value) {
        return !value.held && !(value.looking_at && value.distance <= talking_object_look_range);
    });
    // One reference can appear in both nearby lists; it is still one object. Keep its held/nearest copy.
    std::ranges::stable_sort(candidates, [](const TalkingObjectCandidate& left, const TalkingObjectCandidate& right) {
        if (left.held != right.held) return left.held;
        return left.distance < right.distance;
    });
    std::vector<TalkingObjectCandidate> unique;
    for (auto& value : candidates)
        if (std::ranges::none_of(unique, [&](const TalkingObjectCandidate& seen) { return seen.identity == value.identity; }))
            unique.push_back(std::move(value));
    candidates = std::move(unique);
    if (candidates.empty()) return {};
    std::ranges::sort(candidates, [](const TalkingObjectCandidate& left, const TalkingObjectCandidate& right) {
        if (left.held != right.held) return left.held;
        return left.distance < right.distance;
    });
    const auto& first = candidates.front();
    if (candidates.size() > 1) {
        const auto& second = candidates[1];
        const auto tie = first.held ? second.held : second.distance - first.distance < talking_object_ambiguity_margin;
        if (tie) return {TalkingObjectScanStatus::ambiguous, std::nullopt};
    }
    return {TalkingObjectScanStatus::found, first};
}

// The exact bound object, if it is still in the snapshot and close enough to address. Never adopts another reference.
[[nodiscard]] inline std::optional<TalkingObjectCandidate> locate_talking_object(
    const RuntimeSnapshot& snapshot, const TalkingObjectIdentity& identity) {
    for (auto& candidate : talking_object_candidates(snapshot))
        if (candidate.identity == identity && (candidate.held || candidate.distance <= talking_object_address_range))
            return std::move(candidate);
    return std::nullopt;
}

}  // namespace synth::core
