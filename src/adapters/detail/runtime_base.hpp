#pragma once

#include "adapters/adapter_descriptor.hpp"
#include "runtime/fallout_runtime.hpp"

#include <functional>
#include <array>
#include <limits>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace synth::adapters::detail {

struct CapturedRuntimeValues final {
    core::WorldPose player_pose;
    std::optional<core::WorldPose> hmd_pose;
    std::optional<core::WorldPose> left_controller_pose;
    std::optional<core::WorldPose> right_controller_pose;
    core::ActorSnapshot player;
    std::vector<core::ActorSnapshot> actors;
    std::uint64_t game_time_ticks{};
    std::optional<core::WorldState> world;
    std::vector<core::LoadedPlugin> loaded_plugins;
    std::vector<core::QuestSnapshot> active_quests;
    std::vector<core::NearbyItemSnapshot> nearby_items;
    std::vector<core::PointOfInterestSnapshot> points_of_interest;
    std::string active_quests_observation;
    std::string nearby_items_observation;
    std::string points_of_interest_observation;
    std::string actors_observation;
    std::optional<core::ActorSnapshot> picked_actor{};
    bool picked_observed{};
};

using CapturePump =
    std::function<CapturedRuntimeValues(core::SnapshotClock::time_point,
                                        runtime::RuntimeCapturePurpose)>;
using PresentationPump = std::function<void(std::string_view)>;
using ActorDetailPump = std::function<std::optional<core::ActorSnapshot>(const core::ActorSnapshot&)>;
using RequestedActorPump = std::function<std::optional<core::ActorSnapshot>(std::uint32_t, const std::string&)>;
using ActionPump =
    std::function<runtime::RuntimeActionResult(const runtime::RuntimeActionRequest&)>;

class RuntimeBase : public runtime::IFalloutRuntime {
public:
    // A serialized, non-yielding game-pump scope may share copied scene values.
    // Neither engine references nor per-request actor enrichment enter this cache.
    class CaptureBatch final {
    public:
        explicit CaptureBatch(RuntimeBase& runtime) : runtime_{runtime} {
            runtime_.assert_game_thread();
            if (runtime_.capture_batch_active_) throw std::logic_error{"capture batches cannot nest"};
            runtime_.cached_snapshot_.reset();
            runtime_.capture_batch_active_ = true;
        }
        CaptureBatch(const CaptureBatch&) = delete;
        CaptureBatch& operator=(const CaptureBatch&) = delete;
        ~CaptureBatch() {
            runtime_.cached_snapshot_.reset();
            runtime_.capture_batch_active_ = false;
        }
    private:
        RuntimeBase& runtime_;
    };

    [[nodiscard]] CaptureBatch capture_batch() { return CaptureBatch{*this}; }

    RuntimeBase(const AdapterDescriptor& descriptor,
                CapturePump capture_pump,
                PresentationPump presentation_pump,
                ActionPump action_pump)
        : descriptor_{descriptor},
          game_thread_{std::this_thread::get_id()},
          capture_pump_{std::move(capture_pump)},
          presentation_pump_{std::move(presentation_pump)},
          action_pump_{std::move(action_pump)} {
        if (!capture_pump_ || !presentation_pump_ || !action_pump_) {
            throw std::invalid_argument{"adapter pumps must be configured"};
        }
    }

    [[nodiscard]] core::Game game() const noexcept final { return descriptor_.game; }
    [[nodiscard]] core::RuntimeVariant variant() const noexcept final { return descriptor_.variant; }
    [[nodiscard]] core::RuntimeGeneration generation() const noexcept final {
        return generation_.current();
    }
    [[nodiscard]] bool is_game_thread() const noexcept final {
        std::scoped_lock lock{game_thread_mutex_};
        return std::this_thread::get_id() == game_thread_;
    }
    void assert_game_thread() const final {
        if (!is_game_thread()) {
            throw std::logic_error{"adapter operation must execute on the game thread"};
        }
    }
    [[nodiscard]] std::span<const core::RuntimeCapability> capabilities() const noexcept final {
        return descriptor_.capabilities;
    }

    // F4SE's permanent game-task lane is serialized but may migrate between
    // Bethesda worker threads. Rebind only at the start of that trusted lane so
    // arbitrary background workers remain unable to touch Fallout state.
    void rebind_game_thread() noexcept {
        std::scoped_lock lock{game_thread_mutex_};
        game_thread_ = std::this_thread::get_id();
    }

