#pragma once

#include "core/generation.hpp"
#include "core/cancellation.hpp"
#include "diagnostics/conversation_trace.hpp"
#include "presentation/scene_admission.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <deque>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace synth::runtime { struct RuntimeFacingRequest; }

namespace synth::presentation {

// text_complete means the fallback reading window elapsed, not a renderer acknowledgement.
enum class DialogueDelivery { pending, playing, spoken, reading, text_complete, discarded };

struct DialogueCaption final {
    std::uint64_t generation{};
    std::string speaker;
    std::string text;
    core::CancellationToken cancellation;
    std::shared_ptr<const runtime::RuntimeFacingRequest> facing;
    // Audio, fallback presentation and continuation share this one utterance outcome.
    std::shared_ptr<std::atomic<DialogueDelivery>> delivery{
        std::make_shared<std::atomic<DialogueDelivery>>(DialogueDelivery::pending)};
    std::shared_ptr<diagnostics::ConversationTrace> trace;
    std::string request_id, line_id;
    std::uint64_t context_sequence{};
    std::shared_ptr<SceneAdmission> admission;
    // Optional copy-only sink for the first terminal delivery (spoken, text_complete or discarded).
    // It runs inside playback/presentation teardown and must only enqueue a bounded copy.
    std::function<void(DialogueDelivery terminal, bool cancelled)> on_terminal;
    std::shared_ptr<std::atomic_bool> terminal_reported{std::make_shared<std::atomic_bool>(false)};
    // Presentation only, written by the game-thread present_caption_once: the caption has reached a
    // sink that cannot retract it. Shared like delivery so requeued or copied values keep it; it is
    // independent of delivery and receipts and never means delivered or heard.
    std::shared_ptr<std::atomic_bool> posted{std::make_shared<std::atomic_bool>(false)};

    [[nodiscard]] bool scene_ready(core::SnapshotClock::time_point now) const noexcept {
        return !admission || admission->ready(now);
    }
    [[nodiscard]] bool scene_rejected() const noexcept {
        return admission && admission->rejected.load(std::memory_order_acquire);
    }

    void mark_delivery(DialogueDelivery state) const noexcept {
        if (admission && state == DialogueDelivery::playing) admission->started.store(true, std::memory_order_release);
        const auto previous = delivery->exchange(state, std::memory_order_acq_rel);
        if (trace && previous != state && state != DialogueDelivery::pending) {
            try {
                const auto stage = state == DialogueDelivery::playing ? diagnostics::ConversationStage::playing :
                    state == DialogueDelivery::spoken ? diagnostics::ConversationStage::spoken :
                    state == DialogueDelivery::reading ? diagnostics::ConversationStage::reading :
                    state == DialogueDelivery::text_complete ? diagnostics::ConversationStage::text_complete : diagnostics::ConversationStage::discarded;
                trace->record(stage,generation,context_sequence,request_id,line_id);
            } catch (...) {} // Diagnostics cannot change delivery or escape an audio teardown callback.
        }
        if (on_terminal && (state == DialogueDelivery::spoken || state == DialogueDelivery::text_complete ||
            state == DialogueDelivery::discarded) && !terminal_reported->exchange(true, std::memory_order_acq_rel)) {
            try { on_terminal(state, cancellation.is_cancelled() || scene_rejected()); } catch (...) {}
        }
    }

    [[nodiscard]] bool delivered() const noexcept {
        const auto state = delivery->load(std::memory_order_acquire);
        return !cancellation.is_cancelled() && !scene_rejected() &&
            (state == DialogueDelivery::spoken || state == DialogueDelivery::text_complete);
    }
};

// Spoken captions travel with their audio; only text-only replies use a reading timer.
class DialogueCaptions final {
public:
    using Clock = std::chrono::steady_clock;
    using Caption = std::shared_ptr<const DialogueCaption>;

    // The host takes a shared value, releases this lock, then performs native capture.
    [[nodiscard]] Caption validation_candidate(Clock::time_point now) {
        std::unique_lock lock{mutex_,std::try_to_lock};
        if (!lock.owns_lock()) return {};
        const auto due = [now](const Caption& value) {
            return value && value->admission && !value->cancellation.is_cancelled() &&
                !value->scene_rejected() && !value->scene_ready(now);
        };
        if (due(active_)) return active_;
        return !pending_.empty() && due(pending_.front()) ? pending_.front() : Caption{};
    }

