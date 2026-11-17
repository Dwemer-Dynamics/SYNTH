#pragma once

// Confirmed NPC sale (contract: protocol/v2/README.md, "Confirmed NPC trade").
//
// The server offers one stack of one item an NPC carries for caps. Nothing moves until the player
// presses Yes in a confirmation box, and Yes only starts a purchase that still matches what the box
// described. This header is engine free: it holds the offer binding, the staleness rules, the
// confirmation mailbox shared with the render thread, and the two-step transfer with its undo. The
// transfers themselves are the existing paired give/take requests of IFalloutRuntime; this file adds
// no game access and no new engine call.
//
// A sale is never retried and never half-done silently. When the item moved but the caps did not, the
// item is moved back once and must restore the starting inventories; any other result that cannot be
// proven stops trading for the rest of the session instead of guessing.

#include "actions/equipment.hpp"
#include "core/cancellation.hpp"
#include "core/snapshot.hpp"
#include "protocol_native/v1_codec.hpp"
#include "runtime/fallout_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace synth::trade {

inline constexpr std::uint32_t caps_form_id = 0xF;
inline constexpr std::uint32_t player_form_id = 0x14;
inline constexpr std::uint32_t maximum_count = 99;
inline constexpr std::uint32_t maximum_price = 100000;
inline constexpr std::size_t maximum_name_bytes = 160;
// The server's offer deadline (60000 ms) is the outer bound; the box itself stays up this long.
inline constexpr std::chrono::seconds confirmation_window{60};

// Machine reason codes carried in the receipt detail; the server accepts only these (protocol/v2/README.md).
inline constexpr std::string_view reason_completed = "trade_completed";
inline constexpr std::string_view reason_declined = "player_declined";
inline constexpr std::string_view reason_cell_changed = "stale_cell_changed";
inline constexpr std::string_view reason_inventory_changed = "stale_inventory_changed";
inline constexpr std::string_view reason_caps_changed = "stale_caps_changed";
inline constexpr std::string_view reason_actor_unavailable = "stale_actor_unavailable";
inline constexpr std::string_view reason_expired = "expired";

enum class Answer : unsigned char { yes, cancel, expired, withdrawn, unavailable };

// What the player is shown, copied from the snapshot the offer was validated against.
struct TradeOffer final {
    std::string action_id;
    runtime::RuntimeActorIdentity seller, buyer;
    std::string seller_name;
    core::InventoryItemSnapshot item;   // the NPC's exact stack at offer time
    std::uint32_t count{1};
    std::uint32_t price{};
    std::uint32_t buyer_caps{};         // the player's observed caps at offer time
    std::optional<core::SceneIdentity> scene;
    std::string cell;
    core::RuntimeGeneration generation;
};

[[nodiscard]] inline std::string confirmation_text(const TradeOffer& offer) {
    return offer.seller_name + " offers to sell you " + std::to_string(offer.count) + " " + offer.item.display_name + " for " +
        std::to_string(offer.price) + " caps. You have " + std::to_string(offer.buyer_caps) + " caps. Buy " +
        std::to_string(offer.count) + " " + offer.item.display_name + " from " + offer.seller_name + " for " +
        std::to_string(offer.price) + " caps?";
}

