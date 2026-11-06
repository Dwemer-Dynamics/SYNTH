#pragma once
#include "core/snapshot.hpp"
#include <map>
#include <span>
#include <algorithm>
#include <tuple>

namespace synth::actions {
// The menu may equip items, but must conserve each named item across the exact two inventories.
[[nodiscard]] inline bool gift_menu_exchange_conserved(
    std::span<const core::InventoryItemSnapshot> player_before,
    std::span<const core::InventoryItemSnapshot> npc_before,
    std::span<const core::InventoryItemSnapshot> player_after,
    std::span<const core::InventoryItemSnapshot> npc_after) {
    using Key=std::tuple<std::uint32_t,std::string_view,std::uint32_t,std::string_view>;
    std::map<Key,std::uint64_t> totals;
    for (auto rows:{player_before,npc_before}) {
        if(rows.size()>512)return false;
        for(const auto& item:rows) {
            if(!item.valid() || item.count>2147483647U)return false;
            totals[{item.form_id,item.origin_plugin,item.form_type,item.display_name}]+=item.count;
        }
    }
    for(auto rows:{player_after,npc_after}) {
        if(rows.size()>512)return false;
        for(const auto& item:rows) {
            if(!item.valid() || item.count>2147483647U)return false;
            auto found=totals.find({item.form_id,item.origin_plugin,item.form_type,item.display_name});
            if(found==totals.end() || found->second<item.count)return false;
            found->second-=item.count;
        }
    }
    return std::ranges::all_of(totals,[](const auto& row){return row.second==0;});
}
} // namespace synth::actions
