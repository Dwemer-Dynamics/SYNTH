#pragma once

#include "client/actor_event_delivery.hpp"
#include "client/actor_state_combat.hpp"
#include "client/dialogue_requests.hpp"
#include "client/player_dialogue.hpp"
#include "client/quest_event_delivery.hpp"
#include "client/social_task_delivery.hpp"
#include "client/synth_client.hpp"
#include "client/wait_action.hpp"
#include "config/config.hpp"
#include "context/context.hpp"
#include "core/save_context.hpp"
#include "core/snapshot.hpp"
#include "targeting/targeting.hpp"
#include "tasks/task_lanes.hpp"
#include "voice/voice_sample_worker.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace synth::client {

// Owns one session's registration and context publication: capability construction, server
// initialization, full/delta context encoding and publication, the acknowledged world/quest frame,
// the exact save binding and write epoch, the history checkpoint frontier with its wait notice,
// the acknowledged inventory frame, the world observation retry gate and the dynamic profile refresh
// that publishes its own context first. SaveContextStore stays
// the persisted-binding authority; this object only borrows it with the binding it was created for.
//
// Every publication runs on a network worker. Observation, profile refresh and session request tasks
// hold client_.begin_context_turn() across publish(); the manual publish_context() task does not, so only
// SynthClient's own request lock spans its HTTP exchange. world_context_mutex_ guards only copied
// observation values and is never held over HTTP or game work. Acknowledgement follows a complete, uncancelled, current-generation response:
// the world/quest frame never moves behind a newer frame of the same generation, the save ACK is
// tied to the binding epoch, and the history and inventory frontiers only rise.
//
// The session keeps cross-feature admission (context_observation_ready), producer scheduling,
// target selection policy, quest/player/actor delivery and the social/turn state. It reaches that
// state only through three synchronous hooks: select_target on the submitting thread over the
// copied snapshot (the host coordinator for initialization, where the result is unused, or the
// publish_context() and refresh_dynamic_profiles() callers), and hydrate_social plus
// invalidate_response on the publishing worker.
//
// Lifetimes: the borrowed configuration, client, network lane, response accounting, voice samples,
// quest delivery, flags and save store outlive this object; it is declared after all of them. The
// hooks capture the session. Network tasks queued here capture this object and copied snapshots
// only; the session joins the network lane in its destructor body before any member is destroyed,
// and a rejected, expired or discarded task is destroyed unrun with its counter guard released once.
// The invalidate_response closure placed in each ContextBinding captures this object; bindings live
// only inside network tasks and their line callbacks, which that same join retires.
class ContextPublication final {
public:
    using Snapshot = std::shared_ptr<const core::RuntimeSnapshot>;
    using Notify = std::function<void(std::string)>;
    // Submitting-thread target over the copied snapshot; the worker never retargets.
    using SelectTarget = std::function<std::optional<protocol_native::Identity>(const core::RuntimeSnapshot&)>;
    // Runs on the publishing worker after a fresh, current, complete publication.
    using HydrateSocial = std::function<void(const RequestOutcome&, const core::RuntimeSnapshot&)>;
    // Owner-scoped rollback for a streaming response admitted under this publication.
    using InvalidateResponse = std::function<void(core::CancellationToken)>;

    struct Dependencies final {
        const config::Config& configuration;
        SynthClient& client;
        tasks::TaskLanes& network;
        DialogueRequests& requests;
        voice::SampleWorker& voice_samples;
        QuestEventDelivery& quest_events;
        bool quest_events_enabled;
        const std::atomic_bool& terminal;
        const std::atomic_bool& runtime_paused;
        core::CancellationToken session;
        core::SaveContextStore* save_context;
        core::SaveContextStore::Binding save_binding;
        SelectTarget select_target;
        HydrateSocial hydrate_social;
        InvalidateResponse invalidate_response;
        Notify notify;
    };

    explicit ContextPublication(Dependencies dependencies)
        : configuration_{dependencies.configuration},
          client_{dependencies.client},
          network_{dependencies.network},
          requests_{dependencies.requests},
          voice_samples_{dependencies.voice_samples},
          quest_events_{dependencies.quest_events},
          quest_events_enabled_{dependencies.quest_events_enabled},
          terminal_{dependencies.terminal},
          runtime_paused_{dependencies.runtime_paused},
          session_{std::move(dependencies.session)},
          save_context_{dependencies.save_context},
          save_binding_{std::move(dependencies.save_binding)},
          select_target_{std::move(dependencies.select_target)},
          hydrate_social_{std::move(dependencies.hydrate_social)},
          invalidate_response_{std::move(dependencies.invalidate_response)},
          notify_{std::move(dependencies.notify)} {
        if (!select_target_ || !hydrate_social_ || !invalidate_response_ || !notify_)
            throw std::invalid_argument{"context publication requires target, social, rollback and notification sinks"};
    }