namespace detail {

[[nodiscard]] inline std::optional<std::uint32_t> parse_form_id(std::string_view text) {
    if (text.size() != 10 || !text.starts_with("0x")) return std::nullopt;
    std::uint32_t value{};
    const auto parsed = std::from_chars(text.data() + 2, text.data() + 10, value, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + 10 || value == 0) return std::nullopt;
    return value;
}

[[nodiscard]] inline std::optional<std::uint64_t> unsigned_integer(const json::Value* value) {
    if (!value) return std::nullopt;
    if (const auto* unsigned_value = std::get_if<std::uint64_t>(&value->storage())) return *unsigned_value;
    if (const auto* signed_value = std::get_if<std::int64_t>(&value->storage()); signed_value && *signed_value >= 0)
        return static_cast<std::uint64_t>(*signed_value);
    return std::nullopt;
}

[[nodiscard]] inline const core::ActorSnapshot* find_actor(const core::RuntimeSnapshot& snapshot, const runtime::RuntimeActorIdentity& who) {
    if (who.form_id == snapshot.player().form_id()) {
        return who.origin_plugin == snapshot.player().origin_plugin() && who.playthrough_id == snapshot.player().playthrough_id()
            ? &snapshot.player() : nullptr;
    }
    const core::ActorSnapshot* found{};
    for (const auto& actor : snapshot.actors()) {
        if (actor.form_id() != who.form_id) continue;
        if (found || actor.origin_plugin() != who.origin_plugin || actor.playthrough_id() != who.playthrough_id) return nullptr;
        found = &actor;
    }
    return found;
}

[[nodiscard]] inline bool tradeable_actor(const core::ActorSnapshot& actor) noexcept {
    return actor.alive() && !actor.disabled() && !actor.in_combat();
}

// The seller's unique, unequipped stack of this exact form and name in a complete inventory observation.
[[nodiscard]] inline std::optional<core::InventoryItemSnapshot> seller_stack(const core::ActorSnapshot& seller, const core::InventoryItemSnapshot& wanted) {
    if (seller.inventory_observation() != "complete") return std::nullopt;
    std::optional<core::InventoryItemSnapshot> found;
    for (const auto& item : seller.inventory()) {
        const bool same_name = item.display_name == wanted.display_name;
        const bool same_form = item.form_id == wanted.form_id && item.origin_plugin == wanted.origin_plugin;
        if (!same_name && !same_form) continue;
        if (found || !same_name || !same_form || !item.valid() || item.equipped) return std::nullopt;
        found = item;
    }
    return found;
}

[[nodiscard]] inline std::uint64_t stack_total(std::span<const core::InventoryItemSnapshot> items, std::uint32_t form_id, std::string_view plugin) {
    std::uint64_t total{};
    for (const auto& item : items)
        if (item.form_id == form_id && item.origin_plugin == plugin) total += item.count;
    return total;
}

}  // namespace detail

// Bind a server offer to the captured scene. Every field is re-derived from this snapshot; the wire
// names a stack and a price, never an inventory. Throws std::invalid_argument with a short reason.
[[nodiscard]] inline TradeOffer prepare_trade_offer(const core::RuntimeSnapshot& snapshot, const protocol_native::Action& action,
                                                    const core::CancellationToken& cancellation) {
    if (snapshot.game() != core::Game::fallout4 || snapshot.variant() != core::RuntimeVariant::flat || cancellation.is_cancelled() ||
        cancellation.generation() != snapshot.generation() || action.name != "barter" || action.capability != "action.barter" ||
        action.action_id.empty() || action.action_id.size() > 128 || !action.target || !action.arguments.is_object())
        throw std::invalid_argument{"invalid trade offer owner"};
    const auto& arguments = action.arguments.as_object();
    if (arguments.size() != 5) throw std::invalid_argument{"trade offer arguments are not exactly item, form, plugin, count and price"};
    const auto* name = action.arguments.find("item");
    const auto* form = action.arguments.find("item_form_id");
    const auto* plugin = action.arguments.find("item_origin_plugin");
    const auto count = detail::unsigned_integer(action.arguments.find("count"));
    const auto price = detail::unsigned_integer(action.arguments.find("price"));
    if (!name || !name->is_string() || !form || !form->is_string() || !plugin || !plugin->is_string() || !count || !price ||
        name->as_string().empty() || name->as_string().size() > maximum_name_bytes || *count < 1 || *count > maximum_count ||
        *price < 1 || *price > maximum_price)
        throw std::invalid_argument{"trade offer has an invalid item, count or price"};
    const auto item_form = detail::parse_form_id(form->as_string());
    if (!item_form || *item_form == caps_form_id || *item_form == player_form_id)
        throw std::invalid_argument{"trade offer names no tradeable item"};

    const auto seller_form = detail::parse_form_id(action.actor.form_id);
    const auto buyer_form = detail::parse_form_id(action.target->form_id);
    if (!seller_form || !buyer_form || *buyer_form != player_form_id || *seller_form == player_form_id ||
        action.actor.playthrough_id != snapshot.player().playthrough_id() || action.target->playthrough_id != snapshot.player().playthrough_id() ||
        snapshot.player().form_id() != player_form_id || !snapshot.player().alive())
        throw std::invalid_argument{"trade requires an NPC seller and the captured player"};
    const runtime::RuntimeActorIdentity seller_id{*seller_form, action.actor.origin_plugin, action.actor.playthrough_id};
    const runtime::RuntimeActorIdentity buyer_id{player_form_id, snapshot.player().origin_plugin(), snapshot.player().playthrough_id()};
    const auto* seller = detail::find_actor(snapshot, seller_id);
    if (!seller || !detail::tradeable_actor(*seller)) throw std::invalid_argument{"the seller is not available to trade"};

    core::InventoryItemSnapshot wanted;
    wanted.form_id = *item_form;
    wanted.origin_plugin = plugin->as_string();
    wanted.display_name = name->as_string();
    const auto stack = detail::seller_stack(*seller, wanted);
    if (!stack || stack->form_id == caps_form_id || stack->count < *count)
        throw std::invalid_argument{"the seller does not carry that item in the captured inventory"};
    const auto caps = actions::select_caps_item(snapshot.player().inventory(), snapshot.player().inventory_observation());
    if (!caps || caps->count < *price) throw std::invalid_argument{"the player's observed caps do not cover the price"};

    TradeOffer offer;
    offer.action_id = action.action_id;
    offer.seller = seller_id;
    offer.buyer = buyer_id;
    offer.seller_name = seller->name().empty() ? action.actor.display_name : seller->name();
    if (offer.seller_name.empty()) offer.seller_name = "The seller";
    offer.item = *stack;
    offer.count = static_cast<std::uint32_t>(*count);
    offer.price = static_cast<std::uint32_t>(*price);
    offer.buyer_caps = caps->count;
    if (snapshot.world()) {
        offer.scene = snapshot.world()->scene;
        offer.cell = snapshot.world()->cell;
    }
    offer.generation = snapshot.generation();
    return offer;
}

