#pragma once

#include <chrono>

namespace synth::input {

enum class PressGestureEvent : unsigned char {
    none,
    tap,
    double_tap,
    hold,
};

class PressGesture final {
public:
    using Clock = std::chrono::steady_clock;

    explicit PressGesture(std::chrono::milliseconds double_tap_window = std::chrono::milliseconds{300},
                          std::chrono::milliseconds hold_threshold = std::chrono::milliseconds{600})
        : double_tap_window_(double_tap_window), hold_threshold_(hold_threshold) {}

    // Converts a physical button level into one terminal gesture per press sequence.
    [[nodiscard]] PressGestureEvent update(bool down, Clock::time_point now) noexcept {
        if (down && !down_) {
            down_ = true;
            pressed_at_ = now;
            hold_emitted_ = false;
        }
        if (down_ && down && !hold_emitted_ && now - pressed_at_ >= hold_threshold_) {
            hold_emitted_ = true;
            pending_tap_ = false;
            return PressGestureEvent::hold;
        }
        if (!down && down_) {
            down_ = false;
            if (hold_emitted_) return PressGestureEvent::none;
            if (pending_tap_ && now - first_release_at_ <= double_tap_window_) {
                pending_tap_ = false;
                return PressGestureEvent::double_tap;
            }
            pending_tap_ = true;
            first_release_at_ = now;
        }
        if (!down && pending_tap_ && now - first_release_at_ > double_tap_window_) {
            pending_tap_ = false;
            return PressGestureEvent::tap;
        }
        return PressGestureEvent::none;
    }

    void reset() noexcept {
        down_ = false;
        hold_emitted_ = false;
        pending_tap_ = false;
        pressed_at_ = {};
        first_release_at_ = {};
    }

private:
    std::chrono::milliseconds double_tap_window_;
    std::chrono::milliseconds hold_threshold_;
    Clock::time_point pressed_at_{};
    Clock::time_point first_release_at_{};
    bool down_{};
    bool hold_emitted_{};
    bool pending_tap_{};
};

}  // namespace synth::input
