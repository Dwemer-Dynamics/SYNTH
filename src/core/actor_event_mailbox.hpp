#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace synth::core {

enum class ActorEventKind : std::uint8_t { death, equipped, unequipped };

// Native scalar evidence, not a witness claim, inferred consumption, or sampled actor state.
struct ActorEvent final {
    ActorEventKind kind{};
    std::uint32_t actor{}, other_actor{}, base_object{}, original_reference{};
    std::uint16_t unique_id{};
    std::chrono::steady_clock::time_point observed_at{}; // Original callback time, never replaced by drain/retry time.
    [[nodiscard]] bool valid() const noexcept {
        if (actor == 0 || actor == UINT32_MAX || other_actor == UINT32_MAX || original_reference == UINT32_MAX) return false;
        if (kind == ActorEventKind::death) return base_object == 0 && original_reference == 0 && unique_id == 0;
        return (kind == ActorEventKind::equipped || kind == ActorEventKind::unequipped) &&
            other_actor == 0 && base_object != 0 && base_object != UINT32_MAX;
    }
    bool operator==(const ActorEvent&) const = default;
};

// Reserve order before forwarding; commit IDs learned from native returns afterward. Every gate acquisition is a single try.
class ActorEventMailbox final {
public:
    static constexpr std::size_t capacity = 32;
    struct Ticket final {
        std::uint64_t owner{}, serial{};
        std::size_t slot{};
        [[nodiscard]] explicit operator bool() const noexcept { return serial != 0; }
    };
    struct Batch final { std::uint64_t owner{}; std::array<ActorEvent,capacity> events{}; std::size_t size{}; };
    void arm() noexcept { epoch_.fetch_or(1,std::memory_order_release); }
    void invalidate() noexcept {
        auto owner=epoch_.load(std::memory_order_relaxed);
        while (!epoch_.compare_exchange_weak(owner,(owner & ~std::uint64_t{1})+2,
            std::memory_order_acq_rel,std::memory_order_relaxed)) {}
    }
    [[nodiscard]] std::uint64_t stamp() const noexcept { return epoch_.load(std::memory_order_acquire); }
    [[nodiscard]] bool is_current(std::uint64_t owner) const noexcept { return (owner & 1) && owner == stamp(); }
    [[nodiscard]] Ticket reserve(std::uint64_t owner) noexcept {
        if (!is_current(owner)) return {};
        if (gate_.test_and_set(std::memory_order_acquire)) { ++dropped_; return {}; }
        const Release release{gate_};
        if (!is_current(owner)) return {};
        if (queue_owner_ != owner) clear(owner);
        if (size_ == capacity) { ++dropped_; return {}; }
        const auto index=(head_+size_)%capacity;
        serial_=(serial_+1)&(UINT64_MAX>>2);
        if (serial_==0) ++serial_;
        slots_[index].state.store(serial_<<2,std::memory_order_release);
        ++size_;
        return {owner,serial_,index};
    }
    [[nodiscard]] bool commit(Ticket ticket, ActorEvent event) noexcept {
        if (!ticket || ticket.slot>=capacity) return false;
        if (!event.valid() || !is_current(ticket.owner)) { discard(ticket); return false; }
        if (gate_.test_and_set(std::memory_order_acquire)) {
            ++dropped_;
            discard(ticket); // Gate-free serial CAS prevents a contended completion from stranding the head.
            return false;
        }
        const Release release{gate_};
        if (!is_current(ticket.owner)) { discard(ticket); return false; }
        auto& slot=slots_[ticket.slot];
        const auto reserved=ticket.serial<<2;
        if (slot.state.load(std::memory_order_acquire)!=reserved) return false;
        slot.event=event; // All payload writes, resets and reads share this gate; old tickets cannot mutate reused slots.
        auto expected=reserved;
        return slot.state.compare_exchange_strong(expected,reserved|ready,std::memory_order_release,std::memory_order_relaxed);
    }
    void discard(Ticket ticket) noexcept {
        if (!ticket || ticket.slot>=capacity) return;
        auto expected=ticket.serial<<2;
        (void)slots_[ticket.slot].state.compare_exchange_strong(expected,expected|discarded,
            std::memory_order_release,std::memory_order_relaxed);
    }
    [[nodiscard]] Batch take() noexcept {
        Batch batch;
        if (gate_.test_and_set(std::memory_order_acquire)) return batch;
        const Release release{gate_};
        batch.owner=stamp();
        if (!is_current(batch.owner) || queue_owner_!=batch.owner) { clear(batch.owner); return batch; }
        while (size_!=0) {
            auto& slot=slots_[head_];
            const auto state=slot.state.load(std::memory_order_acquire)&3;
            if (state==0) break;
            if (state==ready) batch.events[batch.size++]=slot.event;
            slot.state.store(0,std::memory_order_release);
            head_=(head_+1)%capacity;
            --size_;
        }
        if (!is_current(batch.owner)) batch.size=0;
        return batch;
    }
    [[nodiscard]] std::uint64_t dropped_total() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    struct Slot final { ActorEvent event{}; std::atomic_uint64_t state{}; };
    struct Release final { std::atomic_flag& gate; ~Release() { gate.clear(std::memory_order_release); } };
    void clear(std::uint64_t owner) noexcept {
        for (auto& slot:slots_) slot.state.store(0,std::memory_order_release);
        head_=size_=0;queue_owner_=owner;
    }
    static constexpr std::uint64_t ready=1, discarded=2;
    static_assert(std::atomic_uint64_t::is_always_lock_free);
    std::atomic_flag gate_{};
    std::atomic_uint64_t epoch_{},dropped_{};
    std::array<Slot,capacity> slots_{};
    std::uint64_t queue_owner_{},serial_{};
    std::size_t head_{},size_{};
};

} // namespace synth::core