// Compare a fresh capture with the state the box described. Empty means the offer still stands.
[[nodiscard]] inline std::optional<std::string_view> stale_reason(const TradeOffer& offer, const core::RuntimeSnapshot& fresh) {
    if (fresh.generation() != offer.generation || fresh.variant() != core::RuntimeVariant::flat) return reason_cell_changed;
    const bool had_world = offer.scene.has_value() || !offer.cell.empty();
    if (had_world) {
        if (!fresh.world()) return reason_cell_changed;
        if (offer.scene ? fresh.world()->scene != offer.scene : fresh.world()->cell != offer.cell) return reason_cell_changed;
    }
    if (fresh.player().form_id() != player_form_id || !fresh.player().alive()) return reason_actor_unavailable;
    const auto* seller = detail::find_actor(fresh, offer.seller);
    if (!seller || !detail::tradeable_actor(*seller)) return reason_actor_unavailable;
    const auto stack = detail::seller_stack(*seller, offer.item);
    if (!stack || stack->count != offer.item.count) return reason_inventory_changed;
    const auto caps = actions::select_caps_item(fresh.player().inventory(), fresh.player().inventory_observation());
    if (!caps || caps->count != offer.buyer_caps || caps->count < offer.price) return reason_caps_changed;
    return std::nullopt;
}

// The pair of inventories exactly as the server's receipt expects them: the NPC (action actor) first, the player second.
[[nodiscard]] inline protocol_native::ActionTransferObservation observe_pair(const core::RuntimeSnapshot& snapshot, const TradeOffer& offer,
                                                                             const protocol_native::Identity& seller_wire,
                                                                             const protocol_native::Identity& buyer_wire, bool extended) {
    protocol_native::ActionTransferObservation pair{{seller_wire, {}, "unavailable"}, {buyer_wire, {}, "unavailable"}};
    const auto copy = [&](const core::ActorSnapshot* actor, protocol_native::ActionInventoryObservation& wire) {
        if (!actor || actor->inventory_observation() == "unavailable") return;
        const std::size_t limit = extended ? 512 : 32;
        wire.observation = actor->inventory_observation();
        for (const auto& item : actor->inventory()) {
            if (wire.items.size() >= limit) { wire.observation = "partial"; break; }
            char id[11]{};
            std::snprintf(id, sizeof(id), "0x%08X", item.form_id);
            wire.items.push_back({id, item.origin_plugin, item.display_name, item.count, item.form_type, item.value, item.weight, item.equipped});
        }
    };
    copy(detail::find_actor(snapshot, offer.seller), pair.donor);
    copy(detail::find_actor(snapshot, offer.buyer), pair.recipient);
    return pair;
}

