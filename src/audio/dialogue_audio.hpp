#pragma once

#include "core/generation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace synth::audio {

namespace detail {
[[nodiscard]] inline bool opaque_identifier(std::string_view value) noexcept {
    if (value.empty() || value.size() > 128) {
        return false;
    }
    return std::ranges::all_of(value, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
               (byte >= '0' && byte <= '9') || character == '_' || character == '-';
    });
}
}  // namespace detail

class MediaId final {
public:
    [[nodiscard]] static MediaId same_origin(std::string value) {
        if (!detail::opaque_identifier(value)) {
            throw std::invalid_argument{"media ID must be an opaque same-origin identifier"};
        }
        return MediaId{std::move(value)};
    }

    [[nodiscard]] std::string_view value() const noexcept { return value_; }
    auto operator<=>(const MediaId&) const = default;

private:
    explicit MediaId(std::string value) : value_{std::move(value)} {}
    std::string value_;
};

struct TtsItem final {
    core::RuntimeGeneration generation;
    std::uint64_t sequence{};
    std::string speaker_id;
    MediaId media_id;
};

enum class EnqueueResult {
    accepted,
    terminal,
    stale_generation,
    invalid_sequence,
    invalid_speaker,
    capacity,
    duplicate_media,
};

enum class InterruptReason { user, generation_change, hard_halt };

struct TtsHaltReport final {
    std::size_t cancelled{};
    bool playing_cancelled{};
    bool cache_cleared{};
    bool terminal{};
};

class TtsQueue final {
public:
    explicit TtsQueue(core::RuntimeGeneration generation, std::size_t maximum_items = 32)
        : generation_{generation}, maximum_items_{maximum_items} {
        if (!generation.valid() || maximum_items == 0) {
            throw std::invalid_argument{"TTS queue requires valid generation and capacity"};
        }
    }

    [[nodiscard]] EnqueueResult enqueue(TtsItem item) {
        if (terminal_) {
            return EnqueueResult::terminal;
        }
        if (item.generation != generation_) {
            return EnqueueResult::stale_generation;
        }
        if (item.sequence == 0 || item.sequence <= last_sequence_) {
            return EnqueueResult::invalid_sequence;
        }
        if (!detail::opaque_identifier(item.speaker_id)) {
            return EnqueueResult::invalid_speaker;
        }
        if (queued_.size() + (playing_.has_value() ? 1U : 0U) >= maximum_items_) {
            return EnqueueResult::capacity;
        }
        if (contains_media(item.media_id)) {
            return EnqueueResult::duplicate_media;
        }
        last_sequence_ = item.sequence;
        queued_.push_back(std::move(item));
        return EnqueueResult::accepted;
    }

    [[nodiscard]] const TtsItem* start_next() noexcept {
        if (terminal_ || playing_ || queued_.empty()) {
            return nullptr;
        }
        playing_ = std::move(queued_.front());
        queued_.pop_front();
        return &*playing_;
    }

    [[nodiscard]] bool finish_playing() noexcept {
        if (!playing_) {
            return false;
        }
        cache_.insert_or_assign(std::string{playing_->media_id.value()}, playing_->speaker_id);
        playing_.reset();
        return true;
    }

    [[nodiscard]] std::size_t interrupt(InterruptReason reason) noexcept {
        if (terminal_) {
            return 0;
        }
        auto cancelled = queued_.size() + (playing_.has_value() ? 1U : 0U);
        queued_.clear();
        playing_.reset();
        if (reason == InterruptReason::hard_halt) {
            cache_.clear();
            terminal_ = true;
        }
        return cancelled;
    }

    [[nodiscard]] std::size_t bind_generation(core::RuntimeGeneration generation) {
        if (!generation.valid()) {
            throw std::invalid_argument{"TTS generation must be valid"};
        }
        if (terminal_ || generation == generation_) {
            return 0;
        }
        const auto cancelled = interrupt(InterruptReason::generation_change);
        generation_ = generation;
        last_sequence_ = 0;
        return cancelled;
    }

    [[nodiscard]] TtsHaltReport hard_halt() noexcept {
        if (terminal_) {
            return {0, false, true, true};
        }
        const auto was_playing = playing_.has_value();
        const auto cancelled = queued_.size() + (was_playing ? 1U : 0U);
        queued_.clear();
        playing_.reset();
        cache_.clear();
        terminal_ = true;
        return {cancelled, was_playing, true, true};
    }

