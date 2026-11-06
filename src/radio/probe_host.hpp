#pragma once
#include "radio/diagnostic.hpp"
#include "runtime/fallout_runtime.hpp"
#include "json/json.hpp"
#include <chrono>
#include <cmath>
#include <locale>
#include <sstream>

namespace synth::radio {

[[nodiscard]] inline std::string probe_json(const ProbeObservation& value, bool enabled,
                                           std::uint64_t sequence, std::string_view status) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    const auto status_json = json::write(json::Value{std::string{status.substr(0, 128)}});
    out << std::boolalpha << "{\"schema\":\"synth.radio.probe.v1\",\"generation\":" << value.generation.value()
        << ",\"sequence\":" << sequence << ",\"enabled\":" << enabled
        << ",\"supported\":" << value.supported << ",\"records_available\":" << value.records_available
        << ",\"status\":" << status_json << ",\"radio_on\":" << value.radio_on
        << ",\"frequency\":" << (std::isfinite(value.frequency) ? value.frequency : 0) << ",\"slots\":[";
    for (std::size_t i = 0; i < value.slots.size(); ++i) {
        const auto& slot = value.slots[i];
        if (i) out << ',';
        out << "{\"slot\":\"" << (i ? 'b' : 'a') << "\",\"available\":" << slot.available
            << ",\"scene_active\":" << slot.scene_active << ",\"phase\":" << slot.phase
            << ",\"action_status\":" << slot.action_status
            << ",\"action_base_status\":" << action_base_status(slot.action_status)
            << ",\"action_status_flags\":" << action_status_flags(slot.action_status)
            << ",\"sound_done\":" << slot.sound_done
            << ",\"sound_id\":" << slot.sound_id << ",\"sound_valid\":" << slot.sound_valid
            << ",\"native_descriptor_id\":" << slot.native_descriptor_id
            << ",\"expected_descriptor_id\":" << slot.expected_descriptor_id
            << ",\"native_descriptor_owned\":" << (slot.sound_valid && slot.expected_descriptor_id != 0 &&
                slot.expected_descriptor_id != 0xFFFFFFFFU && slot.native_descriptor_id == slot.expected_descriptor_id)
            << ",\"sound_state_available\":" << slot.sound_state_available
            << ",\"sound_busy\":" << slot.sound_busy << ",\"sound_paused\":" << slot.sound_paused
            << ",\"sound_playing\":null"
            << ",\"action_duration_seconds\":" << (std::isfinite(slot.action_duration) ? slot.action_duration : 0)
            << ",\"handle_duration_units\":" << slot.handle_duration
            << ",\"native_duration_milliseconds\":" << slot.handle_duration << '}';
    }
    out << "],\"dcr_scenes\":[";
    for (std::size_t i = 0; i < value.dcr_scenes.size(); ++i) {
        const auto& scene = value.dcr_scenes[i];
        if (i) out << ',';
        out << "{\"local_form_id\":" << scene.local_form_id << ",\"available\":" << scene.available
            << ",\"active\":" << scene.active << ",\"phase\":" << scene.phase << '}';
    }
    out << "],\"live_rewrite_authorized\":false,\"audible_cache_refresh_verified\":false}";
    return out.str();
}

class ProbeHost final {
public:
    // Called by the deferred lifecycle handler on the game thread, without engine access.
    // VM/lifecycle callbacks may invalidate only the synchronized probe_mailbox directly.
    void invalidate() {
        probe_mailbox.invalidate();
        generation_ = {};
        enabled_ = false;
        report_.clear();
        cleanup_pending_ = true;
        next_ = {};
    }
    // The adapter owns all RE access. No network, decoding, filesystem reads or publications here.
    void tick(runtime::IFalloutRuntime& runtime, core::SnapshotClock::time_point now) {
        runtime.assert_game_thread();
        const auto generation = runtime.generation();
        if (generation_ != generation) {
            generation_ = generation;
            probe_mailbox.invalidate(generation);
            enabled_ = false;
            cleanup_pending_ = true;
            next_ = {};
        }
        if (cleanup_pending_) {
            // Fresh owned-record lookup only; never retain a pre-load sound ID or engine pointer.
            (void)runtime.execute_radio_probe({generation, ProbeOperation::disable, 0});
            cleanup_pending_ = false;
        }
        const auto request = probe_mailbox.take(generation);
        if (request) {
            const auto result = runtime.execute_radio_probe(*request);
            status_ = result.detail;
            if (result.succeeded && request->operation == ProbeOperation::enable) enabled_ = true;
            if (request->operation == ProbeOperation::disable) enabled_ = false;
            next_ = {};
        }
        if (!enabled_ && !request) return;
        if (now < next_) return;
        next_ = now + std::chrono::milliseconds{100};
        const auto observation = runtime.capture_radio_probe();
        if (observation.generation != generation) return;
        const auto diagnostic = probe_json(observation, enabled_, ++sequence_, status_);
        probe_mailbox.publish(generation, enabled_, diagnostic);
        if (request) report_ = diagnostic;
    }
    [[nodiscard]] std::string take_report() { auto result = std::move(report_); report_.clear(); return result; }
private:
    core::RuntimeGeneration generation_;
    bool enabled_{}, cleanup_pending_{true};
    std::uint64_t sequence_{};
    core::SnapshotClock::time_point next_{};
    std::string status_{"disabled"};
    std::string report_;
};
} // namespace synth::radio
