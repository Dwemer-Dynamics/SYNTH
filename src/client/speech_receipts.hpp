#pragma once

#include "presentation/dialogue_presentation.hpp"
#include "protocol_native/v1_codec.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace synth::client {

struct PendingSpeechReceipt final {
    std::uint64_t generation{};
    std::uint64_t context_sequence{};
    std::string turn_id;
    protocol_native::SpeechReceipt receipt;
};

// Copy-only mailbox filled from playback/presentation teardown. It never touches the network, never
// blocks on a worker and never shares the reserved cancellation lane; overflow drops (fails closed).
class SpeechReceiptOutbox final {
public:
    explicit SpeechReceiptOutbox(std::size_t capacity = 32) : capacity_{capacity} {}

    bool push(PendingSpeechReceipt value) noexcept {
        try {
            std::scoped_lock lock{mutex_};
            if (pending_.size() >= capacity_) { ++dropped_; return false; }
            pending_.push_back(std::move(value));
            return true;
        } catch (...) { return false; }
    }

    // Stale generations are discarded here and revalidated again by the client before encoding.
    [[nodiscard]] std::deque<PendingSpeechReceipt> take(std::uint64_t generation) {
        std::deque<PendingSpeechReceipt> result;
        std::scoped_lock lock{mutex_};
        for (auto& value : pending_) {
            if (value.generation == generation) result.push_back(std::move(value));
            else ++dropped_;
        }
        pending_.clear();
        return result;
    }

    void clear() noexcept {
        std::scoped_lock lock{mutex_};
        dropped_ += pending_.size();
        pending_.clear();
    }

    // Single in-flight drain. The lease is owned by the submitted task, so the slot is released when
    // the last copy is destroyed: rejected submission, queue expiry or cancellation (erased unexecuted),
    // a thrown task and normal completion all release it. Never reset from the callback alone.
    [[nodiscard]] static std::shared_ptr<void> try_begin_drain(const std::shared_ptr<SpeechReceiptOutbox>& outbox) {
        if (!outbox || outbox->draining_.exchange(true, std::memory_order_acq_rel)) return {};
        struct Lease final {
            explicit Lease(std::shared_ptr<SpeechReceiptOutbox> value) noexcept : owner{std::move(value)} {}
            ~Lease() { owner->draining_.store(false, std::memory_order_release); }
            std::shared_ptr<SpeechReceiptOutbox> owner;
        };
        return std::make_shared<Lease>(outbox);
    }

    [[nodiscard]] std::uint64_t dropped() const noexcept {
        std::scoped_lock lock{mutex_};
        return dropped_;
    }

private:
    std::size_t capacity_{};
    mutable std::mutex mutex_;
    std::deque<PendingSpeechReceipt> pending_;
    std::uint64_t dropped_{};
    std::atomic_bool draining_{};
};

// Only completed native audio proves a heard line. The text reading timer is not a renderer
// acknowledgement, so text_complete produces no receipt and the server keeps the line unheard.
[[nodiscard]] inline std::optional<protocol_native::SpeechReceiptOutcome> speech_receipt_outcome(
    presentation::DialogueDelivery terminal, bool cancelled) noexcept {
    using presentation::DialogueDelivery;
    if (terminal == DialogueDelivery::spoken && !cancelled) return protocol_native::SpeechReceiptOutcome::audio_completed;
    if (terminal == DialogueDelivery::discarded || terminal == DialogueDelivery::spoken)
        return cancelled ? protocol_native::SpeechReceiptOutcome::interrupted : protocol_native::SpeechReceiptOutcome::failed;
    return std::nullopt;
}

} // namespace synth::client
