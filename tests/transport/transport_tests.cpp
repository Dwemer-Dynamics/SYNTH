#include "transport/transport_state.hpp"

#include <exception>
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
        std::cout << "transport tests passed (" << assertions << " assertions)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
