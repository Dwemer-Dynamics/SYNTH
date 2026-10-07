#pragma once

#include "../core/generation.hpp"
#include "../core/cancellation.hpp"
#include "../core/runtime_identity.hpp"
#include "../core/snapshot.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace synth::runtime {

// Bootstrap captures contain only the stable identity required to connect.
// Dialogue captures add current actors, while background captures may also
// refresh one cached environmental domain.
enum class RuntimeCapturePurpose : unsigned char {
    bootstrap,
    dialogue,
    background,
    actor_events,
    player_inventory,
};

enum class RuntimeActionName : unsigned char {
    sheathe_weapon,
    equip_item,
    unequip_item,
    consume,
    give_item,
    give_caps,
    take_caps,
    wait_here,
    release_wait,
};

enum class RuntimeActionStatus : unsigned char {
    succeeded,
    rejected,
    unavailable,
    failed,
    unsupported_runtime,
    timed_out,
};

struct RuntimeActorIdentity final {
    std::uint32_t form_id{};
    std::string origin_plugin;
    std::string playthrough_id;
};

// Immutable preparation for the future multi-frame pickup adapter; not a capability or dispatch authorization.
struct RuntimePickupRequest final {
    RuntimeActorIdentity actor;
    core::NearbyItemSnapshot item;
    core::CancellationToken cancellation;
    core::SnapshotClock::time_point deadline{}, captured_at{};
    std::uint64_t context_sequence{};
    std::string action_id;
};

struct RuntimeActionRequest final {
    RuntimeActionName name{};
    // For paired transfers this is the inventory donor, not necessarily the dialogue performer.
    RuntimeActorIdentity actor;
    std::optional<core::InventoryItemSnapshot> item;
    core::CancellationToken cancellation;
    core::SnapshotClock::time_point deadline{};
    std::string action_id;
    std::optional<RuntimeActorIdentity> recipient;
    std::uint32_t amount{1};
};

// Copied after action execution, never an engine pointer or a replacement for the original scene snapshot.
struct RuntimeInventoryObservation final {
    core::RuntimeGeneration generation;
    RuntimeActorIdentity actor;
    std::vector<core::InventoryItemSnapshot> items;
    std::string observation{"unavailable"};
    std::string action_id;

    [[nodiscard]] bool valid() const noexcept {
        return generation.valid() && !action_id.empty() && action_id.size() <= 128 &&
            actor.form_id != 0 && !actor.origin_plugin.empty() &&
            actor.origin_plugin.size() <= 255 && actor.origin_plugin.find_first_of("/\\") == std::string::npos &&
            (actor.origin_plugin.ends_with(".esm") || actor.origin_plugin.ends_with(".esp") || actor.origin_plugin.ends_with(".esl")) &&
            !actor.playthrough_id.empty() && actor.playthrough_id.size() <= 128 && items.size() <= 512 &&
            std::ranges::all_of(items, &core::InventoryItemSnapshot::valid) &&
            (observation == "complete" || observation == "partial" || observation == "unavailable") &&
            (observation != "unavailable" || items.empty());
    }

    [[nodiscard]] bool valid_for(const RuntimeActionRequest& request) const noexcept {
        return valid() && action_id == request.action_id && !request.cancellation.is_cancelled() && generation == request.cancellation.generation() &&
            actor.form_id == request.actor.form_id && actor.origin_plugin == request.actor.origin_plugin &&
            actor.playthrough_id == request.actor.playthrough_id;
    }
};

// Two inventory observations travel together; neither may be attributed to a different actor or turn.
struct RuntimeTransferObservation final {
    RuntimeInventoryObservation donor;
    RuntimeInventoryObservation recipient;

    [[nodiscard]] bool valid_for(const RuntimeActionRequest& request) const noexcept {
        if ((request.name != RuntimeActionName::give_item && request.name != RuntimeActionName::give_caps &&
             request.name != RuntimeActionName::take_caps) || !request.recipient ||
            (request.name == RuntimeActionName::take_caps ? request.actor.form_id != 0x14 : request.actor.form_id == 0x14) ||
            request.actor.form_id == request.recipient->form_id ||
            request.actor.playthrough_id != request.recipient->playthrough_id || !donor.valid_for(request)) return false;
        return recipient.valid() && recipient.generation == request.cancellation.generation() &&
            recipient.action_id == request.action_id && recipient.actor.form_id == request.recipient->form_id &&
            recipient.actor.origin_plugin == request.recipient->origin_plugin &&
            recipient.actor.playthrough_id == request.recipient->playthrough_id;
    }
};

struct RuntimeActionResult final {
    RuntimeActionStatus status{RuntimeActionStatus::failed};
    std::string detail;
    std::optional<RuntimeInventoryObservation> inventory;
    std::optional<RuntimeTransferObservation> transfer;
};

// Immutable, response-owned identities; no engine pointers or mutable target lookup.
struct RuntimeFacingRequest final {
    std::uint64_t generation{}, context_sequence{};
    std::string request_id, turn_id, utterance_id;
    RuntimeActorIdentity speaker, listener;
    core::CancellationToken cancellation;
};

// Copied speech performance for the currently audible utterance; never an engine address.
struct RuntimeSpeechFrame final {
    std::uint64_t generation{};
    std::uint32_t actor_id{};
    std::string utterance_id;
    core::CancellationToken cancellation;
    std::array<float, 7> mouth{};
};

class IFalloutRuntime {
public:
    virtual ~IFalloutRuntime() = default;

    [[nodiscard]] virtual core::Game game() const noexcept = 0;
    [[nodiscard]] virtual core::RuntimeVariant variant() const noexcept = 0;
    [[nodiscard]] virtual core::RuntimeGeneration generation() const noexcept = 0;
    [[nodiscard]] virtual bool is_game_thread() const noexcept = 0;
    virtual void assert_game_thread() const = 0;

    [[nodiscard]] virtual std::span<const core::RuntimeCapability> capabilities() const noexcept = 0;
    [[nodiscard]] virtual std::shared_ptr<const core::RuntimeSnapshot> capture_snapshot(
        core::SnapshotClock::time_point now,
        RuntimeCapturePurpose purpose = RuntimeCapturePurpose::dialogue) = 0;
    [[nodiscard]] virtual RuntimeActionResult execute_action(
        const RuntimeActionRequest& request) = 0;
    [[nodiscard]] virtual RuntimeActionResult face_speech_listener(
        const RuntimeFacingRequest& request) = 0;
    [[nodiscard]] virtual std::shared_ptr<const core::RuntimeSnapshot> capture_actor_snapshot(
        core::SnapshotClock::time_point now, std::uint32_t actor_form_id) = 0;
    [[nodiscard]] virtual std::shared_ptr<const core::RuntimeSnapshot> enrich_actor_snapshot(
        const std::shared_ptr<const core::RuntimeSnapshot>& snapshot, std::uint32_t actor_form_id) = 0;
    virtual void present_notification(std::string_view message) = 0;
    // discard=true invalidates ownership without accessing possibly unloaded game objects.
    virtual void animate_speech(const std::optional<RuntimeSpeechFrame>&, bool = false) {}
};

}  // namespace synth::runtime
