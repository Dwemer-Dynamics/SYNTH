#include "actions/actions.hpp"
#include "actions/equipment.hpp"
#include "runtime/action_completion.hpp"
#include "runtime/pickup_progress.hpp"
#include "runtime/wait_recovery.hpp"
#include "client/action_inventory.hpp"

#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace std::chrono_literals;
int checks{};
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error{std::string{"check failed: "} + #x}; } while (false)

synth::context::ActorIdentity actor() {
    return {{42, "Actors.esm"}, synth::context::PlaythroughIdentity{"save-a"}};
}

synth::actions::ActionCorrelation correlation(std::string id = "action-1",
                                              std::uint64_t runtime = 1,
                                              std::uint64_t response = 2) {
    return {"request-1", std::move(id), synth::core::RuntimeGeneration::from_value(runtime),
            synth::actions::ResponseGeneration::from_value(response)};
}

struct Executor final : synth::actions::IInspectActorExecutor {
    synth::actions::InspectExecution next;
    int calls{};
    synth::actions::InspectExecution inspect_actor(const synth::context::ActorIdentity&) override {
        ++calls;
        return next;
    }
};
}

int main() {
    try {
        {
            synth::runtime::WaitHoldClock hold;
            const auto now = synth::core::SnapshotClock::now();
            CHECK(!hold.started() && !hold.expired(now + 100s));
            hold.start(now);
            CHECK(!hold.expired(now + 89s)); CHECK(hold.expired(now + 90s));
            hold.pause(30s);
            CHECK(!hold.expired(now + 119s)); CHECK(hold.expired(now + 120s));
            hold.start(now + 125s); CHECK(!hold.expired(now + 214s)); CHECK(hold.expired(now + 215s));
        }
        {
            using Receipt = synth::runtime::WaitScriptReceipt;
            synth::core::CancellationSource session{synth::core::RuntimeGeneration::initial()};
            Receipt wait{"process-a-command-1", session.token(), true};
            CHECK(wait.current("process-a-command-1"));
            CHECK(!wait.current("old-process-command-1")); // saved/replayed stack cannot authorize a new process
            wait.finish("unrelated", true);
            CHECK(!wait.done());
            wait.revoke_install(); // Release/reissue while a latent marker move is pending
            CHECK(!wait.current("process-a-command-1"));
            wait.finish("process-a-command-1", true);
            CHECK(wait.done() && !wait.success());
            Receipt second{"second-npc", session.token(), true};
            CHECK(second.current("second-npc"));
            session.cancel(); // load/hard halt
            CHECK(!second.current("second-npc"));
            second.finish("second-npc", true);
            CHECK(second.done() && !second.success());
            Receipt release{"clear-owned-alias", session.token(), false};
            CHECK(release.current("clear-owned-alias")); // cancelled session must not prevent cleanup
            release.revoke_install();
            CHECK(release.current("clear-owned-alias"));
            release.finish("clear-owned-alias", true);
            CHECK(release.success());
            CHECK(!release.current("clear-owned-alias"));
            release.finish("clear-owned-alias", false);
            CHECK(release.success()); // duplicate/late completion cannot overwrite the first receipt
            Receipt reset{"new-world-reset", {}, false};
            CHECK(reset.current("new-world-reset"));
            Receipt invalid{"missing-session", {}, true};
            CHECK(!invalid.current("missing-session"));
        }
        using namespace synth;
        auto now = actions::ActionClock::now();
        actions::ActionProcessor processor{20, [&] { return now; }};
        const auto runtime = core::RuntimeGeneration::initial();
        const auto response = actions::ResponseGeneration::from_value(2);
        const actions::CancellationView active{[] { return false; }};
        Executor executor;
        executor.next = {actions::InspectExecutionStatus::success,
                         actions::InspectActorSnapshot{actor(), "Piper", {1, 2, 3}}, "ok"};
        actions::ActionIntent success = actions::InspectActorIntent{correlation(), actor(), now + 1s};
        const auto first = processor.process(success, runtime, response, active, executor);
        CHECK(first.status == actions::TerminalStatus::success);
        CHECK(first.correlation == correlation());
        CHECK(first.inspection->actor == actor());
        const auto replay = processor.process(success, runtime, response, active, executor);
        CHECK(replay == first);
        CHECK(executor.calls == 1);

        actions::ActionIntent unsupported = actions::UnsupportedIntent{correlation("unsupported"), "move_actor", now + 1s};
        CHECK(processor.process(unsupported, runtime, response, active, executor).status == actions::TerminalStatus::rejected);
        CHECK(processor.process(actions::InspectActorIntent{correlation("mismatch", 2), actor(), now + 1s},
                                runtime, response, active, executor).status == actions::TerminalStatus::rejected);
        CHECK(processor.process(actions::InspectActorIntent{correlation("cancel"), actor(), now + 1s},
                                runtime, response, actions::CancellationView{[] { return true; }}, executor).status ==
              actions::TerminalStatus::cancelled);
        CHECK(processor.process(actions::InspectActorIntent{correlation("timeout"), actor(), now},
                                runtime, response, active, executor).status == actions::TerminalStatus::timeout);

        executor.next = {actions::InspectExecutionStatus::unavailable, std::nullopt, "not loaded"};
        CHECK(processor.process(actions::InspectActorIntent{correlation("unavailable"), actor(), now + 1s},
                                runtime, response, active, executor).status == actions::TerminalStatus::unavailable);
        executor.next = {actions::InspectExecutionStatus::rejected, std::nullopt, "identity mismatch"};
        CHECK(processor.process(actions::InspectActorIntent{correlation("reject"), actor(), now + 1s},
                                runtime, response, active, executor).status == actions::TerminalStatus::rejected);
        executor.next = {actions::InspectExecutionStatus::failed, std::nullopt, "read failed"};
        CHECK(processor.process(actions::InspectActorIntent{correlation("fail"), actor(), now + 1s},
                                runtime, response, active, executor).status == actions::TerminalStatus::failed);
        executor.next = {actions::InspectExecutionStatus::success,
                         actions::InspectActorSnapshot{{{99, "Actors.esm"}, context::PlaythroughIdentity{"save-a"}},
                                                       "wrong", {}}, "bad"};
        CHECK(processor.process(actions::InspectActorIntent{correlation("wrong-actor"), actor(), now + 1s},
                                runtime, response, active, executor).status == actions::TerminalStatus::failed);
        CHECK(processor.terminal_count() == 9);
        // Receipt-relative timing must survive presentation/worker delay and the final game-thread handoff.
        const auto received = std::chrono::steady_clock::time_point{} + 1s;
        const auto expires = synth::runtime::action_start_deadline(100, 30000, received);
        CHECK(expires == received + 100ms);
        CHECK(synth::runtime::action_start_deadline(120000, 30000, received) == received + 30s);
        CHECK(synth::runtime::action_start_deadline(std::numeric_limits<std::uint64_t>::max(), 30000, received) == received + 30s);
        CHECK(synth::runtime::action_start_deadline(std::numeric_limits<std::uint64_t>::max(),
            std::numeric_limits<std::uint64_t>::max(), received) == received + 120s);
        core::CancellationSource owner{runtime};
        int mutations{};
        const auto mutate = [&] {
            ++mutations;
            return synth::runtime::RuntimeActionResult{synth::runtime::RuntimeActionStatus::succeeded, "observed"};
        };
        synth::runtime::ActionCompletion late;
        late.execute(mutate, owner.token(), expires, received + 10s);
        CHECK(mutations == 0);
        CHECK(late.phase == synth::runtime::ActionCompletion::Phase::complete);
        CHECK(late.result->status == synth::runtime::RuntimeActionStatus::timed_out);
        synth::runtime::ActionCompletion boundary;
        boundary.execute(mutate, owner.token(), expires, expires);
        CHECK(mutations == 0 && boundary.result->status == synth::runtime::RuntimeActionStatus::timed_out);
        synth::runtime::ActionCompletion fresh;
        fresh.execute(mutate, owner.token(), expires, expires - 1ms);
        fresh.execute(mutate, owner.token(), expires, expires - 1ms);
        CHECK(mutations == 1 && fresh.result->status == synth::runtime::RuntimeActionStatus::succeeded);
        synth::runtime::ActionCompletion abandoned;
        abandoned.phase = synth::runtime::ActionCompletion::Phase::abandoned;
        abandoned.execute(mutate, owner.token(), expires, received);
        CHECK(mutations == 1 && !abandoned.result);
        owner.cancel();
        synth::runtime::ActionCompletion cancelled;
        cancelled.execute(mutate, owner.token(), expires, received);
        CHECK(mutations == 1 && !cancelled.result);
        core::CancellationSource current_owner{runtime};
        synth::runtime::ActionCompletion failed;
        failed.execute([]() -> synth::runtime::RuntimeActionResult { throw std::runtime_error{"engine error"}; },
                       current_owner.token(), expires, received);
        CHECK(failed.phase == synth::runtime::ActionCompletion::Phase::complete);
        CHECK(failed.result->status == synth::runtime::RuntimeActionStatus::failed);
        CHECK(failed.result->detail == "engine error");
        // Frame-spanning execution claims once, keeps the result pending, and publishes only once.
        synth::runtime::ActionCompletion spanning;
        CHECK(!spanning.finish({synth::runtime::RuntimeActionStatus::succeeded, "too early"}));
        CHECK(spanning.begin(current_owner.token(), expires, received));
        CHECK(spanning.phase == synth::runtime::ActionCompletion::Phase::executing && !spanning.result);
        CHECK(!spanning.begin(current_owner.token(), expires, received));
        spanning.execute(mutate, current_owner.token(), expires, received);
        CHECK(mutations == 1);
        // Workers can inspect the pending operation between frames; begin did not retain its mutex.
        bool observed_pending{};
        std::thread observer{[&] {
            std::scoped_lock lock{spanning.mutex};
            observed_pending = spanning.phase == synth::runtime::ActionCompletion::Phase::executing && !spanning.result;
        }};
        observer.join(); CHECK(observed_pending);
        core::CancellationSource frame_owner{runtime};
        synth::runtime::ActionCompletion interrupted;
        CHECK(interrupted.begin(frame_owner.token(), expires, received));
        frame_owner.cancel();
        CHECK(interrupted.finish({synth::runtime::RuntimeActionStatus::failed, "interrupted after mutation"}));
        CHECK(interrupted.result->detail == "interrupted after mutation");
        CHECK(!interrupted.begin(frame_owner.token(), expires, received));
        CHECK(!interrupted.finish({synth::runtime::RuntimeActionStatus::succeeded, "late overwrite"}));
        CHECK(interrupted.result->detail == "interrupted after mutation");
        bool published_a{}, published_b{};
        std::thread publisher_a{[&] { published_a = spanning.finish({synth::runtime::RuntimeActionStatus::succeeded, "first"}); }};
        std::thread publisher_b{[&] { published_b = spanning.finish({synth::runtime::RuntimeActionStatus::failed, "second"}); }};
        publisher_a.join(); publisher_b.join();
        CHECK(published_a != published_b);
        CHECK(spanning.result && spanning.result->detail == (published_a ? "first" : "second"));
        CHECK(!abandoned.finish({synth::runtime::RuntimeActionStatus::failed, "abandoned"}));
        CHECK(!late.finish({synth::runtime::RuntimeActionStatus::succeeded, "expired overwrite"}));
        CHECK(late.result->status == synth::runtime::RuntimeActionStatus::timed_out);
        // Exact inventory binding must reject ambiguous names, stale IDs and partial name-only observations.
        const core::InventoryItemSnapshot rifle{0x123, "Fallout4.esm", "Combat rifle", 2, 0, 0.0, 0x2b, false};
        auto modified = rifle;
        modified.display_name = "Scoped combat rifle";
        std::vector<core::InventoryItemSnapshot> inventory{rifle, modified};
        CHECK(actions::select_equipment_item(inventory, "complete", "Combat rifle")->form_id == rifle.form_id);
        CHECK(!actions::select_equipment_item(inventory, "complete", "rifle"));
        CHECK(!actions::select_equipment_item(inventory, "complete", "combat rifle"));
        CHECK(!actions::select_equipment_item(inventory, "partial", "Combat rifle"));
        CHECK(actions::select_equipment_item(inventory, "partial", "0x00000123:Combat rifle")->display_name == "Combat rifle");
        CHECK(actions::select_equipment_item(inventory, "partial", "00000123:Combat rifle").has_value());
        CHECK(!actions::select_equipment_item(inventory, "complete", "0x00000123"));
        CHECK(!actions::select_equipment_item(inventory, "complete", "0x00000124:Combat rifle"));
        CHECK(!actions::select_equipment_item(inventory, "complete", "0x00000000"));
        CHECK(!actions::select_equipment_item(inventory, "complete", "0x00000123:"));
        CHECK(!actions::select_equipment_item(inventory, "complete", "0xZZZZ0123"));
        CHECK(!actions::select_equipment_item(inventory, "unavailable", "0x00000123:Combat rifle"));
        inventory.push_back(rifle);
        CHECK(!actions::select_equipment_item(inventory, "complete", "Combat rifle"));
        CHECK(!actions::select_equipment_item(inventory, "complete", "0x00000123:Combat rifle"));
        inventory.pop_back();
        auto equipped = rifle;
        equipped.count = 1;
        equipped.equipped = true;
        auto remaining = rifle;
        remaining.count = 1;
        std::vector<core::InventoryItemSnapshot> split{equipped, remaining, modified};
        CHECK(actions::equipment_postcondition(inventory, split, rifle, true));
        CHECK(actions::equipment_postcondition(split, inventory, rifle, false));
        CHECK(!actions::equipment_postcondition(inventory, inventory, rifle, true));
        CHECK(!actions::equipment_postcondition(split, split, rifle, false));
        split[1].count = 2;
        CHECK(!actions::equipment_postcondition(inventory, split, rifle, true));
        split[1].count = 1;
        split[0].origin_plugin = "Different.esp";
        CHECK(!actions::equipment_postcondition(inventory, split, rifle, true));
        split[0].origin_plugin = rifle.origin_plugin;
        split[0].form_id = 0x124;
        CHECK(!actions::equipment_postcondition(inventory, split, rifle, true));
        split[0].form_id = rifle.form_id;
        split[0].display_name = "Different rifle";
        CHECK(!actions::equipment_postcondition(inventory, split, rifle, true));
        CHECK(!actions::equipment_postcondition(inventory, {}, rifle, true));
        // Consumption success is exactly one selected ALCH unit, not just a true engine return.
        const core::InventoryItemSnapshot aid{0x200, "Fallout4.esm", "Purified water", 2, 0, 0.0, 0x30, false};
        auto named_aid = aid;
        named_aid.display_name = "Named water";
        std::vector<core::InventoryItemSnapshot> consume_before{aid, named_aid};
        auto consume_after = consume_before;
        consume_after[0].count = 1;
        CHECK(actions::consumption_postcondition(consume_before, consume_after, aid));
        CHECK(!actions::consumption_postcondition(consume_before, consume_before, aid));
        consume_after[0].count = 3;
        CHECK(!actions::consumption_postcondition(consume_before, consume_after, aid));
        consume_after.erase(consume_after.begin());
        CHECK(!actions::consumption_postcondition(consume_before, consume_after, aid));
        consume_after = consume_before;
        consume_after[1].count = 1;
        CHECK(!actions::consumption_postcondition(consume_before, consume_after, aid));
        consume_after[0].count = 1;
        CHECK(!actions::consumption_postcondition(consume_before, consume_after, aid));
        consume_after[1].count = 2;
        consume_after[1].display_name = "Unobserved water";
        CHECK(!actions::consumption_postcondition(consume_before, consume_after, aid));
        consume_after = {aid, named_aid};
        consume_after[0].count = 1;
        consume_after[1].form_id = 0x201;
        CHECK(!actions::consumption_postcondition(consume_before, consume_after, aid));
        consume_after[1] = named_aid;
        consume_after[1].origin_plugin = "Different.esp";
        CHECK(!actions::consumption_postcondition(consume_before, consume_after, aid));
        consume_after[1] = named_aid;
        consume_after[1].form_type = 0x2b;
        CHECK(!actions::consumption_postcondition(consume_before, consume_after, aid));
        consume_after[1] = named_aid;
        consume_after[1].equipped = true;
        CHECK(!actions::consumption_postcondition(consume_before, consume_after, aid));
        consume_after[1] = named_aid;
        consume_after[1].count = 0;
        CHECK(!actions::consumption_postcondition(consume_before, consume_after, aid));
        auto last_aid = aid;
        last_aid.count = 1;
        CHECK(actions::consumption_postcondition(std::vector{last_aid}, {}, last_aid));
        CHECK(!actions::consumption_postcondition({}, {}, last_aid));
        CHECK(!actions::consumption_postcondition(std::vector{aid}, {}, last_aid));
        CHECK(!actions::consumption_postcondition(std::vector{last_aid}, {}, aid));
        CHECK(!actions::consumption_postcondition(std::vector{rifle}, {}, rifle));
        CHECK(!actions::consumption_postcondition(std::vector{aid, aid}, std::vector{last_aid, aid}, aid));
        consume_after = {last_aid, named_aid, named_aid};
        consume_after[1].count = 1;
        consume_after[2].count = 1;
        CHECK(actions::consumption_postcondition(consume_before, consume_after, aid));
        consume_before[0].count = std::numeric_limits<std::uint32_t>::max();
        CHECK(!actions::consumption_postcondition(consume_before, consume_after, consume_before[0]));
        consume_before.assign(65, aid);
        CHECK(!actions::consumption_postcondition(consume_before, consume_after, aid));
        consume_after.assign(65, aid);
        CHECK(!actions::consumption_postcondition(std::vector{aid}, consume_after, aid));
        // Transfers require exact deltas on both owners, preserving names and equipped flags across stack merges.
        auto given = aid; given.count = 1;
        const std::vector donor_before{aid, named_aid}, donor_after{given, named_aid};
        const std::vector recipient_before{named_aid}, recipient_after{named_aid, given};
        CHECK(actions::transfer_postcondition(donor_before, donor_after, recipient_before, recipient_after, aid, 1));
        CHECK(actions::transfer_postcondition(std::vector{given}, {}, {}, std::vector{given}, given, 1));
        CHECK(!actions::transfer_postcondition(donor_before, donor_before, recipient_before, recipient_after, aid, 1));
        CHECK(!actions::transfer_postcondition(donor_before, donor_after, recipient_before, recipient_before, aid, 1));
        CHECK(!actions::transfer_postcondition(donor_before, {}, recipient_before, recipient_after, aid, 1));
        auto too_many = recipient_after; too_many.back().count = 2;
        CHECK(!actions::transfer_postcondition(donor_before, donor_after, recipient_before, too_many, aid, 1));
        for (unsigned amount : {0U, 3U, 1000001U, std::numeric_limits<unsigned>::max()})
            CHECK(!actions::transfer_postcondition(donor_before, donor_after, recipient_before, recipient_after, aid, amount));
        for (int mutation = 0; mutation < 7; ++mutation) {
            auto wrong = recipient_after;
            if (mutation == 0) wrong.back().form_id++;
            if (mutation == 1) wrong.back().origin_plugin = "Other.esp";
            if (mutation == 2) wrong.back().form_type++;
            if (mutation == 3) wrong.back().display_name = "Wrong named item";
            if (mutation == 4) wrong.back().equipped = true;
            if (mutation == 5) wrong.front().count--;
            if (mutation == 6) wrong.back().count = 2147483648U;
            CHECK(!actions::transfer_postcondition(donor_before, donor_after, recipient_before, wrong, aid, 1));
        }
        CHECK(!actions::transfer_postcondition(std::vector{aid, aid}, donor_after, recipient_before, recipient_after, aid, 1));
        CHECK(!actions::transfer_postcondition(donor_before, donor_after, recipient_before, recipient_after, given, 1));
        auto merged = aid; merged.count = 3;
        CHECK(actions::transfer_postcondition(donor_before, donor_after, std::vector{aid}, std::vector{merged}, aid, 1));
        CHECK(!actions::transfer_postcondition(donor_before, donor_after, std::vector(65, aid), recipient_after, aid, 1));
        // The post-action observation crosses the worker boundary by value and remains bound to its exact owner.
        core::CancellationSource observed_owner{runtime};
        runtime::RuntimeActionRequest observed_request{runtime::RuntimeActionName::equip_item,
            {0x15, "Fallout4.esm", "save-a"}, rifle, observed_owner.token(), now + 1s, "action:after"};
        runtime::RuntimeInventoryObservation observed{runtime, observed_request.actor, {equipped}, "complete", observed_request.action_id};
        CHECK(observed.valid_for(observed_request));
        auto transfer_request = observed_request;
        transfer_request.name = runtime::RuntimeActionName::give_item;
        transfer_request.recipient = runtime::RuntimeActorIdentity{0x16, "Fallout4.esm", "save-a"};
        runtime::RuntimeTransferObservation transferred{observed,
            {runtime, *transfer_request.recipient, {given}, "complete", observed_request.action_id}};
        CHECK(transferred.valid_for(transfer_request));
        CHECK(!transferred.valid_for(observed_request));
        runtime::ActionCompletion transfer_completion;
        transfer_completion.execute([&] {
            return runtime::RuntimeActionResult{runtime::RuntimeActionStatus::succeeded, "verified", std::nullopt, transferred};
        }, observed_owner.token(), expires, received);
        CHECK(transfer_completion.result->transfer->valid_for(transfer_request));
        for (int mutation = 0; mutation < 6; ++mutation) {
            auto wrong = transferred;
            if (mutation == 0) wrong.recipient.actor.form_id = observed_request.actor.form_id;
            if (mutation == 1) wrong.recipient.actor.playthrough_id = "other-save";
            if (mutation == 2) wrong.recipient.actor.origin_plugin = "Other.esp";
            if (mutation == 3) wrong.recipient.action_id = "other-action";
            if (mutation == 4) wrong.recipient.generation = core::RuntimeGeneration::from_value(2);
            if (mutation == 5) std::swap(wrong.donor, wrong.recipient);
            CHECK(!wrong.valid_for(transfer_request));
        }
        auto same_actor = transfer_request; same_actor.recipient = same_actor.actor;
        CHECK(!transferred.valid_for(same_actor));
        transferred.recipient.items.clear(); transferred.recipient.observation = "unavailable";
        CHECK(transferred.valid_for(transfer_request));
        CHECK(transfer_completion.result->transfer->recipient.items.size() == 1);
        CHECK(transfer_completion.result->transfer->recipient.observation == "complete");
        runtime::ActionCompletion observed_completion;
        observed_completion.execute([&] {
            return runtime::RuntimeActionResult{runtime::RuntimeActionStatus::succeeded, "verified", observed};
        }, observed_owner.token(), expires, received);
        observed.items.clear();
        CHECK(observed_completion.result->inventory->items.size() == 1);
        CHECK(observed_completion.result->inventory->items[0].equipped);
        CHECK(observed_completion.result->inventory->valid_for(observed_request));
        auto mismatched = *observed_completion.result->inventory;
        mismatched.action_id = "action:other";
        CHECK(!mismatched.valid_for(observed_request));
        mismatched = *observed_completion.result->inventory;
        mismatched.actor.form_id = 0x16;
        CHECK(!mismatched.valid_for(observed_request));
        mismatched = *observed_completion.result->inventory;
        mismatched.actor.origin_plugin = "Different.esp";
        CHECK(!mismatched.valid_for(observed_request));
        mismatched = *observed_completion.result->inventory;
        mismatched.actor.playthrough_id = "save-b";
        CHECK(!mismatched.valid_for(observed_request));
        mismatched = *observed_completion.result->inventory;
        mismatched.generation = core::RuntimeGeneration::from_value(2);
        CHECK(!mismatched.valid_for(observed_request));
        observed.observation = "unavailable";
        CHECK(observed.valid_for(observed_request));
        observed.items = {equipped};
        CHECK(!observed.valid());
        observed.observation = "partial";
        CHECK(observed.valid());
        observed.items[0].count = 0;
        CHECK(!observed.valid());
        observed.items.assign(513, equipped);
        CHECK(!observed.valid());
        observed.items.clear();
        observed.observation = "invented";
        CHECK(!observed.valid());
        observed.observation = "complete";
        observed.actor.origin_plugin = "../Fallout4.esm";
        CHECK(!observed.valid());
        // Continuations preserve the scene but replace the whole inventory, including automatic unequips.
        const auto pose=core::WorldPose{{},core::UnitVector3::from({1,0,0}),core::UnitVector3::from({0,0,1})};
        auto pistol=rifle;pistol.form_id=0x124;pistol.display_name="Pistol";pistol.count=1;pistol.equipped=true;
        const auto npc=core::ActorSnapshot{0x15,"Settler",{1,2,3},"Fallout4.esm","save-a"}.with_inventory({rifle,pistol},"complete");
        const auto other=core::ActorSnapshot{0x16,"Settler",{4,5,6},"Fallout4.esm","save-a"}.with_inventory({pistol},"partial");
        const auto original=std::make_shared<const core::RuntimeSnapshot>(core::Game::fallout4,core::RuntimeVariant::flat,runtime,
            12,now,pose,std::nullopt,std::nullopt,std::nullopt,core::ActorSnapshot{0x14,"Player",{},"Fallout4.esm","save-a"},
            std::vector{npc,other},123);
        protocol_native::Action wire_action{"action:after","idem:after","equip_item","action.equip_item",
            {"0x00000015","Fallout4.esm","save-a","Settler"},std::nullopt,json::Value{json::Value::Object{}},1000};
        pistol.equipped=false;
        auto post_inventory=*observed_completion.result->inventory;post_inventory.items={equipped,pistol};
        const auto view=client::prepare_action_inventory(original,wire_action,observed_owner.token(),post_inventory,true);
        CHECK(view.snapshot!=original && view.snapshot->frame()==original->frame());
        CHECK(view.snapshot->captured_at()==original->captured_at() && view.snapshot->game_time_ticks()==123);
        CHECK(view.snapshot->actors()[0].inventory()[0].equipped && !view.snapshot->actors()[0].inventory()[1].equipped);
        CHECK(view.inventory.items[0].equipped && !view.inventory.items[1].equipped);
        CHECK(!original->actors()[0].inventory()[0].equipped && original->actors()[0].inventory()[1].equipped);
        CHECK(view.snapshot->actors()[1].inventory()[0].equipped && view.snapshot->actors()[1].inventory_observation()=="partial");
        CHECK(view.snapshot->actors()[0].position().x==npc.position().x && view.snapshot->actors()[0].name()==npc.name());
        auto unequip=wire_action;unequip.name="unequip_item";unequip.capability="action.unequip_item";unequip.action_id="action:next";
        auto after=post_inventory;after.action_id=unequip.action_id;after.items[0].equipped=false;
        const auto next=client::prepare_action_inventory(view.snapshot,unequip,observed_owner.token(),after,true);
        CHECK(!next.snapshot->actors()[0].inventory()[0].equipped);
        CHECK(view.snapshot->actors()[0].inventory()[0].equipped);
        CHECK(actions::select_equipment_item(view.snapshot->actors()[0].inventory(),"complete","Combat rifle")->equipped);
        auto consume_action=wire_action;consume_action.name="consume";consume_action.capability="action.consume";
        consume_action.action_id="action:consume";
        auto consumed=post_inventory;consumed.action_id=consume_action.action_id;consumed.items={last_aid};
        const auto consumed_view=client::prepare_action_inventory(view.snapshot,consume_action,observed_owner.token(),consumed,true);
        CHECK(consumed_view.inventory.items.size()==1 && consumed_view.inventory.items[0].count==1);
        CHECK(consumed_view.snapshot->actors()[0].inventory()[0].form_id==last_aid.form_id);
        CHECK(consumed_view.snapshot->actors()[1].inventory()[0].form_id==pistol.form_id);
        consumed.items.clear();
        const auto last_consumed=client::prepare_action_inventory(consumed_view.snapshot,consume_action,observed_owner.token(),consumed,true);
        CHECK(last_consumed.inventory.items.empty() && last_consumed.inventory.observation=="complete");
        CHECK(last_consumed.snapshot->actors()[0].inventory().empty() && consumed_view.snapshot->actors()[0].inventory().size()==1);
        consumed.action_id="wrong";
        CHECK(client::prepare_action_inventory(consumed_view.snapshot,consume_action,observed_owner.token(),consumed,true).inventory.observation=="unavailable");
        for (int bad=0;bad<4;++bad) {
            auto missing=post_inventory;
            if(bad==0) missing.action_id="other";
            if(bad==1) missing.actor.form_id=0x16;
            if(bad==2) missing.actor.playthrough_id="save-b";
            if(bad==3) missing.generation=core::RuntimeGeneration::from_value(2);
            const auto unknown=client::prepare_action_inventory(view.snapshot,wire_action,observed_owner.token(),missing,true);
            CHECK(unknown.inventory.observation=="unavailable" && unknown.inventory.items.empty());
            CHECK(unknown.snapshot->actors()[0].inventory().empty());
        }
        const auto unknown=client::prepare_action_inventory(view.snapshot,wire_action,observed_owner.token(),std::nullopt,true);
        CHECK(unknown.inventory.observation=="unavailable" && unknown.snapshot->actors()[0].inventory().empty());
        post_inventory.items.assign(33,equipped);
        const auto bounded=client::prepare_action_inventory(original,wire_action,observed_owner.token(),post_inventory,false);
        CHECK(bounded.inventory.items.size()==32 && bounded.inventory.observation=="partial");
        CHECK(bounded.snapshot->actors()[0].inventory().size()==32);
        post_inventory.items={equipped};post_inventory.items[0].count=2147483648U;
        CHECK(client::prepare_action_inventory(original,wire_action,observed_owner.token(),post_inventory,true).inventory.observation=="unavailable");
        auto give=wire_action;give.name="give_item_to";give.capability="action.give_item_to";give.action_id="action:give";
        give.target=protocol_native::Identity{"0x00000016","Fallout4.esm","save-a","Another label"};
        give.arguments=json::parse(R"({"item":"Combat rifle","amount":2})");
        const auto requested=client::prepare_transfer_action(*original,give,observed_owner.token(),now+1s);
        CHECK(requested.name==runtime::RuntimeActionName::give_item && requested.amount==2);
        CHECK(requested.actor.form_id==0x15 && requested.recipient->form_id==0x16);
        CHECK(requested.item->form_id==rifle.form_id && requested.item->count==2 && !requested.item->equipped);
        CHECK(requested.action_id==give.action_id && requested.deadline==now+1s);
        auto one=give;one.arguments=json::parse(R"({"item":"Combat rifle"})");
        CHECK(client::prepare_transfer_action(*original,one,observed_owner.token(),now+1s).amount==1);
        for(const auto* arguments:{R"({})",R"({"item":"Combat"})",R"({"item":"Pistol"})",R"({"item":"Unknown"})",
            R"({"item":"Combat rifle","amount":0})",R"({"item":"Combat rifle","amount":-1})",R"({"item":"Combat rifle","amount":3})",
            R"({"item":"Combat rifle","amount":1000001})",R"({"item":"Combat rifle","amount":1.0})",R"({"item":"Combat rifle","amount":"1"})",
            R"({"item":"Combat rifle","amount":true})",R"({"item":"Combat rifle","amount":null})",R"({"item":"Combat rifle","target_name":"Settler"})",
            R"({"item":"Combat rifle","amount":1,"extra":true})",R"({"item":1})",R"([])"}) {
            auto bad=give;bad.arguments=json::parse(arguments);bool refused=false;
            try{(void)client::prepare_transfer_action(*original,bad,observed_owner.token(),now+1s);}catch(const std::invalid_argument&){refused=true;}
            CHECK(refused);
        }
        auto partial_scene=std::make_shared<const core::RuntimeSnapshot>(original->with_actor_inventory(0x15,"Fallout4.esm","save-a",{rifle},"partial"));
        bool partial_name_refused=false;
        try{(void)client::prepare_transfer_action(*partial_scene,give,observed_owner.token(),now+1s);}catch(const std::invalid_argument&){partial_name_refused=true;}
        CHECK(partial_name_refused);
        auto explicit_item=give;explicit_item.arguments=json::parse(R"({"item":"0x00000123:Combat rifle","amount":2})");
        CHECK(client::prepare_transfer_action(*partial_scene,explicit_item,observed_owner.token(),now+1s).amount==2);
        auto ambiguous_scene=std::make_shared<const core::RuntimeSnapshot>(original->with_actor_inventory(0x15,"Fallout4.esm","save-a",{rifle,rifle},"complete"));
        bool ambiguous_item_refused=false;
        try{(void)client::prepare_transfer_action(*ambiguous_scene,explicit_item,observed_owner.token(),now+1s);}catch(const std::invalid_argument&){ambiguous_item_refused=true;}
        CHECK(ambiguous_item_refused);
        auto many=rifle;many.count=1'000'001;
        const auto many_scene=original->with_actor_inventory(0x15,"Fallout4.esm","save-a",{many},"complete");
        auto maximum=give;maximum.arguments=json::parse(R"({"item":"Combat rifle","amount":1000000})");
        CHECK(client::prepare_transfer_action(many_scene,maximum,observed_owner.token(),now+1s).amount==1'000'000);
        maximum.arguments=json::parse(R"({"item":"Combat rifle","amount":1000001})");
        bool over_limit=false;
        try{(void)client::prepare_transfer_action(many_scene,maximum,observed_owner.token(),now+1s);}catch(const std::invalid_argument&){over_limit=true;}
        CHECK(over_limit);
        auto caps_item=rifle;caps_item.form_id=0xF;caps_item.form_type=0x23;caps_item.count=100;
        caps_item.display_name="Tappo personalizzato";
        auto caps=give;caps.name="give_caps_to";caps.capability="action.give_caps_to";caps.action_id="action:caps";
        caps.arguments=json::parse(R"({"amount":75})");
        const auto caps_scene=original->with_actor_inventory(0x15,"Fallout4.esm","save-a",{caps_item},"partial");
        const auto caps_request=client::prepare_transfer_action(caps_scene,caps,observed_owner.token(),now+1s);
        CHECK(caps_request.name==runtime::RuntimeActionName::give_caps && caps_request.amount==75);
        CHECK(caps_request.item->form_id==0xF && caps_request.item->display_name==caps_item.display_name);
        CHECK(caps_request.actor.form_id==0x15 && caps_request.recipient->form_id==0x16);
        auto caps_player=caps;caps_player.target=protocol_native::Identity{"0x00000014","Fallout4.esm","save-a","Player"};
        CHECK(client::prepare_transfer_action(caps_scene,caps_player,observed_owner.token(),now+1s).recipient->form_id==0x14);
        for (const auto arguments:{R"({})",R"({"amount":0})",R"({"amount":-1})",R"({"amount":101})",
            R"({"amount":1000001})",R"({"amount":1.0})",R"({"amount":"1"})",R"({"amount":true})",
            R"({"amount":null})",R"({"amount":1,"item":"Combat rifle"})",R"({"amount":1,"target":"Settler"})",R"([])"}) {
            auto bad=caps;bad.arguments=json::parse(arguments);bool refused=false;
            try{(void)client::prepare_transfer_action(caps_scene,bad,observed_owner.token(),now+1s);}catch(const std::invalid_argument&){refused=true;}
            CHECK(refused);
        }
        for (int bad=0;bad<7;++bad) {
            auto changed=caps_item;
            if(bad==0)changed.form_id=0x123; // A display-name match is not currency identity.
            if(bad==1)changed.origin_plugin="Other.esm";
            if(bad==2)changed.form_type=0x2B;
            if(bad==3)changed.equipped=true;
            if(bad==4)changed.count=2147483648U;
            std::vector<core::InventoryItemSnapshot> rows{changed};
            if(bad==5)rows.push_back(changed);
            if(bad==6)rows.clear();
            CHECK(!actions::select_caps_item(rows,"complete"));
        }
        CHECK(!actions::select_caps_item(std::span{&caps_item,1},"unavailable"));
        auto mismatched_caps=caps;mismatched_caps.capability="action.give_item_to";
        bool mismatch_refused=false;
        try{(void)client::prepare_transfer_action(caps_scene,mismatched_caps,observed_owner.token(),now+1s);}catch(const std::invalid_argument&){mismatch_refused=true;}
        CHECK(mismatch_refused);
        auto caps_after=caps_item;caps_after.count=25;
        auto caps_received=caps_item;caps_received.count=75;
        runtime::RuntimeTransferObservation caps_pair{{runtime,caps_request.actor,{caps_after},"complete",caps.action_id},
            {runtime,*caps_request.recipient,{caps_received},"partial",caps.action_id}};
        CHECK(caps_pair.valid_for(caps_request));
        const auto caps_snapshot=std::make_shared<const core::RuntimeSnapshot>(caps_scene);
        const auto caps_view=client::prepare_transfer_inventory(caps_snapshot,caps,observed_owner.token(),caps_pair,true);
        CHECK(caps_view.transfer.donor.items[0].count==25 && caps_view.transfer.recipient.items[0].count==75);
        CHECK(caps_view.snapshot->actors()[0].inventory()[0].count==25 && caps_scene.actors()[0].inventory()[0].count==100);
        CHECK(caps_view.transfer.recipient.observation=="partial");
        caps_pair.recipient.action_id="wrong-caps-action";
        CHECK(!caps_pair.valid_for(caps_request));
        CHECK(client::prepare_transfer_inventory(caps_snapshot,caps,observed_owner.token(),caps_pair,true).transfer.donor.observation=="unavailable");
        // Taking payment keeps the NPC as performer, but binds donor rows and native mutation to the exact player.
        auto take=caps_player;take.name="take_caps_from_player";take.capability="action.take_caps_from_player";
        take.action_id="action:take-caps";
        const auto payer_scene=std::make_shared<const core::RuntimeSnapshot>(caps_scene.with_player_inventory(
            caps_scene.player().with_inventory({caps_item},"complete")));
        const auto payment=client::prepare_transfer_action(*payer_scene,take,observed_owner.token(),now+1s);
        CHECK(payment.name==runtime::RuntimeActionName::take_caps && payment.actor.form_id==0x14);
        CHECK(payment.recipient->form_id==0x15 && payment.amount==75 && payment.item->form_id==0xF);
        CHECK(payment.action_id==take.action_id && payment.deadline==now+1s && take.actor.form_id=="0x00000015");
        auto paid=caps_item;paid.count=175;
        runtime::RuntimeTransferObservation payment_pair{{runtime,payment.actor,{caps_after},"complete",take.action_id},
            {runtime,*payment.recipient,{paid},"partial",take.action_id}};
        CHECK(payment_pair.valid_for(payment));
        const auto payment_view=client::prepare_transfer_inventory(payer_scene,take,observed_owner.token(),payment_pair,true);
        CHECK(payment_view.transfer.donor.actor.form_id=="0x00000014" && payment_view.transfer.recipient.actor.form_id=="0x00000015");
        CHECK(payment_view.snapshot->player().inventory()[0].count==25 && payment_view.transfer.donor.items[0].count==25);
        CHECK(payment_view.snapshot->actors()[0].inventory()[0].count==175 && payment_view.transfer.recipient.items[0].count==175);
        CHECK(payment_view.snapshot->actors()[0].inventory_observation()=="partial");
        CHECK(payer_scene->player().inventory()[0].count==100 && payer_scene->actors()[0].inventory()[0].count==100);
        CHECK(payment_view.snapshot->actors()[1].inventory()[0].form_id==payer_scene->actors()[1].inventory()[0].form_id);
        CHECK(payment_view.snapshot->frame()==payer_scene->frame() && payment_view.snapshot->captured_at()==payer_scene->captured_at());
        CHECK(payment_view.snapshot->game_time_ticks()==payer_scene->game_time_ticks());
        auto wrong_payment=payment;wrong_payment.name=runtime::RuntimeActionName::give_caps;
        CHECK(!payment_pair.valid_for(wrong_payment));
        wrong_payment=payment;wrong_payment.actor=*payment.recipient;
        CHECK(!payment_pair.valid_for(wrong_payment));
        wrong_payment=payment;wrong_payment.actor.form_id=0x16;
        auto npc_payment_pair=payment_pair;npc_payment_pair.donor.actor=wrong_payment.actor;
        CHECK(!npc_payment_pair.valid_for(wrong_payment)); // A fully matching NPC-to-NPC receipt is not player payment.
        for (const auto arguments:{R"({})",R"({"amount":0})",R"({"amount":-1})",R"({"amount":101})",
            R"({"amount":1000001})",R"({"amount":1.0})",R"({"amount":"1"})",R"({"amount":true})",
            R"({"amount":null})",R"({"amount":1,"item":"Caps"})",R"({"amount":1,"target":"Player"})",R"([])"}) {
            auto bad=take;bad.arguments=json::parse(arguments);bool refused=false;
            try{(void)client::prepare_transfer_action(*payer_scene,bad,observed_owner.token(),now+1s);}catch(const std::invalid_argument&){refused=true;}
            CHECK(refused);
        }
        for (int bad=0;bad<9;++bad) {
            auto changed=caps_item;
            if(bad==0)changed.form_id=0x123;
            if(bad==1)changed.origin_plugin="Other.esm";
            if(bad==2)changed.form_type=0x2B;
            if(bad==3)changed.equipped=true;
            if(bad==4)changed.count=2147483648U;
            if(bad==8)changed.count=74;
            std::vector<core::InventoryItemSnapshot> rows{changed};
            if(bad==5)rows.push_back(changed);
            if(bad==6 || bad==7)rows.clear();
            const auto scene=payer_scene->with_player_inventory(payer_scene->player().with_inventory(
                std::move(rows),bad==7?"unavailable":"complete"));
            bool refused=false;
            try{(void)client::prepare_transfer_action(scene,take,observed_owner.token(),now+1s);}catch(const std::invalid_argument&){refused=true;}
            CHECK(refused); // The NPC's sufficient inventory cannot stand in for missing player funds.
        }
        const auto partial_payer=payer_scene->with_player_inventory(payer_scene->player().with_inventory({caps_item},"partial"));
        CHECK(client::prepare_transfer_action(partial_payer,take,observed_owner.token(),now+1s).amount==75);
        for (int bad=0;bad<10;++bad) {
            auto wrong=take;
            if(bad==0)wrong.target.reset();
            if(bad==1)wrong.target=caps.target;
            if(bad==2)wrong.target=wrong.actor;
            if(bad==3)wrong.actor=*take.target;
            if(bad==4)wrong.target->origin_plugin="Other.esm";
            if(bad==5)wrong.target->playthrough_id="other-save";
            if(bad==6)wrong.target->form_id="0x00000099";
            if(bad==7)wrong.actor.origin_plugin="Other.esm";
            if(bad==8)wrong.capability="action.give_caps_to";
            if(bad==9)wrong.action_id.clear();
            bool refused=false;
            try{(void)client::prepare_transfer_action(*payer_scene,wrong,observed_owner.token(),now+1s);}catch(const std::invalid_argument&){refused=true;}
            CHECK(refused);
        }
        for (int bad=0;bad<7;++bad) {
            auto wrong=payment_pair;
            if(bad==0)wrong.donor.action_id="other";
            if(bad==1)wrong.recipient.actor.form_id=0x16;
            if(bad==2)wrong.donor.actor.origin_plugin="Other.esm";
            if(bad==3)wrong.recipient.actor.playthrough_id="other-save";
            if(bad==4)wrong.donor.generation=core::RuntimeGeneration::from_value(2);
            if(bad==5)std::swap(wrong.donor,wrong.recipient);
            if(bad==6)wrong.recipient.action_id="other";
            const auto missing=client::prepare_transfer_inventory(payer_scene,take,observed_owner.token(),wrong,true);
            CHECK(missing.transfer.donor.observation=="unavailable" && missing.transfer.recipient.observation=="unavailable");
            CHECK(missing.snapshot->player().inventory().empty() && missing.snapshot->actors()[0].inventory().empty());
        }
        auto unknown_payment=payment_pair;unknown_payment.donor.items.clear();unknown_payment.donor.observation="unavailable";
        const auto unknown_payer=client::prepare_transfer_inventory(payer_scene,take,observed_owner.token(),unknown_payment,true);
        CHECK(unknown_payer.transfer.donor.observation=="unavailable" && unknown_payer.transfer.recipient.items[0].count==175);
        unknown_payment=payment_pair;unknown_payment.recipient.items.clear();unknown_payment.recipient.observation="unavailable";
        const auto unknown_payee=client::prepare_transfer_inventory(payer_scene,take,observed_owner.token(),unknown_payment,true);
        CHECK(unknown_payee.transfer.donor.items[0].count==25 && unknown_payee.transfer.recipient.observation=="unavailable");
        for(int bad=0;bad<3;++bad) {
            auto actors=std::vector{payer_scene->actors()[0],payer_scene->actors()[1]};
            if(bad==0)actors.push_back(payer_scene->player());
            auto wrong=std::make_shared<const core::RuntimeSnapshot>(core::Game::fallout4,
                bad==1?core::RuntimeVariant::vr:core::RuntimeVariant::flat,
                bad==2?core::RuntimeGeneration::from_value(2):runtime,12,now,pose,std::nullopt,std::nullopt,std::nullopt,
                payer_scene->player(),std::move(actors),123);
            bool refused=false;
            try{(void)client::prepare_transfer_action(*wrong,take,observed_owner.token(),now+1s);}catch(const std::invalid_argument&){refused=true;}
            CHECK(refused);
        }
        runtime::RuntimeTransferObservation pair{{runtime,requested.actor,{},"complete",give.action_id},
            {runtime,*requested.recipient,{rifle},"complete",give.action_id}};
        const auto paired=client::prepare_transfer_inventory(original,give,observed_owner.token(),pair,true);
        CHECK(paired.snapshot->actors()[0].inventory().empty() && paired.transfer.donor.items.empty());
        CHECK(paired.snapshot->actors()[0].inventory_observation()=="complete" && paired.transfer.donor.observation=="complete");
        CHECK(paired.snapshot->actors()[1].inventory()[0].count==2 && paired.transfer.recipient.items[0].count==2);
        CHECK(paired.snapshot->actors()[1].name()=="Settler" && paired.snapshot->actors()[1].position().x==4);
        CHECK(paired.snapshot->frame()==original->frame() && paired.snapshot->captured_at()==original->captured_at());
        CHECK(paired.snapshot->game_time_ticks()==123 && paired.snapshot->generation()==original->generation());
        CHECK(original->actors()[0].inventory().size()==2 && original->actors()[1].inventory()[0].form_id==pistol.form_id);
        for(int bad=0;bad<6;++bad) {
            auto wrong=pair;
            if(bad==0)wrong.donor.action_id="other";
            if(bad==1)wrong.recipient.actor.form_id=0x99;
            if(bad==2)wrong.recipient.actor.origin_plugin="Other.esp";
            if(bad==3)wrong.recipient.actor.playthrough_id="other-save";
            if(bad==4)wrong.recipient.generation=core::RuntimeGeneration::from_value(2);
            if(bad==5)std::swap(wrong.donor,wrong.recipient);
            const auto missing=client::prepare_transfer_inventory(paired.snapshot,give,observed_owner.token(),wrong,true);
            CHECK(missing.transfer.donor.observation=="unavailable" && missing.transfer.recipient.observation=="unavailable");
            CHECK(missing.snapshot->actors()[0].inventory().empty() && missing.snapshot->actors()[1].inventory().empty());
        }
        const auto absent=client::prepare_transfer_inventory(paired.snapshot,give,observed_owner.token(),std::nullopt,true);
        CHECK(absent.transfer.donor.observation=="unavailable" && absent.transfer.recipient.observation=="unavailable");
        for(int bad=0;bad<4;++bad) {
            auto actors=std::vector{npc,other};
            if(bad==0)actors.push_back(other);
            if(bad==1)actors.emplace_back(0x16,"Conflicting plugin",core::Vec3{},"Other.esp","save-a");
            auto wrong=std::make_shared<const core::RuntimeSnapshot>(core::Game::fallout4,
                bad==2?core::RuntimeVariant::vr:core::RuntimeVariant::flat,
                bad==3?core::RuntimeGeneration::from_value(2):runtime,12,now,pose,std::nullopt,std::nullopt,std::nullopt,
                original->player(),std::move(actors),123);
            bool refused=false;
            try{(void)client::prepare_transfer_inventory(wrong,give,observed_owner.token(),pair,true);}catch(const std::invalid_argument&){refused=true;}
            CHECK(refused);
        }
        auto failed_pair=pair;failed_pair.recipient.items.clear();failed_pair.recipient.observation="unavailable";
        const auto failed_recipient=client::prepare_transfer_inventory(original,give,observed_owner.token(),failed_pair,true);
        CHECK(failed_recipient.transfer.donor.observation=="complete" && failed_recipient.transfer.recipient.observation=="unavailable");
        failed_pair=pair;failed_pair.recipient.items[0].count=2147483648U;
        const auto malformed_recipient=client::prepare_transfer_inventory(original,give,observed_owner.token(),failed_pair,true);
        CHECK(malformed_recipient.transfer.donor.observation=="complete" && malformed_recipient.transfer.recipient.observation=="unavailable");
        CHECK(malformed_recipient.snapshot->actors()[1].inventory().empty());
        failed_pair=pair;failed_pair.recipient.items[0].display_name=std::string(1,static_cast<char>(0xff));
        const auto invalid_text=client::prepare_transfer_inventory(original,give,observed_owner.token(),failed_pair,true);
        CHECK(invalid_text.transfer.donor.observation=="complete" && invalid_text.transfer.recipient.observation=="unavailable");
        CHECK(invalid_text.snapshot->actors()[1].inventory().empty());
        auto to_player=give;to_player.target=protocol_native::Identity{"0x00000014","Fallout4.esm","save-a","Player"};
        CHECK(client::prepare_transfer_action(*original,to_player,observed_owner.token(),now+1s).recipient->form_id==0x14);
        auto player_pair=pair;player_pair.recipient.actor.form_id=0x14;
        const auto player_view=client::prepare_transfer_inventory(original,to_player,observed_owner.token(),player_pair,true);
        CHECK(player_view.snapshot->player().inventory()[0].count==2 && player_view.transfer.recipient.items[0].count==2);
        CHECK(player_view.snapshot->actors()[1].inventory()[0].form_id==pistol.form_id);
        CHECK(player_view.snapshot->player().name()==original->player().name() && original->player().inventory().empty());
        for(int bad=0;bad<9;++bad) {
            auto wrong=give;
            if(bad==0)wrong.target.reset();
            if(bad==1)wrong.target=wrong.actor;
            if(bad==2)wrong.actor=*to_player.target;
            if(bad==3)wrong.target->form_id="0x00000099";
            if(bad==4)wrong.target->origin_plugin="Other.esp";
            if(bad==5)wrong.target->playthrough_id="other-save";
            if(bad==6)wrong.target->form_id="0xGG000016";
            if(bad==7)wrong.name="equip_item";
            if(bad==8)wrong.capability="action.give_item";
            bool refused=false;
            try{(void)client::prepare_transfer_inventory(original,wrong,observed_owner.token(),pair,true);}catch(const std::invalid_argument&){refused=true;}
            CHECK(refused);
        }
        auto large_pair=player_pair;large_pair.donor.items.assign(512,rifle);large_pair.recipient.items.assign(512,rifle);
        const auto legacy_pair=client::prepare_transfer_inventory(original,to_player,observed_owner.token(),large_pair,false);
        CHECK(legacy_pair.transfer.donor.items.size()==32 && legacy_pair.transfer.recipient.items.size()==32);
        CHECK(legacy_pair.snapshot->actors()[0].inventory().size()==32 && legacy_pair.snapshot->player().inventory().size()==32);
        CHECK(legacy_pair.transfer.donor.observation=="partial" && legacy_pair.transfer.recipient.observation=="partial");
        const auto full_pair=client::prepare_transfer_inventory(original,to_player,observed_owner.token(),large_pair,true);
        CHECK(full_pair.transfer.donor.items.size()==512 && full_pair.transfer.recipient.items.size()==512);
        CHECK(full_pair.transfer.donor.observation=="complete" && full_pair.transfer.recipient.observation=="complete");
        for(auto* side:{&large_pair.donor,&large_pair.recipient}) for(auto& item:side->items) {
            item.display_name=std::string(255,'\x01');item.origin_plugin=std::string(251,'\x01')+".esm";
        }
        const auto byte_bounded=client::prepare_transfer_inventory(original,to_player,observed_owner.token(),large_pair,true);
        CHECK(byte_bounded.transfer.donor.items.size()<512 || byte_bounded.transfer.recipient.items.size()<512);
        CHECK(byte_bounded.snapshot->actors()[0].inventory().size()==byte_bounded.transfer.donor.items.size());
        CHECK(byte_bounded.snapshot->player().inventory().size()==byte_bounded.transfer.recipient.items.size());
        CHECK(large_pair.donor.items.size()==512 && large_pair.recipient.items.size()==512);
        const auto same_rows=[](const core::ActorSnapshot& local,const protocol_native::ActionInventoryObservation& wire) {
            CHECK(local.inventory_observation()==wire.observation && local.inventory().size()==wire.items.size());
            CHECK(std::ranges::equal(local.inventory(),wire.items,[](const auto& a,const auto& b) {
                char form[11]{};std::snprintf(form,sizeof(form),"0x%08X",a.form_id);
                return form==b.form_id && a.origin_plugin==b.origin_plugin && a.display_name==b.display_name &&
                    a.count==b.count && a.value==b.value && a.weight==b.weight && a.form_type==b.form_type && a.equipped==b.equipped;
            }));
        };
        same_rows(byte_bounded.snapshot->actors()[0],byte_bounded.transfer.donor);
        same_rows(byte_bounded.snapshot->player(),byte_bounded.transfer.recipient);
        same_rows(paired.snapshot->actors()[0],paired.transfer.donor);
        same_rows(paired.snapshot->actors()[1],paired.transfer.recipient);
        protocol_native::ActionResultEvent receipt{std::string(128,'s'),std::string(128,'r'),std::string(128,'t'),"flat",
            std::string(64,'v'),std::string(64,'v'),1,{"action.give_item_to","action.transfer_inventory","action.inventory_observation",
                "dialogue.turn_ownership","context.inventory_512"},give.action_id,give.idempotency_key,"failed",std::string(1024,'\x01'),2,7,
            std::nullopt,byte_bounded.transfer};
        for(int cap=5;cap<128;++cap)receipt.capabilities.push_back(std::string(60,'x')+std::to_string(cap));
        const auto encoded_pair=protocol_native::encode_action_result(receipt);
        CHECK(encoded_pair.size()<=1024*1024);
        const auto encoded_value=json::parse(encoded_pair);
        const auto& encoded_result=protocol_native::required(protocol_native::required(encoded_value,"payload"),"result");
        CHECK(protocol_native::required(protocol_native::required(encoded_result,"inventory"),"items").as_array().size()==byte_bounded.snapshot->actors()[0].inventory().size());
        CHECK(protocol_native::required(protocol_native::required(encoded_result,"recipient_inventory"),"items").as_array().size()==byte_bounded.snapshot->player().inventory().size());
        // Pickup preparation binds an exact world RefID/name; similarly named actors/items cannot stand in.
        const core::NearbyItemSnapshot dropped{0xFF001234, rifle.form_id, 0x18AA,
            std::nullopt, "Fallout4.esm", "Fallout4.esm", rifle.display_name, {10, 20, 0}, 25, 3.0, 3, 20, 0x2B};
        const auto pickup_scene = [&](std::vector<core::NearbyItemSnapshot> items, std::string quality,
                                      std::vector<core::ActorSnapshot> actors, core::RuntimeVariant lane = core::RuntimeVariant::flat,
                                      core::RuntimeGeneration generation = core::RuntimeGeneration::initial()) {
            return core::RuntimeSnapshot{core::Game::fallout4, lane, generation, 12, now, pose,
                std::nullopt, std::nullopt, std::nullopt, original->player(), std::move(actors), 123,
                std::nullopt, {}, {}, std::move(items), {}, "unavailable", std::move(quality)};
        };
        const auto ground = pickup_scene({dropped}, "complete", {npc, other});
        protocol_native::Action pickup{"action:pickup", "idem:pickup", "pickup_item", "action.pickup_item",
            wire_action.actor, std::nullopt, json::parse(R"({"item":"0xFF001234:Combat rifle"})"), 1000};
        const auto pickup_request = client::prepare_pickup_action(ground, pickup, observed_owner.token(), now + 1s);
        CHECK(pickup_request.actor.form_id == 0x15 && pickup_request.item.reference_id == dropped.reference_id);
        CHECK(pickup_request.item.base_form_id == rifle.form_id && pickup_request.item.count == 3);
        CHECK(!pickup_request.item.reference_origin_plugin && pickup_request.item.cell_form_id == dropped.cell_form_id);
        CHECK(pickup_request.action_id == pickup.action_id && pickup_request.context_sequence == 12);
        CHECK(pickup_request.captured_at == now && pickup_request.deadline == now + 1s);
        CHECK(pickup_request.cancellation.generation() == runtime && ground.nearby_items()[0].count == 3);
        CHECK(actions::pickup_reference_matches(dropped, dropped));
        auto moved_reference = dropped;
        moved_reference.position = {120, 30, 1}; moved_reference.distance = 125;
        moved_reference.looking_at = !dropped.looking_at; moved_reference.stealing = !dropped.stealing;
        CHECK(actions::pickup_reference_matches(dropped, moved_reference)); // Player crime/presentation is not NPC permission.
        for (int mismatch = 0; mismatch < 13; ++mismatch) {
            auto changed = dropped;
            if (mismatch == 0) ++changed.reference_id;
            if (mismatch == 1) ++changed.base_form_id;
            if (mismatch == 2) ++changed.cell_form_id;
            if (mismatch == 3) changed.reference_origin_plugin = "Other.esp";
            if (mismatch == 4) changed.base_origin_plugin = "Other.esp";
            if (mismatch == 5) changed.cell_origin_plugin = "Other.esp";
            if (mismatch == 6) ++changed.form_type;
            if (mismatch == 7) changed.display_name = "Renamed rifle";
            if (mismatch == 8) ++changed.count;
            if (mismatch == 9) changed.held = true;
            if (mismatch == 10) changed.count = 0;
            if (mismatch == 11) changed.position.x = std::numeric_limits<double>::quiet_NaN();
            if (mismatch == 12) changed.distance = -1;
            CHECK(!actions::pickup_reference_matches(dropped, changed));
            CHECK(!actions::pickup_reference_matches(changed, dropped));
        }
        using Progress = synth::runtime::PickupProgress;
        using Command = Progress::Command;
        using Inspection = Progress::Inspection;
        Progress travel{pickup_request};
        CHECK(travel.advance(12, runtime, now, Inspection::far) == Command::none);
        CHECK(travel.advance(13, runtime, now, Inspection::far) == Command::install);
        CHECK(travel.cleanup_owed() && !travel.transfer_attempted());
        CHECK(travel.advance(14, runtime, now, Inspection::near) == Command::none);
        CHECK(travel.installation_finished(Progress::Installation::installed));
        CHECK(!travel.installation_finished(Progress::Installation::installed));
        CHECK(travel.advance(15, runtime, now, Inspection::far) == Command::none);
        CHECK(travel.advance(16, runtime, now, Inspection::near) == Command::transfer);
        CHECK(travel.transfer_attempted());
        CHECK(travel.advance(17, runtime, now, Inspection::near) == Command::none);
        CHECK(travel.transfer_finished()); CHECK(!travel.transfer_finished());
        CHECK(travel.advance(18, runtime, now, Inspection::near) == Command::observe);
        CHECK(travel.observation_finished(true) && !travel.done());
        CHECK(travel.advance(19, runtime, now + 2s, Inspection::unavailable) == Command::restore);
        CHECK(travel.restoration_finished(false) && !travel.done());
        CHECK(travel.advance(19, runtime, now + 2s, Inspection::unavailable) == Command::none);
        CHECK(travel.advance(20, runtime, now + 2s, Inspection::unavailable) == Command::restore);
        CHECK(travel.restoration_finished(true) && travel.done() && !travel.cleanup_owed());
        CHECK(travel.outcome() == Progress::Outcome::succeeded);
        CHECK(travel.advance(21, runtime, now + 2s, Inspection::near) == Command::none);
        Progress nearby{pickup_request};
        CHECK(nearby.advance(13, runtime, now, Inspection::near) == Command::transfer);
        CHECK(!nearby.cleanup_owed() && nearby.transfer_finished());
        CHECK(nearby.advance(14, runtime, now, Inspection::near) == Command::observe);
        CHECK(nearby.observation_finished(false) && nearby.done());
        CHECK(nearby.outcome() == Progress::Outcome::failed && nearby.transfer_attempted());
        // A post-transfer cancellation/deadline must not erase mutation evidence or permit a retry.
        for (const bool approach : {false, true}) {
            for (const bool cancel : {false, true}) {
                for (const bool verified : {false, true}) {
                    core::CancellationSource late_cancel{runtime};
                    auto late_request = pickup_request; late_request.cancellation = late_cancel.token();
                    Progress post_transfer{late_request};
                    std::uint64_t frame = 13;
                    if (approach) {
                        CHECK(post_transfer.advance(frame++, runtime, now, Inspection::far) == Command::install);
                        CHECK(post_transfer.installation_finished(Progress::Installation::installed));
                    }
                    CHECK(post_transfer.advance(frame++, runtime, now, Inspection::near) == Command::transfer);
                    if (cancel) late_cancel.cancel();
                    const auto observation_time = cancel ? now : late_request.deadline;
                    CHECK(post_transfer.advance(frame++, runtime, observation_time, Inspection::near) == Command::none);
                    CHECK(!post_transfer.done() && post_transfer.outcome() == Progress::Outcome::pending);
                    CHECK(post_transfer.transfer_finished());
                    CHECK(post_transfer.advance(frame++, runtime, observation_time, Inspection::unavailable) == Command::observe);
                    CHECK(post_transfer.advance(frame++, runtime, observation_time, Inspection::near) == Command::none);
                    CHECK(post_transfer.observation_finished(verified));
                    CHECK(post_transfer.outcome() == (verified ? Progress::Outcome::succeeded : Progress::Outcome::failed));
                    if (approach) {
                        CHECK(!post_transfer.done() && post_transfer.cleanup_owed());
                        CHECK(post_transfer.advance(frame++, runtime, observation_time, Inspection::near) == Command::restore);
                        CHECK(post_transfer.restoration_finished(true));
                    }
                    CHECK(post_transfer.done() && !post_transfer.cleanup_owed() && post_transfer.transfer_attempted());
                    CHECK(post_transfer.advance(frame++, runtime, observation_time, Inspection::near) == Command::none);
                }
            }
        }
        Progress stale_transfer{pickup_request};
        CHECK(stale_transfer.advance(13, runtime, now, Inspection::near) == Command::transfer);
        CHECK(stale_transfer.transfer_finished());
        CHECK(stale_transfer.advance(14, core::RuntimeGeneration::from_value(runtime.value() + 1),
                                     now, Inspection::near) == Command::discard);
        CHECK(stale_transfer.done() && stale_transfer.outcome() == Progress::Outcome::stale);
        CHECK(!stale_transfer.observation_finished(true));
        Progress uncertain_install{pickup_request};
        CHECK(uncertain_install.advance(13, runtime, now, Inspection::far) == Command::install);
        CHECK(uncertain_install.installation_finished(Progress::Installation::uncertain));
        CHECK(uncertain_install.advance(14, runtime, now, Inspection::near) == Command::restore);
        CHECK(uncertain_install.restoration_finished(true) && uncertain_install.done());
        CHECK(!uncertain_install.transfer_attempted());
        Progress expired_progress{pickup_request};
        CHECK(expired_progress.advance(13, runtime, now + 1s, Inspection::near) == Command::none);
        CHECK(expired_progress.done() && expired_progress.outcome() == Progress::Outcome::timed_out);
        Progress stale_progress{pickup_request};
        CHECK(stale_progress.advance(13, runtime, now, Inspection::far) == Command::install);
        CHECK(stale_progress.installation_finished(Progress::Installation::installed));
        CHECK(stale_progress.advance(14, core::RuntimeGeneration::from_value(runtime.value() + 1), now,
                                     Inspection::near) == Command::discard);
        CHECK(stale_progress.done() && stale_progress.cleanup_owed());
        CHECK(stale_progress.outcome() == Progress::Outcome::stale && !stale_progress.transfer_attempted());
        CHECK(!stale_progress.restoration_finished(true));
        CHECK(stale_progress.advance(15, runtime, now, Inspection::near) == Command::none);
        core::CancellationSource pickup_cancel{runtime};
        auto cancellable_request = pickup_request; cancellable_request.cancellation = pickup_cancel.token();
        Progress cancelled_progress{cancellable_request};
        CHECK(cancelled_progress.advance(13, runtime, now, Inspection::far) == Command::install);
        CHECK(cancelled_progress.installation_finished(Progress::Installation::installed));
        pickup_cancel.cancel();
        CHECK(cancelled_progress.advance(14, runtime, now, Inspection::near) == Command::restore);
        CHECK(cancelled_progress.outcome() == Progress::Outcome::cancelled && !cancelled_progress.transfer_attempted());
        CHECK(cancelled_progress.restoration_finished(true) && cancelled_progress.done());
        Progress unavailable_progress{pickup_request};
        CHECK(!unavailable_progress.transfer_finished() && !unavailable_progress.observation_finished(true));
        CHECK(!unavailable_progress.installation_finished(Progress::Installation::installed));
        CHECK(!unavailable_progress.restoration_finished(true));
        CHECK(unavailable_progress.advance(13, runtime, now - 1ms, Inspection::near) == Command::none);
        CHECK(unavailable_progress.advance(13, runtime, now, Inspection::unavailable) == Command::none);
        CHECK(unavailable_progress.advance(14, runtime, now + 1s, Inspection::unavailable) == Command::none);
        CHECK(unavailable_progress.done() && !unavailable_progress.transfer_attempted());
        Progress rejected_install{pickup_request};
        CHECK(rejected_install.advance(13, runtime, now, Inspection::far) == Command::install);
        CHECK(rejected_install.installation_finished(Progress::Installation::rejected));
        CHECK(rejected_install.done() && !rejected_install.cleanup_owed());
        Progress lost_target{pickup_request};
        CHECK(lost_target.advance(13, runtime, now, Inspection::far) == Command::install);
        CHECK(lost_target.installation_finished(Progress::Installation::installed));
        CHECK(lost_target.advance(14, runtime, now, Inspection::rejected) == Command::none);
        CHECK(lost_target.advance(15, runtime, now, Inspection::near) == Command::restore);
        CHECK(lost_target.restoration_finished(true) && !lost_target.transfer_attempted());
        CHECK(client::prepare_pickup_action(pickup_scene({dropped}, "partial", {npc}), pickup,
            observed_owner.token(), now + 1s).item.count == 3);
        auto stolen = dropped; stolen.stealing = true;
        CHECK(client::prepare_pickup_action(pickup_scene({stolen}, "complete", {npc}), pickup,
            observed_owner.token(), now + 1s).item.stealing); // Player theft flag is not NPC permission; native must check the NPC.
        for (const auto selector : {"Combat rifle", "0x00000123:Combat rifle", "0xFF001235:Combat rifle",
            "0xFF001234:combat rifle", "0xFF001234:", "0xFF001234: ", "0xFF001234", "FF001234:Combat rifle",
            "0xFF00123:Combat rifle", "0xFF0012345:Combat rifle", "0xFF00123Z:Combat rifle", "0x00000000:Combat rifle"})
            CHECK(!actions::select_pickup_item(ground.nearby_items(), "complete", selector));
        CHECK(actions::select_pickup_item(ground.nearby_items(), "partial", "0Xff001234:Combat rifle").has_value());
        for (const auto quality : {"cached", "unavailable", "invented"})
            CHECK(!actions::select_pickup_item(ground.nearby_items(), quality, "0xFF001234:Combat rifle"));
        for (int bad = 0; bad < 7; ++bad) {
            auto changed = dropped;
            if (bad == 0) changed.held = true;
            if (bad == 1) changed.count = 0;
            if (bad == 2) changed.count = 2147483648U;
            if (bad == 3) changed.base_origin_plugin = "../Fallout4.esm";
            if (bad == 4) changed.reference_origin_plugin = "../Other.esp";
            std::vector rows{changed};
            if (bad == 5) rows.push_back(changed);
            if (bad == 6) rows.assign(33, changed);
            CHECK(!actions::select_pickup_item(rows, "complete", "0xFF001234:Combat rifle"));
        }
        auto same_name = dropped; ++same_name.reference_id;
        CHECK(actions::select_pickup_item(std::vector{same_name, dropped}, "complete", "0xFF001234:Combat rifle")->reference_id == dropped.reference_id);
        for (const auto arguments : {R"({})", R"({"item":1})", R"({"item":null})", R"([])",
            R"({"item":"0xFF001234:Combat rifle","amount":1})", R"({"item":"0xFF001234:Combat rifle","target_name":"Settler"})"}) {
            auto bad = pickup; bad.arguments = json::parse(arguments); bool rejected = false;
            try { (void)client::prepare_pickup_action(ground, bad, observed_owner.token(), now + 1s); }
            catch (const std::invalid_argument&) { rejected = true; }
            CHECK(rejected);
        }
        for (int bad = 0; bad < 11; ++bad) {
            auto changed = pickup;
            if (bad == 0) changed.actor.form_id = "0x00000014";
            if (bad == 1) changed.actor.form_id = "0x00000099";
            if (bad == 2) changed.actor.origin_plugin = "Other.esp";
            if (bad == 3) changed.actor.playthrough_id = "other-save";
            if (bad == 4) changed.target = changed.actor;
            if (bad == 5) changed.capability = "action.give_item_to";
            if (bad == 6) changed.name = "give_item_to";
            if (bad == 7) changed.action_id.clear();
            if (bad == 8) changed.action_id.assign(129, 'x');
            if (bad == 9) changed.actor.form_id = "0xZZZZ0015";
            bool rejected = false;
            try { (void)client::prepare_pickup_action(ground, changed, observed_owner.token(), bad == 10 ? now : now + 1s); }
            catch (const std::invalid_argument&) { rejected = true; }
            CHECK(rejected);
        }
        for (int bad = 0; bad < 6; ++bad) {
            const auto changed = pickup_scene(bad == 5 ? std::vector<core::NearbyItemSnapshot>{} : std::vector{dropped},
                bad == 4 ? "cached" : "complete", bad == 0 ? std::vector{npc, npc} : bad == 1 ? std::vector{other} : std::vector{npc},
                bad == 2 ? core::RuntimeVariant::vr : core::RuntimeVariant::flat,
                bad == 3 ? core::RuntimeGeneration::from_value(2) : runtime);
            bool rejected = false;
            try { (void)client::prepare_pickup_action(changed, pickup, observed_owner.token(), now + 1s); }
            catch (const std::invalid_argument&) { rejected = true; }
            CHECK(rejected);
        }
        // A base-total increase alone is insufficient: the exact world stack must retire and names must be conserved.
        using RefState = actions::PickupReferenceState;
        auto arrived = rifle; arrived.count = 5; arrived.equipped = true;
        CHECK(actions::pickup_postcondition(std::vector{rifle, modified}, std::vector{arrived, modified}, dropped, RefState::retired, "complete", "complete"));
        auto split_arrival = arrived; split_arrival.count = 4; arrived.count = 1;
        CHECK(actions::pickup_postcondition(std::vector{rifle, modified}, std::vector{arrived, split_arrival, modified}, dropped, RefState::retired, "complete", "complete"));
        arrived.count = 3;
        CHECK(actions::pickup_postcondition({}, std::vector{arrived}, dropped, RefState::retired, "complete", "complete"));
        CHECK(!actions::pickup_postcondition({}, std::vector{arrived}, dropped, RefState::present, "complete", "complete"));
        CHECK(!actions::pickup_postcondition({}, std::vector{arrived}, dropped, RefState::unavailable, "complete", "complete"));
        for (int bad = 0; bad < 9; ++bad) {
            auto changed = arrived;
            if (bad == 0) changed.count = 2;
            if (bad == 1) changed.count = 4;
            if (bad == 2) changed.form_id = dropped.reference_id;
            if (bad == 3) changed.origin_plugin = "Other.esm";
            if (bad == 4) changed.display_name = "Scoped combat rifle";
            if (bad == 5) changed.form_type = 0x23;
            if (bad == 6) changed.count = 0;
            if (bad == 7) changed.count = 2147483648U;
            CHECK(!actions::pickup_postcondition({}, bad == 8 ? std::vector<core::InventoryItemSnapshot>{} : std::vector{changed}, dropped, RefState::retired, "complete", "complete"));
        }
        auto pickup_boundary = dropped; pickup_boundary.count = 65535; arrived.count = 65535;
        CHECK(actions::pickup_postcondition({}, std::vector{arrived}, pickup_boundary, RefState::retired, "complete", "complete"));
        CHECK(!actions::pickup_postcondition(std::vector{modified}, std::vector{arrived}, pickup_boundary, RefState::retired, "complete", "complete"));
        CHECK(!actions::pickup_postcondition(std::vector(513, rifle), std::vector{arrived}, pickup_boundary, RefState::retired, "complete", "complete"));
        CHECK(!actions::pickup_postcondition({}, std::vector(513, arrived), pickup_boundary, RefState::retired, "complete", "complete"));
        // Full receipts preserve unrelated items and accept only pre-established secondary ammunition.
        arrived.count = dropped.count;
        const core::InventoryItemSnapshot ammo{0x777, "Fallout4.esm", ".45 round", 6, 0, 0.0, 0x2c, false};
        const core::InventoryItemSnapshot junk{0x778, "Workshop.esp", "Screw", 4, 0, 0.0, 0x23, false};
        auto ammo_after = ammo; ammo_after.count = 12;
        CHECK(actions::pickup_postcondition(std::vector{junk, ammo}, std::vector{arrived, ammo_after, junk}, dropped, RefState::retired, "complete", "complete", ammo));
        CHECK(!actions::pickup_postcondition(std::vector{junk, ammo}, std::vector{arrived, ammo_after, junk}, dropped, RefState::retired, "complete", "complete"));
        CHECK(!actions::pickup_postcondition(std::vector{junk, ammo}, std::vector{arrived, ammo, junk}, dropped, RefState::retired, "complete", "complete", ammo));
        CHECK(actions::pickup_postcondition(std::vector{junk}, std::vector{junk, arrived}, dropped, RefState::retired, "complete", "complete"));
        for (const auto quality : {"partial", "unavailable", "cached", ""}) {
            CHECK(!actions::pickup_postcondition({}, std::vector{arrived}, dropped, RefState::retired, quality, "complete"));
            CHECK(!actions::pickup_postcondition({}, std::vector{arrived}, dropped, RefState::retired, "complete", quality));
        }
        for (int bad = 0; bad < 7; ++bad) {
            auto changed = junk;
            if (bad == 0) --changed.count;
            if (bad == 1) ++changed.count;
            if (bad == 2) ++changed.form_id;
            if (bad == 3) changed.origin_plugin = "Other.esp";
            if (bad == 4) changed.display_name = "Bolt";
            if (bad == 5) changed.form_type = 0x2c;
            if (bad == 6) changed.count = 0;
            CHECK(!actions::pickup_postcondition(std::vector{junk}, std::vector{changed, arrived}, dropped, RefState::retired, "complete", "complete"));
        }
        for (int bad = 0; bad < 5; ++bad) {
            auto changed = ammo;
            if (bad == 0) changed.count = 0;
            if (bad == 1) changed.count = 2147483648U;
            if (bad == 2) changed.form_type = 0x23;
            if (bad == 3) changed.equipped = true;
            if (bad == 4) changed.form_id = dropped.base_form_id;
            CHECK(!actions::pickup_postcondition({}, std::vector{arrived, changed}, dropped, RefState::retired, "complete", "complete", changed));
        }
        auto split_ammo = ammo; split_ammo.count = 3;
        CHECK(actions::pickup_postcondition({}, std::vector{arrived, split_ammo, split_ammo}, dropped, RefState::retired, "complete", "complete", ammo));
        auto large_before = std::vector(511, junk);
        auto large_after = large_before; large_after.push_back(arrived);
        CHECK(actions::pickup_postcondition(large_before, large_after, dropped, RefState::retired, "complete", "complete"));
        large_before.push_back(junk); large_after.push_back(junk);
        CHECK(!actions::pickup_postcondition(large_before, large_after, dropped, RefState::retired, "complete", "complete"));
        observed_owner.cancel();
        bool cancelled_pickup = false;
        try { (void)client::prepare_pickup_action(ground, pickup, observed_owner.token(), now + 1s); }
        catch (const std::invalid_argument&) { cancelled_pickup = true; }
        CHECK(cancelled_pickup && pickup_request.cancellation.is_cancelled());
        bool cancelled_payment=false;
        try{(void)client::prepare_transfer_inventory(payer_scene,take,observed_owner.token(),payment_pair,true);}catch(const std::invalid_argument&){cancelled_payment=true;}
        CHECK(cancelled_payment && !payment_pair.valid_for(payment));
        bool cancelled_transfer=false;
        try{(void)client::prepare_transfer_inventory(original,give,observed_owner.token(),pair,true);}catch(const std::invalid_argument&){cancelled_transfer=true;}
        CHECK(cancelled_transfer);
        CHECK(!transferred.valid_for(transfer_request));
        bool cancelled_rejected=false;
        try {(void)client::prepare_action_inventory(original,wire_action,observed_owner.token(),post_inventory,true);}
        catch(const std::invalid_argument&){cancelled_rejected=true;}
        CHECK(cancelled_rejected);
        CHECK(!observed_completion.result->inventory->valid_for(observed_request));
        std::cout << "actions tests passed (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
