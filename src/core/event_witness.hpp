#pragma once

// Engine-free first-observation evidence for game-authored events: canonical witness identities
// granted once at the first qualified observation, and observed-residency tracking for vanilla
// dialogue lines. Neither claims a voice start, line of sight or proof that anyone listened.

#include "core/snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace synth::core {

struct WitnessIdentity final {
    std::uint32_t form_id{};
    std::string origin_plugin, playthrough_id, display_name;

    [[nodiscard]] bool valid() const noexcept {
        return form_id != 0 && form_id != UINT32_MAX && form_id >> 24 != 0xFF && !origin_plugin.empty() &&
            origin_plugin.size() <= 255 && !playthrough_id.empty() && playthrough_id.size() <= 128 &&
            !display_name.empty() && display_name.size() <= 255;
    }
    bool operator==(const WitnessIdentity&) const = default;
};

// Immutable once built. The listening policy is evaluated against the actual speaker in the same
// physical cell at the first observation; later arrivals never join and unknown evidence grants nothing.
struct EventWitnessSet final {
    static constexpr std::size_t capacity = 8;
    static constexpr std::string_view basis = "first_observed";
    static constexpr std::string_view policy = "same_cell_speaker_radius_1000_v1";
    static constexpr double radius = 1000.0;

    SceneIdentity scene;
    bool interior{};
    std::uint64_t frame{}, game_time_ticks{};
    std::vector<WitnessIdentity> actors;

    [[nodiscard]] bool valid() const noexcept {
        if (!scene.valid(interior) || frame == 0 || game_time_ticks == 0 ||
            game_time_ticks > RuntimeGeneration::maximum_wire_value || actors.size() > capacity) return false;
        for (std::size_t i = 0; i < actors.size(); ++i) {
            if (!actors[i].valid() || actors[i].form_id == 0x14) return false;
            for (std::size_t j = 0; j < i; ++j) if (actors[j].form_id == actors[i].form_id) return false;
        }
        return true;
    }
};

// Copied source evidence for one NPC line; never an engine pointer.
struct DialogueLine final {
    static constexpr std::size_t maximum_text_bytes = 512;
    WitnessIdentity speaker;
    std::uint32_t topic_info_form_id{};
    std::string topic_info_plugin, text;

    [[nodiscard]] bool valid() const noexcept {
        if (!speaker.valid() || speaker.form_id == 0x14 || topic_info_form_id == 0 || topic_info_form_id == UINT32_MAX ||
            topic_info_plugin.empty() || topic_info_plugin.size() > 255 || text.empty() || text.size() > maximum_text_bytes)
            return false;
        for (const unsigned char character : text) if (character < 0x20 && character != '\t') return false;
        return true;
    }
    bool operator==(const DialogueLine&) const = default;
};

struct DialogueObservation final {
    std::uint64_t serial{};
    DialogueLine line;
    EventWitnessSet witnesses;

    [[nodiscard]] bool valid() const noexcept {
        if (serial == 0 || serial > RuntimeGeneration::maximum_wire_value || !line.valid() || !witnesses.valid())
            return false;
        for (const auto& actor : witnesses.actors)
            if (actor.form_id == line.speaker.form_id || actor.playthrough_id != line.speaker.playthrough_id) return false;
        return true;
    }
};

// Game-pump-owned residency episodes over successful samples only. One line resident across polls is
// one observation; an observed absence or a positively observed excluded line (player, non-actor)
// followed by the same line is a new one. Unavailable samples (lock contention, unsupported state)
// change nothing, so they never invent an absence. Residency is keyed by the speaker's canonical
// identity, the INFO identity and the exact text; the mutable display name never starts a new line.
// Identical lines repeated between two polls are merged: this is sampling, not complete callback coverage.
class DialogueResidencyTracker final {
public:
    // A line already resident when arming is a baseline: a delayed first sample cannot claim earlier hearing.
    void arm(std::uint64_t owner) noexcept {
        owner_ = owner;
        baseline_pending_ = true;
        current_.reset();
    }
    void invalidate() noexcept { arm(0); }
    [[nodiscard]] std::uint64_t owner() const noexcept { return owner_; }

    void absent(std::uint64_t owner) noexcept {
        if (owner == 0 || owner != owner_) return;
        baseline_pending_ = false;
        current_.reset();
    }
    // A known non-NPC line ends any NPC residency without admitting anything itself.
    void excluded(std::uint64_t owner) noexcept { absent(owner); }

    // True only when this successful sample begins a new observed residency for the current owner.
    [[nodiscard]] bool present(std::uint64_t owner, const DialogueLine& line) {
        if (owner == 0 || owner != owner_ || !line.valid()) return false;
        if (baseline_pending_) {
            baseline_pending_ = false;
            current_ = line;
            return false;
        }
        if (current_ && same_line(*current_, line)) return false;
        current_ = line;
        return true;
    }

private:
    [[nodiscard]] static bool same_line(const DialogueLine& a, const DialogueLine& b) noexcept {
        return a.speaker.form_id == b.speaker.form_id && a.speaker.origin_plugin == b.speaker.origin_plugin &&
            a.speaker.playthrough_id == b.speaker.playthrough_id && a.topic_info_form_id == b.topic_info_form_id &&
            a.topic_info_plugin == b.topic_info_plugin && a.text == b.text;
    }
    std::uint64_t owner_{};
    bool baseline_pending_{true};
    std::optional<DialogueLine> current_;
};

} // namespace synth::core
