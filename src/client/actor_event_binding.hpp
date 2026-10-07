#pragma once

#include "context/context.hpp"
#include "core/actor_event_mailbox.hpp"
#include <span>

namespace synth::client {

// Base-form metadata only; no current inventory count, instance name, consumption or stack ownership claim.
struct ActorEventItem final {
    std::uint32_t form_id{};
    std::string origin_plugin, name;
    std::uint16_t form_type{};
    [[nodiscard]] bool valid() const noexcept {
        return form_id && form_id!=UINT32_MAX && context::valid_plugin_name(origin_plugin) &&
            !name.empty() && name.size()<=255 && form_type>0 && form_type<256;
    }
};

struct ActorEventMetadata final {
    std::shared_ptr<const core::RuntimeSnapshot> scene;
    std::vector<ActorEventItem> items;
};

enum class ActorEventKnowledge : unsigned char { unavailable, player_equipment };
struct BoundActorEvent final {
    core::ActorEvent evidence;
    context::ActorIdentity subject;
    // Missing with evidence.other_actor!=0 means unresolved, not an absent/unknown native killer.
    std::optional<context::ActorIdentity> other;
    std::optional<ActorEventItem> item;
    ActorEventKnowledge player_knowledge{ActorEventKnowledge::unavailable};
};
struct BoundActorEventScene final {
    std::string session_id;
    std::uint64_t native_epoch{};
    std::shared_ptr<const core::RuntimeSnapshot> scene;
    std::vector<BoundActorEvent> events;
    std::size_t rejected{};
};

// Game-pump-owned binding to one arm-time owner. No engine access or knowledge inferred from proximity/late LOS.
class ActorEventSceneBinder final {
public:
    using Snapshot=std::shared_ptr<const core::RuntimeSnapshot>;
    static constexpr auto maximum_capture_delay=std::chrono::seconds{2};
    ActorEventSceneBinder(std::string session_id,std::uint64_t native_epoch,Snapshot arm_scene)
        : session_id_{std::move(session_id)},epoch_{native_epoch},owner_{std::move(arm_scene)},latest_{owner_} {
        if (session_id_.empty() || session_id_.size()>128 || !(epoch_&1) || !owner_ ||
            owner_->variant()!=core::RuntimeVariant::flat || owner_->player().form_id()!=0x14 ||
            owner_->player().playthrough_id()=="unknown" || owner_->captured_at()==core::SnapshotClock::time_point{} ||
            owner_->game_time_ticks()==0 || owner_->game_time_ticks()>core::RuntimeGeneration::maximum_wire_value ||
            owner_->frame()==0)
            throw std::invalid_argument{"actor event arm owner is unavailable"};
    }

    // Caller rechecks lifecycle after native metadata capture. This immutable result is retained unchanged for later delivery.
    [[nodiscard]] std::shared_ptr<const BoundActorEventScene> bind(
        std::string_view current_session,std::uint64_t current_epoch,Snapshot scene,
        std::span<const core::ActorEvent> events,const ActorEventMetadata& metadata,
        core::SnapshotClock::time_point now) {
        if (current_session!=session_id_ || current_epoch!=epoch_ || !scene || metadata.scene!=scene ||
            scene->variant()!=owner_->variant() || scene->generation()!=owner_->generation() ||
            context::identity_of(scene->player())!=context::identity_of(owner_->player()) ||
            scene->frame()<latest_->frame() || scene->captured_at()<latest_->captured_at() ||
            scene->game_time_ticks()<latest_->game_time_ticks() ||
            scene->game_time_ticks()>core::RuntimeGeneration::maximum_wire_value ||
            now<scene->captured_at() || now-scene->captured_at()>maximum_capture_delay ||
            events.empty() || events.size()>core::ActorEventMailbox::capacity ||
            metadata.items.size()>core::ActorEventMailbox::capacity) return {};
        latest_=scene; // Older captures cannot bind after a newer observed save frontier, even within one generation.
        auto bound=std::make_shared<BoundActorEventScene>();
        bound->session_id=session_id_;bound->native_epoch=epoch_;bound->scene=std::move(scene);
        const auto& snapshot=*bound->scene;
        // A duplicated loaded FormID is ambiguous even if names/plugins happen to match.
        const auto resolve=[&](std::uint32_t id)->const core::ActorSnapshot* {
            const core::ActorSnapshot* result=id==snapshot.player().form_id() ? &snapshot.player() : nullptr;
            for (const auto& actor:snapshot.actors()) {
                if (actor.form_id()!=id) continue;
                if (result) return nullptr;
                result=&actor;
            }
            if (result && (result->playthrough_id()!=snapshot.player().playthrough_id() || result->disabled())) return nullptr;
            return result;
        };
        bound->events.reserve(events.size());
        for (const auto& event:events) {
            const auto* subject=resolve(event.actor);
            if (!event.valid() || !subject || event.observed_at==core::SnapshotClock::time_point{} ||
                event.observed_at<owner_->captured_at() || event.observed_at>snapshot.captured_at() ||
                now-event.observed_at>maximum_capture_delay) { ++bound->rejected;continue; }
            std::optional<ActorEventItem> item;
            if (event.kind!=core::ActorEventKind::death) {
                const ActorEventItem* found{};
                bool duplicate=false;
                for (const auto& candidate:metadata.items) {
                    if (candidate.form_id!=event.base_object) continue;
                    if (found) { duplicate=true;break; }
                    found=&candidate;
                }
                if (!found || duplicate || !found->valid()) { ++bound->rejected;continue; }
                item=*found;
            }
            std::optional<context::ActorIdentity> other;
            if (event.other_actor) if (const auto* actor=resolve(event.other_actor)) other=context::identity_of(*actor);
            const auto knowledge=event.actor==snapshot.player().form_id() && event.kind!=core::ActorEventKind::death
                ? ActorEventKnowledge::player_equipment : ActorEventKnowledge::unavailable;
            bound->events.push_back({event,context::identity_of(*subject),std::move(other),std::move(item),knowledge});
        }
        return bound;
    }

private:
    std::string session_id_;
    std::uint64_t epoch_{};
    Snapshot owner_,latest_;
};

} // namespace synth::client
