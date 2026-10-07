#pragma once

#include "core/actor_event_mailbox.hpp"
#include "core/flat_event_identity_scope.hpp"
#include <F4SE/F4SE.h>
#include <RE/B/BSTEvent.h>
#include <cstring>

namespace synth::adapters {

// Exact flat observers only. The host arms after negotiated transport and a stable copied owner scene.
class FlatActorEvents final {
public:
    [[nodiscard]] static bool install() {
        if (installed_) return available();
        const auto module=REX::FModule::GetExecutingModule();
        if (module.GetFileVersion().pack()!=REL::Version{1,11,240,0}.pack()) return false;
        REL::Relocation<std::uintptr_t> death{RE::VTABLE::GameScript__BasicEventHandler[25]};
        REL::Relocation<std::uintptr_t> equipment{RE::VTABLE::GameScript__InventoryEventHandler[3]};
        REL::Relocation<std::uintptr_t> policy{RE::VTABLE::GameScript__HandlePolicy[0]};
        const auto death_entry=reinterpret_cast<std::uintptr_t*>(death.address())[1];
        const auto equipment_entry=reinterpret_cast<std::uintptr_t*>(equipment.address())[1];
        const auto policy_entry=reinterpret_cast<std::uintptr_t*>(policy.address())[7];
        if (death_entry!=module.GetBaseAddress()+0x11931A0 || equipment_entry!=module.GetBaseAddress()+0x10C17A0 ||
            policy_entry!=module.GetBaseAddress()+0x109A4F0) {
            REX::WARN("SYNTH actor events unavailable: exact native observer slots differ");
            return false;
        }
        original_death_=reinterpret_cast<Handler>(death_entry);
        original_equipment_=reinterpret_cast<Handler>(equipment_entry);
        original_policy_=reinterpret_cast<Policy>(policy_entry);
        policy.write_vfunc(7,handle_for_object);
        death.write_vfunc(1,death_event);
        equipment.write_vfunc(1,equipment_event);
        installed_=true;
        REX::INFO("SYNTH actor events: exact flat death/equipment/scalar identity observers installed");
        return true;
    }
    [[nodiscard]] static bool available() {
        if (!installed_) return false;
        REL::Relocation<std::uintptr_t> death{RE::VTABLE::GameScript__BasicEventHandler[25]};
        REL::Relocation<std::uintptr_t> equipment{RE::VTABLE::GameScript__InventoryEventHandler[3]};
        REL::Relocation<std::uintptr_t> policy{RE::VTABLE::GameScript__HandlePolicy[0]};
        return reinterpret_cast<std::uintptr_t*>(death.address())[1]==reinterpret_cast<std::uintptr_t>(&death_event) &&
            reinterpret_cast<std::uintptr_t*>(equipment.address())[1]==reinterpret_cast<std::uintptr_t>(&equipment_event) &&
            reinterpret_cast<std::uintptr_t*>(policy.address())[7]==reinterpret_cast<std::uintptr_t>(&handle_for_object);
    }
    static void arm() { if (available()) mailbox_.arm(); else mailbox_.invalidate(); }
    [[nodiscard]] static std::uint64_t stamp() noexcept { return mailbox_.stamp(); }
    static void invalidate() noexcept { mailbox_.invalidate(); }
    [[nodiscard]] static auto take() noexcept { return mailbox_.take(); }
    [[nodiscard]] static bool is_current(std::uint64_t owner) noexcept { return mailbox_.is_current(owner); }
    [[nodiscard]] static std::uint64_t dropped_total() noexcept { return mailbox_.dropped_total(); }
    [[nodiscard]] static std::uint64_t unresolved_total() noexcept { return unresolved_.load(std::memory_order_relaxed); }

private:
    using Handler=RE::BSEventNotifyControl (*)(void*,const void*,void*);
    using Policy=std::uint64_t (*)(const void*,std::uint32_t,const void*);
    struct Pending final {
        core::ActorEventMailbox::Ticket ticket;
        ~Pending() { mailbox_.discard(ticket); }
    };
    // Observe only the original invocation's scalar return. No new engine lookup, actor dereference or retained pointer.
    static std::uint64_t handle_for_object(const void* self,std::uint32_t type,const void* object) {
        const auto result=original_policy_(self,type,object);
        core::FlatEventIdentityScope::observe(type,reinterpret_cast<std::uintptr_t>(object),result);
        return result;
    }
    static RE::BSEventNotifyControl death_event(void* self,const void* event,void* source) {
        const auto owner=mailbox_.stamp();
        std::uintptr_t actor{},other{};
        if (event && mailbox_.is_current(owner)) {
            const auto* bytes=static_cast<const unsigned char*>(event);
            if (bytes[0x10]==1) { // Dying is not confirmed death.
                std::memcpy(&actor,bytes,sizeof(actor));
                std::memcpy(&other,bytes+8,sizeof(other));
            }
        }
        const Pending pending{actor ? mailbox_.reserve(owner) : core::ActorEventMailbox::Ticket{}};
        const auto observed_at=pending.ticket ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const core::FlatEventIdentityScope identities{pending.ticket ? owner : 0,actor,other};
        const auto result=original_death_(self,event,source);
        if (pending.ticket && mailbox_.is_current(owner)) {
            if (const auto ids=identities.result(owner))
                (void)mailbox_.commit(pending.ticket,{core::ActorEventKind::death,ids->actor,ids->other,0,0,0,observed_at});
            else ++unresolved_;
        }
        return result;
    }
    static RE::BSEventNotifyControl equipment_event(void* self,const void* event,void* source) {
        const auto owner=mailbox_.stamp();
        std::uintptr_t actor{};
        core::ActorEvent observation;
        if (event && mailbox_.is_current(owner)) {
            const auto* bytes=static_cast<const unsigned char*>(event);
            if (bytes[0x12]<=1) {
                std::memcpy(&actor,bytes,sizeof(actor));
                std::memcpy(&observation.base_object,bytes+8,sizeof(observation.base_object));
                std::memcpy(&observation.original_reference,bytes+12,sizeof(observation.original_reference));
                std::memcpy(&observation.unique_id,bytes+16,sizeof(observation.unique_id));
                observation.kind=bytes[0x12] ? core::ActorEventKind::equipped : core::ActorEventKind::unequipped;
            }
        }
        const Pending pending{actor ? mailbox_.reserve(owner) : core::ActorEventMailbox::Ticket{}};
        if (pending.ticket) observation.observed_at=std::chrono::steady_clock::now();
        const core::FlatEventIdentityScope identities{pending.ticket ? owner : 0,actor};
        const auto result=original_equipment_(self,event,source);
        if (pending.ticket && mailbox_.is_current(owner)) {
            if (const auto ids=identities.result(owner)) {
                observation.actor=ids->actor;
                if (!observation.valid()) ++unresolved_;
                (void)mailbox_.commit(pending.ticket,observation);
            } else ++unresolved_;
        }
        return result;
    }
    static inline core::ActorEventMailbox mailbox_;
    static inline Handler original_death_{},original_equipment_{};
    static inline Policy original_policy_{};
    static inline std::atomic_bool installed_{};
    static inline std::atomic_uint64_t unresolved_{};
};

} // namespace synth::adapters
