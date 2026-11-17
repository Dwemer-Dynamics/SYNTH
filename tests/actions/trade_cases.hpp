#pragma once
// Confirmed NPC sale: offer binding, staleness, the answered purchase with its undo, and the confirmation mailbox.
#include "protocol_native/v1_codec.hpp"
#include "trade/confirmed_trade.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace synth::tests {

namespace trade_detail {
using core::InventoryItemSnapshot;

inline InventoryItemSnapshot item(std::uint32_t form, const char* name, std::uint32_t count, std::int32_t value = 12,
                                  std::uint16_t type = 46, bool equipped = false) {
    return {form, "Fallout4.esm", name, count, value, 1.0, type, equipped};
}
inline InventoryItemSnapshot caps(std::uint32_t count) { return item(0xF, "Bottle Cap", count, 1, 0x23); }

// One mutable scene the fake runtime reads and the fake transfers change.
struct Scene final {
    std::vector<InventoryItemSnapshot> npc{item(0x22F75, "Nuka-Cola", 3), caps(100)};
    std::vector<InventoryItemSnapshot> player{caps(100)};
    std::string npc_observation{"complete"}, player_observation{"complete"};
    std::uint32_t cell{0x1000};
    bool npc_present{true}, npc_in_combat{false}, npc_alive{true};
    core::RuntimeGeneration generation{core::RuntimeGeneration::initial()};
    core::RuntimeVariant lane{core::RuntimeVariant::flat};

    [[nodiscard]] std::shared_ptr<const core::RuntimeSnapshot> snapshot() const {
        const auto pose = core::WorldPose{{}, core::UnitVector3::from({1, 0, 0}), core::UnitVector3::from({0, 0, 1})};
        core::ActorSnapshot player_actor{0x14, "Sole Survivor", {}, "Fallout4.esm", "save-a"};
        player_actor = player_actor.with_inventory(player, player_observation);
        std::vector<core::ActorSnapshot> actors;
        if (npc_present) {
            core::ActorSnapshot seller{0x1A4D7, "Preston Garvey", {1, 2, 3}, "Fallout4.esm", "save-a", npc_alive, false, npc_in_combat};
            actors.push_back(seller.with_inventory(npc, npc_observation));
        }
        core::WorldState world;
        world.cell = "Sanctuary";
        world.scene = core::SceneIdentity{cell, 0x3C, "Fallout4.esm", "Fallout4.esm"};
        return std::make_shared<const core::RuntimeSnapshot>(core::Game::fallout4, lane, generation, 12, core::SnapshotClock::now(), pose,
            std::nullopt, std::nullopt, std::nullopt, std::move(player_actor), std::move(actors), 100, std::move(world));
    }
};

inline std::uint32_t total(const std::vector<InventoryItemSnapshot>& items, std::uint32_t form) {
    std::uint32_t sum{};
    for (const auto& value : items) if (value.form_id == form) sum += value.count;
    return sum;
}

inline void set_total(std::vector<InventoryItemSnapshot>& items, const InventoryItemSnapshot& like, std::int64_t delta) {
    for (auto it = items.begin(); it != items.end(); ++it) {
        if (it->form_id != like.form_id) continue;
        const auto next = static_cast<std::int64_t>(it->count) + delta;
        if (next <= 0) items.erase(it); else it->count = static_cast<std::uint32_t>(next);
        return;
    }
    if (delta > 0) { auto added = like; added.count = static_cast<std::uint32_t>(delta); items.push_back(added); }
}

inline protocol_native::Action offer_action(const char* arguments =
    R"({"item":"Nuka-Cola","item_form_id":"0x00022F75","item_origin_plugin":"Fallout4.esm","count":1,"price":12})") {
    return {"action:trade", "idem:trade", "barter", "action.barter", {"0x0001A4D7", "Fallout4.esm", "save-a", "Preston Garvey"},
            protocol_native::Identity{"0x00000014", "Fallout4.esm", "save-a", "Sole Survivor"}, json::parse(arguments), 60000};
}

// Fake runtime IO: counts and orders every transfer, and can fail or partly apply chosen steps.
struct Io final {
    Scene& scene;
    trade::Answer answer{trade::Answer::yes};
    std::function<void()> before_yes;       // runs between the box and the fresh capture, as the world would
    std::vector<std::string> calls;
    int fail_call{-1};                      // 0-based transfer index that reports failure and moves nothing
    int fail_undo_move{-1};                 // transfer index that reports failure but leaves the stacks moved
    int partial_caps{0};                    // a caps transfer that moves this many caps and reports failure
    int asked{};

