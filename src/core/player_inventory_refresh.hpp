#pragma once

#include <chrono>
#include <cstdint>

namespace synth::core {

// Game-pump-owned cadence. Workers expose only their latest acknowledged inventory frame.
class PlayerInventoryRefresh final {
public:
    using Clock = std::chrono::steady_clock;

    void reset(std::uint64_t generation, Clock::time_point now) noexcept {
        *this = {};
        generation_ = generation;
        revision_ = 1;
        dirty_ = true;
        due_at_ = now + std::chrono::seconds{2};
        reconcile_at_ = now + std::chrono::seconds{30};
    }

    void mark_dirty(std::uint64_t generation, Clock::time_point now) noexcept {
        if (generation == 0 || generation != generation_) return;
        if (!dirty_ || (pending_frame_ != 0 && pending_revision_ == revision_))
            due_at_ = now + std::chrono::milliseconds{200};
        ++revision_;
        dirty_ = true;
    }

    [[nodiscard]] bool due(std::uint64_t generation, Clock::time_point now,
                           std::uint64_t acknowledged_frame) noexcept {
        if (generation == 0 || generation != generation_) return false;
        if (pending_frame_ != 0 && acknowledged_frame >= pending_frame_) {
            if (pending_revision_ == revision_) dirty_ = false;
            pending_frame_ = 0;
            reconcile_at_ = now + std::chrono::seconds{30};
        }
        if (!dirty_ && now >= reconcile_at_) {
            ++revision_;
            dirty_ = true;
            due_at_ = now;
        }
        return dirty_ && now >= due_at_;
    }

    // Reserve before native capture, so failures and rejected delivery cannot retry every frame.
    [[nodiscard]] std::uint64_t attempt(Clock::time_point now) noexcept {
        due_at_ = now + std::chrono::seconds{2};
        return revision_;
    }

    void submitted(std::uint64_t revision, std::uint64_t frame) noexcept {
        pending_revision_ = revision;
        pending_frame_ = frame;
    }

private:
    std::uint64_t generation_{};
    std::uint64_t revision_{};
    std::uint64_t pending_revision_{};
    std::uint64_t pending_frame_{};
    bool dirty_{};
    Clock::time_point due_at_{};
    Clock::time_point reconcile_at_{};
};

} // namespace synth::core
