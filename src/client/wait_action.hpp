#pragma once

#include "protocol_native/v1_codec.hpp"
#include "runtime/fallout_runtime.hpp"
#include <charconv>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace synth::client {

class WaitActionLifetime final {
public:
    explicit WaitActionLifetime(core::CancellationToken session) : session_{session}, owner_{session} {}
    [[nodiscard]] core::CancellationToken token() {
        std::scoped_lock lock{mutex_};
        return owner_.token();
    }
    void stop() {
        std::scoped_lock lock{mutex_};
        owner_.cancel();
        const core::CancellationSource next{session_};
        owner_ = next;
    }
private:
    core::CancellationToken session_;
    core::CancellationSource owner_;
    std::mutex mutex_;
};

// Called under the action queue lock. Never evict an accepted command while this session remains alive.
class WaitActionReplayGuard final {
public:
    [[nodiscard]] bool claim(const protocol_native::Action& action) {
        if (action.action_id.empty() || action.action_id.size() > 128 || action.idempotency_key.empty() ||
            action.idempotency_key.size() > 128 || ids_.size() >= 2048 || ids_.contains(action.action_id) ||
            keys_.contains(action.idempotency_key)) return false;
        ids_.insert(action.action_id);
        keys_.insert(action.idempotency_key);
        return true;
    }
private:
    std::unordered_set<std::string> ids_, keys_;
};

inline void append_wait_capabilities(std::vector<std::string>& capabilities, core::RuntimeVariant variant) {
    if (variant == core::RuntimeVariant::flat) {
        capabilities.push_back("action.wait_here");
        capabilities.push_back("action.release_wait");
    }
}

// Execute under the requesting turn, then retain the existing bounded hold under its runtime session.
[[nodiscard]] inline runtime::RuntimeActionRequest prepare_wait_action(const core::RuntimeSnapshot& snapshot,
    const protocol_native::Action& action, const core::CancellationToken& turn,
    const core::CancellationToken& session, core::SnapshotClock::time_point deadline) {
    if (snapshot.game() != core::Game::fallout4 || snapshot.variant() != core::RuntimeVariant::flat ||
        turn.is_cancelled() || session.is_cancelled() || snapshot.generation() != turn.generation() ||
        snapshot.generation() != session.generation() || snapshot.frame() == 0 ||
        deadline <= core::SnapshotClock::now() || deadline <= snapshot.captured_at() ||
        (action.name != "wait_here" && action.name != "release_wait") || action.capability != "action." + action.name ||
        action.action_id.empty() || action.action_id.size() > 128 || action.target ||
        !action.arguments.is_object() || !action.arguments.as_object().empty())
        throw std::invalid_argument{"invalid wait owner, deadline or arguments"};
    (void)protocol_native::identity(protocol_native::identity_value(action.actor));
    std::uint32_t form_id{};
    if (action.actor.form_id.size() != 10 || !action.actor.form_id.starts_with("0x"))
        throw std::invalid_argument{"invalid wait actor FormID"};
    const auto parsed = std::from_chars(action.actor.form_id.data() + 2, action.actor.form_id.data() + 10, form_id, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != action.actor.form_id.data() + 10 || form_id == 0 ||
        form_id == 0x14 || form_id == snapshot.player().form_id() ||
        action.actor.playthrough_id != snapshot.player().playthrough_id())
        throw std::invalid_argument{"wait requires an NPC in this playthrough"};
    const core::ActorSnapshot* performer{};
    for (const auto& actor : snapshot.actors()) {
        if (actor.form_id() != form_id) continue;
        if (performer || actor.origin_plugin() != action.actor.origin_plugin ||
            actor.playthrough_id() != action.actor.playthrough_id || !actor.alive() || actor.disabled())
            throw std::invalid_argument{"unavailable, ambiguous or mismatched wait actor"};
        performer = &actor;
    }
    if (!performer) throw std::invalid_argument{"wait actor is absent from the captured scene"};
    runtime::RuntimeActionRequest request{action.name == "wait_here" ? runtime::RuntimeActionName::wait_here
            : runtime::RuntimeActionName::release_wait,
        {form_id, performer->origin_plugin(), performer->playthrough_id()}, {}, turn, deadline, action.action_id};
    request.wait_session = session;
    return request;
}

} // namespace synth::client
