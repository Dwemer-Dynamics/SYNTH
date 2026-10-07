#pragma once

#include "fallout_runtime.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace synth::runtime {

class FakeFalloutRuntime final : public IFalloutRuntime {
public:
    explicit FakeFalloutRuntime(core::RuntimeVariant variant)
        : variant_{variant}, game_thread_{std::this_thread::get_id()} {
        capabilities_.push_back(core::RuntimeCapability::notifications);
        if (variant == core::RuntimeVariant::vr) {
            capabilities_.push_back(core::RuntimeCapability::hmd_pose);
        }
    }

    [[nodiscard]] core::Game game() const noexcept override { return core::Game::fallout4; }
    [[nodiscard]] core::RuntimeVariant variant() const noexcept override { return variant_; }
    [[nodiscard]] core::RuntimeGeneration generation() const noexcept override {
        return generation_clock_.current();
    }

    [[nodiscard]] bool is_game_thread() const noexcept override {
        return std::this_thread::get_id() == game_thread_;
    }

    void assert_game_thread() const override {
        if (!is_game_thread()) {
            throw std::logic_error{"Fallout runtime operation must execute on the game thread"};
        }
    }

    [[nodiscard]] std::span<const core::RuntimeCapability> capabilities() const noexcept override {
        return capabilities_;
    }

    [[nodiscard]] std::shared_ptr<const core::RuntimeSnapshot> capture_snapshot(
        core::SnapshotClock::time_point now,
        RuntimeCapturePurpose = RuntimeCapturePurpose::dialogue) override {
        assert_game_thread();
        std::scoped_lock lock{state_mutex_};
        return std::make_shared<const core::RuntimeSnapshot>(
            game(),
            variant_,
            generation_clock_.current(),
            ++frame_,
            now,
            player_pose_,
            variant_ == core::RuntimeVariant::vr ? hmd_pose_ : std::nullopt,
            std::nullopt,
            std::nullopt,
            player_,
            actors_);
    }

    [[nodiscard]] std::shared_ptr<const core::RuntimeSnapshot> capture_actor_snapshot(
        core::SnapshotClock::time_point, std::uint32_t) override {
        assert_game_thread();
        return {}; // The fake has no independent loaded-actor lookup.
    }

    [[nodiscard]] std::shared_ptr<const core::RuntimeSnapshot> enrich_actor_snapshot(
        const std::shared_ptr<const core::RuntimeSnapshot>&, std::uint32_t) override {
        assert_game_thread();
        return {}; // The fake has no independent live detail source.
    }

    void present_notification(std::string_view message) override {
        assert_game_thread();
        std::scoped_lock lock{state_mutex_};
        notifications_.emplace_back(message);
    }

    [[nodiscard]] RuntimeActionResult execute_action(
        const RuntimeActionRequest&) override {
        assert_game_thread();
        return {RuntimeActionStatus::unsupported_runtime,
                "fake runtime does not execute Fallout actions"};
    }

    [[nodiscard]] core::RuntimeGeneration advance_generation() {
        assert_game_thread();
        std::scoped_lock lock{state_mutex_};
        player_pose_ = origin_pose();
        hmd_pose_.reset();
        actors_.clear();
        return generation_clock_.advance();
    }

    [[nodiscard]] RuntimeActionResult face_speech_listener(const RuntimeFacingRequest&) override {
        assert_game_thread();
        return {RuntimeActionStatus::unsupported_runtime, "fake runtime does not rotate Fallout actors"};
    }

    void set_player_pose(core::WorldPose pose) {
        assert_game_thread();
        std::scoped_lock lock{state_mutex_};
        player_pose_ = std::move(pose);
    }

    void set_hmd_pose(core::WorldPose pose,
                      core::SnapshotClock::time_point captured_at,
                      core::SnapshotClock::duration maximum_age) {
        assert_game_thread();
        if (variant_ != core::RuntimeVariant::vr) {
            throw std::logic_error{"flat runtime has no HMD pose"};
        }
        std::scoped_lock lock{state_mutex_};
        hmd_pose_.emplace(std::move(pose), captured_at, maximum_age, frame_);
    }

    void clear_hmd_pose() {
        assert_game_thread();
        std::scoped_lock lock{state_mutex_};
        hmd_pose_.reset();
    }

    void set_actors(std::vector<core::ActorSnapshot> actors) {
        assert_game_thread();
        std::scoped_lock lock{state_mutex_};
        actors_ = std::move(actors);
    }

    [[nodiscard]] std::vector<std::string> notifications() const {
        std::scoped_lock lock{state_mutex_};
        return notifications_;
    }

private:
    static core::WorldPose origin_pose() {
        return core::WorldPose{
            core::Vec3{},
            core::UnitVector3::from({0.0, 1.0, 0.0}),
            core::UnitVector3::from({0.0, 0.0, 1.0}),
        };
    }

    core::RuntimeVariant variant_;
    std::thread::id game_thread_;
    core::GenerationClock generation_clock_;
    std::vector<core::RuntimeCapability> capabilities_;

    mutable std::mutex state_mutex_;
    std::uint64_t frame_{};
    core::WorldPose player_pose_{origin_pose()};
    std::optional<core::TimedPose> hmd_pose_;
    core::ActorSnapshot player_{0x14, "Sole Survivor", {}, "Fallout4.esm", "fake-playthrough"};
    std::vector<core::ActorSnapshot> actors_;
    std::vector<std::string> notifications_;
};

}  // namespace synth::runtime
