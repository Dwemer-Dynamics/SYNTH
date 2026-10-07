#pragma once
#include "core/cancellation.hpp"
#include "core/snapshot.hpp"
#include <optional>
#include <string>
#include <string_view>

namespace synth::runtime {
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
