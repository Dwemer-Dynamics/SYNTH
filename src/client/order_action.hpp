#pragma once

// Follow, Stop Following and Take a Seat orders. The server only names the order; the game applies it
// through the runtime adapter, and the order is bound to the exact reply line it rode on. Nothing here
// touches an actor: validation and receipts are engine-free, the adapter owns the physical order.

#include "protocol_native/v1_codec.hpp"
#include "runtime/fallout_runtime.hpp"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace synth::client {

[[nodiscard]] inline bool is_order_action(std::string_view name) noexcept {
    return name == "follow_player" || name == "stop_following" || name == "take_a_seat";
}

// Advertised only by a flat runtime whose adapter applies and observes the orders (orders_ready()).
inline void append_order_capabilities(std::vector<std::string>& capabilities, core::RuntimeVariant variant, bool orders_ready) {
    if (variant == core::RuntimeVariant::flat && orders_ready) {
        capabilities.push_back("action.follow_player");
        capabilities.push_back("action.stop_following");
        capabilities.push_back("action.take_a_seat");
        capabilities.push_back("action.order_verified");
    }
}

[[nodiscard]] inline std::string order_actor_key(const protocol_native::Identity& actor) {
    std::string plugin{actor.origin_plugin};
    std::ranges::transform(plugin, plugin.begin(), [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c); });
    return actor.form_id + "|" + plugin + "|" + actor.playthrough_id;
}

// Remembers the NPC reply lines this session accepted, so an order applies only for the exact line it rode on.
// A cancelled or replaced turn never applies its order; each reply line carries at most one order.
class OrderReplyBinding final {
public:
    void note_reply(const protocol_native::Line& line, const protocol_native::Identity& speaker, const core::CancellationToken& turn) {
        if (line.line_id.empty() || line.line_id.size() > 128 || turn.is_cancelled()) return;
        std::scoped_lock lock{mutex_};
        if (replies_.size() >= 64) replies_.pop_front();
        replies_.push_back(Reply{line.generation, line.request_id, line.turn_id, line.line_id, order_actor_key(speaker), turn, false});
    }

    // Called under the action queue lock path; true exactly once per reply line while its turn is live.
    [[nodiscard]] bool claim(const protocol_native::Line& line, const protocol_native::Action& action, const core::CancellationToken& turn) {
        if (action.reply_line_id.empty() || turn.is_cancelled()) return false;
        std::scoped_lock lock{mutex_};
        for (auto& reply : replies_) {
            if (reply.line_id != action.reply_line_id) continue;
            if (reply.consumed || reply.generation != line.generation || reply.request_id != line.request_id ||
                reply.turn_id != line.turn_id || reply.actor != order_actor_key(action.actor) ||
                reply.turn.is_cancelled() || reply.turn.generation() != turn.generation()) return false;
            reply.consumed = true;
            return true;
        }
        return false;
    }

    void clear() {
        std::scoped_lock lock{mutex_};
        replies_.clear();
    }

private:
    struct Reply {
        std::uint64_t generation{};
        std::string request_id, turn_id, line_id, actor;
        core::CancellationToken turn;
        bool consumed{};
    };
    std::mutex mutex_;
    std::deque<Reply> replies_;
};

