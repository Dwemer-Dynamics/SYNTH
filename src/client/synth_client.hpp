#pragma once

#include "client/http.hpp"
#include "core/cancellation.hpp"
#include "protocol_native/v1_codec.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <unordered_set>
#include <vector>

namespace synth::client {

struct SessionOptions final {
    std::string runtime_session_id;
    std::string runtime_variant;
    std::string client_version;
    std::string runtime_version;
    std::string locale{"en-US"};
    std::vector<std::string> capabilities;
    std::uint32_t request_timeout_ms{30'000};
    std::size_t maximum_response_bytes{4U * 1024U * 1024U};
    std::uint64_t protocol_version{1};
};

// Immutable acknowledgement owned by a turn, never inferred from the latest client state.
struct ContextBinding final {
    std::uint64_t generation{};
    std::uint64_t sequence{};
    core::CancellationToken cancellation;
    std::string turn_id;
    std::string context_request_id;
    // Streaming admission requires an owner-scoped rollback of pending speech.
    std::function<void()> invalidate_response;
};

struct InterruptedTurn final {
    std::string context_request_id;
    std::uint64_t generation{};
    core::CancellationToken cancellation;
};

enum class RequestStatus : unsigned char {
    complete,
    cancelled,
    transport_failure,
    http_failure,
    server_failure,
    malformed_response,
    stale_response,
};

struct RequestOutcome final {
    RequestStatus status{RequestStatus::transport_failure};
    std::string request_id;
    std::string turn_id;
    std::string detail;
    std::vector<protocol_native::Line> lines;
    ContextBinding context_binding;
    bool save_checkpoint_acknowledged{};
    unsigned http_status{};
};

// Only SynthClient creates receipts. Polling retains the exact wire owner, never a caller's current selection.
class DiaryAdmission final {
public:
    [[nodiscard]] const RequestOutcome& outcome() const noexcept { return outcome_; }

private:
    friend class SynthClient;
    DiaryAdmission(RequestOutcome outcome, std::string body, std::string session, bool partial_supported)
        : outcome_{std::move(outcome)}, body_{std::move(body)}, session_{std::move(session)}, partial_supported_{partial_supported} {}
    RequestOutcome outcome_;
    std::string body_;
    std::string session_;
    bool partial_supported_{};
};

struct TtsStatusOutcome final {
    RequestStatus status{RequestStatus::transport_failure};
    std::string detail;
    std::optional<protocol_native::TtsStatus> value;
};

struct VisualOutcome final {
    RequestStatus status{RequestStatus::transport_failure};
    std::string request_id;
    std::string turn_id;
    std::string capture_id;
    std::string detail;
};

class SynthClient final {
public:
    using LineCallback = std::function<void(const protocol_native::Line&)>;

    SynthClient(IHttpTransport& transport, SessionOptions options)
        : transport_{transport}, options_{std::move(options)} {
        if (!valid_wire_id(options_.runtime_session_id) ||
            !valid_variant(options_.runtime_variant) || options_.client_version.empty()
            || options_.runtime_version.empty() || options_.request_timeout_ms == 0
            || options_.maximum_response_bytes == 0
            || (options_.protocol_version != 1 && options_.protocol_version != 2)) {
            throw std::invalid_argument{"invalid SYNTH session options"};
        }
    }

