#pragma once

#include "core/snapshot.hpp"

#include <array>
#include <deque>
#include <optional>

namespace synth::core {

// Scalars copied on the game thread, not an actor pointer or an inferred combat outcome.
struct PlayerEventSample final {
    RuntimeGeneration generation;
    RuntimeVariant variant{RuntimeVariant::flat};
    std::uint32_t form_id{};
    std::string origin_plugin;
    std::string playthrough_id;
    SnapshotClock::time_point observed_at;
    std::uint64_t game_time_ticks{};
    std::int16_t level{};
    bool in_combat{};
    bool operator==(const PlayerEventSample&) const = default;
};

enum class PlayerEventKind : unsigned char { level_up, combat_end };

struct PlayerEventTransition final {
    std::uint64_t serial{};
    PlayerEventKind kind{};
    PlayerEventSample before;
    PlayerEventSample after;
    bool operator==(const PlayerEventTransition&) const = default;
};

// Game-thread-owned FIFO. Copy a transition to a worker; acknowledge its serial back on the game thread.
// Sampling never waits for the response lane, and later samples cannot replace retained retry evidence.
class PlayerEventSampler final {
public:
    static constexpr std::size_t capacity = 16;

    PlayerEventSampler(RuntimeGeneration generation, RuntimeVariant variant)
        : generation_{generation}, variant_{variant} {
        if (!generation.valid()) throw std::invalid_argument{"player sampler requires a generation"};
    }

    [[nodiscard]] bool due(SnapshotClock::time_point now) noexcept {
        if (next_sample_ && now < *next_sample_) return false;
        next_sample_ = now + std::chrono::seconds{1};
        return true;
    }

    // Missing/dead/loading observations break continuity; ordinary menu pauses do not call this.
    void unavailable() noexcept { previous_.reset(); }

    [[nodiscard]] std::size_t observe(PlayerEventSample sample) {
        if (sample.generation != generation_ || sample.variant != variant_) return 0;
        if (sample.form_id == 0 || sample.origin_plugin.empty() || sample.origin_plugin.size() > 260 ||
            sample.playthrough_id.empty() || sample.playthrough_id.size() > 128 || sample.level < 1 ||
            sample.game_time_ticks == 0 || sample.game_time_ticks > RuntimeGeneration::maximum_wire_value) {
            unavailable();
            return 0;
        }
        // Retain owner independently of the baseline so an unavailable frame cannot hide an identity switch.
        const auto same_owner = [&](const PlayerEventSample& old) {
            return old.form_id == sample.form_id && old.origin_plugin == sample.origin_plugin &&
                old.playthrough_id == sample.playthrough_id;
        };
        if (owner_ && sample.observed_at <= owner_->observed_at) return 0;
        if (owner_ && !same_owner(*owner_)) {
            discard_pending();
            previous_.reset();
        }
        if (owner_ && same_owner(*owner_) &&
            (sample.game_time_ticks < owner_->game_time_ticks || sample.level < owner_->level)) {
            // A timeline discontinuity is not a combat-end event or an ordinary level decrease.
            discard_pending();
            previous_.reset();
        }
        owner_ = sample;
        const auto was_pending = size_;
        if (previous_) {
            // Frozen Dialectic's RPG tick appends level-up before combat-end when both changed.
            if (sample.level > previous_->level) enqueue(PlayerEventKind::level_up, *previous_, sample);
            if (previous_->in_combat && !sample.in_combat) enqueue(PlayerEventKind::combat_end, *previous_, sample);
        }
        previous_ = std::move(sample);
        return size_ - was_pending;
    }

    [[nodiscard]] const PlayerEventTransition* front() const noexcept {
        return size_ == 0 ? nullptr : &*slots_[head_];
    }
    [[nodiscard]] std::size_t pending() const noexcept { return size_; }
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }

    // A delayed completion for another item/owner cannot consume the current head.
    [[nodiscard]] bool acknowledge(std::uint64_t serial, bool accepted = true, bool retain_reaction = false) {
        if (size_ == 0 || slots_[head_]->serial != serial) return false;
        if (accepted && retain_reaction && reactions_.size() < capacity) reactions_.push_back(*slots_[head_]);
        if (!accepted) ++dropped_;
        slots_[head_].reset();
        head_ = (head_ + 1) % capacity;
        --size_;
        return true;
    }

    // Reactions have their own bounded lifetime; consuming one never deletes passive history evidence.
    [[nodiscard]] const PlayerEventTransition* reaction(SnapshotClock::time_point now) {
        while (!reactions_.empty() && (now < reactions_.front().after.observed_at ||
            now - reactions_.front().after.observed_at > std::chrono::seconds{60})) reactions_.pop_front();
        return reactions_.empty() ? nullptr : &reactions_.front();
    }
    [[nodiscard]] bool consume_reaction(std::uint64_t serial) {
        if (reactions_.empty() || reactions_.front().serial != serial) return false;
        reactions_.pop_front();
        return true;
    }

private:
    void discard_pending() noexcept {
        reactions_.clear();
        dropped_ += size_;
        for (auto& slot : slots_) slot.reset();
        size_ = 0;
        head_ = 0;
    }

    void enqueue(PlayerEventKind kind, const PlayerEventSample& before, const PlayerEventSample& after) {
        if (size_ == capacity || serial_ == RuntimeGeneration::maximum_wire_value) {
            ++dropped_;
            return;
        }
        slots_[(head_ + size_) % capacity] = PlayerEventTransition{++serial_, kind, before, after};
        ++size_;
    }

    RuntimeGeneration generation_;
    RuntimeVariant variant_;
    std::optional<SnapshotClock::time_point> next_sample_;
    std::optional<PlayerEventSample> owner_;
    std::optional<PlayerEventSample> previous_;
    std::array<std::optional<PlayerEventTransition>, capacity> slots_;
    std::deque<PlayerEventTransition> reactions_;
    std::size_t head_{};
    std::size_t size_{};
    std::uint64_t serial_{};
    std::uint64_t dropped_{};
};

}  // namespace synth::core
