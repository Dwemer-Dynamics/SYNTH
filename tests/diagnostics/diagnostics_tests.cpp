#include "diagnostics/diagnostics.hpp"
#include "presentation/dialogue_presentation.hpp"
#include "client/rechat_prefetch.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace synth::diagnostics;
int assertions{};
#define CHECK(value) do { ++assertions; if (!(value)) throw std::runtime_error{std::string{"CHECK failed: "} + #value}; } while (false)

void test_structured_record_and_payload_policy() {
    const auto suppressed = make_record(LogLevel::warning, "request-42", "retrying", "voice bytes");
    CHECK(suppressed.level == LogLevel::warning);
    CHECK(suppressed.request_id == "request-42");
    CHECK(suppressed.message == "retrying");
    CHECK(!suppressed.payload.has_value());
    CHECK(to_string(suppressed.level) == "warning");

    const auto included = make_record(LogLevel::debug, "request-43", "received", "body text",
                                      {.include_payloads = true});
    CHECK(included.payload == "body text");
}

void test_secret_redaction() {
    const auto record = make_record(
        LogLevel::error, "request-1",
        "Authorization: Bearer-secret\nx-api-key=abc123 password='hunter2' token=qwerty");
    CHECK(record.message.find("Bearer-secret") == std::string::npos);
    CHECK(record.message.find("abc123") == std::string::npos);
    CHECK(record.message.find("hunter2") == std::string::npos);
    CHECK(record.message.find("qwerty") == std::string::npos);
    CHECK(record.message.find("[REDACTED]") != std::string::npos);
}

void test_url_redaction() {
    const auto value = redact(
        "GET https://alice:password@example.com/path?access_token=topsecret&safe=yes "
        "http://user:key@127.0.0.1/");
    CHECK(value.find("alice") == std::string::npos);
    CHECK(value.find("password") == std::string::npos);
    CHECK(value.find("topsecret") == std::string::npos);
    CHECK(value.find("user:key") == std::string::npos);
    CHECK(value.find("safe=yes") != std::string::npos);
}

void test_bounds_and_buffer() {
    const auto record = make_record(LogLevel::info, "long-request", std::string(100, 'x'),
                                    std::nullopt,
                                    {.maximum_message_bytes = 20,
                                     .maximum_payload_bytes = 10,
                                     .maximum_request_id_bytes = 4});
    CHECK(record.message.size() == 20);
    CHECK(record.request_id == "long");

    Buffer buffer{2};
    buffer.push(make_record(LogLevel::info, "one", "first"));
    buffer.push(make_record(LogLevel::info, "two", "second"));
    buffer.push(make_record(LogLevel::info, "three", "third"));
    CHECK(buffer.records().size() == 2);
    CHECK(buffer.records().front().request_id == "two");
}
}  // namespace

int main() {
    try {
        test_structured_record_and_payload_policy();
        test_secret_redaction();
        test_url_redaction();
        test_bounds_and_buffer();
        auto trace = std::make_shared<ConversationTrace>();
        trace->record(ConversationStage::line_applied,7,11,"request:1","line-1");
        const auto first = trace->take();
        CHECK(first && first->generation == 7 && first->context_sequence == 11);
        CHECK(first->request_id == "request:1" && first->line_id == "line-1");
        CHECK(stage_name(first->stage) == "line_applied");
        trace->record(ConversationStage::line_applied,7,11,"Authorization: secret\n","https://private/path");
        const auto redacted = trace->take();
        CHECK(redacted->request_id == "invalid" && redacted->line_id == "invalid");
        CHECK(redacted->milliseconds >= first->milliseconds);
        for (unsigned i=0; i<130; ++i) trace->record(ConversationStage::line_applied,7);
        CHECK(trace->dropped() == 2);
        unsigned count=0; while (trace->take()) ++count;
        CHECK(count == 128);
        synth::presentation::DialogueCaption caption;
        caption.trace=trace; caption.generation=7; caption.request_id="request:1"; caption.line_id="line-1";
        caption.mark_delivery(synth::presentation::DialogueDelivery::playing);
        caption.mark_delivery(synth::presentation::DialogueDelivery::playing);
        caption.mark_delivery(synth::presentation::DialogueDelivery::spoken);
        CHECK(trace->take()->stage == ConversationStage::playing);
        CHECK(trace->take()->stage == ConversationStage::spoken);
        CHECK(!trace->take());
        auto parent=std::make_shared<synth::presentation::DialogueCaption>();
        synth::core::CancellationSource owner{synth::core::RuntimeGeneration::initial()};
        parent->cancellation=owner.token(); parent->trace=trace;
        synth::client::RechatPrefetch failed{parent,std::chrono::steady_clock::now()+std::chrono::seconds{1}};
        failed.finish(false);
        failed.cancel();
        CHECK(trace->take()->stage == ConversationStage::prefetch_incomplete);
        CHECK(!trace->take());
        std::cout << "diagnostics tests passed (" << assertions << " assertions)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
