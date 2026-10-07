#pragma once

#include "adapters/commonlib_capture.hpp"
#include "client/actor_event_binding.hpp"
#include <span>

namespace synth::adapters::commonlib {

// Resolve at most 64 event identities under one try-lease, then shallow-copy pinned loaded actors after releasing it.
[[nodiscard]] inline std::shared_ptr<const core::RuntimeSnapshot> actor_event_scene(
    const runtime::IFalloutRuntime& runtime,std::shared_ptr<const core::RuntimeSnapshot> scene,
    std::span<const core::ActorEvent> events) {
    runtime.assert_game_thread();
    if (!scene || runtime.variant()!=core::RuntimeVariant::flat || scene->variant()!=runtime.variant() ||
        scene->generation()!=runtime.generation() || events.empty() || events.size()>core::ActorEventMailbox::capacity ||
        !scene->world() || scene->world()->game_time_ticks==0 || !world_ready_for_capture() ||
        playthrough_id()!=scene->player().playthrough_id()) return {};
    const auto generation=runtime.generation();
    auto* player=RE::PlayerCharacter::GetSingleton();
    if (!player || player->GetFormID()!=scene->player().form_id()) return {};
    std::array<std::uint32_t,64> ids{};
    std::array<RE::NiPointer<RE::Actor>,64> actors{};
    std::size_t count{};
    for (const auto& event:events) if (event.valid()) {
        for (const auto id:{event.actor,event.other_actor}) {
            if (!id || id==scene->player().form_id() || std::find(ids.begin(),ids.begin()+count,id)!=ids.begin()+count) continue;
            ids[count++]=id;
        }
    }
    if (count) {
        const auto [map,lock]=RE::TESForm::GetAllForms();
        const TryReadLock lease{lock.get()};
        if (!lease.owns_lock() || !map) return {}; // Never replace missing identity evidence with nearby discovery.
        for (std::size_t i=0;i<count;++i) {
            const auto found=map->find(ids[i]);
            if (found!=map->end() && found->second && found->second->GetFormID()==ids[i])
                actors[i]=found->second->As<RE::Actor>(); // Pin through the shallow capture; refcount attachment is atomic.
        }
    }
    std::vector<core::ActorSnapshot> copied;copied.reserve(count);
    for (std::size_t i=0;i<count;++i) {
        if (runtime.generation()!=generation || !world_ready_for_capture() || playthrough_id()!=scene->player().playthrough_id()) return {};
        const auto& actor=actors[i];
        if (!actor || actor->GetFormID()!=ids[i] || !actor->Get3D() || actor->IsDisabled()) continue;
        const auto* player_cell=player->GetParentCell();
        const auto* actor_cell=actor->GetParentCell();
        if (!player_cell || !actor_cell || (player_cell!=actor_cell && (player_cell->IsInterior() || actor_cell->IsInterior() ||
            !player_cell->worldSpace || player_cell->worldSpace!=actor_cell->worldSpace))) continue;
        try { copied.push_back(actor_snapshot(*actor,scene->player().playthrough_id(),player,false,false)); }
        catch (const std::invalid_argument&) {} // Unavailable identity remains unresolved; never fabricate one from the scalar ID.
    }
    if (runtime.generation()!=generation || !world_ready_for_capture() || playthrough_id()!=scene->player().playthrough_id()) return {};
    return std::make_shared<const core::RuntimeSnapshot>(scene->with_event_actors(std::move(copied)));
}

// Safe-pump metadata only. One nonblocking form-table lease; no actor inventory/instance/extra-data access.
[[nodiscard]] inline client::ActorEventMetadata actor_event_metadata(
    const runtime::IFalloutRuntime& runtime,std::shared_ptr<const core::RuntimeSnapshot> scene,
    std::span<const core::ActorEvent> events) {
    runtime.assert_game_thread();
    if (!scene || runtime.variant()!=core::RuntimeVariant::flat || scene->variant()!=runtime.variant() ||
        scene->generation()!=runtime.generation() || events.size()>core::ActorEventMailbox::capacity ||
        !world_ready_for_capture() || playthrough_id()!=scene->player().playthrough_id()) return {};
    const auto generation=runtime.generation();
    std::array<std::uint32_t,core::ActorEventMailbox::capacity> ids{};
    std::array<RE::TESForm*,core::ActorEventMailbox::capacity> forms{};
    std::size_t count{};
    for (const auto& event:events) {
        if (!event.valid() || event.kind==core::ActorEventKind::death ||
            std::find(ids.begin(),ids.begin()+count,event.base_object)!=ids.begin()+count) continue;
        ids[count++]=event.base_object;
    }
    client::ActorEventMetadata metadata{scene,{}};
    if (count==0) return metadata;
    {
        const auto [map,lock]=RE::TESForm::GetAllForms();
        const TryReadLock lease{lock.get()};
        if (!lease.owns_lock() || !map) return metadata; // Missing metadata is unavailable, never guessed.
        for (std::size_t i=0;i<count;++i) {
            const auto found=map->find(ids[i]);
            if (found!=map->end()) forms[i]=found->second;
        }
    }
    metadata.items.reserve(count);
    for (std::size_t i=0;i<count;++i) {
        const auto* form=forms[i];
        if (!form || form->GetFormID()!=ids[i] || !form->IsBoundObject()) continue;
        const auto* full_name=form->As<RE::TESFullName>();
        auto name=full_name ? bounded_label(full_name->GetFullName()) : std::string{};
        if (name.empty()) name=form_fallback(*form);
        client::ActorEventItem item{ids[i],origin_plugin(*form),std::move(name),
            static_cast<std::uint16_t>(form->GetFormType())};
        if (item.valid()) metadata.items.push_back(std::move(item));
    }
    // Raw form pointers end here. The caller must also recheck its native epoch and session after this call.
    return runtime.generation()==generation && world_ready_for_capture() &&
        playthrough_id()==scene->player().playthrough_id() ? std::move(metadata) : client::ActorEventMetadata{};
}

} // namespace synth::adapters::commonlib
