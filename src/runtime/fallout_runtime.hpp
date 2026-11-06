#pragma once

#include "../core/generation.hpp"
#include "../core/cancellation.hpp"
#include "../core/runtime_identity.hpp"
#include "../core/snapshot.hpp"
#include "../radio/diagnostic.hpp"

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
    take_item,
    open_inventory,
    wait_here,
    release_wait,
    attack,
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

enum class RuntimeSocialVisitStatus : unsigned char { pending, arrived, released, unavailable, failed };
struct RuntimeSocialVisitRequest final {
    std::string task_id, execution_id;
    RuntimeActorIdentity actor, listener;
    core::CancellationToken cancellation;
    core::SnapshotClock::time_point deadline{};
    bool release{};
    [[nodiscard]] bool valid_ids() const noexcept {
        const auto id=[](std::string_view value) {
            const auto alphanumeric=[](unsigned char ch) {
                return (ch>='a' && ch<='z') || (ch>='A' && ch<='Z') || (ch>='0' && ch<='9');
            };
            return !value.empty() && value.size()<=128 && alphanumeric(value.front()) && std::ranges::all_of(value,[&](unsigned char ch) {
                return alphanumeric(ch) || ch=='-' || ch=='_' || ch==':' || ch=='.';
            });
        };
        const auto actor_id=[](const RuntimeActorIdentity& value) {
            return value.form_id!=0 && value.form_id!=0x14 && !value.origin_plugin.empty() && value.origin_plugin.size()<=255 &&
                value.origin_plugin.find_first_of("/\\") == std::string::npos && !value.playthrough_id.empty() && value.playthrough_id.size()<=128;
        };
        return id(task_id) && id(execution_id) && actor_id(actor) && actor_id(listener) &&
            actor.form_id!=listener.form_id && actor.playthrough_id==listener.playthrough_id;
    }
    [[nodiscard]] bool same_execution(const RuntimeSocialVisitRequest& other) const noexcept {
        const auto same_actor=[](const RuntimeActorIdentity& a,const RuntimeActorIdentity& b) {
            const auto fold=[](unsigned char ch) {return ch>='A' && ch<='Z'?ch+('a'-'A'):ch;};
            return a.form_id==b.form_id && a.playthrough_id==b.playthrough_id &&
                std::ranges::equal(a.origin_plugin,b.origin_plugin,[&](auto x,auto y){return fold(x)==fold(y);});
        };
        return valid_ids() && other.valid_ids() && task_id==other.task_id && execution_id==other.execution_id &&
            same_actor(actor,other.actor) && same_actor(listener,other.listener) && cancellation.generation()==other.cancellation.generation();
    }

};
struct RuntimeSocialVisitResult final {
    RuntimeSocialVisitStatus status{RuntimeSocialVisitStatus::unavailable};
    std::string task_id, execution_id, detail_code;
    double distance{};
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
    // Wait execution is turn-owned; an accepted hold survives later input until Stop/load or its existing timeout.
    std::optional<core::CancellationToken> wait_session;
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
        return valid_for(request, request.actor);
    }

    [[nodiscard]] bool valid_for(const RuntimeActionRequest& request, const RuntimeActorIdentity& owner) const noexcept {
        return valid() && action_id == request.action_id && !request.cancellation.is_cancelled() &&
            generation == request.cancellation.generation() && actor.form_id == owner.form_id &&
            actor.origin_plugin == owner.origin_plugin && actor.playthrough_id == owner.playthrough_id;
    }
};

// Two inventory observations travel together; neither may be attributed to a different actor or turn.
struct RuntimeTransferObservation final {
    RuntimeInventoryObservation donor;
    RuntimeInventoryObservation recipient;

    [[nodiscard]] bool valid_for(const RuntimeActionRequest& request) const noexcept {
        const bool gift_menu = request.name == RuntimeActionName::open_inventory;
        const bool player_donates = gift_menu || request.name == RuntimeActionName::take_caps ||
            request.name == RuntimeActionName::take_item;
        if ((!player_donates && request.name != RuntimeActionName::give_item &&
             request.name != RuntimeActionName::give_caps) || !request.recipient) return false;
        // The NPC operates the gift menu. Its inventory receives items from the player.
        const auto& donor_owner = gift_menu ? *request.recipient : request.actor;
        const auto& recipient_owner = gift_menu ? request.actor : *request.recipient;
        return (donor_owner.form_id == 0x14) == player_donates &&
            donor_owner.form_id != recipient_owner.form_id &&
            donor_owner.playthrough_id == recipient_owner.playthrough_id &&
            donor.valid_for(request, donor_owner) && recipient.valid_for(request, recipient_owner);
    }
};

