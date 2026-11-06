#pragma once
#include "core/cancellation.hpp"
#include "core/snapshot.hpp"
#include "runtime/fallout_runtime.hpp"
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace synth::runtime {
// Reset acknowledgement is followed by independent native alias/package observations.
struct WaitResetProof final {
    bool aliases_observed{}, packages_observed{}, owned_package{};
    std::size_t aliased_references{};
    [[nodiscard]] bool cleared() const noexcept {
        return aliases_observed && aliased_references==0 && packages_observed && !owned_package;
    }
};
class WaitResetClock final {
public:
    [[nodiscard]] bool observe(const WaitResetProof& proof,core::SnapshotClock::time_point now) noexcept {
        if(!proof.cleared()) {ready_after_.reset();return false;}
        if(!ready_after_) {ready_after_=now+std::chrono::milliseconds{100};return false;}
        return now>=*ready_after_;
    }
private:
    std::optional<core::SnapshotClock::time_point> ready_after_;
};
// Copied native state and exact script alias receipts; a VM return or selected AI package is insufficient.
struct WaitPhysicalProof final {
    bool actor_available{}, target_owned{}, package_owned{}, aliases_clear{}, restrained{}, same_cell{};
    double distance{}, speed{};
    bool package_observed{};
    std::uint32_t owned_package{};
    [[nodiscard]] bool stationary() const noexcept {
        return same_cell && std::isfinite(distance) && distance>=0 && distance<=64 && std::isfinite(speed) && speed>=0 && speed<=5;
    }
    [[nodiscard]] bool held() const noexcept {
        return actor_available && target_owned && package_observed && package_owned && owned_package!=0 && stationary();
    }
    [[nodiscard]] bool released() const noexcept {return actor_available && aliases_clear && package_observed && owned_package==0;}
};
// Near Package Start has no command-admission anchor. Establish one once its
// exact owned package is active; walking while the bounded VM install runs is
// not an escape from a hold that has not started yet.
class WaitPositionGuard final {
public:
    [[nodiscard]] bool observe_active_package(const WaitPhysicalProof& proof,bool owner_current) noexcept {
        if(anchored_ || !owner_current || !proof.actor_available || !proof.same_cell ||
            !proof.target_owned || !proof.package_observed || !proof.package_owned || proof.owned_package==0 ||
            !std::isfinite(proof.distance) || proof.distance<0)return false;
        anchored_=true;return true;
    }
    [[nodiscard]] bool interrupted(const WaitPhysicalProof& proof) const noexcept {
        return !proof.actor_available || !proof.same_cell || !std::isfinite(proof.distance) ||
            proof.distance<0 || (anchored_&&proof.distance>64);
    }
    [[nodiscard]] bool anchored() const noexcept {return anchored_;}
private:
    bool anchored_{};
};
class WaitSettleClock final {
public:
    [[nodiscard]] bool observe(const WaitPhysicalProof& proof,core::SnapshotClock::time_point now) noexcept {
        if(!proof.held()) {ready_after_.reset();return false;}
        if(!ready_after_) {ready_after_=now+std::chrono::milliseconds{100};return false;}
        return now>=*ready_after_;
    }
private:
    std::optional<core::SnapshotClock::time_point> ready_after_;
};
inline bool same_wait_request(const RuntimeActionRequest& a,const RuntimeActionRequest& b) noexcept {
    const auto fold=[](unsigned char ch){return ch>='A'&&ch<='Z'?ch+('a'-'A'):ch;};
    return (a.name==RuntimeActionName::wait_here || a.name==RuntimeActionName::release_wait) && a.name==b.name &&
        !a.action_id.empty() && a.action_id==b.action_id && a.actor.form_id==b.actor.form_id &&
        a.actor.playthrough_id==b.actor.playthrough_id &&
        std::ranges::equal(a.actor.origin_plugin,b.actor.origin_plugin,[&](auto x,auto y){return fold(x)==fold(y);}) &&
        a.cancellation.generation()==b.cancellation.generation();
}
// Ninety seconds of unpaused gameplay, starting only after the alias install is confirmed.
class WaitHoldClock final {
public:
    void start(core::SnapshotClock::time_point now) noexcept { end_ = now + std::chrono::seconds{90}; }
    void pause(core::SnapshotClock::duration duration) noexcept { if (end_) *end_ += duration; }
    bool started() const noexcept { return end_.has_value(); }
    bool expired(core::SnapshotClock::time_point now) const noexcept { return end_ && now >= *end_; }
private:
    std::optional<core::SnapshotClock::time_point> end_;
};

// One process-unique receipt per VM operation; cancellation must never suppress cleanup operations.
class WaitScriptReceipt final {
public:
    WaitScriptReceipt(std::string ticket, core::CancellationToken cancellation, bool install)
        : ticket_{std::move(ticket)}, cancellation_{std::move(cancellation)}, install_{install} {}
    bool current(std::string_view ticket) const noexcept {
        return !ticket_.empty() && ticket_ == ticket && !done_ && !revoked_ &&
            (!install_ || !cancellation_.is_cancelled());
    }
    void revoke_install() noexcept { if (install_) revoked_ = true; }
    void finish(std::string_view ticket, bool success) noexcept {
        if (ticket_ != ticket || done_) return;
        success_ = success && current(ticket);
        done_ = true;
    }
    bool done() const noexcept { return done_; }
    bool success() const noexcept { return done_ && success_; }
private:
    std::string ticket_;
    core::CancellationToken cancellation_;
    bool install_{}, revoked_{}, done_{}, success_{};
};
}