struct TradeResult final {
    std::string status{"failed"};
    std::string detail;
    // Fresh, copied inventories after the last step, when one could be captured.
    std::shared_ptr<const core::RuntimeSnapshot> snapshot;
    bool moved{};
    // The session must stop offering sales: a step could not be proven or undone.
    bool stop_trading{};
};

// Narrow seams the caller binds to the dispatcher, the game thread and the render-thread prompt.
struct TradeIo final {
    // Show the box and wait for the answer. Worker thread only.
    std::function<Answer(const TradeOffer&)> ask;
    // One fresh capture of the whole scene, copied on the game thread. Null when none could be taken.
    std::function<std::shared_ptr<const core::RuntimeSnapshot>()> capture;
    // One native paired transfer. Null when it did not complete (generation, queue, deadline or cancellation).
    std::function<std::optional<runtime::RuntimeActionResult>(const runtime::RuntimeActionRequest&)> transfer;
};

namespace detail {

[[nodiscard]] inline bool inventories_match(const core::RuntimeSnapshot& left, const core::RuntimeSnapshot& right, const TradeOffer& offer) {
    const auto same = [&](const runtime::RuntimeActorIdentity& who) {
        const auto* a = find_actor(left, who);
        const auto* b = find_actor(right, who);
        if (!a || !b || a->inventory_observation() != "complete" || b->inventory_observation() != "complete" ||
            a->inventory().size() != b->inventory().size()) return false;
        for (const auto& item : a->inventory()) {
            if (stack_total(b->inventory(), item.form_id, item.origin_plugin) != stack_total(a->inventory(), item.form_id, item.origin_plugin)) return false;
        }
        return true;
    };
    return same(offer.seller) && same(offer.buyer);
}

// Item left the seller for the player once, and the price left the player for the seller once.
[[nodiscard]] inline bool sale_conserved(const core::RuntimeSnapshot& before, const core::RuntimeSnapshot& after, const TradeOffer& offer) {
    const auto* seller0 = find_actor(before, offer.seller);
    const auto* buyer0 = find_actor(before, offer.buyer);
    const auto* seller1 = find_actor(after, offer.seller);
    const auto* buyer1 = find_actor(after, offer.buyer);
    if (!seller0 || !buyer0 || !seller1 || !buyer1) return false;
    for (const auto* actor : {seller0, buyer0, seller1, buyer1})
        if (actor->inventory_observation() != "complete") return false;
    const auto total = [](const core::ActorSnapshot* actor, std::uint32_t form, std::string_view plugin) {
        return static_cast<std::int64_t>(stack_total(actor->inventory(), form, plugin));
    };
    const std::int64_t count = offer.count, price = offer.price;
    return total(seller1, offer.item.form_id, offer.item.origin_plugin) - total(seller0, offer.item.form_id, offer.item.origin_plugin) == -count &&
           total(buyer1, offer.item.form_id, offer.item.origin_plugin) - total(buyer0, offer.item.form_id, offer.item.origin_plugin) == count &&
           total(seller1, caps_form_id, "Fallout4.esm") - total(seller0, caps_form_id, "Fallout4.esm") == price &&
           total(buyer1, caps_form_id, "Fallout4.esm") - total(buyer0, caps_form_id, "Fallout4.esm") == -price;
}

[[nodiscard]] inline bool succeeded(const std::optional<runtime::RuntimeActionResult>& result) noexcept {
    return result && !result->pending && result->status == runtime::RuntimeActionStatus::succeeded;
}

}  // namespace detail

