// Opt-in, test-only loopback dialogue driver. Never built by CMake/xmake, never run by ctest or hosted CI, never
// shipped. It drives the real SynthClient and the real SaveContextStore over real HTTP against a disposable
// Synthserver that the caller has already started on loopback, so it needs no game and no WinHTTP.
//
// One run, in order: init and native-turn ACK; a context; an explicit /talk (context, then input_text with the
// conversation intent bound to that exact context ACK) whose NDJSON dialogue is parsed as it streams; the same
// terminal-delivery boundary SpeechDelivery wires (DialogueCaption::on_terminal -> speech_receipt_outcome ->
// SpeechReceiptOutbox); the resulting audio_completed receipt posted twice; a checkpoint context whose exact ACK
// advances the save anchor; then two replacement sessions restored from the older and the newer serialized
// co-save bytes, each publishing its loaded_context and making one /talk.
//
// ContextPublication and SpeechDelivery transitively need Windows headers (WinHTTP, XAudio2), so their few lines of
// ACK/receipt glue are mirrored below and marked; everything else is the shipped code. The second receipt is an
// explicit server idempotence probe: production sends each receipt once and never retries.
//
// Handshake (stdout/stdin, one line each): the driver prints "PHASE <name> <json>" and waits for "GO" so the harness
// can register the actor, script the loopback provider and inspect the database between real client operations.
// Any other reply aborts. Evidence lines start with "LOOPBACK ". Exit 0 only if every client-side check passed.
//
// Usage: loopback_dialogue_driver http://127.0.0.1:PORT   (any non-loopback address is refused)
#include "client/http.hpp"
#include "client/speech_receipts.hpp"
#include "client/synth_client.hpp"
#include "core/save_context.hpp"
#include "presentation/dialogue_presentation.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

using namespace synth;

#define CHECK(value) do { if (!(value)) throw std::runtime_error{"check failed at line " + std::to_string(__LINE__) + ": " #value}; } while (false)

namespace {

// Minimal blocking-with-deadline HTTP/1.1 client for 127.0.0.1 only, following WinHttpTransport's contract: whole
// body bounded by maximum_response_bytes, response_chunk called for 2xx bytes as they arrive, cancellation and the
// request timeout polled every 25 ms, connect refusal reported as server_unavailable.
class LoopbackHttpTransport final : public client::IHttpTransport {
public:
    explicit LoopbackHttpTransport(std::string_view base) {
        constexpr std::string_view prefix{"http://127.0.0.1:"};
        if (!base.starts_with(prefix)) throw std::invalid_argument{"loopback driver accepts only http://127.0.0.1:PORT"};
        const auto digits = base.substr(prefix.size());
        unsigned value{};
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (error != std::errc{} || end != digits.data() + digits.size() || value == 0 || value > 65535)
            throw std::invalid_argument{"invalid loopback port"};
        port_ = static_cast<std::uint16_t>(value);
    }

