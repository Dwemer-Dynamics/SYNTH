#pragma once

#include "core/snapshot.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>

namespace synth::actions {

// Pickup selectors name a world reference, never an inventory base form or a nearest-name fallback.
[[nodiscard]] inline std::optional<core::NearbyItemSnapshot> select_pickup_item(
    std::span<const core::NearbyItemSnapshot> nearby, std::string_view observation,
    std::string_view selector) {
    if (nearby.size() > 32 || (observation != "complete" && observation != "partial") ||
        selector.size() < 12 || selector.size() > 255 ||
        !(selector.starts_with("0x") || selector.starts_with("0X")) || selector[10] != ':') return std::nullopt;
    std::uint32_t reference_id{};
    const auto parsed = std::from_chars(selector.data() + 2, selector.data() + 10, reference_id, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != selector.data() + 10 || reference_id == 0) return std::nullopt;
    const auto name = selector.substr(11);
    if (name.find_first_not_of(" \t\r\n\v\f") == std::string_view::npos) return std::nullopt;
    std::optional<core::NearbyItemSnapshot> selected;
    for (const auto& item : nearby) {
        if (item.reference_id != reference_id) continue;
        if (selected || !item.valid() || item.count > 2147483647U || item.held ||
            item.display_name != name) return std::nullopt;
        selected = item;
    }
    return selected;
}

// Position and player-relative presentation may change during approach; identity and the whole stack may not.
[[nodiscard]] inline bool pickup_reference_matches(const core::NearbyItemSnapshot& selected,
                                                  const core::NearbyItemSnapshot& current) noexcept {
    return selected.valid() && current.valid() && !selected.held && !current.held &&
        selected.count <= 2147483647U && selected.reference_id == current.reference_id &&
        selected.base_form_id == current.base_form_id && selected.cell_form_id == current.cell_form_id &&
        selected.reference_origin_plugin == current.reference_origin_plugin &&
        selected.base_origin_plugin == current.base_origin_plugin && selected.cell_origin_plugin == current.cell_origin_plugin &&
        selected.form_type == current.form_type && selected.display_name == current.display_name && selected.count == current.count;
}

// Missing/unloaded is not retired: the native adapter must positively observe the selected reference's retirement.
enum class PickupReferenceState : unsigned char { unavailable, present, retired };

// Require complete inventory conservation; ammo is a pre-transfer total delta, null only when verified absent.
[[nodiscard]] inline bool pickup_postcondition(
    std::span<const core::InventoryItemSnapshot> before,
    std::span<const core::InventoryItemSnapshot> after,
    const core::NearbyItemSnapshot& selected, PickupReferenceState reference_state,
    std::string_view before_observation, std::string_view after_observation,
    const std::optional<core::InventoryItemSnapshot>& expected_ammunition = std::nullopt) {
    if (!selected.valid() || selected.count > 2147483647U || selected.held ||
        reference_state != PickupReferenceState::retired || before.size() > 512 || after.size() > 512 ||
        before_observation != "complete" || after_observation != "complete") return false;
    if (expected_ammunition && (!expected_ammunition->valid() || selected.form_type != 0x2B ||
        expected_ammunition->form_type != 0x2C || expected_ammunition->equipped ||
        expected_ammunition->form_id == selected.base_form_id || expected_ammunition->count > 2147483647U)) return false;
    using ItemKey = std::tuple<std::uint32_t, std::string_view, std::uint32_t, std::string_view>;
    std::map<ItemKey, std::uint64_t> remaining;
    for (const auto& item : before) {
        if (!item.valid() || item.count > 2147483647U) return false;
        remaining[{item.form_id, item.origin_plugin, item.form_type, item.display_name}] += item.count;
    }
    remaining[{selected.base_form_id, selected.base_origin_plugin, selected.form_type, selected.display_name}] += selected.count;
    if (expected_ammunition) {
        const auto& ammo = *expected_ammunition;
        remaining[{ammo.form_id, ammo.origin_plugin, ammo.form_type, ammo.display_name}] += ammo.count;
    }
    for (const auto& item : after) {
        if (!item.valid() || item.count > 2147483647U) return false;
        const auto entry = remaining.find({item.form_id, item.origin_plugin, item.form_type, item.display_name});
        if (entry == remaining.end() || entry->second < item.count) return false;
        entry->second -= item.count;
    }
    // Native pickup can auto-equip weapons. Full post-action inventory must report equipment changes separately.
    return std::ranges::all_of(remaining, [](const auto& entry) { return entry.second == 0; });
}

// Select Fallout's base caps record by identity, never a localized label or a similarly named mod item.
[[nodiscard]] inline std::optional<core::InventoryItemSnapshot> select_caps_item(
    std::span<const core::InventoryItemSnapshot> inventory, std::string_view observation) {
    if (observation != "complete" && observation != "partial") return std::nullopt;
    std::optional<core::InventoryItemSnapshot> selected;
    for (const auto& item : inventory) {
        if (item.form_id != 0x0000000F) continue;
        if (selected || !item.valid() || item.origin_plugin != "Fallout4.esm" || item.form_type != 0x23 ||
            item.equipped || item.count > 2147483647U) return std::nullopt;
        selected = item;
    }
    return selected;
}

// Bind model text to one observed item; never use substring matches or invent an unobserved base ID.
[[nodiscard]] inline std::optional<core::InventoryItemSnapshot> select_equipment_item(
    std::span<const core::InventoryItemSnapshot> inventory, std::string_view observation,
    std::string_view selector) {
    if (selector.empty() || selector.size() > 255 ||
        (observation != "complete" && observation != "partial")) return std::nullopt;
    std::uint32_t form_id{};
    auto name = selector;
    auto id_text = selector.substr(0, selector.find(':'));
    const bool explicit_id = id_text.starts_with("0x") || id_text.starts_with("0X");
    if (explicit_id) id_text.remove_prefix(2);
    if (id_text.size() == 8) {
        const auto parsed = std::from_chars(id_text.data(), id_text.data() + id_text.size(), form_id, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != id_text.data() + id_text.size()) form_id = 0;
        else {
            if (form_id == 0) return std::nullopt;
            name = selector.find(':') == std::string_view::npos ? std::string_view{} : selector.substr(selector.find(':') + 1);
            if (selector.find(':') != std::string_view::npos && name.empty()) return std::nullopt;
        }
    }
    if (explicit_id && form_id == 0) return std::nullopt;
    // A partial list cannot establish that a name is unique across unseen base forms.
    if (form_id == 0 && observation != "complete") return std::nullopt;
    std::optional<core::InventoryItemSnapshot> selected;
    for (const auto& item : inventory) {
        if (!item.valid() || (form_id != 0 && item.form_id != form_id) ||
            (!name.empty() && item.display_name != name)) continue;
        if (selected) return std::nullopt;
        selected = item;
    }
    return selected;
}

// Equipment may split a stack; verify the named item's aggregate state and preserve its entire base count.
[[nodiscard]] inline bool equipment_postcondition(
    std::span<const core::InventoryItemSnapshot> before,
    std::span<const core::InventoryItemSnapshot> after,
    const core::InventoryItemSnapshot& selected, bool equip) {
    std::uint64_t before_total{}, after_total{}, before_named{}, after_named{}, equipped{};
    for (const auto& item : before) {
        if (!item.valid() || item.form_id != selected.form_id || item.origin_plugin != selected.origin_plugin) return false;
        before_total += item.count;
        if (item.display_name == selected.display_name) before_named += item.count;
    }
    for (const auto& item : after) {
        if (!item.valid() || item.form_id != selected.form_id || item.origin_plugin != selected.origin_plugin) return false;
        after_total += item.count;
        if (item.display_name == selected.display_name) {
            after_named += item.count;
            if (item.equipped) equipped += item.count;
        }
    }
    return before_total != 0 && before_total == after_total && before_named != 0 &&
           before_named == after_named && (equip ? equipped == 1 : equipped == 0);
}

// A consume receipt requires one observed ALCH item to disappear, with every other named stack preserved.
[[nodiscard]] inline bool consumption_postcondition(
    std::span<const core::InventoryItemSnapshot> before,
    std::span<const core::InventoryItemSnapshot> after,
    const core::InventoryItemSnapshot& selected) {
    if (!selected.valid() || selected.form_type != 0x30 || selected.equipped ||
        selected.count > 2147483647U || before.empty() || before.size() > 64 || after.size() > 64) return false;
    std::map<std::string_view, std::uint64_t> remaining;
    bool found{};
    for (const auto& item : before) {
        if (!item.valid() || item.form_id != selected.form_id || item.origin_plugin != selected.origin_plugin ||
            item.form_type != selected.form_type || item.equipped || item.count > 2147483647U) return false;
        if (item.display_name == selected.display_name) {
            if (found || item.count != selected.count) return false;
            found = true;
        }
        remaining[item.display_name] += item.count;
    }
    if (!found) return false;
    --remaining[selected.display_name];
    for (const auto& item : after) {
        if (!item.valid() || item.form_id != selected.form_id || item.origin_plugin != selected.origin_plugin ||
            item.form_type != selected.form_type || item.equipped || item.count > 2147483647U) return false;
        const auto entry = remaining.find(item.display_name);
        if (entry == remaining.end() || entry->second < item.count) return false;
        entry->second -= item.count;
    }
    for (const auto& [name, count] : remaining) {
        if (count != 0) return false;
    }
    return true;
}

// A transfer must conserve each named/equipped stack on both sides, not merely move a base-form total.
[[nodiscard]] inline bool transfer_postcondition(
    std::span<const core::InventoryItemSnapshot> donor_before,
    std::span<const core::InventoryItemSnapshot> donor_after,
    std::span<const core::InventoryItemSnapshot> recipient_before,
    std::span<const core::InventoryItemSnapshot> recipient_after,
    const core::InventoryItemSnapshot& selected, std::uint32_t amount) {
    if (!selected.valid() || selected.equipped || selected.count > 2147483647U ||
        amount == 0 || amount > 1000000 || amount > selected.count || donor_before.empty()) return false;
    using Key = std::pair<std::string_view, bool>;
    const auto verify_side = [&](std::span<const core::InventoryItemSnapshot> before,
                                 std::span<const core::InventoryItemSnapshot> after, bool donor) {
        if (before.size() > 64 || after.size() > 64) return false;
        std::map<Key, std::uint64_t> remaining;
        bool found{};
        for (const auto& item : before) {
            if (!item.valid() || item.form_id != selected.form_id || item.origin_plugin != selected.origin_plugin ||
                item.form_type != selected.form_type || item.count > 2147483647U) return false;
            if (donor && item.display_name == selected.display_name) {
                if (found || item.equipped || item.count != selected.count) return false;
                found = true;
            }
            remaining[{item.display_name, item.equipped}] += item.count;
        }
        const Key key{selected.display_name, false};
        if (donor) {
            if (!found) return false;
            remaining[key] -= amount;
        } else remaining[key] += amount;
        for (const auto& item : after) {
            if (!item.valid() || item.form_id != selected.form_id || item.origin_plugin != selected.origin_plugin ||
                item.form_type != selected.form_type || item.count > 2147483647U) return false;
            const auto entry = remaining.find({item.display_name, item.equipped});
            if (entry == remaining.end() || entry->second < item.count) return false;
            entry->second -= item.count;
        }
        return std::ranges::all_of(remaining, [](const auto& entry) { return entry.second == 0; });
    };
    return verify_side(donor_before, donor_after, true) && verify_side(recipient_before, recipient_after, false);
}

} // namespace synth::actions
