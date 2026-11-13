#pragma once

#include "client/synth_client.hpp"
#include "core/cancellation.hpp"
#include "presentation/dialogue_presentation.hpp"
#include "protocol_native/v1_codec.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>

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

// Terminal caption hook for a native v2 turn line; empty for any other line. Its first terminal delivery
// pushes one copied receipt; only completed native audio becomes audio_completed.
[[nodiscard]] inline std::function<void(presentation::DialogueDelivery, bool)> speech_receipt_hook(
    std::weak_ptr<SpeechReceiptOutbox> outbox, const protocol_native::Line& line, const protocol_native::Dialogue& dialogue,
    bool native_turn_ready) {
    const auto* speaker = std::get_if<protocol_native::Identity>(&dialogue.speaker);
    if (!speaker || !dialogue.native_turn || !native_turn_ready ||
        line.protocol_version != 2 || line.context_sequence == 0) return {};
    return [outbox = std::move(outbox),
            pending = PendingSpeechReceipt{line.generation, line.context_sequence, line.turn_id,
                {line.request_id, line.line_id, *speaker, {}}}](
                presentation::DialogueDelivery terminal, bool cancelled) mutable {
        const auto outcome = speech_receipt_outcome(terminal, cancelled);
        const auto target = outbox.lock();
        if (!outcome || !target) return;
        pending.receipt.outcome = *outcome;
        (void)target->push(std::move(pending));
    };
}

// The server's only receipt acknowledgement: start, one speech_receipt_accepted status carrying the
// stored state, then a complete end. It also acknowledges interrupted and failed receipts, so this
// is not a heard signal. HTTP success alone, refusals (receipt_conflict, receipt_owner_mismatch,
// receipt_invalid) and extra or repeated lines return null.
[[nodiscard]] inline const std::string* accepted_receipt_state(const RequestOutcome& outcome) {
    if (outcome.status != RequestStatus::complete || outcome.lines.size() != 3) return nullptr;
    const auto* status = std::get_if<protocol_native::Status>(&outcome.lines[1].payload);
    const auto* end = std::get_if<protocol_native::End>(&outcome.lines[2].payload);
    return std::holds_alternative<protocol_native::Start>(outcome.lines[0].payload) && status && end &&
        status->code == "speech_receipt_accepted" && !status->detail.empty() && end->status == "complete"
        ? &status->detail : nullptr;
}

// Network worker: one server-accepted spoken receipt for this generation, under the drain's token.
using HeardReceipt = std::function<void(std::uint64_t, const core::CancellationToken&)>;

struct SpeechReceiptExchange final {
    RequestOutcome outcome;
    std::optional<std::string> accepted_state;
    bool heard{};
};

// Network worker, shared by SpeechDelivery's drain and the loopback driver: send one copied receipt and
// notify heard history. Heard history exists only after an accepted completed-audio receipt stored as
// spoken, while the drain is live and still owns the receipt's generation. Never retried here.
[[nodiscard]] inline SpeechReceiptExchange exchange_speech_receipt(SynthClient& client, const PendingSpeechReceipt& value,
                                                                   const core::CancellationToken& cancellation,
                                                                   const std::atomic_bool& terminal,
                                                                   const HeardReceipt& heard) {
    SpeechReceiptExchange result{client.send_speech_receipt(value.receipt,
        {value.generation, value.context_sequence, {}, value.turn_id})};
    if (const auto* state = accepted_receipt_state(result.outcome)) result.accepted_state = *state;
    if (heard && !cancellation.is_cancelled() && !terminal.load(std::memory_order_acquire) &&
        value.generation == cancellation.generation().value() &&
        value.receipt.outcome == protocol_native::SpeechReceiptOutcome::audio_completed &&
        result.accepted_state == "spoken") {
        heard(value.generation, cancellation);
        result.heard = true;
    }
    return result;
}

} // namespace synth::client