// Current physical wait evidence. Queue admission or an unowned restraint is never a successful hold.
struct RuntimeWaitObservation final {
    core::RuntimeGeneration generation;
    RuntimeActorIdentity actor;
    std::string action_id;
    RuntimeActionName name{};
    bool owned_hold{}, restrained{};
    std::uint32_t owned_package{};
    bool stationary{};
    [[nodiscard]] bool valid_for(const RuntimeActionRequest& request) const noexcept {
        const auto same_plugin=[](std::string_view a,std::string_view b) {
            return std::ranges::equal(a,b,[](unsigned char x,unsigned char y) {
                const auto fold=[](unsigned char c){return c>='A'&&c<='Z'?c+('a'-'A'):c;};
                return fold(x)==fold(y);
            });
        };
        return (name==RuntimeActionName::wait_here || name==RuntimeActionName::release_wait) && name==request.name &&
            generation.valid() && generation==request.cancellation.generation() && !request.cancellation.is_cancelled() && !action_id.empty() && action_id.size()<=128 &&
            action_id==request.action_id && actor.form_id!=0 && actor.form_id!=0x14 && actor.form_id==request.actor.form_id &&
            !actor.origin_plugin.empty() && same_plugin(actor.origin_plugin,request.actor.origin_plugin) &&
            !actor.playthrough_id.empty() && actor.playthrough_id==request.actor.playthrough_id;
    }
    [[nodiscard]] bool confirms_success(const RuntimeActionRequest& request) const noexcept {
        return valid_for(request) && (name==RuntimeActionName::wait_here ? owned_hold && owned_package!=0 && stationary : !owned_hold && owned_package==0);
    }
};

struct RuntimeActionResult final {
    RuntimeActionStatus status{RuntimeActionStatus::failed};
    std::string detail;
    std::optional<RuntimeInventoryObservation> inventory;
    std::optional<RuntimeTransferObservation> transfer;
    // Async script admission is not completion; the worker polls again off the game thread.
    bool pending{};
    std::optional<RuntimeWaitObservation> wait{};
    [[nodiscard]] bool verified_wait_for(const RuntimeActionRequest& request) const noexcept {
        return !pending && status==RuntimeActionStatus::succeeded && wait && wait->confirms_success(request);
    }
};

// Immutable, response-owned identities; no engine pointers or mutable target lookup.
struct RuntimeFacingRequest final {
    std::uint64_t generation{}, context_sequence{};
    std::string request_id, turn_id, utterance_id;
    RuntimeActorIdentity speaker, listener;
    core::CancellationToken cancellation;
};

// A temporary conversation participant, owned by the exact dialogue cancellation token.
struct RuntimeConversationHold final {
    RuntimeActorIdentity actor;
    core::CancellationToken cancellation;
    // Only explicit player input may temporarily bring a calm NPC toward the player.
    bool approach_player{};
};

// Copied speech performance for the currently audible utterance; never an engine address.
struct RuntimeSpeechFrame final {
    std::uint64_t generation{};
    std::uint32_t actor_id{};
    std::string utterance_id;
    core::CancellationToken cancellation;
    std::array<float, 7> mouth{};
};

// Copied actor ownership for a complete local audio clip, including its paused frames.
struct RuntimeSpeechOwner final {
    std::uint64_t generation{};
    std::string request_id, turn_id, utterance_id;
    RuntimeActorIdentity speaker;
    core::CancellationToken cancellation;
};

// Fresh immutable coordinates for one owning clip, copied only on the game thread.
struct RuntimeSpeechSpatial final {
    core::Vec3 speaker_position;
    core::WorldPose listener_pose;
    bool interior{};
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
    [[nodiscard]] virtual RuntimeSocialVisitResult social_visit(const RuntimeSocialVisitRequest& request) {
        assert_game_thread();
        return {RuntimeSocialVisitStatus::unavailable,request.task_id,request.execution_id,"unsupported_runtime",0};
    }
    [[nodiscard]] virtual RuntimeActionResult face_speech_listener(
        const RuntimeFacingRequest& request) = 0;
    [[nodiscard]] virtual std::shared_ptr<const core::RuntimeSnapshot> capture_actor_snapshot(
        core::SnapshotClock::time_point now, std::uint32_t actor_form_id) = 0;
    [[nodiscard]] virtual std::shared_ptr<const core::RuntimeSnapshot> enrich_actor_snapshot(
        const std::shared_ptr<const core::RuntimeSnapshot>& snapshot, std::uint32_t actor_form_id) = 0;
    virtual void present_notification(std::string_view message) = 0;
    // discard=true invalidates ownership without accessing possibly unloaded game objects.
    virtual void animate_speech(const std::optional<RuntimeSpeechFrame>&, bool = false) {}
    // Non-null admits/maintains exclusive actor speech. Empty restores; discard forgets stale state.
    [[nodiscard]] virtual bool set_speech_owner(const std::optional<RuntimeSpeechOwner>& owner, bool = false) {
        assert_game_thread();
        return !owner;
    }
    [[nodiscard]] virtual std::optional<RuntimeSpeechSpatial> capture_speech_spatial(const RuntimeSpeechOwner&) {
        assert_game_thread();
        return std::nullopt;
    }
    virtual void update_conversation_holds(std::span<const RuntimeConversationHold>, bool = false) {
        assert_game_thread();
    }
    [[nodiscard]] virtual bool conversation_actions_ready() { assert_game_thread(); return true; }
    // Optional original native-radio diagnostic seam; VR and other adapters fail closed.
    [[nodiscard]] virtual radio::ProbeObservation capture_radio_probe() {
        assert_game_thread();
        return {generation()};
    }
    [[nodiscard]] virtual radio::ProbeResult execute_radio_probe(const radio::ProbeRequest&) {
        assert_game_thread();
        return {};
    }
};

}  // namespace synth::runtime
