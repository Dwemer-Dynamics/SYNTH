#pragma once

#include "audio/native_playback.hpp"
#include "runtime/fallout_runtime.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <optional>
#include <string_view>

namespace synth::presentation {

// Compatibility facade: all native face access is now behind IFalloutRuntime.
class CommonlibFaceAnimation final {
public:
    using Clock = std::chrono::steady_clock;

    void pump(const std::optional<audio::PlaybackFrame>& frame,
              std::uint32_t resolution, double intensity, Clock::time_point now,
              runtime::IFalloutRuntime& runtime) {
        const auto actor_id = frame ? parse_form_id(frame->speaker_id) : std::nullopt;
        if (frame && frame->generation != runtime.generation().value()) return;
        if (!frame || !actor_id ||
            frame->cancellation.is_cancelled() || intensity <= 0) {
            reset(runtime, false);
            return;
        }
        if (now < next_update_ && utterance_ == frame->utterance_id) return;
        runtime::RuntimeSpeechFrame speech{frame->generation, *actor_id, frame->utterance_id, frame->cancellation};
        const auto gain = static_cast<float>(std::clamp(intensity, 0.0, 3.0));
        for (std::size_t i = 0; i < speech.mouth.size(); ++i)
            speech.mouth[i] = std::clamp(frame->mouth[i] * gain, 0.0F, 1.0F);
        runtime.animate_speech(speech);
        utterance_ = frame->utterance_id;
        next_update_ = now + audio::speech_animation_interval(resolution);
    }

    void reset(runtime::IFalloutRuntime& runtime, bool discard = true) {
        runtime.animate_speech(std::nullopt, discard);
        utterance_.clear();
        next_update_ = {};
    }

private:
    [[nodiscard]] static std::optional<std::uint32_t> parse_form_id(std::string_view value) noexcept {
        if (value.starts_with("0x") || value.starts_with("0X")) value.remove_prefix(2);
        if (value.empty() || value.size() > 8) return std::nullopt;
        std::uint32_t id{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), id, 16);
        if (error != std::errc{} || end != value.data() + value.size() || id == 0) return std::nullopt;
        return id;
    }

    std::string utterance_;
    Clock::time_point next_update_{};
};

} // namespace synth::presentation
