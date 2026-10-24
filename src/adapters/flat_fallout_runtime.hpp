#pragma once

#include "adapters/detail/runtime_base.hpp"

#include <utility>

namespace synth::adapters {

class FlatFalloutRuntime final : public detail::RuntimeBase {
public:
    using RadioCapturePump = std::function<radio::ProbeObservation(core::RuntimeGeneration)>;
    using RadioActionPump = std::function<radio::ProbeResult(const radio::ProbeRequest&)>;
    FlatFalloutRuntime(detail::CapturePump capture_pump,
                       detail::PresentationPump presentation_pump,
                       detail::ActionPump action_pump)
        : RuntimeBase{flat_descriptor, std::move(capture_pump), std::move(presentation_pump),
                      std::move(action_pump)} {}

    void set_radio_probe_pumps(RadioCapturePump capture, RadioActionPump action) {
        assert_game_thread();
        radio_capture_ = std::move(capture);
        radio_action_ = std::move(action);
    }
    [[nodiscard]] radio::ProbeObservation capture_radio_probe() override {
        assert_game_thread();
        return radio_capture_ ? radio_capture_(generation()) : radio::ProbeObservation{generation()};
    }
    [[nodiscard]] radio::ProbeResult execute_radio_probe(const radio::ProbeRequest& request) override {
        assert_game_thread();
        if (!request.valid() || request.generation != generation()) return {false, "stale_generation"};
        return radio_action_ ? radio_action_(request) : radio::ProbeResult{};
    }
private:
    RadioCapturePump radio_capture_;
    RadioActionPump radio_action_;

};

}  // namespace synth::adapters
