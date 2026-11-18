#include "transport/notify_wake.hpp"
#include "transport/transport_state.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using synth::core::Game;
using synth::core::RuntimeGeneration;
using synth::core::RuntimeVariant;
using namespace synth::transport;

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

Correlation correlation(std::string request,
                        std::uint64_t turn,
                        RuntimeVariant variant,
                        std::uint64_t runtime_generation = 1,
                        std::uint64_t response_generation = 1) {
    return {std::move(request),
            turn,
            Game::fallout4,
            variant,
            RuntimeGeneration::from_value(runtime_generation),
            ResponseGeneration::from_value(response_generation)};
}

LineEnvelope envelope(Correlation value, std::string line_id, std::size_t bytes = 10) {
    return {std::move(value), std::move(line_id), bytes};
}

DecodedLine start(Correlation value, std::string line_id = "start", std::size_t bytes = 10) {
    return ResponseStart{envelope(std::move(value), std::move(line_id), bytes)};
}

DecodedLine output(Correlation value, std::string line_id, std::size_t bytes = 10) {
    return OutputLine{envelope(std::move(value), std::move(line_id), bytes), "decoded text"};
}

DecodedLine terminal(Correlation value,
                     std::string line_id = "terminal",
                     RemoteTerminalStatus status = RemoteTerminalStatus::completed,
                     std::size_t bytes = 10) {
    return TerminalLine{envelope(std::move(value), std::move(line_id), bytes), status, {}};
}

void test_flat_and_vr_correlation() {
    for (const auto variant : {RuntimeVariant::flat, RuntimeVariant::vr}) {
        TransportState state;
        const auto expected = state.begin_turn("request", 7, Game::fallout4, variant,
                                               RuntimeGeneration::from_value(3));
        CHECK(expected.variant == variant);
        CHECK(expected.response_generation.value() == 1);
        CHECK(state.ingest(start(expected), RuntimeGeneration::from_value(3)) ==
              IngestResult::accepted);
        CHECK(state.ingest(output(expected, "text"), RuntimeGeneration::from_value(3)) ==
              IngestResult::accepted);
        CHECK(state.ingest(terminal(expected), RuntimeGeneration::from_value(3)) ==
              IngestResult::accepted);
        CHECK(state.active()->terminal_state()->reason == TerminalReason::remote_completed);
        CHECK(state.active()->terminal_transitions() == 1);
    }

    TransportState state;
    const auto flat = state.begin_turn("request", 1, Game::fallout4, RuntimeVariant::flat,
                                       RuntimeGeneration::from_value(1));
    auto wrong_variant = flat;
    wrong_variant.variant = RuntimeVariant::vr;
    CHECK(state.ingest(start(wrong_variant), RuntimeGeneration::from_value(1)) ==
          IngestResult::rejected_mismatched_correlation);
}

void test_malformed_order_and_duplicates() {
    const auto expected = correlation("order", 1, RuntimeVariant::flat);
    NdjsonStreamState missing_start{expected, {}};
    CHECK(missing_start.ingest(output(expected, "early"), RuntimeGeneration::from_value(1)) ==
          IngestResult::rejected_malformed_order);
    CHECK(missing_start.terminal_state()->reason == TerminalReason::malformed_stream);
    CHECK(missing_start.terminal_transitions() == 1);

    NdjsonStreamState duplicate{expected, {}};
    CHECK(duplicate.ingest(start(expected), RuntimeGeneration::from_value(1)) ==
          IngestResult::accepted);
    CHECK(duplicate.ingest(start(expected), RuntimeGeneration::from_value(1)) ==
          IngestResult::duplicate);
    CHECK(duplicate.accepted_lines() == 1);
    CHECK(!duplicate.terminal());

    NdjsonStreamState second_start{expected, {}};
    CHECK(second_start.ingest(start(expected), RuntimeGeneration::from_value(1)) ==
          IngestResult::accepted);
    CHECK(second_start.ingest(start(expected, "start-2"), RuntimeGeneration::from_value(1)) ==
          IngestResult::rejected_malformed_order);
    CHECK(second_start.terminal_transitions() == 1);
}

