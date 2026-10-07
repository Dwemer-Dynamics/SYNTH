#pragma once

#include "core/picked_reference_mailbox.hpp"
#include <F4SE/F4SE.h>
#include <RE/B/BSTEvent.h>
#include <cstring>

namespace synth::adapters {

// Binding/layout independently checked against exact 1.11.240; see FLAT-PICKED-REFERENCE-AUDIT.md.
// Observe the existing typed sink without creating/registering a source during save loading.
class FlatPickedReference final {
public:
    [[nodiscard]] static bool install() {
        if (installed_) return true;
        const auto module = REX::FModule::GetExecutingModule();
        if (module.GetFileVersion().pack() != REL::Version{1, 11, 240, 0}.pack()) return false;
        REL::Relocation<std::uintptr_t> table{RE::VTABLE::BSTValueEventSink_ViewCasterUpdateEvent_[0]};
        const auto entry = reinterpret_cast<std::uintptr_t*>(table.address())[1];
        if (entry != module.GetBaseAddress() + 0xA11BC0) {
            REX::WARN("SYNTH native pick unavailable: ViewCaster observer slot differs from Fallout 4 1.11.240");
            return false;
        }
        original_ = reinterpret_cast<Handler>(entry);
        table.write_vfunc(1, observe);
        installed_ = true;
        REX::INFO("SYNTH native pick: exact flat ViewCaster observer installed");
        return true;
    }
    static void arm() {
        if (!installed_) return;
        REL::Relocation<std::uintptr_t> table{RE::VTABLE::BSTValueEventSink_ViewCasterUpdateEvent_[0]};
        if (reinterpret_cast<std::uintptr_t*>(table.address())[1] == reinterpret_cast<std::uintptr_t>(&observe))
            mailbox_.arm();
        else mailbox_.invalidate();
    }
    static void invalidate() noexcept { mailbox_.invalidate(); }
    [[nodiscard]] static std::optional<std::uint32_t> read() noexcept { return mailbox_.read(); }
    [[nodiscard]] static std::uint64_t stamp() noexcept { return mailbox_.stamp(); }
private:
    using Handler = RE::BSEventNotifyControl (*)(void*, const void*, void*);

    // No engine lookup, dispatcher call, allocation or SYNTH lock under the native observer.
    static RE::BSEventNotifyControl observe(void* self, const void* event, void* source) {
        const auto owner = mailbox_.stamp();
        std::uint32_t handle{};
        if (event) {
            const auto* bytes = static_cast<const unsigned char*>(event);
            if (bytes[0x38] != 0) std::memcpy(&handle, bytes, sizeof(handle));
        }
        const auto result = original_(self, event, source);
        (void)mailbox_.record(owner, handle);
        return result;
    }
    static inline core::PickedReferenceMailbox mailbox_;
    static inline Handler original_{};
    static inline std::atomic_bool installed_{};
};

} // namespace synth::adapters
