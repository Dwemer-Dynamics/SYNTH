#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace synth::diagnostics {

enum class ConversationStage {
    turn_admitted, line_applied, speech_ready, audio_queued,
    playing, spoken, reading, text_complete, discarded,
    continuation_dispatch, prefetch_received, prefetch_release,
    cancel_sent, cancel_acknowledged, cancel_unconfirmed, session_retired,
    prefetch_cancelled, prefetch_expired, prefetch_parent_discarded, prefetch_overflow, prefetch_incomplete,
    media_prepare_started, media_prepared, media_reused, media_failed,
    continuation_no_candidate, continuation_private, continuation_budget_exhausted, continuation_ineligible,
    continuation_complete, continuation_failed, continuation_queue_full, continuation_scene_rejected
};

[[nodiscard]] inline std::string_view stage_name(ConversationStage stage) noexcept {
    constexpr std::string_view names[]{"turn_admitted","line_applied","speech_ready","audio_queued",
        "playing","spoken","reading","text_complete","discarded","continuation_dispatch",
        "prefetch_received","prefetch_release","cancel_sent","cancel_acknowledged","cancel_unconfirmed","session_retired",
        "prefetch_cancelled","prefetch_expired","prefetch_parent_discarded","prefetch_overflow","prefetch_incomplete",
        "media_prepare_started","media_prepared","media_reused","media_failed",
        "continuation_no_candidate","continuation_private","continuation_budget_exhausted","continuation_ineligible",
        "continuation_complete","continuation_failed","continuation_queue_full","continuation_scene_rejected"};
    const auto index = static_cast<unsigned>(stage);
    return index < std::size(names) ? names[index] : "invalid";
}

struct ConversationRecord {
    ConversationStage stage{};
    std::uint64_t milliseconds{}, generation{}, context_sequence{};
    std::string request_id, line_id;
};

// Lossy bounded diagnostics: contention must never delay the game or a response worker.
class ConversationTrace final {
public:
    void record(ConversationStage stage, std::uint64_t generation, std::uint64_t context_sequence = 0,
                std::string_view request = {}, std::string_view line = {}) noexcept {
        try {
        std::unique_lock lock{mutex_,std::try_to_lock};
        if (!lock.owns_lock()) { ++dropped_; return; }
        if (records_.size() == 128) { records_.pop_front(); ++dropped_; }
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        records_.push_back({stage,static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count()),
            generation,context_sequence,identifier(request),identifier(line)});
        } catch (...) { ++dropped_; }
    }

    [[nodiscard]] std::optional<ConversationRecord> take() {
        std::unique_lock lock{mutex_,std::try_to_lock};
        if (!lock.owns_lock() || records_.empty()) return {};
        auto result = std::move(records_.front()); records_.pop_front(); return result;
    }
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    static std::string identifier(std::string_view value) {
        if (value.size()>160) return "invalid";
        for (const auto c : value) if (!((c>='a' && c<='z') || (c>='A' && c<='Z') ||
            (c>='0' && c<='9') || c==':' || c=='.' || c=='_' || c=='-')) return "invalid";
        return std::string{value};
    }
    std::mutex mutex_;
    std::deque<ConversationRecord> records_;
    std::atomic_uint64_t dropped_{};
};

} // namespace synth::diagnostics
