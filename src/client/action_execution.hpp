#pragma once

#include "actions/equipment.hpp"
#include "client/action_inventory.hpp"
#include "client/attack_action.hpp"
#include "client/context_publication.hpp"
#include "client/player_dialogue.hpp"
#include "client/synth_client.hpp"
#include "client/wait_action.hpp"
#include "config/config.hpp"
#include "core/snapshot.hpp"
#include "runtime/action_completion.hpp"
#include "runtime/game_thread_dispatcher.hpp"
#include "tasks/task_lanes.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace synth::client {

// Owns one session's server-requested actions: the bounded pending queue, the Wait/attack replay
// guard, the Wait lifetime, the active-action count read by every idle and admission gate, and the
// execute workflow with its receipt. Native work runs only as a dispatcher task on the game thread
// through the selected IFalloutRuntime; the network worker waits on a shared completion and sends
// exactly one bounded result. Wait outlives turn replacement: clear() drops queued actions but not
// the Wait lifetime, which ends only on stop_wait() (local Stop) or when the session token is
// cancelled at retirement (load, new game, main menu, shutdown, or after a completed halt). Hard
// halt itself only clears the queue: the flat entry releases installed native Wait packages through
// its adapter before calling hard_halt(), and the token is cancelled when that session retires.
//
// The session keeps hold suspension before enqueue, the response binding and its rollback and
// conversation ending, and its social task owner keeps the release policy; this object reaches them
// only through the route_line, end_conversation and release_social callbacks. The queue lock is a leaf: it is never held across
// a callback, a runtime call or HTTP, except that while_idle()/try_while_idle() run the caller's
// rechat check under it. Lock order is therefore action queue, then rechat.
//
// Lifetimes: the borrowed configuration, client, network lane, dispatcher, dialogue, context
// publication and terminal flag outlive this object; it is declared after all of them. Network
// tasks queued here capture this object and copied snapshots/actions/bindings only; the session
// joins the network lane in its destructor body before any member is destroyed, and a rejected,
// expired or discarded task is destroyed unrun with its activity guard released once. Dispatcher
// tasks capture only the shared completion, the copied request, token and deadline, never this
// object, so a late game-thread run after a worker abandons the completion touches no owner state.
class ActionExecution final {
public:
    using Snapshot = std::shared_ptr<const core::RuntimeSnapshot>;
    using Notify = std::function<void(std::string)>;
    // Routes a validated continuation line with the receipt's snapshot and cancellation.
    using RouteLine = std::function<void(const protocol_native::Line&, const Snapshot&, const core::CancellationToken&)>;
    // Ends SYNTH-owned conversation state for the exact actor; runs while the turn is held current.
    using EndConversation = std::function<void(const protocol_native::Identity&)>;
    // Game thread only. Releases a social visit owned by the queued actor; false keeps the queue blocked.
    using ReleaseSocial = std::function<bool(const protocol_native::Identity&, runtime::IFalloutRuntime&)>;

    struct Dependencies final {
        const config::Config& configuration;
        SynthClient& client;
        tasks::TaskLanes& network;
        runtime::GameThreadDispatcher& dispatcher;
        PlayerDialogue& dialogue;
        ContextPublication& context;
        const std::atomic_bool& terminal;
        core::CancellationToken session;
        RouteLine route_line;
        EndConversation end_conversation;
        ReleaseSocial release_social;
        Notify notify;
    };

    explicit ActionExecution(Dependencies dependencies)
        : configuration_{dependencies.configuration},
          client_{dependencies.client},
          network_{dependencies.network},
          dispatcher_{dependencies.dispatcher},
          dialogue_{dependencies.dialogue},
          context_{dependencies.context},
          terminal_{dependencies.terminal},
          wait_lifetime_{std::move(dependencies.session)},
          route_line_{std::move(dependencies.route_line)},
          end_conversation_{std::move(dependencies.end_conversation)},
          release_social_{std::move(dependencies.release_social)},
          notify_{std::move(dependencies.notify)} {
        if (!route_line_ || !end_conversation_ || !release_social_ || !notify_)
            throw std::invalid_argument{"action execution requires line, conversation, social and notification sinks"};
    }