    client::HttpResult send(const client::HttpRequest& request, const client::Cancelled& cancelled) override {
        if (request.timeout_ms == 0 || !request.target.starts_with('/')) throw std::invalid_argument{"invalid HTTP request"};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{request.timeout_ms};
        if (cancelled && cancelled()) return {client::TransportFailure::cancelled, {}};
        Socket socket{::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
        if (socket.fd < 0) return {client::TransportFailure::server_unavailable, {}};
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port_);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(socket.fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 && errno != EINPROGRESS)
            return {client::TransportFailure::server_unavailable, {}};
        if (const auto failure = wait(socket.fd, POLLOUT, deadline, cancelled)) return {*failure, {}};
        int error{};
        socklen_t length = sizeof(error);
        if (::getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0)
            return {client::TransportFailure::server_unavailable, {}};

        const char* method = request.method == client::HttpMethod::get ? "GET"
            : request.method == client::HttpMethod::post ? "POST" : "PUT";
        std::string head = std::string{method} + " " + request.target + " HTTP/1.1\r\nHost: 127.0.0.1:" +
            std::to_string(port_) + "\r\nUser-Agent: SYNTH/1\r\nConnection: close\r\nContent-Length: " +
            std::to_string(request.body.size()) + "\r\n";
        if (!request.content_type.empty()) head += "Content-Type: " + request.content_type + "\r\n";
        if (!request.accept.empty()) head += "Accept: " + request.accept + "\r\n";
        const auto outgoing = head + "\r\n" + request.body;
        for (std::size_t sent = 0; sent < outgoing.size();) {
            if (const auto failure = wait(socket.fd, POLLOUT, deadline, cancelled)) return {*failure, {}};
            const auto count = ::send(socket.fd, outgoing.data() + sent, outgoing.size() - sent, MSG_NOSIGNAL);
            if (count < 0 && errno != EAGAIN && errno != EINTR) return {client::TransportFailure::server_unavailable, {}};
            if (count > 0) sent += static_cast<std::size_t>(count);
        }

        client::HttpResponse response;
        std::string raw;
        bool headers_done{}, chunked{}, eof{};
        std::optional<std::size_t> content_length;
        std::size_t chunk_remaining{};
        bool chunk_trailer{};
        const auto deliver = [&](std::string_view bytes) -> std::optional<client::TransportFailure> {
            if (bytes.empty()) return std::nullopt;
            if (bytes.size() > request.maximum_response_bytes - response.body.size())
                return client::TransportFailure::response_too_large;
            response.body.append(bytes);
            if (response.status >= 200 && response.status < 300 && request.response_chunk && !request.response_chunk(bytes))
                return client::TransportFailure::protocol_error;
            return std::nullopt;
        };
        std::array<char, 64U * 1024U> buffer;
        for (;;) {
            if (!eof) {
                if (const auto failure = wait(socket.fd, POLLIN, deadline, cancelled)) return {*failure, {}};
                const auto count = ::recv(socket.fd, buffer.data(), buffer.size(), 0);
                if (count < 0 && errno != EAGAIN && errno != EINTR) return {client::TransportFailure::server_unavailable, {}};
                if (count == 0) eof = true;
                if (count > 0) raw.append(buffer.data(), static_cast<std::size_t>(count));
            }
            if (!headers_done) {
                const auto end = raw.find("\r\n\r\n");
                if (end == std::string::npos) {
                    if (eof || raw.size() > 65536) return {client::TransportFailure::protocol_error, {}};
                    continue;
                }
                const auto block = raw.substr(0, end);
                raw.erase(0, end + 4);
                headers_done = true;
                if (!block.starts_with("HTTP/1.") || block.size() < 12) return {client::TransportFailure::protocol_error, {}};
                const auto [_, status_error] = std::from_chars(block.data() + 9, block.data() + 12, response.status);
                if (status_error != std::errc{}) return {client::TransportFailure::protocol_error, {}};
                for (std::size_t at = block.find("\r\n"); at != std::string::npos;) {
                    const auto next = block.find("\r\n", at + 2);
                    auto line = block.substr(at + 2, next == std::string::npos ? std::string::npos : next - at - 2);
                    at = next;
                    const auto colon = line.find(':');
                    if (colon == std::string::npos) continue;
                    std::string name = line.substr(0, colon);
                    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    auto value = line.substr(colon + 1);
                    value.erase(0, value.find_first_not_of(' '));
                    if (name == "content-type") response.content_type = value;
                    else if (name == "transfer-encoding" && value.find("chunked") != std::string::npos) chunked = true;
                    else if (name == "content-length") content_length = std::stoull(value);
                }
            }
            if (!chunked) {
                if (const auto failure = deliver(raw)) return {*failure, {}};
                raw.clear();
                if (content_length && response.body.size() >= *content_length) break;
                if (eof) {
                    if (content_length && response.body.size() != *content_length) return {client::TransportFailure::protocol_error, {}};
                    break;
                }
                continue;
            }
            bool done{};
            for (;;) {
                if (chunk_remaining > 0) {
                    const auto take = std::min(chunk_remaining, raw.size());
                    if (const auto failure = deliver(std::string_view{raw}.substr(0, take))) return {*failure, {}};
                    raw.erase(0, take);
                    chunk_remaining -= take;
                    if (chunk_remaining > 0) break;
                    chunk_trailer = true;
                }
                if (chunk_trailer) {
                    if (raw.size() < 2) break;
                    if (!raw.starts_with("\r\n")) return {client::TransportFailure::protocol_error, {}};
                    raw.erase(0, 2);
                    chunk_trailer = false;
                }
                const auto line_end = raw.find("\r\n");
                if (line_end == std::string::npos) break;
                std::size_t size{};
                const auto [__, size_error] = std::from_chars(raw.data(), raw.data() + line_end, size, 16);
                if (size_error != std::errc{}) return {client::TransportFailure::protocol_error, {}};
                raw.erase(0, line_end + 2);
                if (size == 0) { done = true; break; }
                chunk_remaining = size;
            }
            if (done) break;
            if (eof) return {client::TransportFailure::protocol_error, {}};
        }
        return {client::TransportFailure::none, std::move(response)};
    }