// A bounded receipt on the existing action-detail channel, like Wait. A queue acceptance or another
// actor's observation never becomes a remembered success.
[[nodiscard]] inline std::string verified_order_result(const protocol_native::Action& action,
    const runtime::RuntimeActionRequest& request, const runtime::RuntimeOrderObservation& observed) {
    if (!observed.confirms_success(request) || action.action_id != request.action_id || action.capability != "action." + action.name ||
        !is_order_action(action.name) || action.actor.playthrough_id != request.actor.playthrough_id)
        throw std::invalid_argument{"order result has no owned physical postcondition"};
    std::uint32_t form{};
    const auto& id = action.actor.form_id;
    if (id.size() != 10 || !id.starts_with("0x")) throw std::invalid_argument{"invalid order result actor"};
    const auto parsed = std::from_chars(id.data() + 2, id.data() + id.size(), form, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != id.data() + id.size() || form != request.actor.form_id)
        throw std::invalid_argument{"order result actor identity differs"};
    char package[11]{}, seat[11]{};
    std::snprintf(package, sizeof(package), "0x%08X", observed.owned_package);
    std::snprintf(seat, sizeof(seat), "0x%08X", observed.seat_form);
    auto encoded = json::write(json::Value{protocol_native::object({
        {"schema", "synth.action.order.result.v1"}, {"action_id", action.action_id}, {"action", action.name},
        {"actor", protocol_native::identity_value(action.actor)}, {"reply_line_id", action.reply_line_id},
        {"following", observed.follows_player}, {"seated", observed.seated}, {"released", observed.released},
        {"owned_package", package}, {"seat", seat}, {"reload_policy", "release_owned_order"}
    })});
    if (encoded.size() > 1024) throw std::invalid_argument{"order physical receipt exceeds detail budget"};
    return encoded;
}

// The NPC must be the addressed, live, non-player actor of this playthrough; the order carries no target
// or arguments and the game decides where and how it is applied.
[[nodiscard]] inline runtime::RuntimeActionRequest prepare_order_action(const core::RuntimeSnapshot& snapshot,
    const protocol_native::Action& action, const core::CancellationToken& turn, core::SnapshotClock::time_point deadline) {
    if (snapshot.game() != core::Game::fallout4 || snapshot.variant() != core::RuntimeVariant::flat ||
        turn.is_cancelled() || snapshot.generation() != turn.generation() || snapshot.frame() == 0 ||
        deadline <= core::SnapshotClock::now() || deadline <= snapshot.captured_at() ||
        !is_order_action(action.name) || action.capability != "action." + action.name ||
        action.action_id.empty() || action.action_id.size() > 128 || action.reply_line_id.empty() ||
        action.reply_line_id.size() > 128 || action.target || !action.arguments.is_object() || !action.arguments.as_object().empty())
        throw std::invalid_argument{"invalid order owner, deadline, reply binding or arguments"};
    (void)protocol_native::identity(protocol_native::identity_value(action.actor));
    std::uint32_t form_id{};
    if (action.actor.form_id.size() != 10 || !action.actor.form_id.starts_with("0x"))
        throw std::invalid_argument{"invalid order actor FormID"};
    const auto parsed = std::from_chars(action.actor.form_id.data() + 2, action.actor.form_id.data() + 10, form_id, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != action.actor.form_id.data() + 10 || form_id == 0 ||
        form_id == 0x14 || form_id == snapshot.player().form_id() || action.actor.playthrough_id != snapshot.player().playthrough_id())
        throw std::invalid_argument{"an order requires an NPC in this playthrough"};
    const core::ActorSnapshot* performer{};
    for (const auto& actor : snapshot.actors()) {
        if (actor.form_id() != form_id) continue;
        if (performer || actor.origin_plugin() != action.actor.origin_plugin ||
            actor.playthrough_id() != action.actor.playthrough_id || !actor.alive() || actor.disabled())
            throw std::invalid_argument{"unavailable, ambiguous or mismatched order actor"};
        performer = &actor;
    }
    if (!performer) throw std::invalid_argument{"order actor is absent from the captured scene"};
    // A hostile or fighting NPC takes no order; the game's own AI keeps priority in combat.
    if (performer->in_combat() || performer->hostile_to_player())
        throw std::invalid_argument{"order actor is in combat or hostile"};
    const auto name = action.name == "follow_player" ? runtime::RuntimeActionName::follow_player
        : action.name == "stop_following" ? runtime::RuntimeActionName::stop_following : runtime::RuntimeActionName::take_a_seat;
    return {name, {form_id, performer->origin_plugin(), performer->playthrough_id()}, {}, turn, deadline, action.action_id};
}

}  // namespace synth::client
