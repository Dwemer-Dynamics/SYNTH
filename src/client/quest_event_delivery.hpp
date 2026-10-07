#pragma once

#include "core/snapshot.hpp"
#include "protocol_native/v1_codec.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>

namespace synth::client {

// One generation's immutable captures. Admission never waits; binding/allocation runs on the worker.
class QuestEventDelivery final {
public:
    using Snapshot = std::shared_ptr<const core::RuntimeSnapshot>;
    static constexpr std::size_t capacity = 4;
    static constexpr std::size_t witness_capacity = 128;
    struct Prepared final {
        Snapshot snapshot;
        protocol_native::NativeQuestBatch batch;
    };

    explicit QuestEventDelivery(core::RuntimeGeneration generation) : generation_{generation} {}
    void stop() noexcept { stopped_.store(true, std::memory_order_release); }
    [[nodiscard]] bool pending() const noexcept { return pending_.load(std::memory_order_acquire) != 0; }
    [[nodiscard]] bool full() const noexcept { return pending_.load(std::memory_order_acquire) >= capacity; }
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t unobserved() const noexcept { return unobserved_.load(std::memory_order_relaxed); }

    [[nodiscard]] bool enqueue(Snapshot snapshot, std::span<const core::QuestEvent> events) {
        if (!snapshot || snapshot->variant() != core::RuntimeVariant::flat || snapshot->generation() != generation_ ||
            stopped_.load(std::memory_order_acquire) || events.empty() || events.size() > core::QuestEventMailbox::capacity)
            return false;
        std::unique_lock lock{mutex_, std::try_to_lock};
        if (!lock.owns_lock() || pending_.load(std::memory_order_relaxed) == capacity) {
            dropped_.fetch_add(events.size(), std::memory_order_relaxed);
            return false;
        }
        if (stopped_.load(std::memory_order_acquire) || !same_player(*snapshot) || serial_ == UINT64_MAX) return false;
        if (!owner_) owner_ = snapshot;
        auto& slot = slots_[(head_ + pending_.load(std::memory_order_relaxed)) % capacity];
        slot.snapshot = std::move(snapshot);
        std::ranges::copy(events, slot.events.begin());
        slot.size = events.size();
        slot.serial = ++serial_;
        pending_.fetch_add(1, std::memory_order_release);
        return true;
    }

    // Only call after this exact snapshot's context acknowledgement, not after capture or send.
    void acknowledge_journal(Snapshot snapshot, std::uint64_t sequence) {
        if (!snapshot || snapshot->variant() != core::RuntimeVariant::flat || snapshot->generation() != generation_ ||
            sequence == 0 || sequence > 9'007'199'254'740'991ULL) return;
        std::scoped_lock lock{mutex_};
        if (stopped_.load(std::memory_order_acquire) || !same_player(*snapshot) || sequence <= acknowledged_sequence_) return;
        if (!owner_) owner_ = snapshot;
        acknowledged_sequence_ = sequence;
        if (!fresh(*snapshot)) return;
        for (const auto& quest : snapshot->active_quests()) {
            if (!visible(quest)) continue;
            auto existing = std::ranges::find_if(witnesses_, [&](const auto& witness) {
                return witness.form_id == quest.form_id;
            });
            if (existing == witnesses_.end()) existing = witnesses_.begin() + (next_witness_++ % witness_capacity);
            *existing = {quest.form_id, quest.origin_plugin, sequence};
        }
    }

    // Repeated calls return the same immutable batch and capture until an explicit terminal disposition.
    [[nodiscard]] std::shared_ptr<const Prepared> prepare() {
        std::scoped_lock lock{mutex_};
        if (stopped_.load(std::memory_order_acquire)) return {};
        if (prepared_) return prepared_;
        while (pending_.load(std::memory_order_relaxed) != 0) {
            const auto& slot = slots_[head_];
            auto candidate = std::make_shared<Prepared>();
            candidate->snapshot = slot.snapshot;
            candidate->batch.batch_id = "quest:" + std::to_string(slot.serial);
            candidate->batch.events.reserve(slot.size);
            for (std::size_t i = 0; i < slot.size; ++i) {
                const auto& event = slot.events[i];
                if (!event.valid()) { dropped_.fetch_add(1, std::memory_order_relaxed); continue; }
                const auto& quests = slot.snapshot->active_quests();
                const auto current = std::ranges::find_if(quests, [&](const auto& quest) {
                    return quest.form_id == event.form_id && visible(quest);
                });
                if (fresh(*slot.snapshot) && current != quests.end()) {
                    candidate->batch.events.push_back({event, current->origin_plugin, 0});
                    continue;
                }
                const auto previous = std::ranges::find_if(witnesses_, [&](const auto& witness) {
                    return witness.form_id == event.form_id;
                });
                const auto known = std::ranges::find_if(quests, [&](const auto& quest) { return quest.form_id == event.form_id; });
                if (previous != witnesses_.end() && (known == quests.end() || known->origin_plugin == previous->origin_plugin))
                    candidate->batch.events.push_back({event, previous->origin_plugin, previous->sequence});
                else unobserved_.fetch_add(1, std::memory_order_relaxed);
            }
            if (!candidate->batch.events.empty()) {
                prepared_ = std::move(candidate);
                return stopped_.load(std::memory_order_acquire) ? nullptr : prepared_;
            }
            pop();
        }
        return {};
    }

    // A stale completion cannot remove another batch; transient failures must not call finish.
    [[nodiscard]] bool finish(const std::shared_ptr<const Prepared>& batch, bool acknowledged) {
        std::scoped_lock lock{mutex_};
        if (stopped_.load(std::memory_order_acquire) || !batch || batch != prepared_) return false;
        if (!acknowledged) dropped_.fetch_add(batch->batch.events.size(), std::memory_order_relaxed);
        prepared_.reset();
        pop();
        return true;
    }

private:
    struct Slot {
        Snapshot snapshot;
        std::array<core::QuestEvent, core::QuestEventMailbox::capacity> events{};
        std::size_t size{};
        std::uint64_t serial{};
    };
    struct Witness { std::uint32_t form_id{}; std::string origin_plugin; std::uint64_t sequence{}; };
    [[nodiscard]] bool same_player(const core::RuntimeSnapshot& snapshot) const noexcept {
        return !owner_ || (owner_->player().form_id() == snapshot.player().form_id() &&
            owner_->player().origin_plugin() == snapshot.player().origin_plugin() &&
            owner_->player().playthrough_id() == snapshot.player().playthrough_id());
    }
    [[nodiscard]] static bool fresh(const core::RuntimeSnapshot& snapshot) noexcept {
        return snapshot.active_quests_observation() == "complete" || snapshot.active_quests_observation() == "partial";
    }
    [[nodiscard]] static bool visible(const core::QuestSnapshot& quest) noexcept {
        return quest.active_objectives > 0 && (quest.objectives_observation == "complete" || quest.objectives_observation == "partial");
    }
    void pop() {
        slots_[head_] = {};
        head_ = (head_ + 1) % capacity;
        pending_.fetch_sub(1, std::memory_order_release);
    }

    const core::RuntimeGeneration generation_;
    std::atomic_bool stopped_{};
    std::atomic_size_t pending_{};
    std::atomic_uint64_t dropped_{}, unobserved_{};
    std::mutex mutex_;
    Snapshot owner_;
    std::array<Slot, capacity> slots_{};
    std::size_t head_{}, next_witness_{};
    std::uint64_t serial_{}, acknowledged_sequence_{};
    std::array<Witness, witness_capacity> witnesses_{};
    std::shared_ptr<const Prepared> prepared_;
};

} // namespace synth::client
