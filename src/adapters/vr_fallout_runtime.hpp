#pragma once

#include "adapters/detail/runtime_base.hpp"

#include <utility>

namespace synth::adapters {

class VrFalloutRuntime final : public detail::RuntimeBase {
public:
    VrFalloutRuntime(detail::CapturePump capture_pump,
                     detail::PresentationPump presentation_pump,
                     detail::ActionPump action_pump)
        : RuntimeBase{vr_descriptor, std::move(capture_pump), std::move(presentation_pump),
                      std::move(action_pump)} {}

};

}  // namespace synth::adapters
