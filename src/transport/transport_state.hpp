#pragma once

#include "core/generation.hpp"
#include "core/runtime_identity.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>

namespace synth::transport {

inline constexpr std::uint64_t maximum_wire_integer = 9'007'199'254'740'991ULL;

class ResponseGeneration final {
public:
    using value_type = std::uint64_t;

    constexpr ResponseGeneration() noexcept = default;

    [[nodiscard]] static constexpr ResponseGeneration from_value(value_type value) {
        if (value == 0 || value > maximum_wire_integer) {
            throw std::invalid_argument{"response generation is outside the wire-safe range"};
        }
        return ResponseGeneration{value};
    }

    [[nodiscard]] constexpr value_type value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

    auto operator<=>(const ResponseGeneration&) const = default;

private:
    explicit constexpr ResponseGeneration(value_type value) noexcept : value_{value} {}
    value_type value_{};
};

class ResponseGenerationClock final {
public:
    [[nodiscard]] ResponseGeneration next() {
        if (value_ == maximum_wire_integer) {
            throw std::overflow_error{"response generation exhausted"};
        }
        return ResponseGeneration::from_value(++value_);
    }

private:
    ResponseGeneration::value_type value_{};
};

struct Correlation final {
    std::string request_id;
    std::uint64_t turn{};
    core::Game game{core::Game::fallout4};
    core::RuntimeVariant variant{core::RuntimeVariant::flat};
    core::RuntimeGeneration runtime_generation;
    ResponseGeneration response_generation;

