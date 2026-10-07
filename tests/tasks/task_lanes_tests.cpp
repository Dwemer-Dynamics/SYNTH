#include "tasks/task_lanes.hpp"

#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using synth::core::CancellationSource;
using synth::core::RuntimeGeneration;
using synth::tasks::LaneConfig;
using synth::tasks::ShutdownMode;
using synth::tasks::SubmitResult;
using synth::tasks::TaskClass;
using synth::tasks::TaskLanes;

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

struct FakeClock final {
    TaskLanes::Deadline value{};
    [[nodiscard]] TaskLanes::Deadline now() const noexcept { return value; }
    void advance(TaskLanes::Clock::duration duration) noexcept { value += duration; }
};

CancellationSource source() {
    return CancellationSource{RuntimeGeneration::initial()};
}

void test_named_lanes_and_reserved_capacity() {
    FakeClock clock;
    TaskLanes lanes{{LaneConfig{"network", 4, 2, 2}, LaneConfig{"media", 2, 1, 1}},
                    0,
                    [&] { return clock.now(); }};
    auto cancellation = source();
    const auto deadline = clock.now() + 1min;
    auto noop = [](const auto&) {};

    check_throws<std::invalid_argument>([&] {
        (void)lanes.try_submit("network", TaskClass::current_turn, deadline, {}, noop);
    });
    CHECK(lanes.health().lanes[0].accepted == 0);

    CHECK(lanes.try_submit("network", TaskClass::background, deadline, cancellation.token(), noop) ==
          SubmitResult::accepted);
    CHECK(lanes.try_submit("network", TaskClass::background, deadline, cancellation.token(), noop) ==
          SubmitResult::accepted);
    CHECK(lanes.try_submit("network", TaskClass::background, deadline, cancellation.token(), noop) ==
          SubmitResult::saturated);
    CHECK(lanes.try_submit("network", TaskClass::current_turn, deadline, cancellation.token(), noop) ==
          SubmitResult::accepted);
    CHECK(lanes.try_submit("network", TaskClass::current_turn, deadline, cancellation.token(), noop) ==
          SubmitResult::accepted);
    CHECK(lanes.try_submit("network", TaskClass::current_turn, deadline, cancellation.token(), noop) ==
          SubmitResult::saturated);

    const auto health = lanes.health();
    CHECK(health.lanes[0].pending_background == 2);
    CHECK(health.lanes[0].pending_current_turn == 2);
    CHECK(health.lanes[0].saturated == 2);
    check_throws<std::invalid_argument>([&] {
        (void)lanes.try_submit("unknown", TaskClass::background, deadline, cancellation.token(), noop);
    });
}

void test_fairness_and_lane_round_robin() {
    FakeClock clock;
    TaskLanes lanes{{LaneConfig{"network", 8, 0, 2}, LaneConfig{"media", 4, 0, 2}},
                    0,
                    [&] { return clock.now(); }};
    auto cancellation = source();
    std::vector<std::string> order;
    const auto deadline = clock.now() + 1min;
    auto submit = [&](std::string_view lane, TaskClass task_class, std::string label) {
        CHECK(lanes.try_submit(lane,
                               task_class,
                               deadline,
                               cancellation.token(),
                               [&, label = std::move(label)](const auto&) { order.push_back(label); }) ==
              SubmitResult::accepted);
    };

    submit("network", TaskClass::current_turn, "turn-1");
    submit("network", TaskClass::current_turn, "turn-2");
    submit("network", TaskClass::current_turn, "turn-3");
    submit("network", TaskClass::background, "background");
    submit("media", TaskClass::background, "media");

    CHECK(lanes.run_until_idle() == 5);
    CHECK(order == (std::vector<std::string>{"turn-1", "media", "turn-2", "background", "turn-3"}));
    CHECK(lanes.health().lanes[0].executed == 4);
}

void test_deadline_cancellation_and_exception_containment() {
    FakeClock clock;
    TaskLanes lanes{{LaneConfig{"work", 6, 1, 1}}, 0, [&] { return clock.now(); }};
    auto live = source();
    auto cancelled = source();
    cancelled.cancel();
    int executed{};

    CHECK(lanes.try_submit("work",
                           TaskClass::background,
                           clock.now() + 1s,
                           live.token(),
                           [&](const auto&) { ++executed; }) == SubmitResult::accepted);
    CHECK(lanes.try_submit("work",
                           TaskClass::background,
                           clock.now() + 1s,
                           cancelled.token(),
                           [&](const auto&) { ++executed; }) == SubmitResult::accepted);
    CHECK(lanes.try_submit("work",
                           TaskClass::background,
                           clock.now() + 1s,
                           live.token(),
                           [](const auto&) { throw std::runtime_error{"contained"}; }) ==
          SubmitResult::accepted);
    clock.advance(1s);

    CHECK(lanes.run_until_idle() == 3);
    CHECK(executed == 0);
    const auto health = lanes.health().lanes[0];
    CHECK(health.expired == 2);
    CHECK(health.cancelled == 1);
    CHECK(health.failed == 0);

    CHECK(lanes.try_submit("work",
                           TaskClass::background,
                           clock.now() + 1s,
                           live.token(),
                           [](const auto&) { throw std::runtime_error{"contained"}; }) ==
          SubmitResult::accepted);
    CHECK(lanes.run_one());
    CHECK(lanes.health().lanes[0].failed == 1);
}

