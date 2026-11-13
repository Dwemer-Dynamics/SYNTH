#pragma once

#include "client/synth_client.hpp"
#include "core/cancellation.hpp"
#include "core/save_context.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace synth::client {

// Engine-free save checkpoint of one runtime session's writer binding: the history frontier that accepted
// flat dialogue, accepted heard receipts and wait outcomes raise, and the exact context ACK that covers a
// revision of it. ContextPublication owns one; the loopback dialogue driver drives the same object.
// Every method is nonblocking and thread-safe. The borrowed store and terminal flag outlive this object.
class SaveCheckpoint final {
public:
    SaveCheckpoint(core::SaveContextStore* store, core::SaveContextStore::Binding binding,
                   core::CancellationToken session, const std::atomic_bool& terminal)
        : store_{store}, binding_{std::move(binding)}, session_{std::move(session)}, terminal_{terminal} {}

    SaveCheckpoint(const SaveCheckpoint&) = delete;
    SaveCheckpoint& operator=(const SaveCheckpoint&) = delete;

    [[nodiscard]] bool bound() const noexcept { return store_ != nullptr; }
    [[nodiscard]] const core::SaveContextStore::Binding& binding() const noexcept { return binding_; }

    // Retirement revokes this binding's write epoch before any later ACK can land.
    void stop_writer() {
        if (store_) store_->stop_writer(binding_.epoch);
    }

    [[nodiscard]] bool writer_current() const {
        return !store_ || store_->writer_current(binding_.epoch);
    }

    // The frontier a publication must cover; read before the publication is sent.
    [[nodiscard]] std::uint64_t revision() const noexcept { return serial_.load(std::memory_order_acquire); }

    // A raised frontier that no saved ACK covers yet.
    [[nodiscard]] bool due() const noexcept {
        return serial_.load(std::memory_order_acquire) > acknowledged_.load(std::memory_order_acquire);
    }

    // An accepted flat dialogue response line, noted on receipt before presentation, raises the
    // history frontier a later save-ACKed publication must cover. It is not a heard receipt.
    void note_dialogue_history(core::RuntimeVariant variant, const core::CancellationToken& cancellation) {
        if(store_ && variant==core::RuntimeVariant::flat && !stopped(cancellation))
            serial_.fetch_add(1,std::memory_order_acq_rel);
    }

    // The server inserts heard history only when it accepts a spoken receipt, after any earlier save
    // ACK. It raises the same frontier, which only a later exactly accepted publication clears.
    void note_heard_receipt(std::uint64_t generation, const core::CancellationToken& cancellation) {
        if(!store_ || stopped(cancellation) ||
            generation!=session_.generation().value() || cancellation.generation()!=session_.generation() ||
            !store_->writer_current(binding_.epoch)) return;
        serial_.fetch_add(1,std::memory_order_acq_rel);
    }

    // A committed private narrator change (selection, recorded input or an accepted completed reply) is captured only
    // by a later context publication, so it raises the same frontier under the same owner checks as a heard receipt.
    void note_private_narrator(std::uint64_t generation, const core::CancellationToken& cancellation) {
        note_heard_receipt(generation, cancellation);
    }

    // The result is newer than its issuing snapshot. Reuse ordinary
    // world observation to acknowledge a frontier after the consequence.
    void note_wait_outcome(const core::CancellationToken& cancellation) {
        if(!store_ || stopped(cancellation)) return;
        const auto serial=serial_.fetch_add(1,std::memory_order_acq_rel)+1;
        wait_notice_.store(serial,std::memory_order_release);
    }

    // Game pump: true exactly once after a saved ACK covers the latest wait outcome.
    [[nodiscard]] bool take_wait_notice(std::uint64_t generation) {
        auto notice=wait_notice_.load(std::memory_order_acquire);
        return notice && !terminal_.load(std::memory_order_acquire) && generation==session_.generation().value() &&
            acknowledged_.load(std::memory_order_acquire)>=notice &&
            wait_notice_.compare_exchange_strong(notice,0,std::memory_order_acq_rel);
    }

    // Only an exact context_accepted proves the saved cutoff; a bare or wrong completion keeps the
    // older ACK and any raised frontier, whatever arrived while the publication was in flight. A saved
    // ACK covers only the revision read before its publication, never one raised during it.
    [[nodiscard]] bool acknowledge(const SynthClient& client, const RequestOutcome& outcome, std::string_view playthrough_id,
                                   std::uint64_t revision, core::RuntimeGeneration published,
                                   const core::CancellationToken& cancellation) {
        if (!store_ || !client.context_accepted(outcome) || stopped(cancellation)) return false;
        if (!store_->acknowledge(binding_.epoch, {std::string{playthrough_id}, client.runtime_session_id(),
                outcome.context_binding.generation, outcome.context_binding.sequence})) return false;
        if(revision && outcome.status==RequestStatus::complete && !stopped(cancellation) && published==session_.generation()) {
            auto before=acknowledged_.load(std::memory_order_acquire);
            while(before<revision && !acknowledged_.compare_exchange_weak(
                before,revision,std::memory_order_release,std::memory_order_acquire)) {}
        }
        return true;
    }

private:
    [[nodiscard]] bool stopped(const core::CancellationToken& cancellation) const noexcept {
        return cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire);
    }

    core::SaveContextStore* const store_;
    const core::SaveContextStore::Binding binding_;
    const core::CancellationToken session_;
    const std::atomic_bool& terminal_;
    std::atomic_uint64_t serial_{}, acknowledged_{}, wait_notice_{};
};

} // namespace synth::client