    trade::TradeIo bind() {
        trade::TradeIo io;
        io.ask = [this](const trade::TradeOffer&) { ++asked; if (before_yes) before_yes(); return answer; };
        io.capture = [this] { return scene.snapshot(); };
        io.transfer = [this](const runtime::RuntimeActionRequest& request) -> std::optional<runtime::RuntimeActionResult> {
            const auto index = static_cast<int>(calls.size());
            calls.push_back(std::string{request.name == runtime::RuntimeActionName::give_item ? "give_item"
                : request.name == runtime::RuntimeActionName::take_caps ? "take_caps"
                : request.name == runtime::RuntimeActionName::take_item ? "take_item" : "other"} + ":" + std::to_string(request.amount));
            runtime::RuntimeActionResult done{runtime::RuntimeActionStatus::succeeded, "ok"};
            if (index == fail_call) { done.status = runtime::RuntimeActionStatus::failed; return done; }
            const bool to_player = request.name == runtime::RuntimeActionName::give_item;
            auto& from = to_player ? scene.npc : (request.name == runtime::RuntimeActionName::take_item ? scene.player : scene.player);
            auto& to = to_player ? scene.player : scene.npc;
            if (request.name == runtime::RuntimeActionName::take_item) { auto& give = scene.player; auto& take = scene.npc;
                set_total(give, *request.item, -static_cast<std::int64_t>(request.amount)); set_total(take, *request.item, request.amount); return done; }
            if (request.name == runtime::RuntimeActionName::take_caps && partial_caps > 0) {
                set_total(from, *request.item, -partial_caps); set_total(to, *request.item, partial_caps);
                done.status = runtime::RuntimeActionStatus::failed; return done;
            }
            set_total(from, *request.item, -static_cast<std::int64_t>(request.amount));
            set_total(to, *request.item, request.amount);
            if (index == fail_undo_move) done.status = runtime::RuntimeActionStatus::failed;
            return done;
        };
        return io;
    }
};
}  // namespace trade_detail

