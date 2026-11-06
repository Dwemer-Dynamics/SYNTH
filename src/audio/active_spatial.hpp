#pragma once

#include "audio/dialogue_audio.hpp"
#include "runtime/fallout_runtime.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace synth::audio {

// Copied configuration and poses only. Native actor/camera/HMD reads stay in the runtime adapter.
struct SpatialPlaybackSettings final {
    float base_volume{1.0F};
    double distance_scale{1.0};
    double interior_maximum{1.0};
    double exterior_maximum{1.0};
    double interior_dropoff{1.0};
    double exterior_dropoff{1.0};
    bool invert_heading{};

    bool operator==(const SpatialPlaybackSettings&) const = default;

    [[nodiscard]] bool valid() const noexcept {
        return std::isfinite(base_volume) && base_volume >= 0.0F && base_volume <= 2.0F &&
            std::isfinite(distance_scale) && distance_scale > 0.0 && distance_scale <= 20.0 &&
            std::isfinite(interior_maximum) && interior_maximum > 0.0 &&
            std::isfinite(exterior_maximum) && exterior_maximum > 0.0 &&
            std::isfinite(interior_dropoff) && interior_dropoff >= 0.0 && interior_dropoff <= 2.0 &&
            std::isfinite(exterior_dropoff) && exterior_dropoff >= 0.0 && exterior_dropoff <= 2.0;
    }
};

struct SpatialMix final {
    float volume{};
    float pan{};

    [[nodiscard]] bool valid() const noexcept {
        return std::isfinite(volume) && volume >= 0.0F && volume <= 2.0F &&
            std::isfinite(pan) && pan >= -1.0F && pan <= 1.0F;
    }
};

struct ActiveSpatialRequest final {
    std::uint64_t playback_session{};
    std::uint64_t playback_token{};
    runtime::RuntimeSpeechOwner owner;
    SpatialPlaybackSettings settings;
};

// The caller supplies the current effective listener: camera in flat, fresh HMD in VR.
[[nodiscard]] inline std::optional<SpatialMix> spatial_mix(
    const SpatialPlaybackSettings& settings, Vec3 source, const Pose& listener, bool interior) {
    if (!settings.valid()) return std::nullopt;
    SpatialScene scene;
    if (scene.update_listener(RuntimeView::flat, listener, std::nullopt, listener.sample) !=
            ListenerResult::accepted || !scene.set_source("active-speaker", source)) return std::nullopt;
    const auto distance = scene.distance_to("active-speaker");
    if (!distance || !std::isfinite(*distance)) return std::nullopt;
    const auto maximum = interior ? settings.interior_maximum : settings.exterior_maximum;
    const auto dropoff = interior ? settings.interior_dropoff : settings.exterior_dropoff;
    const auto attenuation = std::clamp(1.0 - (*distance * settings.distance_scale / maximum) * dropoff, 0.0, 1.0);
    const auto azimuth = scene.azimuth_sine("active-speaker").value_or(0.0);
    return SpatialMix{static_cast<float>(settings.base_volume * attenuation),
                      static_cast<float>(settings.invert_heading ? -azimuth : azimuth)};
}

// Dialogue is one emitter. Stereo TTS is folded equally into that emitter.
// XAudio2 indexes its matrix by destination: matrix[source_channels * destination + source].
// Surround devices use their declared front left/right channels, without inventing HRTF/occlusion.
[[nodiscard]] inline std::optional<std::vector<float>> point_source_matrix(
    std::uint32_t source_channels, std::uint32_t destination_channels,
    std::uint32_t destination_mask, float pan) {
    if ((source_channels != 1 && source_channels != 2) || destination_channels == 0 ||
        destination_channels > 8 || static_cast<std::uint32_t>(std::popcount(destination_mask)) != destination_channels ||
        !std::isfinite(pan) || pan < -1.0F || pan > 1.0F) return std::nullopt;
    std::vector<float> matrix(source_channels * destination_channels, 0.0F);
    const auto weight = 1.0F / static_cast<float>(source_channels);
    if (destination_channels == 1) {
        if (destination_mask != 0x1 && destination_mask != 0x2 && destination_mask != 0x4) return std::nullopt;
        std::fill(matrix.begin(), matrix.end(), weight);
        return matrix;
    }
    constexpr std::uint32_t front_left = 0x1, front_right = 0x2;
    if ((destination_mask & (front_left | front_right)) != (front_left | front_right)) return std::nullopt;
    const auto left = std::sqrt((1.0F - pan) * 0.5F) * weight;
    const auto right = std::sqrt((1.0F + pan) * 0.5F) * weight;
    // Speaker-mask order is ascending bit order; FL and FR are the first two when present.
    for (std::uint32_t channel = 0; channel < source_channels; ++channel) {
        matrix[channel] = left;
        matrix[source_channels + channel] = right;
    }
    return matrix;
}

}  // namespace synth::audio
