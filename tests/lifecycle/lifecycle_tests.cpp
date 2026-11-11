#include "lifecycle/lifecycle_coordinator.hpp"
#include "core/save_context.hpp"
#include "core/rest_event_mailbox.hpp"
#include "core/quest_event_mailbox.hpp"
#include "core/flat_event_identity_scope.hpp"
#include "core/actor_event_mailbox.hpp"
#include "core/player_inventory_refresh.hpp"
#include "core/inventory_change_mailbox.hpp"
#include <algorithm>
#include <functional>
#include <optional>
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

struct FakeRandom {
    std::optional<synth::core::PlaythroughUuid> value;
    int* calls{};
    std::optional<synth::core::PlaythroughUuid> operator()() const {
        if (calls) ++*calls;
        return value;
    }
};

synth::core::PlaythroughUuid random_bytes(std::uint8_t fill) {
    synth::core::PlaythroughUuid bytes{};
    bytes.fill(fill);
    return bytes;
}

std::string uuid_of(std::uint8_t fill) { return synth::core::uuid_text(synth::core::uuid_v4(random_bytes(fill))); }

struct SaveRecord { std::uint32_t type, version, length; synth::core::SavedRecordBytes data; };
struct SaveSerialization {
    std::vector<SaveRecord> records;
    mutable std::size_t next{};
    bool short_read{};
    bool write_ok{true};
    mutable std::optional<synth::core::SavedRecordBytes> written;
    mutable std::function<void()> during{}; // Runs once inside the callback, after it claimed the load.
    bool throw_read{};
    bool GetNextRecordInfo(std::uint32_t& type, std::uint32_t& version, std::uint32_t& length) const {
        if (during) std::exchange(during, {})();
        if (next == records.size()) return false;
        const auto& r = records[next++]; type = r.type; version = r.version; length = r.length;
        return true;
    }
    std::uint32_t ReadRecordData(void* output, std::uint32_t length) const {
        if (throw_read) throw std::runtime_error{"co-save read"};
        std::copy_n(records[next - 1].data.begin(), std::min<std::size_t>(length, records[next - 1].data.size()),
                    static_cast<std::uint8_t*>(output));
        return short_read ? length - 1 : length;
    }
    bool WriteRecord(std::uint32_t type, std::uint32_t version, const void* input, std::uint32_t length) const {
        CHECK(type == synth::core::saved_context_record_type && version == 2 && length == 292);
        const auto* bytes = static_cast<const std::uint8_t*>(input);
        CHECK(synth::core::decode_saved_record({bytes, length}));
        written.emplace();
        std::copy_n(bytes, length, written->begin());
        return write_ok;
    }
};

// One complete F4SE load: PreLoadGame, revert, zero or more load callbacks, then PostLoadGame.
synth::core::ResolvedIdentity load_save(synth::core::SaveContextStore& store, std::vector<std::vector<SaveRecord>> callbacks,
                                        std::uint8_t fresh = 0x77, bool success = true) {
    store.begin_load();
    store.revert();
    for (auto& records : callbacks) synth::core::read_saved_context(SaveSerialization{std::move(records)}, store);
    return store.finish_load(success, FakeRandom{random_bytes(fresh)});
}

SaveRecord saved_record(const synth::core::SaveContextStore& store) {
    SaveSerialization writer;
    CHECK(synth::core::write_saved_context(writer, store) && writer.written);
    return {synth::core::saved_context_record_type, 2, 292, *writer.written};
}

std::optional<synth::core::SavedContext> saved_anchor(const synth::core::SaveContextStore& store) {
    const auto bytes = store.for_save();
    CHECK(bytes);
    const auto record = synth::core::decode_saved_record(*bytes);
    CHECK(record);
    return record->anchor ? synth::core::decode_saved_context(*record->anchor) : std::nullopt;
}

