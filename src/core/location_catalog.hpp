// Engine-free model of one exported Commonwealth location (the `location_catalog` event payload).
// The game adapter fills these values on the game thread; nothing here touches the engine. The
// bounds repeat protocol/v2/schemas/location-catalog.schema.json, so an invalid record is dropped
// by the client instead of failing a whole batch at the server.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace synth::core {

inline constexpr std::size_t maximum_catalog_batch = 50;
inline constexpr std::size_t maximum_catalog_markers = 32;
inline constexpr std::size_t maximum_catalog_keywords = 64;
inline constexpr std::size_t maximum_catalog_name_bytes = 255;
inline constexpr double maximum_catalog_coordinate = 10000000.0;

// A form identified by the plugin that defines it and its 24 local bits, never by load-order slot.
struct PluginForm final {
    std::string origin_plugin;
    std::uint32_t local_form_id{};

    [[nodiscard]] bool valid() const noexcept {
        if (local_form_id == 0 || local_form_id > 0x00FFFFFFU || origin_plugin.empty() || origin_plugin.size() > 255 ||
            origin_plugin.find_first_of("/\\") != std::string::npos) return false;
        const auto dot = origin_plugin.find_last_of('.');
        if (dot == std::string::npos || dot == 0 || origin_plugin.size() - dot != 4) return false;
        std::string extension;
        for (auto c = dot + 1; c < origin_plugin.size(); ++c) {
            const auto value = static_cast<unsigned char>(origin_plugin[c]);
            extension.push_back(static_cast<char>(value >= 'A' && value <= 'Z' ? value + 32 : value));
        }
        return extension == "esm" || extension == "esp" || extension == "esl";
    }
    [[nodiscard]] bool same_form(const PluginForm& other) const noexcept {
        return local_form_id == other.local_form_id && origin_plugin.size() == other.origin_plugin.size() &&
            std::ranges::equal(origin_plugin, other.origin_plugin, [](unsigned char left, unsigned char right) {
                return (left >= 'A' && left <= 'Z' ? left + 32 : left) == (right >= 'A' && right <= 'Z' ? right + 32 : right);
            });
    }
    bool operator==(const PluginForm&) const = default;
};

enum class CatalogMarkerType : unsigned char {
    location_center,
    inside_entrance,
    outside_entrance,
    boss_treasure,
    map_marker,
    world_marker,
    workshop,
};

[[nodiscard]] inline const char* catalog_marker_name(CatalogMarkerType type) noexcept {
    switch (type) {
    case CatalogMarkerType::location_center: return "location_center";
    case CatalogMarkerType::inside_entrance: return "inside_entrance";
    case CatalogMarkerType::outside_entrance: return "outside_entrance";
    case CatalogMarkerType::boss_treasure: return "boss_treasure";
    case CatalogMarkerType::map_marker: return "map_marker";
    case CatalogMarkerType::world_marker: return "world_marker";
    case CatalogMarkerType::workshop: return "workshop";
    }
    return "";
}

struct CatalogPoint final {
    double x{};
    double y{};
    double z{};
    [[nodiscard]] bool valid() const noexcept {
        return std::ranges::all_of(std::array{x, y, z}, [](double v) {
            return std::isfinite(v) && std::fabs(v) <= maximum_catalog_coordinate;
        });
    }
    bool operator==(const CatalogPoint&) const = default;
};

// An editor ID the server accepts: one to 128 of [A-Za-z0-9_]. Anything else is sent as no editor ID.
[[nodiscard]] inline bool catalog_editor_id(const std::string& value) noexcept {
    return !value.empty() && value.size() <= 128 && std::ranges::all_of(value, [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

struct CatalogWorldspace final {
    PluginForm ref;
    std::string editor_id;
    std::string name;
    std::optional<PluginForm> parent;
    bool uses_parent_map{};
    [[nodiscard]] bool valid() const noexcept {
        return ref.valid() && (editor_id.empty() || catalog_editor_id(editor_id)) && name.size() <= maximum_catalog_name_bytes &&
            (!parent || parent->valid());
    }
    bool operator==(const CatalogWorldspace&) const = default;
};

// A reference the game ties to the location. An interior marker has no worldspace; an exterior one always has.
struct CatalogMarker final {
    CatalogMarkerType type{CatalogMarkerType::location_center};
    PluginForm ref;
    std::optional<PluginForm> cell;
    bool interior{};
    std::optional<CatalogWorldspace> worldspace;
    CatalogPoint position;
    bool disabled{};
    [[nodiscard]] bool valid() const noexcept {
        return ref.valid() && (!cell || cell->valid()) && interior != worldspace.has_value() &&
            (!worldspace || worldspace->valid()) && position.valid();
    }
    bool operator==(const CatalogMarker&) const = default;
};

struct CatalogLocation final {
    PluginForm ref;
    std::string name;
    std::string editor_id;
    std::optional<PluginForm> parent;
    std::vector<std::string> keywords;
    // Save-dependent facts; absent means the game could not answer, never "no".
    std::optional<bool> cleared;
    std::optional<bool> discovered;
    std::vector<CatalogMarker> markers;

    [[nodiscard]] bool valid() const noexcept {
        if (!ref.valid() || name.empty() || name.size() > maximum_catalog_name_bytes ||
            (!editor_id.empty() && !catalog_editor_id(editor_id)) || (parent && !parent->valid()) ||
            keywords.size() > maximum_catalog_keywords || markers.size() > maximum_catalog_markers) return false;
        for (std::size_t i = 0; i < keywords.size(); ++i) {
            const auto& keyword = keywords[i];
            if (keyword.empty() || keyword.size() > 128 || !catalog_editor_id(keyword) ||
                (keyword.front() >= '0' && keyword.front() <= '9') || keyword.front() == '_' ||
                std::ranges::find(keywords.begin(), keywords.begin() + static_cast<std::ptrdiff_t>(i), keyword) !=
                    keywords.begin() + static_cast<std::ptrdiff_t>(i)) return false;
        }
        return std::ranges::all_of(markers, [](const CatalogMarker& marker) { return marker.valid(); });
    }
    bool operator==(const CatalogLocation&) const = default;
};

}  // namespace synth::core