    void enqueue(Caption caption) {
        if (!caption || caption->text.empty() || caption->text.size() > 8192 ||
            caption->cancellation.is_cancelled()) return;
        std::scoped_lock lock{mutex_};
        if (pending_.size() == 16) {
            pending_.front()->mark_delivery(DialogueDelivery::discarded);
            pending_.pop_front();
        }
        pending_.push_back(std::move(caption));
    }

    // Playback errors are reported on the game thread; keep their caption pending on contention.
    void enqueue_failed(Caption& caption) {
        if (!caption) return;
        std::unique_lock lock{mutex_, std::try_to_lock};
        if (!lock.owns_lock()) return;
        if (pending_.size() == 16) {
            pending_.front()->mark_delivery(DialogueDelivery::discarded);
            pending_.pop_front();
        }
        pending_.push_back(std::move(caption));
    }

    [[nodiscard]] Caption frame(std::uint64_t generation, Caption spoken, bool paused,
                                Clock::time_point now) {
        // A contended worker queue must never stall Fallout's update.
        std::unique_lock lock{mutex_, std::try_to_lock};
        if (!lock.owns_lock()) return {};
        const auto valid = [generation](const Caption& value) {
            return value && value->generation == generation && !value->cancellation.is_cancelled() && !value->scene_rejected();
        };
        std::erase_if(pending_, [&](const Caption& value) {
            if (valid(value)) return false;
            if (value) value->mark_delivery(DialogueDelivery::discarded);
            return true;
        });
        if (active_ && !valid(active_)) {
            active_->mark_delivery(DialogueDelivery::discarded);
            active_.reset();
        }
        const auto delta = last_frame_ == Clock::time_point{} || now < last_frame_
                             ? Clock::duration{} : now - last_frame_;
        last_frame_ = now;
        if (paused || valid(spoken)) {
            if (active_) expires_ += delta;
            return paused ? Caption{} : spoken;
        }
        if (active_ && !active_->scene_ready(now)) { expires_ += delta; return {}; }
        if (active_ && now >= expires_) {
            active_->mark_delivery(DialogueDelivery::text_complete);
            active_.reset();
        }
        if (!active_ && !pending_.empty() && pending_.front()->scene_ready(now)) {
            active_ = std::move(pending_.front());
            pending_.pop_front();
            active_->mark_delivery(DialogueDelivery::reading);
            // Byte count is conservative for non-ASCII; preserve the complete text.
            expires_ = now + std::chrono::milliseconds{
                std::clamp<std::size_t>(1500 + active_->text.size() * 55, 3000, 30000)};
        }
        return active_;
    }

    // A contended presentation queue is busy, never permission to advance a conversation.
    [[nodiscard]] bool idle() {
        std::unique_lock lock{mutex_, std::try_to_lock};
        return lock.owns_lock() && !active_ && pending_.empty();
    }

private:
    std::mutex mutex_;
    std::deque<Caption> pending_;
    Caption active_;
    Clock::time_point expires_{};
    Clock::time_point last_frame_{};
};

// Game thread: hands the caption chosen by DialogueCaptions::frame to a sink that cannot retract its
// output (the VR HUD message) at most once per caption lifetime. frame() keeps FIFO, pause and scene
// admission; ownership is re-checked here, at display, so a cancelled, replaced, rejected or stale
// caption never reaches the sink. The posted mark lives on the caption itself, so a failed spoken
// line requeued as text, or a text caption resumed after any number of spoken ones, is not reposted
// and no owner history is kept. Delivery state is untouched: text completion stays with the reading
// window and heard receipts with playback. Once posted, a message cannot be withdrawn.
template <class Sink>
bool present_caption_once(const DialogueCaptions::Caption& caption, std::uint64_t generation, Sink&& sink) {
    if (!caption || caption->generation != generation || caption->text.empty() ||
        caption->cancellation.is_cancelled() || caption->scene_rejected() ||
          (caption->admission && caption->admission->companion_speaker_form_id != 0 &&
              !caption->scene_ready(core::SnapshotClock::now())) ||
        caption->posted->exchange(true, std::memory_order_acq_rel)) return false;
    if (caption->admission) caption->admission->started.store(true, std::memory_order_release);
    sink(*caption);
    return true;
}

// Bounded system-notification queue: any thread pushes copied text and the game-thread pump drains
// it. Dialogue never enters it, so cancelling a turn cannot leave its text stranded here.
class NotificationQueue final {
public:
    static constexpr std::size_t capacity = 16;

    void push(std::string message) {
        if (message.empty()) return;
        std::scoped_lock lock{mutex_};
        if (pending_.size() == capacity) pending_.pop_front();
        pending_.push_back(std::move(message));
    }