void test_saved_context_roundtrip_and_stale_writers() {
    using namespace synth::core;
    const auto a = uuid_of(0x11);
    CHECK(a == "11111111-1111-4111-9111-111111111111");
    const SavedContext original{a, "session:old", 7, 9};
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

    const auto uuid = uuid_v4(random_bytes(0x11));
    const auto record = encode_saved_record({RuntimeVariant::flat, uuid, bytes});
    CHECK(record && (*record)[0] == 1 && (*record)[17] == 1);
    CHECK(decode_saved_record(*record)->anchor == bytes);
    CHECK(!encode_saved_record({RuntimeVariant::flat, random_bytes(0x11), std::nullopt})); // Not a v4 UUID.
    CHECK(!encode_saved_record({RuntimeVariant::flat, uuid_v4(random_bytes(0x22)), bytes})); // Foreign anchor.
    auto tampered = *record;
    tampered[17] = 0; // Flag says no anchor but checkpoint bytes remain.
    CHECK(!decode_saved_record(tampered));
    tampered = *record;
    tampered[0] = 3;
    CHECK(!decode_saved_record(tampered));

    SaveContextStore store;
    CHECK(!store.for_save() && !store.playthrough_id() && !store.bind(a, store.lifetime()));
    CHECK(store.start_new_game(FakeRandom{random_bytes(0x11)}).resolution == IdentityResolution::new_game);
    CHECK(store.playthrough_id() == a);
    const auto old = *store.bind(a, store.lifetime());
    CHECK(!old.loaded_context);
    CHECK(!store.bind("player:b", store.lifetime()) && !store.bind(uuid_of(0x22), store.lifetime()) && !store.bind("", store.lifetime()));
    CHECK(store.writer_current(old.epoch)); // A rejected snapshot cannot disturb the current writer.
    CHECK(store.acknowledge(old.epoch, original));
    CHECK(!store.acknowledge(old.epoch, {uuid_of(0x22), "session:old", 7, 10}));
    CHECK(!store.acknowledge(old.epoch, {a, "session:old", 7, 8}));
    CHECK(saved_anchor(store) == original);
    const auto disk = saved_record(store);

    // Reloading the same UUID still rejects the old writer, its ACKs and its binding.
    CHECK(load_save(store, {{disk}}).resolution == IdentityResolution::restored);
    CHECK(!store.acknowledge(old.epoch, {a, "session:old", 7, 99}) && !store.writer_current(old.epoch));
    const auto current = *store.bind(a, store.lifetime());
    CHECK(current.loaded_context == original);
    CHECK(store.acknowledge(current.epoch, {a, "session:new", 8, 1}));
    CHECK(current.loaded_context == original); // Immutable origin, even as the latest ACK advances.
    store.stop_writer(old.epoch);
    CHECK(store.acknowledge(current.epoch, {a, "session:new", 8, 2}));
    store.stop_writer(current.epoch);
    CHECK(!store.acknowledge(current.epoch, {a, "session:new", 8, 3}));
    CHECK((saved_anchor(store) == SavedContext{a, "session:new", 8, 2}));
}

void test_saved_context_same_world_replacement() {
    using namespace synth::core;
    const auto a = uuid_of(0x11);
    const SavedContext disk{a, "session:disk", 7, 9};
    const SavedContext acknowledged{a, "session:before-stop", 8, 12};
    const SavedContext next_acknowledged{a, "session:after-stop", 8, 2};
    SaveContextStore store;
    const auto record = *encode_saved_record({RuntimeVariant::flat, uuid_v4(random_bytes(0x11)), encode_saved_context(disk)});
    CHECK(load_save(store, {{{saved_context_record_type, 2, 292, record}}}).resolution == IdentityResolution::restored);
    const auto original = *store.bind(a, store.lifetime());
    CHECK(original.loaded_context == disk);
    CHECK(store.acknowledge(original.epoch, acknowledged));
    CHECK(original.loaded_context == disk); // An active session keeps its copied origin.

    CHECK(store.writer_current(original.epoch));
    store.stop_writer(original.epoch); // Halt retires the writer without loading a different save.
    CHECK(!store.writer_current(original.epoch)); // A late heard receipt cannot dirty a retired binding.
    CHECK(saved_anchor(store) == acknowledged);
    CHECK(!store.acknowledge(original.epoch, {a, "session:before-stop", 8, 99}));
    const auto restarted = *store.bind(a, store.lifetime());
    CHECK(restarted.loaded_context == acknowledged); // The replacement must inherit the live frontier and UUID.
    CHECK(store.playthrough_id() == a);
    CHECK(!store.acknowledge(restarted.epoch, {uuid_of(0x22), "session:after-stop", 8, 1}));
    store.stop_writer(original.epoch); // Late retirement cannot stop the replacement writer.
    CHECK(store.acknowledge(restarted.epoch, next_acknowledged));
    CHECK(restarted.loaded_context == acknowledged);
    CHECK(original.loaded_context == disk);

    const auto replaced = *store.bind(a, store.lifetime()); // Settings replacement also transfers the frontier.
    CHECK(replaced.loaded_context == next_acknowledged);
    CHECK(store.writer_current(replaced.epoch) && !store.writer_current(restarted.epoch)); // Stale same-world writer.
    CHECK(!store.acknowledge(restarted.epoch, {a, "session:after-stop", 8, 99}));
    CHECK(saved_anchor(store) == next_acknowledged);
}

