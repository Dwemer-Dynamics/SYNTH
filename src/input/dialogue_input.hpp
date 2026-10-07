#pragma once

#include "core/generation.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace synth::input {

struct TextBounds final {
    std::size_t maximum_utf8_bytes{2'048};
    std::size_t maximum_code_points{1'024};
};

enum class TextEditResult {
    accepted,
    empty,
    malformed_utf8,
    byte_limit,
    code_point_limit,
    terminal,
};

namespace detail {

[[nodiscard]] inline std::optional<std::size_t> utf8_code_points(std::string_view text) noexcept {
    std::size_t count{};
    for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t width{};
        std::uint32_t value{};
        if (lead <= 0x7fU) {
            width = 1;
            value = lead;
        } else if (lead >= 0xc2U && lead <= 0xdfU) {
            width = 2;
            value = lead & 0x1fU;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            width = 3;
            value = lead & 0x0fU;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            width = 4;
            value = lead & 0x07U;
        } else {
            return std::nullopt;
        }
        if (index + width > text.size()) {
            return std::nullopt;
        }
        for (std::size_t offset = 1; offset < width; ++offset) {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) {
                return std::nullopt;
            }
            value = (value << 6U) | (continuation & 0x3fU);
        }
        if ((width == 3 && value < 0x800U) || (width == 4 && value < 0x10000U) ||
            (value >= 0xd800U && value <= 0xdfffU) || value > 0x10ffffU) {
            return std::nullopt;
        }
        ++count;
        index += width;
    }
    return count;
}

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

class TypedTextBuffer final {
public:
    explicit TypedTextBuffer(TextBounds bounds = {}) : bounds_{bounds} {
        if (bounds.maximum_utf8_bytes == 0 || bounds.maximum_code_points == 0) {
            throw std::invalid_argument{"typed text bounds must be non-zero"};
        }
    }

    [[nodiscard]] TextEditResult append(std::string_view text) {
        if (terminal_) {
            return TextEditResult::terminal;
        }
        if (text.empty()) {
            return TextEditResult::empty;
        }
        const auto added_points = detail::utf8_code_points(text);
        if (!added_points) {
            return TextEditResult::malformed_utf8;
        }
        if (text.size() > bounds_.maximum_utf8_bytes - bytes_.size()) {
            return TextEditResult::byte_limit;
        }
        if (*added_points > bounds_.maximum_code_points - code_points_) {
            return TextEditResult::code_point_limit;
        }
        bytes_.append(text);
        code_points_ += *added_points;
        return TextEditResult::accepted;
    }

    void erase_last_code_point() noexcept {
        if (terminal_ || bytes_.empty()) {
            return;
        }
        auto index = bytes_.size() - 1;
        while (index > 0 && (static_cast<unsigned char>(bytes_[index]) & 0xc0U) == 0x80U) {
            --index;
        }
        bytes_.erase(index);
        --code_points_;
    }

    [[nodiscard]] std::optional<std::string> take() {
        if (terminal_ || bytes_.empty()) {
            return std::nullopt;
        }
        auto result = std::move(bytes_);
        bytes_.clear();
        code_points_ = 0;
        return result;
    }

    void clear() noexcept {
        bytes_.clear();
        code_points_ = 0;
    }

    void hard_halt() noexcept {
        clear();
        terminal_ = true;
    }

    [[nodiscard]] std::string_view text() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t code_points() const noexcept { return code_points_; }
    [[nodiscard]] bool terminal() const noexcept { return terminal_; }

private:
    TextBounds bounds_;
    std::string bytes_;
    std::size_t code_points_{};
    bool terminal_{};
};

enum class Hand { none, left, right };
enum class InputDevice { vr_controller, game_action, keyboard };
enum class SemanticAction { push_to_talk };
enum class ButtonTransition { pressed, released };

struct SemanticInputEvent final {
    SemanticAction action{SemanticAction::push_to_talk};
    ButtonTransition transition{ButtonTransition::released};
    InputDevice device{InputDevice::game_action};
    Hand hand{Hand::none};
};

struct PushToTalkBinding final {
    Hand controller_hand{Hand::right};
    bool keyboard_development_fallback{false};
};

enum class PttEventResult { accepted, wrong_hand, fallback_disabled, terminal };

class PushToTalkState final {
public:
    explicit PushToTalkState(PushToTalkBinding binding = {}) : binding_{binding} {
        if (binding.controller_hand == Hand::none) {
            throw std::invalid_argument{"push-to-talk controller hand must be left or right"};
        }
    }

    [[nodiscard]] PttEventResult apply(const SemanticInputEvent& event) noexcept {
        if (terminal_) {
            return PttEventResult::terminal;
        }
        if (event.device == InputDevice::keyboard) {
            if (!binding_.keyboard_development_fallback) {
                return PttEventResult::fallback_disabled;
            }
        } else if (event.device == InputDevice::vr_controller && event.hand != binding_.controller_hand) {
            return PttEventResult::wrong_hand;
        }
        active_ = event.transition == ButtonTransition::pressed;
        return PttEventResult::accepted;
    }

    void release() noexcept { active_ = false; }
    void hard_halt() noexcept {
        active_ = false;
        terminal_ = true;
    }

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool terminal() const noexcept { return terminal_; }

private:
    PushToTalkBinding binding_;
    bool active_{};
    bool terminal_{};
};

struct VoiceCaptureOptions final {
    bool open_microphone{false};
    bool voice_activity_detection{false};
};

enum class CaptureSuspension : std::uint8_t {
    menu = 1U << 0U,
    load = 1U << 1U,
    server_loss = 1U << 2U,
    halt = 1U << 3U,
};

class VoiceCaptureGate final {
public:
    explicit VoiceCaptureGate(VoiceCaptureOptions options = {}) noexcept : options_{options} {}