void test_joined_shutdown() {
    TaskLanes lanes{{LaneConfig{"worker", 8, 2, 2}}, 2};
    auto cancellation = source();
    std::vector<int> results;
    std::mutex results_mutex;
    for (int value = 0; value < 6; ++value) {
        CHECK(lanes.try_submit("worker",
                               TaskClass::current_turn,
                               TaskLanes::Clock::now() + 1min,
                               cancellation.token(),
                               [&, value](const auto&) {
                                   std::scoped_lock lock{results_mutex};
                                   results.push_back(value);
                               }) == SubmitResult::accepted);
    }
    lanes.shutdown(ShutdownMode::drain);
    const auto health = lanes.health();
    CHECK(health.joined);
    CHECK(!health.accepting);
    CHECK(health.lanes[0].executed == 6);
    CHECK(results.size() == 6);
    CHECK(lanes.try_submit("worker",
                           TaskClass::current_turn,
                           TaskLanes::Clock::now() + 1min,
                           cancellation.token(),
                           [](const auto&) {}) == SubmitResult::stopped);
}

void test_discard_pending_keeps_lane_available_for_terminal_work() {
    FakeClock clock;
    TaskLanes lanes{{LaneConfig{"network", 4, 1, 2}}, 0, [&] { return clock.now(); }};
    auto cancellation = source();
    int executed{};
    const auto submit = [&] {
        return lanes.try_submit("network", TaskClass::current_turn, clock.now() + 1min,
                                cancellation.token(), [&](const auto&) { ++executed; });
    };

    CHECK(submit() == SubmitResult::accepted);
    CHECK(submit() == SubmitResult::accepted);
    CHECK(lanes.discard_pending() == 2);
    CHECK(lanes.health().lanes[0].discarded == 2);
    CHECK(lanes.health().accepting);
    auto old_turn = source();
    CHECK(lanes.try_submit("network", TaskClass::background, clock.now() + 1min,
                          old_turn.token(), [&](const auto&) { ++executed; }) == SubmitResult::accepted);
    old_turn.cancel();
    CHECK(submit() == SubmitResult::accepted);
    CHECK(lanes.discard_cancelled() == 1);
    CHECK(lanes.health().lanes[0].cancelled == 1);
    CHECK(lanes.run_until_idle() == 1);
    CHECK(executed == 1);
}

void test_configuration_validation() {
    check_throws<std::invalid_argument>([] {
        (void)TaskLanes{{LaneConfig{"", 1, 0, 1}}, 0};
    });
    check_throws<std::invalid_argument>([] {
        (void)TaskLanes{{LaneConfig{"same", 1, 0, 1}, LaneConfig{"same", 1, 0, 1}}, 0};
    });
}

