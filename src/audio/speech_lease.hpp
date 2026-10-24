#pragma once

#include "runtime/fallout_runtime.hpp"

#include <functional>
#include <optional>

namespace synth::audio {

using SpeechOwnerPump = std::function<bool(const std::optional<runtime::RuntimeSpeechOwner>&, bool)>;

[[nodiscard]] inline bool same_speech_owner(const runtime::RuntimeSpeechOwner& left,
                                           const runtime::RuntimeSpeechOwner& right) noexcept {
    return left.generation == right.generation && left.request_id == right.request_id &&
        left.turn_id == right.turn_id && left.utterance_id == right.utterance_id &&
        left.speaker.form_id == right.speaker.form_id &&
        left.speaker.origin_plugin == right.speaker.origin_plugin &&
        left.speaker.playthrough_id == right.speaker.playthrough_id;
}

// Game-thread copied state only. No actor access, queue lock or worker/destructor callbacks.
class SpeechLease final {
public:
    [[nodiscard]] bool maintain(const runtime::RuntimeSpeechOwner& next, const SpeechOwnerPump& pump) noexcept {
        if (next.generation == 0 || next.cancellation.generation().value() != next.generation ||
            next.cancellation.is_cancelled() || next.request_id.empty() || next.turn_id.empty() ||
            next.utterance_id.empty() || next.speaker.form_id == 0 ||
            next.speaker.origin_plugin.empty() || next.speaker.playthrough_id.empty()) return false;
        if (owner_ && !same_speech_owner(*owner_, next) && !release(pump)) return false;
        // Retain the attempted owner before admission: an adapter that throws must still be cleaned up.
        try {
            owner_ = next;
            if (pump && pump(owner_, false)) return true;
        } catch (...) {}
        // Caller stops any local voice before release; failed admission remains owned for cleanup/retry.
        return false;
    }

    [[nodiscard]] bool release(const SpeechOwnerPump& pump, bool discard = false) noexcept {
        if (!owner_ && !discard) return true;
        try {
            if (pump && !pump(std::nullopt, discard)) return false;
        } catch (...) { return false; }
        owner_.reset();
        return true;
    }

    [[nodiscard]] const std::optional<runtime::RuntimeSpeechOwner>& owner() const noexcept { return owner_; }

private:
    std::optional<runtime::RuntimeSpeechOwner> owner_;
};

}  // namespace synth::audio