    void set_push_to_talk(bool active) noexcept { push_to_talk_ = active; }

    void set_suspended(CaptureSuspension reason, bool suspended) noexcept {
        const auto bit = static_cast<std::uint8_t>(reason);
        if (suspended) {
            suspensions_ |= bit;
        } else if (reason != CaptureSuspension::halt) {
            suspensions_ &= static_cast<std::uint8_t>(~bit);
        }
    }

    [[nodiscard]] bool should_capture(bool vad_detects_speech = false) const noexcept {
        if (suspensions_ != 0) {
            return false;
        }
        if (push_to_talk_) {
            return true;
        }
        return options_.open_microphone &&
               (!options_.voice_activity_detection || vad_detects_speech);
    }

    void hard_halt() noexcept {
        push_to_talk_ = false;
        set_suspended(CaptureSuspension::halt, true);
    }

    [[nodiscard]] VoiceCaptureOptions options() const noexcept { return options_; }
    [[nodiscard]] bool suspended() const noexcept { return suspensions_ != 0; }
    [[nodiscard]] bool terminal() const noexcept {
        return (suspensions_ & static_cast<std::uint8_t>(CaptureSuspension::halt)) != 0;
    }

private:
    VoiceCaptureOptions options_;
    std::uint8_t suspensions_{};
    bool push_to_talk_{};
};

enum class UploadState { pending, uploading, cancelled };
enum class UploadResult { accepted, duplicate, invalid_reference, capacity, byte_limit, terminal };

struct UploadSnapshot final {
    std::string reference;
    core::RuntimeGeneration generation;
    std::size_t bytes{};
    UploadState state{UploadState::pending};
};

struct UploadHaltReport final {
    std::size_t cancelled{};
    std::size_t released_bytes{};
    bool terminal{};
};

class SttUploadState final {
public:
    SttUploadState(std::size_t maximum_uploads = 4, std::size_t maximum_bytes = 4U * 1024U * 1024U)
        : maximum_uploads_{maximum_uploads}, maximum_bytes_{maximum_bytes} {
        if (maximum_uploads == 0 || maximum_bytes == 0) {
            throw std::invalid_argument{"STT upload bounds must be non-zero"};
        }
    }

    [[nodiscard]] UploadResult add(std::string reference,
                                   core::RuntimeGeneration generation,
                                   std::size_t bytes) {
        if (terminal_) {
            return UploadResult::terminal;
        }
        if (!detail::opaque_identifier(reference) || !generation.valid() || bytes == 0) {
            return UploadResult::invalid_reference;
        }
        if (find(reference) != uploads_.end()) {
            return UploadResult::duplicate;
        }
        if (uploads_.size() >= maximum_uploads_) {
            return UploadResult::capacity;
        }
        if (bytes > maximum_bytes_ - bytes_) {
            return UploadResult::byte_limit;
        }
        bytes_ += bytes;
        uploads_.push_back({std::move(reference), generation, bytes, UploadState::pending});
        return UploadResult::accepted;
    }

    [[nodiscard]] bool mark_uploading(std::string_view reference) noexcept {
        const auto item = find(reference);
        if (terminal_ || item == uploads_.end() || item->state != UploadState::pending) {
            return false;
        }
        item->state = UploadState::uploading;
        return true;
    }

    [[nodiscard]] bool complete(std::string_view reference,
                                core::RuntimeGeneration generation) noexcept {
        const auto item = find(reference);
        if (terminal_ || item == uploads_.end() || item->generation != generation ||
            item->state == UploadState::cancelled) {
            return false;
        }
        bytes_ -= item->bytes;
        uploads_.erase(item);
        return true;
    }

    [[nodiscard]] std::size_t cancel_generation(core::RuntimeGeneration generation) noexcept {
        std::size_t cancelled{};
        for (auto& upload : uploads_) {
            if (upload.generation == generation && upload.state != UploadState::cancelled) {
                upload.state = UploadState::cancelled;
                bytes_ -= upload.bytes;
                ++cancelled;
            }
        }
        return cancelled;
    }

    void discard_cancelled() noexcept {
        std::erase_if(uploads_, [](const auto& upload) { return upload.state == UploadState::cancelled; });
    }

    [[nodiscard]] UploadHaltReport hard_halt() noexcept {
        if (terminal_) {
            return {0, 0, true};
        }
        const auto released = bytes_;
        const auto cancelled = static_cast<std::size_t>(std::ranges::count_if(
            uploads_, [](const auto& upload) { return upload.state != UploadState::cancelled; }));
        uploads_.clear();
        bytes_ = 0;
        terminal_ = true;
        return {cancelled, released, true};
    }

    [[nodiscard]] const std::deque<UploadSnapshot>& uploads() const noexcept { return uploads_; }
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool terminal() const noexcept { return terminal_; }

private:
    [[nodiscard]] std::deque<UploadSnapshot>::iterator find(std::string_view reference) noexcept {
        return std::ranges::find(uploads_, reference, &UploadSnapshot::reference);
    }

    std::size_t maximum_uploads_;
    std::size_t maximum_bytes_;
    std::size_t bytes_{};
    std::deque<UploadSnapshot> uploads_;
    bool terminal_{};
};

}  // namespace synth::input