    ActionExecution(const ActionExecution&) = delete;
    ActionExecution& operator=(const ActionExecution&) = delete;

    // Admitted, queued-on-the-network-lane or running action tasks.
    [[nodiscard]] std::size_t active() const noexcept { return active_actions_.load(std::memory_order_acquire); }

    [[nodiscard]] core::CancellationToken wait_lifetime() { return wait_lifetime_.token(); }
    void stop_wait() { wait_lifetime_.stop(); }

    // Turn replacement and hard halt drop queued actions; the replay guard and Wait lifetime remain.
    void clear() {
        std::scoped_lock lock{action_mutex_};
        pending_actions_.clear();
    }

    // Runs `check` under the queue lock only while no action is queued; false when not idle.
    template <class Check>
    [[nodiscard]] bool while_idle(Check&& check) {
        std::scoped_lock lock{action_mutex_};
        return pending_actions_.empty() && std::forward<Check>(check)();
    }

    // Nonblocking variant for frame-rate admission checks; a contended queue reads as busy.
    template <class Check>
    [[nodiscard]] bool try_while_idle(Check&& check) {
        std::unique_lock lock{action_mutex_, std::try_to_lock};
        return lock.owns_lock() && pending_actions_.empty() && std::forward<Check>(check)();
    }

    // Freeze the start window on receipt. The caller suspends holds and filters social-owned lines.
    void enqueue(const protocol_native::Action& action, Snapshot snapshot, ContextBinding binding) {
        // Human menu selection has its own bounded window, independent of the HTTP timeout.
        const auto action_budget=action.name=="open_inventory" && action.capability=="action.open_inventory" && client_.gift_menu_ready()
            ? std::uint64_t{120000} : request_timeout_ms();
        const auto action_deadline = runtime::action_start_deadline(action.deadline_ms, action_budget);
        std::scoped_lock lock{action_mutex_};
        if (pending_actions_.size() >= 32) {
            notify_("SYNTH action queue is full");
            return;
        }
        if ((action.name == "wait_here" || action.name == "release_wait" || action.name == "attack") && !wait_replays_.claim(action)) {
            notify_("SYNTH action was repeated or its session limit was reached");
            return;
        }
        pending_actions_.push_back(PendingAction{action, std::move(snapshot), std::move(binding),
            action_deadline, ++next_serial_});
    }

    enum class Pump : unsigned char { idle, blocked, dispatched };

    // Game thread, after the session's speech/response/activity idle gate. The front actor is copied
    // and the lock released before the social release runs native work; dispatch then requires the
    // same front entry, so a replaced or newly arrived front waits for its own release next frame.
    [[nodiscard]] Pump pump(runtime::IFalloutRuntime& runtime) {
        std::optional<std::uint64_t> front;
        std::optional<protocol_native::Identity> actor;
        {
            std::scoped_lock lock{action_mutex_};
            if (!pending_actions_.empty()) {
                front = pending_actions_.front().serial;
                actor = pending_actions_.front().action.actor;
            }
        }
        if (actor && !release_social_(*actor, runtime)) return Pump::blocked;
        if (!runtime.conversation_actions_ready()) return Pump::blocked;
        std::optional<PendingAction> pending;
        {
            std::scoped_lock lock{action_mutex_};
            if (!pending_actions_.empty()) {
                if (!front || pending_actions_.front().serial != *front) return Pump::blocked;
                pending.emplace(std::move(pending_actions_.front()));
                pending_actions_.pop_front();
            }
        }
        if (!pending) return Pump::idle;
        execute_action(std::move(*pending));
        return Pump::dispatched;
    }

private:
    struct PendingAction final {
        protocol_native::Action action;
        Snapshot snapshot;
        ContextBinding binding;
        tasks::TaskLanes::Deadline deadline;
        std::uint64_t serial{};
    };

    using RuntimeActionCompletion = runtime::ActionCompletion;

