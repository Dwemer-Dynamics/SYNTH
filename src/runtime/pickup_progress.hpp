#pragma once

#include "runtime/fallout_runtime.hpp"
#include <stdexcept>
#include <utility>

namespace synth::runtime {

// Game-thread-only control state; contains copied request data, never retained engine pointers.
// The adapter must provide fresh exact-identity inspection and revalidate immediately before each mutation.
class PickupProgress final {
public:
    enum class Inspection : unsigned char { unavailable, rejected, near, far };
    enum class Command : unsigned char { none, install, transfer, observe, restore, discard };
    enum class Outcome : unsigned char { pending, succeeded, failed, cancelled, timed_out, stale };
    enum class Installation : unsigned char { installed, rejected, uncertain };

    explicit PickupProgress(RuntimePickupRequest request) : request_{std::move(request)} {
        if (!request_.cancellation.generation().valid() || !request_.item.valid() ||
            request_.actor.form_id == 0 || request_.actor.origin_plugin.empty() ||
            request_.actor.playthrough_id.empty() || request_.action_id.empty() ||
            request_.deadline <= request_.captured_at) throw std::invalid_argument{"invalid pickup progress owner"};
    }
    PickupProgress(const PickupProgress&) = delete;
    PickupProgress& operator=(const PickupProgress&) = delete;

    [[nodiscard]] const RuntimePickupRequest& request() const noexcept { return request_; }
    // Outcome is publishable only when done(); cleanup may still be owed after a stale discard.
    [[nodiscard]] Outcome outcome() const noexcept { return outcome_; }
    [[nodiscard]] bool done() const noexcept { return phase_ == Phase::terminal; }
    [[nodiscard]] bool cleanup_owed() const noexcept { return cleanup_owed_; }
    [[nodiscard]] bool transfer_attempted() const noexcept { return transfer_attempted_; }

    // At most one command per monotonically increasing pump frame. Stale epochs never request native cleanup.
    [[nodiscard]] Command advance(std::uint64_t frame, core::RuntimeGeneration generation,
                                  core::SnapshotClock::time_point now, Inspection inspection) {
        if (done()) return Command::none;
        if (generation != request_.cancellation.generation()) {
            outcome_ = Outcome::stale; phase_ = Phase::terminal;
            return Command::discard; // Host must retain/report unresolved cleanup; do not dereference old identities.
        }
        if (frame <= last_frame_ || now < request_.captured_at || now < last_time_) return Command::none;
        last_frame_ = frame; last_time_ = now;
        // Cancellation closes admission, not evidence collection for an already attempted mutation.
        // Generation invalidation above still forbids observation against a different loaded world.
        if (!transfer_attempted_ && phase_ != Phase::cleanup && phase_ != Phase::restoring &&
            (request_.cancellation.is_cancelled() || now >= request_.deadline)) {
            stop(request_.cancellation.is_cancelled() ? Outcome::cancelled : Outcome::timed_out);
        }
        if (phase_ == Phase::cleanup) {
            phase_ = Phase::restoring;
            return Command::restore;
        }
        if (phase_ == Phase::observation) {
            phase_ = Phase::observing;
            return Command::observe;
        }
        if (phase_ != Phase::initial && phase_ != Phase::approach) return Command::none;
        if (inspection == Inspection::rejected) { stop(Outcome::failed); return Command::none; }
        if (inspection == Inspection::unavailable) return Command::none;
        if (inspection == Inspection::near) {
            transfer_attempted_ = true; // Latch before calling native code, including throwing/uncertain calls.
            phase_ = Phase::transferring;
            return Command::transfer;
        }
        if (phase_ == Phase::initial) {
            cleanup_owed_ = true; // An installation exception may still have mutated the holder.
            phase_ = Phase::installing;
            return Command::install;
        }
        return Command::none;
    }

    [[nodiscard]] bool installation_finished(Installation result) noexcept {
        if (phase_ != Phase::installing) return false;
        if (result == Installation::installed) phase_ = Phase::approach;
        else {
            if (result == Installation::rejected) cleanup_owed_ = false; // Adapter guarantees no mutation.
            stop(Outcome::failed);
        }
        return true;
    }

    // Even an uncertain native return proceeds to observation, never another transfer attempt.
    [[nodiscard]] bool transfer_finished() noexcept {
        if (phase_ != Phase::transferring) return false;
        phase_ = Phase::observation;
        return true;
    }

    // Verified means exact reference retirement plus whole-stack conservation and a complete owned receipt.
    [[nodiscard]] bool observation_finished(bool verified) noexcept {
        if (phase_ != Phase::observing) return false;
        stop(verified ? Outcome::succeeded : Outcome::failed);
        return true;
    }

    // Busy cleanup remains explicit and may retry on a later frame, even after the action deadline.
    // released means cleared our exact package or proved that the holder is no longer ours.
    [[nodiscard]] bool restoration_finished(bool released) noexcept {
        if (phase_ != Phase::restoring) return false;
        if (released) { cleanup_owed_ = false; phase_ = Phase::terminal; }
        else phase_ = Phase::cleanup;
        return true;
    }

private:
    enum class Phase : unsigned char { initial, installing, approach, transferring, observation,
                                      observing, cleanup, restoring, terminal };
    void stop(Outcome result) noexcept {
        outcome_ = result;
        phase_ = cleanup_owed_ ? Phase::cleanup : Phase::terminal;
    }
    const RuntimePickupRequest request_;
    Phase phase_{Phase::initial};
    Outcome outcome_{Outcome::pending};
    std::uint64_t last_frame_{request_.context_sequence};
    core::SnapshotClock::time_point last_time_{};
    bool cleanup_owed_{}, transfer_attempted_{};
};

} // namespace synth::runtime
