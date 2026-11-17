#pragma once

// Engine-free supervision of one Take a Seat order and the seats that recently failed. The flat adapter
// feeds it copied observations on the game thread; it never touches an actor. Seat choice and the
// package that walks the NPC stay with the adapter. Ported in behaviour, not code, from the CHIM 4
// SeatOrder state machine.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace synth::runtime {

struct SeatLimits final {
    std::chrono::seconds order_duration{300}, no_progress{5}, max_walk{30}, sit_down{5}, unreachable_for{600};
    float arrived_distance{128.0F}, progress_step{64.0F};
    std::uint32_t max_attempts{3};
};

enum class SeatKind : unsigned char { furniture, ground, wait };
enum class SeatStep : unsigned char { keep, retry, finish };
enum class SeatReason : unsigned char {
    none, duration, player_left, seat_gone, seat_taken, seated, not_seated, no_progress, walk_timeout
};

// One copied game-thread observation of the NPC and its current seat.
// player_left: the adapter saw the NPC and player separated by cell or beyond the leave distance.
struct SeatSample final {
    bool player_left{}, seat_present{}, seated_on_seat{};
    std::uint32_t free_markers{};
    float distance{};
};

class SeatOrderWatch final {
public:
    using Clock = std::chrono::steady_clock;
    explicit SeatOrderWatch(SeatLimits limits = {}) : limits_{limits} {}

    void begin(Clock::time_point now) {
        kind_ = SeatKind::furniture;
        seat_ = 0;
        seated_ = false;
        ground_tried_ = false;
        attempts_ = 0;
        tried_.clear();
        arrived_.reset();
        order_start_ = now;
        started_ = true;
    }

    // The NPC was sent to a new seat; progress is measured from its distance to that seat.
    void target(SeatKind kind, std::uint32_t seat_form, float distance, Clock::time_point now) {
        kind_ = kind;
        seat_ = seat_form;
        seated_ = false;
        arrived_.reset();
        attempt_start_ = last_progress_ = now;
        best_distance_ = distance;
        if (kind == SeatKind::furniture) {
            ++attempts_;
            tried_.push_back(seat_form);
        } else if (kind == SeatKind::ground) ground_tried_ = true;
    }

    [[nodiscard]] SeatStep check(const SeatSample& sample, Clock::time_point now, SeatReason& reason) {
        reason = SeatReason::none;
        if (!started_ || now - order_start_ >= limits_.order_duration) { reason = SeatReason::duration; return SeatStep::finish; }
        if (sample.player_left) { reason = SeatReason::player_left; return SeatStep::finish; }
        // A waiting or already seated NPC needs no further watch; its package seats it again after a short stand.
        if (kind_ == SeatKind::wait || seated_) return SeatStep::keep;
        if (!sample.seat_present) { reason = SeatReason::seat_gone; return SeatStep::retry; }
        if (sample.seated_on_seat) { seated_ = true; reason = SeatReason::seated; return SeatStep::keep; }
        if (kind_ == SeatKind::furniture && sample.free_markers == 0) { reason = SeatReason::seat_taken; return SeatStep::retry; }
        if (sample.distance < best_distance_ - limits_.progress_step) { best_distance_ = sample.distance; last_progress_ = now; }
        if (sample.distance <= limits_.arrived_distance) {
            if (!arrived_) arrived_ = now;
            if (now - *arrived_ >= limits_.sit_down) { reason = SeatReason::not_seated; return SeatStep::retry; }
            return SeatStep::keep;
        }
        arrived_.reset();
        if (now - last_progress_ >= limits_.no_progress) { reason = SeatReason::no_progress; return SeatStep::retry; }
        if (now - attempt_start_ >= limits_.max_walk) { reason = SeatReason::walk_timeout; return SeatStep::retry; }
        return SeatStep::keep;
    }

    // A seat someone else took may be free again soon; one the NPC could not reach is skipped for a while.
    void mark_failed(SeatReason reason, Clock::time_point now) {
        if (kind_ == SeatKind::furniture && seat_ != 0 && reason != SeatReason::seat_taken)
            unreachable_[seat_] = now + limits_.unreachable_for;
    }

    // Seats already tried in this order plus recently unreachable ones; expired entries are dropped here.
    [[nodiscard]] std::vector<std::uint32_t> skipped(Clock::time_point now) {
        std::erase_if(unreachable_, [now](const auto& entry) { return entry.second <= now; });
        auto skip = tried_;
        for (const auto& entry : unreachable_)
            if (std::ranges::find(skip, entry.first) == skip.end()) skip.push_back(entry.first);
        return skip;
    }

    // After max_attempts seats the NPC takes the ground fallback, then waits there.
    [[nodiscard]] SeatKind next_kind() const noexcept {
        if (attempts_ < limits_.max_attempts) return SeatKind::furniture;
        return ground_tried_ ? SeatKind::wait : SeatKind::ground;
    }
    [[nodiscard]] SeatKind kind() const noexcept { return kind_; }
    [[nodiscard]] std::uint32_t attempts() const noexcept { return attempts_; }

private:
    SeatLimits limits_;
    SeatKind kind_{SeatKind::furniture};
    std::uint32_t seat_{}, attempts_{};
    bool seated_{}, ground_tried_{}, started_{};
    float best_distance_{};
    Clock::time_point order_start_{}, attempt_start_{}, last_progress_{};
    std::optional<Clock::time_point> arrived_;
    std::vector<std::uint32_t> tried_;
    std::unordered_map<std::uint32_t, Clock::time_point> unreachable_;
};

}  // namespace synth::runtime
