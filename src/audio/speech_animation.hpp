#pragma once

#include "core/cancellation.hpp"
#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <vector>

namespace synth::audio {

// Copied cross-thread permission only: the identity is compared, never dereferenced.
// The game-thread adapter writes the mouth; an engine worker may only request its native merge.
class SpeechMergePermit final {
public:
    using Clock = std::chrono::steady_clock;
    SpeechMergePermit(std::uintptr_t key, core::CancellationToken token)
        : identity_key_{key}, cancellation_{std::move(token)} {}
    void publish(Clock::time_point now) noexcept {
        expires_.store(ticks(now + std::chrono::milliseconds{250}), std::memory_order_release);
        enabled_.store(true, std::memory_order_release);
    }
    void revoke() noexcept { enabled_.store(false, std::memory_order_release); }
    bool allows(std::uintptr_t key, Clock::time_point now) const noexcept {
        return key == identity_key_ && enabled_.load(std::memory_order_acquire) &&
            !cancellation_.is_cancelled() && ticks(now) < expires_.load(std::memory_order_acquire);
    }
    std::atomic_uint64_t merged{};
private:
    static std::int64_t ticks(Clock::time_point time) noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    }
    const std::uintptr_t identity_key_;
    const core::CancellationToken cancellation_;
    std::atomic_bool enabled_{};
    std::atomic_int64_t expires_{};
};

// Semantic shapes, not Skyrim/FaceGen array indices. The Fallout adapter owns that mapping.
enum class MouthShape : std::uint8_t { rest, open, wide, closed, teeth, round, funnel, tongue };
using MouthWeights = std::array<float, 7>;

// Bounded text approximation following CHIM's text/viseme approach, sampled on the audio clock.
// No engine objects, filesystem, language model or provider-specific timestamps are required.
class SpeechAnimation final {
public:
    static constexpr std::size_t maximum_text = 8192;

    explicit SpeechAnimation(std::string_view text) {
        if (text.size() > maximum_text) return;
        shapes_.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            const auto upper = [](unsigned char c) { return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c; };
            const auto c = upper(static_cast<unsigned char>(text[i]));
            const auto next = i + 1 < text.size() ? upper(static_cast<unsigned char>(text[i + 1])) : 0;
            MouthShape shape{MouthShape::rest};
            if ((c == 'S' || c == 'C') && next == 'H') { shape = MouthShape::funnel; ++i; }
            else if (c == 'T' && next == 'H') { shape = MouthShape::tongue; ++i; }
            else if (c == 'O' && next == 'O') { shape = MouthShape::funnel; ++i; }
            else if (c == 'E' && next == 'E') { shape = MouthShape::wide; ++i; }
            else if (c == 'B' || c == 'M' || c == 'P') shape = MouthShape::closed;
            else if (c == 'F' || c == 'V') shape = MouthShape::teeth;
            else if (c == 'O' || c == 'U' || c == 'W' || c == 'Q' || c == 'R') shape = MouthShape::round;
            else if (c == 'E' || c == 'I' || c == 'Y') shape = MouthShape::wide;
            else if (c == 'L' || c == 'T' || c == 'D' || c == 'N') shape = MouthShape::tongue;
            else if (c >= 'A' && c <= 'Z') shape = MouthShape::open;
            // Unknown UTF-8 bytes are skipped safely, never interpreted as engine morph indices.
            else if (c >= 128) continue;
            shapes_.push_back(shape);
        }
    }

    [[nodiscard]] MouthWeights sample(double seconds, double duration, float level) const noexcept {
        MouthWeights result{};
        if (shapes_.empty() || !std::isfinite(seconds) || !std::isfinite(duration) ||
            !std::isfinite(level) || seconds < 0 || duration <= 0 || seconds >= duration || level <= 0) return result;
        const auto cursor = seconds / duration * static_cast<double>(shapes_.size());
        const auto index = std::min(static_cast<std::size_t>(cursor), shapes_.size() - 1);
        const auto weight = std::clamp(level, 0.0F, 1.0F);
        // Crossfade the tail of each segment; preserve the closed-lip consonants at its center.
        const auto blend = static_cast<float>(std::clamp((cursor - static_cast<double>(index) - 0.65) / 0.35, 0.0, 1.0));
        const auto add = [&](MouthShape shape, float value) {
            if (shape != MouthShape::rest) result[static_cast<std::size_t>(shape) - 1] += value;
        };
        add(shapes_[index], weight * (1.0F - blend));
        add(index + 1 < shapes_.size() ? shapes_[index + 1] : MouthShape::rest, weight * blend);
        return result;
    }

    [[nodiscard]] std::size_t size() const noexcept { return shapes_.size(); }

private:
    std::vector<MouthShape> shapes_;
};

// Keep the existing setting and range: it is a minimum interval in microseconds, as in CHIM.
// The game tick is the upper bound on update frequency; there is no timer or busy loop.
[[nodiscard]] inline std::chrono::microseconds speech_animation_interval(std::uint32_t resolution) noexcept {
    return std::chrono::microseconds{std::clamp<std::uint32_t>(resolution, 50, 1500)};
}

} // namespace synth::audio