    void cancel_all() noexcept override {}

private:
    struct Socket final {
        int fd;
        ~Socket() { if (fd >= 0) ::close(fd); }
    };
    static std::optional<client::TransportFailure> wait(int fd, short events, std::chrono::steady_clock::time_point deadline,
                                                        const client::Cancelled& cancelled) {
        for (;;) {
            if (cancelled && cancelled()) return client::TransportFailure::cancelled;
            if (std::chrono::steady_clock::now() >= deadline) return client::TransportFailure::timeout;
            pollfd item{fd, events, 0};
            const auto ready = ::poll(&item, 1, 25);
            if (ready > 0) return std::nullopt;
            if (ready < 0 && errno != EINTR) return client::TransportFailure::server_unavailable;
        }
    }
    std::uint16_t port_{};
};

json::Value text(std::string value) { return json::Value{std::move(value)}; }

void evidence(std::string_view kind, json::Value::Object fields) {
    fields.insert(fields.begin(), {"kind", text(std::string{kind})});
    std::cout << "LOOPBACK " << json::write(json::Value{std::move(fields)}) << std::endl;
}

json::Value anchor_value(const std::optional<core::SavedContext>& value) {
    if (!value) return json::Value{nullptr};
    return json::Value{protocol_native::object({{"playthrough_id", value->playthrough_id}, {"session_id", value->session_id},
        {"generation", value->generation}, {"context_sequence", value->context_sequence}})};
}

void phase(std::string_view name, json::Value::Object fields) {
    std::cout << "PHASE " << name << ' ' << json::write(json::Value{std::move(fields)}) << std::endl;
    std::string reply;
    if (!std::getline(std::cin, reply) || reply != "GO") throw std::runtime_error{"harness aborted at phase " + std::string{name}};
}

// The co-save serialization interface read_saved_context() expects, over one serialized record.
struct CoSave final {
    core::SavedRecordBytes record;
    mutable bool read{};
    bool GetNextRecordInfo(std::uint32_t& type, std::uint32_t& version, std::uint32_t& length) const {
        if (read) return false;
        read = true;
        type = core::saved_context_record_type;
        version = core::saved_context_record_version;
        length = static_cast<std::uint32_t>(record.size());
        return true;
    }
    std::uint32_t ReadRecordData(void* out, std::uint32_t length) const {
        std::memcpy(out, record.data(), length);
        return length;
    }
};

std::optional<core::SavedContext> saved_anchor(const core::SavedRecordBytes& bytes) {
    const auto record = core::decode_saved_record(bytes);
    return record && record->anchor ? core::decode_saved_context(*record->anchor) : std::nullopt;
}

const protocol_native::Status* only_status(const client::RequestOutcome& outcome) {
    return outcome.lines.size() == 3 ? std::get_if<protocol_native::Status>(&outcome.lines[1].payload) : nullptr;
}

// Mirror of SpeechDelivery::accepted_receipt_state (speech_delivery.hpp): start, one speech_receipt_accepted status
// carrying the stored state, then a complete end.
const std::string* accepted_receipt_state(const client::RequestOutcome& outcome) {
    if (outcome.status != client::RequestStatus::complete || outcome.lines.size() != 3) return nullptr;
    const auto* status = std::get_if<protocol_native::Status>(&outcome.lines[1].payload);
    const auto* end = std::get_if<protocol_native::End>(&outcome.lines[2].payload);
    return std::holds_alternative<protocol_native::Start>(outcome.lines[0].payload) && status && end &&
        status->code == "speech_receipt_accepted" && !status->detail.empty() && end->status == "complete"
        ? &status->detail : nullptr;
}

constexpr std::string_view player_form{"0x00000014"};
constexpr std::string_view target_form{"0x0001A4D7"};

// One runtime session: the shipped client plus the store binding a writer holds.
struct Writer final {
    std::string session;
    core::RuntimeGeneration generation;
    core::SaveContextStore& store;
    core::SaveContextStore::Binding binding;
    std::string playthrough;
    client::SynthClient synth;
    core::CancellationSource owner{generation};