void test_saved_context_disk_load_replaces_live_frontier() {
    using namespace synth::core;
    const auto a = uuid_of(0x11);
    const SavedContext earlier_disk{a, "session:earlier-save", 3, 4};
    const SavedContext later_live{a, "session:later-live", 7, 20};
    const SavedContext after_reload{a, "session:after-reload", 8, 1};
    SaveContextStore store;
    (void)store.start_new_game(FakeRandom{random_bytes(0x11)});
    const auto offline = saved_record(store); // Persisted before any session, network or ACK.
    CHECK(!saved_anchor(store));
    const auto first = *store.bind(a, store.lifetime());
    CHECK(store.acknowledge(first.epoch, earlier_disk));
    const auto earlier = saved_record(store);
    CHECK(store.acknowledge(first.epoch, later_live));

    // An actual load discards the later live frontier and binds no writer until PostLoadGame.
    store.begin_load();
    CHECK(!store.for_save() && !store.playthrough_id() && !store.writer_current(first.epoch) && !store.bind(a, store.lifetime()));
    store.revert();
    read_saved_context(SaveSerialization{{earlier}}, store);
    CHECK(!store.playthrough_id()); // Staged only.
    CHECK(store.finish_load(true, FakeRandom{}).resolution == IdentityResolution::restored);
    const auto reloaded = *store.bind(a, store.lifetime());
    CHECK(reloaded.loaded_context == earlier_disk);
    CHECK(!store.acknowledge(first.epoch, {a, "session:later-live", 7, 99}));
    CHECK(store.acknowledge(reloaded.epoch, after_reload));
    CHECK(reloaded.loaded_context == earlier_disk);

    // The offline save restores the same UUID without any checkpoint.
    CHECK(load_save(store, {{offline}}).resolution == IdentityResolution::restored);
    const auto blank = *store.bind(a, store.lifetime());
    CHECK(!blank.loaded_context && !saved_anchor(store));
    CHECK(!store.acknowledge(reloaded.epoch, after_reload));
    CHECK(store.acknowledge(blank.epoch, after_reload));
}