    [[nodiscard]] std::shared_ptr<const core::RuntimeSnapshot> capture_snapshot(
        core::SnapshotClock::time_point now,
        runtime::RuntimeCapturePurpose purpose = runtime::RuntimeCapturePurpose::dialogue) final {
        assert_game_thread();
        const auto isolated = purpose == runtime::RuntimeCapturePurpose::actor_events ||
                              purpose == runtime::RuntimeCapturePurpose::player_inventory;
        if (isolated && variant() != core::RuntimeVariant::flat)
            throw std::invalid_argument{"isolated native capture requires flat runtime"};
        if (capture_batch_active_ && cached_snapshot_ &&
            capture_epoch_current() &&
            cached_snapshot_->generation() == generation() && cached_snapshot_->frame() == frame_ &&
            purpose != runtime::RuntimeCapturePurpose::bootstrap && !isolated &&
            (purpose == cached_purpose_ ||
             (cached_purpose_ == runtime::RuntimeCapturePurpose::dialogue &&
              purpose == runtime::RuntimeCapturePurpose::background))) {
            // Keep the original capture time, including VR pose expiry. A later
            // consumer in this pump must not make these same values look newer.
            return cached_snapshot_;
        }
        cached_snapshot_.reset();
        const auto capture_generation = generation();
        const auto native_epoch = capture_epoch_reader_ ? capture_epoch_reader_() : 0;
        auto captured = capture_pump_(now, purpose);
        if (generation() != capture_generation)
            throw std::runtime_error{"capture crossed a runtime generation boundary"};
        if (capture_epoch_reader_ && capture_epoch_reader_() != native_epoch)
            throw std::runtime_error{"capture crossed a native observation boundary"};
        capture_epoch_ = native_epoch;
        const auto frame = ++frame_;
        std::optional<core::TimedPose> hmd_pose;
        std::optional<core::TimedPose> left_controller_pose;
        std::optional<core::TimedPose> right_controller_pose;
        if (captured.hmd_pose) {
            hmd_pose.emplace(std::move(*captured.hmd_pose), now,
                             std::chrono::milliseconds{50}, frame);
        }
        if (captured.left_controller_pose) {
            left_controller_pose.emplace(std::move(*captured.left_controller_pose), now,
                                         std::chrono::milliseconds{50}, frame);
        }
        if (captured.right_controller_pose) {
            right_controller_pose.emplace(std::move(*captured.right_controller_pose), now,
                                          std::chrono::milliseconds{50}, frame);
        }
        auto snapshot = std::make_shared<const core::RuntimeSnapshot>(
            game(), variant(), capture_generation, frame, now, std::move(captured.player_pose),
            std::move(hmd_pose), std::move(left_controller_pose),
            std::move(right_controller_pose), std::move(captured.player),
            std::move(captured.actors), captured.game_time_ticks, std::move(captured.world),
            std::move(captured.loaded_plugins), std::move(captured.active_quests),
            std::move(captured.nearby_items), std::move(captured.points_of_interest),
            std::move(captured.active_quests_observation), std::move(captured.nearby_items_observation),
            std::move(captured.points_of_interest_observation), std::move(captured.actors_observation));
        if (captured.picked_observed)
            snapshot = std::make_shared<const core::RuntimeSnapshot>(snapshot->with_picked_actor(captured.picked_actor));
        if (capture_batch_active_ && purpose != runtime::RuntimeCapturePurpose::bootstrap && !isolated) {
            cached_purpose_ = purpose;
            cached_snapshot_ = snapshot;
        }
        return snapshot;
    }

    void set_requested_actor_pump(RequestedActorPump pump) {
        assert_game_thread();
        requested_actor_pump_ = std::move(pump);
    }

    // Direct lookup and shallow capture happen in the same guarded frame as the rest of this scene.
    [[nodiscard]] std::shared_ptr<const core::RuntimeSnapshot> capture_actor_snapshot(
        core::SnapshotClock::time_point now, std::uint32_t actor_form_id) final {
        assert_game_thread();
        if (!requested_actor_pump_ || actor_form_id == 0 || actor_form_id == 0x14) return {};
        const auto expected_generation = generation();
        const auto snapshot = capture_snapshot(now);
        if (generation() != expected_generation || !capture_epoch_current()) return {};
        auto observed = requested_actor_pump_(actor_form_id, snapshot->player().playthrough_id());
        if (!observed || observed->form_id() != actor_form_id || generation() != expected_generation || !capture_epoch_current() ||
            snapshot->frame() != frame_) return {};
        return std::make_shared<const core::RuntimeSnapshot>(snapshot->with_requested_actor(*observed));
    }