inline int trade_contract_checks() {
    using namespace trade_detail;
    int checks{};
    const auto check = [&](bool value, const char* reason) { ++checks; if (!value) throw std::runtime_error(reason); };
    core::CancellationSource session{core::RuntimeGeneration::initial()};
    const auto token = session.token();
    const auto later = std::chrono::steady_clock::now() + std::chrono::seconds{30};

    // The offer binds to the captured scene and names exactly one stack.
    {
        Scene scene;
        const auto snapshot = scene.snapshot();
        const auto offer = trade::prepare_trade_offer(*snapshot, offer_action(), token);
        check(offer.item.form_id == 0x22F75 && offer.item.count == 3 && offer.count == 1 && offer.price == 12, "offer lost its stack or price");
        check(offer.buyer_caps == 100 && offer.seller.form_id == 0x1A4D7 && offer.buyer.form_id == 0x14, "offer lost its actors or caps");
        check(offer.seller_name == "Preston Garvey" && offer.cell == "Sanctuary" && offer.scene && offer.scene->cell_form_id == 0x1000, "offer lost its scene");
        const auto text = trade::confirmation_text(offer);
        check(text == "Preston Garvey offers to sell you 1 Nuka-Cola for 12 caps. You have 100 caps. Buy 1 Nuka-Cola from Preston Garvey for 12 caps?",
              "confirmation text changed");
    }

    // Anything the snapshot cannot prove is refused before a box exists.
    {
        const auto refused = [&](Scene scene, protocol_native::Action action, const char* why) {
            bool threw{};
            try { (void)trade::prepare_trade_offer(*scene.snapshot(), action, token); } catch (const std::invalid_argument&) { threw = true; }
            check(threw, why);
        };
        refused(Scene{}, offer_action(R"({"item":"Fat Man","item_form_id":"0x00022F75","item_origin_plugin":"Fallout4.esm","count":1,"price":12})"), "wrong name accepted");
        refused(Scene{}, offer_action(R"({"item":"Nuka-Cola","item_form_id":"0x00022F76","item_origin_plugin":"Fallout4.esm","count":1,"price":12})"), "wrong form accepted");
        refused(Scene{}, offer_action(R"({"item":"Nuka-Cola","item_form_id":"0x00022F75","item_origin_plugin":"Other.esp","count":1,"price":12})"), "wrong plugin accepted");
        refused(Scene{}, offer_action(R"({"item":"Bottle Cap","item_form_id":"0x0000000F","item_origin_plugin":"Fallout4.esm","count":1,"price":12})"), "caps sold as an item");
        refused(Scene{}, offer_action(R"({"item":"Nuka-Cola","item_form_id":"0x00022F75","item_origin_plugin":"Fallout4.esm","count":4,"price":12})"), "more than carried");
        refused(Scene{}, offer_action(R"({"item":"Nuka-Cola","item_form_id":"0x00022F75","item_origin_plugin":"Fallout4.esm","count":0,"price":12})"), "zero count accepted");
        refused(Scene{}, offer_action(R"({"item":"Nuka-Cola","item_form_id":"0x00022F75","item_origin_plugin":"Fallout4.esm","count":100,"price":12})"), "huge count accepted");
        refused(Scene{}, offer_action(R"({"item":"Nuka-Cola","item_form_id":"0x00022F75","item_origin_plugin":"Fallout4.esm","count":1,"price":0})"), "free price accepted");
        refused(Scene{}, offer_action(R"({"item":"Nuka-Cola","item_form_id":"0x00022F75","item_origin_plugin":"Fallout4.esm","count":1,"price":100001})"), "huge price accepted");
        refused(Scene{}, offer_action(R"({"item":"Nuka-Cola","item_form_id":"0x00022F75","item_origin_plugin":"Fallout4.esm","count":1,"price":101})"), "unaffordable price accepted");
        refused(Scene{}, offer_action(R"({"item":"Nuka-Cola","item_form_id":"0x00022F75","item_origin_plugin":"Fallout4.esm","count":1,"price":12.5})"), "fractional price accepted");
        refused(Scene{}, offer_action(R"({"item":"Nuka-Cola","item_form_id":"0x00022F75","item_origin_plugin":"Fallout4.esm","count":1})"), "missing price accepted");
        refused(Scene{}, offer_action(R"({"item":"Nuka-Cola","item_form_id":"0x00022F75","item_origin_plugin":"Fallout4.esm","count":1,"price":12,"note":"x"})"), "extra argument accepted");
        auto player_seller = offer_action(); player_seller.actor = {"0x00000014", "Fallout4.esm", "save-a", "Sole Survivor"};
        refused(Scene{}, player_seller, "player sold to themselves");
        auto npc_buyer = offer_action(); npc_buyer.target = protocol_native::Identity{"0x0001A4D7", "Fallout4.esm", "save-a", "Preston Garvey"};
        refused(Scene{}, npc_buyer, "an NPC bought");
        auto foreign = offer_action(); foreign.actor.playthrough_id = "other-save";
        refused(Scene{}, foreign, "another save's actor accepted");
        auto not_barter = offer_action(); not_barter.name = "give_item_to";
        refused(Scene{}, not_barter, "wrong action accepted");
        Scene absent; absent.npc_present = false; refused(absent, offer_action(), "absent seller");
        Scene dead; dead.npc_alive = false; refused(dead, offer_action(), "dead seller");
        Scene fighting; fighting.npc_in_combat = true; refused(fighting, offer_action(), "seller in combat");
        Scene partial; partial.npc_observation = "partial"; refused(partial, offer_action(), "partial inventory proved a unique name");
        Scene equipped; equipped.npc[0].equipped = true; refused(equipped, offer_action(), "equipped stack sold");
        Scene twin; twin.npc.push_back(item(0x22F76, "Nuka-Cola", 1)); refused(twin, offer_action(), "duplicate name accepted");
        Scene poor; poor.player = {caps(11)}; refused(poor, offer_action(), "player could not pay");
        Scene no_caps; no_caps.player = {item(0x22F75, "Nuka-Cola", 1)}; refused(no_caps, offer_action(), "player without caps");
        Scene vr; vr.lane = core::RuntimeVariant::vr; refused(vr, offer_action(), "VR accepted");
        core::CancellationSource other{core::RuntimeGeneration::from_value(7)};
        bool threw{};
        try { (void)trade::prepare_trade_offer(*Scene{}.snapshot(), offer_action(), other.token()); } catch (const std::invalid_argument&) { threw = true; }
        check(threw, "another generation's token accepted");
        core::CancellationSource cancelled{token}; cancelled.cancel();
        threw = false;
        try { (void)trade::prepare_trade_offer(*Scene{}.snapshot(), offer_action(), cancelled.token()); } catch (const std::invalid_argument&) { threw = true; }
        check(threw, "cancelled turn accepted");
    }

    // Yes: the item and caps move once, in that order, and the receipt carries both inventories.
    {
        Scene scene;
        const auto before = scene.snapshot();
        const auto offer = trade::prepare_trade_offer(*before, offer_action(), token);
        Io io{scene};
        auto result = trade::run_trade(offer, before, io.bind(), token, later);
        check(result && result->status == "succeeded" && result->detail == "trade_completed" && !result->stop_trading, "a clean sale did not complete");
        check(io.calls == std::vector<std::string>{"give_item:1", "take_caps:12"}, "the sale did not move the item then the caps");
        check(total(scene.npc, 0x22F75) == 2 && total(scene.player, 0x22F75) == 1 && total(scene.npc, 0xF) == 112 && total(scene.player, 0xF) == 88, "wrong totals");
        const auto pair = trade::observe_pair(*result->snapshot, offer, offer_action().actor, *offer_action().target, true);
        check(pair.donor.actor.form_id == "0x0001A4D7" && pair.recipient.actor.form_id == "0x00000014", "receipt owners are not NPC then player");
        check(pair.donor.observation == "complete" && pair.recipient.observation == "complete", "receipt lost its observation quality");
        std::uint64_t npc_caps{}, player_caps{}, player_cola{};
        for (const auto& row : pair.donor.items) if (row.form_id == "0x0000000F") npc_caps = row.count;
        for (const auto& row : pair.recipient.items) { if (row.form_id == "0x0000000F") player_caps = row.count; if (row.form_id == "0x00022F75") player_cola = row.count; }
        check(npc_caps == 112 && player_caps == 88 && player_cola == 1, "receipt rows do not show the sale");
        check(trade::observe_pair(*result->snapshot, offer, offer_action().actor, *offer_action().target, false).donor.items.size() == 2, "unextended rows were wrong");
    }

    // Cancel and expiry: nothing is asked of the runtime at all.
    for (const auto answer : {trade::Answer::cancel, trade::Answer::expired}) {
        Scene scene;
        const auto before = scene.snapshot();
        const auto offer = trade::prepare_trade_offer(*before, offer_action(), token);
        Io io{scene, answer};
        auto result = trade::run_trade(offer, before, io.bind(), token, later);
        check(result && io.calls.empty(), "an unanswered or cancelled box moved something");
        check(answer == trade::Answer::cancel ? (result->status == "cancelled" && result->detail == "player_declined")
                                              : (result->status == "timed_out" && result->detail == "expired"), "wrong decline outcome");
        check(total(scene.npc, 0x22F75) == 3 && total(scene.player, 0xF) == 100, "a decline changed an inventory");
    }
    {
        Scene scene;
        const auto before = scene.snapshot();
        const auto offer = trade::prepare_trade_offer(*before, offer_action(), token);
        Io unavailable{scene, trade::Answer::unavailable};
        auto result = trade::run_trade(offer, before, unavailable.bind(), token, later);
        check(result && result->status == "unavailable" && unavailable.calls.empty(), "no box did not fail closed");
        core::CancellationSource turn{token};
        Io withdrawn{scene, trade::Answer::withdrawn};
        check(!trade::run_trade(offer, before, withdrawn.bind(), turn.token(), later), "a withdrawn box still produced a receipt");
    }

    // Stale: the world changed while the box was open. Yes is answered "stale" and nothing moves.
    {
        const auto stale = [&](const char* why, const char* reason, const std::function<void(Scene&)>& change) {
            Scene scene;
            const auto before = scene.snapshot();
            const auto offer = trade::prepare_trade_offer(*before, offer_action(), token);
            Io io{scene};
            io.before_yes = [&] { change(scene); };
            auto result = trade::run_trade(offer, before, io.bind(), token, later);
            check(result && result->status == "cancelled" && result->detail == reason && io.calls.empty() && !result->stop_trading, why);
        };
        stale("cell change", "stale_cell_changed", [](Scene& s) { s.cell = 0x2000; });
        stale("load", "stale_cell_changed", [](Scene& s) { s.generation = core::RuntimeGeneration::from_value(9); });
        stale("seller left", "stale_actor_unavailable", [](Scene& s) { s.npc_present = false; });
        stale("seller died", "stale_actor_unavailable", [](Scene& s) { s.npc_alive = false; });
        stale("combat started", "stale_actor_unavailable", [](Scene& s) { s.npc_in_combat = true; });
        stale("seller sold it elsewhere", "stale_inventory_changed", [](Scene& s) { set_total(s.npc, item(0x22F75, "Nuka-Cola", 1), -1); });
        stale("seller lost the stack", "stale_inventory_changed", [](Scene& s) { s.npc = {caps(100)}; });
        stale("seller inventory unreadable", "stale_inventory_changed", [](Scene& s) { s.npc_observation = "partial"; });
        stale("player spent caps", "stale_caps_changed", [](Scene& s) { set_total(s.player, caps(1), -50); });
        stale("player gained caps", "stale_caps_changed", [](Scene& s) { set_total(s.player, caps(1), 5); });
        stale("player caps unreadable", "stale_caps_changed", [](Scene& s) { s.player_observation = "unavailable"; s.player.clear(); });
        // A deadline that passed while the box was open is expiry, not a purchase.
        Scene scene;
        const auto before = scene.snapshot();
        const auto offer = trade::prepare_trade_offer(*before, offer_action(), token);
        Io io{scene};
        auto result = trade::run_trade(offer, before, io.bind(), token, std::chrono::steady_clock::now() - std::chrono::seconds{1});
        check(result && result->status == "timed_out" && io.calls.empty(), "an expired Yes moved something");
    }

    // A step that fails: nothing moved is reported plainly; a half-done sale is undone once and proven.
    {
        Scene scene;
        const auto before = scene.snapshot();
        const auto offer = trade::prepare_trade_offer(*before, offer_action(), token);
        Io first{scene}; first.fail_call = 0;
        auto result = trade::run_trade(offer, before, first.bind(), token, later);
        check(result && result->status == "failed" && !result->stop_trading && first.calls.size() == 1 && total(scene.npc, 0x22F75) == 3, "a refused item step was not clean");

        Scene undone;
        Io pay{undone}; pay.fail_call = 1;
        result = trade::run_trade(offer, before, pay.bind(), token, later);
        check(result && result->status == "failed" && !result->stop_trading, "a refused payment was not undone");
        check(pay.calls == std::vector<std::string>{"give_item:1", "take_caps:12", "take_item:1"}, "payment failure did not return the item once");
        check(total(undone.npc, 0x22F75) == 3 && total(undone.player, 0x22F75) == 0 && total(undone.player, 0xF) == 100 && total(undone.npc, 0xF) == 100, "undo did not restore both sides");

        Scene stuck;
        Io lost{stuck}; lost.fail_call = 1;
        // The return transfer reports failure and leaves the item with the player.
        auto stuck_io = lost.bind();
        const auto inner = stuck_io.transfer;
        stuck_io.transfer = [&](const runtime::RuntimeActionRequest& request) -> std::optional<runtime::RuntimeActionResult> {
            if (request.name == runtime::RuntimeActionName::take_item) return runtime::RuntimeActionResult{runtime::RuntimeActionStatus::failed, "blocked"};
            return inner(request);
        };
        result = trade::run_trade(offer, before, stuck_io, token, later);
        check(result && result->status == "failed" && result->stop_trading, "a failed undo did not stop trading");

        Scene partial;
        Io half{partial}; half.partial_caps = 5;
        result = trade::run_trade(offer, before, half.bind(), token, later);
        check(result && result->status == "failed" && result->stop_trading, "a partial payment did not stop trading");
        check(half.calls == std::vector<std::string>{"give_item:1", "take_caps:12"}, "a partial payment was retried or reversed");

        // A runtime that never answers the item step (generation change, queue full) is unproven, not clean.
        Scene silent;
        Io quiet{silent};
        auto quiet_io = quiet.bind();
        quiet_io.transfer = [&](const runtime::RuntimeActionRequest&) -> std::optional<runtime::RuntimeActionResult> { silent.npc[0].count = 2; return std::nullopt; };
        result = trade::run_trade(offer, before, quiet_io, token, later);
        check(result && result->status == "failed" && result->stop_trading, "an unanswered step that moved the item was called clean");
    }

    // The confirmation mailbox: one box, one answer, a stale click answers nothing.
    {
        trade::TradePrompt box;
        std::atomic_bool terminal{};
        Scene scene;
        const auto offer = trade::prepare_trade_offer(*scene.snapshot(), offer_action(), token);
        check(box.ask(offer, token, terminal) == trade::Answer::unavailable, "a prompt without a presenter was shown");
        box.set_installed(true);
        check(!box.view(), "an idle prompt had a view");
        trade::Answer got{trade::Answer::withdrawn};
        std::thread waiter{[&] { got = box.ask(offer, token, terminal, std::chrono::seconds{5}); }};
        std::optional<trade::TradePrompt::View> seen;
        for (int attempt = 0; attempt < 400 && !(seen = box.view()); ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds{5});
        check(seen && seen->body == trade::confirmation_text(offer) && seen->title == "Buy Nuka-Cola?", "the box did not show the offer");
        check(box.ask(offer, token, terminal) == trade::Answer::unavailable, "a second box opened on top of the first");
        check(!box.answer(seen->token + 1, trade::Answer::yes), "a wrong token answered the box");
        check(box.answer(seen->token, trade::Answer::yes), "the box refused its own token");
        check(!box.answer(seen->token, trade::Answer::cancel), "a second answer replaced the first");
        waiter.join();
        check(got == trade::Answer::yes && !box.is_open(), "the first answer was lost");
        check(!box.answer(seen->token, trade::Answer::yes), "a late click answered a closed box");
        // An old token never answers the next box.
        std::thread second{[&] { got = box.ask(offer, token, terminal, std::chrono::seconds{5}); }};
        std::optional<trade::TradePrompt::View> next;
        for (int attempt = 0; attempt < 400 && !(next = box.view()); ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds{5});
        check(next && next->token != seen->token && !box.answer(seen->token, trade::Answer::yes), "an old token answered a new box");
        check(box.answer(next->token, trade::Answer::cancel), "the new box refused its token");
        second.join();
        check(got == trade::Answer::cancel, "Cancel did not decline");
        check(box.ask(offer, token, terminal, std::chrono::milliseconds{30}) == trade::Answer::expired && !box.is_open(), "an ignored box did not expire");
        core::CancellationSource turn{token};
        std::thread aborted{[&] { got = box.ask(offer, turn.token(), terminal, std::chrono::seconds{5}); }};
        for (int attempt = 0; attempt < 400 && !box.view(); ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds{5});
        turn.cancel();
        aborted.join();
        check(got == trade::Answer::withdrawn && !box.is_open(), "a cancelled turn left its box open");
        std::thread retired{[&] { got = box.ask(offer, token, terminal, std::chrono::seconds{5}); }};
        for (int attempt = 0; attempt < 400 && !box.view(); ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds{5});
        terminal.store(true);
        retired.join();
        check(got == trade::Answer::withdrawn && !box.is_open(), "a retired session left its box open");
    }

    // The mirrored fixtures decode into exactly what the preparation expects.
    {
        std::ifstream in{"protocol/v2/fixtures/valid/response-barter.ndjson"};
        std::string line, offer_line;
        while (std::getline(in, line)) if (line.find("\"type\":\"action\"") != std::string::npos) offer_line = line;
        check(!offer_line.empty(), "the offer fixture is missing");
        const auto decoded = protocol_native::decode_line(offer_line);
        const auto* action = std::get_if<protocol_native::Action>(&decoded.payload);
        check(action && action->name == "barter" && action->capability == "action.barter" && action->target && action->target->form_id == "0x00000014", "fixture offer did not decode");
        check(action->arguments.find("price") && action->arguments.find("item_form_id"), "fixture offer lost its arguments");
        std::ifstream ack{"protocol/v2/fixtures/valid/response-init-trade.ndjson"};
        bool acknowledged{};
        while (std::getline(ack, line)) {
            const auto status = protocol_native::decode_line(line);
            if (const auto* value = std::get_if<protocol_native::Status>(&status.payload)) acknowledged = value->trade_confirmation;
        }
        check(acknowledged, "the trade acknowledgement did not decode");
    }
    return checks;
}
}  // namespace synth::tests
