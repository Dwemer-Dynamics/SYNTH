#include "lifecycle/lifecycle_coordinator.hpp"
#include "core/save_context.hpp"
#include "core/rest_event_mailbox.hpp"
#include "core/quest_event_mailbox.hpp"
#include "core/flat_event_identity_scope.hpp"
#include "core/actor_event_mailbox.hpp"
#include "core/player_inventory_refresh.hpp"
#include "core/inventory_change_mailbox.hpp"
#include <thread>
#include <vector>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace synth::lifecycle;

int assertions{};

#define CHECK(condition)                                                                        \
    do {                                                                                        \
        ++assertions;                                                                           \
        if (!(condition)) {                                                                     \
            throw std::runtime_error{std::string{"CHECK failed: "} + #condition + " at " +     \
                                     __FILE__ + ":" + std::to_string(__LINE__)};                \
        }                                                                                       \
    } while (false)

template <class Exception, class Function>
void check_throws(Function&& function) {
    ++assertions;
    try {
        std::forward<Function>(function)();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error{"expected exception was not thrown"};
}

void test_initialization_registration_and_completion() {
    LifecycleCoordinator lifecycle;
    CHECK(!lifecycle.health().initialized);
    CHECK(lifecycle.register_operation("early", OperationKind::network, lifecycle.stamp()) ==
          RegistrationResult::requires_initialization);
    CHECK(lifecycle.initialize());
    const auto stamp = lifecycle.stamp();
    CHECK(lifecycle.register_operation("network", OperationKind::network, stamp) ==
          RegistrationResult::accepted);
    CHECK(lifecycle.register_operation("media", OperationKind::media, stamp) ==
          RegistrationResult::accepted);
    CHECK(lifecycle.register_operation("action", OperationKind::action, stamp) ==
          RegistrationResult::accepted);
    CHECK(lifecycle.register_operation("network", OperationKind::network, stamp) ==
          RegistrationResult::duplicate);
    CHECK(!lifecycle.cancellation_token("network").is_cancelled());
    CHECK(lifecycle.complete("network", stamp) == CompletionResult::accepted);
    CHECK(lifecycle.complete("missing", stamp) == CompletionResult::unknown_operation);

    const auto health = lifecycle.health();
    CHECK(health.active_network == 0);
    CHECK(health.active_media == 1);
    CHECK(health.active_actions == 1);
    CHECK(health.completed == 1);
}

void test_new_turn_advances_only_response_and_cancels_all_kinds() {
    LifecycleCoordinator lifecycle;
    CHECK(lifecycle.initialize());
    const auto first = lifecycle.stamp();
    int callbacks{};
    for (const auto [id, kind] : {std::pair{"network", OperationKind::network},
                                 std::pair{"media", OperationKind::media},
                                 std::pair{"action", OperationKind::action}}) {
        CHECK(lifecycle.register_operation(id, kind, first, [&] { ++callbacks; }) ==
              RegistrationResult::accepted);
    }
    const auto network_token = lifecycle.cancellation_token("network");
    const auto second = lifecycle.begin_new_turn();
    CHECK(second.runtime == first.runtime);
    CHECK(second.response.value() == first.response.value() + 1);
    CHECK(callbacks == 3);
    CHECK(network_token.is_cancelled());
    CHECK(lifecycle.health().cancelled == 3);
    CHECK(lifecycle.complete("network", first) == CompletionResult::stale_response_generation);

    CHECK(lifecycle.register_operation("new", OperationKind::network, first) ==
          RegistrationResult::requires_initialization);
    CHECK(lifecycle.register_operation("new", OperationKind::network, second) ==
          RegistrationResult::accepted);
}

void test_runtime_invalidation_requires_reinitialization() {
    for (const auto reason : {InvalidationReason::load,
                              InvalidationReason::new_game,
                              InvalidationReason::main_menu}) {
        LifecycleCoordinator lifecycle;
        CHECK(lifecycle.initialize());
        const auto old = lifecycle.stamp();
        CHECK(lifecycle.register_operation("request", OperationKind::network, old) ==
              RegistrationResult::accepted);
        const auto next = lifecycle.invalidate(reason);
        CHECK(next.runtime.value() == old.runtime.value() + 1);
        CHECK(next.response.value() == old.response.value() + 1);
        CHECK(!lifecycle.health().initialized);
        CHECK(lifecycle.register_operation("blocked", OperationKind::media, next) ==
              RegistrationResult::requires_initialization);
        CHECK(lifecycle.initialize());
        CHECK(lifecycle.register_operation("ready", OperationKind::media, next) ==
              RegistrationResult::accepted);
    }
}

void test_stale_completion_is_rejected_without_removing_current_operation() {
    LifecycleCoordinator lifecycle;
    CHECK(lifecycle.initialize());
    const auto first = lifecycle.stamp();
    const auto second = lifecycle.begin_new_turn();
    CHECK(lifecycle.register_operation("current", OperationKind::action, second) ==
          RegistrationResult::accepted);
    CHECK(lifecycle.complete("current", first) == CompletionResult::stale_response_generation);
    CHECK(lifecycle.health().active_actions == 1);

    auto stale_runtime = second;
    stale_runtime.runtime = synth::core::RuntimeGeneration::from_value(second.runtime.value() + 1);
    CHECK(lifecycle.complete("current", stale_runtime) ==
          CompletionResult::stale_runtime_generation);
    CHECK(lifecycle.complete("current", second) == CompletionResult::accepted);
    CHECK(lifecycle.health().stale_completions == 2);
}

void test_shutdown_is_terminal_and_contains_cancel_exceptions() {
    LifecycleCoordinator lifecycle;
    CHECK(lifecycle.initialize());
    const auto stamp = lifecycle.stamp();
    int callback{};
    CHECK(lifecycle.register_operation("throwing", OperationKind::network, stamp, [] {
              throw std::runtime_error{"contained"};
          }) == RegistrationResult::accepted);
    CHECK(lifecycle.register_operation("called", OperationKind::media, stamp, [&] { ++callback; }) ==
          RegistrationResult::accepted);

    const auto stopped = lifecycle.invalidate(InvalidationReason::shutdown);
    CHECK(callback == 1);
    CHECK(lifecycle.health().shutdown);
    CHECK(!lifecycle.initialize());
    CHECK(lifecycle.register_operation("late", OperationKind::action, stopped) ==
          RegistrationResult::shutdown);
    CHECK(lifecycle.invalidate(InvalidationReason::shutdown) == stopped);
    check_throws<std::logic_error>([&] { (void)lifecycle.begin_new_turn(); });
}

void test_validation() {
    LifecycleCoordinator lifecycle;
    CHECK(lifecycle.initialize());
    check_throws<std::invalid_argument>([&] {
        (void)lifecycle.register_operation("", OperationKind::network, lifecycle.stamp());
    });
    check_throws<std::invalid_argument>([] { (void)ResponseGeneration::from_value(0); });
}

void test_saved_context_roundtrip_and_stale_writers() {
    using namespace synth::core;
    const SavedContext original{"player:a", "session:old", 7, 9};
    const auto bytes = encode_saved_context(original);
    CHECK(bytes && bytes->size() == 274);
    CHECK(decode_saved_context(*bytes) == original);
    CHECK(!decode_saved_context(std::span{*bytes}.first(273)));
    auto malformed = *bytes;
    malformed[128] = 'x';
    CHECK(!decode_saved_context(malformed));
    malformed = *bytes;
    std::fill(malformed.begin() + 258, malformed.end(), std::uint8_t{0});
    CHECK(!decode_saved_context(malformed));
    CHECK(!encode_saved_context({"../player", "session", 1, 1}));
    CHECK(!encode_saved_context({"player", "session", 1, 9'007'199'254'740'992ULL}));
    CHECK(encode_saved_context({std::string(128, 'a'), std::string(128, 'b'), 1, 1}));

    SaveContextStore store;
    CHECK(!store.for_save());
    const auto old = store.bind("player:a");
    CHECK(!old.loaded_context);
    CHECK(store.acknowledge(old.epoch, original));
    CHECK(!store.acknowledge(old.epoch, {"player:b", "session:old", 7, 10}));
    CHECK(!store.acknowledge(old.epoch, {"player:a", "session:old", 7, 8}));
    CHECK(store.for_save() == bytes);
    const auto load_epoch = store.reset();
    CHECK(!store.acknowledge(old.epoch, {"player:a", "session:old", 7, 99}));
    store.restore(load_epoch, bytes);
    const auto current = store.bind("player:a");
    CHECK(current.loaded_context == original);
    CHECK(store.acknowledge(current.epoch, {"player:a", "session:new", 8, 1}));
    CHECK(current.loaded_context == original); // Immutable origin, even as the latest ACK advances.
    store.stop_writer(old.epoch);
    CHECK(store.acknowledge(current.epoch, {"player:a", "session:new", 8, 2}));
    store.stop_writer(current.epoch);
    CHECK(!store.acknowledge(current.epoch, {"player:a", "session:new", 8, 3}));
    CHECK(!store.bind("player:b").loaded_context);
    CHECK(!store.for_save());
    store.restore(load_epoch, bytes); // An obsolete load cannot replace a newer writer.
    CHECK(!store.for_save());
    const auto another = store.bind("player:a");
    CHECK(store.acknowledge(another.epoch, original));
    CHECK(!store.bind("").loaded_context && !store.for_save());
    CHECK(!store.acknowledge(another.epoch, original));
}

void test_bounded_saved_context_records() {
    using namespace synth::core;
    struct Record { std::uint32_t type, version, length; SavedContextBytes data; };
    struct Serialization {
        std::vector<Record> records;
        mutable std::size_t next{};
        bool short_read{};
        bool write_ok{true};
        bool GetNextRecordInfo(std::uint32_t& type, std::uint32_t& version, std::uint32_t& length) const {
            if (next == records.size()) return false;
            const auto& r = records[next++]; type = r.type; version = r.version; length = r.length;
            return true;
        }
        std::uint32_t ReadRecordData(void* output, std::uint32_t length) const {
            std::copy_n(records[next - 1].data.begin(), length, static_cast<std::uint8_t*>(output));
            return short_read ? length - 1 : length;
        }
        bool WriteRecord(std::uint32_t type, std::uint32_t version, const void* input, std::uint32_t length) const {
            CHECK(type == saved_context_record_type && version == 1 && length == 274);
            CHECK(decode_saved_context({static_cast<const std::uint8_t*>(input), length}));
            return write_ok;
        }
    };
    const auto bytes = *encode_saved_context({"player:a", "session:saved", 2, 3});
    const Record valid{saved_context_record_type, 1, 274, bytes};
    SaveContextStore store;
    Serialization reader{{{1234, 900, 500000, {}}, valid}};
    read_saved_context(reader, store); // Unknown records are skipped by the interface, not allocated.
    CHECK(store.for_save() == bytes);
    CHECK(write_saved_context(reader, store));
    reader.write_ok = false;
    CHECK(!write_saved_context(reader, store));
    for (auto bad : {Record{saved_context_record_type, 2, 274, bytes},
                     Record{saved_context_record_type, 1, 1000000, bytes},
                     Record{saved_context_record_type, 1, 1, bytes}}) {
        reader = {{bad}};
        read_saved_context(reader, store);
        CHECK(!store.for_save());
    }
    reader = {{valid, valid}};
    read_saved_context(reader, store);
    CHECK(!store.for_save());
    reader = {{valid}, 0, true};
    read_saved_context(reader, store);
    CHECK(!store.for_save());
    reader = {std::vector<Record>(65, {1234, 1, 0, {}})};
    read_saved_context(reader, store);
    CHECK(!store.for_save() && reader.next == 64);
    reader = {};
    read_saved_context(reader, store);
    CHECK(!store.for_save());
}

// Reserve before forwarding, complete afterward: nested callbacks must not reorder or cross a load.
void test_actor_event_reservations() {
    using namespace synth::core;
    ActorEventMailbox events;
    const ActorEvent death{ActorEventKind::death,0x1234,0x2345};
    const ActorEvent equip{ActorEventKind::equipped,0x1234,0,0x3456,0x4567,42};
    CHECK(!events.reserve(0) && !events.commit({},death));
    events.arm();const auto owner=events.stamp();
    const auto outer=events.reserve(owner),inner=events.reserve(owner);
    CHECK(outer && inner && events.commit(inner,equip));
    CHECK(events.take().size==0);
    CHECK(events.commit(outer,death) && !events.commit(outer,equip));
    const auto ordered=events.take();
    CHECK(ordered.size==2 && ordered.events[0]==death && ordered.events[1]==equip);
    const auto abandoned=events.reserve(owner),following=events.reserve(owner);
    events.discard(abandoned);CHECK(!events.commit(abandoned,death));
    CHECK(events.commit(following,equip));events.discard(following);
    CHECK(events.take().size==1);
    for (const auto invalid : {ActorEvent{},ActorEvent{ActorEventKind::death,1,0,1},
            ActorEvent{ActorEventKind::equipped,1},ActorEvent{ActorEventKind::equipped,1,2,3},
            ActorEvent{static_cast<ActorEventKind>(255),1},ActorEvent{ActorEventKind::death,UINT32_MAX}}) {
        CHECK(!events.commit(events.reserve(owner),invalid));
        CHECK(events.take().size==0); // Bad evidence cannot strand a reserved head.
    }
    auto late=events.reserve(owner);events.invalidate();events.arm();
    auto next=events.reserve(events.stamp());
    CHECK(next && !events.commit(late,death));events.discard(late);
    CHECK(events.commit(next,equip));CHECK(events.take().events[0]==equip);
    CHECK(!events.reserve(owner));
    for (std::size_t i=0;i<ActorEventMailbox::capacity;++i) CHECK(events.commit(events.reserve(events.stamp()),death));
    CHECK(!events.reserve(events.stamp()) && events.dropped_total()==1);
    CHECK(events.take().size==ActorEventMailbox::capacity);
    CHECK(events.commit(events.reserve(events.stamp()),equip));events.invalidate();
    CHECK(events.take().size==0);
    CHECK(!events.commit({1,1,ActorEventMailbox::capacity},death));
}

void test_flat_event_identity_scope() {
    using Scope = synth::core::FlatEventIdentityScope;
    using synth::core::flat_vm_actor_form_id;
    constexpr auto actor_type = 0x41U;
    constexpr auto handle = 0x0000FFFF00000014ULL;
    CHECK(flat_vm_actor_form_id(actor_type,handle)==0x14U);
    CHECK(flat_vm_actor_form_id(actor_type,0x0000FFFFFF123456ULL)==0xFF123456);
    for (const auto invalid : {0ULL,0xFFFF00000000ULL,0xFFFFFFFFFFFFULL,0x1000000000014ULL,
                               0x2000000000014ULL,0x3000000000014ULL,0x8000FFFF00000014ULL,0x14ULL})
        CHECK(!flat_vm_actor_form_id(actor_type,invalid));
    CHECK(!flat_vm_actor_form_id(0x40,handle));
    Scope::observe(actor_type,123,handle); // No scope, no retained identity.
    Scope outer{3,123,456};
    CHECK(!outer.result(3));
    Scope::observe(actor_type,999,handle);
    CHECK(!outer.result(3));
    Scope::observe(actor_type,123,handle);
    CHECK(!outer.result(3)); // A nonnull killer cannot silently become unknown.
    Scope::observe(actor_type,456,0xFFFF00000020ULL);
    CHECK((outer.result(3)==Scope::Identities{0x14,0x20}));
    CHECK(!outer.result(5) && !outer.result(2)); // Reentrant load invalidates completed identities.
    {
        Scope inner{3,123};
        Scope::observe(actor_type,456,handle);
        CHECK(!inner.result(3) && !outer.result(3));
        Scope::observe(actor_type,123,0xFFFF00000021ULL);
        CHECK((inner.result(3)==Scope::Identities{0x21,0}));
    }
    CHECK((outer.result(3)==Scope::Identities{0x14,0x20}));
    try { Scope throwing{3,123};Scope::observe(actor_type,123,0xFFFF00000021ULL);throw std::runtime_error{"probe"}; }
    catch (const std::runtime_error&) {}
    CHECK((outer.result(3)==Scope::Identities{0x14,0x20}));
    // An unrelated engine thread cannot populate the current callback's correlation scope.
    bool other_thread_ok=false;
    std::thread worker{[&] {
        Scope::observe(actor_type,123,0xFFFF00000022ULL);
        Scope local{3,123};Scope::observe(actor_type,123,0xFFFF00000023ULL);
        other_thread_ok=local.result(3)==Scope::Identities{0x23,0};
    }};
    worker.join();CHECK(other_thread_ok);
    CHECK((outer.result(3)==Scope::Identities{0x14,0x20}));
    {
        Scope self{3,123,123};Scope::observe(actor_type,123,handle);
        CHECK((self.result(3)==Scope::Identities{0x14,0x14}));
        Scope::observe(actor_type,123,0xFFFF00000021ULL);
        CHECK(!self.result(3)); // Conflicting identities are not latest-wins.
        Scope::observe(actor_type,123,handle);CHECK(!self.result(3));
    }
    {
        Scope invalid{3,123};Scope::observe(0x40,123,handle);
        Scope::observe(actor_type,123,handle);CHECK(!invalid.result(3));
    }
    { Scope disarmed{2,123};Scope::observe(actor_type,123,handle);CHECK(!disarmed.result(2)); }
    { Scope missing{3,0};Scope::observe(actor_type,0,handle);CHECK(!missing.result(3)); }
}

void test_quest_event_reservations() {
    using namespace synth::core;
    QuestEventMailbox quests;
    const QuestEvent stage{QuestEventKind::stage, 0x1234, 10, 7, false};
    const QuestEvent started{QuestEventKind::started, 0x5678};
    const QuestEvent stopped{QuestEventKind::stopped, 0x5678, 0, 0, true};
    CHECK(!quests.reserve(quests.stamp(), stage));
    CHECK(!quests.commit({}));
    CHECK(quests.take().size == 0);
    quests.arm();
    const auto owner = quests.stamp();
    CHECK(quests.is_current(owner));
    CHECK(!quests.reserve(owner, {}));
    CHECK(!quests.reserve(owner, {static_cast<QuestEventKind>(9), 1}));
    CHECK(!quests.reserve(owner, {QuestEventKind::stage, UINT32_MAX}));
    CHECK(!quests.reserve(owner, {QuestEventKind::stage, 1, 0, 0, true}));
    CHECK(!quests.reserve(owner, {QuestEventKind::started, 1, 2}));
    CHECK(!quests.reserve(owner, {QuestEventKind::stopped, 1, 0, 2}));
    const auto outer = quests.reserve(owner, stage);
    const auto inner = quests.reserve(owner, started);
    CHECK(outer && inner);
    CHECK(quests.commit(inner));
    CHECK(quests.take().size == 0); // Later completion cannot overtake the pending outer event.
    CHECK(quests.commit(outer));
    CHECK(!quests.commit(outer));
    const auto ordered = quests.take();
    CHECK(ordered.owner == owner && ordered.size == 2);
    CHECK(ordered.events[0] == stage && ordered.events[1] == started);
    CHECK(!quests.commit(inner));
    const auto abandoned = quests.reserve(owner, stage);
    const auto after_abandoned = quests.reserve(owner, stopped);
    CHECK(quests.commit(after_abandoned));
    quests.discard(abandoned);
    quests.discard(after_abandoned); // Cannot erase an already committed event.
    const auto failed_stop = quests.take();
    CHECK(failed_stop.size == 1 && failed_stop.events[0] == stopped);
    const auto old = quests.reserve(owner, stage);
    quests.invalidate();
    CHECK(!quests.is_current(owner));
    quests.arm();
    const auto replacement = quests.reserve(quests.stamp(), started); // Reuses the physical slot.
    CHECK(replacement && replacement.serial != old.serial);
    CHECK(!quests.commit(old));
    quests.discard(old);
    CHECK(quests.commit(replacement));
    const auto new_save = quests.take();
    CHECK(new_save.size == 1 && new_save.events[0] == started);
    CHECK(new_save.owner != owner && quests.is_current(new_save.owner));
    CHECK(!quests.reserve(owner, stage));
    CHECK(quests.commit(quests.reserve(quests.stamp(), stage)));
    quests.invalidate();
    CHECK(quests.take().size == 0); // Already completed observations also cannot survive a load.
    quests.arm();
    for (std::size_t cycle = 0; cycle < 3; ++cycle) {
        for (std::size_t i = 0; i < QuestEventMailbox::capacity; ++i)
            CHECK(quests.commit(quests.reserve(quests.stamp(), {QuestEventKind::stage, 0x1234,
                static_cast<std::uint16_t>(i), static_cast<std::uint8_t>(i)})));
        CHECK(!quests.reserve(quests.stamp(), stage));
        const auto full = quests.take();
        CHECK(full.size == QuestEventMailbox::capacity);
        for (std::size_t i = 0; i < full.size; ++i)
            CHECK(full.events[i].stage == i && full.events[i].item == i);
    }
    CHECK(quests.dropped_total() == 3);
    const auto pending = quests.reserve(quests.stamp(), stage);
    quests.invalidate();
    CHECK(!quests.commit(pending));
    CHECK(quests.take().size == 0);
    CHECK(quests.dropped_total() == 3); // Load invalidations are not pressure losses or replacement-save events.
    CHECK(!quests.commit({1, 1, QuestEventMailbox::capacity}));
}

void test_inventory_change_signal() {
    synth::core::InventoryChangeMailbox changes;
    CHECK(!changes.revision());
    CHECK(!changes.record(changes.stamp(),0,0x14,99));
    changes.arm();
    CHECK(!changes.revision());
    CHECK(!changes.record(changes.stamp(),1,2,99));
    CHECK(!changes.record(changes.stamp(),0,0x14,0));
    CHECK(!changes.record(changes.stamp(),0x14,0,UINT32_MAX));
    CHECK(!changes.record(changes.stamp(),UINT32_MAX,0x14,99));
    CHECK(!changes.record(changes.stamp(),0x14,UINT32_MAX,99));
    CHECK(!changes.revision());
    CHECK(changes.record(changes.stamp(),0,0x14,99)); // Pickup.
    const auto first = changes.revision();
    CHECK(first.has_value());
    CHECK(changes.record(changes.stamp(),0x14,0,99)); // Drop.
    CHECK(changes.revision() != first);
    CHECK(changes.record(changes.stamp(),0x14,0x200,99)); // Player to container, including trade/storage paths.
    CHECK(changes.record(changes.stamp(),0x200,0x14,99));
    const auto old_callback = changes.stamp();
    CHECK(changes.record(old_callback,0x14,0x14,99)); // Same-container changes still deserve fresh observation.
    CHECK(!changes.record(old_callback,0x14,0,99)); // Older callbacks cannot overwrite a newer publication.
    const auto before_load = changes.stamp();
    changes.invalidate();
    CHECK(!changes.revision());
    changes.arm();
    CHECK(!changes.record(before_load,0x14,0,99));
    CHECK(!changes.revision());
    CHECK(changes.record(changes.stamp(),0x14,0,99));
    const auto before_burst = changes.revision();
    for (int i = 0; i < 1000; ++i) CHECK(changes.record(changes.stamp(),0,0x14,99));
    CHECK(changes.revision() != before_burst); // One latest revision, never an unbounded event queue.
    changes.invalidate();
    CHECK(!changes.revision());
}

void test_inventory_refresh_cadence() {
    using synth::core::PlayerInventoryRefresh;
    using namespace std::chrono_literals;
    PlayerInventoryRefresh refresh;
    const auto start = PlayerInventoryRefresh::Clock::time_point{100s};
    CHECK(!refresh.due(1, start, 0));
    refresh.reset(1, start);
    CHECK(!refresh.due(1, start + 1999ms, 0));
    CHECK(refresh.due(1, start + 2s, 0));
    auto revision = refresh.attempt(start + 2s);
    // A busy inventory or rejected network queue waits before recapturing.
    CHECK(!refresh.due(1, start + 3999ms, 0));
    CHECK(refresh.due(1, start + 4s, 0));
    revision = refresh.attempt(start + 4s);
    refresh.submitted(revision, 10);
    CHECK(!refresh.due(1, start + 5s, 9));
    CHECK(refresh.due(1, start + 6s, 9));
    CHECK(!refresh.due(1, start + 6s, 10));
    CHECK(!refresh.due(1, start + 35999ms, 10));
    CHECK(refresh.due(1, start + 36s, 10));
    revision = refresh.attempt(start + 36s);
    refresh.submitted(revision, 11);
    CHECK(!refresh.due(1, start + 36s, 11));
    refresh.mark_dirty(1, start + 37s);
    refresh.mark_dirty(1, start + 37100ms); // Bursts do not indefinitely extend the first debounce.
    CHECK(!refresh.due(1, start + 37199ms, 11));
    CHECK(refresh.due(1, start + 37200ms, 11));
    revision = refresh.attempt(start + 37200ms);
    refresh.submitted(revision, 12);
    refresh.mark_dirty(1, start + 37300ms); // An old in-flight ACK cannot clear a subsequent change.
    CHECK(!refresh.due(1, start + 37499ms, 12));
    CHECK(refresh.due(1, start + 37500ms, 12));
    revision = refresh.attempt(start + 37500ms);
    refresh.submitted(revision, 13);
    CHECK(!refresh.due(1, start + 38s, 14)); // A newer actual inventory capture can satisfy the observation.
    CHECK(!refresh.due(2, start + 90s, 99));
    refresh.mark_dirty(2, start + 39s);
    CHECK(!refresh.due(1, start + 40s, 14));
    refresh.reset(2, start + 50s);
    CHECK(!refresh.due(1, start + 90s, 99));
    CHECK(!refresh.due(2, start + 51s, 0));
    CHECK(refresh.due(2, start + 52s, 0));
    refresh = {};
    CHECK(!refresh.due(2, start + 90s, 99));
}

}  // namespace

int main() {
    try {
        synth::core::RestEventMailbox rest;
        using synth::core::RestStart;
        CHECK(!rest.record(rest.stamp(), RestStart::sleep));
        CHECK(rest.take().size == 0);
        rest.arm();
        const auto first_rest_epoch = rest.stamp();
        CHECK(rest.record(first_rest_epoch, RestStart::sleep));
        CHECK(rest.record(first_rest_epoch, RestStart::sleep));
        CHECK(rest.record(first_rest_epoch, RestStart::wait));
        const auto repeated_starts = rest.take();
        CHECK(repeated_starts.size == 3);
        CHECK(repeated_starts.events[0] == RestStart::sleep);
        CHECK(repeated_starts.events[1] == RestStart::sleep);
        CHECK(repeated_starts.events[2] == RestStart::wait);
        CHECK(rest.take().size == 0);
        CHECK(!rest.record(first_rest_epoch, static_cast<RestStart>(7)));
        CHECK(rest.record(first_rest_epoch, RestStart::wait));
        rest.invalidate();
        CHECK(rest.take().size == 0);
        CHECK(!rest.record(first_rest_epoch, RestStart::sleep));
        rest.arm();
        CHECK(rest.stamp() != first_rest_epoch);
        CHECK(!rest.record(first_rest_epoch, RestStart::wait));
        CHECK(rest.record(rest.stamp(), RestStart::wait));
        rest.arm();
        const auto rearmed_start = rest.take();
        CHECK(rearmed_start.size == 1 && rearmed_start.events[0] == RestStart::wait);
        CHECK(rest.dropped_total() == 0);
        for (std::size_t i = 0; i < synth::core::RestEventBatch::capacity; ++i)
            CHECK(rest.record(rest.stamp(), i % 2 == 0 ? RestStart::sleep : RestStart::wait));
        CHECK(!rest.record(rest.stamp(), RestStart::sleep));
        CHECK(rest.dropped_total() == 1);
        const auto bounded_starts = rest.take();
        CHECK(bounded_starts.size == synth::core::RestEventBatch::capacity);
        for (std::size_t i = 0; i < bounded_starts.size; ++i)
            CHECK(bounded_starts.events[i] == (i % 2 == 0 ? RestStart::sleep : RestStart::wait));
        CHECK(rest.take().size == 0);
        CHECK(rest.record(rest.stamp(), RestStart::wait));
        const auto before_replacement = rest.stamp();
        rest.invalidate(); // Settings/session replacement fences pending starts too, not just game loads.
        rest.arm();
        CHECK(rest.take().size == 0);
        CHECK(!rest.record(before_replacement, RestStart::sleep));
        CHECK(rest.dropped_total() == 1); // Lifetime health does not create an event in the new epoch.
        test_quest_event_reservations();
        test_flat_event_identity_scope();
        test_actor_event_reservations();
        test_inventory_refresh_cadence();
        test_inventory_change_signal();
        test_initialization_registration_and_completion();
        test_new_turn_advances_only_response_and_cancels_all_kinds();
        test_runtime_invalidation_requires_reinitialization();
        test_stale_completion_is_rejected_without_removing_current_operation();
        test_shutdown_is_terminal_and_contains_cancel_exceptions();
        test_validation();
        test_saved_context_roundtrip_and_stale_writers();
        test_bounded_saved_context_records();
        std::cout << "lifecycle tests passed (" << assertions << " assertions)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
