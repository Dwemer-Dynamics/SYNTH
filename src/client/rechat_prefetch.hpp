#pragma once

#include "presentation/dialogue_presentation.hpp"
#include "protocol_native/v1_codec.hpp"

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

namespace synth::client {

// One speculative response stays invisible until its exact parent utterance is delivered.
class RechatPrefetch final {
public:
    using Clock = std::chrono::steady_clock;
    using MediaBytes = std::shared_ptr<const std::vector<std::byte>>;
    static constexpr std::size_t media_capacity = 16U * 1024U * 1024U;
    explicit RechatPrefetch(presentation::DialogueCaptions::Caption parent, Clock::time_point deadline)
        : parent_{std::move(parent)}, cancellation_{parent_->cancellation}, deadline_{deadline} {}

    [[nodiscard]] core::CancellationToken token() const noexcept { return cancellation_.token(); }
    void cancel(diagnostics::ConversationStage reason = diagnostics::ConversationStage::prefetch_cancelled) noexcept {
        cancellation_.cancel();
        if (!reported_.exchange(true,std::memory_order_acq_rel) && parent_->trace)
            parent_->trace->record(reason,parent_->generation,parent_->context_sequence,parent_->request_id,parent_->line_id);
    }

    [[nodiscard]] bool invalid(Clock::time_point now) noexcept {
        if (cancellation_.is_cancelled()) { cancel(); return true; }
        if (!released_.load(std::memory_order_acquire) && now >= deadline_) {
            cancel(diagnostics::ConversationStage::prefetch_expired); return true;
        }
        if (parent_->delivery->load(std::memory_order_acquire) == presentation::DialogueDelivery::discarded) {
            cancel(diagnostics::ConversationStage::prefetch_parent_discarded); return true;
        }
        return false;
    }

    // Protocol parsing already bounds each line. Overflow stops speculation, never evicts an action or terminal.
    void append(const protocol_native::Line& line) {
        std::scoped_lock lock{mutex_};
        if (invalid(Clock::now()) || finished_) return;
        if (lines_.size() == 128) { cancel(diagnostics::ConversationStage::prefetch_overflow); lines_.clear(); return; }
        lines_.push_back(line);
    }

    void finish(bool complete) {
        std::scoped_lock lock{mutex_};
        finished_ = true;
        if (!complete) { cancel(diagnostics::ConversationStage::prefetch_incomplete); lines_.clear(); }
    }

    // Warm only the first voiced line: one existing speech worker, one bounded WAV, no presentation.
    [[nodiscard]] bool claim_media(const protocol_native::Line& line) {
        std::scoped_lock lock{mutex_};
        const auto* dialogue = std::get_if<protocol_native::Dialogue>(&line.payload);
        if (invalid(Clock::now()) || finished_ || !media_line_.empty() || line.line_id.empty() || !dialogue ||
            !dialogue->speech || dialogue->speech->status == "failed") return false;
        media_line_ = line.line_id;
        return true;
    }

    void finish_media(MediaBytes bytes) {
        std::scoped_lock lock{mutex_};
        if (invalid(Clock::now()) || media_line_.empty()) return;
        if (bytes && (bytes->empty() || bytes->size() > media_capacity)) bytes.reset();
        media_ = std::move(bytes);
        media_finished_ = true;
    }

    // Called on the same FIFO speech lane after its preparation job, never on the game thread.
    // nullopt means unprepared; an engaged null pointer is a failed preparation (text fallback).
    [[nodiscard]] std::optional<MediaBytes> prepared_media(std::string_view line_id) {
        std::scoped_lock lock{mutex_};
        if (invalid(Clock::now()) || !released_.load(std::memory_order_acquire) || !media_finished_ || media_line_ != line_id) return {};
        return media_;
    }

    // Game-thread polling cannot wait for a network callback holding the buffer lock.
    [[nodiscard]] std::optional<std::vector<protocol_native::Line>> take(Clock::time_point now) {
        std::unique_lock lock{mutex_, std::try_to_lock};
        if (!lock.owns_lock() || invalid(now) || !finished_ || taken_ || !parent_->delivered()) return {};
        taken_ = true;
        return std::move(lines_);
    }

    // The delivery worker calls this only after fresh scene admission, before applying any child line.
    // Speculation expiry stops here; parent/turn cancellation still invalidates the delivered child.
    [[nodiscard]] bool release(Clock::time_point now) {
        std::scoped_lock lock{mutex_};
        if (invalid(now) || !taken_ || released_.load(std::memory_order_acquire)) return false;
        released_.store(true,std::memory_order_release);
        if (parent_->trace) parent_->trace->record(diagnostics::ConversationStage::prefetch_release,
            parent_->generation,parent_->context_sequence,parent_->request_id,parent_->line_id);
        return true;
    }

private:
    presentation::DialogueCaptions::Caption parent_;
    core::CancellationSource cancellation_;
    Clock::time_point deadline_;
    std::mutex mutex_;
    std::vector<protocol_native::Line> lines_;
    bool finished_{};
    bool taken_{};
    std::string media_line_;
    MediaBytes media_;
    bool media_finished_{};
    std::atomic_bool released_{};
    std::atomic_bool reported_{};
};

} // namespace synth::client
