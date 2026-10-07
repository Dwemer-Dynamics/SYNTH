#pragma once
#if defined(SYNTH_WITH_F4SEVR)
#error "Pickup inspection requires independently audited flat bindings"
#endif

#include "adapters/commonlib_actions.hpp"
#include "runtime/pickup_progress.hpp"
#include <F4SE/F4SE.h>

namespace synth::adapters::commonlib {

struct PickupInspection final {
    runtime::PickupProgress::Inspection state{runtime::PickupProgress::Inspection::unavailable};
    std::optional<core::InventoryItemSnapshot> ammunition;
    std::optional<std::vector<core::InventoryItemSnapshot>> inventory;
};

// Targeted read only: retain references within this call, never carry engine pointers into another frame.
// Near/far is not theft/quest permission or transfer authorization; mutation must revalidate again.
[[nodiscard]] inline PickupInspection inspect_pickup(
    const runtime::IFalloutRuntime& runtime, const runtime::RuntimePickupRequest& request) {
    using Inspection = runtime::PickupProgress::Inspection;
    runtime.assert_game_thread();
    const auto epoch = FlatPickedReference::stamp();
    const auto started = core::SnapshotClock::now();
    if (runtime.variant() != core::RuntimeVariant::flat ||
        REX::FModule::GetExecutingModule().GetFileVersion().pack() != REL::Version{1,11,240,0}.pack() ||
        request.cancellation.is_cancelled() || request.cancellation.generation() != runtime.generation() ||
        started >= request.deadline || started < request.captured_at || menu_mode_active()) return {Inspection::unavailable};
    const auto* manager = RE::BGSSaveLoadManager::GetSingleton();
    if (!manager || manager->currentPlayerID == 0) return {Inspection::unavailable};
    if (!request.item.valid() || request.item.held || request.actor.form_id == 0 || request.actor.form_id == 0x14 ||
        request.actor.playthrough_id != playthrough_id()) return {Inspection::rejected};

    RE::NiPointer<RE::Actor> actor{RE::TESForm::GetFormByID<RE::Actor>(request.actor.form_id)};
    RE::NiPointer<RE::TESObjectREFR> reference{RE::TESForm::GetFormByID<RE::TESObjectREFR>(request.item.reference_id)};
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!actor || !reference || !player || actor.get() == player || reference.get() == actor.get() ||
        actor->IsDeleted() || actor->IsDisabled() || actor->IsDead(false) || !actor->Get3D() ||
        !actor->currentProcess || !actor->inventoryList || reference->IsDeleted() || reference->IsDisabled() ||
        !reference->Get3D()) return {Inspection::rejected};
    auto* cell = reference->GetParentCell();
    if (!cell || actor->GetParentCell() != cell || player->GetParentCell() != cell ||
        cell->GetFormID() != request.item.cell_form_id ||
        !plugin_name_equal(origin_plugin(*actor), request.actor.origin_plugin)) return {Inspection::rejected};
    if (actor->lifeState != 0 || actor->knockState != 0 || actor->inSyncAnim || actor->IsInCombat() ||
        actor->DoGetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal ||
        (actor->niFlags.flags & static_cast<std::uint32_t>(RE::Actor::BOOL_FLAGS::kScenePackage)) != 0)
        return {Inspection::rejected};
    const auto* middle_high = actor->currentProcess->middleHigh;
    if (!middle_high) return {Inspection::unavailable};
    // Native cleanup releases furniture reservations through blocking extra-data writes, even after standing.
    if (middle_high->currentFurniture || middle_high->occupiedFurniture || middle_high->reservationSlot >= 0)
        return {Inspection::rejected};
    const auto grabbed = player->grabbedObject.get();
    if (grabbed && grabbed.get() == reference.get()) return {Inspection::rejected};
    auto* base = reference->GetObjectReference();
    if (!base || !is_nearby_item_type(base->GetFormType())) return {Inspection::rejected};
    static REL::Relocation<std::uintptr_t> count_vtable{RE::VTABLE::ExtraCount[0]};
    static REL::Relocation<std::uintptr_t> ammo_vtable{RE::VTABLE::ExtraAmmo[0]};
    const auto metadata = inventory_metadata(*base, reference->extraList, count_vtable.address(), ammo_vtable.address());
    if (!metadata) return {Inspection::unavailable};
    if (!metadata->reference_loaded_ammo) return {Inspection::unavailable};
    if (base->GetFormType() == RE::ENUM_FORM_ID::kWEAP &&
        static_cast<std::uint64_t>(*metadata->reference_loaded_ammo) * metadata->reference_count > 2147483647U)
        return {Inspection::rejected};
    const auto statistics = inventory_statistics(*base, metadata->instance.get());
    if (!statistics) return {Inspection::unavailable};
    std::optional<core::InventoryItemSnapshot> ammunition;
    if (base->GetFormType() == RE::ENUM_FORM_ID::kWEAP && *metadata->reference_loaded_ammo != 0) {
        const auto* weapon = base->As<RE::TESObjectWEAP>();
        if (!weapon) return {Inspection::unavailable};
        const RE::TBO_InstanceData* instance = metadata->instance ? metadata->instance.get() : &weapon->weaponData;
        static REL::Relocation<std::uintptr_t> instance_vtable{RE::VTABLE::TESObjectWEAP__InstanceData[0]};
        static REL::Relocation<std::uintptr_t> data_vtable{RE::VTABLE::TESObjectWEAP__Data[0]};
        std::uintptr_t observed_vtable{};
        std::memcpy(&observed_vtable, instance, sizeof(observed_vtable));
        if (observed_vtable != instance_vtable.address() && observed_vtable != data_vtable.address())
            return {Inspection::unavailable};
        // Native500A61 uses instance+68 when present; base+200 only when the instance pointer is absent.
        auto* ammo = static_cast<const RE::TESObjectWEAP::InstanceData*>(instance)->ammo;
        if (!ammo || ammo->GetFormType() != RE::ENUM_FORM_ID::kAMMO) return {Inspection::unavailable};
        const auto ammo_metadata = inventory_metadata(*ammo, nullptr);
        const auto ammo_statistics = inventory_statistics(*ammo, nullptr);
        if (!ammo_metadata || !ammo_statistics) return {Inspection::unavailable};
        ammunition = core::InventoryItemSnapshot{ammo->GetFormID(), origin_plugin(*ammo), ammo_metadata->name,
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(*metadata->reference_loaded_ammo) * metadata->reference_count),
            ammo_statistics->value, ammo_statistics->weight, static_cast<std::uint16_t>(ammo->GetFormType()), false};
        if (!ammunition->valid()) return {Inspection::unavailable};
    }
    std::optional<std::string> reference_plugin;
    if (const auto* file = reference->GetFile(0); file) {
        auto name = bounded_label(file->GetFilename().data());
        if (name.empty()) return {Inspection::unavailable};
        reference_plugin = std::move(name);
    }
    const auto position = point(reference->GetPosition());
    const auto performer_position = point(actor->GetPosition());
    const auto distance = std::hypot(position.x - performer_position.x, position.y - performer_position.y,
                                     position.z - performer_position.z);
    if (!position.finite() || !performer_position.finite() || !std::isfinite(distance)) return {Inspection::unavailable};
    const core::NearbyItemSnapshot current{reference->GetFormID(), base->GetFormID(), cell->GetFormID(),
        std::move(reference_plugin), origin_plugin(*base), origin_plugin(*cell), metadata->name,
        position, distance, statistics->weight, metadata->reference_count, statistics->value,
        static_cast<std::uint16_t>(base->GetFormType()), false, false, false};
    if (!actions::pickup_reference_matches(request.item, current)) return {Inspection::rejected};
    std::optional<std::vector<core::InventoryItemSnapshot>> inventory;
    if (distance <= 128.0) {
        auto captured = inventory_snapshot(*actor, std::min(request.deadline, started + std::chrono::milliseconds{2}));
        if (captured.second != "complete") return {Inspection::unavailable};
        inventory = std::move(captured.first);
    }
    if (request.cancellation.is_cancelled() || request.cancellation.generation() != runtime.generation() ||
        epoch != FlatPickedReference::stamp() || menu_mode_active() || core::SnapshotClock::now() >= request.deadline ||
        core::SnapshotClock::now() - started >= std::chrono::milliseconds{2}) return {Inspection::unavailable};
    return {distance <= 128.0 ? Inspection::near : Inspection::far, std::move(ammunition), std::move(inventory)};
}

} // namespace synth::adapters::commonlib