    [[nodiscard]] const std::string& runtime_session_id() const noexcept { return options_.runtime_session_id; }
    // The parent-bound wire contract fails closed if the server cannot honor owned continuation.
    [[nodiscard]] bool uses_rechat_parent_contract() const noexcept {
        return options_.protocol_version == 2 &&
            std::ranges::find(options_.capabilities, "dialogue.rechat.ownership") != options_.capabilities.end();
    }
    [[nodiscard]] bool uses_rechat_scene_contract() const noexcept {
        return uses_rechat_parent_contract() &&
            std::ranges::find(options_.capabilities, "dialogue.rechat.scene") != options_.capabilities.end();
    }
    [[nodiscard]] bool rechat_scene_ready() const noexcept {
        return rechat_scene_negotiated_.load(std::memory_order_acquire) && !halted_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool quest_events_ready() const noexcept {
        return quest_events_negotiated_.load(std::memory_order_acquire) && !halted_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool player_events_ready() const noexcept {
        return player_events_negotiated_.load(std::memory_order_acquire) && !halted_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool actor_events_ready() const noexcept {
        return actor_events_negotiated_.load(std::memory_order_acquire) && !halted_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool extended_inventory_ready() const noexcept {
        return extended_inventory_negotiated_.load(std::memory_order_acquire) && !halted_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool action_inventory_ready() const noexcept {
        return action_inventory_negotiated_.load(std::memory_order_acquire) && !halted_.load(std::memory_order_acquire);
    }
    // Wire selection only; actual forwarding and equipment execution also require the completed init ACK.
    [[nodiscard]] bool uses_action_inventory_contract() const noexcept {
        return options_.protocol_version==2 &&
            std::ranges::find(options_.capabilities,"action.inventory_observation")!=options_.capabilities.end() &&
            std::ranges::find(options_.capabilities,"dialogue.turn_ownership")!=options_.capabilities.end();
    }
    // Opted-in equipment must not mutate the game when its resulting inventory cannot be acknowledged.
    // Legacy sessions retain their existing execution path; cancellation still governs every dispatch.
    [[nodiscard]] bool equipment_actions_ready() const noexcept {
        return !uses_action_inventory_contract() || action_inventory_ready();
    }
    // Consumption has no legacy fallback: an equipment-only ACK cannot authorize its mutation or receipt.
    [[nodiscard]] bool uses_consumption_contract() const noexcept {
        return options_.runtime_variant == "flat" && uses_action_inventory_contract() &&
            std::ranges::find(options_.capabilities, "action.consume") != options_.capabilities.end() &&
            std::ranges::find(options_.capabilities, "action.consume_inventory") != options_.capabilities.end();
    }
    [[nodiscard]] bool consumption_ready() const noexcept {
        return consumption_negotiated_.load(std::memory_order_acquire) && action_inventory_ready();
    }
    // Paired transfers require their own acknowledgement; single-actor receipt readiness is insufficient.
    [[nodiscard]] bool uses_transfer_contract() const noexcept {
        return options_.runtime_variant == "flat" && uses_action_inventory_contract() &&
            std::ranges::find(options_.capabilities, "action.give_item_to") != options_.capabilities.end() &&
            std::ranges::find(options_.capabilities, "action.transfer_inventory") != options_.capabilities.end();
    }
    [[nodiscard]] bool transfer_ready() const noexcept {
        return transfer_negotiated_.load(std::memory_order_acquire) && action_inventory_ready();
    }
    // Caps extend the paired contract, but an older server's transfer ACK never grants currency mutation.
    [[nodiscard]] bool uses_caps_contract() const noexcept {
        return uses_transfer_contract() &&
            std::ranges::find(options_.capabilities, "action.give_caps_to") != options_.capabilities.end() &&
            std::ranges::find(options_.capabilities, "action.caps_inventory") != options_.capabilities.end();
    }
    [[nodiscard]] bool caps_ready() const noexcept {
        return caps_negotiated_.load(std::memory_order_acquire) && transfer_ready();
    }
    // Player debits require a separate opt-in and ACK; NPC-giving-caps readiness grants no payment authority.
    [[nodiscard]] bool uses_player_caps_contract() const noexcept {
        return uses_caps_contract() &&
            std::ranges::find(options_.capabilities, "action.take_caps_from_player") != options_.capabilities.end() &&
            std::ranges::find(options_.capabilities, "action.player_caps_inventory") != options_.capabilities.end();
    }
    [[nodiscard]] bool player_caps_ready() const noexcept {
        return player_caps_negotiated_.load(std::memory_order_acquire) && caps_ready();
    }
    [[nodiscard]] bool player_reactions_ready() const noexcept {
        return player_reactions_negotiated_.load(std::memory_order_acquire) && !halted_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool quest_tracking_ready() const noexcept {
        return quest_tracking_negotiated_.load(std::memory_order_acquire) && !halted_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool quest_reactions_ready() const noexcept {
        return quest_reactions_negotiated_.load(std::memory_order_acquire) && !halted_.load(std::memory_order_acquire);
    }

    // Worker-only ownership of a context publication and the request that consumes it.
    // Individual HTTP methods re-enter this lock; other workers cannot replace the
    // server's v1 session context between the two calls. Hard-halt transport cancellation bypasses this lock.
    [[nodiscard]] std::unique_lock<std::recursive_mutex> begin_context_turn() {
        return std::unique_lock<std::recursive_mutex>{request_mutex_};
    }

    [[nodiscard]] RequestOutcome initialize(
        std::uint64_t generation, bool hmd_pose_fresh,
        const std::vector<protocol_native::LoadedPlugin>& loaded_plugins = {}) {
        std::scoped_lock request_lock{request_mutex_};
        std::string request_id;
        std::string turn_id;
        std::string body;
        {
            std::scoped_lock state_lock{state_mutex_};
            if (generation == 0 || generation <= generation_) {
                throw std::invalid_argument{"init generation must advance"};
            }
            generation_ = generation;
            active_turn_anchor_.reset();
            halt_attempted_ = false;
            context_sequence_ = 0;
            publication_sequence_ = 0;
            rechat_scene_negotiated_.store(false, std::memory_order_release);
            quest_events_negotiated_.store(false, std::memory_order_release);
            player_events_negotiated_.store(false, std::memory_order_release);
            actor_events_negotiated_.store(false, std::memory_order_release);
            extended_inventory_negotiated_.store(false, std::memory_order_release);
            action_inventory_negotiated_.store(false, std::memory_order_release);
            consumption_negotiated_.store(false, std::memory_order_release);
            transfer_negotiated_.store(false, std::memory_order_release);
            caps_negotiated_.store(false, std::memory_order_release);
            player_caps_negotiated_.store(false, std::memory_order_release);
            player_reactions_negotiated_.store(false, std::memory_order_release);
            quest_tracking_negotiated_.store(false, std::memory_order_release);
            quest_reactions_negotiated_.store(false, std::memory_order_release);
            halted_.store(false, std::memory_order_release);
            request_id = next_id("init");
            turn_id = "turn:init:" + std::to_string(generation);
            body = protocol_native::encode_init(
                context(request_id, turn_id), options_.locale, hmd_pose_fresh, loaded_plugins);
        }
        auto outcome = send_event(request_id, turn_id, generation, std::move(body), {});
        {
            std::scoped_lock state_lock{state_mutex_};
            initialized_ = outcome.status == RequestStatus::complete;
            const auto* status = outcome.lines.size() == 3
                ? std::get_if<protocol_native::Status>(&outcome.lines[1].payload) : nullptr;
            const bool framed_init = initialized_ && options_.protocol_version == 2 && status &&
                std::holds_alternative<protocol_native::Start>(outcome.lines.front().payload) &&
                std::holds_alternative<protocol_native::End>(outcome.lines.back().payload);
            const bool supported_init = framed_init && options_.runtime_variant == "flat";
            rechat_scene_negotiated_.store(framed_init && status->code.starts_with("init_") &&
                status->code.ends_with("_accepted") && status->rechat_scene && uses_rechat_scene_contract(),
                std::memory_order_release);
            const bool player_caps_support = supported_init && status->code == "init_player_caps_inventory_accepted" && uses_player_caps_contract();
            player_caps_negotiated_.store(player_caps_support, std::memory_order_release);
            const bool caps_support = supported_init && (player_caps_support || status->code == "init_caps_inventory_accepted") && uses_caps_contract();
            caps_negotiated_.store(caps_support, std::memory_order_release);
            const bool transfer_support = supported_init && (caps_support || status->code == "init_transfer_inventory_accepted") && uses_transfer_contract();
            transfer_negotiated_.store(transfer_support, std::memory_order_release);
            const bool consumption_support = supported_init && (transfer_support || status->code == "init_consume_inventory_accepted") &&
                uses_consumption_contract();
            consumption_negotiated_.store(consumption_support, std::memory_order_release);
            const bool action_inventory_support = supported_init && (transfer_support || consumption_support || status->code == "init_action_inventory_accepted") &&
                uses_action_inventory_contract();
            action_inventory_negotiated_.store(action_inventory_support, std::memory_order_release);
            const bool inventory_support = supported_init && (action_inventory_support || status->code == "init_inventory_512_accepted") &&
                std::ranges::find(options_.capabilities, "context.inventory_512") != options_.capabilities.end();
            extended_inventory_negotiated_.store(inventory_support, std::memory_order_release);
            const bool actor_support = supported_init && (action_inventory_support || inventory_support || status->code == "init_actor_events_accepted") &&
                std::ranges::find(options_.capabilities, "context.actor_events") != options_.capabilities.end();
            actor_events_negotiated_.store(actor_support, std::memory_order_release);
            quest_events_negotiated_.store(supported_init &&
                (action_inventory_support || inventory_support || actor_support || status->code == "init_quest_events_accepted" || status->code == "init_quest_events_tracking_accepted" || status->code == "init_quest_reactions_accepted" || status->code == "init_player_events_accepted" || status->code == "init_player_reactions_accepted") &&
                std::ranges::find(options_.capabilities, "context.quest_events") != options_.capabilities.end(),
                std::memory_order_release);
            quest_tracking_negotiated_.store(supported_init &&
                (action_inventory_support || inventory_support || actor_support || status->code == "init_quest_tracking_accepted" || status->code == "init_quest_events_tracking_accepted" || status->code == "init_quest_reactions_accepted" || status->code == "init_player_events_accepted" || status->code == "init_player_reactions_accepted") &&
                std::ranges::find(options_.capabilities, "context.quest_tracking") != options_.capabilities.end(),
                std::memory_order_release);
            quest_reactions_negotiated_.store(supported_init && (action_inventory_support || inventory_support || actor_support || status->code == "init_quest_reactions_accepted" || status->code == "init_player_events_accepted" || status->code == "init_player_reactions_accepted") &&
                quest_tracking_negotiated_.load(std::memory_order_acquire) &&
                std::ranges::find(options_.capabilities, "dialogue.quest_reactions") != options_.capabilities.end(),
                std::memory_order_release);
            player_events_negotiated_.store(supported_init && (action_inventory_support || inventory_support || actor_support || status->code == "init_player_events_accepted" || status->code == "init_player_reactions_accepted") &&
                std::ranges::find(options_.capabilities, "context.player_events") != options_.capabilities.end(),
                std::memory_order_release);
            player_reactions_negotiated_.store(supported_init && (action_inventory_support || inventory_support || actor_support || status->code == "init_player_reactions_accepted") &&
                player_events_negotiated_.load(std::memory_order_acquire) &&
                std::ranges::find(options_.capabilities, "dialogue.player_reactions") != options_.capabilities.end(),
                std::memory_order_release);
        }
        // Capabilities must match init: an unacknowledged scene contract cannot silently downgrade.
        if (outcome.status == RequestStatus::complete && uses_rechat_scene_contract() && !rechat_scene_ready()) {
            std::scoped_lock state_lock{state_mutex_};
            initialized_ = false;
            halted_.store(true, std::memory_order_release);
            outcome.status = RequestStatus::malformed_response;
            outcome.detail = "server did not acknowledge fresh continuation scenes";
        }
        return outcome;
    }

    [[nodiscard]] RequestOutcome publish_context(
        std::uint64_t sequence,
        std::string playthrough_id,
        const protocol_native::Identity& player,
        const std::optional<protocol_native::Identity>& target,
        const std::vector<protocol_native::Identity>& audience,
        const std::vector<protocol_native::ActorState>& actor_states,
        const std::optional<protocol_native::WorldState>& world,
        const std::vector<protocol_native::QuestState>& active_quests,
        const std::vector<protocol_native::NearbyItemState>& nearby_items,
        const std::vector<protocol_native::PointOfInterestState>& points_of_interest,
        LineCallback callback = {}, core::CancellationToken cancellation = {},
        const std::optional<core::SavedContext>& loaded_context = std::nullopt,
        std::string active_quests_observation = {},
        std::string nearby_items_observation = {}, std::string points_of_interest_observation = {},
        std::string audience_observation = {},
        const std::optional<protocol_native::NativeQuestBatch>& quest_events = std::nullopt,
        const std::optional<protocol_native::NativePlayerEvent>& player_event = std::nullopt,
        const std::optional<protocol_native::NativeActorBatch>& actor_events = std::nullopt) {
        std::scoped_lock request_lock{request_mutex_};
        std::string request_id;
        std::string turn_id;
        std::string body;
        std::uint64_t generation{};
        protocol_native::EventContext publication_context;
        bool tracking_supported{};
        bool inventory_supported{};
        {
            std::scoped_lock state_lock{state_mutex_};
            require_ready();
            // Zero requests a publication-order ID, independent of captured frame order.
            if (quest_events && !quest_events_ready())
                throw std::invalid_argument{"server did not acknowledge native quest support"};
            if (player_event && !player_events_ready())
                throw std::invalid_argument{"server did not acknowledge native player event support"};
            if (actor_events && !actor_events_ready())
                throw std::invalid_argument{"server did not acknowledge native actor event support"};
            if (cancellation.generation().valid() && cancellation.generation().value() != generation_)
                throw std::invalid_argument{"context owner belongs to another runtime generation"};
            if (sequence == 0) sequence = publication_sequence_ + 1;
            if (sequence > 9'007'199'254'740'991ULL)
                throw std::invalid_argument{"context sequence is out of bounds"};
            if (sequence <= context_sequence_) throw std::invalid_argument{"context sequence must advance"};
            publication_sequence_ = std::max(publication_sequence_, sequence);
            request_id = next_id("context");
            turn_id = "turn:context:" + std::to_string(sequence);
            generation = generation_;
            publication_context = context(request_id, turn_id);
            tracking_supported = quest_tracking_ready();
            inventory_supported = extended_inventory_ready();
        }
        // Game-thread readiness/interruption shares state_mutex_: do not hold it for snapshot copies or encoding.
        // request_mutex_ still serializes publication and initialization; send_event rechecks cancellation/retirement.
        auto negotiated_quests = active_quests;
        if (!tracking_supported) for (auto& quest : negotiated_quests) quest.tracked.reset();
        auto negotiated_states = actor_states;
        const std::size_t inventory_limit = inventory_supported ? 512 : 32;
        for (auto& state : negotiated_states) {
            if (state.inventory.size() <= inventory_limit) continue;
            state.inventory.resize(inventory_limit);
            state.inventory_observation = "partial";
        }
        // Run only on the worker: bound the actual encoded request, including escaped names.
        // Never mutate the captured snapshot or describe a trimmed inventory as complete.
        for (;;) {
            try {
                body = protocol_native::encode_context(
                    publication_context, playthrough_id, sequence, player, target,
                    audience, negotiated_states, world, negotiated_quests, nearby_items, points_of_interest, loaded_context,
                    active_quests_observation, nearby_items_observation,
                    points_of_interest_observation, audience_observation, quest_events, player_event, actor_events);
                if (body.size() <= 1'048'576) break;
            } catch (const json::ByteLimitError&) {
                // The bounded writer stops before allocating an oversized body. Other errors propagate.
            }
            auto largest = std::ranges::max_element(negotiated_states, {}, [](const auto& state) { return state.inventory.size(); });
            if (largest == negotiated_states.end() || largest->inventory.empty())
                throw std::invalid_argument{"context exceeds server request byte limit"};
            largest->inventory.resize(largest->inventory.size() / 2);
            largest->inventory_observation = "partial";
        }
        auto outcome = send_event(request_id, turn_id, generation, std::move(body),
                                  std::move(callback), true, "/main.php", {generation, 0, cancellation});
        if ((player_event || actor_events) && outcome.status == RequestStatus::complete) {
            const auto* status = outcome.lines.size() == 3
                ? std::get_if<protocol_native::Status>(&outcome.lines[1].payload) : nullptr;
            if (!status || status->code != "context_accepted") {
                outcome.status = RequestStatus::malformed_response;
                outcome.detail = "native event context acknowledgement is missing";
            }
        }
        if (outcome.status == RequestStatus::complete) {
            std::scoped_lock state_lock{state_mutex_};
            if (generation_ == generation) {
                context_sequence_ = sequence;
                outcome.context_binding = {generation, sequence, cancellation, turn_id, request_id};
            }
        }
        return outcome;
    }

    [[nodiscard]] RequestOutcome send_text(std::string text, LineCallback callback = {},
                                            ContextBinding binding = {}, const protocol_native::ControlSelection& selection = {}) {
        std::scoped_lock request_lock{request_mutex_};
        std::string request_id;
        std::string turn_id;
        std::string body;
        std::uint64_t generation{};
        {
            std::scoped_lock state_lock{state_mutex_};
            require_ready();
            if (text.empty() || text.size() > 4096) throw std::invalid_argument{"dialogue text is out of bounds"};
            request_id = next_id("text");
            turn_id = binding.turn_id.empty() ? next_id("turn") : binding.turn_id;
            generation = generation_;
            body = protocol_native::encode_input_text(context(request_id, turn_id, binding), std::move(text), selection);
        }
        return send_event(request_id, turn_id, generation, std::move(body), std::move(callback), true, "/main.php", binding, true);
    }

    [[nodiscard]] RequestOutcome activate(const protocol_native::Identity& actor,
                                          std::string source, ContextBinding binding = {}) {
        std::scoped_lock request_lock{request_mutex_};
        std::string request_id;
        std::string turn_id;
        std::string body;
        std::uint64_t generation{};
        {
            std::scoped_lock state_lock{state_mutex_};
            require_ready();
            if (source != "manual" && source != "automatic") {
                throw std::invalid_argument{"invalid activation source"};
            }
            request_id = next_id("activate");
            turn_id = binding.turn_id.empty() ? next_id("turn") : binding.turn_id;
            generation = generation_;
            body = protocol_native::encode_activate(context(request_id, turn_id, binding), actor,
                                                    std::move(source));
        }
        return send_event(request_id, turn_id, generation, std::move(body), {}, true, "/main.php", binding);
    }

    [[nodiscard]] RequestOutcome trigger(std::string kind,
                                         const protocol_native::Identity& actor,
                                         LineCallback callback = {},
                                         std::optional<std::uint64_t> game_time_ticks = std::nullopt,
                                         std::optional<std::string> external_text = std::nullopt,
                                         ContextBinding binding = {},
                                         std::optional<std::string> capture_id = std::nullopt,
                                         std::optional<std::uint64_t> previous_context_sequence = std::nullopt,
                                         std::optional<std::uint64_t> player_event_serial = std::nullopt,
                                         std::optional<protocol_native::RechatParent> rechat_parent = std::nullopt) {
        std::scoped_lock request_lock{request_mutex_};
        std::string request_id;
        std::string turn_id;
        std::string body;
        std::uint64_t generation{};
        {
            std::scoped_lock state_lock{state_mutex_};
            require_ready();
            if (kind != "auto_greeting" && kind != "bored" && kind != "combat_bark" &&
                kind != "rechat" && kind != "external_comment" && kind != "external_reaction" &&
                kind != "external_tts" && kind != "quest_updated" && kind != "player_reaction") {
                throw std::invalid_argument{"invalid dialogue trigger"};
            }
            if (kind == "quest_updated" && !quest_reactions_ready())
                throw std::invalid_argument{"quest reactions were not negotiated"};
            if (kind == "player_reaction" && !player_reactions_ready())
                throw std::invalid_argument{"player reactions were not negotiated"};
            if (kind == "auto_greeting" && (!game_time_ticks || *game_time_ticks == 0)) {
                throw std::invalid_argument{"auto greeting requires Fallout game time"};
            }
            if ((kind == "external_reaction" || kind == "external_tts") &&
                (!external_text || external_text->empty() || external_text->size() > 1000)) {
                throw std::invalid_argument{"external trigger requires 1 to 1000 bytes of text"};
            }
            if (kind != "external_reaction" && kind != "external_tts" && external_text) {
                throw std::invalid_argument{"text is not valid for this dialogue trigger"};
            }
            request_id = next_id("trigger");
            turn_id = binding.turn_id.empty() ? next_id("turn") : binding.turn_id;
            generation = generation_;
            body = protocol_native::encode_trigger(context(request_id, turn_id, binding),
                                                   kind, actor, game_time_ticks,
                                                   kind == "external_reaction" ? external_text : std::nullopt,
                                                   kind == "external_tts" ? external_text : std::nullopt, capture_id, previous_context_sequence, player_event_serial, std::move(rechat_parent));
        }
        const auto target = kind == "external_tts" ? "/processor/npc_tts_play.php" : "/main.php";
        return send_event(request_id, turn_id, generation, std::move(body), std::move(callback), true, target, binding, true);
    }

    [[nodiscard]] RequestOutcome send_audio(const protocol_native::Media& media,
                                            LineCallback callback = {}, ContextBinding binding = {}, const protocol_native::ControlSelection& selection = {}) {
        std::scoped_lock request_lock{request_mutex_};
        std::string request_id;
        std::string turn_id;
        std::string body;
        std::uint64_t generation{};
        {
            std::scoped_lock state_lock{state_mutex_};
            require_ready();
            request_id = next_id("audio");
            turn_id = binding.turn_id.empty() ? next_id("turn") : binding.turn_id;
            generation = generation_;
            body = protocol_native::encode_input_audio(context(request_id, turn_id, binding), media, selection);
        }
        return send_event(request_id, turn_id, generation, std::move(body), std::move(callback), true, "/main.php", binding, true);
    }

    // Worker-only, generation-owned settings RPC; it never starts or cancels a dialogue turn.
    [[nodiscard]] RequestOutcome control(std::string setting, std::string value) {
        std::scoped_lock request_lock{request_mutex_};
        std::string request_id, turn_id, body;
        std::uint64_t generation{};
        {
            std::scoped_lock state_lock{state_mutex_};
            require_ready();
            if (options_.protocol_version != 2 ||
                std::ranges::find(options_.capabilities, "control.menu") == options_.capabilities.end())
                throw std::invalid_argument{"control menu is not negotiated"};
            request_id = next_id("control");
            turn_id = next_id("settings");
            generation = generation_;
            body = protocol_native::encode_control(context(request_id, turn_id), std::move(setting), std::move(value));
        }
        return send_event(request_id, turn_id, generation, std::move(body), {}, true, "/main.php", {}, false, 5000);
    }

    [[nodiscard]] RequestOutcome refresh_dynamic_profiles(
        const std::vector<protocol_native::Identity>& actors, bool include_narrator, ContextBinding binding = {}) {
        std::scoped_lock request_lock{request_mutex_};
        std::string request_id;
        std::string turn_id;
        std::string body;
        std::uint64_t generation{};
        {
            std::scoped_lock state_lock{state_mutex_};
            require_ready();
            if (actors.size() > 16) throw std::invalid_argument{"too many profile refresh actors"};
            request_id = next_id("profile-refresh");
            turn_id = binding.turn_id.empty() ? next_id("turn") : binding.turn_id;
            generation = generation_;
            body = protocol_native::encode_profile_refresh(
                context(request_id, turn_id, binding), actors, include_narrator);
        }
        return send_event(request_id, turn_id, generation, std::move(body), {}, true, "/main.php", binding);
    }

    // A diary response acknowledges durable admission, never model completion or game dialogue.
    [[nodiscard]] DiaryAdmission request_npc_diary(const protocol_native::Identity& actor, ContextBinding binding) {
        return request_diary(actor, std::move(binding), protocol_native::DiaryRole::npc);
    }

    [[nodiscard]] DiaryAdmission request_diary(const protocol_native::Identity& actor, ContextBinding binding, protocol_native::DiaryRole role) {
        if (role != protocol_native::DiaryRole::npc && role != protocol_native::DiaryRole::narrator && role != protocol_native::DiaryRole::player)
            throw std::invalid_argument{"unsupported diary role"};
        return request_diary(protocol_native::DiaryRequest{
            role == protocol_native::DiaryRole::npc ? std::vector<protocol_native::Identity>{actor} : std::vector<protocol_native::Identity>{},
            role == protocol_native::DiaryRole::player, role == protocol_native::DiaryRole::narrator}, std::move(binding));
    }

    [[nodiscard]] DiaryAdmission request_diary(const protocol_native::DiaryRequest& request, ContextBinding binding) {
        std::scoped_lock request_lock{request_mutex_};
        std::string request_id, turn_id, body;
        std::uint64_t generation{};
        const auto batch = request.actors.size() + static_cast<std::size_t>(request.include_player) +
            static_cast<std::size_t>(request.include_narrator) > 1;
        {
            std::scoped_lock state_lock{state_mutex_};
            require_ready();
            const auto has = [this](std::string_view capability) {
                return std::ranges::find(options_.capabilities, capability) != options_.capabilities.end();
            };
            if ((batch && !has("diary.batch.status")) ||
                (!request.actors.empty() && !has("diary.manual.npc")) ||
                (request.include_player && !has("diary.manual.player")) ||
                (request.include_narrator && !has("diary.manual.narrator")) ||
                (request.reason == protocol_native::DiaryReason::sleep && !has("diary.automatic.sleep")) ||
                (request.reason == protocol_native::DiaryReason::wait && !has("diary.automatic.wait")))
                throw std::invalid_argument{"diary role or observation capability unavailable"};
            request_id = next_id("diary");
            turn_id = binding.turn_id.empty() ? next_id("turn") : binding.turn_id;
            generation = generation_;
            body = protocol_native::encode_diary_request(context(request_id, turn_id, binding), request);
        }
        auto outcome = send_event(request_id, turn_id, generation, body, {}, true, "/main.php", binding);
        validate_diary_status(outcome, false);
        return DiaryAdmission{std::move(outcome), std::move(body), options_.runtime_session_id, batch};
    }

    // Poll the exact admission owner with a short RPC; this endpoint never starts or replays generation.
    [[nodiscard]] RequestOutcome fetch_diary_status(const DiaryAdmission& receipt) {
        std::scoped_lock request_lock{request_mutex_};
        const auto& admission = receipt.outcome_;
        {
            std::scoped_lock state_lock{state_mutex_};
            require_ready();
            if (admission.status != RequestStatus::complete || admission.detail != "diary_queued" ||
                admission.request_id.empty() || admission.turn_id.empty() || receipt.session_ != options_.runtime_session_id)
                throw std::invalid_argument{"a queued diary admission is required"};
            // Validate the old binding against the current generation without rebuilding any event fields.
            (void)context(admission.request_id, admission.turn_id, admission.context_binding);
        }
        auto outcome = send_event(admission.request_id, admission.turn_id, admission.context_binding.generation,
            receipt.body_, {}, true, "/diary_status.php", admission.context_binding, false, 2000);
        validate_diary_status(outcome, true, receipt.partial_supported_);
        return outcome;
    }

    [[nodiscard]] HttpResult upload_media(const protocol_native::Media& media,
                                          std::string bytes,
                                          const Cancelled& cancelled = {}) {
        std::scoped_lock request_lock{request_mutex_};
        {
            std::scoped_lock state_lock{state_mutex_};
            require_ready();
        }
        if (media.media_id != "media/" + media.sha256 || media.size != bytes.size()
            || bytes.empty() || bytes.size() > 16U * 1024U * 1024U) {
            throw std::invalid_argument{"media upload does not match its opaque reference"};
        }
        return transport_.send(HttpRequest{
            .method = HttpMethod::put,
            .target = "/media.php/" + media.media_id,
            .content_type = media.content_type,
            .body = std::move(bytes),
            .timeout_ms = options_.request_timeout_ms,
            .accept = "application/json",
            .maximum_response_bytes = 64U * 1024U,
        }, cancelled);
    }

    [[nodiscard]] VisualOutcome send_visual_capture(
        std::string interaction_mode,
        std::string perspective,
        const protocol_native::Media& media,
        const std::optional<protocol_native::WorldState>& world,
        const std::optional<protocol_native::Identity>& subject,
        const std::vector<protocol_native::Identity>& nearby_actors,
        const Cancelled& cancelled = {}, ContextBinding binding = {}) {
        std::scoped_lock request_lock{request_mutex_};
        VisualOutcome outcome;
        const auto capture_cancelled = [&] {
            return (binding.cancellation.generation().valid() && binding.cancellation.is_cancelled()) || (cancelled && cancelled()) ||
                   halted_.load(std::memory_order_acquire);
        };
        if (capture_cancelled()) { outcome.status = RequestStatus::cancelled; return outcome; }
        std::string body;
        std::uint64_t generation{};
        {
            std::scoped_lock state_lock{state_mutex_};
            require_ready();
            if (interaction_mode != "store_only" && interaction_mode != "describe" &&
                interaction_mode != "npc_portrait") {
                throw std::invalid_argument{"invalid visual interaction mode"};
            }
            if ((options_.runtime_variant == "vr" && perspective != "hmd") ||
                (options_.runtime_variant == "flat" && perspective != "first_person" &&
                 perspective != "third_person")) {
                throw std::invalid_argument{"visual perspective does not match runtime lane"};
            }
            if (interaction_mode == "npc_portrait" && !subject) {
                throw std::invalid_argument{"NPC portrait requires canonical subject identity"};
            }
            outcome.request_id = next_id("visual");
            outcome.turn_id = binding.turn_id.empty() ? next_id("turn") : binding.turn_id;
            outcome.capture_id = next_id("capture");
            generation = generation_;
            body = protocol_native::encode_visual_capture(
                context(outcome.request_id, outcome.turn_id, binding), outcome.capture_id,
                std::move(interaction_mode), std::move(perspective), media, world, subject,
                nearby_actors);
            if (!binding.context_request_id.empty() && binding.cancellation.generation().valid()
                && options_.protocol_version == 2 && !capture_cancelled()
                && std::ranges::find(options_.capabilities, "dialogue.turn_cancel") != options_.capabilities.end()) {
                active_turn_anchor_ = InterruptedTurn{binding.context_request_id, generation, binding.cancellation};
            }
        }
        const auto result = transport_.send(HttpRequest{
            .method = HttpMethod::post,
            .target = "/itt.php",
            .content_type = "application/json",
            .body = std::move(body),
            .timeout_ms = options_.request_timeout_ms,
            .accept = "application/json",
            .maximum_response_bytes = 256U * 1024U,
        }, capture_cancelled);
        if (capture_cancelled()) { outcome.status = RequestStatus::cancelled; return outcome; }
        {
            std::scoped_lock state_lock{state_mutex_};
            if (generation_ != generation) { outcome.status = RequestStatus::stale_response; return outcome; }
        }
        if (!result.delivered()) {
            outcome.status = result.failure == TransportFailure::cancelled
                                 ? RequestStatus::cancelled
                                 : RequestStatus::transport_failure;
            return outcome;
        }
        try {
            const auto response = protocol_native::decode_visual_response(result.response.body);
            if (response.request_id != outcome.request_id || response.turn_id != outcome.turn_id ||
                response.capture_id != outcome.capture_id || response.generation != generation) {
                outcome.status = RequestStatus::stale_response;
                return outcome;
            }
            outcome.detail = response.detail;
            if (result.response.status < 200 || result.response.status >= 300 || !response.ok) {
                outcome.status = RequestStatus::http_failure;
                if (outcome.detail.empty()) {
                    outcome.detail = "HTTP " + std::to_string(result.response.status);
                }
                return outcome;
            }
        } catch (const std::exception& error) {
            outcome.status = RequestStatus::malformed_response;
            outcome.detail = error.what();
            return outcome;
        }
        outcome.status = RequestStatus::complete;
        return outcome;
    }

    [[nodiscard]] HttpResult fetch_media(std::string_view media_id,
                                         const Cancelled& cancelled = {},
                                         std::chrono::steady_clock::time_point deadline =
                                             std::chrono::steady_clock::time_point::max()) {
        if (!media_id.starts_with("media/") || media_id.size() != 70) {
            throw std::invalid_argument{"invalid media ID"};
        }
        return send_speech_request(HttpRequest{
            .method = HttpMethod::get,
            .target = "/media.php/" + std::string{media_id},
            .content_type = {},
            .body = {},
            .timeout_ms = options_.request_timeout_ms,
            .accept = "audio/wav, audio/ogg",
            .maximum_response_bytes = 16U * 1024U * 1024U,
        }, cancelled, deadline);
    }

    [[nodiscard]] TtsStatusOutcome fetch_tts_status(std::string_view cache_key,
                                                    const Cancelled& cancelled = {},
                                                    std::chrono::steady_clock::time_point deadline =
                                                        std::chrono::steady_clock::time_point::max()) {
        if (cache_key.size() != 32 || !std::ranges::all_of(cache_key, [](char character) {
                return (character >= '0' && character <= '9') ||
                       (character >= 'a' && character <= 'f');
            })) {
            throw std::invalid_argument{"invalid TTS cache key"};
        }
        const auto result = send_speech_request(HttpRequest{
            .method = HttpMethod::get,
            .target = "/tts_status.php?cache_key=" + std::string{cache_key},
            .content_type = {},
            .body = {},
            .timeout_ms = options_.request_timeout_ms,
            .accept = "application/json",
            .maximum_response_bytes = 64U * 1024U,
        }, cancelled, deadline);
        if (!result.delivered()) {
            return {result.failure == TransportFailure::cancelled ? RequestStatus::cancelled
                                                                  : RequestStatus::transport_failure,
                    {}, std::nullopt};
        }
        if (result.response.status < 200 || result.response.status >= 300) {
            return {RequestStatus::http_failure,
                    "HTTP " + std::to_string(result.response.status), std::nullopt};
        }
        try {
            auto value = protocol_native::decode_tts_status(result.response.body);
            if (value.cache_key != cache_key) {
                return {RequestStatus::stale_response, "TTS cache key mismatch", std::nullopt};
            }
            return {RequestStatus::complete, {}, std::move(value)};
        } catch (const std::exception& error) {
            return {RequestStatus::malformed_response, error.what(), std::nullopt};
        }
    }

    // Admission is state-only: the game thread never waits on a request holding the HTTP mutex.
    [[nodiscard]] std::optional<InterruptedTurn> take_interrupted_turn() {
        std::scoped_lock lock{state_mutex_};
        if (!active_turn_anchor_ || !active_turn_anchor_->cancellation.is_cancelled()) return std::nullopt;
        return std::exchange(active_turn_anchor_, std::nullopt);
    }

    // Control RPCs must be able to overtake the serial dialogue request lane.
    [[nodiscard]] RequestOutcome cancel(std::string cancel_request_id, std::uint64_t expected_generation = 0) {
        std::string request_id;
        std::string turn_id;
        std::string body;
        std::uint64_t generation{};
        {
            std::scoped_lock state_lock{state_mutex_};
            require_ready();
            if (expected_generation != 0 && expected_generation != generation_)
                return {RequestStatus::cancelled, {}, {}, "cancel anchor belongs to a retired generation"};
            request_id = next_id("cancel");
            turn_id = next_id("turn");
            generation = generation_;
            body = protocol_native::encode_cancel(context(request_id, turn_id), std::move(cancel_request_id));
        }
        return send_event(request_id, turn_id, generation, std::move(body), {}, true, "/main.php", {}, false, 2000);
    }

    [[nodiscard]] RequestOutcome send_action_result(std::string action_id,
                                                    std::string idempotency_key,
                                                    std::string status,
                                                    std::string detail,
                                                    LineCallback callback = {}, ContextBinding binding = {},
                                                    std::optional<protocol_native::ActionInventoryObservation> inventory = std::nullopt,
                                                    std::optional<protocol_native::ActionTransferObservation> transfer = std::nullopt,
                                                    bool caps_transfer = false, bool player_caps_transfer = false) {
        std::scoped_lock request_lock{request_mutex_};
        std::string request_id;
        std::string turn_id;
        std::string body;
        std::uint64_t generation{};
        {
            std::scoped_lock state_lock{state_mutex_};
            require_ready();
            request_id = next_id("action-result");
            if (inventory && !action_inventory_ready()) throw std::invalid_argument{"action inventory support is not acknowledged"};
            if (transfer && !transfer_ready()) throw std::invalid_argument{"transfer inventory support is not acknowledged"};
            if (caps_transfer && (!transfer || !caps_ready())) throw std::invalid_argument{"caps inventory support is not acknowledged or paired evidence is missing"};
            if (player_caps_transfer && (!caps_transfer || !transfer || !player_caps_ready()))
                throw std::invalid_argument{"player caps support is not acknowledged or paired currency evidence is missing"};
            turn_id = binding.turn_id.empty() ? next_id("turn") : binding.turn_id;
            generation = generation_;
            const auto bound_context = context(request_id, turn_id, binding);
            body = protocol_native::encode_action_result(protocol_native::ActionResultEvent{
                options_.runtime_session_id, request_id, turn_id, options_.runtime_variant, options_.client_version,
                options_.runtime_version, generation, options_.capabilities, std::move(action_id),
                std::move(idempotency_key), std::move(status), std::move(detail),
                bound_context.protocol_version, bound_context.context_sequence, std::move(inventory), std::move(transfer)});
        }
        return send_event(request_id, turn_id, generation, std::move(body), std::move(callback), true, "/main.php", binding, true);
    }

    [[nodiscard]] RequestOutcome hard_halt() {
        auto outcome = retire_session();
        return outcome ? std::move(*outcome)
                       : RequestOutcome{RequestStatus::cancelled, {}, {}, "session has no unretired initialization"};
    }

    // Coordinator-only retirement also covers init sent without an acknowledgement.
    [[nodiscard]] std::optional<RequestOutcome> retire_session() {
        halted_.store(true, std::memory_order_release);
        transport_.cancel_all();
        std::scoped_lock request_lock{request_mutex_};
        std::string request_id;
        std::string turn_id;
        std::string body;
        std::uint64_t generation{};
        {
            std::scoped_lock state_lock{state_mutex_};
            if (generation_ == 0 || halt_attempted_) return std::nullopt;
            halt_attempted_ = true;
            request_id = next_id("halt");
            turn_id = next_id("turn");
            generation = generation_;
            body = protocol_native::encode_halt(context(request_id, turn_id));
            initialized_ = false;
        }
        auto outcome = send_event(request_id, turn_id, generation, std::move(body), {}, false,
                                  "/main.php", {}, false, 2000);
        return outcome;
    }

    [[nodiscard]] bool ready() const noexcept {
        std::scoped_lock lock{state_mutex_};
        return initialized_ && !halted_.load(std::memory_order_acquire);
    }

private:
    // Immutable speech reads run beside dialogue HTTP and retain one cancellation/deadline budget.
    [[nodiscard]] HttpResult send_speech_request(HttpRequest request, const Cancelled& cancelled,
                                                std::chrono::steady_clock::time_point deadline) {
        using Clock = std::chrono::steady_clock;
        deadline = std::min(deadline, Clock::now() + std::chrono::milliseconds{request.timeout_ms});
        std::uint64_t generation{};
        {
            std::scoped_lock state_lock{state_mutex_};
            require_ready();
            generation = generation_;
        }
        const auto failure = [&] {
            if (cancelled && cancelled()) return TransportFailure::cancelled;
            std::scoped_lock state_lock{state_mutex_};
            if (generation_ != generation || !initialized_ || halted_.load(std::memory_order_acquire))
                return TransportFailure::cancelled;
            return Clock::now() >= deadline ? TransportFailure::timeout : TransportFailure::none;
        };
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (const auto stopped = failure(); stopped != TransportFailure::none) return {stopped, {}};
        if (remaining.count() <= 0) return {TransportFailure::timeout, {}};
        request.timeout_ms = static_cast<std::uint32_t>(remaining.count());
        auto result = transport_.send(request, [&] {
            return failure() != TransportFailure::none;
        });
        if (const auto stopped = failure(); stopped != TransportFailure::none) return {stopped, {}};
        return result;
    }

    [[nodiscard]] protocol_native::EventContext context(const std::string& request_id,
                                                        const std::string& turn_id, ContextBinding binding = {}) const {
        if (binding.cancellation.generation().valid() && binding.cancellation.generation().value() != generation_)
            throw std::invalid_argument{"turn owner belongs to another runtime generation"};
        if (binding.sequence != 0 && (binding.generation != generation_ || binding.sequence > context_sequence_))
            throw std::invalid_argument{"context binding is not acknowledged in this generation"};
        return {options_.runtime_session_id, request_id, turn_id, options_.runtime_variant, options_.client_version,
                options_.runtime_version, generation_, options_.capabilities, options_.protocol_version,
                options_.protocol_version == 2 ? binding.sequence : 0};
    }

    [[nodiscard]] RequestOutcome send_event(const std::string& request_id,
                                            const std::string& turn_id,
                                            std::uint64_t expected_generation,
                                            std::string body,
                                            LineCallback callback,
                                            bool cancel_when_halted = true,
                                            std::string target = "/main.php", ContextBinding binding = {},
                                            bool owns_turn = false, std::uint32_t timeout_ms = 0) {
        const auto cancelled = [this, cancel_when_halted, cancellation = binding.cancellation] {
            return (cancel_when_halted && halted_.load(std::memory_order_acquire)) ||
                (cancellation.generation().valid() && cancellation.is_cancelled());
        };
        if (cancelled()) return {RequestStatus::cancelled, request_id, turn_id,
                                 "request cancelled before send", {}, binding};
        if (owns_turn && !binding.context_request_id.empty() && binding.cancellation.generation().valid()
            && options_.protocol_version == 2
            && std::ranges::find(options_.capabilities, "dialogue.turn_cancel") != options_.capabilities.end()) {
            std::scoped_lock lock{state_mutex_};
            if (!cancelled()) active_turn_anchor_ = InterruptedTurn{
                binding.context_request_id, expected_generation, binding.cancellation};
        }
        RequestOutcome outcome{
            .status = RequestStatus::transport_failure,
            .request_id = request_id,
            .turn_id = turn_id,
            .detail = {},
            .lines = {},
            .context_binding = binding,
        };
        bool started{};
        bool ended{};
        bool received_chunks{};
        bool admitted_speech{};
        std::string pending;
        std::string parse_error;
        std::unordered_set<std::string> line_ids;
        const bool incremental = owns_turn && callback && binding.invalidate_response;
        // Any unsuccessful exit invalidates only this request owner's pending speech.
        struct AdmissionGuard final {
            const RequestOutcome& outcome;
            const bool& admitted;
            const std::function<void()>& invalidate;
            ~AdmissionGuard() {
                if (admitted && outcome.status != RequestStatus::complete && invalidate) {
                    try { invalidate(); } catch (...) {}
                }
            }
        } admission_guard{outcome, admitted_speech, binding.invalidate_response};
        const auto feed = [&](std::string_view bytes) {
            try {
                if (cancelled()) return false;
                pending.append(bytes);
                std::size_t offset{};
                for (;;) {
                    const auto newline = pending.find('\n', offset);
                    if (newline == std::string::npos) break;
                    const auto wire = std::string_view{pending}.substr(offset, newline - offset);
                    offset = newline + 1;
                    if (wire.empty()) continue;
                    if (ended) throw std::invalid_argument{"response continues after terminal line"};
                    auto line = protocol_native::decode_line(wire);
                    if (const auto* dialogue = std::get_if<protocol_native::Dialogue>(&line.payload);
                        dialogue && dialogue->listener &&
                        (std::ranges::find(options_.capabilities, "dialogue.listener_identity") == options_.capabilities.end()
                         || std::ranges::find(options_.capabilities, "dialogue.turn_ownership") == options_.capabilities.end()))
                        throw std::invalid_argument{"unnegotiated dialogue listener"};
                    if (line.request_id != request_id || line.turn_id != turn_id
                        || line.generation != expected_generation || line.runtime_variant != options_.runtime_variant
                        || line.protocol_version != options_.protocol_version
                        || line.context_sequence != (options_.protocol_version == 2 ? binding.sequence : 0)) {
                        outcome.status = RequestStatus::stale_response;
                        throw std::invalid_argument{"response does not match the active request"};
                    }
                    if (!line_ids.insert(line.line_id).second)
                        throw std::invalid_argument{"duplicate response line ID"};
                    if (std::holds_alternative<protocol_native::Start>(line.payload)) {
                        if (started) throw std::invalid_argument{"duplicate response start"};
                        started = true;
                    } else if (!started) {
                        throw std::invalid_argument{"response has no start line"};
                    }
                    if (const auto* end = std::get_if<protocol_native::End>(&line.payload)) {
                        ended = true;
                        outcome.status = end->status == "complete" ? RequestStatus::complete
                            : end->status == "cancelled" ? RequestStatus::cancelled : RequestStatus::server_failure;
                        if (end->status != "complete") outcome.detail = "server response " + end->status;
                    }
                    outcome.lines.push_back(std::move(line));
                    // Actions and continuation controls remain behind valid terminal completion.
                    if (incremental && std::holds_alternative<protocol_native::Dialogue>(outcome.lines.back().payload)) {
                        if (cancelled()) return false;
                        admitted_speech = true;
                        callback(outcome.lines.back());
                    }
                }
                pending.erase(0, offset);
                return true;
            } catch (const std::exception& error) {
                parse_error = error.what();
                if (outcome.status != RequestStatus::stale_response) outcome.status = RequestStatus::malformed_response;
                return false;
            }
        };
        const auto result = transport_.send(HttpRequest{
            .method = HttpMethod::post,
            .target = std::move(target),
            .content_type = "application/json",
            .body = std::move(body),
            .timeout_ms = timeout_ms == 0 ? options_.request_timeout_ms : timeout_ms,
            .accept = "application/x-ndjson",
            .maximum_response_bytes = options_.maximum_response_bytes,
            .response_chunk = incremental ? std::function<bool(std::string_view)>{[&](std::string_view bytes) {
                received_chunks = true;
                return feed(bytes);
            }} : std::function<bool(std::string_view)>{},
        }, cancelled);
        if (cancelled()) {
            outcome.status = RequestStatus::cancelled;
            outcome.detail = "request cancelled before response delivery";
            return outcome;
        }
        if (!parse_error.empty()) {
            outcome.detail = parse_error;
            return outcome;
        }
        if (!result.delivered()) {
            outcome.status = result.failure == TransportFailure::cancelled
                                 ? RequestStatus::cancelled
                                 : RequestStatus::transport_failure;
            switch (result.failure) {
            case TransportFailure::timeout: outcome.detail = "server request timed out"; break;
            case TransportFailure::cancelled: outcome.detail = "request cancelled"; break;
            case TransportFailure::server_unavailable: outcome.detail = "server is unavailable"; break;
            case TransportFailure::response_too_large: outcome.detail = "server response exceeded its limit"; break;
            default: outcome.detail = "HTTP transport failed"; break;
            }
            return outcome;
        }
        outcome.http_status = result.response.status;
        if (result.response.status < 200 || result.response.status >= 300) {
            outcome.status = RequestStatus::http_failure;
            outcome.detail = "HTTP " + std::to_string(result.response.status);
            // Report only a bounded machine code, never provider text or the raw response body.
            if (result.response.body.size() <= 64U * 1024U) {
                try {
                    const auto error = json::parse(result.response.body);
                    const auto* code = error.find("code");
                    if (protocol_native::text(error, "schema") == "synth.error.response.v1"
                        && code && code->is_string()) {
                        const auto& value = code->as_string();
                        if (!value.empty() && value.size() <= 64 && value.front() >= 'a' && value.front() <= 'z'
                            && std::ranges::all_of(value, [](char ch) {
                                return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
                                    || ch == '_' || ch == '.' || ch == '-';
                            })) outcome.detail += ": " + value;
                    }
                } catch (const std::exception&) {
                    // Non-protocol errors retain the HTTP status without reflecting untrusted content.
                }
            }
            return outcome;
        }
        try {
            if (!received_chunks && !feed(result.response.body)) {
                outcome.detail = parse_error;
                if (cancelled()) outcome.status = RequestStatus::cancelled;
                return outcome;
            }
            if (!pending.empty()) throw std::invalid_argument{"truncated NDJSON line"};
            if (!ended) throw std::invalid_argument{"response has no terminal line"};
            if (outcome.status == RequestStatus::complete && callback) {
                for (const auto& line : outcome.lines) {
                    if (cancelled()) {
                        outcome.status = RequestStatus::cancelled;
                        outcome.detail = "request cancelled before response delivery";
                        break;
                    }
                    if (!incremental || !std::holds_alternative<protocol_native::Dialogue>(line.payload)) callback(line);
                }
            }
        } catch (const std::exception& error) {
            outcome.status = RequestStatus::malformed_response;
            outcome.detail = error.what();
            return outcome;
        }
        return outcome;
    }

    // Diary control replies are exactly start/status/end, never dialogue, actions or partial streams.
    static void validate_diary_status(RequestOutcome& outcome, bool polling, bool partial_supported = false) {
        if (outcome.status != RequestStatus::complete) return;
        const auto* status = outcome.lines.size() == 3
            ? std::get_if<protocol_native::Status>(&outcome.lines[1].payload) : nullptr;
        if (!status || (status->code != "diary_queued" && status->code != "diary_disabled" &&
            (!polling || (status->code != "diary_running" && status->code != "diary_ready" &&
                          status->code != "diary_failed" && status->code != "diary_cancelled" &&
                          (!partial_supported || status->code != "diary_partial"))))) {
            outcome.status = RequestStatus::malformed_response;
            outcome.detail = "invalid diary control response";
        } else outcome.detail = status->code;
    }

    [[nodiscard]] std::string next_id(std::string_view prefix) {
        return std::string{prefix} + ':' + std::to_string(generation_) + ':' + std::to_string(++id_counter_);
    }

    void require_ready() const {
        if (!initialized_ || halted_.load(std::memory_order_acquire)) throw std::logic_error{"SYNTH session is not initialized"};
    }
    [[nodiscard]] static bool valid_variant(std::string_view value) noexcept {
        return value == "flat" || value == "vr";
    }
    [[nodiscard]] static bool valid_wire_id(std::string_view value) noexcept {
        if (value.empty() || value.size() > 128 || !std::isalnum(static_cast<unsigned char>(value.front()))) {
            return false;
        }
        return std::ranges::all_of(value, [](char character) {
            return std::isalnum(static_cast<unsigned char>(character)) || character == '.' ||
                   character == '_' || character == ':' || character == '-';
        });
    }

    IHttpTransport& transport_;
    SessionOptions options_;
    mutable std::recursive_mutex request_mutex_;
    mutable std::mutex state_mutex_;
    std::uint64_t generation_{};
    std::uint64_t context_sequence_{};
    std::uint64_t publication_sequence_{};
    std::uint64_t id_counter_{};
    bool initialized_{};
    std::atomic_bool quest_events_negotiated_{};
    std::atomic_bool player_events_negotiated_{};
    std::atomic_bool actor_events_negotiated_{};
    std::atomic_bool extended_inventory_negotiated_{};
    std::atomic_bool action_inventory_negotiated_{};
    std::atomic_bool consumption_negotiated_{};
    std::atomic_bool transfer_negotiated_{};
    std::atomic_bool caps_negotiated_{};
    std::atomic_bool player_caps_negotiated_{};
    std::atomic_bool player_reactions_negotiated_{};
    std::atomic_bool quest_tracking_negotiated_{};
    std::atomic_bool quest_reactions_negotiated_{};
    std::atomic_bool rechat_scene_negotiated_{};
    bool halt_attempted_{};
    std::optional<InterruptedTurn> active_turn_anchor_;
    std::atomic_bool halted_{};
};

}  // namespace synth::client