// Run one answered offer. `before` is the scene the box described; the caller already holds the
// session's cancellation and sends exactly one receipt from whatever this returns. Never throws.
[[nodiscard]] inline std::optional<TradeResult> run_trade(const TradeOffer& offer, const std::shared_ptr<const core::RuntimeSnapshot>& before,
                                                          const TradeIo& io, const core::CancellationToken& cancellation,
                                                          std::chrono::steady_clock::time_point deadline) {
    TradeResult result;
    const auto answer = io.ask(offer);
    if (answer == Answer::withdrawn || cancellation.is_cancelled()) return std::nullopt;
    result.snapshot = before;
    if (answer == Answer::unavailable) {
        result.status = "unavailable";
        result.detail = "no confirmation box is available; nothing was bought";
        return result;
    }
    if (answer == Answer::expired) {
        result.status = "timed_out";
        result.detail = std::string{reason_expired};
        result.snapshot = io.capture();
        if (!result.snapshot) result.snapshot = before;
        return result;
    }
    auto fresh = io.capture();
    if (answer == Answer::cancel) {
        result.status = "cancelled";
        result.detail = std::string{reason_declined};
        if (fresh) result.snapshot = fresh;
        return result;
    }
    // Yes. Nothing is trusted from before the box: the scene, both actors, the stack and the caps are read again.
    if (!fresh) {
        result.status = "cancelled";
        result.detail = std::string{reason_actor_unavailable};
        return result;
    }
    result.snapshot = fresh;
    if (const auto stale = stale_reason(offer, *fresh)) {
        result.status = "cancelled";
        result.detail = std::string{*stale};
        return result;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        result.status = "timed_out";
        result.detail = std::string{reason_expired};
        return result;
    }

    const auto* seller = detail::find_actor(*fresh, offer.seller);
    const auto* buyer = detail::find_actor(*fresh, offer.buyer);
    const auto item = seller ? detail::seller_stack(*seller, offer.item) : std::nullopt;
    const auto caps = buyer ? actions::select_caps_item(buyer->inventory(), buyer->inventory_observation()) : std::nullopt;
    if (!item || !caps) {
        result.status = "cancelled";
        result.detail = std::string{reason_inventory_changed};
        return result;
    }
    const auto request = [&](runtime::RuntimeActionName name, const runtime::RuntimeActorIdentity& donor,
                             const runtime::RuntimeActorIdentity& recipient, const core::InventoryItemSnapshot& what, std::uint32_t amount) {
        return runtime::RuntimeActionRequest{name, donor, what, cancellation, deadline, offer.action_id, recipient, amount};
    };

    // 1. The item moves seller -> player with the game's own transfer.
    const auto first = io.transfer(request(runtime::RuntimeActionName::give_item, offer.seller, offer.buyer, *item, offer.count));
    auto after_first = io.capture();
    if (cancellation.is_cancelled()) return std::nullopt;
    if (after_first) result.snapshot = after_first;
    if (!detail::succeeded(first)) {
        // A refused step moved nothing only if the inventories say so; an unproven one stops trading.
        if (after_first && detail::inventories_match(*fresh, *after_first, offer)) {
            result.status = "failed";
            result.detail = "the item could not be transferred; nothing changed";
            return result;
        }
        result.status = "failed";
        result.detail = "the item transfer could not be verified";
        result.moved = true;
        result.stop_trading = true;
        return result;
    }
    if (!after_first) {
        result.status = "failed";
        result.detail = "the sale could not be verified after the item moved";
        result.moved = true;
        result.stop_trading = true;
        return result;
    }
    const auto* player_after_item = detail::find_actor(*after_first, offer.buyer);
    const auto player_caps = player_after_item ? actions::select_caps_item(player_after_item->inventory(), player_after_item->inventory_observation()) : std::nullopt;
    const auto player_item = player_after_item ? detail::seller_stack(*player_after_item, offer.item) : std::nullopt;

    // 2. The price moves player -> seller.
    std::optional<runtime::RuntimeActionResult> second;
    if (player_caps && player_caps->count == offer.buyer_caps && !cancellation.is_cancelled())
        second = io.transfer(request(runtime::RuntimeActionName::take_caps, offer.buyer, offer.seller, *player_caps, offer.price));
    auto after_second = io.capture();
    if (cancellation.is_cancelled()) return std::nullopt;
    if (after_second) result.snapshot = after_second;
    if (detail::succeeded(second) && after_second && detail::sale_conserved(*fresh, *after_second, offer)) {
        result.status = "succeeded";
        result.detail = std::string{reason_completed};
        result.moved = true;
        return result;
    }

    // Payment did not complete cleanly. If no caps moved at all, return the item once and prove the undo.
    const auto seller_caps_before = detail::stack_total(seller->inventory(), caps_form_id, "Fallout4.esm");
    bool no_caps_moved = false;
    if (after_second) {
        const auto* b = detail::find_actor(*after_second, offer.buyer);
        const auto* s = detail::find_actor(*after_second, offer.seller);
        no_caps_moved = b && s && detail::stack_total(b->inventory(), caps_form_id, "Fallout4.esm") == offer.buyer_caps &&
            detail::stack_total(s->inventory(), caps_form_id, "Fallout4.esm") == seller_caps_before;
    }
    if (no_caps_moved && player_item && !cancellation.is_cancelled()) {
        const auto undo = io.transfer(request(runtime::RuntimeActionName::take_item, offer.buyer, offer.seller, *player_item, offer.count));
        auto restored = io.capture();
        if (restored) result.snapshot = restored;
        if (detail::succeeded(undo) && restored && detail::inventories_match(*fresh, *restored, offer)) {
            result.status = "failed";
            result.detail = "payment failed; the item was returned and nothing changed";
            return result;
        }
        result.status = "failed";
        result.detail = "the sale could not be finished and could not be undone";
        result.moved = true;
        result.stop_trading = true;
        return result;
    }
    result.status = "failed";
    result.detail = "the sale could not be verified";
    result.moved = true;
    result.stop_trading = true;
    return result;
}