void test_output_after_terminal_and_exactly_once() {
    const auto expected = correlation("terminal", 1, RuntimeVariant::flat);
    NdjsonStreamState stream{expected, {}};
    CHECK(stream.ingest(start(expected), RuntimeGeneration::from_value(1)) ==
          IngestResult::accepted);
    CHECK(stream.ingest(terminal(expected), RuntimeGeneration::from_value(1)) ==
          IngestResult::accepted);
    CHECK(stream.ingest(output(expected, "late"), RuntimeGeneration::from_value(1)) ==
          IngestResult::rejected_after_terminal);
    CHECK(stream.ingest(terminal(expected, "terminal-2"), RuntimeGeneration::from_value(1)) ==
          IngestResult::rejected_after_terminal);
    CHECK(!stream.cancel("too late"));
    CHECK(stream.terminal_transitions() == 1);
    CHECK(stream.accepted_lines() == 2);
}

void test_stale_generations_and_mismatches() {
    const auto expected = correlation("generation", 4, RuntimeVariant::vr, 8, 9);
    NdjsonStreamState stream{expected, {}};

    CHECK(stream.ingest(start(expected), RuntimeGeneration::from_value(9)) ==
          IngestResult::rejected_stale_runtime_generation);

    auto stale_runtime = expected;
    stale_runtime.runtime_generation = RuntimeGeneration::from_value(7);
    CHECK(stream.ingest(start(stale_runtime), RuntimeGeneration::from_value(8)) ==
          IngestResult::rejected_stale_runtime_generation);

    auto stale_response = expected;
    stale_response.response_generation = ResponseGeneration::from_value(8);
    CHECK(stream.ingest(start(stale_response), RuntimeGeneration::from_value(8)) ==
          IngestResult::rejected_stale_response_generation);

    auto wrong_request = expected;
    wrong_request.request_id = "other";
    CHECK(stream.ingest(start(wrong_request), RuntimeGeneration::from_value(8)) ==
          IngestResult::rejected_mismatched_correlation);
    CHECK(stream.accepted_lines() == 0);
    CHECK(!stream.terminal());
}

void test_cancellation() {
    const auto expected = correlation("cancel", 2, RuntimeVariant::flat);
    NdjsonStreamState stream{expected, {}};
    CHECK(stream.cancel("player cancelled"));
    CHECK(stream.cancelled());
    CHECK(!stream.cancel());
    CHECK(stream.ingest(start(expected), RuntimeGeneration::from_value(1)) ==
          IngestResult::rejected_cancelled);
    CHECK(stream.terminal_transitions() == 1);

    NdjsonStreamState remote{expected, {}};
    CHECK(remote.ingest(start(expected), RuntimeGeneration::from_value(1)) ==
          IngestResult::accepted);
    CHECK(remote.ingest(terminal(expected, "cancelled", RemoteTerminalStatus::cancelled),
                        RuntimeGeneration::from_value(1)) == IngestResult::accepted);
    CHECK(remote.cancelled());
    CHECK(remote.terminal_transitions() == 1);
}

