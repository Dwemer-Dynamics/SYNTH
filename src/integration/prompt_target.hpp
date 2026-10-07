#pragma once
#include "core/cancellation.hpp"
#include "core/snapshot.hpp"
#include <cstdint>
#include <string>

namespace synth::integration {
// Copied prompt identity and session liveness only; no actor, runtime, UI or session object is retained.
struct PromptTarget final {
    core::CancellationToken session;
    std::uint32_t form_id{}, base_form_id{};
    std::string origin_plugin, base_origin_plugin, playthrough_id, display_name;
    bool manual_chat{};
    std::string conversation_mode;
    std::uint64_t conversation_model_slot{1};
    bool nearby_conversation{};

    // Names and poses may change; a retained recipient must keep its full canonical identity.
    [[nodiscard]] bool matches(const core::ActorSnapshot& actor) const noexcept {
        return actor.form_id() == form_id && actor.base_form_id() == base_form_id &&
            actor.origin_plugin() == origin_plugin && actor.base_origin_plugin() == base_origin_plugin &&
            actor.playthrough_id() == playthrough_id;
    }
};
}