// The one open confirmation box. The worker opens it and waits; the render thread reads a copy and
// submits one answer. Every prompt has a token, so a late click on an older box answers nothing.
class TradePrompt final {
public:
    struct View final {
        std::uint64_t token{};
        std::string title;
        std::string body;
    };

    // Set once at load when a presenter (the Menu Framework) exists. Without it no offer is ever advertised.
    void set_installed(bool installed) noexcept { installed_.store(installed, std::memory_order_release); }
    [[nodiscard]] bool installed() const noexcept { return installed_.load(std::memory_order_acquire); }

    // Worker thread. Returns when answered, expired, withdrawn (cancellation, terminal) or no presenter exists.
    [[nodiscard]] Answer ask(const TradeOffer& offer, const core::CancellationToken& cancellation, const std::atomic_bool& terminal,
                             std::chrono::steady_clock::duration window = confirmation_window) {
        if (!installed()) return Answer::unavailable;
        std::unique_lock lock{mutex_};
        if (open_) return Answer::unavailable;  // one box at a time; a second offer is refused, not queued
        open_ = Open{++next_token_, "Buy " + offer.item.display_name + "?", confirmation_text(offer), {}};
        const auto expires = std::chrono::steady_clock::now() + window;
        for (;;) {
            if (open_->answer) break;
            if (cancellation.is_cancelled() || terminal.load(std::memory_order_acquire)) { open_.reset(); return Answer::withdrawn; }
            if (std::chrono::steady_clock::now() >= expires) { open_.reset(); return Answer::expired; }
            changed_.wait_for(lock, std::chrono::milliseconds{25});
        }
        const auto answer = *open_->answer;
        open_.reset();
        return answer;
    }

    // Render thread: a copy of the open box, or nothing.
    [[nodiscard]] std::optional<View> view() const {
        std::scoped_lock lock{mutex_};
        if (!open_ || open_->answer) return std::nullopt;
        return View{open_->token, open_->title, open_->body};
    }

    // Render thread: the first answer to the open box with this token wins; anything else is refused.
    bool answer(std::uint64_t token, Answer value) {
        std::scoped_lock lock{mutex_};
        if (!open_ || open_->token != token || open_->answer) return false;
        open_->answer = value == Answer::yes ? Answer::yes : Answer::cancel;
        changed_.notify_all();
        return true;
    }

    [[nodiscard]] bool is_open() const {
        std::scoped_lock lock{mutex_};
        return open_.has_value();
    }

private:
    struct Open final {
        std::uint64_t token{};
        std::string title, body;
        std::optional<Answer> answer;
    };

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Open> open_;
    std::uint64_t next_token_{};
    std::atomic_bool installed_{};
};

// Process-wide: the Menu Framework window and every session's action worker share this one box.
[[nodiscard]] inline TradePrompt& prompt() {
    static TradePrompt instance;
    return instance;
}

// A sale that could not be proven stops all later offers for the rest of the game session.
[[nodiscard]] inline std::atomic_bool& trading_stopped() {
    static std::atomic_bool stopped{};
    return stopped;
}

}  // namespace synth::trade