    Writer(client::IHttpTransport& transport, std::string id, std::uint64_t generation_value, core::SaveContextStore& save)
        : session{id}, generation{core::RuntimeGeneration::from_value(generation_value)}, store{save},
          binding{*save.bind(*save.playthrough_id(), save.lifetime())}, playthrough{*save.playthrough_id()},
          synth{transport, client::SessionOptions{.runtime_session_id = std::move(id), .runtime_variant = "flat",
              .client_version = "loopback-driver", .runtime_version = "1.10.163",
              .capabilities = {"dialogue.text", "dialogue.native_turn", "dialogue.turn_ownership", "context.saved_anchor",
                               "context.actor_identity"},
              .protocol_version = 2}} {
        const auto outcome = synth.initialize(generation.value(), false);
        const auto* status = only_status(outcome);
        evidence("init", {{"session", session}, {"generation", generation.value()}, {"status", text(status ? status->code : "")},
            {"native_turn_ready", json::Value{synth.native_turn_ready()}}});
        CHECK(outcome.status == client::RequestStatus::complete && synth.native_turn_ready());
    }

    [[nodiscard]] protocol_native::Identity actor(std::string_view form, std::string name) const {
        return {std::string{form}, "Fallout4.esm", playthrough, std::move(name)};
    }

    // Mirror of ContextPublication::publish's checkpoint step (context_publication.hpp): only an exact
    // context_accepted ACK advances this writer's saved anchor in the shared store.
    client::RequestOutcome publish() {
        const auto target = actor(target_form, "Settler");
        auto outcome = synth.publish_context(0, playthrough, actor(player_form, "Sole Survivor"), target, {target}, {},
            std::nullopt, {}, {}, {}, {}, owner.token(), binding.loaded_context);
        CHECK(synth.context_accepted(outcome));
        outcome.save_checkpoint_acknowledged = store.acknowledge(binding.epoch,
            {playthrough, synth.runtime_session_id(), outcome.context_binding.generation, outcome.context_binding.sequence});
        CHECK(outcome.save_checkpoint_acknowledged);
        evidence("context_ack", {{"session", session}, {"sequence", outcome.context_binding.sequence},
            {"request_id", outcome.request_id}, {"loaded_context", anchor_value(binding.loaded_context)}});
        return outcome;
    }

    struct Talk final {
        client::RequestOutcome context, reply;
        protocol_native::Line line;
        std::size_t streamed_callbacks{};
    };