    auto operator<=>(const Correlation&) const = default;
};

struct LineEnvelope final {
    Correlation correlation;
    std::string line_id;
    std::size_t wire_bytes{};
};

struct ResponseStart final {
    LineEnvelope envelope;
};

struct OutputLine final {
    LineEnvelope envelope;
    std::string text;
};

enum class RemoteTerminalStatus : unsigned char {
    completed,
    failed,
    cancelled,
};

struct TerminalLine final {
    LineEnvelope envelope;
    RemoteTerminalStatus status{RemoteTerminalStatus::completed};
    std::string detail;
};

using DecodedLine = std::variant<ResponseStart, OutputLine, TerminalLine>;

struct StreamLimits final {
    std::size_t maximum_lines{1'024};
    std::size_t maximum_line_bytes{64 * 1'024};
    std::size_t maximum_stream_bytes{4 * 1'024 * 1'024};
};

enum class IngestResult : unsigned char {
    accepted,
    duplicate,
    rejected_no_active_turn,
    rejected_mismatched_correlation,
    rejected_stale_runtime_generation,
    rejected_stale_response_generation,
    rejected_superseded_turn,
    rejected_cancelled,
    rejected_after_terminal,
    rejected_malformed_order,
    rejected_limit,
};

enum class TerminalReason : unsigned char {
    remote_completed,
    remote_failed,
    remote_cancelled,
    locally_cancelled,
    superseded,
    malformed_stream,
    limit_exceeded,
};

struct TerminalState final {
    TerminalReason reason;
    std::string detail;
};

class NdjsonStreamState final {
public:
    NdjsonStreamState(Correlation expected, StreamLimits limits)
        : expected_{std::move(expected)}, limits_{limits} {
        validate_correlation(expected_);
        if (limits_.maximum_lines == 0 || limits_.maximum_line_bytes == 0 ||
            limits_.maximum_stream_bytes == 0 ||
            limits_.maximum_line_bytes > limits_.maximum_stream_bytes) {
            throw std::invalid_argument{"invalid NDJSON stream limits"};
        }
    }

    [[nodiscard]] const Correlation& correlation() const noexcept { return expected_; }
    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] bool terminal() const noexcept { return terminal_.has_value(); }
    [[nodiscard]] bool cancelled() const noexcept {
        return terminal_ && (terminal_->reason == TerminalReason::locally_cancelled ||
                             terminal_->reason == TerminalReason::remote_cancelled);
    }
    [[nodiscard]] const std::optional<TerminalState>& terminal_state() const noexcept {
        return terminal_;
    }
    [[nodiscard]] std::size_t accepted_lines() const noexcept { return accepted_lines_; }
    [[nodiscard]] std::size_t accepted_bytes() const noexcept { return accepted_bytes_; }
    [[nodiscard]] std::size_t terminal_transitions() const noexcept {
        return terminal_transitions_;
    }

    [[nodiscard]] IngestResult ingest(const DecodedLine& line,
                                      core::RuntimeGeneration current_runtime_generation) {
        const auto& envelope = envelope_of(line);

        if (envelope.correlation.runtime_generation != current_runtime_generation) {
            return IngestResult::rejected_stale_runtime_generation;
        }
        if (envelope.correlation.response_generation != expected_.response_generation) {
            return IngestResult::rejected_stale_response_generation;
        }
        if (!same_request_context(envelope.correlation, expected_)) {
            return envelope.correlation.turn < expected_.turn
                       ? IngestResult::rejected_superseded_turn
                       : IngestResult::rejected_mismatched_correlation;
        }
        if (seen_line_ids_.contains(envelope.line_id)) {
            return IngestResult::duplicate;
        }
        if (terminal_) {
            return cancelled() ? IngestResult::rejected_cancelled
                               : IngestResult::rejected_after_terminal;
        }
        if (envelope.line_id.empty() || envelope.wire_bytes == 0) {
            terminate_once(TerminalReason::malformed_stream, "invalid line envelope");
            return IngestResult::rejected_malformed_order;
        }
        if (envelope.wire_bytes > limits_.maximum_line_bytes ||
            accepted_lines_ == limits_.maximum_lines ||
            envelope.wire_bytes > limits_.maximum_stream_bytes - accepted_bytes_) {
            terminate_once(TerminalReason::limit_exceeded, "NDJSON stream limit exceeded");
            return IngestResult::rejected_limit;
        }

        if (std::holds_alternative<ResponseStart>(line)) {
            if (started_) {
                terminate_once(TerminalReason::malformed_stream, "response start was not first");
                return IngestResult::rejected_malformed_order;
            }
            started_ = true;
        } else if (!started_) {
            terminate_once(TerminalReason::malformed_stream, "response line preceded start");
            return IngestResult::rejected_malformed_order;
        }

        seen_line_ids_.insert(envelope.line_id);
        ++accepted_lines_;
        accepted_bytes_ += envelope.wire_bytes;

        if (const auto* terminal_line = std::get_if<TerminalLine>(&line)) {
            switch (terminal_line->status) {
            case RemoteTerminalStatus::completed:
                terminate_once(TerminalReason::remote_completed, terminal_line->detail);
                break;
            case RemoteTerminalStatus::failed:
                terminate_once(TerminalReason::remote_failed, terminal_line->detail);
                break;
            case RemoteTerminalStatus::cancelled:
                terminate_once(TerminalReason::remote_cancelled, terminal_line->detail);
                break;
            }
        }
        return IngestResult::accepted;
    }

    [[nodiscard]] bool cancel(std::string detail = {}) {
        return terminate_once(TerminalReason::locally_cancelled, std::move(detail));
    }

    [[nodiscard]] bool supersede() {
        return terminate_once(TerminalReason::superseded, "turn superseded");
    }

private:
    [[nodiscard]] static const LineEnvelope& envelope_of(const DecodedLine& line) noexcept {
        return std::visit([](const auto& value) -> const LineEnvelope& { return value.envelope; },
                          line);
    }

    static void validate_correlation(const Correlation& value) {
        if (value.request_id.empty() || value.turn == 0 || value.turn > maximum_wire_integer ||
            !value.runtime_generation.valid() || !value.response_generation.valid()) {
            throw std::invalid_argument{"invalid response correlation"};
        }
    }

    [[nodiscard]] static bool same_request_context(const Correlation& lhs,
                                                   const Correlation& rhs) noexcept {
        return lhs.request_id == rhs.request_id && lhs.turn == rhs.turn && lhs.game == rhs.game &&
               lhs.variant == rhs.variant &&
               lhs.runtime_generation == rhs.runtime_generation;
    }

    bool terminate_once(TerminalReason reason, std::string detail) {
        if (terminal_) {
            return false;
        }
        terminal_.emplace(TerminalState{reason, std::move(detail)});
        ++terminal_transitions_;
        return true;
    }

    Correlation expected_;
    StreamLimits limits_;
    std::unordered_set<std::string> seen_line_ids_;
    std::optional<TerminalState> terminal_;
    std::size_t accepted_lines_{};
    std::size_t accepted_bytes_{};
    std::size_t terminal_transitions_{};
    bool started_{};
};

class TransportState final {
public:
    explicit TransportState(StreamLimits limits = {}) : limits_{limits} {}

    [[nodiscard]] Correlation begin_turn(std::string request_id,
                                         std::uint64_t turn,
                                         core::Game game,
                                         core::RuntimeVariant variant,
                                         core::RuntimeGeneration runtime_generation) {
        if (active_) {
            (void)active_->supersede();
        }
        Correlation correlation{std::move(request_id),
                                turn,
                                game,
                                variant,
                                runtime_generation,
                                response_generations_.next()};
        active_.emplace(correlation, limits_);
        return correlation;
    }

    [[nodiscard]] IngestResult ingest(const DecodedLine& line,
                                      core::RuntimeGeneration current_runtime_generation) {
        if (!active_) {
            return IngestResult::rejected_no_active_turn;
        }
        return active_->ingest(line, current_runtime_generation);
    }

    [[nodiscard]] bool cancel_active(std::string detail = {}) {
        return active_ && active_->cancel(std::move(detail));
    }

    [[nodiscard]] const NdjsonStreamState* active() const noexcept {
        return active_ ? &*active_ : nullptr;
    }

private:
    StreamLimits limits_;
    ResponseGenerationClock response_generations_;
    std::optional<NdjsonStreamState> active_;
};

}  // namespace synth::transport
