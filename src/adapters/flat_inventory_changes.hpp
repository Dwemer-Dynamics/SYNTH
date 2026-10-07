#pragma once

#include "core/inventory_change_mailbox.hpp"
#include <F4SE/F4SE.h>
#include <RE/B/BSTEvent.h>
#include <array>
#include <cstring>

namespace synth::adapters {

// Exact 1.11.240 TESContainerChangedEvent observer, independent of actor-history transport negotiation.
class FlatInventoryChanges final {
public:
    [[nodiscard]] static bool install() {
        if (installed_) return available();
        const auto module = REX::FModule::GetExecutingModule();
        if (module.GetFileVersion().pack() != REL::Version{1,11,240,0}.pack()) return false;
        REL::Relocation<std::uintptr_t> table{RE::VTABLE::GameScript__InventoryEventHandler[2]};
        const auto entry = reinterpret_cast<std::uintptr_t*>(table.address())[1];
        if (entry != module.GetBaseAddress() + 0x10C1150) {
            REX::WARN("SYNTH inventory changes unavailable: exact container observer slot differs");
            return false;
        }
        original_ = reinterpret_cast<Handler>(entry);
        table.write_vfunc(1, observe);
        installed_ = true;
        REX::INFO("SYNTH inventory changes: exact flat container-change observer installed");
        return true;
    }
    [[nodiscard]] static bool available() {
        if (!installed_) return false;
        REL::Relocation<std::uintptr_t> table{RE::VTABLE::GameScript__InventoryEventHandler[2]};
        return reinterpret_cast<std::uintptr_t*>(table.address())[1] == reinterpret_cast<std::uintptr_t>(&observe);
    }
    static void arm() { if (available()) changes_.arm(); else changes_.invalidate(); }
    static void invalidate() noexcept { changes_.invalidate(); }
    [[nodiscard]] static auto revision() noexcept { return changes_.revision(); }

private:
    using Handler = RE::BSEventNotifyControl (*)(void*,const void*,void*);

    // Copy the audited scalar prefix before forwarding exactly once; never query inventory under engine callback locks.
    static RE::BSEventNotifyControl observe(void* self, const void* event, void* source) {
        const auto owner = changes_.stamp();
        std::array<std::uint32_t,3> fields{};
        if (event) std::memcpy(fields.data(), event, sizeof(fields)); // old/new/base FormID at 0/4/8.
        const auto result = original_(self,event,source);
        (void)changes_.record(owner,fields[0],fields[1],fields[2]);
        return result;
    }

    static inline core::InventoryChangeMailbox changes_;
    static inline Handler original_{};
    static inline std::atomic_bool installed_{};
};

} // namespace synth::adapters