void test_saved_context_identity_resolution() {
    using namespace synth::core;
    const auto a = uuid_of(0x11);
    SaveContextStore store;
    int calls{};
    // NewGame identities are unique. F4SE publishes NewGame only after the original NewGame_Internal returns, so
    // a Revert inside the original precedes publication and cannot touch the new identity; every later revert
    // revokes it at once, bound or not, online or offline. The exact engine order remains in-game acceptance.
    CHECK(store.revert());
    CHECK(store.start_new_game(FakeRandom{random_bytes(0x11), &calls}).resolution == IdentityResolution::new_game);
    CHECK(store.playthrough_id() == a);
    const auto stamped = saved_record(store); // An early autosave persists the identity before any session or ACK.
    CHECK(store.start_new_game(FakeRandom{random_bytes(0x22), &calls}).resolution == IdentityResolution::new_game);
    CHECK(store.playthrough_id() == uuid_of(0x22) && calls == 2);
    const auto never_bound = store.lifetime();
    CHECK(never_bound != 0 && store.revert()); // Never bound and offline: revoked immediately.
    CHECK(!store.playthrough_id() && !store.for_save() && store.lifetime() == 0 && !store.bind(uuid_of(0x22), never_bound));
    // A revert while NewGame draws its UUID supersedes that UUID.
    CHECK(store.start_new_game([&store] {
        CHECK(store.revert());
        return std::optional{random_bytes(0x23)};
    }).resolution == IdentityResolution::stale);
    CHECK(!store.playthrough_id() && !store.identity_ready());
    // A stale draw has no lifecycle consequence: the later NewGame that resolved during it keeps its world.
    ResolvedIdentity nested{};
    const auto outer = store.start_new_game([&store, &nested] {
        nested = store.start_new_game([] { return std::optional{random_bytes(0x24)}; });
        return std::optional{random_bytes(0x25)};
    });
    const auto nested_lifetime = store.lifetime();
    CHECK(outer.resolution == IdentityResolution::stale && new_game_lifecycle(outer) == NewGameLifecycle::none);
    CHECK(new_game_lifecycle(nested) == NewGameLifecycle::ready && store.playthrough_id() == uuid_of(0x24));
    CHECK(nested_lifetime != 0 && store.bind(uuid_of(0x24), nested_lifetime));
    CHECK(new_game_lifecycle({IdentityResolution::random_failed}) == NewGameLifecycle::teardown);
    // The lifetime (VR's capture epoch) changes the moment a Revert, PreLoadGame or NewGame begins, before
    // any resolution or deferred lifecycle teardown.
    std::uint64_t during_draw{1};
    CHECK(store.start_new_game([&store, &during_draw] {
        during_draw = store.lifetime();
        return std::optional{random_bytes(0x26)};
    }).resolution == IdentityResolution::new_game);
    CHECK(during_draw == 0 && store.lifetime() != nested_lifetime && store.lifetime() != 0);
    store.begin_load();
    CHECK(store.lifetime() == 0);
    store.revert();
    CHECK(store.finish_load(true, FakeRandom{random_bytes(0x27)}).resolution == IdentityResolution::separated);
    const auto separated_lifetime = store.lifetime();
    CHECK(store.revert() && store.lifetime() == 0 && separated_lifetime != 0);

    // A valid record restores without calling the random generator.
    CHECK(load_save(store, {{stamped}}).resolution == IdentityResolution::restored && store.playthrough_id() == a);
    CHECK(store.finish_load(true, FakeRandom{random_bytes(0x33), &calls}).resolution == IdentityResolution::stale);
    CHECK(calls == 2 && store.playthrough_id() == a);

    // Anything else gets a fresh isolated UUID with no anchor; the unstamped save stays unstamped until saved.
    auto legacy = stamped;
    legacy.version = 1;
    auto wrong_lane = stamped;
    wrong_lane.data[0] = 2;
    auto corrupt = stamped;
    corrupt.data[7] ^= 0xFF;
    const SaveRecord unknown{1234, 1, 0, {}};
    const std::vector<std::pair<std::vector<std::vector<SaveRecord>>, LoadedIdentity>> separated{
        {{}, LoadedIdentity::missing},
        {{{unknown}}, LoadedIdentity::missing},
        {{{legacy}}, LoadedIdentity::legacy},
        {{{wrong_lane}}, LoadedIdentity::wrong_lane},
        {{{corrupt}}, LoadedIdentity::malformed},
        {{{{saved_context_record_type, 3, 292, stamped.data}}}, LoadedIdentity::unsupported},
        {{{{saved_context_record_type, 2, 1'000'000, stamped.data}}}, LoadedIdentity::malformed},
        {{{stamped, stamped}}, LoadedIdentity::duplicate},
        {{{stamped}, {stamped}}, LoadedIdentity::duplicate}, // Duplicate plugin blocks call the callback twice.
        {{std::vector<SaveRecord>(65, unknown)}, LoadedIdentity::malformed},
    };
    std::uint8_t fill = 0x40;
    for (const auto& [callbacks, cause] : separated) {
        const auto resolved = load_save(store, callbacks, ++fill);
        CHECK(resolved.resolution == IdentityResolution::separated && resolved.cause == cause);
        CHECK(store.playthrough_id() == uuid_of(fill) && !saved_anchor(store));
        CHECK(store.bind(uuid_of(fill), store.lifetime()) && !store.bind(a, store.lifetime()));
    }
    SaveSerialization short_read{{stamped}, 0, true};
    store.begin_load();
    read_saved_context(short_read, store);
    CHECK(store.finish_load(true, FakeRandom{random_bytes(0x60)}).cause == LoadedIdentity::malformed);

    // A revert after the record was staged contradicts it.
    store.begin_load();
    read_saved_context(SaveSerialization{{stamped}}, store);
    store.revert();
    CHECK(store.finish_load(true, FakeRandom{random_bytes(0x61)}).cause == LoadedIdentity::contradicted);
    CHECK(store.playthrough_id() == uuid_of(0x61));

    // A failed load, a callback outside a load and a superseded resolution activate nothing.
    CHECK(load_save(store, {{stamped}}, 0x62, false).resolution == IdentityResolution::load_failed);
    CHECK(!store.playthrough_id() && !store.for_save());
    read_saved_context(SaveSerialization{{stamped}}, store);
    CHECK(store.finish_load(true, FakeRandom{random_bytes(0x63)}).resolution == IdentityResolution::stale);
    CHECK(!store.playthrough_id());
    store.begin_load();
    const auto superseded = store.finish_load(true, [&store] {
        store.begin_load(); // Another load starts while the fresh UUID is being drawn.
        return std::optional{random_bytes(0x64)};
    });
    CHECK(superseded.resolution == IdentityResolution::stale && !store.playthrough_id());

    // Random failure fails closed, including a throwing generator, and leaves the load closed.
    CHECK(store.finish_load(true, FakeRandom{}).resolution == IdentityResolution::random_failed);
    CHECK(!store.playthrough_id() && !store.for_save() && !store.bind(a, store.lifetime()));
    CHECK(store.finish_load(true, FakeRandom{random_bytes(0x65)}).resolution == IdentityResolution::stale);
    CHECK(store.start_new_game([]() -> std::optional<PlaythroughUuid> { throw std::runtime_error{"rng"}; })
              .resolution == IdentityResolution::random_failed);
    CHECK(!store.playthrough_id());
    CHECK(load_save(store, {{stamped}}).resolution == IdentityResolution::restored); // Recovery is a later load.

    // The VR lane accepts only its own records.
    SaveContextStore vr{RuntimeVariant::vr};
    CHECK(load_save(vr, {{stamped}}).cause == LoadedIdentity::wrong_lane);
    CHECK(load_save(vr, {{saved_record(vr)}}).resolution == IdentityResolution::restored);
    CHECK(decode_saved_record(*vr.for_save())->lane == RuntimeVariant::vr);
    SaveSerialization failing;
    failing.write_ok = false;
    CHECK(!write_saved_context(failing, vr));
}