    void set_actor_detail_pump(ActorDetailPump pump) {
        assert_game_thread();
        actor_detail_pump_ = std::move(pump);
    }

    void set_player_inventory_pump(ActorDetailPump pump) {
        assert_game_thread();
        player_inventory_pump_ = std::move(pump);
    }

    // The flat host supplies its immediate native observation stamp, including invalidation before generation advances.
    void set_capture_epoch_reader(std::function<std::uint64_t()> reader) {
        assert_game_thread();
        if (frame_ != 0) throw std::logic_error{"capture epoch reader must be configured before capture"};
        capture_epoch_reader_ = std::move(reader);
    }

    // Enrichment belongs to the current capture frame; it never resolves an old worker-owned snapshot anew.
    [[nodiscard]] std::shared_ptr<const core::RuntimeSnapshot> enrich_actor_snapshot(
        const std::shared_ptr<const core::RuntimeSnapshot>& snapshot, std::uint32_t actor_form_id) final {
        assert_game_thread();
        if (!snapshot || snapshot->game() != game() || snapshot->variant() != variant() ||
            snapshot->generation() != generation() || snapshot->frame() != frame_ || !capture_epoch_current() || !actor_detail_pump_)
            return {};
        const auto found = std::ranges::find_if(snapshot->actors(), [&](const auto& actor) {
            return actor.form_id() == actor_form_id && actor.playthrough_id() == snapshot->player().playthrough_id();
        });
        if (found == snapshot->actors().end()) return {};
        auto observed = actor_detail_pump_(*found);
        if (!observed || snapshot->generation() != generation() || snapshot->frame() != frame_ || !capture_epoch_current()) return {};
        auto enriched = snapshot->with_actor_details(actor_form_id, *observed);
        if (player_inventory_pump_ && snapshot->player().inventory_observation() == "unavailable") {
            auto player = player_inventory_pump_(snapshot->player());
            if (!player || snapshot->generation() != generation() || snapshot->frame() != frame_ || !capture_epoch_current()) return {};
            enriched = enriched.with_player_inventory(*player);
        }
        return std::make_shared<const core::RuntimeSnapshot>(std::move(enriched));
    }

    void present_notification(std::string_view message) final {
        assert_game_thread();
        presentation_pump_(message);
    }