// Speech/action/halt admission counters must settle even when no callback runs.
void test_activity_lifetime_on_every_exit() {
    FakeClock clock;
    std::atomic_size_t active{};
    TaskLanes lanes{{LaneConfig{"work", 1, 1, 1}}, 0, [&] { return clock.now(); }};
    auto live = source();
    int executed{};
    const auto submit = [&](const auto& token, bool fail = false) {
        return lanes.try_submit("work", TaskClass::current_turn, clock.now() + 1s, token,
            TaskLanes::track_activity(active, [&, fail](const auto&) {
                CHECK(active.load() == 1);
                ++executed;
                if (fail) throw std::runtime_error{"contained"};
            }));
    };
    CHECK(submit(live.token()) == SubmitResult::accepted);
    CHECK(active.load() == 1);
    CHECK(submit(live.token()) == SubmitResult::saturated);
    CHECK(active.load() == 1);
    CHECK(lanes.run_one());
    CHECK(active.load() == 0);
    CHECK(executed == 1);
    CHECK(submit(live.token(), true) == SubmitResult::accepted);
    CHECK(lanes.run_one());
    CHECK(active.load() == 0);
    CHECK(lanes.health().lanes[0].failed == 1);

    CHECK(submit(live.token()) == SubmitResult::accepted);
    clock.advance(1s);
    CHECK(lanes.run_one());
    CHECK(active.load() == 0);
    CHECK(lanes.health().lanes[0].expired == 1);
    auto cancelled = source();
    CHECK(submit(cancelled.token()) == SubmitResult::accepted);
    cancelled.cancel();
    CHECK(lanes.run_one());
    CHECK(active.load() == 0);
    CHECK(lanes.health().lanes[0].cancelled == 1);
    CHECK(submit(live.token()) == SubmitResult::accepted);
    CHECK(lanes.discard_pending() == 1);
    CHECK(active.load() == 0);
    CHECK(submit(live.token()) == SubmitResult::accepted);
    lanes.shutdown(ShutdownMode::cancel_pending);
    CHECK(active.load() == 0);
    CHECK(submit(live.token()) == SubmitResult::stopped);
    CHECK(active.load() == 0);
    CHECK(executed == 2);

    // std::function copies share one admission; only the final owner releases it.
    auto task = TaskLanes::track_activity(active, [](const auto&) {});
    auto copy = task;
    CHECK(active.load() == 1);
    task = {};
    CHECK(active.load() == 1);
    copy = {};
    CHECK(active.load() == 0);
    check_throws<std::invalid_argument>([&] { (void)TaskLanes::track_activity(active, {}); });
    CHECK(active.load() == 0);

    // Visual jobs share one immutable frame and release both counters on discarded work.
    std::atomic_size_t visual{};
    auto pixels = std::make_shared<const std::vector<std::byte>>(1024);
    std::weak_ptr<const std::vector<std::byte>> observed = pixels;
    auto encode = TaskLanes::track_activity(visual, TaskLanes::track_activity(active,
        [pixels](const auto&) { CHECK(pixels->size() == 1024); }));
    pixels.reset();
    auto encoded_copy = encode;
    CHECK(visual.load() == 1 && active.load() == 1 && !observed.expired());
    encode = {};
    CHECK(visual.load() == 1 && active.load() == 1 && !observed.expired());
    encoded_copy = {};
    CHECK(visual.load() == 0 && active.load() == 0 && observed.expired());
}

// A slow earlier voice must hold later delivery, without occupying the network lane or leaking activity.
void test_ordered_speech_delivery_keeps_network_independent() {
    auto cancellation=source();
    std::atomic_size_t activity{};
    std::promise<void> entered, release, later, network_done;
    auto started=entered.get_future();auto gate=release.get_future().share();
    auto second=later.get_future();auto network_finished=network_done.get_future();
    std::vector<int> playback;
    TaskLanes speech{{LaneConfig{"speech",16,4,3}},1};
    TaskLanes network{{LaneConfig{"network",16,1,3}},2};
    const auto deadline=TaskLanes::Clock::now()+3s;
    const auto first_admitted=speech.try_submit("speech",TaskClass::current_turn,deadline,cancellation.token(),
        TaskLanes::track_activity(activity,[&](const auto&) {
            entered.set_value();gate.wait_until(deadline);playback.push_back(1);
        }));
    const auto first_started=started.wait_for(1s);
    const auto second_admitted=speech.try_submit("speech",TaskClass::current_turn,deadline,cancellation.token(),
        TaskLanes::track_activity(activity,[&](const auto&) {playback.push_back(2);later.set_value();}));
    const auto network_admitted=network.try_submit("network",TaskClass::current_turn,deadline,cancellation.token(),
        [&](const auto&) {network_done.set_value();});
    const auto network_state=network_finished.wait_for(1s);
    const auto second_state=second.wait_for(50ms);
    release.set_value();
    speech.shutdown(ShutdownMode::drain);network.shutdown(ShutdownMode::drain);
    CHECK(first_admitted==SubmitResult::accepted && second_admitted==SubmitResult::accepted);
    CHECK(network_admitted==SubmitResult::accepted);
    CHECK(first_started==std::future_status::ready && network_state==std::future_status::ready);
    CHECK(second_state==std::future_status::timeout);
    CHECK(playback==(std::vector<int>{1,2}));
    CHECK(activity.load()==0);
}

}  // namespace

int main() {
    try {
        test_named_lanes_and_reserved_capacity();
        test_fairness_and_lane_round_robin();
        test_deadline_cancellation_and_exception_containment();
        test_joined_shutdown();
        test_discard_pending_keeps_lane_available_for_terminal_work();
        test_configuration_validation();
        test_activity_lifetime_on_every_exit();
        test_ordered_speech_delivery_keeps_network_independent();
        std::cout << "task lane tests passed (" << assertions << " assertions)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
