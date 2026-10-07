#pragma once

#include "protocol_native/v1_codec.hpp"
#include "runtime/fallout_runtime.hpp"

namespace synth::client {

// Bind only actors present in this response's immutable scene, never a current target or a name match.
[[nodiscard]] inline std::shared_ptr<const runtime::RuntimeFacingRequest> bind_speech_facing(
    const protocol_native::Line& line, const protocol_native::Dialogue& dialogue,
    const core::RuntimeSnapshot& snapshot, const core::CancellationToken& cancellation) {
    const auto* speaker = std::get_if<protocol_native::Identity>(&dialogue.speaker);
    const auto utterance = dialogue.speech ? dialogue.speech->utterance_id : line.line_id;
    if (!speaker || !dialogue.listener || line.protocol_version != 2 ||
        line.request_id.empty() || line.turn_id.empty() || utterance.empty() ||
        line.generation != snapshot.generation().value() || line.context_sequence == 0 ||
        line.runtime_variant != core::to_string(snapshot.variant()) ||
        cancellation.generation() != snapshot.generation() || cancellation.is_cancelled()) return {};
    const auto resolve = [&](const protocol_native::Identity& identity) -> const core::ActorSnapshot* {
        std::uint32_t form{};
        if (identity.form_id.size() != 10 || !identity.form_id.starts_with("0x")) return nullptr;
        const auto [end, error] = std::from_chars(identity.form_id.data()+2,
            identity.form_id.data()+identity.form_id.size(), form, 16);
        if (error != std::errc{} || end != identity.form_id.data()+identity.form_id.size() || form == 0) return nullptr;
        const auto matches = [&](const core::ActorSnapshot& actor) {
            const auto lower = [](unsigned char ch) { return ch >= 'A' && ch <= 'Z' ? ch+('a'-'A') : ch; };
            return actor.form_id() == form && actor.playthrough_id() == identity.playthrough_id &&
                std::ranges::equal(actor.origin_plugin(), identity.origin_plugin,
                    [&](auto left, auto right) { return lower(left) == lower(right); });
        };
        if (matches(snapshot.player())) return &snapshot.player();
        const core::ActorSnapshot* found{};
        for (const auto& actor : snapshot.actors()) if (matches(actor)) {
            if (found) return nullptr;
            found = &actor;
        }
        return found;
    };
    const auto* source = resolve(*speaker);
    const auto* target = resolve(*dialogue.listener);
    if (!source || !target || source->form_id() == snapshot.player().form_id() ||
        source->form_id() == target->form_id() || source->playthrough_id() != snapshot.player().playthrough_id() ||
        target->playthrough_id() != source->playthrough_id()) return {};
    return std::make_shared<const runtime::RuntimeFacingRequest>(runtime::RuntimeFacingRequest{
        line.generation, line.context_sequence, line.request_id, line.turn_id, utterance,
        {source->form_id(), source->origin_plugin(), source->playthrough_id()},
        {target->form_id(), target->origin_plugin(), target->playthrough_id()}, cancellation});
}

}  // namespace synth::client
