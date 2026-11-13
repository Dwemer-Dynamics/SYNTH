#pragma once

#include "core/snapshot.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <optional>
#include <ranges>
#include <vector>

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

// A teammate copied from a periodic scene that showed it and the player in this encounter's combat.
struct CombatParticipant final {
    std::uint32_t form_id{};
    std::string origin_plugin;
    std::string playthrough_id;
    std::uint64_t game_time_ticks{};
    SnapshotClock::time_point observed_at;
    bool operator==(const CombatParticipant&) const = default;
};

struct PlayerEventTransition final {
    std::uint64_t serial{};
    PlayerEventKind kind{};
    PlayerEventSample before;
    PlayerEventSample after;
    std::vector<CombatParticipant> participants; // combat_end only; empty means participation is unproven.
    bool operator==(const PlayerEventTransition&) const = default;
};

// Game-thread-owned FIFO. Copy a transition to a worker; acknowledge its serial back on the game thread.
// Sampling never waits for the response lane, and later samples cannot replace retained retry evidence.
class PlayerEventSampler final {
public:
    static constexpr std::size_t capacity = 16;
    static constexpr std::size_t participant_capacity = 8;

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
    void unavailable() noexcept { previous_.reset(); end_encounter(); }

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
            end_encounter();
        }
        if (owner_ && same_owner(*owner_) &&
            (sample.game_time_ticks < owner_->game_time_ticks || sample.level < owner_->level)) {
            // A timeline discontinuity is not a combat-end event or an ordinary level decrease.
            discard_pending();
            previous_.reset();
            end_encounter();
        }
        owner_ = sample;
        const auto was_pending = size_;
        if (previous_) {
            // Frozen Dialectic's RPG tick appends level-up before combat-end when both changed.
            if (sample.level > previous_->level) enqueue(PlayerEventKind::level_up, *previous_, sample);
            if (previous_->in_combat && !sample.in_combat) enqueue(PlayerEventKind::combat_end, *previous_, sample);
        }
        if (!sample.in_combat) end_encounter();
        else if (!previous_ || !previous_->in_combat) {
            end_encounter();
            encounter_start_ = sample.observed_at;
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
        if (accepted && retain_reaction && reactions_.size() < capacity &&
            (slots_[head_]->kind != PlayerEventKind::combat_end || serial > consumed_combat_serial_))
            reactions_.push_back(*slots_[head_]);
        if (!accepted) ++dropped_;
        slots_[head_].reset();
        head_ = (head_ + 1) % capacity;
        --size_;
        return true;
    }

    // Game thread: record teammates a periodic scene shows fighting while this owner's sampled encounter is open.
    // Nothing is inferred from later teammate status; the bounded first observations are copied unchanged.
    void observe_participants(const RuntimeSnapshot& scene) {
        if (!encounter_start_ || !previous_ || !previous_->in_combat || scene.generation() != generation_ ||
            scene.variant() != variant_ || scene.player().form_id() != previous_->form_id ||
            scene.player().origin_plugin() != previous_->origin_plugin ||
            scene.player().playthrough_id() != previous_->playthrough_id || !scene.player().in_combat() ||
            scene.captured_at() < *encounter_start_ || scene.game_time_ticks() == 0 ||
            scene.game_time_ticks() > RuntimeGeneration::maximum_wire_value) return;
        for (const auto& actor : scene.actors()) {
            if (participants_.size() == participant_capacity) return;
            if (!actor.teammate() || !actor.in_combat() || !actor.alive() || actor.form_id() == 0 ||
                actor.form_id() == previous_->form_id || actor.origin_plugin().empty() || actor.origin_plugin().size() > 260 ||
                actor.playthrough_id() != previous_->playthrough_id ||
                std::ranges::any_of(participants_, [&](const auto& old) { return old.form_id == actor.form_id(); })) continue;
            participants_.push_back({actor.form_id(), actor.origin_plugin(), actor.playthrough_id(),
                scene.game_time_ticks(), scene.captured_at()});
        }
    }

    // Reactions have their own bounded lifetime; consuming one never deletes passive history evidence.
    [[nodiscard]] const PlayerEventTransition* reaction(SnapshotClock::time_point now, bool include_combat_end = true) {
        expire(now);
        const auto found = std::ranges::find_if(reactions_, [&](const auto& value) {
            return include_combat_end || value.kind != PlayerEventKind::combat_end;
        });
        return found == reactions_.end() ? nullptr : &*found;
    }
    [[nodiscard]] bool consume_reaction(std::uint64_t serial) {
        const auto found = std::ranges::find(reactions_, serial, &PlayerEventTransition::serial);
        if (found == reactions_.end()) return false;
        reactions_.erase(found);
        return true;
    }
    // Newest retained combat end; its older duplicates are consumed with it when it is decided.
    [[nodiscard]] const PlayerEventTransition* latest_combat_end(SnapshotClock::time_point now) {
        expire(now);
        const auto found = std::ranges::find(reactions_.rbegin(), reactions_.rend(), PlayerEventKind::combat_end,
                                             &PlayerEventTransition::kind);
        return found == reactions_.rend() ? nullptr : &*found;
    }
    std::size_t consume_combat_ends_through(std::uint64_t serial) {
        consumed_combat_serial_ = std::max(consumed_combat_serial_, serial);
        return std::erase_if(reactions_, [&](const auto& value) {
            return value.kind == PlayerEventKind::combat_end && value.serial <= serial;
        });
    }
    [[nodiscard]] bool in_combat() const noexcept { return previous_ && previous_->in_combat; }
    // Superseded ends remain history; delayed acknowledgements cannot turn them back into remarks.
    void discard_combat_reactions() { (void)consume_combat_ends_through(serial_); }

private:
    void expire(SnapshotClock::time_point now) {
        while (!reactions_.empty() && (now < reactions_.front().after.observed_at ||
            now - reactions_.front().after.observed_at > std::chrono::seconds{60})) reactions_.pop_front();
    }
    void end_encounter() noexcept {
        encounter_start_.reset();
        participants_.clear();
    }

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
        std::vector<CombatParticipant> participants;
        if (kind == PlayerEventKind::combat_end)
            for (const auto& value : participants_)
                if (value.observed_at <= after.observed_at && value.game_time_ticks <= after.game_time_ticks) participants.push_back(value);
        slots_[(head_ + size_) % capacity] = PlayerEventTransition{++serial_, kind, before, after, std::move(participants)};
        ++size_;
    }

    RuntimeGeneration generation_;
    RuntimeVariant variant_;
    std::optional<SnapshotClock::time_point> next_sample_;
    std::optional<PlayerEventSample> owner_;
    std::optional<PlayerEventSample> previous_;
    std::optional<SnapshotClock::time_point> encounter_start_;
    std::vector<CombatParticipant> participants_;
    std::array<std::optional<PlayerEventTransition>, capacity> slots_;
    std::deque<PlayerEventTransition> reactions_;
    std::size_t head_{};
    std::size_t size_{};
    std::uint64_t serial_{};
    std::uint64_t consumed_combat_serial_{};
    std::uint64_t dropped_{};
};

}  // namespace synth::core