    ContextPublication(const ContextPublication&) = delete;
    ContextPublication& operator=(const ContextPublication&) = delete;

    [[nodiscard]] static std::vector<std::string> capabilities(core::RuntimeVariant variant, bool rest_events_available,
                                                             bool quest_events_available, bool actor_events_available) {
        std::vector<std::string> result{
            "dialogue.text", "dialogue.audio", "dialogue.turn_ownership", "dialogue.turn_cancel", "dialogue.rechat.ownership", "dialogue.rechat.scene", "dialogue.listener_identity", "dialogue.ambient_npc", "dialogue.native_turn", "control.menu",
            "runtime.retirement", "context.saved_anchor", "media.binary", "context.actor_identity",
            "media.image.upload", "visual_context.capture", "visual_context.describe",
            "visual_context.portrait",
            "profiles.dynamic.refresh", "diary.manual.npc", "diary.manual.narrator", "diary.manual.player", "diary.batch.status",
            "context.actor_state", "context.actor_factions", "context.combat_targets", "context.world",
            "context.loaded_plugins", "context.quests",
            "context.nearby_items", "context.points_of_interest", "agents.manage",
            "action.inspect_actor", "action.inspect_surroundings",
            "action.check_inventory", "action.read_quests", "action.end_conversation",
            "action.sheathe_weapon"};
        append_wait_capabilities(result, variant);
        if (variant==core::RuntimeVariant::flat) result.push_back("action.attack");
        if (variant == core::RuntimeVariant::flat && quest_events_available) result.push_back("context.quest_events");
        if (variant == core::RuntimeVariant::flat && actor_events_available) result.push_back("context.actor_events");
        if (variant == core::RuntimeVariant::flat) {
            result.push_back("social.tasks.v1");
            result.push_back("context.location_identity");
            result.push_back("context.combat_contacts");
            result.push_back("context.enemy_awareness");
            result.push_back("action.equip_item");
            result.push_back("action.unequip_item");
            result.push_back("action.inventory_observation");
            result.push_back("action.consume");
            result.push_back("action.consume_inventory");
            result.push_back("action.give_item_to");
            result.push_back("action.transfer_inventory");
            result.push_back("action.give_caps_to");
            result.push_back("action.caps_inventory");
            result.push_back("action.take_caps_from_player");
            result.push_back("action.player_caps_inventory");
            result.push_back("action.take_item_from_player");
            result.push_back("action.player_item_inventory");
            result.push_back("action.open_inventory");
            result.push_back("action.gift_menu_inventory");
            result.push_back("context.inventory_512");
            result.push_back("context.player_events");
            result.push_back("dialogue.player_reactions");
            result.push_back("context.quest_tracking");
            result.push_back("dialogue.quest_reactions");
        }
        if (variant == core::RuntimeVariant::vr) {
            result.push_back("runtime.hmd_pose");
            result.push_back("runtime.controller_pose");
        } else if (rest_events_available) {
            result.push_back("diary.automatic.sleep");
            result.push_back("diary.automatic.wait");
        }
        return result;
    }

