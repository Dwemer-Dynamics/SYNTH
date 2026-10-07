#pragma once

#if defined(SYNTH_WITH_F4SEVR)
#error "Pickup package construction has only been audited for flat Fallout 4"
#endif

#include "runtime/fallout_runtime.hpp"
#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>

namespace synth::adapters {

// Build a private, uninstalled travel package. The caller must not hand this unique owner
// to ActorPackage: installation needs a separate refcounted owner and restoration transaction.
[[nodiscard]] inline std::unique_ptr<RE::TESPackage> prepare_pickup_travel_package(
    const runtime::IFalloutRuntime& runtime, const core::CancellationToken& cancellation,
    core::SnapshotClock::time_point deadline, RE::ObjectRefHandle destination) {
    runtime.assert_game_thread();
    if (runtime.variant() != core::RuntimeVariant::flat || !destination ||
        cancellation.is_cancelled() || cancellation.generation() != runtime.generation() ||
        core::SnapshotClock::now() >= deadline) return {};

    const auto module = REX::FModule::GetExecutingModule();
    if (module.GetFileVersion().pack() != REL::Version{1, 11, 240, 0}.pack()) return {};
    const auto base = module.GetBaseAddress();
    static REL::Relocation<RE::TESPackage* (*)(std::int32_t)> create{REL::ID(2211661)};
    static REL::Relocation<void (*)(RE::PackageLocation*, std::int32_t)> set_type{REL::ID(2211916)};
    static REL::Relocation<void (*)(RE::PackageLocation*, const std::uint32_t*)> set_handle{REL::ID(4480239)};
    static REL::Relocation<void (*)(RE::PackageLocation*, std::int32_t)> set_radius{REL::ID(2211917)};
    static REL::Relocation<void (*)(RE::TESPackage*, RE::Actor*)> initialize{REL::ID(2211735)};
    if (create.address() != base + 0x757DB0 || set_type.address() != base + 0x764AB0 ||
        set_handle.address() != base + 0x764D10 || set_radius.address() != base + 0x764B50 ||
        initialize.address() != base + 0x75D3B0) return {};

    // Exact native entry/dispatch anchors; an unknown binding must not allocate or call an actor.
    const auto matches = [base](std::uintptr_t rva, std::initializer_list<unsigned char> bytes) {
        return std::memcmp(reinterpret_cast<const void*>(base + rva), bytes.begin(), bytes.size()) == 0;
    };
    if (!matches(0x757DB0, {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10}) ||
        !matches(0x764AB0, {0x0F,0xBE,0x41,0x08,0x4C,0x8B,0xC1,0x3B,0xD0}) ||
        !matches(0x764D10, {0xC6,0x41,0x08,0x00,0x8B,0x02,0x89,0x41,0x18,0xC3}) ||
        !matches(0x764B50, {0x0F,0xB6,0x41,0x08,0xFE,0xC0,0x3C,0x0E}) ||
        !matches(0x75D3B0, {0x40,0x53,0x55,0x56,0x57,0x41,0x56}) ||
        !matches(0x75DA00, {0x07,0xD4,0x75,0x00}) ||
        !matches(0x75D407, {0x44,0x89,0xB1,0xC0,0x00,0x00,0x00})) return {};

    // Native kind6 owns a PackageLocation; kind1 is the different follow-target path.
    auto* created = create(6);
    if (created == nullptr) return {};
    // A referenced result violates the factory contract; never delete somebody else's package.
    if (created->refCount != 0) return {};
    std::unique_ptr<RE::TESPackage> package{created};
    std::uintptr_t package_vtable{}, location_vtable{};
    std::memcpy(&package_vtable, package.get(), sizeof(package_vtable));
    if (package_vtable != base + 0x2516EA8 || package->data.packType != 6 ||
        package->packLoc == nullptr || package->packTarg != nullptr || package->packData != nullptr ||
        package->ownerQuest != nullptr || (package->data.packFlags & 0x800U) != 0) return {};
    std::memcpy(&location_vtable, package->packLoc, sizeof(location_vtable));
    if (location_vtable != base + 0x2517AC8) return {};

    const auto handle = destination.get_handle();
    set_type(package->packLoc, 0);
    set_handle(package->packLoc, &handle);
    set_radius(package->packLoc, 128);
    package->data.packFlags |= 0x6U; // Same travel flags as the audited native kind6 caller.
    initialize(package.get(), nullptr);

    // Recheck ownership before cleanup or field reads if native initialization violated its contract.
    if (package->refCount != 0) {
        (void)package.release();
        return {};
    }
    if (package->packLoc == nullptr) return {};

    // Read back the engine-owned fields; no fake C++ PackageLocation layout or raw target pointer.
    const auto* location = reinterpret_cast<const unsigned char*>(package->packLoc);
    std::uint32_t bound_handle{};
    std::int32_t radius{}, procedure{};
    static_assert(sizeof(package->procedureType) == sizeof(procedure));
    std::memcpy(&bound_handle, location + 0x18, sizeof(bound_handle));
    std::memcpy(&radius, location + 0x0C, sizeof(radius));
    std::memcpy(&procedure, &package->procedureType, sizeof(procedure));
    if (location[0x08] != 0 || bound_handle != handle || radius != 128 || procedure != 0 ||
        cancellation.is_cancelled() ||
        cancellation.generation() != runtime.generation() || core::SnapshotClock::now() >= deadline) return {};
    return package;
}

} // namespace synth::adapters