    // Reserve one actor or a transfer pair for a game-thread operation, including cleanup across frames.
    // FormID conflicts are conservative: changing plugin/playthrough text cannot bypass a live reservation.
    [[nodiscard]] std::optional<std::uint64_t> reserve_action_actors(
        std::span<const std::uint32_t> actors, core::RuntimeGeneration owner_generation) {
        assert_game_thread();
        if (owner_generation != generation() || actors.empty() || actors.size() > 2 ||
            actors[0] == 0 || (actors.size() == 2 && (actors[1] == 0 || actors[0] == actors[1])) ||
            next_action_reservation_ == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
        ActionReservation* available{};
        for (auto& entry : action_reservations_) {
            if (entry.id == 0) { if (!available) available = &entry; continue; }
            for (const auto actor : actors)
                if (actor == entry.actors[0] || actor == entry.actors[1]) return std::nullopt;
        }
        if (!available) return std::nullopt;
        *available = {++next_action_reservation_, owner_generation, {actors[0], actors.size() == 2 ? actors[1] : 0}};
        return available->id;
    }

    // A stale completion cannot release a new timeline's actor. Cancellation alone does not release cleanup ownership.
    [[nodiscard]] bool release_action_actors(std::uint64_t id, core::RuntimeGeneration owner_generation) {
        assert_game_thread();
        if (id == 0 || owner_generation != generation()) return false;
        for (auto& entry : action_reservations_) {
            if (entry.id == id && entry.generation == owner_generation) { entry = {}; return true; }
        }
        return false;
    }

    [[nodiscard]] runtime::RuntimeActionResult execute_action(
        const runtime::RuntimeActionRequest& request) final {
        assert_game_thread();
        const auto owner_generation = generation();
        const std::array actors{request.actor.form_id, request.recipient ? request.recipient->form_id : 0U};
        const auto reservation = reserve_action_actors(std::span{actors}.first(request.recipient ? 2 : 1), owner_generation);
        if (!reservation) return {runtime::RuntimeActionStatus::rejected, "action actors are invalid or reserved by another operation"};
        // An action can mutate the scene, even when it subsequently fails.
        cached_snapshot_.reset();
        try {
            auto result = action_pump_(request);
            cached_snapshot_.reset();
            (void)release_action_actors(*reservation, owner_generation);
            return result;
        } catch (...) {
            cached_snapshot_.reset();
            (void)release_action_actors(*reservation, owner_generation);
            throw;
        }
    }

    void set_facing_pump(std::function<runtime::RuntimeActionResult(const runtime::RuntimeFacingRequest&)> pump) {
        assert_game_thread();
        facing_pump_ = std::move(pump);
    }

    void set_speech_pump(std::function<void(const std::optional<runtime::RuntimeSpeechFrame>&, bool)> pump) {
        assert_game_thread();
        speech_pump_ = std::move(pump);
    }

    void animate_speech(const std::optional<runtime::RuntimeSpeechFrame>& frame, bool discard = false) final {
        assert_game_thread();
        if (!speech_pump_) return;
        if (discard) {
            speech_pump_(std::nullopt, true);
            return;
        }
        if (frame && (frame->generation != generation().value() ||
            frame->cancellation.generation() != generation() || frame->cancellation.is_cancelled() ||
            frame->actor_id == 0 || frame->utterance_id.empty())) {
            // A late stale frame must not clear another current utterance's ownership.
            return;
        }
        speech_pump_(frame, false);
    }

    [[nodiscard]] runtime::RuntimeActionResult face_speech_listener(
        const runtime::RuntimeFacingRequest& request) final {
        assert_game_thread();
        if (request.generation != generation().value() || request.cancellation.generation() != generation() ||
            request.cancellation.is_cancelled() || request.context_sequence == 0 ||
            request.request_id.empty() || request.turn_id.empty() || request.utterance_id.empty())
            return {runtime::RuntimeActionStatus::unavailable, "speech facing owner is no longer current"};
        if (!facing_pump_) return {runtime::RuntimeActionStatus::unsupported_runtime, "speech facing adapter unavailable"};
        const auto owner_generation = generation();
        const std::array actors{request.speaker.form_id}; // Only the speaker is rotated; the listener is read-only.
        const auto reservation = reserve_action_actors(actors, owner_generation);
        if (!reservation) return {runtime::RuntimeActionStatus::rejected, "speech facing actor is reserved by another operation"};
        cached_snapshot_.reset();
        try {
            auto result = facing_pump_(request);
            cached_snapshot_.reset();
            (void)release_action_actors(*reservation, owner_generation);
            return result;
        } catch (...) {
            cached_snapshot_.reset();
            (void)release_action_actors(*reservation, owner_generation);
            throw;
        }
    }

    [[nodiscard]] core::RuntimeGeneration invalidate() {
        assert_game_thread();
        if (speech_pump_) speech_pump_(std::nullopt, true);
        cached_snapshot_.reset();
        frame_ = 0;
        const auto next = generation_.advance();
        action_reservations_.fill({}); // Copied control state only; never native package cleanup.
        return next;
    }

protected:
    [[nodiscard]] static core::WorldPose origin_pose() {
        return {
            core::Vec3{},
            core::UnitVector3::from({0.0, 1.0, 0.0}),
            core::UnitVector3::from({0.0, 0.0, 1.0}),
        };
    }

private:
    struct ActionReservation {
        std::uint64_t id{};
        core::RuntimeGeneration generation;
        std::array<std::uint32_t, 2> actors{};
    };
    std::array<ActionReservation, 64> action_reservations_{};
    std::uint64_t next_action_reservation_{};
    [[nodiscard]] bool capture_epoch_current() const {
        return !capture_epoch_reader_ || capture_epoch_reader_() == capture_epoch_;
    }

    const AdapterDescriptor& descriptor_;
    mutable std::mutex game_thread_mutex_;
    std::thread::id game_thread_;
    CapturePump capture_pump_;
    PresentationPump presentation_pump_;
    ActionPump action_pump_;
    std::function<runtime::RuntimeActionResult(const runtime::RuntimeFacingRequest&)> facing_pump_;
    std::function<void(const std::optional<runtime::RuntimeSpeechFrame>&, bool)> speech_pump_;
    ActorDetailPump actor_detail_pump_;
    ActorDetailPump player_inventory_pump_;
    std::function<std::uint64_t()> capture_epoch_reader_;
    std::uint64_t capture_epoch_{};
    RequestedActorPump requested_actor_pump_;
    core::GenerationClock generation_;
    std::uint64_t frame_{};
    bool capture_batch_active_{};
    runtime::RuntimeCapturePurpose cached_purpose_{runtime::RuntimeCapturePurpose::bootstrap};
    std::shared_ptr<const core::RuntimeSnapshot> cached_snapshot_;
};

}  // namespace synth::adapters::detail