    [[nodiscard]] std::size_t queued() const noexcept { return queued_.size(); }
    [[nodiscard]] const std::optional<TtsItem>& playing() const noexcept { return playing_; }
    [[nodiscard]] bool cached(const MediaId& media_id) const noexcept {
        return cache_.contains(std::string{media_id.value()});
    }
    [[nodiscard]] bool terminal() const noexcept { return terminal_; }

private:
    [[nodiscard]] bool contains_media(const MediaId& media_id) const noexcept {
        const auto same = [&](const TtsItem& item) { return item.media_id == media_id; };
        return (playing_ && same(*playing_)) || std::ranges::any_of(queued_, same) ||
               cache_.contains(std::string{media_id.value()});
    }

    core::RuntimeGeneration generation_;
    std::size_t maximum_items_;
    std::uint64_t last_sequence_{};
    std::deque<TtsItem> queued_;
    std::optional<TtsItem> playing_;
    std::unordered_map<std::string, std::string> cache_;
    bool terminal_{};
};

struct Vec3 final {
    double x{};
    double y{};
    double z{};

    [[nodiscard]] Vec3 operator-(const Vec3& other) const noexcept {
        return {x - other.x, y - other.y, z - other.z};
    }
};

[[nodiscard]] inline double dot(const Vec3& left, const Vec3& right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

[[nodiscard]] inline double length(const Vec3& value) noexcept { return std::sqrt(dot(value, value)); }

[[nodiscard]] inline std::optional<Vec3> normalized(const Vec3& value) noexcept {
    const auto magnitude = length(value);
    if (!std::isfinite(magnitude) || magnitude <= 1e-9) {
        return std::nullopt;
    }
    return Vec3{value.x / magnitude, value.y / magnitude, value.z / magnitude};
}

struct Pose final {
    Vec3 position;
    Vec3 forward;
    Vec3 up;
    std::uint64_t sample{};
};

enum class RuntimeView { flat, vr };

enum class ListenerResult { accepted, missing_pose, stale_hmd, invalid_pose };

class SpatialScene final {
public:
    [[nodiscard]] ListenerResult update_listener(RuntimeView runtime,
                                                 const std::optional<Pose>& flat_player,
                                                 const std::optional<Pose>& vr_hmd,
                                                 std::uint64_t frame_sample) noexcept {
        const Pose* selected{};
        if (runtime == RuntimeView::flat) {
            if (!flat_player) {
                listener_.reset();
                return ListenerResult::missing_pose;
            }
            selected = &*flat_player;
        } else {
            if (!vr_hmd) {
                listener_.reset();
                return ListenerResult::missing_pose;
            }
            if (vr_hmd->sample != frame_sample) {
                listener_.reset();
                return ListenerResult::stale_hmd;
            }
            selected = &*vr_hmd;
        }
        if (!valid_pose(*selected)) {
            listener_.reset();
            return ListenerResult::invalid_pose;
        }
        listener_ = *selected;
        return ListenerResult::accepted;
    }

    [[nodiscard]] bool set_source(std::string speaker_id, Vec3 position) {
        if (!detail::opaque_identifier(speaker_id) || !finite(position)) {
            return false;
        }
        sources_.insert_or_assign(std::move(speaker_id), position);
        return true;
    }

    [[nodiscard]] std::optional<double> distance_to(std::string_view speaker_id) const noexcept {
        if (!listener_) {
            return std::nullopt;
        }
        const auto source = sources_.find(std::string{speaker_id});
        if (source == sources_.end()) {
            return std::nullopt;
        }
        return length(source->second - listener_->position);
    }

    [[nodiscard]] std::optional<double> azimuth_sine(std::string_view speaker_id) const noexcept {
        if (!listener_) {
            return std::nullopt;
        }
        const auto source = sources_.find(std::string{speaker_id});
        if (source == sources_.end()) {
            return std::nullopt;
        }
        const auto direction = normalized(source->second - listener_->position);
        const auto forward = normalized(listener_->forward);
        const auto up = normalized(listener_->up);
        if (!direction || !forward || !up) {
            return std::nullopt;
        }
        const Vec3 right{forward->y * up->z - forward->z * up->y,
                         forward->z * up->x - forward->x * up->z,
                         forward->x * up->y - forward->y * up->x};
        const auto unit_right = normalized(right);
        return unit_right ? std::optional<double>{dot(*direction, *unit_right)} : std::nullopt;
    }

    void clear() noexcept {
        listener_.reset();
        sources_.clear();
    }

    [[nodiscard]] bool has_listener() const noexcept { return listener_.has_value(); }

private:
    [[nodiscard]] static bool finite(const Vec3& value) noexcept {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    [[nodiscard]] static bool valid_pose(const Pose& pose) noexcept {
        return finite(pose.position) && normalized(pose.forward).has_value() &&
               normalized(pose.up).has_value();
    }

    std::optional<Pose> listener_;
    std::unordered_map<std::string, Vec3> sources_;
};

}  // namespace synth::audio