    template <class Present>
    void drain(Present&& present) {
        std::deque<std::string> drained;
        {
            std::scoped_lock lock{mutex_};
            drained.swap(pending_);
        }
        for (const auto& message : drained) present(message);
    }

private:
    std::mutex mutex_;
    std::deque<std::string> pending_;
};

struct Subtitle final {
    core::RuntimeGeneration generation;
    std::string speaker_id;
    std::string text;
};

enum class SubtitleResult { accepted, empty, too_long, stale_generation, terminal };

class PassiveSubtitles final {
public:
    PassiveSubtitles(core::RuntimeGeneration generation,
                     std::size_t maximum_entries = 4,
                     std::size_t maximum_text_bytes = 512)
        : generation_{generation},
          maximum_entries_{maximum_entries},
          maximum_text_bytes_{maximum_text_bytes} {
        if (!generation.valid() || maximum_entries == 0 || maximum_text_bytes == 0) {
            throw std::invalid_argument{"subtitle state requires valid non-zero bounds"};
        }
    }

    [[nodiscard]] SubtitleResult show(Subtitle subtitle) {
        if (terminal_) return SubtitleResult::terminal;
        if (subtitle.generation != generation_) return SubtitleResult::stale_generation;
        if (subtitle.text.empty()) return SubtitleResult::empty;
        if (subtitle.text.size() > maximum_text_bytes_) return SubtitleResult::too_long;
        if (entries_.size() == maximum_entries_) entries_.pop_front();
        entries_.push_back(std::move(subtitle));
        return SubtitleResult::accepted;
    }

    void bind_generation(core::RuntimeGeneration generation) {
        if (!generation.valid()) throw std::invalid_argument{"subtitle generation must be valid"};
        entries_.clear();
        generation_ = generation;
    }

    void hard_halt() noexcept { entries_.clear(); terminal_ = true; }
    [[nodiscard]] const std::deque<Subtitle>& entries() const noexcept { return entries_; }
    [[nodiscard]] bool terminal() const noexcept { return terminal_; }

private:
    core::RuntimeGeneration generation_;
    std::size_t maximum_entries_;
    std::size_t maximum_text_bytes_;
    std::deque<Subtitle> entries_;
    bool terminal_{};
};

enum class PresentationCapability { lipsync, facing };
enum class CapabilityState { unavailable, ready, failed };

struct CapabilityStatus final {
    CapabilityState lipsync{CapabilityState::unavailable};
    CapabilityState facing{CapabilityState::unavailable};
    std::string lipsync_error;
    std::string facing_error;
};

class OptionalPerformance final {
public:
    void ready(PresentationCapability capability) {
        state(capability) = CapabilityState::ready;
        error(capability).clear();
    }

    void fail(PresentationCapability capability, std::string message) {
        state(capability) = CapabilityState::failed;
        error(capability) = message.empty() ? "unspecified capability failure" : std::move(message);
    }

    [[nodiscard]] bool attempt(PresentationCapability capability) const noexcept {
        return state(capability) == CapabilityState::ready;
    }

    [[nodiscard]] bool audio_blocked() const noexcept { return false; }
    [[nodiscard]] const CapabilityStatus& status() const noexcept { return status_; }

    void clear() noexcept { status_ = {}; }

private:
    [[nodiscard]] CapabilityState& state(PresentationCapability capability) noexcept {
        return capability == PresentationCapability::lipsync ? status_.lipsync : status_.facing;
    }
    [[nodiscard]] const CapabilityState& state(PresentationCapability capability) const noexcept {
        return capability == PresentationCapability::lipsync ? status_.lipsync : status_.facing;
    }
    [[nodiscard]] std::string& error(PresentationCapability capability) noexcept {
        return capability == PresentationCapability::lipsync ? status_.lipsync_error : status_.facing_error;
    }

    CapabilityStatus status_;
};

struct HaltReport final {
    std::size_t subtitles_cleared{};
    bool optional_state_cleared{};
    bool terminal_cancellation{};
};

class PresentationState final {
public:
    explicit PresentationState(core::RuntimeGeneration generation) : subtitles_{generation} {}

    [[nodiscard]] HaltReport hard_halt() noexcept {
        const auto count = subtitles_.entries().size();
        subtitles_.hard_halt();
        optional_.clear();
        return {count, true, true};
    }

    PassiveSubtitles& subtitles() noexcept { return subtitles_; }
    OptionalPerformance& optional() noexcept { return optional_; }

private:
    PassiveSubtitles subtitles_;
    OptionalPerformance optional_;
};

}  // namespace synth::presentation
