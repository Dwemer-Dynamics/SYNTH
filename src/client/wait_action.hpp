#pragma once

#include "protocol_native/v1_codec.hpp"
#include "runtime/fallout_runtime.hpp"
#include <charconv>
#include <cstdio>
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
        capabilities.push_back("action.wait_verified");
        capabilities.push_back("context.wait_state");
    }
}

// Reuse the existing bounded action-detail receipt, as ReadQuests does. A native
// queue acceptance or another actor's observation cannot become a remembered success.
[[nodiscard]] inline std::string verified_wait_result(const protocol_native::Action& action,
    const runtime::RuntimeActionRequest& request, const runtime::RuntimeWaitObservation& observed) {
    if (!observed.confirms_success(request) || action.action_id!=request.action_id ||
        action.capability!="action."+action.name ||
        action.name!=(request.name==runtime::RuntimeActionName::wait_here?"wait_here":"release_wait") ||
        action.actor.playthrough_id!=request.actor.playthrough_id)
        throw std::invalid_argument{"wait result has no owned physical postcondition"};
    if(action.name=="wait_here" && ((observed.owned_package&0x00ffffffU)<0x900U ||
        (observed.owned_package&0x00ffffffU)>0x9ffU || (observed.owned_package>>24)==0xffU))
        throw std::invalid_argument{"wait result does not identify an authored SYNTH wait package"};
    std::uint32_t form{};
    const auto& id=action.actor.form_id;
    if(id.size()!=10||!id.starts_with("0x"))throw std::invalid_argument{"invalid wait result actor"};
    const auto parsed=std::from_chars(id.data()+2,id.data()+id.size(),form,16);
    if(parsed.ec!=std::errc{}||parsed.ptr!=id.data()+id.size()||form!=request.actor.form_id||
        !std::ranges::equal(action.actor.origin_plugin,request.actor.origin_plugin,[](unsigned char a,unsigned char b) {
            const auto fold=[](unsigned char c){return c>='A'&&c<='Z'?c+('a'-'A'):c;};return fold(a)==fold(b);
        }))throw std::invalid_argument{"wait result actor identity differs"};
    char package[11]{};
    std::snprintf(package,sizeof(package),"0x%08X",observed.owned_package);
    auto encoded=json::write(json::Value{protocol_native::object({
        {"schema","synth.action.wait.result.v1"},{"action_id",action.action_id},{"action",action.name},
        {"actor",protocol_native::identity_value(action.actor)},{"owned_hold",observed.owned_hold},
        {"restrained",observed.restrained},{"owned_package",package},{"stationary",observed.stationary},
        {"lease_ms",90000},{"reload_policy","release_owned_hold"}
    })});
    if(encoded.size()>1024)throw std::invalid_argument{"wait physical receipt exceeds detail budget"};
    return encoded;
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
