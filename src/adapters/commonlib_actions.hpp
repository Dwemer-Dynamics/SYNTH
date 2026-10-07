#pragma once

#include "adapters/commonlib_capture.hpp"
#include "actions/equipment.hpp"
#include "runtime/fallout_runtime.hpp"
#if !defined(SYNTH_WITH_F4SEVR)
#include "adapters/flat_wait_package.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <string_view>

namespace synth::adapters::commonlib {

#if !defined(SYNTH_WITH_F4SEVR)
struct EquipmentInventory final {
    struct Stack final {
        RE::BSTSmartPointer<RE::BGSInventoryItem::Stack> retained;
        RE::BSTSmartPointer<RE::ExtraDataList> extra;
        std::uint32_t index{};
    };
    std::vector<Stack> stacks;
    std::vector<core::InventoryItemSnapshot> items;
};

// Targeted complete base-item read: bounded list/stack traversal, no engine getters under the inventory lock.
[[nodiscard]] inline std::optional<EquipmentInventory> equipment_inventory(
    RE::Actor& actor, RE::TESBoundObject& object) {
    if (!actor.inventoryList) return std::nullopt;
    const auto started = core::SnapshotClock::now();
    EquipmentInventory result;
    result.stacks.reserve(64);
    result.items.reserve(64);
    {
        const TryReadLock lock{actor.inventoryList->rwLock};
        if (!lock.owns_lock()) return std::nullopt;
        std::size_t inspected{};
        bool found{};
        for (const auto& item : actor.inventoryList->data) {
            if (++inspected > maximum_inventory_entries ||
                core::SnapshotClock::now() - started >= std::chrono::microseconds{500}) return std::nullopt;
            if (item.object != &object) continue;
            if (found || !item.stackData) return std::nullopt;
            found = true;
            std::uint32_t index{};
            for (auto stack = item.stackData; stack; stack = stack->nextStack, ++index) {
                if (index >= 64 || core::SnapshotClock::now() - started >= std::chrono::microseconds{500}) return std::nullopt;
                const auto count = stack->GetCount();
                if (count > 2147483647U) return std::nullopt;
                if (count == 0) continue;
                result.stacks.push_back({stack, stack->extra, index});
                result.items.push_back({0, {}, {}, count, 0, 0.0, 0, stack->IsEquipped()});
            }
        }
    }
    const auto plugin = origin_plugin(object);
    if (plugin.empty()) return std::nullopt;
    for (std::size_t index = 0; index < result.stacks.size(); ++index) {
        if (core::SnapshotClock::now() - started >= std::chrono::milliseconds{2}) return std::nullopt;
        const auto metadata = inventory_metadata(object, result.stacks[index].extra);
        if (!metadata) return std::nullopt;
        auto& item = result.items[index];
        item.form_id = object.GetFormID();
        item.origin_plugin = plugin;
        item.display_name = metadata->name;
        item.form_type = static_cast<std::uint16_t>(object.GetFormType());
        if (!item.valid()) return std::nullopt;
    }
    return result;
}
#endif

[[nodiscard]] inline bool plugin_name_equal(std::string_view left,
                                            std::string_view right) noexcept {
    return std::ranges::equal(left, right, [](unsigned char lhs, unsigned char rhs) {
        return std::tolower(lhs) == std::tolower(rhs);
    });
}

// Resolves and mutates only the exact live actor represented by the canonical protocol identity.
[[nodiscard]] inline runtime::RuntimeActionResult execute_action(
    const runtime::RuntimeActionRequest& request) {
    const auto* manager = RE::BGSSaveLoadManager::GetSingleton();
    if (manager == nullptr || manager->currentPlayerID == 0) {
        return {runtime::RuntimeActionStatus::unavailable,
                "Fallout 4 has not published a stable playthrough identity"};
    }
    if (request.actor.playthrough_id != playthrough_id()) {
        return {runtime::RuntimeActionStatus::unavailable,
                "action actor belongs to a different playthrough"};
    }

    auto* actor = RE::TESForm::GetFormByID<RE::Actor>(request.actor.form_id);
    if (actor == nullptr) {
        return {runtime::RuntimeActionStatus::unavailable,
                "canonical actor is not loaded"};
    }
    auto plugin = origin_plugin(*actor);
    if (plugin.empty() && actor->GetFormID() == 0x14) plugin = "Fallout4.esm";
    if (!plugin_name_equal(plugin, request.actor.origin_plugin)) {
        return {runtime::RuntimeActionStatus::unavailable,
                "loaded form does not match the canonical actor plugin"};
    }

    switch (request.name) {
    case runtime::RuntimeActionName::wait_here:
    case runtime::RuntimeActionName::release_wait: {
#if defined(SYNTH_WITH_F4SEVR)
        return {runtime::RuntimeActionStatus::unsupported_runtime, "Wait Here requires the flat wait-package adapter"};
#else
        return FlatWaitPackage::apply(*actor, request);
#endif
    }
    case runtime::RuntimeActionName::take_caps:
    case runtime::RuntimeActionName::give_caps:
    case runtime::RuntimeActionName::give_item: {
#if defined(SYNTH_WITH_F4SEVR)
        return {runtime::RuntimeActionStatus::unsupported_runtime,
                "VR inventory transfers are not implemented; capabilities are disabled"};
#else
        using Status = runtime::RuntimeActionStatus;
        const auto epoch = FlatPickedReference::stamp();
        auto* player = RE::PlayerCharacter::GetSingleton();
        const bool taking = request.name == runtime::RuntimeActionName::take_caps;
        if (!player || (taking ? actor != player : actor == player) || !request.recipient || !request.item || !request.item->valid() ||
            request.action_id.empty() || request.action_id.size() > 128 || request.amount == 0 ||
            request.amount > 1000000 || request.amount > request.item->count ||
            request.recipient->playthrough_id != request.actor.playthrough_id || request.item->equipped ||
            request.cancellation.is_cancelled() || menu_mode_active())
            return {Status::unavailable, "transfer requires its exact donor role, item, amount and distinct recipient"};
        auto* recipient = RE::TESForm::GetFormByID<RE::Actor>(request.recipient->form_id);
        if (!recipient || recipient == actor)
            return {Status::rejected, "transfer recipient is missing or is the donor"};
        auto recipient_plugin = origin_plugin(*recipient);
        if (recipient_plugin.empty() && recipient == player) recipient_plugin = "Fallout4.esm";
        if (!plugin_name_equal(recipient_plugin, request.recipient->origin_plugin))
            return {Status::rejected, "transfer recipient does not match its canonical plugin"};
        for (auto* participant : {actor, recipient}) {
            if (participant->IsDeleted() || participant->IsDisabled() || participant->IsDead(false) ||
                !participant->Get3D() || !participant->currentProcess || !participant->inventoryList ||
                !participant->GetParentCell() || participant->GetParentCell() != player->GetParentCell())
                return {Status::unavailable, "both transfer actors must be loaded in the player's current cell"};
            if (participant->lifeState != 0 || participant->knockState != 0 || participant->inSyncAnim ||
                participant->IsInCombat() || participant->DoGetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal ||
                (participant->niFlags.flags & static_cast<std::uint32_t>(RE::Actor::BOOL_FLAGS::kScenePackage)) != 0)
                return {Status::rejected, "transfer actor is busy, incapacitated or in a scene"};
        }
        auto* object = RE::TESForm::GetFormByID<RE::TESBoundObject>(request.item->form_id);
        if (!object || !plugin_name_equal(origin_plugin(*object), request.item->origin_plugin) ||
            static_cast<std::uint16_t>(object->GetFormType()) != request.item->form_type ||
            !(object->Is(RE::ENUM_FORM_ID::kWEAP) || object->Is(RE::ENUM_FORM_ID::kARMO) ||
              object->Is(RE::ENUM_FORM_ID::kAMMO) || object->Is(RE::ENUM_FORM_ID::kMISC) ||
              object->Is(RE::ENUM_FORM_ID::kALCH) || object->Is(RE::ENUM_FORM_ID::kBOOK) || object->Is(RE::ENUM_FORM_ID::kKEYM)))
            return {Status::rejected, "transfer requires an exact observed inventory base form"};
        if (request.name == runtime::RuntimeActionName::give_caps || taking) {
            // Exact 1.11.240: CommonLib's getter signature and initialization-array offset are stale.
            // Read only the initialized currency slot; never invoke its lazy constructor. See CAPS-CURRENCY-AUDIT.md.
            const auto module = REX::FModule::GetExecutingModule();
            static REL::Relocation<std::uintptr_t> defaults{REL::ID(4796209)};
            if (module.GetFileVersion().pack() != REL::Version{1, 11, 240, 0}.pack() ||
                defaults.address() != module.GetBaseAddress() + 0x30E78E0)
                return {Status::unavailable, "currency observation is unsupported on this runtime"};
            const auto* bytes = reinterpret_cast<const unsigned char*>(defaults.address());
            std::uintptr_t vtable{};
            std::memcpy(&vtable, bytes, sizeof(vtable));
            if (vtable != module.GetBaseAddress() + 0x24AAF98 || bytes[0xC83] != 1)
                return {Status::unavailable, "Fallout currency object is not initialized"};
            RE::TESForm* currency{};
            std::memcpy(&currency, bytes + 0x38, sizeof(currency));
            if (currency != object ||
                object->GetFormID() != 0x0000000F || !object->Is(RE::ENUM_FORM_ID::kMISC) ||
                !plugin_name_equal(origin_plugin(*object), "Fallout4.esm"))
                return {Status::rejected, "caps transfer does not match the live Fallout currency object"};
        }
        const auto before = equipment_inventory(*actor, *object);
        const auto recipient_before = equipment_inventory(*recipient, *object);
        if (!before || !recipient_before)
            return {Status::unavailable, "both transfer inventories must be completely readable without waiting"};
        std::optional<std::size_t> selected;
        for (std::size_t index = 0; index < before->items.size(); ++index) {
            if (before->items[index].display_name != request.item->display_name) continue;
            if (selected) return {Status::rejected, "transfer item matches multiple live instances"};
            selected = index;
        }
        if (!selected) return {Status::rejected, "transfer item is no longer in the donor inventory"};
        const auto& item = before->items[*selected];
        const auto& stack = before->stacks[*selected];
        if (item.equipped || request.amount > item.count)
            return {Status::rejected, "transfer cannot remove equipped items or more than the live stack"};
        const auto metadata = inventory_metadata(*object, stack.extra);
        if (!metadata || metadata->name != item.display_name || !object->GetPlayable(metadata->instance.get()))
            return {Status::rejected, "transfer instance is unavailable or not playable"};
        if (stack.extra) {
            const TryReadLock lock{stack.extra->extraRWLock};
            if (!lock.owns_lock()) return {Status::unavailable, "transfer extra data is busy"};
            if (stack.extra->extraData.HasType(RE::ExtraAliasInstanceArray::TYPE))
                return {Status::rejected, "quest-alias inventory cannot be transferred by SYNTH"};
        }
        {
            const TryReadLock lock{actor->inventoryList->rwLock};
            if (!lock.owns_lock()) return {Status::unavailable, "transfer donor inventory became busy"};
            bool current{};
            std::size_t inspected{};
            for (const auto& entry : actor->inventoryList->data) {
                if (++inspected > maximum_inventory_entries) break;
                if (entry.object != object) continue;
                auto candidate = entry.stackData;
                for (std::uint32_t index = 0; candidate && index < stack.index; ++index) candidate = candidate->nextStack;
                current = candidate == stack.retained && candidate && candidate->extra == stack.extra &&
                    candidate->GetCount() == item.count && !candidate->IsEquipped();
                break;
            }
            if (!current) return {Status::unavailable, "transfer stack changed before execution"};
        }
        // The normal RemoveItem virtual owns native notifications and preserves instance metadata.
        // Exactly one stack is selected; never emulate a transfer with separate remove/add calls.
        RE::TESObjectREFR::RemoveItemData remove{object, static_cast<std::int32_t>(request.amount)};
        remove.stackData.push_back(stack.index);
        remove.reason = RE::ITEM_REMOVE_REASON::kStoreContainer;
        remove.otherContainer = recipient;
        if (request.cancellation.is_cancelled() || epoch != FlatPickedReference::stamp() || menu_mode_active())
            return {Status::unavailable, "transfer turn or native load boundary changed"};
        if (core::SnapshotClock::now() >= request.deadline)
            return {Status::timed_out, "transfer expired before mutation"};
        runtime::RuntimeActionResult result{Status::failed, "transfer outcome is unconfirmed"};
        try {
            (void)actor->RemoveItem(remove);
            // Native callbacks may cross a load/menu boundary. Recheck before each dereference and publication.
            const auto after = epoch == FlatPickedReference::stamp() && !menu_mode_active()
                ? equipment_inventory(*actor, *object) : std::nullopt;
            const auto recipient_after = epoch == FlatPickedReference::stamp() && !menu_mode_active()
                ? equipment_inventory(*recipient, *object) : std::nullopt;
            if (epoch == FlatPickedReference::stamp() && !menu_mode_active() &&
                after && recipient_after && actions::transfer_postcondition(before->items, after->items,
                    recipient_before->items, recipient_after->items, item, request.amount))
                result = {Status::succeeded, "exact item count transferred; donor and recipient inventories verified"};
        } catch (...) {
            // An engine exception or failed postcondition may follow a partial mutation; observe both actors.
        }
        result.transfer.emplace(runtime::RuntimeTransferObservation{
            {request.cancellation.generation(), request.actor, {}, "unavailable", request.action_id},
            {request.cancellation.generation(), *request.recipient, {}, "unavailable", request.action_id}});
        for (auto [participant, observation] : {
                std::pair{actor, &result.transfer->donor}, std::pair{recipient, &result.transfer->recipient}}) {
            if (request.cancellation.is_cancelled() || epoch != FlatPickedReference::stamp() || menu_mode_active()) break;
            try {
                auto [items, quality] = inventory_snapshot(*participant);
                if (!request.cancellation.is_cancelled() && epoch == FlatPickedReference::stamp() && !menu_mode_active()) {
                    observation->items = std::move(items);
                    observation->observation = std::move(quality);
                }
            } catch (...) {
                // One actor's unavailable observation must not prevent an independent attempt for the other.
            }
        }
        return result;
#endif
    }
    case runtime::RuntimeActionName::equip_item:
    case runtime::RuntimeActionName::unequip_item:
    case runtime::RuntimeActionName::consume: {
#if defined(SYNTH_WITH_F4SEVR)
        return {runtime::RuntimeActionStatus::unsupported_runtime,
                "VR inventory mutations are not implemented; capabilities are disabled"};
#else
        using Status = runtime::RuntimeActionStatus;
        const bool consume = request.name == runtime::RuntimeActionName::consume;
        const auto epoch = FlatPickedReference::stamp();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!request.item || !request.item->valid() || request.action_id.empty() || request.action_id.size() > 128 || !player || actor == player ||
            actor->IsDeleted() || actor->IsDisabled() || actor->IsDead(false) || !actor->Get3D() ||
            !actor->currentProcess || menu_mode_active() || request.cancellation.is_cancelled())
            return {Status::unavailable, "inventory action actor, item or turn is unavailable"};
        auto* actor_cell = actor->GetParentCell();
        auto* player_cell = player->GetParentCell();
        if (!actor_cell || !player_cell || (actor_cell != player_cell &&
            (actor_cell->IsInterior() || player_cell->IsInterior() || !actor_cell->worldSpace ||
             actor_cell->worldSpace != player_cell->worldSpace)))
            return {Status::unavailable, "inventory action actor has left the current scene"};
        if (actor->lifeState != 0 || actor->knockState != 0 || actor->inSyncAnim ||
            actor->IsInCombat() || actor->DoGetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal ||
            (actor->niFlags.flags & static_cast<std::uint32_t>(RE::Actor::BOOL_FLAGS::kScenePackage)) != 0)
            return {Status::rejected, "inventory action actor is busy, incapacitated or in a scene"};
        auto* object = RE::TESForm::GetFormByID<RE::TESBoundObject>(request.item->form_id);
        if (!object || !plugin_name_equal(origin_plugin(*object), request.item->origin_plugin))
            return {Status::rejected, "inventory item does not match the observed base form"};
        if (consume ? (!object->Is(RE::ENUM_FORM_ID::kALCH) || request.item->form_type != 0x30) :
            (!object->Is(RE::ENUM_FORM_ID::kWEAP) && !object->Is(RE::ENUM_FORM_ID::kARMO)))
            return {Status::rejected, consume ? "consumption requires an observed ALCH consumable" : "equipment requires a weapon or armor"};
        const auto before = equipment_inventory(*actor, *object);
        if (!before) return {Status::unavailable, "action inventory could not be read completely without waiting"};
        // Native actions can also affect other base items. Capture the whole inventory, not only the requested base.
        // Failure to observe is explicit and never changes the already determined execution outcome.
        const auto finish = [&](runtime::RuntimeActionResult result) {
            result.inventory.emplace(runtime::RuntimeInventoryObservation{
                request.cancellation.generation(), request.actor, {}, "unavailable", request.action_id});
            if (request.cancellation.is_cancelled() || epoch != FlatPickedReference::stamp() || menu_mode_active()) return result;
            try {
                auto [items, observation] = inventory_snapshot(*actor);
                if (!request.cancellation.is_cancelled() && epoch == FlatPickedReference::stamp() && !menu_mode_active()) {
                    result.inventory->items = std::move(items);
                    result.inventory->observation = std::move(observation);
                }
            } catch (...) {
                // The mutation's result must still be delivered if optional metadata capture fails.
            }
            return result;
        };
        std::optional<std::size_t> selected;
        for (std::size_t index = 0; index < before->items.size(); ++index) {
            if (before->items[index].display_name != request.item->display_name) continue;
            if (selected) return {Status::rejected, "inventory item matches multiple live instances"};
            selected = index;
        }
        if (!selected) return {Status::rejected, "observed item is no longer in the actor inventory"};
        const auto& item = before->items[*selected];
        const auto& stack = before->stacks[*selected];
        const bool equip = request.name == runtime::RuntimeActionName::equip_item;
        if (consume && item.equipped) return {Status::rejected, "equipped items cannot be consumed"};
        if (!consume && item.equipped) {
            if (equip) return finish({Status::succeeded, "item is already equipped; live state verified"});
        } else if (!consume && !equip) return {Status::rejected, "item is not currently equipped"};
        const auto metadata = inventory_metadata(*object, stack.extra);
        if (!metadata || metadata->name != item.display_name)
            return {Status::unavailable, "inventory instance is unavailable"};
        RE::ActorEquipManager* equipment{};
        std::optional<RE::BGSObjectInstance> instance;
        RE::AlchemyItem* potion{};
        if (consume) {
            potion = object->As<RE::AlchemyItem>();
            if (!potion || metadata->instance || potion->listOfEffects.size() > 128 ||
                (!potion->listOfEffects.empty() && potion->listOfEffects.data() == nullptr))
                return {Status::unavailable, "consumable base effects are unavailable or unsupported"};
            for (const auto* effect : potion->listOfEffects) {
                if (!effect || !effect->effectSetting)
                    return {Status::unavailable, "consumable has an unavailable effect"};
            }
            // Do not apply effects before discovering that a quest-owned stack cannot safely be removed.
            if (stack.extra) {
                const TryReadLock lock{stack.extra->extraRWLock};
                if (!lock.owns_lock()) return {Status::unavailable, "consumable extra data became busy"};
                if (stack.extra->extraData.HasType(RE::ExtraAliasInstanceArray::TYPE))
                    return {Status::rejected, "quest-alias inventory cannot be consumed by SYNTH"};
            }
        } else {
            equipment = RE::ActorEquipManager::GetSingleton();
            if (!equipment) return {Status::unavailable, "equipment manager is unavailable"};
            // The native constructor owns its instance. Never smart-own an embedded base instance.
            instance.emplace(object, metadata->instance.get());
        }
        if (!actor->inventoryList || request.cancellation.is_cancelled() || epoch != FlatPickedReference::stamp())
            return {Status::unavailable, "inventory or load boundary changed during instance preparation"};
        {
            // Stack IDs are positions, not form IDs. Revalidate the retained stack before releasing this lock.
            const TryReadLock lock{actor->inventoryList->rwLock};
            if (!lock.owns_lock()) return {Status::unavailable, "action inventory became busy"};
            bool current{};
            std::size_t inspected{};
            for (const auto& entry : actor->inventoryList->data) {
                if (++inspected > maximum_inventory_entries) break;
                if (entry.object != object) continue;
                auto candidate = entry.stackData;
                for (std::uint32_t index = 0; candidate && index < stack.index; ++index) candidate = candidate->nextStack;
                current = candidate == stack.retained && candidate && candidate->extra == stack.extra &&
                    candidate->GetCount() == item.count && candidate->IsEquipped() == item.equipped;
                break;
            }
            if (!current) return {Status::unavailable, "inventory stack changed before execution"};
        }
        if (request.cancellation.is_cancelled() || epoch != FlatPickedReference::stamp())
            return {Status::unavailable, "inventory action turn or native load boundary changed"};
        if (core::SnapshotClock::now() >= request.deadline)
            return {Status::timed_out, "inventory action expired before mutation"};
        if (consume) {
            // Exact flat 1.11.240 DrinkPotion dispatch: ID 2698330 is the engine queue thread ID.
            // A matching thread unpacks inline; off-thread DrinkPotion can enqueue and still return true.
            // Never change queue flags or create an unowned deferred DrinkPotion task.
            static REL::Relocation<const volatile std::uint32_t*> queue_thread{REL::ID(2698330)};
            const auto engine_thread = *queue_thread;
            if (engine_thread == 0 || engine_thread != REX::W32::GetCurrentThreadId())
                return {Status::unavailable, "consumption cannot execute synchronously on this engine thread"};
        }
        // No inventory/extra-data lock is held across mutation; equipment keeps its non-forcing native restrictions.
        runtime::RuntimeActionResult result{Status::failed, "inventory operation failed; runtime outcome is unconfirmed"};
        try {
            const bool accepted = consume ? actor->DrinkPotion(potion, stack.index) : equip
                ? equipment->EquipObject(actor, *instance, stack.index, 1, nullptr, false, false, true, true, false)
                : equipment->UnequipObject(actor, &*instance, 1, nullptr, stack.index, false, false, true, true, nullptr);
            if (!accepted) {
                result = {Status::rejected, "Fallout 4 rejected the inventory operation; success is unconfirmed"};
            } else {
                const auto after = epoch == FlatPickedReference::stamp() && !menu_mode_active()
                    ? equipment_inventory(*actor, *object) : std::nullopt;
                const bool confirmed = epoch == FlatPickedReference::stamp() && !menu_mode_active() && after && (consume
                    ? actions::consumption_postcondition(before->items, after->items, item)
                    : actions::equipment_postcondition(before->items, after->items, item, equip));
                result = confirmed
                    ? runtime::RuntimeActionResult{Status::succeeded, consume ? "one item consumed; live inventory count verified" :
                        equip ? "item equipped; live state and count verified" : "item unequipped; live state and count verified"}
                    : runtime::RuntimeActionResult{Status::failed, "inventory operation ran but its expected state and count could not be confirmed"};
            }
        } catch (...) {
            // Even an exception after entering the engine may have changed inventory; retain an observation attempt.
        }
        return finish(std::move(result));
#endif
    }
    case runtime::RuntimeActionName::sheathe_weapon:
        if (actor->IsDead(false) || actor->IsDisabled()) {
            return {runtime::RuntimeActionStatus::rejected,
                    "actor cannot sheathe a weapon in the current state"};
        }
        if (!actor->GetWeaponMagicDrawn()) {
            return {runtime::RuntimeActionStatus::succeeded,
                    "weapon is already sheathed"};
        }
        (void)actor->SetWeaponMagicDrawn(false);
        if (!actor->GetWeaponMagicDrawn()) {
            return {runtime::RuntimeActionStatus::succeeded,
                    "weapon sheathed and runtime state verified"};
        }
        return {runtime::RuntimeActionStatus::rejected,
                "Fallout 4 did not confirm the sheathed weapon state"};
    }
    return {runtime::RuntimeActionStatus::unsupported_runtime,
            "action is not implemented by this Fallout runtime"};
}

}  // namespace synth::adapters::commonlib