void test_line_and_stream_limits() {
    const auto expected = correlation("limits", 1, RuntimeVariant::flat);

    NdjsonStreamState line_bytes{expected, {.maximum_lines = 4,
                                           .maximum_line_bytes = 5,
                                           .maximum_stream_bytes = 20}};
    CHECK(line_bytes.ingest(start(expected, "large", 6), RuntimeGeneration::from_value(1)) ==
          IngestResult::rejected_limit);
    CHECK(line_bytes.terminal_state()->reason == TerminalReason::limit_exceeded);

    NdjsonStreamState lines{expected, {.maximum_lines = 2,
                                      .maximum_line_bytes = 10,
                                      .maximum_stream_bytes = 20}};
    CHECK(lines.ingest(start(expected, "s", 5), RuntimeGeneration::from_value(1)) ==
          IngestResult::accepted);
    CHECK(lines.ingest(output(expected, "o", 5), RuntimeGeneration::from_value(1)) ==
          IngestResult::accepted);
    CHECK(lines.ingest(terminal(expected, "t", RemoteTerminalStatus::completed, 5),
                       RuntimeGeneration::from_value(1)) == IngestResult::rejected_limit);
    CHECK(lines.accepted_lines() == 2);

    NdjsonStreamState bytes{expected, {.maximum_lines = 4,
                                      .maximum_line_bytes = 10,
                                      .maximum_stream_bytes = 12}};
    CHECK(bytes.ingest(start(expected, "s", 7), RuntimeGeneration::from_value(1)) ==
          IngestResult::accepted);
    CHECK(bytes.ingest(output(expected, "o", 6), RuntimeGeneration::from_value(1)) ==
          IngestResult::rejected_limit);
    CHECK(bytes.accepted_bytes() == 7);
    CHECK(bytes.terminal_transitions() == 1);

    check_throws<std::invalid_argument>([&] {
        (void)NdjsonStreamState{expected, {.maximum_lines = 0,
                                          .maximum_line_bytes = 1,
                                          .maximum_stream_bytes = 1}};
    });
}

void test_superseded_turns_and_response_generation() {
    TransportState state;
    const auto runtime = RuntimeGeneration::from_value(4);
    const auto first = state.begin_turn("request-1", 10, Game::fallout4, RuntimeVariant::flat,
                                        runtime);
    CHECK(state.ingest(start(first), runtime) == IngestResult::accepted);

    const auto second = state.begin_turn("request-2", 11, Game::fallout4, RuntimeVariant::flat,
                                         runtime);
    CHECK(second.response_generation.value() == first.response_generation.value() + 1);
    CHECK(state.active()->correlation() == second);
    CHECK(state.ingest(output(first, "old"), runtime) ==
          IngestResult::rejected_stale_response_generation);

    auto old_turn_current_generation = second;
    old_turn_current_generation.turn = 10;
    CHECK(state.ingest(start(old_turn_current_generation), runtime) ==
          IngestResult::rejected_superseded_turn);
    CHECK(state.ingest(start(second), runtime) == IngestResult::accepted);
    CHECK(state.ingest(terminal(second), runtime) == IngestResult::accepted);
    CHECK(state.active()->terminal_transitions() == 1);

    TransportState no_turn;
    CHECK(no_turn.ingest(start(second), runtime) == IngestResult::rejected_no_active_turn);
}

void test_value_validation() {
    check_throws<std::invalid_argument>([] { (void)ResponseGeneration::from_value(0); });
    check_throws<std::invalid_argument>([] {
        (void)ResponseGeneration::from_value(maximum_wire_integer + 1);
    });
    check_throws<std::invalid_argument>([] {
        (void)NdjsonStreamState{correlation("", 1, RuntimeVariant::flat), {}};
    });
    check_throws<std::invalid_argument>([] {
        (void)NdjsonStreamState{correlation("request", 0, RuntimeVariant::flat), {}};
    });
}