    [[nodiscard]] bool initialize(Snapshot snapshot) {
        if (!snapshot || snapshot->generation() != session_.generation() ||
            initialized_.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }
        if (!submit(std::move(snapshot), true)) {
            initialized_.store(false, std::memory_order_release);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool publish_context(Snapshot snapshot) {
        if (!snapshot || !client_.ready()) {
            return false;
        }
        return submit(std::move(snapshot), false);
    }

    // Game thread: copy up to 16 audible identities and the selected target; the worker publishes
    // that exact snapshot under the context transaction before requesting the refresh.
    [[nodiscard]] bool refresh_dynamic_profiles(Snapshot snapshot, std::string_view scope, std::uint32_t target_form_id) {
        if (!snapshot || !client_.ready() || terminal_.load(std::memory_order_acquire) ||
            runtime_paused_.load(std::memory_order_acquire)) {
            return false;
        }
        std::vector<protocol_native::Identity> actors;
        actors.reserve(std::min<std::size_t>(snapshot->actors().size(), 16));
        for (const auto& actor : snapshot->actors()) {
            if (scope == "narrator" || (scope == "target" && actor.form_id() != target_form_id)) continue;
            if (actors.size() == 16) break;
            if (targeting::distance(snapshot->player_pose().position(), actor.position()) <=
                    configuration_.activation.automatic_hearing_distance &&
                targeting::dialogue_actor_available(actor)) {
                actors.push_back(identity(actor));
            }
        }
        const auto include_narrator = scope == "narrator" ||
            (scope == "automatic" && configuration_.behavior.dynamic_profile_include_narrator);
        if (actors.empty() && !include_narrator) return false;
        const auto target = select_target_(*snapshot);
        const auto deadline = tasks::TaskLanes::Clock::now() +
                              std::chrono::milliseconds{request_timeout_ms() * 2ULL};
        const auto accepted = network_.try_submit(
            "network", tasks::TaskClass::background, deadline, session_,
            [this, actors = std::move(actors), include_narrator, snapshot = std::move(snapshot), target](
                const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                try {
                    const auto context_turn = client_.begin_context_turn();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    const auto context_outcome = publish(snapshot, target, cancellation);
                    if (context_outcome.status != RequestStatus::complete) {
                        notify_("SYNTH profile context failed: " + context_outcome.detail);
                        return;
                    }
                    const auto outcome = client_.refresh_dynamic_profiles(actors, include_narrator, context_outcome.context_binding);
                    if (outcome.status != RequestStatus::complete) {
                        notify_("SYNTH dynamic profile refresh failed: " + outcome.detail);
                    }
                } catch (const std::exception& error) {
                    notify_(std::string{"SYNTH dynamic profile refresh: "} + error.what());
                }
            });
        return accepted == tasks::SubmitResult::accepted;
    }

    [[nodiscard]] bool initialization_failed() const noexcept {
        return initialization_failed_.load(std::memory_order_acquire);
    }

    // Set once by a successful initialization on the worker; the quest delivery gate reads it.
    [[nodiscard]] bool quest_events_registered() const noexcept {
        return quest_event_ready_.load(std::memory_order_acquire);
    }

    // Retirement revokes this binding's write epoch before any later ACK can land.
    void stop_save_writer() {
        if (save_context_) save_context_->stop_writer(save_binding_.epoch);
    }

    // A bound save requires an ACK as part of a checkpoint; without one, publication alone suffices.
    [[nodiscard]] bool save_bound() const noexcept { return save_context_ != nullptr; }

    // Background world publications, including the social checkpoint, track through this one counter.
    [[nodiscard]] std::size_t observation_tasks() const noexcept {
        return world_context_tasks_.load(std::memory_order_acquire);
    }
    template <class F>
        requires std::constructible_from<tasks::TaskLanes::Task, F&&>
    [[nodiscard]] tasks::TaskLanes::Task track_observation(F&& task) {
        return tasks::TaskLanes::track_activity(world_context_tasks_, std::forward<F>(task));
    }

    [[nodiscard]] std::uint64_t acknowledged_inventory_frame() const noexcept {
        return acknowledged_inventory_frame_.load(std::memory_order_acquire);
    }

    // This gate is also rechecked on the worker, under the serialized context transaction.
    [[nodiscard]] bool world_context_needed(const core::RuntimeSnapshot& snapshot, bool inventory_refresh = false) {
        const bool wait_checkpoint=history_checkpoint_serial_.load(std::memory_order_acquire)>
            history_checkpoint_acknowledged_.load(std::memory_order_acquire);
        if (inventory_refresh && snapshot.player().inventory_observation() == "unavailable") return false;
        const auto observed = context::world_observation(snapshot);
        const auto quests = context::quest_observation(snapshot);
        if (!observed && !quests && !inventory_refresh && !wait_checkpoint) return false;
        std::unique_lock lock{world_context_mutex_, std::try_to_lock};
        if (!lock.owns_lock()) return false;
        if (snapshot.generation().value() == acknowledged_world_generation_ &&
            snapshot.frame() < acknowledged_world_frame_) return false;
        return wait_checkpoint || inventory_refresh || (observed && observed != acknowledged_world_context_) ||
               (quests && quests != acknowledged_quest_context_);
    }

    // Persist observations without activation or model work. The session has already applied its
    // admission gate and this object's world_context_needed(); `turn` is the session's current turn.
    [[nodiscard]] bool observe_world(Snapshot snapshot, bool inventory_refresh, core::CancellationToken turn) {
        const auto accepted = network_.try_submit("network", tasks::TaskClass::background,
            tasks::TaskLanes::Clock::now() + std::chrono::milliseconds{request_timeout_ms()}, std::move(turn),
            tasks::TaskLanes::track_activity(world_context_tasks_,
            [this, snapshot = std::move(snapshot), inventory_refresh](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire) ||
                    runtime_paused_.load(std::memory_order_acquire)) return;
                try {
                    const auto context_turn = client_.begin_context_turn();
                    // A normal request may have acknowledged a newer scene while this work waited.
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire) ||
                        runtime_paused_.load(std::memory_order_acquire) || !world_context_needed(*snapshot, inventory_refresh)) return;
                    const auto result = publish(snapshot, std::nullopt, cancellation);
                    if (result.status == RequestStatus::complete) return;
                } catch (const std::exception&) {}
                if (!cancellation.is_cancelled() && !terminal_.load(std::memory_order_acquire) &&
                    !world_context_failure_reported_.exchange(true, std::memory_order_acq_rel))
                    notify_("SYNTH world context was not saved; retrying on a later observation");
            }));
        return accepted == tasks::SubmitResult::accepted;
    }

    // Game thread, nonblocking: the acknowledged sequence a tracked-quest change reacts to, if any.
    [[nodiscard]] std::optional<std::uint64_t> quest_reaction_sequence(const core::RuntimeSnapshot& snapshot) {
        const auto journal = context::quest_observation(snapshot);
        std::unique_lock lock{world_context_mutex_, std::try_to_lock};
        if (!lock.owns_lock() || !journal || !acknowledged_quest_context_ ||
            acknowledged_quest_player_ != actor_key(identity(snapshot.player())) ||
            !context::tracked_quest_changed(*acknowledged_quest_context_, *journal)) return std::nullopt;
        return acknowledged_quest_sequence_;
    }

    // Worker, inside the context transaction: the reacted-to quest frame is still the latest ACK.
    [[nodiscard]] bool quest_sequence_current(std::uint64_t previous_sequence) {
        std::scoped_lock lock{world_context_mutex_};
        return acknowledged_quest_sequence_ == previous_sequence;
    }

    // An accepted flat dialogue response line, noted on receipt before presentation, raises the
    // history frontier a later save-ACKed publication must cover. It is not a heard receipt.
    void note_dialogue_history(core::RuntimeVariant variant, const core::CancellationToken& cancellation) {
        if(save_context_ && variant==core::RuntimeVariant::flat &&
            !cancellation.is_cancelled() && !terminal_.load(std::memory_order_acquire))
            history_checkpoint_serial_.fetch_add(1,std::memory_order_acq_rel);
    }

    // The result is newer than its issuing snapshot. Reuse ordinary
    // world observation to acknowledge a frontier after the consequence.
    void note_wait_outcome(const core::CancellationToken& cancellation) {
        if(!save_context_ || cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
        const auto serial=history_checkpoint_serial_.fetch_add(1,std::memory_order_acq_rel)+1;
        wait_checkpoint_notice_.store(serial,std::memory_order_release);
    }

    // Game pump: true exactly once after a saved ACK covers the latest wait outcome.
    [[nodiscard]] bool take_wait_checkpoint_notice(std::uint64_t generation) {
        auto notice=wait_checkpoint_notice_.load(std::memory_order_acquire);
        return notice && !terminal_.load(std::memory_order_acquire) && generation==session_.generation().value() &&
            history_checkpoint_acknowledged_.load(std::memory_order_acquire)>=notice &&
            wait_checkpoint_notice_.compare_exchange_strong(notice,0,std::memory_order_acq_rel);
    }

    // Worker only. Observation and session request callers hold begin_context_turn(); the manual
    // submit task calls this without it.
    [[nodiscard]] RequestOutcome publish(const Snapshot& owned_snapshot,
                                         const std::optional<protocol_native::Identity>& target, core::CancellationToken cancellation,
                                         std::vector<protocol_native::Identity>* published_audience = nullptr,
                                         const std::optional<protocol_native::NativeQuestBatch>& quest_events = std::nullopt,
                                         const std::optional<protocol_native::NativePlayerEvent>& player_event = std::nullopt,
                                         const std::shared_ptr<const ActorEventDelivery::Prepared>& actor_events = {},
                                         const protocol_native::ControlSelection& controls = {},
                                         const runtime::SocialTaskDescriptor* social_pair = nullptr) {
        const auto& snapshot = *owned_snapshot;
        const auto wait_checkpoint=history_checkpoint_serial_.load(std::memory_order_acquire);
        if (actor_events && (actor_events->capture->scene!=owned_snapshot || target || quest_events || player_event ||
            actor_events->actors.empty() || actor_events->actors.size()>ActorEventDelivery::actor_capacity))
            throw std::invalid_argument{"actor event context does not own this fragment"};
        if(social_pair&&(!client_.social_tasks_ready()||actor_events||quest_events||player_event||!target||
            !social_wire_actor_matches(*target,social_pair->performer)))throw std::invalid_argument{"invalid saved visit context"};

        std::vector<protocol_native::Identity> audience;
        std::vector<protocol_native::ActorState> actor_states;
        audience.reserve(std::min<std::size_t>(snapshot.actors().size(), 16));
        actor_states.reserve(std::min<std::size_t>(snapshot.actors().size() + 1, 16));
        auto hearing_distance = snapshot.world() && snapshot.world()->interior
            ? configuration_.activation.interior_hearing_distance
            : configuration_.activation.exterior_hearing_distance;
        hearing_distance = static_cast<std::uint32_t>(targeting::conversation_range(hearing_distance, controls.mode));
        const auto selected_actor_key = target ? actor_key(*target) : std::string{};
        const auto make_state = [&](const core::ActorSnapshot& actor) {
            if (!selected_actor_key.empty() && actor_key(identity(actor)) == selected_actor_key)
                voice_samples_.request(actor.voice_type(), actor.name());
            std::vector<protocol_native::InventoryItem> inventory;
            inventory.reserve(actor.inventory().size());
            for (const auto& item : actor.inventory()) {
                char form_id[11]{};
                std::snprintf(form_id, sizeof(form_id), "0x%08X", item.form_id);
                inventory.push_back(protocol_native::InventoryItem{
                    form_id, item.origin_plugin, item.display_name, item.count, item.form_type,
                    item.value, item.weight, item.equipped});
            }
            std::vector<protocol_native::FactionMembership> factions;
            factions.reserve(actor.factions().size());
            for (const auto& faction : actor.factions()) {
                char form_id[11]{};
                std::snprintf(form_id, sizeof(form_id), "0x%08X", faction.form_id);
                factions.push_back(protocol_native::FactionMembership{
                    form_id, faction.origin_plugin, faction.display_name, faction.editor_id,
                    faction.rank});
            }
            auto state = protocol_native::ActorState{
                .actor = identity(actor),
                .base_form_id = [&] {
                    char form_id[11]{};
                    std::snprintf(form_id, sizeof(form_id), "0x%08X", actor.base_form_id());
                    return std::string{form_id};
                }(),
                .base_origin_plugin = actor.base_origin_plugin(),
                .x = actor.position().x,
                .y = actor.position().y,
                .z = actor.position().z,
                .distance = targeting::distance(snapshot.player_pose().position(), actor.position()),
                .alive = actor.alive(),
                .disabled = actor.disabled(),
                .in_combat = actor.in_combat(),
                .hostile_to_player = actor.hostile_to_player(),
                .sneaking = actor.sneaking(),
                .level = static_cast<std::uint64_t>(std::max<std::int16_t>(0, actor.level())),
                .race = actor.race(),
                .sex = actor.sex(),
                .voice_type = actor.voice_type(),
                .teammate = actor.teammate(),
                .health_percent = actor.health_percent(),
                .action_points_percent = actor.action_points_percent(),
                .inventory = std::move(inventory),
                .line_of_sight = actor.line_of_sight()
                                     ? (*actor.line_of_sight() ? "visible" : "blocked")
                                     : "unknown",
                .factions = std::move(factions),
                .faction_observation = actor.faction_observation(),
                .life_state = actor.life_state(),
                .posture = actor.posture(),
                .weapon_drawn = actor.weapon_drawn(),
                .movement_speed = actor.movement_speed(),
                .sprinting = actor.sprinting(),
                .talking_to_player = actor.talking_to_player(),
                .in_power_armor = actor.in_power_armor(),
                .current_package = actor.current_package()
                    ? protocol_native::PackageState{
                          true,
                          [&] {
                              char form_id[11]{};
                              std::snprintf(form_id, sizeof(form_id), "0x%08X",
                                            actor.current_package()->form_id);
                              return std::string{form_id};
                          }(),
                          actor.current_package()->origin_plugin,
                          actor.current_package()->display_name,
                          actor.current_package()->editor_id}
                    : protocol_native::PackageState{},
                .inventory_observation = actor.inventory_observation(),
                .faction_completeness = actor.faction_completeness(),
                .health_percent_available = actor.health_percent_available(),
                .action_points_percent_available = actor.action_points_percent_available(),
                .combat_target = protocol_native::CombatTargetState{
                    actor.combat_target().observation,
                    actor.combat_target().actor ? std::optional{protocol_native::Identity{
                        [&] {
                            char form_id[11]{};
                            std::snprintf(form_id, sizeof(form_id), "0x%08X", actor.combat_target().actor->form_id);
                            return std::string{form_id};
                        }(), actor.combat_target().actor->origin_plugin, actor.combat_target().actor->playthrough_id,
                        actor.combat_target().actor->display_name}} : std::nullopt,
                    actor.combat_target().actor ? actor.combat_target().actor->race : std::string{}},
            };
            if (actor.conversation_held()) state.conversation_held=true;
            if (snapshot.variant()==core::RuntimeVariant::flat && actor.wait_held().has_value()) state.wait_held=actor.wait_held();
            append_actor_combat_observations(state, actor, client_.combat_contacts_ready(), client_.enemy_awareness_ready());
            return state;
        };
        actor_states.push_back(make_state(snapshot.player()));
        const std::function<bool(const core::ActorSnapshot&)> selected = target
            ? std::function<bool(const core::ActorSnapshot&)>{[&](const auto& actor) {
                  return actor_key(identity(actor)) == selected_actor_key;
              }} : std::function<bool(const core::ActorSnapshot&)>{};
        if (actor_events) {
            // This audience carries exact source identities, not a new hearing/visibility or witness claim.
            for (const auto* resolved:actor_events->resolve_actor_states()) {
                if (resolved==&snapshot.player()) continue;
                audience.push_back(identity(*resolved));actor_states.push_back(make_state(*resolved));
            }
        } else if(social_pair) {
            // Only a saved task's private progress scene expands past player hearing.
            // Ordinary input witnesses keep their existing hearing selection unchanged.
            for(const auto& required:{social_pair->performer,social_pair->recipient}) {
                const core::ActorSnapshot* found{};
                for(const auto& actor:snapshot.actors())if(social_wire_actor_matches(identity(actor),required)) {
                    if(found)throw std::invalid_argument{"ambiguous saved visit actor"};found=&actor;
                }
                if(!found||!found->alive()||found->disabled()||!found->conversation_area_available()||
                    targeting::distance(snapshot.player_pose().position(),found->position())>2000)
                    throw std::invalid_argument{"saved visit actor unavailable"};
                audience.push_back(identity(*found));actor_states.push_back(make_state(*found));
            }
        } else {
            for (const auto* actor : targeting::context_participants(snapshot, hearing_distance, selected)) {
                if (controls.mode == "NARRATOR" || controls.mode == "INJECTION_LOG") continue;
                if (!controls.mode.empty() && !selected_actor_key.empty() && actor_key(identity(*actor)) != selected_actor_key &&
                    (!actor->conversation_area_available() || !targeting::player_speech_audible(
                        hearing_distance, targeting::distance(snapshot.player_pose().position(), actor->position())))) continue;
                if ((controls.mode == "WHISPER" || controls.mode == "CLOSE") &&
                    (actor_key(identity(*actor)) != selected_actor_key ||
                     targeting::distance(snapshot.player_pose().position(), actor->position()) > 200)) continue;
                audience.push_back(identity(*actor));
                if (actor_states.size() < 16) actor_states.push_back(make_state(*actor));
            }
        }
        std::vector<protocol_native::QuestState> active_quests;
        active_quests.reserve(snapshot.active_quests().size());
        for (const auto& quest : snapshot.active_quests()) {
            char form_id[11]{};
            std::snprintf(form_id, sizeof(form_id), "0x%08X", quest.form_id);
            active_quests.push_back(protocol_native::QuestState{
                form_id, quest.origin_plugin, quest.display_name, quest.editor_id,
                quest.current_stage, quest.active_objectives, quest.objectives, quest.objectives_observation, quest.tracked});
        }
        const auto form_id = [](std::uint32_t value) {
            char encoded[11]{};
            std::snprintf(encoded, sizeof(encoded), "0x%08X", value);
            return std::string{encoded};
        };
        std::vector<protocol_native::NearbyItemState> nearby_items;
        nearby_items.reserve(snapshot.nearby_items().size());
        for (const auto& item : snapshot.nearby_items()) {
            nearby_items.push_back(protocol_native::NearbyItemState{
                form_id(item.reference_id), form_id(item.base_form_id), form_id(item.cell_form_id),
                item.reference_origin_plugin, item.base_origin_plugin, item.cell_origin_plugin,
                item.display_name, item.position.x, item.position.y, item.position.z,
                item.distance, item.weight, item.count, item.form_type, item.value,
                item.stealing, item.looking_at, item.held});
        }
        std::vector<protocol_native::PointOfInterestState> points_of_interest;
        points_of_interest.reserve(snapshot.points_of_interest().size());
        for (const auto& poi : snapshot.points_of_interest()) {
            points_of_interest.push_back(protocol_native::PointOfInterestState{
                form_id(poi.reference_id), form_id(poi.base_form_id), form_id(poi.cell_form_id),
                poi.reference_origin_plugin, poi.base_origin_plugin, poi.cell_origin_plugin,
                poi.display_name, poi.kind, poi.position.x, poi.position.y, poi.position.z,
                poi.distance, poi.locked, poi.looking_at});
        }
        auto outcome = client_.publish_context(0, snapshot.player().playthrough_id(),
                                       identity(snapshot.player()),
                                       target,
                                       audience, actor_states,
                                       snapshot.world()
                                           ? std::optional{protocol_native::WorldState{
                                                 snapshot.world()->location, snapshot.world()->cell,
                                                 snapshot.world()->worldspace, snapshot.world()->weather,
                                                 snapshot.world()->interior,
                                                 snapshot.world()->game_time_ticks, snapshot.world()->scene,
                                                 snapshot.world()->location_ref}}
                                           : std::nullopt,
                                       active_quests, nearby_items, points_of_interest,
                                       {}, cancellation, save_binding_.loaded_context, snapshot.active_quests_observation(),
                                       snapshot.nearby_items_observation(), snapshot.points_of_interest_observation(),
                                       protocol_native::retained_collection_observation(
                                           snapshot.actors_observation(), snapshot.actors().size(), audience.size()), quest_events, player_event,
                                       actor_events ? std::optional{actor_events->packet(core::SnapshotClock::now())} : std::nullopt);
        if (outcome.status == RequestStatus::complete && !cancellation.is_cancelled() &&
            !terminal_.load(std::memory_order_acquire)) {
            if (quest_events_enabled_) quest_events_.acknowledge_journal(owned_snapshot, outcome.context_binding.sequence);
            std::scoped_lock lock{world_context_mutex_};
            if (snapshot.generation().value() != acknowledged_world_generation_ ||
                snapshot.frame() >= acknowledged_world_frame_) {
                acknowledged_world_context_ = context::world_observation(snapshot);
                if (auto quests = context::quest_observation(snapshot)) {
                    acknowledged_quest_context_ = std::move(quests);
                    acknowledged_quest_sequence_ = outcome.context_binding.sequence;
                    acknowledged_quest_player_ = actor_key(identity(snapshot.player()));
                }
                acknowledged_world_frame_ = snapshot.frame();
                acknowledged_world_generation_ = snapshot.generation().value();
                world_context_failure_reported_.store(false, std::memory_order_release);
            }
        }
        if (save_context_ && outcome.status == RequestStatus::complete && !cancellation.is_cancelled()
            && !terminal_.load(std::memory_order_acquire)) {
            outcome.save_checkpoint_acknowledged = save_context_->acknowledge(save_binding_.epoch,
                {snapshot.player().playthrough_id(), client_.runtime_session_id(),
                 outcome.context_binding.generation, outcome.context_binding.sequence});
        }
        if(wait_checkpoint && save_context_ && outcome.save_checkpoint_acknowledged &&
            outcome.status==RequestStatus::complete && !cancellation.is_cancelled() &&
            !terminal_.load(std::memory_order_acquire) && snapshot.generation()==session_.generation()) {
            auto before=history_checkpoint_acknowledged_.load(std::memory_order_acquire);
            while(before<wait_checkpoint && !history_checkpoint_acknowledged_.compare_exchange_weak(
                before,wait_checkpoint,std::memory_order_release,std::memory_order_acquire)) {}
        }
        // Acknowledgment belongs to copied values and the saved frontier, never merely to queue admission.
        if (outcome.status == RequestStatus::complete && !cancellation.is_cancelled() &&
            !terminal_.load(std::memory_order_acquire) &&
            snapshot.generation() == session_.generation() &&
            snapshot.player().inventory_observation() != "unavailable" &&
            (!save_context_ || outcome.save_checkpoint_acknowledged)) {
            auto previous = acknowledged_inventory_frame_.load(std::memory_order_acquire);
            while (snapshot.frame() > previous && !acknowledged_inventory_frame_.compare_exchange_weak(
                previous, snapshot.frame(), std::memory_order_release, std::memory_order_acquire)) {}
        }
        if (published_audience && outcome.status == RequestStatus::complete) *published_audience = std::move(audience);
        const auto now=core::SnapshotClock::now();
        if(client_.social_tasks_ready()&&outcome.status==RequestStatus::complete&&!cancellation.is_cancelled()&&
            !terminal_.load(std::memory_order_acquire)&&snapshot.generation()==session_.generation()&&
            now>=snapshot.captured_at()&&now-snapshot.captured_at()<=std::chrono::seconds{2})
            hydrate_social_(outcome, snapshot);
        outcome.context_binding.invalidate_response = [this, cancellation] {
            invalidate_response_(cancellation);
        };
        return outcome;
    }

private:
    [[nodiscard]] static protocol_native::Identity identity(const core::ActorSnapshot& actor) {
        return PlayerDialogue::identity(actor);
    }

    [[nodiscard]] static std::string actor_key(const protocol_native::Identity& actor) {
        return PlayerDialogue::actor_key(actor);
    }

    [[nodiscard]] std::uint64_t request_timeout_ms() const noexcept {
        return static_cast<std::uint64_t>(configuration_.behavior.ai_response_timeout_seconds) * 1000ULL;
    }

    [[nodiscard]] bool submit(Snapshot snapshot, bool initialize_first) {
        const auto target = select_target_(*snapshot);
        const auto deadline = tasks::TaskLanes::Clock::now() +
                              std::chrono::milliseconds{request_timeout_ms() * 2ULL};
        const auto result = network_.try_submit(
            "network", tasks::TaskClass::current_turn, deadline, session_,
            requests_.track_response(
            [this, snapshot = std::move(snapshot), initialize_first, target](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled()) {
                    return;
                }

                try {
                    if (initialize_first) {
                        const auto hmd_fresh = snapshot->hmd_pose_capability(core::SnapshotClock::now()) ==
                                               core::PoseCapability::fresh;
                        std::vector<protocol_native::LoadedPlugin> plugins;
                        plugins.reserve(snapshot->loaded_plugins().size());
                        for (const auto& plugin : snapshot->loaded_plugins()) {
                            plugins.push_back(protocol_native::LoadedPlugin{
                                plugin.name, plugin.form_id_prefix, plugin.light,
                                plugin.compile_index, plugin.small_file_compile_index,
                                plugin.partial_index});
                        }
                        const auto outcome = client_.initialize(
                            snapshot->generation().value(), hmd_fresh, plugins);
                        if (outcome.status != RequestStatus::complete) {
                            notify_("SYNTH server initialization failed: " + outcome.detail);
                            initialization_failed_.store(true, std::memory_order_release);
                            return;
                        }
                        quest_event_ready_.store(quest_events_enabled_ && client_.quest_events_ready(), std::memory_order_release);
                        notify_("SYNTH connected");
                        std::vector<std::string> voice_plugins;
                        for (const auto& plugin : snapshot->loaded_plugins()) voice_plugins.push_back(plugin.name);
                        voice_samples_.configure(std::move(voice_plugins));
                        return;
                    }
                    const auto outcome = publish(snapshot, target, cancellation);
                    if (outcome.status == RequestStatus::complete) {
                        notify_("SYNTH context updated");
                    } else {
                        notify_("SYNTH context update failed: " + outcome.detail);
                    }
                } catch (const std::exception& error) {
                    notify_(std::string{"SYNTH session: "} + error.what());
                    if (initialize_first) {
                        initialization_failed_.store(true, std::memory_order_release);
                    }
                }
            }));
        if (result != tasks::SubmitResult::accepted) {
            notify_("SYNTH network queue is full");
            return false;
        }
        return true;
    }

    const config::Config& configuration_;
    SynthClient& client_;
    tasks::TaskLanes& network_;
    DialogueRequests& requests_;
    voice::SampleWorker& voice_samples_;
    QuestEventDelivery& quest_events_;
    const bool quest_events_enabled_;
    const std::atomic_bool& terminal_;
    const std::atomic_bool& runtime_paused_;
    const core::CancellationToken session_;
    core::SaveContextStore* const save_context_;
    const core::SaveContextStore::Binding save_binding_;
    SelectTarget select_target_;
    HydrateSocial hydrate_social_;
    InvalidateResponse invalidate_response_;
    Notify notify_;
    std::atomic_bool initialized_{};
    std::atomic_bool initialization_failed_{};
    std::atomic_bool quest_event_ready_{};
    std::atomic_size_t world_context_tasks_{};
    std::atomic_uint64_t history_checkpoint_serial_{},history_checkpoint_acknowledged_{},wait_checkpoint_notice_{};
    std::atomic_bool world_context_failure_reported_{};
    std::atomic_uint64_t acknowledged_inventory_frame_{};
    std::mutex world_context_mutex_;
    std::optional<context::WorldObservation> acknowledged_world_context_;
    std::optional<context::QuestObservation> acknowledged_quest_context_;
    std::uint64_t acknowledged_quest_sequence_{};
    std::string acknowledged_quest_player_;
    std::uint64_t acknowledged_world_frame_{};
    std::uint64_t acknowledged_world_generation_{};
};

}  // namespace synth::client
