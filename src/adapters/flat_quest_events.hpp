#pragma once

#include "core/quest_event_mailbox.hpp"
#include <F4SE/F4SE.h>
#include <RE/B/BSTEvent.h>
#include <cstring>

namespace synth::adapters {

// Exact 1.11.240 concrete sinks; capture is armed only after owned server support is acknowledged.
class FlatQuestEvents final {
public:
    [[nodiscard]] static bool install() {
        if (installed_) return available();
        const auto module = REX::FModule::GetExecutingModule();
        if (module.GetFileVersion().pack() != REL::Version{1, 11, 240, 0}.pack()) return false;
        REL::Relocation<std::uintptr_t> stage{RE::VTABLE::GameScript__FragmentEventHandler[3]};
        REL::Relocation<std::uintptr_t> lifecycle{RE::VTABLE::GameScript__QuestCallbackMgr[1]};
        const auto stage_entry = reinterpret_cast<std::uintptr_t*>(stage.address())[1];
        const auto lifecycle_entry = reinterpret_cast<std::uintptr_t*>(lifecycle.address())[1];
        // Verify both before writing either; never replace another mod's observer chain.
        if (stage_entry != module.GetBaseAddress() + 0x10AF860 ||
            lifecycle_entry != module.GetBaseAddress() + 0x10D4CF0) {
            REX::WARN("SYNTH quest observation unavailable: handler slots differ from Fallout 4 1.11.240");
            return false;
        }
        original_stage_ = reinterpret_cast<Handler>(stage_entry);
        original_lifecycle_ = reinterpret_cast<Handler>(lifecycle_entry);
        stage.write_vfunc(1, stage_changed);
        lifecycle.write_vfunc(1, started_stopped);
        installed_ = true;
        REX::INFO("SYNTH quest observation: exact flat stage/start-stop handlers installed");
        return true;
    }
    [[nodiscard]] static bool available() {
        if (!installed_) return false;
        REL::Relocation<std::uintptr_t> stage{RE::VTABLE::GameScript__FragmentEventHandler[3]};
        REL::Relocation<std::uintptr_t> lifecycle{RE::VTABLE::GameScript__QuestCallbackMgr[1]};
        return reinterpret_cast<std::uintptr_t*>(stage.address())[1] == reinterpret_cast<std::uintptr_t>(&stage_changed) &&
            reinterpret_cast<std::uintptr_t*>(lifecycle.address())[1] == reinterpret_cast<std::uintptr_t>(&started_stopped);
    }
    static void arm() { if (available()) mailbox_.arm(); else mailbox_.invalidate(); }
    static void invalidate() noexcept { mailbox_.invalidate(); }
    [[nodiscard]] static core::QuestEventMailbox::Batch take() noexcept { return mailbox_.take(); }
    [[nodiscard]] static bool is_current(std::uint64_t owner) noexcept { return mailbox_.is_current(owner); }
    [[nodiscard]] static std::uint64_t dropped_total() noexcept { return mailbox_.dropped_total(); }

private:
    using Handler = RE::BSEventNotifyControl (*)(void*, const void*, void*);
    // Exceptional native unwinding must not leave an unfinished reservation blocking later events.
    struct Pending final {
        core::QuestEventMailbox::Ticket ticket;
        ~Pending() { mailbox_.discard(ticket); }
    };

    static RE::BSEventNotifyControl stage_changed(void* self, const void* event, void* source) {
        const auto owner = mailbox_.stamp();
        core::QuestEvent observation;
        if (event && mailbox_.is_current(owner)) {
            const auto* bytes = static_cast<const unsigned char*>(event);
            std::memcpy(&observation.form_id, bytes + 0x08, sizeof(observation.form_id));
            std::memcpy(&observation.stage, bytes + 0x0C, sizeof(observation.stage));
            observation.item = bytes[0x0E];
        }
        const Pending pending{mailbox_.reserve(owner, observation)};
        const auto result = original_stage_(self, event, source);
        (void)mailbox_.commit(pending.ticket);
        return result;
    }

    static RE::BSEventNotifyControl started_stopped(void* self, const void* event, void* source) {
        const auto owner = mailbox_.stamp();
        core::QuestEvent observation;
        if (event && mailbox_.is_current(owner)) {
            const auto* bytes = static_cast<const unsigned char*>(event);
            if (bytes[0x04] <= 1 && bytes[0x05] <= 1) {
                std::memcpy(&observation.form_id, bytes, sizeof(observation.form_id));
                observation.kind = bytes[0x04] ? core::QuestEventKind::started : core::QuestEventKind::stopped;
                observation.failed = bytes[0x05] != 0;
            }
        }
        const Pending pending{mailbox_.reserve(owner, observation)};
        const auto result = original_lifecycle_(self, event, source);
        (void)mailbox_.commit(pending.ticket);
        return result;
    }

    static inline core::QuestEventMailbox mailbox_;
    static inline Handler original_stage_{}, original_lifecycle_{};
    static inline std::atomic_bool installed_{};
};

} // namespace synth::adapters