// ---- Optional wake-up channel -------------------------------------------------------------------------------------

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// A scripted connection: frames are pushed by the test, receive() blocks like a socket until one arrives.
class FakeConnection final : public INotifyConnection {
public:
    void push(std::string frame) {
        {
            std::scoped_lock lock{mutex_};
            frames_.push_back(std::move(frame));
        }
        changed_.notify_all();
    }
    void close_remote() {
        {
            std::scoped_lock lock{mutex_};
            closed_ = true;
        }
        changed_.notify_all();
    }
    [[nodiscard]] ReceiveResult receive(std::string& frame) override {
        std::unique_lock lock{mutex_};
        changed_.wait(lock, [&] { return !frames_.empty() || closed_ || aborted_; });
        if (aborted_) return ReceiveResult::failed;
        if (!frames_.empty()) {
            frame = std::move(frames_.front());
            frames_.pop_front();
            return ReceiveResult::frame;
        }
        return ReceiveResult::closed;
    }
    void abort() noexcept override {
        {
            std::scoped_lock lock{mutex_};
            aborted_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::string> frames_;
    bool closed_{};
    bool aborted_{};
};

// Forwards to a connection the test keeps hold of, so frames can be pushed after the channel owns it.
class Forwarding final : public INotifyConnection {
public:
    explicit Forwarding(std::shared_ptr<FakeConnection> target) : target_{std::move(target)} {}
    [[nodiscard]] ReceiveResult receive(std::string& frame) override { return target_->receive(frame); }
    void abort() noexcept override { target_->abort(); }

private:
    std::shared_ptr<FakeConnection> target_;
};

class FakeTransport final : public INotifyTransport {
public:
    // Connections handed out in order; once exhausted (or when none were queued) connect() fails.
    void queue(std::shared_ptr<FakeConnection> connection) {
        std::scoped_lock lock{mutex_};
        queue_.push_back(std::move(connection));
    }
    [[nodiscard]] std::unique_ptr<INotifyConnection> connect(std::string_view, std::uint16_t) override {
        std::scoped_lock lock{mutex_};
        attempts_.push_back(Clock::now());
        if (queue_.empty()) return nullptr;
        auto next = std::move(queue_.front());
        queue_.pop_front();
        return std::make_unique<Forwarding>(std::move(next));
    }
    [[nodiscard]] std::vector<Clock::time_point> attempts() {
        std::scoped_lock lock{mutex_};
        return attempts_;
    }

private:
    std::mutex mutex_;
    std::deque<std::shared_ptr<FakeConnection>> queue_;
    std::vector<Clock::time_point> attempts_;
};

constexpr const char* welcome_frame = R"({"schema":"synth.notify.hint.v1","type":"welcome"})";
constexpr const char* wake_frame = R"({"schema":"synth.notify.hint.v1","type":"wake","topic":"speech"})";

template <class Predicate>
bool eventually(Predicate&& predicate, std::chrono::milliseconds limit = 3000ms) {
    const auto deadline = Clock::now() + limit;
    while (Clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

void test_notify_hint_parsing() {
    CHECK(parse_notify_hint(welcome_frame) == HintKind::welcome);
    CHECK(parse_notify_hint(wake_frame) == HintKind::wake);
    // A hint is closed: extra keys (any message content), wrong shapes and unknown values are all ignored.
    CHECK(!parse_notify_hint(R"({"schema":"synth.notify.hint.v1","type":"wake","topic":"speech","text":"hi"})"));
    CHECK(!parse_notify_hint(R"({"schema":"synth.notify.hint.v1","type":"wake"})"));
    CHECK(!parse_notify_hint(R"({"schema":"synth.notify.hint.v1","type":"welcome","topic":"speech"})"));
    CHECK(!parse_notify_hint(R"({"schema":"synth.notify.hint.v1","type":"wake","topic":"dialogue"})"));
    CHECK(!parse_notify_hint(R"({"schema":"synth.notify.hint.v2","type":"wake","topic":"speech"})"));
    CHECK(!parse_notify_hint(R"({"type":"wake","topic":"speech"})"));
    CHECK(!parse_notify_hint(R"(["wake"])"));
    CHECK(!parse_notify_hint("not json"));
    CHECK(!parse_notify_hint(""));
    CHECK(!parse_notify_hint(std::string(300, ' ') + wake_frame));
}

void test_wake_signal_ends_a_wait_and_is_never_lost() {
    WakeSignal signal;
    CHECK(!signal.connected());
    CHECK(signal.poll_interval() == 100ms);
    signal.set_connected(true);
    CHECK(signal.poll_interval() == 250ms);
    signal.set_connected(false);

    // Nothing arrives: the wait ends at its deadline, which is plain polling.
    const auto seen = signal.epoch();
    CHECK(!signal.wait_until(Clock::now() + 30ms, seen));
    // A hint that lands between reading the epoch and waiting is not lost.
    signal.notify();
    const auto start = Clock::now();
    CHECK(signal.wait_until(start + 5s, seen));
    CHECK(Clock::now() - start < 1s);
    // A hint from another thread ends a long wait early.
    const auto before = signal.epoch();
    std::thread sender{[&] {
        std::this_thread::sleep_for(20ms);
        signal.notify();
    }};
    const auto waited = Clock::now();
    CHECK(signal.wait_until(waited + 10s, before));
    CHECK(Clock::now() - waited < 2s);
    sender.join();
}

void test_a_hint_wakes_the_poll_through_the_channel() {
    FakeTransport transport;
    auto connection = std::make_shared<FakeConnection>();
    transport.queue(connection);
    WakeSignal signal;
    std::vector<std::string> logs;
    std::mutex log_mutex;
    NotifyChannel channel{transport, signal, [&](std::string_view line) {
        std::scoped_lock lock{log_mutex};
        logs.emplace_back(line);
    }};
    const auto seen = signal.epoch();
    channel.start("127.0.0.1", 8091);
    // A wake before the welcome is not trusted; the welcome itself wakes once (state may have changed while down).
    connection->push(wake_frame);
    connection->push(welcome_frame);
    CHECK(eventually([&] { return signal.connected(); }));
    CHECK(signal.epoch() == seen + 1);
    const auto after_welcome = signal.epoch();
    std::thread sender{[&] {
        std::this_thread::sleep_for(20ms);
        connection->push("not a hint");
        connection->push(wake_frame);
    }};
    const auto start = Clock::now();
    CHECK(signal.wait_until(start + 10s, after_welcome));
    CHECK(Clock::now() - start < 2s);
    sender.join();
    channel.stop();
    CHECK(!signal.connected());
    std::scoped_lock lock{log_mutex};
    CHECK(logs.empty()); // Connecting is silent; only an outage is reported.
}

void test_absent_channel_leaves_polling_unchanged() {
    FakeTransport transport; // Every connect fails: the server is old or the channel is not running.
    WakeSignal signal;
    std::atomic_int logged{};
    NotifyChannel channel{transport, signal, [&](std::string_view) { ++logged; }, {1ms, 4ms}};
    const auto seen = signal.epoch();
    channel.start("127.0.0.1", 8091);
    CHECK(eventually([&] { return transport.attempts().size() >= 4; }));
    CHECK(signal.epoch() == seen);
    CHECK(!signal.connected());
    CHECK(signal.poll_interval() == 100ms);
    CHECK(!signal.wait_until(Clock::now() + 20ms, seen)); // The poll sleeps its full interval, as before.
    CHECK(logged == 1);                                    // One line for the whole outage, not one per retry.
    const auto started = Clock::now();
    channel.stop();
    CHECK(Clock::now() - started < 2s);
}

void test_reconnect_backs_off_and_does_not_spin() {
    ReconnectBackoff backoff{20ms, 80ms};
    CHECK(backoff.next() == 20ms);
    CHECK(backoff.next() == 40ms);
    CHECK(backoff.next() == 80ms);
    CHECK(backoff.next() == 80ms);
    backoff.reset();
    CHECK(backoff.next() == 20ms);

    // A channel that is never reachable: each gap is at least the scheduled delay, and the attempts stay few.
    FakeTransport transport;
    WakeSignal signal;
    NotifyChannel channel{transport, signal, {}, {20ms, 80ms}};
    channel.start("127.0.0.1", 8091);
    std::this_thread::sleep_for(450ms);
    channel.stop();
    const auto attempts = transport.attempts();
    CHECK(attempts.size() >= 4 && attempts.size() <= 9);
    const std::chrono::milliseconds expected[] = {20ms, 40ms, 80ms, 80ms};
    for (std::size_t i = 0; i < 4; ++i) CHECK(attempts[i + 1] - attempts[i] >= expected[i] - 2ms);
}

void test_a_connection_that_never_welcomes_does_not_reset_the_backoff() {
    FakeTransport transport;
    for (int i = 0; i < 4; ++i) {
        auto connection = std::make_shared<FakeConnection>();
        connection->close_remote(); // Accepts, then closes without a welcome frame.
        transport.queue(connection);
    }
    WakeSignal signal;
    NotifyChannel channel{transport, signal, {}, {20ms, 80ms}};
    channel.start("127.0.0.1", 8091);
    CHECK(eventually([&] { return transport.attempts().size() >= 4; }));
    channel.stop();
    const auto attempts = transport.attempts();
    CHECK(attempts[1] - attempts[0] >= 18ms);
    CHECK(attempts[2] - attempts[1] >= 38ms);
    CHECK(attempts[3] - attempts[2] >= 78ms);
    CHECK(!signal.connected());
}

void test_a_dropped_channel_returns_to_plain_polling_and_reconnects() {
    FakeTransport transport;
    auto first = std::make_shared<FakeConnection>();
    auto second = std::make_shared<FakeConnection>();
    transport.queue(first);
    transport.queue(second);
    WakeSignal signal;
    std::atomic_int logged{};
    NotifyChannel channel{transport, signal, [&](std::string_view) { ++logged; }, {5ms, 20ms}};
    channel.start("127.0.0.1", 8091);
    first->push(welcome_frame);
    CHECK(eventually([&] { return signal.connected(); }));
    first->close_remote();
    CHECK(eventually([&] { return !signal.connected(); }));
    CHECK(signal.poll_interval() == 100ms); // Back to the short interval the moment the channel is gone.
    second->push(welcome_frame);
    CHECK(eventually([&] { return signal.connected(); }));
    CHECK(logged == 1); // The loss was reported once.
    channel.stop();
}

void test_stop_ends_a_blocked_receive_and_shutdown_refuses_restart() {
    FakeTransport transport;
    auto connection = std::make_shared<FakeConnection>();
    transport.queue(connection);
    WakeSignal signal;
    NotifyChannel channel{transport, signal, {}, {5ms, 20ms}};
    channel.start("127.0.0.1", 8091);
    connection->push(welcome_frame);
    CHECK(eventually([&] { return signal.connected(); }));
    const auto started = Clock::now();
    channel.stop(); // The thread is blocked in receive(); abort() must release it.
    CHECK(Clock::now() - started < 2s);
    CHECK(!signal.connected());
    channel.shutdown();
    const auto attempts = transport.attempts().size();
    channel.start("127.0.0.1", 8091);
    std::this_thread::sleep_for(30ms);
    CHECK(transport.attempts().size() == attempts);
}

}  // namespace

int main() {
    try {
        test_flat_and_vr_correlation();
        test_malformed_order_and_duplicates();
        test_output_after_terminal_and_exactly_once();
        test_stale_generations_and_mismatches();
        test_cancellation();
        test_line_and_stream_limits();
        test_superseded_turns_and_response_generation();
        test_value_validation();
        test_notify_hint_parsing();
        test_wake_signal_ends_a_wait_and_is_never_lost();
        test_a_hint_wakes_the_poll_through_the_channel();
        test_absent_channel_leaves_polling_unchanged();
        test_reconnect_backs_off_and_does_not_spin();
        test_a_connection_that_never_welcomes_does_not_reset_the_backoff();
        test_a_dropped_channel_returns_to_plain_polling_and_reconnects();
        test_stop_ends_a_blocked_receive_and_shutdown_refuses_restart();
        std::cout << "transport tests passed (" << assertions << " assertions)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