// Each load callback stages only into the transaction it entered, and each PostLoadGame resolves at most once.
void test_saved_context_load_transactions() {
    using namespace synth::core;
    const auto a = uuid_of(0x11);
    SaveContextStore store;
    (void)store.start_new_game(FakeRandom{random_bytes(0x11)});
    const auto stamped = saved_record(store);
    const auto first_lifetime = store.lifetime();

    // Same UUID, new lifetime: a snapshot stamped before the reload cannot bind.
    CHECK(load_save(store, {{stamped}}).resolution == IdentityResolution::restored && store.playthrough_id() == a);
    CHECK(store.lifetime() != first_lifetime && !store.bind(a, first_lifetime) && !store.bind(a, 0));
    CHECK(store.bind(a, store.lifetime()));

    // Load A's reader is still running when load B opens: it cannot stage A's record into B.
    store.begin_load();
    SaveSerialization reader_a{{stamped}};
    reader_a.during = [&store] { store.begin_load(); };
    read_saved_context(reader_a, store);
    auto resolved = store.finish_load(true, FakeRandom{random_bytes(0x31)});
    CHECK(resolved.resolution == IdentityResolution::separated && resolved.cause == LoadedIdentity::missing);
    CHECK(store.playthrough_id() == uuid_of(0x31));
    {
        store.begin_load();
        auto late = store.enter_load(); // A claim on load C...
        store.begin_load();             // ...outlives it into load D.
        late.stage(LoadedIdentity::restored, decode_saved_record(stamped.data));
        CHECK(store.finish_load(true, FakeRandom{random_bytes(0x32)}).cause == LoadedIdentity::missing);
    }

    // PostLoadGame while a callback is still reading never restores from it; its late staging is ignored.
    store.begin_load();
    SaveSerialization premature{{stamped}};
    premature.during = [&] { resolved = store.finish_load(true, FakeRandom{random_bytes(0x33)}); };
    read_saved_context(premature, store);
    CHECK(resolved.resolution == IdentityResolution::separated && resolved.cause == LoadedIdentity::incomplete);
    CHECK(store.playthrough_id() == uuid_of(0x33));
    CHECK(store.finish_load(true, FakeRandom{random_bytes(0x34)}).resolution == IdentityResolution::stale);
    CHECK(store.playthrough_id() == uuid_of(0x33));

    // A duplicate PostLoadGame while the fresh UUID is drawn resolves nothing; the first completes once.
    int calls{};
    store.begin_load();
    std::optional<ResolvedIdentity> duplicate;
    resolved = store.finish_load(true, [&] {
        ++calls;
        duplicate = store.finish_load(true, FakeRandom{random_bytes(0x35), &calls});
        return std::optional{random_bytes(0x36)};
    });
    CHECK(duplicate && duplicate->resolution == IdentityResolution::stale && calls == 1);
    CHECK(resolved.resolution == IdentityResolution::separated && store.playthrough_id() == uuid_of(0x36));
    // A revert while a load's fresh UUID is drawn supersedes it and reports a revoked world.
    store.begin_load();
    resolved = store.finish_load(true, [&store] {
        CHECK(store.revert());
        return std::optional{random_bytes(0x37)};
    });
    CHECK(resolved.resolution == IdentityResolution::stale && !store.playthrough_id());

    // A revert inside the load keeps the load but contradicts a record whose callback already began.
    store.begin_load();
    SaveSerialization contradicted{{stamped}};
    contradicted.during = [&store] { CHECK(!store.revert()); };
    read_saved_context(contradicted, store);
    CHECK(!store.playthrough_id());
    CHECK(store.finish_load(true, FakeRandom{random_bytes(0x38)}).cause == LoadedIdentity::contradicted);
    CHECK(store.playthrough_id() == uuid_of(0x38));

    // A throwing reader stages a malformed outcome through its claim, so the load is not left incomplete.
    store.begin_load();
    SaveSerialization throwing{{stamped}};
    throwing.throw_read = true;
    check_throws<std::runtime_error>([&] { read_saved_context(throwing, store); });
    CHECK(store.finish_load(true, FakeRandom{random_bytes(0x39)}).cause == LoadedIdentity::malformed);
    // A missing interface claims and abandons the load the same way; outside a load nothing is claimed.
    store.begin_load();
    (void)store.enter_load();
    CHECK(store.finish_load(true, FakeRandom{random_bytes(0x3A)}).cause == LoadedIdentity::malformed);
    CHECK(!store.enter_load().open());
    CHECK(load_save(store, {{stamped}}).resolution == IdentityResolution::restored); // The record itself is intact.
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
        test_saved_context_same_world_replacement();
        test_saved_context_disk_load_replaces_live_frontier();
        test_saved_context_identity_resolution();
    test_saved_context_load_transactions();
        std::cout << "lifecycle tests passed (" << assertions << " assertions)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
