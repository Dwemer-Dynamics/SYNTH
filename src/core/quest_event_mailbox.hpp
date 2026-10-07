#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace synth::core {

enum class QuestEventKind : std::uint8_t { stage, started, stopped };

// Exact event scalars only; these are not a journal snapshot or proof of player/NPC knowledge.
struct QuestEvent final {
    QuestEventKind kind{};
    std::uint32_t form_id{};
    std::uint16_t stage{};
    std::uint8_t item{};
    bool failed{};

    [[nodiscard]] bool valid() const noexcept {
        if (form_id == 0 || form_id == UINT32_MAX) return false;
        if (kind == QuestEventKind::stage) return !failed;
        return (kind == QuestEventKind::started || kind == QuestEventKind::stopped) && stage == 0 && item == 0;
    }
    bool operator==(const QuestEvent&) const = default;
};

class QuestEventMailbox final {
public:
    static constexpr std::size_t capacity = 32;
    struct Ticket final {
        std::uint64_t owner{}, serial{};
        std::size_t slot{};
        [[nodiscard]] explicit operator bool() const noexcept { return serial != 0; }
    };
    struct Batch final {
        std::uint64_t owner{};
        std::array<QuestEvent, capacity> events{};
        std::size_t size{};
    };

    void arm() noexcept { epoch_.fetch_or(1, std::memory_order_release); }
    void invalidate() noexcept {
        auto owner = epoch_.load(std::memory_order_relaxed);
        while (!epoch_.compare_exchange_weak(owner, (owner & ~std::uint64_t{1}) + 2,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {}
    }
    [[nodiscard]] std::uint64_t stamp() const noexcept { return epoch_.load(std::memory_order_acquire); }
    [[nodiscard]] bool is_current(std::uint64_t owner) const noexcept { return (owner & 1) && stamp() == owner; }

    // Single try only, never spin under an engine callback. Reservation order survives nested forwarding.
    [[nodiscard]] Ticket reserve(std::uint64_t owner, QuestEvent event) noexcept {
        if (!event.valid() || !is_current(owner)) return {};
        if (gate_.test_and_set(std::memory_order_acquire)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        const ReleaseGate release{gate_};
        if (!is_current(owner)) return {};
        if (queue_owner_ != owner) clear(owner);
        if (size_ == capacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        const auto index = (head_ + size_) % capacity;
        auto& slot = slots_[index];
        // Keep serials unique across load/reset and slot reuse; zero is the invalid ticket.
        serial_ = (serial_ + 1) & (UINT64_MAX >> 2);
        if (serial_ == 0) ++serial_;
        slot.event = event;
        slot.state.store(serial_ << 2, std::memory_order_release);
        ++size_;
        return {owner, serial_, index};
    }

    // No gate acquisition after native forwarding, so contention cannot strand a reserved head.
    [[nodiscard]] bool commit(Ticket ticket) noexcept {
        if (!ticket || ticket.slot >= capacity) return false;
        if (!is_current(ticket.owner)) { discard(ticket); return false; }
        auto expected = ticket.serial << 2;
        return slots_[ticket.slot].state.compare_exchange_strong(expected, expected | ready,
            std::memory_order_release, std::memory_order_relaxed);
    }
    void discard(Ticket ticket) noexcept {
        if (!ticket || ticket.slot >= capacity) return;
        auto expected = ticket.serial << 2;
        (void)slots_[ticket.slot].state.compare_exchange_strong(expected, expected | discarded,
            std::memory_order_release, std::memory_order_relaxed);
    }

    // Safe-pump consumer only. Check batch.owner again after binding the game's immutable context.
    [[nodiscard]] Batch take() noexcept {
        Batch result;
        if (gate_.test_and_set(std::memory_order_acquire)) return result;
        const ReleaseGate release{gate_};
        result.owner = stamp();
        if (!is_current(result.owner) || queue_owner_ != result.owner) {
            clear(result.owner);
            return result;
        }
        while (size_ != 0) {
            auto& slot = slots_[head_];
            const auto state = slot.state.load(std::memory_order_acquire) & 3;
            if (state == 0) break; // A later completed callback cannot overtake this original handler.
            if (state == ready) result.events[result.size++] = slot.event;
            slot.state.store(0, std::memory_order_release);
            head_ = (head_ + 1) % capacity;
            --size_;
        }
        if (!is_current(result.owner)) result.size = 0;
        return result;
    }

    // Process health only; do not turn drops into a quest event on the next save.
    [[nodiscard]] std::uint64_t dropped_total() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    struct Slot final { QuestEvent event{}; std::atomic_uint64_t state{}; };
    struct ReleaseGate final {
        std::atomic_flag& gate;
        ~ReleaseGate() { gate.clear(std::memory_order_release); }
    };
    // Called only with the try-gate held. Old callbacks retain tickets, never pointers to mutable payloads.
    void clear(std::uint64_t owner) noexcept {
        for (auto& slot : slots_) slot.state.store(0, std::memory_order_release);
        head_ = size_ = 0;
        queue_owner_ = owner;
    }
    static constexpr std::uint64_t ready = 1, discarded = 2;
    static_assert(std::atomic_uint64_t::is_always_lock_free);
    std::atomic_flag gate_{};
    std::atomic_uint64_t epoch_{}, dropped_{};
    std::array<Slot, capacity> slots_{};
    std::uint64_t queue_owner_{}, serial_{};
    std::size_t head_{}, size_{};
};

} // namespace synth::core