    void execute_action(PendingAction pending) {
        const auto turn = pending.binding.cancellation;
        if (turn.is_cancelled()) return;
        const auto deadline = pending.deadline;
        // Expired actions still need a bounded terminal receipt; this budget never authorizes execution.
        const auto delivery_deadline = tasks::TaskLanes::Clock::now() +
                                       std::chrono::milliseconds{request_timeout_ms()};
        const auto accepted = network_.try_submit(
            "network", tasks::TaskClass::current_turn, delivery_deadline, turn,
            tasks::TaskLanes::track_activity(active_actions_,
            [this, pending = std::move(pending), deadline](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                const auto& action = pending.action;
                const auto& snapshot = pending.snapshot;
                std::string status{"unsupported_runtime"};
                std::string detail{"action capability is not implemented by this runtime"};
                std::optional<runtime::RuntimeInventoryObservation> inventory_observation;
                std::optional<runtime::RuntimeTransferObservation> transfer_observation;
                if (tasks::TaskLanes::Clock::now() >= deadline) {
                    status = "timed_out";
                    detail = "action expired while awaiting presentation or worker dispatch";
                } else if ((action.name == "equip_item" || action.name == "unequip_item") &&
                    action.capability == "action." + action.name && !client_.equipment_actions_ready()) {
                    status = "rejected";
                    detail = "server has not acknowledged equipment inventory observations; no action was executed";
                } else if (action.name == "consume" && action.capability == "action.consume" && !client_.consumption_ready()) {
                    status = "rejected";
                    detail = "server has not acknowledged consumption inventory observations; no action was executed";
                } else if (action.name == "give_item_to" && action.capability == "action.give_item_to" && !client_.transfer_ready()) {
                    status = "rejected";
                    detail = "server has not acknowledged paired transfer inventories; no action was executed";
                } else if (action.name == "give_caps_to" && action.capability == "action.give_caps_to" && !client_.caps_ready()) {
                    status = "rejected";
                    detail = "server has not acknowledged caps inventory observations; no action was executed";
                } else if (action.name == "take_caps_from_player" && action.capability == "action.take_caps_from_player" && !client_.player_caps_ready()) {
                    status = "rejected";
                    detail = "server has not acknowledged player payment inventories; no action was executed";
                } else if (action.name == "take_item_from_player" && action.capability == "action.take_item_from_player" && !client_.player_item_ready()) {
                    status = "rejected";
                    detail = "server has not acknowledged player item inventories; no action was executed";
                } else if (action.name=="open_inventory" && action.capability=="action.open_inventory" && !client_.gift_menu_ready()) {
                    status="rejected";detail="server has not acknowledged gift menu inventory observations";
                } else if (action.name == "end_conversation" &&
                    action.capability == "action.end_conversation") {
                    const auto matches = [&](const core::ActorSnapshot& actor) {
                        const auto actor_identity = identity(actor);
                        return actor_identity.form_id == action.actor.form_id &&
                               actor.origin_plugin() == action.actor.origin_plugin &&
                               actor.playthrough_id() == action.actor.playthrough_id;
                    };
                    auto present = matches(snapshot->player());
                    for (const auto& actor : snapshot->actors()) {
                        if (!present && matches(actor)) present = true;
                    }
                    if (!present) {
                        status = "unavailable";
                        detail = "canonical actor is no longer present in the captured frame";
                    } else {
                        bool expired{};
                        if (!dialogue_.while_turn_current(cancellation, [&] {
                            expired = tasks::TaskLanes::Clock::now() >= deadline;
                            if (!expired) end_conversation_(action.actor);
                        })) return;
                        if (expired) {
                            status = "timed_out";
                            detail = "action expired before conversation control";
                        } else {
                            status = "succeeded";
                            detail = "SYNTH conversation ended and reactivation cooldown applied";
                        }
                    }
                } else if ((action.name == "sheathe_weapon" || action.name == "equip_item" || action.name == "unequip_item" || action.name == "consume" || action.name == "give_item_to" || action.name == "give_caps_to" || action.name == "take_caps_from_player" || action.name == "take_item_from_player" || action.name == "open_inventory" || action.name == "wait_here" || action.name == "release_wait" || action.name == "attack") &&
                    action.capability == "action." + action.name) {
                    std::uint32_t form_id{};
                    if (action.actor.form_id.size() != 10 ||
                        !action.actor.form_id.starts_with("0x")) {
                        status = "rejected";
                        detail = "action actor has an invalid canonical form ID";
                    } else {
                        const auto first = action.actor.form_id.data() + 2;
                        const auto last = action.actor.form_id.data() + action.actor.form_id.size();
                        const auto parsed = std::from_chars(first, last, form_id, 16);
                        if (parsed.ec != std::errc{} || parsed.ptr != last || form_id == 0) {
                            status = "rejected";
                            detail = "action actor has an invalid canonical form ID";
                        } else {
                            runtime::RuntimeActionRequest request{
                                runtime::RuntimeActionName::sheathe_weapon,
                                {form_id, action.actor.origin_plugin, action.actor.playthrough_id}};
                            bool request_valid=true;
                            if (action.name=="attack") {
                                try { request=prepare_attack_action(*snapshot,action,cancellation,deadline); }
                                catch (const std::invalid_argument& error) { request_valid=false;status="rejected";detail=error.what(); }
                            } else if (action.name == "wait_here" || action.name == "release_wait") {
                                try { request = prepare_wait_action(*snapshot, action, cancellation, wait_lifetime_.token(), deadline); }
                                catch (const std::invalid_argument& error) {
                                    request_valid = false; status = "rejected"; detail = error.what();
                                }
                            } else if (action.name=="open_inventory") {
                                try {request=prepare_gift_menu_action(*snapshot,action,cancellation,deadline);}
                                catch(const std::invalid_argument& error){request_valid=false;status="rejected";detail=error.what();}
                            } else if (action.name == "give_item_to" || action.name == "give_caps_to" || action.name == "take_caps_from_player" || action.name == "take_item_from_player" || action.name == "open_inventory") {
                                try { request=prepare_transfer_action(*snapshot,action,cancellation,deadline); }
                                catch (const std::invalid_argument& error) {
                                    request_valid=false;status="rejected";detail=error.what();
                                }
                            } else if (action.name != "sheathe_weapon") {
                                request.name = action.name == "consume" ? runtime::RuntimeActionName::consume :
                                               action.name == "equip_item" ? runtime::RuntimeActionName::equip_item
                                                                           : runtime::RuntimeActionName::unequip_item;
                                request.cancellation = cancellation;
                                request.deadline = deadline;
                                request.action_id = action.action_id;
                                const auto* item = action.arguments.find("item");
                                if (item && item->is_string() && action.arguments.as_object().size() == 1 &&
                                    (!action.target || actor_key(*action.target) == actor_key(action.actor))) {
                                    for (const auto& actor : snapshot->actors()) {
                                        if (actor_key(identity(actor)) != actor_key(action.actor)) continue;
                                        request.item = actions::select_equipment_item(actor.inventory(), actor.inventory_observation(), item->as_string());
                                        break;
                                    }
                                }
                            }
                            if (request_valid) {
                              bool action_pending{};
                              do {
                                action_pending=false;
                                auto completion = std::make_shared<RuntimeActionCompletion>();
                                const auto dispatch = dispatcher_.try_enqueue(
                                    snapshot->generation(),
                                    [completion, request, cancellation, deadline](
                                        runtime::IFalloutRuntime& fallout) {
                                        completion->execute([&] { return fallout.execute_action(request); },
                                                            cancellation, deadline);
                                    });
                                if (dispatch == runtime::DispatchResult::stale) {
                                    status = "cancelled";
                                    detail = "runtime generation changed before action dispatch";
                                } else if (dispatch == runtime::DispatchResult::saturated) {
                                    status = "failed";
                                    detail = "game-thread action queue is full";
                                } else if (dispatch == runtime::DispatchResult::stopped) {
                                    status = "cancelled";
                                    detail = "game-thread action dispatcher is stopped";
                                } else {
                                    std::unique_lock lock{completion->mutex};
                                    while (completion->phase !=
                                           RuntimeActionCompletion::Phase::complete) {
                                        if (cancellation.is_cancelled() ||
                                            terminal_.load(std::memory_order_acquire)) {
                                            if (completion->phase ==
                                                RuntimeActionCompletion::Phase::pending) {
                                                completion->phase =
                                                    RuntimeActionCompletion::Phase::abandoned;
                                            }
                                            return;
                                        }
                                        if (tasks::TaskLanes::Clock::now() >= deadline &&
                                            completion->phase == RuntimeActionCompletion::Phase::pending) {
                                            completion->phase =
                                                RuntimeActionCompletion::Phase::abandoned;
                                            status = "timed_out";
                                            detail = "action expired before game-thread dispatch";
                                            break;
                                        }
                                        completion->changed.wait_for(
                                            lock, std::chrono::milliseconds{10});
                                    }
                                    if (completion->result) {
                                        action_pending=completion->result->pending;
                                        if (completion->result->inventory && completion->result->inventory->valid_for(request))
                                            inventory_observation=completion->result->inventory;
                                        if (completion->result->transfer && completion->result->transfer->valid_for(request))
                                            transfer_observation=completion->result->transfer;
                                        detail = completion->result->detail;
                                        switch (completion->result->status) {
                                        case runtime::RuntimeActionStatus::succeeded:
                                            status = "succeeded";
                                            break;
                                        case runtime::RuntimeActionStatus::rejected:
                                            status = "rejected";
                                            break;
                                        case runtime::RuntimeActionStatus::unavailable:
                                            status = "unavailable";
                                            break;
                                        case runtime::RuntimeActionStatus::failed:
                                            status = "failed";
                                            break;
                                        case runtime::RuntimeActionStatus::unsupported_runtime:
                                            status = "unsupported_runtime";
                                            break;
                                        case runtime::RuntimeActionStatus::timed_out:
                                            status = "timed_out";
                                            break;
                                        }
                                        if(!action_pending && status=="succeeded" &&
                                            (action.name=="wait_here"||action.name=="release_wait")) {
                                            try {
                                                if(!completion->result->wait)throw std::invalid_argument{"wait action lacks a physical receipt"};
                                                detail=verified_wait_result(action,request,*completion->result->wait);
                                            } catch(const std::invalid_argument& error) {status="failed";detail=error.what();}
                                        }
                                    }
                                }
                                if (action_pending) {
                                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                                    if (tasks::TaskLanes::Clock::now()>=deadline) {
                                        action_pending=false;status="timed_out";detail="native action was queued but its physical postcondition was not confirmed";
                                    } else std::this_thread::sleep_for(std::chrono::milliseconds{25});
                                }
                              } while (action_pending);
                            }
                        }
                    }
                } else if (action.name == "inspect_actor" &&
                           action.capability == "action.inspect_actor") {
                    const core::ActorSnapshot* found{};
                    const auto matches = [&](const core::ActorSnapshot& actor) {
                        const auto actor_identity = identity(actor);
                        return actor_identity.form_id == action.actor.form_id &&
                               actor.origin_plugin() == action.actor.origin_plugin &&
                               actor.playthrough_id() == action.actor.playthrough_id;
                    };
                    if (matches(snapshot->player())) found = &snapshot->player();
                    for (const auto& actor : snapshot->actors()) {
                        if (found == nullptr && matches(actor)) found = &actor;
                    }
                    if (found == nullptr) {
                        status = "unavailable";
                        detail = "canonical actor is no longer present in the captured frame";
                    } else {
                        status = "succeeded";
                        json::Value::Object position{{"x", found->position().x},
                                                     {"y", found->position().y},
                                                     {"z", found->position().z}};
                        json::Value::Object inspection{
                            {"display_name", found->name()}, {"form_id", action.actor.form_id},
                            {"origin_plugin", found->origin_plugin()},
                            {"playthrough_id", found->playthrough_id()},
                            {"position", json::Value{std::move(position)}}};
                        inspection.emplace_back("alive", found->alive());
                        inspection.emplace_back("disabled", found->disabled());
                        inspection.emplace_back("in_combat", found->in_combat());
                        inspection.emplace_back("hostile_to_player", found->hostile_to_player());
                        inspection.emplace_back("sneaking", found->sneaking());
                        inspection.emplace_back("level", static_cast<std::int64_t>(found->level()));
                        inspection.emplace_back("race", found->race());
                        inspection.emplace_back("sex", found->sex());
                        inspection.emplace_back("voice_type", found->voice_type());
                        inspection.emplace_back("teammate", found->teammate());
                        inspection.emplace_back("health_percent", found->health_percent_available() ? json::Value{found->health_percent()} : json::Value{});
                        inspection.emplace_back("action_points_percent", found->action_points_percent_available() ? json::Value{found->action_points_percent()} : json::Value{});
                        inspection.emplace_back(
                            "line_of_sight",
                            found->line_of_sight()
                                ? (*found->line_of_sight() ? "visible" : "blocked")
                                : "unknown");
                        detail = json::write(json::Value{std::move(inspection)});
                    }
                } else if (action.name == "inspect_surroundings" &&
                           action.capability == "action.inspect_surroundings") {
                    detail.clear();
                    json::Value::Array actors;
                    json::Value::Array items;
                    json::Value::Array points_of_interest;
                    const auto encode = [&](const auto& actor_rows, const auto& item_rows,
                                            const auto& poi_rows) {
                        return json::write(json::Value{protocol_native::object({
                            {"schema", "synth.action.inspect_surroundings.result.v1"},
                            {"actors", actor_rows}, {"items", item_rows},
                            {"points_of_interest", poi_rows},
                            {"nearby_items_observation", protocol_native::retained_collection_observation(
                                snapshot->nearby_items_observation(), snapshot->nearby_items().size(), item_rows.size())},
                            {"points_of_interest_observation", protocol_native::retained_collection_observation(
                                snapshot->points_of_interest_observation(), snapshot->points_of_interest().size(), poi_rows.size())},
                            {"truncated", actor_rows.size() < snapshot->actors().size() ||
                                              item_rows.size() < snapshot->nearby_items().size() ||
                                              poi_rows.size() < snapshot->points_of_interest().size()},
                        })});
                    };
                    for (const auto& nearby : snapshot->actors()) {
                        if (actors.size() == 2) break;
                        const auto& position = nearby.position();
                        const auto& player_position = snapshot->player_pose().position();
                        const auto dx = position.x - player_position.x;
                        const auto dy = position.y - player_position.y;
                        const auto dz = position.z - player_position.z;
                        auto candidate = actors;
                        candidate.emplace_back(json::Value{protocol_native::object({
                            {"identity", protocol_native::identity_value(identity(nearby))},
                            {"distance", std::sqrt(dx * dx + dy * dy + dz * dz)},
                            {"in_combat", nearby.in_combat()},
                            {"hostile_to_player", nearby.hostile_to_player()},
                            {"line_of_sight", nearby.line_of_sight()
                                                   ? (*nearby.line_of_sight() ? "visible" : "blocked")
                                                   : "unknown"},
                        })});
                        auto encoded = encode(candidate, items, points_of_interest);
                        if (encoded.size() > 1024) break;
                        actors = std::move(candidate);
                        detail = std::move(encoded);
                    }
                    for (const auto& item : snapshot->nearby_items()) {
                        if (items.size() == 2) break;
                        char reference_id[11]{};
                        char base_form_id[11]{};
                        std::snprintf(reference_id, sizeof(reference_id), "0x%08X", item.reference_id);
                        std::snprintf(base_form_id, sizeof(base_form_id), "0x%08X", item.base_form_id);
                        auto candidate = items;
                        candidate.emplace_back(json::Value{protocol_native::object({
                            {"reference_id", reference_id}, {"base_form_id", base_form_id},
                            {"base_origin_plugin", item.base_origin_plugin},
                            {"display_name", item.display_name}, {"distance", item.distance},
                            {"count", static_cast<std::uint64_t>(item.count)},
                            {"stealing", item.stealing},
                            {"looking_at", item.looking_at}, {"held", item.held},
                        })});
                        auto encoded = encode(actors, candidate, points_of_interest);
                        if (encoded.size() > 1024) break;
                        items = std::move(candidate);
                        detail = std::move(encoded);
                    }
                    for (const auto& poi : snapshot->points_of_interest()) {
                        if (points_of_interest.size() == 2) break;
                        char reference_id[11]{};
                        std::snprintf(reference_id, sizeof(reference_id), "0x%08X", poi.reference_id);
                        auto candidate = points_of_interest;
                        candidate.emplace_back(json::Value{protocol_native::object({
                            {"reference_id", reference_id}, {"display_name", poi.display_name},
                            {"kind", poi.kind}, {"distance", poi.distance},
                            {"locked", poi.locked}, {"looking_at", poi.looking_at},
                        })});
                        auto encoded = encode(actors, items, candidate);
                        if (encoded.size() > 1024) break;
                        points_of_interest = std::move(candidate);
                        detail = std::move(encoded);
                    }
                    status = "succeeded";
                    if (detail.empty()) detail = encode(actors, items, points_of_interest);
                } else if (action.name == "check_inventory" &&
                           action.capability == "action.check_inventory") {
                    const core::ActorSnapshot* found{};
                    const auto matches = [&](const core::ActorSnapshot& actor) {
                        const auto actor_identity = identity(actor);
                        return actor_identity.form_id == action.actor.form_id &&
                               actor.origin_plugin() == action.actor.origin_plugin &&
                               actor.playthrough_id() == action.actor.playthrough_id;
                    };
                    if (matches(snapshot->player())) found = &snapshot->player();
                    for (const auto& actor : snapshot->actors()) {
                        if (found == nullptr && matches(actor)) found = &actor;
                    }
                    if (found == nullptr) {
                        status = "unavailable";
                        detail = "canonical actor is no longer present in the captured frame";
                    } else {
                        json::Value::Array items;
                        for (const auto& item : found->inventory()) {
                            char form_id[11]{};
                            std::snprintf(form_id, sizeof(form_id), "0x%08X", item.form_id);
                            items.emplace_back(json::Value{protocol_native::object({
                                {"form_id", form_id}, {"origin_plugin", item.origin_plugin},
                                {"display_name", item.display_name},
                                {"count", static_cast<std::uint64_t>(item.count)},
                                {"value", static_cast<std::int64_t>(item.value)},
                                {"weight", item.weight},
                                {"form_type", static_cast<std::uint64_t>(item.form_type)},
                                {"equipped", item.equipped},
                            })});
                        }
                        auto result = protocol_native::inventory_action_result(
                            action.actor, std::move(items), found->inventory_observation());
                        status = std::move(result.first);
                        detail = std::move(result.second);
                    }
                } else if (action.name == "read_quests" &&
                           action.capability == "action.read_quests") {
                    json::Value::Array quests;
                    for (const auto& quest : snapshot->active_quests()) {
                        char form_id[11]{};
                        std::snprintf(form_id, sizeof(form_id), "0x%08X", quest.form_id);
                        quests.emplace_back(protocol_native::quest_state_value({
                            form_id, quest.origin_plugin, quest.display_name, quest.editor_id,
                            quest.current_stage, quest.active_objectives, quest.objectives,
                            quest.objectives_observation, quest.tracked}, client_.quest_tracking_ready()));
                    }
                    auto result = protocol_native::quest_action_result(
                        std::move(quests), snapshot->active_quests_observation());
                    status = std::move(result.first);
                    detail = std::move(result.second);
                }
                if (detail.size() > 1024) detail.resize(1024);
                try {
                    auto continuation_snapshot=snapshot;
                    std::optional<protocol_native::ActionInventoryObservation> inventory;
                    std::optional<protocol_native::ActionTransferObservation> transfer;
                    if (client_.action_inventory_ready() &&
                        (action.name=="equip_item" || action.name=="unequip_item" ||
                            (action.name=="consume" && client_.consumption_ready())) && action.capability=="action."+action.name) {
                        auto prepared=prepare_action_inventory(snapshot,action,cancellation,inventory_observation,client_.extended_inventory_ready());
                        continuation_snapshot=std::move(prepared.snapshot);inventory=std::move(prepared.inventory);
                    }
                    if (((client_.transfer_ready() && action.name=="give_item_to") || (client_.caps_ready() && action.name=="give_caps_to") ||
                        (client_.player_caps_ready() && action.name=="take_caps_from_player") ||
                        (client_.player_item_ready() && action.name=="take_item_from_player") ||
                        (client_.gift_menu_ready() && action.name=="open_inventory"))
                        && action.capability=="action."+action.name) {
                        auto prepared=prepare_transfer_inventory(snapshot,action,cancellation,transfer_observation,client_.extended_inventory_ready());
                        continuation_snapshot=std::move(prepared.snapshot);transfer=std::move(prepared.transfer);
                    }
                    const auto outcome = client_.send_action_result(
                        action.action_id, action.idempotency_key, std::move(status),
                        std::move(detail), [this, continuation_snapshot, cancellation](const protocol_native::Line& continuation) {
                            route_line_(continuation, continuation_snapshot, cancellation);
                        }, pending.binding, std::move(inventory), std::move(transfer),
                        action.name=="give_caps_to" || action.name=="take_caps_from_player", action.name=="take_caps_from_player", action.name=="take_item_from_player", action.name=="open_inventory");
                    if (outcome.status != RequestStatus::complete) {
                        notify_("SYNTH action result delivery failed: " + outcome.detail);
                    } else if(action.name=="wait_here"||action.name=="release_wait") {
                        context_.note_wait_outcome(cancellation);
                    }
                } catch (const std::exception& error) {
                    notify_(std::string{"SYNTH action result: "} + error.what());
                    if ((action.name=="give_item_to" || action.name=="give_caps_to" || action.name=="take_caps_from_player" || action.name=="take_item_from_player" || action.name=="open_inventory") && action.capability=="action."+action.name) {
                        // An impossible pair cannot produce a truthful receipt. Retire only its owned chain.
                        (void)dialogue_.cancel_turn_if_owner(cancellation);
                        if (!pending.binding.context_request_id.empty()) {
                            try {
                                const auto stopped=client_.cancel(pending.binding.context_request_id,pending.binding.generation);
                                if (stopped.status!=RequestStatus::complete && stopped.status!=RequestStatus::cancelled)
                                    notify_("SYNTH transfer stopped locally; server cancellation was not acknowledged");
                            } catch (const std::exception&) {
                                notify_("SYNTH transfer stopped locally; server cancellation failed");
                            }
                        }
                    }
                }
            }));
        if (accepted != tasks::SubmitResult::accepted) {
            notify_("SYNTH action dispatch queue is full");
        }
    }

    [[nodiscard]] static protocol_native::Identity identity(const core::ActorSnapshot& actor) {
        return PlayerDialogue::identity(actor);
    }

    [[nodiscard]] static std::string actor_key(const protocol_native::Identity& actor) {
        return PlayerDialogue::actor_key(actor);
    }

    [[nodiscard]] std::uint64_t request_timeout_ms() const noexcept {
        return static_cast<std::uint64_t>(configuration_.behavior.ai_response_timeout_seconds) * 1000ULL;
    }

    const config::Config& configuration_;
    SynthClient& client_;
    tasks::TaskLanes& network_;
    runtime::GameThreadDispatcher& dispatcher_;
    PlayerDialogue& dialogue_;
    ContextPublication& context_;
    const std::atomic_bool& terminal_;
    WaitActionLifetime wait_lifetime_;
    WaitActionReplayGuard wait_replays_;
    RouteLine route_line_;
    EndConversation end_conversation_;
    ReleaseSocial release_social_;
    Notify notify_;
    std::atomic_size_t active_actions_{};
    std::mutex action_mutex_;
    std::deque<PendingAction> pending_actions_;
    std::uint64_t next_serial_{};
};

}  // namespace synth::client
