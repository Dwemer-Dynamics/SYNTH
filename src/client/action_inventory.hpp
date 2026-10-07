#pragma once

#include "protocol_native/v1_codec.hpp"
#include "runtime/fallout_runtime.hpp"
#include "actions/equipment.hpp"
#include <charconv>
#include <cstdio>
#include <memory>
#include <utility>

namespace synth::client {

// Freeze the exact NPC and world reference from this turn; the native adapter must revalidate before approach/transfer.
[[nodiscard]] inline runtime::RuntimePickupRequest prepare_pickup_action(const core::RuntimeSnapshot& snapshot,
    const protocol_native::Action& action, const core::CancellationToken& cancellation, core::SnapshotClock::time_point deadline) {
    if (snapshot.game() != core::Game::fallout4 || snapshot.variant() != core::RuntimeVariant::flat ||
        cancellation.is_cancelled() || cancellation.generation() != snapshot.generation() || snapshot.frame() == 0 ||
        deadline <= snapshot.captured_at() || action.name != "pickup_item" || action.capability != "action.pickup_item" ||
        action.action_id.empty() || action.action_id.size() > 128 || action.target ||
        !action.arguments.is_object() || action.arguments.as_object().size() != 1)
        throw std::invalid_argument{"invalid pickup owner, deadline or arguments"};
    (void)protocol_native::identity(protocol_native::identity_value(action.actor));
    std::uint32_t form_id{};
    if (action.actor.form_id.size() != 10 || !action.actor.form_id.starts_with("0x"))
        throw std::invalid_argument{"invalid pickup actor FormID"};
    const auto parsed = std::from_chars(action.actor.form_id.data() + 2, action.actor.form_id.data() + 10, form_id, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != action.actor.form_id.data() + 10 || form_id == 0 ||
        form_id == 0x14 || form_id == snapshot.player().form_id() ||
        action.actor.playthrough_id != snapshot.player().playthrough_id())
        throw std::invalid_argument{"pickup requires an NPC in this playthrough"};
    const core::ActorSnapshot* performer{};
    for (const auto& actor : snapshot.actors()) {
        if (actor.form_id() != form_id) continue;
        if (performer || actor.origin_plugin() != action.actor.origin_plugin || actor.playthrough_id() != action.actor.playthrough_id)
            throw std::invalid_argument{"ambiguous or mismatched pickup actor"};
        performer = &actor;
    }
    if (!performer) throw std::invalid_argument{"pickup actor is absent from the captured scene"};
    const auto* selector = action.arguments.find("item");
    const auto selected = selector && selector->is_string() ? actions::select_pickup_item(
        snapshot.nearby_items(), snapshot.nearby_items_observation(), selector->as_string()) : std::nullopt;
    if (!selected) throw std::invalid_argument{"pickup requires an exact freshly observed RefID:ItemName"};
    return {{performer->form_id(), performer->origin_plugin(), performer->playthrough_id()}, *selected,
        cancellation, deadline, snapshot.captured_at(), snapshot.frame(), action.action_id};
}

struct ActionInventoryContinuation final {
    std::shared_ptr<const core::RuntimeSnapshot> snapshot;
    protocol_native::ActionInventoryObservation inventory;
};

// Resolve only this captured scene; display labels, mutable targets and historical registries confer no ownership.
[[nodiscard]] inline std::pair<const core::ActorSnapshot*, const core::ActorSnapshot*> transfer_actors(
    const core::RuntimeSnapshot& snapshot, const protocol_native::Action& action, const core::CancellationToken& cancellation) {
    if (snapshot.variant()!=core::RuntimeVariant::flat || cancellation.is_cancelled() || snapshot.generation()!=cancellation.generation() ||
        !((action.name=="give_item_to" && action.capability=="action.give_item_to") ||
          (action.name=="give_caps_to" && action.capability=="action.give_caps_to") ||
          (action.name=="take_caps_from_player" && action.capability=="action.take_caps_from_player")) || !action.target || action.action_id.empty())
        throw std::invalid_argument{"invalid transfer continuation owner"};
    const auto resolve = [&](const protocol_native::Identity& identity) {
        (void)protocol_native::identity(protocol_native::identity_value(identity));
        std::uint32_t form_id{};
        if (identity.form_id.size()!=10 || !identity.form_id.starts_with("0x"))
            throw std::invalid_argument{"invalid transfer actor FormID"};
        const auto parsed=std::from_chars(identity.form_id.data()+2,identity.form_id.data()+10,form_id,16);
        if (parsed.ec!=std::errc{} || parsed.ptr!=identity.form_id.data()+10 || form_id==0 ||
            identity.playthrough_id!=snapshot.player().playthrough_id()) throw std::invalid_argument{"invalid transfer actor identity"};
        const core::ActorSnapshot* found=nullptr;
        const auto consider=[&](const core::ActorSnapshot& actor) {
            if (actor.form_id()!=form_id) return;
            if (found || actor.origin_plugin()!=identity.origin_plugin || actor.playthrough_id()!=identity.playthrough_id)
                throw std::invalid_argument{"ambiguous or mismatched transfer actor"};
            found=&actor;
        };
        consider(snapshot.player());
        for (const auto& actor:snapshot.actors()) consider(actor);
        if (!found) throw std::invalid_argument{"transfer actor is absent from the captured scene"};
        return found;
    };
    const auto* performer=resolve(action.actor);
    const auto* target=resolve(*action.target);
    const bool taking=action.name=="take_caps_from_player";
    if (performer==&snapshot.player() || performer->form_id()==target->form_id() ||
        (taking && (target!=&snapshot.player() || target->form_id()!=0x14)))
        throw std::invalid_argument{"transfer requires an NPC performer and the exact distinct inventory target"};
    return taking ? std::pair{target,performer} : std::pair{performer,target};
}

// Validate model arguments and select exactly one observed stack before scheduling any native work.
[[nodiscard]] inline runtime::RuntimeActionRequest prepare_transfer_action(const core::RuntimeSnapshot& snapshot,
    const protocol_native::Action& action, const core::CancellationToken& cancellation, core::SnapshotClock::time_point deadline) {
    const auto [donor,recipient]=transfer_actors(snapshot,action,cancellation);
    if (!action.arguments.is_object()) throw std::invalid_argument{"transfer arguments must be an object"};
    const auto* item=action.arguments.find("item");
    const auto* amount=action.arguments.find("amount");
    const bool taking=action.name=="take_caps_from_player";
    const bool caps=taking || action.name=="give_caps_to";
    if (caps ? (!amount || action.arguments.as_object().size()!=1) :
        (!item || !item->is_string() || action.arguments.as_object().size()!=(amount?2:1)))
        throw std::invalid_argument{"caps require only an integer amount; items require item and optional integer amount"};
    std::uint64_t quantity=1;
    if (amount) {
        if (const auto* value=std::get_if<std::uint64_t>(&amount->storage())) quantity=*value;
        else if (const auto* signed_value=std::get_if<std::int64_t>(&amount->storage()); signed_value && *signed_value>0) quantity=static_cast<std::uint64_t>(*signed_value);
        else throw std::invalid_argument{"transfer amount must be a positive integer"};
    }
    const auto selected=caps ? actions::select_caps_item(donor->inventory(),donor->inventory_observation()) :
        actions::select_equipment_item(donor->inventory(),donor->inventory_observation(),item->as_string());
    if (!selected || selected->equipped || quantity==0 || quantity>1'000'000 || quantity>selected->count)
        throw std::invalid_argument{"transfer item or quantity is unavailable in the captured inventory"};
    return {taking ? runtime::RuntimeActionName::take_caps : caps ? runtime::RuntimeActionName::give_caps : runtime::RuntimeActionName::give_item,
        {donor->form_id(),donor->origin_plugin(),donor->playthrough_id()},
        selected,cancellation,deadline,action.action_id,
        runtime::RuntimeActorIdentity{recipient->form_id(),recipient->origin_plugin(),recipient->playthrough_id()},static_cast<std::uint32_t>(quantity)};
}

struct ActionTransferContinuation final {
    std::shared_ptr<const core::RuntimeSnapshot> snapshot;
    protocol_native::ActionTransferObservation transfer;
};

// Prepare both owned observations before publishing either; wire and local continuations share exactly these rows.
[[nodiscard]] inline ActionTransferContinuation prepare_transfer_inventory(
    const std::shared_ptr<const core::RuntimeSnapshot>& snapshot, const protocol_native::Action& action,
    const core::CancellationToken& cancellation, const std::optional<runtime::RuntimeTransferObservation>& observed, bool extended) {
    if (!snapshot) throw std::invalid_argument{"transfer snapshot is absent"};
    const auto [donor,recipient]=transfer_actors(*snapshot,action,cancellation);
    const bool taking=action.name=="take_caps_from_player";
    runtime::RuntimeActionRequest owner{taking ? runtime::RuntimeActionName::take_caps :
        action.name=="give_caps_to" ? runtime::RuntimeActionName::give_caps : runtime::RuntimeActionName::give_item,
        {donor->form_id(),donor->origin_plugin(),donor->playthrough_id()},std::nullopt,cancellation,{},action.action_id,
        runtime::RuntimeActorIdentity{recipient->form_id(),recipient->origin_plugin(),recipient->playthrough_id()}};
    const bool owned=observed && observed->valid_for(owner);
    std::vector<core::InventoryItemSnapshot> donor_items,recipient_items;
    ActionTransferContinuation prepared{snapshot,{{taking?*action.target:action.actor,{},"unavailable"},
        {taking?action.actor:*action.target,{},"unavailable"}}};
    const auto copy=[&](const runtime::RuntimeInventoryObservation* source, std::vector<core::InventoryItemSnapshot>& items,
                        protocol_native::ActionInventoryObservation& wire) {
        if (!source) return;
        items=source->items;wire.observation=source->observation;
        const std::size_t limit=extended?512:32;
        if (items.size()>limit) {items.resize(limit);wire.observation="partial";}
        for (const auto& item:items) {
            char id[11]{};std::snprintf(id,sizeof(id),"0x%08X",item.form_id);
            wire.items.push_back({id,item.origin_plugin,item.display_name,item.count,item.form_type,item.value,item.weight,item.equipped});
        }
        // A malformed native string must become unknown, not prevent delivery of the other actor's evidence.
        try { (void)json::write(protocol_native::action_inventory_value(wire,extended),json::Limits{.maximum_bytes=4*1024*1024}); }
        catch (const std::invalid_argument&) {items.clear();wire.items.clear();wire.observation="unavailable";}
        catch (const json::Error&) {items.clear();wire.items.clear();wire.observation="unavailable";}
    };
    copy(owned?&observed->donor:nullptr,donor_items,prepared.transfer.donor);
    copy(owned?&observed->recipient:nullptr,recipient_items,prepared.transfer.recipient);
    // Reserve 64KiB for the bounded event envelope, capabilities, IDs, status/detail and newline.
    // Halving the larger side bounds this loop to at most 20 reductions even at 512 rows per actor.
    for (;;) {
        if (cancellation.is_cancelled()) throw std::invalid_argument{"transfer continuation was cancelled"};
        try {
            (void)json::write(json::Value{protocol_native::object({
                {"inventory",protocol_native::action_inventory_value(prepared.transfer.donor,extended)},
                {"recipient_inventory",protocol_native::action_inventory_value(prepared.transfer.recipient,extended)}})},
                json::Limits{.maximum_bytes=960*1024});
            break;
        } catch (const json::ByteLimitError&) {
            auto& wire=prepared.transfer.donor.items.size()>=prepared.transfer.recipient.items.size()
                ? prepared.transfer.donor : prepared.transfer.recipient;
            if (wire.items.empty()) throw;
            wire.items.resize(wire.items.size()/2);wire.observation="partial";
        }
    }
    donor_items.resize(prepared.transfer.donor.items.size());recipient_items.resize(prepared.transfer.recipient.items.size());
    auto next=donor==&snapshot->player()
        ? snapshot->with_player_inventory(snapshot->player().with_inventory(std::move(donor_items),prepared.transfer.donor.observation))
        : snapshot->with_actor_inventory(donor->form_id(),donor->origin_plugin(),donor->playthrough_id(),
            std::move(donor_items),prepared.transfer.donor.observation);
    if (recipient==&snapshot->player()) next=next.with_player_inventory(snapshot->player().with_inventory(
        std::move(recipient_items),prepared.transfer.recipient.observation));
    else next=next.with_actor_inventory(recipient->form_id(),recipient->origin_plugin(),recipient->playthrough_id(),
        std::move(recipient_items),prepared.transfer.recipient.observation);
    if (cancellation.is_cancelled()) throw std::invalid_argument{"transfer continuation was cancelled"};
    prepared.snapshot=std::make_shared<const core::RuntimeSnapshot>(std::move(next));
    return prepared;
}

// The receipt and local continuation use the same copied rows; missing or mismatched evidence means unknown.
[[nodiscard]] inline ActionInventoryContinuation prepare_action_inventory(
    const std::shared_ptr<const core::RuntimeSnapshot>& snapshot, const protocol_native::Action& action,
    const core::CancellationToken& cancellation, const std::optional<runtime::RuntimeInventoryObservation>& observed,
    bool extended) {
    if (!snapshot || cancellation.is_cancelled() || snapshot->generation()!=cancellation.generation() ||
        (action.name!="equip_item" && action.name!="unequip_item" && action.name!="consume") || action.capability!="action."+action.name ||
        action.actor.form_id.size()!=10 || !action.actor.form_id.starts_with("0x"))
        throw std::invalid_argument{"invalid inventory continuation owner"};
    std::uint32_t form_id{};
    const auto parsed=std::from_chars(action.actor.form_id.data()+2,action.actor.form_id.data()+10,form_id,16);
    if (parsed.ec!=std::errc{} || parsed.ptr!=action.actor.form_id.data()+10 || form_id==0)
        throw std::invalid_argument{"invalid inventory continuation actor"};
    runtime::RuntimeActionRequest request{action.name=="consume" ? runtime::RuntimeActionName::consume :
        action.name=="equip_item" ? runtime::RuntimeActionName::equip_item : runtime::RuntimeActionName::unequip_item,
        {form_id,action.actor.origin_plugin,action.actor.playthrough_id},std::nullopt,cancellation,{},action.action_id};
    std::vector<core::InventoryItemSnapshot> items;
    std::string quality="unavailable";
    if (observed && observed->valid_for(request)) {items=observed->items;quality=observed->observation;}
    if (!std::ranges::all_of(items,[](const auto& item) {
        return item.count<=2147483647 && item.value>=0 &&
            item.display_name.find_first_not_of(" \t\r\n\v\f")!=std::string::npos;
    })) {items.clear();quality="unavailable";}
    const std::size_t limit=extended && snapshot->variant()==core::RuntimeVariant::flat ? 512 : 32;
    if (items.size()>limit) {items.resize(limit);quality="partial";}
    ActionInventoryContinuation result{std::make_shared<const core::RuntimeSnapshot>(snapshot->with_actor_inventory(
        form_id,action.actor.origin_plugin,action.actor.playthrough_id,items,quality)),{action.actor,{},quality}};
    for (const auto& item:items) {
        char id[11]{};std::snprintf(id,sizeof(id),"0x%08X",item.form_id);
        result.inventory.items.push_back({id,item.origin_plugin,item.display_name,static_cast<std::uint64_t>(item.count),
            static_cast<std::uint64_t>(item.form_type),item.value,item.weight,item.equipped});
    }
    return result;
}

} // namespace synth::client
