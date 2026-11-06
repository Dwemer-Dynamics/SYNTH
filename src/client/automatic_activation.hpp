#pragma once

#include "targeting/targeting.hpp"

namespace synth::client {

// The picked and nearest paths use the same physical automatic-hearing limit.
// Availability, actor classes and explicit user settings remain the caller's policy.
[[nodiscard]] inline targeting::TargetingResult select_flat_automatic_activation(
    const core::RuntimeSnapshot& snapshot,core::SnapshotClock::time_point now,
    core::RuntimeGeneration generation,double hearing_distance,
    const std::function<bool(const core::ActorSnapshot&)>& eligible = {}) {
    using targeting::TargetingStatus;
    if(snapshot.variant()!=core::RuntimeVariant::flat)
        return {TargetingStatus::unavailable_pose,{}};
    if(snapshot.generation()!=generation||snapshot.frame()==0||now<snapshot.captured_at()||
        now-snapshot.captured_at()>std::chrono::seconds{2})return {TargetingStatus::stale_pose,{}};
    if(!std::isfinite(hearing_distance)||hearing_distance<=0||!snapshot.player().alive()||snapshot.player().disabled()||
        (snapshot.actors_observation()!="complete"&&snapshot.actors_observation()!="partial"))return {};
    const context::PlaythroughIdentity playthrough{snapshot.player().playthrough_id()};
    const auto physical=[&](const core::ActorSnapshot& actor) {
        return actor.form_id()!=0x14&&actor.conversation_area_available()&&(!eligible||eligible(actor));
    };
    auto picked=targeting::select_flat_crosshair(snapshot,playthrough,{hearing_distance,75},physical);
    if(picked.target||picked.status==TargetingStatus::identity_mismatch)return picked;
    return targeting::select_nearest(snapshot,playthrough,snapshot.player_pose().position(),hearing_distance,physical);
}

// A suppressed greeting is a retryable policy decision, not a permanent actor
// blacklist. Admission is bounded while backend salutation/day cooldowns retain authority.
class AutomaticActivationCadence final {
public:
    static constexpr std::size_t capacity=128;
    static constexpr auto retry_interval=std::chrono::seconds{60};
    [[nodiscard]] bool due(std::string_view actor,core::SnapshotClock::time_point now) const noexcept {
        if(actor.empty()||actor.size()>512)return false;
        const auto found=std::ranges::find_if(entries_,[&](const auto& entry){return entry.actor==actor;});
        return found==entries_.end()||now>=found->next;
    }
    void admitted(std::string actor,core::SnapshotClock::time_point now) {
        if(actor.empty()||actor.size()>512)return;
        const auto found=std::ranges::find_if(entries_,[&](const auto& entry){return entry.actor==actor;});
        if(found!=entries_.end()) {found->next=now+retry_interval;return;}
        if(entries_.size()==capacity)entries_.erase(std::ranges::min_element(entries_,{},&Entry::next));
        entries_.push_back({std::move(actor),now+retry_interval});
    }
    void forget(std::string_view actor) {std::erase_if(entries_,[&](const auto& entry){return entry.actor==actor;});}
    void clear() noexcept {entries_.clear();}
    [[nodiscard]] std::size_t size() const noexcept {return entries_.size();}
private:
    struct Entry final {std::string actor;core::SnapshotClock::time_point next;};
    std::vector<Entry> entries_;
};
} // namespace synth::client