    // DialogueRequests::submit_text's sequence: hold the context turn, publish, then send bound input_text.
    Talk talk(std::string question) {
        Talk result;
        const auto turn = synth.begin_context_turn();
        result.context = publish();
        std::vector<protocol_native::Line> streamed;
        result.reply = synth.send_text(std::move(question), [&](const protocol_native::Line& line) { streamed.push_back(line); },
            result.context.context_binding, {}, protocol_native::TextIntent::conversation);
        CHECK(result.reply.status == client::RequestStatus::complete);
        std::vector<protocol_native::Line> dialogue;
        for (const auto& line : result.reply.lines)
            if (std::holds_alternative<protocol_native::Dialogue>(line.payload)) dialogue.push_back(line);
        CHECK(dialogue.size() == 1);
        result.line = dialogue.front();
        result.streamed_callbacks = streamed.size();
        const auto& said = std::get<protocol_native::Dialogue>(result.line.payload);
        const auto* speaker = std::get_if<protocol_native::Identity>(&said.speaker);
        evidence("dialogue", {{"session", session}, {"request_id", result.reply.request_id}, {"turn_id", result.line.turn_id},
            {"line_id", result.line.line_id}, {"context_sequence", result.line.context_sequence},
            {"generation", result.line.generation}, {"native_turn", json::Value{said.native_turn}},
            {"speaker_form", text(speaker ? speaker->form_id : "")}, {"text", said.text},
            {"lines", static_cast<std::uint64_t>(result.reply.lines.size())},
            {"streamed_callbacks", static_cast<std::uint64_t>(result.streamed_callbacks)}});
        // Streaming proof: the line callback ran for the parsed lines before send_text returned.
        CHECK(result.streamed_callbacks == result.reply.lines.size() && result.streamed_callbacks >= 3);
        CHECK(result.line.request_id == result.reply.request_id && result.line.context_sequence == result.context.context_binding.sequence);
        CHECK(speaker && speaker->form_id == target_form && speaker->playthrough_id == playthrough);
        return result;
    }
};

int run(std::string_view base) {
    LoopbackHttpTransport transport{base};
    core::SaveContextStore store;
    const auto fill = [] { core::PlaythroughUuid bytes{}; bytes.fill(0x5a); return std::optional{bytes}; };
    CHECK(store.start_new_game(fill).resolution == core::IdentityResolution::new_game);

    Writer first{transport, "loopback-s1", 1, store};
    CHECK(!first.binding.loaded_context);
    const auto initial = first.publish();
    phase("context", {{"session", first.session}, {"playthrough_id", first.playthrough},
        {"target_form", std::string{target_form}}, {"sequence", initial.context_binding.sequence}});

    // Explicit /talk; its own context ACK is the latest anchor before the line is heard.
    auto heard = first.talk("Loopback question one?");
    const auto older_bytes = store.for_save();
    CHECK(older_bytes && saved_anchor(*older_bytes) && saved_anchor(*older_bytes)->context_sequence == heard.context.context_binding.sequence);

    // Delivery boundary exactly as SpeechDelivery wires it (receipt_hook): a native-turn caption's first terminal
    // delivery, here audio playback reported spoken and not cancelled, becomes one copied pending receipt.
    const auto& said = std::get<protocol_native::Dialogue>(heard.line.payload);
    const auto* speaker = std::get_if<protocol_native::Identity>(&said.speaker);
    CHECK(speaker && speaker->form_id == target_form && said.native_turn && first.synth.native_turn_ready() && heard.line.protocol_version == 2 && heard.line.context_sequence != 0);
    const auto outbox = std::make_shared<client::SpeechReceiptOutbox>();
    {
        presentation::DialogueCaption caption{.generation = heard.line.generation, .text = said.text, .cancellation = first.owner.token(),
            .request_id = heard.line.request_id, .line_id = heard.line.line_id, .context_sequence = heard.line.context_sequence};
        caption.on_terminal = [target = std::weak_ptr<client::SpeechReceiptOutbox>{outbox},
            pending = client::PendingSpeechReceipt{heard.line.generation, heard.line.context_sequence, heard.line.turn_id,
                {heard.line.request_id, heard.line.line_id, *speaker, {}}}](presentation::DialogueDelivery terminal, bool cancelled) mutable {
            const auto outcome = client::speech_receipt_outcome(terminal, cancelled);
            const auto locked = target.lock();
            if (!outcome || !locked) return;
            pending.receipt.outcome = *outcome;
            (void)locked->push(std::move(pending));
        };
        caption.mark_delivery(presentation::DialogueDelivery::playing);
        caption.mark_delivery(presentation::DialogueDelivery::spoken);
        caption.mark_delivery(presentation::DialogueDelivery::spoken); // A repeated terminal report must not push again.
    }
    auto pending = outbox->take(first.generation.value());
    CHECK(pending.size() == 1 && pending.front().receipt.outcome == protocol_native::SpeechReceiptOutcome::audio_completed);
    json::Value::Array receipts;
    for (int attempt = 1; attempt <= 2; ++attempt) {
        const auto& value = pending.front();
        const auto outcome = first.synth.send_speech_receipt(value.receipt, {value.generation, value.context_sequence, {}, value.turn_id});
        const auto* state = accepted_receipt_state(outcome);
        evidence("receipt", {{"attempt", static_cast<std::uint64_t>(attempt)}, {"request_id", outcome.request_id},
            {"dialogue_request_id", value.receipt.dialogue_request_id}, {"line_id", value.receipt.line_id},
            {"accepted_state", text(state ? *state : "")}, {"status", static_cast<std::uint64_t>(outcome.status)}});
        CHECK(state && *state == "spoken");
        receipts.emplace_back(text(outcome.request_id));
    }

    // Checkpoint: the next exact context ACK after the heard receipt becomes the newer saved anchor.
    const auto checkpoint = first.publish();
    const auto newer_bytes = store.for_save();
    CHECK(newer_bytes);
    const auto older = saved_anchor(*older_bytes);
    const auto newer = saved_anchor(*newer_bytes);
    CHECK(older && newer && newer->context_sequence == checkpoint.context_binding.sequence);
    CHECK(older->session_id == first.session && newer->session_id == first.session && older->context_sequence < newer->context_sequence);
    phase("checkpoint", {{"session", first.session}, {"dialogue_request_id", heard.line.request_id}, {"line_id", heard.line.line_id},
        {"heard_text", said.text}, {"receipt_request_ids", json::Value{std::move(receipts)}},
        {"older_anchor", anchor_value(older)}, {"newer_anchor", anchor_value(newer)}});

    // Load each co-save into a fresh store (as PreLoadGame/read/PostLoadGame do) and talk from a replacement session.
    const auto load = [&](std::string id, std::uint64_t generation, const core::SavedRecordBytes& bytes, std::string_view name) {
        core::SaveContextStore loaded;
        loaded.begin_load();
        core::read_saved_context(CoSave{bytes}, loaded);
        CHECK(loaded.finish_load(true, fill).resolution == core::IdentityResolution::restored);
        CHECK(loaded.playthrough_id() == first.playthrough);
        Writer writer{transport, std::move(id), generation, loaded};
        CHECK(writer.binding.loaded_context == saved_anchor(bytes));
        const auto reply = writer.talk("Loopback question from the " + std::string{name} + " save?");
        phase(std::string{name} + "_talk", {{"session", writer.session}, {"loaded_context", anchor_value(writer.binding.loaded_context)},
            {"request_id", reply.reply.request_id}});
    };
    load("loopback-s2", 2, *older_bytes, "older");
    load("loopback-s3", 3, *newer_bytes, "newer");
    std::cout << "DONE" << std::endl;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: loopback_dialogue_driver http://127.0.0.1:PORT\n";
        return 2;
    }
    try {
        return run(argv[1]);
    } catch (const std::exception& error) {
        std::cout << "FAILED " << error.what() << std::endl;
        return 1;
    }
}
