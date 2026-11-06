#pragma once
#include "runtime/fallout_runtime.hpp"
#include <cmath>
#include <optional>
#include <span>

namespace synth::runtime {
inline bool same_approach_actor(const RuntimeActorIdentity& a,const RuntimeActorIdentity& b) noexcept {
    return a.form_id==b.form_id && a.origin_plugin==b.origin_plugin && a.playthrough_id==b.playthrough_id;
}
struct ConversationApproachCandidate final {
    RuntimeConversationHold hold;
    RuntimeActorIdentity listener;
    double distance{};
    bool loaded{}, calm{}, same_area{}, native_follower{}, explicit_wait{};
    [[nodiscard]] bool eligible() const noexcept {
        return hold.approach_player && hold.actor.form_id && hold.actor.form_id!=0x14 && listener.form_id==0x14 &&
            !hold.actor.origin_plugin.empty() && !hold.actor.playthrough_id.empty() &&
            hold.actor.playthrough_id==listener.playthrough_id && !hold.cancellation.is_cancelled() &&
            loaded && calm && same_area && !native_follower && !explicit_wait && std::isfinite(distance) &&
            distance>=0 && distance<=2000;
    }
};
enum class ConversationApproachKind : unsigned char { acquire, refresh, release };
struct ConversationApproachOperation final {
    ConversationApproachKind kind{};
    std::uint64_t revision{};
    RuntimeActorIdentity actor, listener;
    core::CancellationToken cancellation;
};

// Engine-free owner state. Exactly one player-addressed NPC; cleanup always precedes a replacement.
class ConversationApproachController final {
    enum class Phase : unsigned char { acquire, acquiring, active, refresh, refreshing, release, releasing };
    struct Lease {
        RuntimeConversationHold hold;
        RuntimeActorIdentity listener;
        core::SnapshotClock::time_point expires, hard_end, next_refresh;
        Phase phase{Phase::acquire};
        bool desired{}, closing{};
        std::uint64_t revision{};
    };
    std::optional<Lease> lease_;
    std::optional<RuntimeConversationHold> suppressed_;
    std::uint64_t revision_{};
public:
    void reset() noexcept { lease_.reset();suppressed_.reset();++revision_; }
    void update(std::span<const ConversationApproachCandidate> candidates,core::SnapshotClock::time_point now) {
        if (suppressed_ && (suppressed_->cancellation.is_cancelled() || std::ranges::none_of(candidates,[&](const auto& value) {
            return value.hold.approach_player && !value.hold.cancellation.is_cancelled() && same_approach_actor(value.hold.actor,suppressed_->actor);
        }))) suppressed_.reset();
        const ConversationApproachCandidate* chosen{};
        for (const auto& candidate:candidates) if (candidate.eligible() &&
            (!suppressed_ || !same_approach_actor(candidate.hold.actor,suppressed_->actor))) { chosen=&candidate;break; }
        if (!lease_ && chosen) lease_=Lease{chosen->hold,chosen->listener,now+std::chrono::seconds{90},
            now+std::chrono::seconds{300},now,Phase::acquire,true,false,++revision_};
        if (!lease_) return;
        auto& lease=*lease_;
        lease.desired=!lease.closing && chosen && same_approach_actor(chosen->hold.actor,lease.hold.actor) &&
            same_approach_actor(chosen->listener,lease.listener);
        if (lease.desired) {
            // A replacement player turn cancels its predecessor; renew only that newly owned turn.
            if (lease.hold.cancellation.is_cancelled())
                lease.expires=std::min(now+std::chrono::seconds{90},lease.hard_end);
            lease.hold=chosen->hold;
        }
        if (now>=lease.expires || now>=lease.hard_end) {
            suppressed_=lease.hold;lease.desired=false;
        }
        if (!lease.desired || lease.hold.cancellation.is_cancelled()) {
            lease.closing=true;
            if (lease.phase!=Phase::acquiring && lease.phase!=Phase::refreshing && lease.phase!=Phase::releasing)
                lease.phase=Phase::release;
        }
    }
    [[nodiscard]] bool install_current(const ConversationApproachOperation& operation,core::SnapshotClock::time_point now) const noexcept {
        return lease_ && operation.kind!=ConversationApproachKind::release && lease_->revision==operation.revision &&
            lease_->desired && !lease_->closing && !operation.cancellation.is_cancelled() &&
            !lease_->hold.cancellation.is_cancelled() && now<lease_->expires && now<lease_->hard_end &&
            same_approach_actor(lease_->hold.actor,operation.actor) && same_approach_actor(lease_->listener,operation.listener);
    }
    [[nodiscard]] std::optional<ConversationApproachOperation> next(core::SnapshotClock::time_point now) {
        if (!lease_) return {};
        auto& lease=*lease_;
        if (lease.phase==Phase::active && now>=lease.next_refresh) lease.phase=Phase::refresh;
        ConversationApproachKind kind{};
        if (lease.phase==Phase::release) {kind=ConversationApproachKind::release;lease.phase=Phase::releasing;}
        else if (lease.phase==Phase::acquire) {kind=ConversationApproachKind::acquire;lease.phase=Phase::acquiring;}
        else if (lease.phase==Phase::refresh) {kind=ConversationApproachKind::refresh;lease.phase=Phase::refreshing;}
        else return {};
        return ConversationApproachOperation{kind,lease.revision,lease.hold.actor,lease.listener,lease.hold.cancellation};
    }
    void complete(const ConversationApproachOperation& operation,bool success,core::SnapshotClock::time_point now) {
        if (!lease_ || lease_->revision!=operation.revision || !same_approach_actor(lease_->hold.actor,operation.actor)) return;
        if (operation.kind==ConversationApproachKind::release) {
            if (success) lease_.reset();
            else lease_->phase=Phase::release; // Never reuse an alias with unconfirmed cleanup.
            return;
        }
        if (!success || !install_current(operation,now)) {
            if (!success && lease_->desired && !operation.cancellation.is_cancelled()) suppressed_=lease_->hold;
            lease_->closing=true;lease_->phase=Phase::release;
        } else {lease_->phase=Phase::active;lease_->next_refresh=now+std::chrono::seconds{1};}
    }
    [[nodiscard]] bool idle() const noexcept {return !lease_;}
};
} // namespace synth::runtime
