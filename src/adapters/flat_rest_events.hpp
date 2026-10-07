#pragma once

#include "core/rest_event_mailbox.hpp"
#include <F4SE/F4SE.h>
#include <RE/B/BSTEvent.h>

namespace synth::adapters {

// Exact 1.11.240 GameScript sink slots, independently resolved from this executable's RTTI and address library.
class FlatRestEvents final {
public:
    [[nodiscard]] static bool install() {
        if (installed_) return true;
        const auto module = REX::FModule::GetExecutingModule();
        if (module.GetFileVersion().pack() != REL::Version{1, 11, 240, 0}.pack()) return false;
        REL::Relocation<std::uintptr_t> sleep{RE::VTABLE::GameScript__SleepEventHandler[0]};
        REL::Relocation<std::uintptr_t> wait{RE::VTABLE::GameScript__WaitEventHandler[0]};
        const auto sleep_entry = reinterpret_cast<std::uintptr_t*>(sleep.address())[1];
        const auto wait_entry = reinterpret_cast<std::uintptr_t*>(wait.address())[1];
        // Refuse a changed slot (including another mod's hook), rather than replacing its observer chain.
        if (sleep_entry != module.GetBaseAddress() + 0x010DABB0 ||
            wait_entry != module.GetBaseAddress() + 0x011A7620) {
            REX::WARN("SYNTH rest observation unavailable: start-handler slots differ from Fallout 4 1.11.240");
            return false;
        }
        original_sleep_ = reinterpret_cast<Handler>(sleep_entry);
        original_wait_ = reinterpret_cast<Handler>(wait_entry);
        sleep.write_vfunc(1, sleep_start);
        wait.write_vfunc(1, wait_start);
        installed_ = true;
        REX::INFO("SYNTH rest observation: exact flat sleep/wait start handlers installed");
        return true;
    }

    static void arm() noexcept { if (installed_) mailbox_.arm(); }
    // Called by the flat game-thread adapter before freezing a session's capabilities, never by workers.
    [[nodiscard]] static bool available() {
        if (!installed_) return false;
        REL::Relocation<std::uintptr_t> sleep{RE::VTABLE::GameScript__SleepEventHandler[0]};
        REL::Relocation<std::uintptr_t> wait{RE::VTABLE::GameScript__WaitEventHandler[0]};
        return reinterpret_cast<std::uintptr_t*>(sleep.address())[1] == reinterpret_cast<std::uintptr_t>(&sleep_start) &&
            reinterpret_cast<std::uintptr_t*>(wait.address())[1] == reinterpret_cast<std::uintptr_t>(&wait_start);
    }
    static void invalidate() noexcept { mailbox_.invalidate(); }
    [[nodiscard]] static core::RestEventBatch take() noexcept { return mailbox_.take(); }
    [[nodiscard]] static std::uint64_t dropped_total() noexcept { return mailbox_.dropped_total(); }

private:
    using Handler = RE::BSEventNotifyControl (*)(void*, const void*, void*);

    // Capture the epoch before forwarding: a load inside the original handler must discard this observation.
    static RE::BSEventNotifyControl sleep_start(void* self, const void* event, void* source) {
        const auto owner = mailbox_.stamp();
        const auto result = original_sleep_(self, event, source);
        if (event) (void)mailbox_.record(owner, core::RestStart::sleep);
        return result;
    }

    static RE::BSEventNotifyControl wait_start(void* self, const void* event, void* source) {
        const auto owner = mailbox_.stamp();
        const auto result = original_wait_(self, event, source);
        if (event) (void)mailbox_.record(owner, core::RestStart::wait);
        return result;
    }

    static inline core::RestEventMailbox mailbox_;
    static inline Handler original_sleep_{}, original_wait_{};
    static inline std::atomic_bool installed_{};
};

} // namespace synth::adapters
