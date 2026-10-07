#pragma once

#include "runtime/fallout_runtime.hpp"

#include <cmath>
#include <numbers>
#include <optional>

namespace synth::presentation {

// Fallout yaw is clockwise from +Y. Too-close and non-finite positions cannot define a heading.
[[nodiscard]] inline std::optional<float> facing_yaw(core::Vec3 speaker, core::Vec3 listener) noexcept {
    const auto dx = listener.x - speaker.x;
    const auto dy = listener.y - speaker.y;
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(speaker.z) ||
        !std::isfinite(listener.z) || std::hypot(dx, dy) < 1.0) return {};
    auto yaw = std::atan2(dx, dy);
    if (yaw < 0) yaw += 2 * std::numbers::pi;
    return static_cast<float>(yaw);
}

// Consume once at actual speech playback, including a failed/suppressed attempt. Null frames preserve pause ownership.
class SpeechFacingOnce final {
public:
    [[nodiscard]] std::optional<runtime::RuntimeActionResult> pump(
        std::shared_ptr<const runtime::RuntimeFacingRequest> request, runtime::IFalloutRuntime& runtime) {
        runtime.assert_game_thread();
        if (!request || request->generation != runtime.generation().value() || request->cancellation.is_cancelled()) return {};
        if (last_ && last_->generation == request->generation && last_->request_id == request->request_id &&
            last_->turn_id == request->turn_id && last_->utterance_id == request->utterance_id) return {};
        last_ = std::move(request);
        return runtime.face_speech_listener(*last_);
    }
private:
    std::shared_ptr<const runtime::RuntimeFacingRequest> last_;
};

}  // namespace synth::presentation
