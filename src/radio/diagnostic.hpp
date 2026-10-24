#pragma once

#include "core/generation.hpp"
#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace synth::radio {

enum class ProbeOperation : unsigned char { enable, play, stop, disable, observe };
struct ProbeRequest final {
    core::RuntimeGeneration generation;
    ProbeOperation operation{};
    unsigned char slot{};
    [[nodiscard]] bool valid() const noexcept {
        return generation.valid() && slot < 2 &&
            static_cast<unsigned>(operation) <= static_cast<unsigned>(ProbeOperation::observe);
    }
};
struct SlotObservation final {
    bool available{}, scene_active{}, sound_done{}, sound_valid{};
    bool sound_state_available{}, sound_busy{}, sound_paused{};
    std::uint32_t phase{}, action_status{}, sound_id{0xFFFFFFFFU};
    std::uint32_t native_descriptor_id{0xFFFFFFFFU}, expected_descriptor_id{0xFFFFFFFFU};
    float action_duration{};
    std::uint64_t handle_duration{};
    // This is a copied diagnostic, never an authorization to rewrite a live audio file.
    // sound_id is the action's descriptor FormID, not a BSSoundHandle ID. The
    // manager's busy result includes queued, playing or paused selected-listener audio;
    // it does not independently establish audibility or strictly playing state.
};
enum class SlotReadiness : unsigned char { ready, busy, state_unavailable, descriptor_mismatch };
[[nodiscard]] constexpr std::uint32_t action_base_status(std::uint32_t raw) noexcept { return raw & 3U; }
[[nodiscard]] constexpr std::uint32_t action_status_flags(std::uint32_t raw) noexcept { return raw & ~3U; }
[[nodiscard]] inline SlotReadiness slot_readiness(const SlotObservation& value,
                                                 std::uint32_t owned_descriptor_id) noexcept {
    constexpr std::uint32_t invalid = 0xFFFFFFFFU;
    if (!value.available || !value.sound_state_available || !owned_descriptor_id || owned_descriptor_id == invalid ||
        value.expected_descriptor_id != owned_descriptor_id)
        return SlotReadiness::state_unavailable;
    if (value.sound_valid != (value.native_descriptor_id != invalid)) return SlotReadiness::state_unavailable;
    if ((value.sound_id != invalid && value.sound_id != owned_descriptor_id) ||
        (value.native_descriptor_id != invalid && value.native_descriptor_id != owned_descriptor_id))
        return SlotReadiness::descriptor_mismatch;
    const auto status = action_base_status(value.action_status);
    if (value.scene_active || value.phase != invalid || value.sound_busy || value.sound_paused || status == 1 || status == 3)
        return SlotReadiness::busy;
    // EndAction clears soundDone to false. A valid but finished native handle,
    // or the action's preserved upper status flags, must not veto a new play.
    return SlotReadiness::ready;
}
struct SceneObservation final {
    std::uint32_t local_form_id{}, phase{};
    bool available{}, active{};
};
struct ProbeObservation final {
    core::RuntimeGeneration generation;
    bool supported{}, records_available{}, radio_on{};
    float frequency{};
    std::array<SlotObservation, 2> slots;
    std::array<SceneObservation, 5> dcr_scenes;
};
struct ProbeResult final {
    bool succeeded{};
    std::string detail{"unsupported_runtime"};
};

// VM callbacks touch primitive copied values only. One pending command bounds work and
// rejects overlap rather than replacing a previous user's request.
class ProbeMailbox final {
public:
    [[nodiscard]] bool submit(ProbeRequest request) {
        std::scoped_lock lock{mutex_};
        if (!request.valid() || !generation_.valid() || request.generation != generation_ || pending_) return false;
        if (!enabled_ && request.operation != ProbeOperation::enable) return false;
        pending_ = request;
        return true;
    }
    [[nodiscard]] std::optional<ProbeRequest> take(core::RuntimeGeneration generation) {
        std::scoped_lock lock{mutex_};
        if (generation != generation_) return std::nullopt;
        auto request = pending_;
        pending_.reset();
        return request;
    }
    void publish(core::RuntimeGeneration generation, bool enabled, std::string diagnostic) {
        std::scoped_lock lock{mutex_};
        if (generation != generation_) return;
        enabled_ = enabled;
        diagnostic_ = diagnostic.size() <= 8192 ? std::move(diagnostic) : "{\"error\":\"oversized_diagnostic\"}";
    }
    void invalidate(core::RuntimeGeneration generation = {}) {
        std::scoped_lock lock{mutex_};
        generation_ = generation;
        pending_.reset();
        enabled_ = false;
        diagnostic_ = "{\"schema\":\"synth.radio.probe.v1\",\"enabled\":false,\"status\":\"invalidated\"}";
    }
    [[nodiscard]] core::RuntimeGeneration generation() const {
        std::scoped_lock lock{mutex_};
        return generation_;
    }
    [[nodiscard]] std::string diagnostic() const {
        std::scoped_lock lock{mutex_};
        return diagnostic_;
    }
    [[nodiscard]] bool enabled() const {
        std::scoped_lock lock{mutex_};
        return enabled_;
    }
private:
    mutable std::mutex mutex_;
    core::RuntimeGeneration generation_;
    bool enabled_{};
    std::optional<ProbeRequest> pending_;
    std::string diagnostic_{"{\"schema\":\"synth.radio.probe.v1\",\"enabled\":false}"};
};

inline ProbeMailbox probe_mailbox;
} // namespace synth::radio
