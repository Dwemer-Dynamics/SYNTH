#pragma once

#include "context/context.hpp"
#include "core/snapshot.hpp"
#include "protocol_native/v1_codec.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

namespace synth::client {

// The single engine-free conversion from copied actors and selection results to wire identities,
// and the exact actor key every owner compares. PlayerDialogue::identity/actor_key forward here.
[[nodiscard]] inline protocol_native::Identity wire_identity(const core::ActorSnapshot& actor) {
    char form_id[11]{};
    std::snprintf(form_id, sizeof(form_id), "0x%08X", actor.form_id());
    return {form_id, actor.origin_plugin(), actor.playthrough_id(), actor.name()};
}

[[nodiscard]] inline protocol_native::Identity wire_identity(const context::TargetContext& actor) {
    char form_id[11]{};
    std::snprintf(form_id, sizeof(form_id), "0x%08X", actor.identity().form().form_id());
    return {form_id, actor.identity().form().plugin(), actor.identity().playthrough().value(),
            actor.name()};
}

[[nodiscard]] inline std::string wire_actor_key(const protocol_native::Identity& actor) {
    auto plugin = actor.origin_plugin;
    std::ranges::transform(plugin, plugin.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return actor.playthrough_id + "|" + plugin + "|" + actor.form_id;
}

}  // namespace synth::client
