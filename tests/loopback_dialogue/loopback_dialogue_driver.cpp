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
// ContextPublication and SpeechDelivery transitively need Windows headers (WinHTTP, XAudio2), so the driver calls
// the engine-free pieces they delegate to: speech_receipt_hook (terminal delivery -> outbox), exchange_speech_receipt
// (receipt ACK parsing and the heard notification) and SaveCheckpoint (history frontier and exact context ACK
// revision). The driver feeds no flat dialogue history, so the frontier it observes is raised by the heard receipt
// alone. The second receipt is an explicit server idempotence probe without a heard hook: production never retries.
//
// Handshake (stdout/stdin, one line each): the driver prints "PHASE <name> <json>" and waits for "GO" so the harness
// can register the actor, script the loopback provider and inspect the database between real client operations.
// Any other reply aborts. Evidence lines start with "LOOPBACK ". Exit 0 only if every client-side check passed.
//
// The opt-in "companion" scenario (second argument) is a separate run against a fixture companion, Piper, with
// SYNTHETIC captured actor state: a context, a manual activate, an ordinary unmarked question, a marked conversation,
// an unmarked "Wait here." whose returned typed action is checked by the shipped engine-free prepare_wait_action over a
// synthetic scene, and a SIMULATED executor outcome (nothing runs in a game) sent through the real action-result API,
// then repeated, contradicted, superseded, cancelled and generation-invalidated. See README.md for its phases and limits.
//
// Usage: loopback_dialogue_driver http://127.0.0.1:PORT [dialogue|companion]   (any non-loopback address is refused)
#include "client/http.hpp"
#include "client/save_checkpoint.hpp"
#include "client/speech_receipts.hpp"
#include "client/synth_client.hpp"
#include "client/wait_action.hpp"
#include "core/save_context.hpp"
#include "presentation/dialogue_presentation.hpp"
#include "runtime/action_completion.hpp"

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
#include <functional>
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

constexpr std::string_view player_form{"0x00000014"};
constexpr std::string_view target_form{"0x0001A4D7"};

std::vector<std::string> dialogue_capabilities() {
    return {"dialogue.text", "dialogue.native_turn", "dialogue.turn_ownership", "context.saved_anchor", "context.actor_identity"};
}

// One runtime session: the shipped client plus the shipped SaveCheckpoint over the store binding a writer holds.
struct Writer final {
    std::string session;
    core::RuntimeGeneration generation;
    std::string playthrough;
    client::SynthClient synth;
    core::CancellationSource owner{generation};
    std::atomic_bool terminal{};
    client::SaveCheckpoint checkpoint;
    // What each context publishes about its target. The default scenario publishes no actor states.
    std::string target_form_id{target_form}, target_name{"Settler"};
    std::vector<protocol_native::ActorState> actor_states;
    std::string audience_observation;

    Writer(client::IHttpTransport& transport, std::string id, std::uint64_t generation_value, core::SaveContextStore& save,
           std::vector<std::string> capabilities = dialogue_capabilities())
        : session{id}, generation{core::RuntimeGeneration::from_value(generation_value)}, playthrough{*save.playthrough_id()},
          synth{transport, client::SessionOptions{.runtime_session_id = std::move(id), .runtime_variant = "flat",
              .client_version = "loopback-driver", .runtime_version = "1.10.163",
              .capabilities = std::move(capabilities),
              .protocol_version = 2}},
          checkpoint{&save, *save.bind(playthrough, save.lifetime()), owner.token(), terminal} {
        const auto outcome = synth.initialize(generation.value(), false);
        const auto* status = only_status(outcome);
        evidence("init", {{"session", session}, {"generation", generation.value()}, {"status", text(status ? status->code : "")},
            {"native_turn_ready", json::Value{synth.native_turn_ready()}}});
        CHECK(outcome.status == client::RequestStatus::complete && synth.native_turn_ready());
    }

    [[nodiscard]] protocol_native::Identity actor(std::string_view form, std::string name) const {
        return {std::string{form}, "Fallout4.esm", playthrough, std::move(name)};
    }

    // ContextPublication::publish's checkpoint order: read the frontier revision, publish, then the shared
    // SaveCheckpoint saves only an exact context_accepted ACK and covers that revision.
    client::RequestOutcome publish() {
        const auto target = actor(target_form_id, target_name);
        const auto revision = checkpoint.revision();
        auto outcome = synth.publish_context(0, playthrough, actor(player_form, "Sole Survivor"), target, {target}, actor_states,
            std::nullopt, {}, {}, {}, {}, owner.token(), checkpoint.binding().loaded_context, {}, {}, {}, audience_observation);
        outcome.save_checkpoint_acknowledged = checkpoint.acknowledge(synth, outcome, playthrough, revision, generation, owner.token());
        CHECK(outcome.save_checkpoint_acknowledged);
        evidence("context_ack", {{"session", session}, {"sequence", outcome.context_binding.sequence},
            {"request_id", outcome.request_id}, {"loaded_context", anchor_value(checkpoint.binding().loaded_context)}});
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
        CHECK(speaker && speaker->form_id == target_form_id && speaker->playthrough_id == playthrough);
        return result;
    }

    // The same hold-publish-send sequence for any intent, leaving every response check to the caller.
    struct Exchange final {
        client::RequestOutcome context, reply;
    };
    Exchange ask(std::string text, protocol_native::TextIntent intent) {
        Exchange result;
        const auto turn = synth.begin_context_turn();
        result.context = publish();
        result.reply = synth.send_text(std::move(text), {}, result.context.context_binding, {}, intent);
        return result;
    }
};

int run(std::string_view base) {
    LoopbackHttpTransport transport{base};
    core::SaveContextStore store;
    const auto fill = [] { core::PlaythroughUuid bytes{}; bytes.fill(0x5a); return std::optional{bytes}; };
    CHECK(store.start_new_game(fill).resolution == core::IdentityResolution::new_game);

    Writer first{transport, "loopback-s1", 1, store};
    CHECK(!first.checkpoint.binding().loaded_context);
    const auto initial = first.publish();
    phase("context", {{"session", first.session}, {"playthrough_id", first.playthrough},
        {"target_form", std::string{target_form}}, {"sequence", initial.context_binding.sequence}});

    // Explicit /talk; its own context ACK is the latest anchor before the line is heard.
    auto heard = first.talk("Loopback question one?");
    const auto older_bytes = store.for_save();
    CHECK(older_bytes && saved_anchor(*older_bytes) && saved_anchor(*older_bytes)->context_sequence == heard.context.context_binding.sequence);

    // Delivery boundary through SpeechDelivery's shared hook: a native-turn caption's first terminal delivery, here
    // audio playback reported spoken and not cancelled, becomes one copied pending receipt.
    const auto& said = std::get<protocol_native::Dialogue>(heard.line.payload);
    const auto outbox = std::make_shared<client::SpeechReceiptOutbox>();
    {
        presentation::DialogueCaption caption{.generation = heard.line.generation, .text = said.text, .cancellation = first.owner.token(),
            .request_id = heard.line.request_id, .line_id = heard.line.line_id, .context_sequence = heard.line.context_sequence,
            .on_terminal = client::speech_receipt_hook(outbox, heard.line, said, first.synth.native_turn_ready())};
        CHECK(caption.on_terminal);
        caption.mark_delivery(presentation::DialogueDelivery::playing);
        caption.mark_delivery(presentation::DialogueDelivery::spoken);
        caption.mark_delivery(presentation::DialogueDelivery::spoken); // A repeated terminal report must not push again.
    }
    auto pending = outbox->take(first.generation.value());
    CHECK(pending.size() == 1 && pending.front().receipt.outcome == protocol_native::SpeechReceiptOutcome::audio_completed);
    // The /talk's own ACK covered every earlier frontier revision, so nothing is due until the line is heard.
    CHECK(!first.checkpoint.due());
    json::Value::Array receipts;
    for (int attempt = 1; attempt <= 2; ++attempt) {
        const auto& value = pending.front();
        // The session's composition (PluginSession -> ContextPublication -> SaveCheckpoint) for the real receipt only.
        const auto exchange = client::exchange_speech_receipt(first.synth, value, first.owner.token(), first.terminal,
            attempt == 1 ? client::HeardReceipt{std::bind_front(&client::SaveCheckpoint::note_heard_receipt, &first.checkpoint)}
                         : client::HeardReceipt{});
        evidence("receipt", {{"attempt", static_cast<std::uint64_t>(attempt)}, {"request_id", exchange.outcome.request_id},
            {"dialogue_request_id", value.receipt.dialogue_request_id}, {"line_id", value.receipt.line_id},
            {"accepted_state", text(exchange.accepted_state.value_or(""))},
            {"status", static_cast<std::uint64_t>(exchange.outcome.status)}});
        CHECK(exchange.accepted_state == "spoken");
        // The accepted heard receipt makes a checkpoint due before anything is published.
        CHECK(exchange.heard == (attempt == 1) && first.checkpoint.due());
        receipts.emplace_back(text(exchange.outcome.request_id));
    }

    // Checkpoint: the next exact context ACK covers the heard receipt and becomes the newer saved anchor.
    const auto checkpoint = first.publish();
    CHECK(!first.checkpoint.due());
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
        CHECK(writer.checkpoint.binding().loaded_context == saved_anchor(bytes));
        const auto reply = writer.talk("Loopback question from the " + std::string{name} + " save?");
        phase(std::string{name} + "_talk", {{"session", writer.session},
            {"loaded_context", anchor_value(writer.checkpoint.binding().loaded_context)},
            {"request_id", reply.reply.request_id}});
    };
    load("loopback-s2", 2, *older_bytes, "older");
    load("loopback-s3", 3, *newer_bytes, "newer");
    std::cout << "DONE" << std::endl;
    return 0;
}

// ---- Companion scenario (opt-in) ----

// Piper's Fallout4.esm reference and base forms. Every other captured value below is SYNTHETIC test data.
constexpr std::string_view piper_form{"0x00002F1F"};
constexpr std::string_view piper_base_form{"0x00002F1E"};
constexpr std::string_view simulated_detail{"SIMULATED loopback executor: no game runtime, nothing was executed"};

std::vector<const protocol_native::Dialogue*> dialogues(const client::RequestOutcome& outcome) {
    std::vector<const protocol_native::Dialogue*> found;
    for (const auto& line : outcome.lines)
        if (const auto* said = std::get_if<protocol_native::Dialogue>(&line.payload)) found.push_back(said);
    return found;
}

std::vector<protocol_native::Action> actions(const client::RequestOutcome& outcome) {
    std::vector<protocol_native::Action> found;
    for (const auto& line : outcome.lines)
        if (const auto* action = std::get_if<protocol_native::Action>(&line.payload)) found.push_back(*action);
    return found;
}

bool same_form(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

// A refusal must be the server's exact protocol code, never a transport failure or a malformed reply.
void check_refused(const client::RequestOutcome& outcome, std::string_view code) {
    CHECK(outcome.status == client::RequestStatus::http_failure && outcome.http_status == 409);
    CHECK(outcome.detail == "HTTP 409: " + std::string{code});
}

int run_companion(std::string_view base) {
    LoopbackHttpTransport transport{base};
    core::SaveContextStore store;
    const auto fill = [] { core::PlaythroughUuid bytes{}; bytes.fill(0x6b); return std::optional{bytes}; };
    CHECK(store.start_new_game(fill).resolution == core::IdentityResolution::new_game);
    // The flat Wait capabilities come from the shipped helper; turn cancellation anchors the cancelled-work probe.
    auto capabilities = dialogue_capabilities();
    capabilities.emplace_back("dialogue.turn_cancel");
    client::append_wait_capabilities(capabilities, core::RuntimeVariant::flat);

    Writer s{transport, "loopback-companion", 1, store, capabilities};
    s.target_form_id = piper_form;
    s.target_name = "Piper";
    const auto piper = s.actor(piper_form, "Piper");
    // SYNTHETIC captured state: a live, visible, idle teammate that no SYNTH Wait package holds.
    protocol_native::ActorState captured{.actor = piper, .base_form_id = std::string{piper_base_form},
        .base_origin_plugin = "Fallout4.esm", .x = 96.0, .y = 128.0, .z = 0.0, .distance = 160.0, .alive = true, .level = 12,
        .race = "HumanRace", .sex = "female", .teammate = true, .health_percent = 100.0, .action_points_percent = 100.0,
        .line_of_sight = "visible", .life_state = "alive", .posture = "normal", .health_percent_available = true,
        .action_points_percent_available = true, .wait_held = false};
    s.actor_states = {captured};
    s.audience_observation = "complete";

    // 1. Context: the captured state reaches the server; no NPC is registered by the driver or (yet) the server.
    const auto first = s.publish();
    evidence("companion_context", {{"session", s.session}, {"synthetic_state", json::Value{true}},
        {"sequence", first.context_binding.sequence}, {"request_id", first.request_id}});
    phase("companion_context", {{"session", s.session}, {"playthrough_id", s.playthrough}, {"generation", s.generation.value()},
        {"target_form", std::string{piper_form}}, {"target_name", text("Piper")}, {"sequence", first.context_binding.sequence},
        {"context_request_id", first.request_id}, {"synthetic_state", json::Value{true}}});

    // 2. Manual activation bound to that exact context ACK, as ActivationTriggers sends it.
    const auto activation = s.synth.activate(piper, "manual", first.context_binding);
    const auto* activated = only_status(activation);
    evidence("companion_activate", {{"request_id", activation.request_id}, {"status", text(activated ? activated->code : "")}});
    CHECK(activation.status == client::RequestStatus::complete && activated && activated->code == "activate_accepted");

    // 3. Ordinary unmarked question: the legacy route, never a receipt-owned native-turn line, and no typed action.
    const auto question = s.ask("Piper, what do you make of this place?", protocol_native::TextIntent::unmarked);
    const auto asked = dialogues(question.reply);
    evidence("companion_question", {{"request_id", question.reply.request_id}, {"status", static_cast<std::uint64_t>(question.reply.status)},
        {"dialogue_lines", static_cast<std::uint64_t>(asked.size())}, {"actions", static_cast<std::uint64_t>(actions(question.reply).size())}});
    CHECK(question.reply.status == client::RequestStatus::complete && !asked.empty() && actions(question.reply).empty());
    CHECK(std::ranges::none_of(asked, [](const auto* said) { return said->native_turn; }));
    phase("companion_question", {{"session", s.session}, {"activate_request_id", activation.request_id},
        {"request_id", question.reply.request_id}, {"context_sequence", question.context.context_binding.sequence},
        {"dialogue_lines", static_cast<std::uint64_t>(asked.size())}});

    // 4. Marked conversation: exactly one native-turn line spoken by Piper.
    const auto conversation = s.talk("Piper, tell me about Diamond City.");
    phase("companion_conversation", {{"session", s.session}, {"request_id", conversation.reply.request_id},
        {"line_id", conversation.line.line_id}, {"context_sequence", conversation.context.context_binding.sequence},
        {"text", std::get<protocol_native::Dialogue>(conversation.line.payload).text}});

    // 5. Unmarked "Wait here." returns one typed wait_here action owned by Piper, checked by the shipped validator over
    // a SYNTHETIC captured scene of the same generation. Its replay guard admits the action id once.
    const auto command = s.ask("Wait here.", protocol_native::TextIntent::unmarked);
    const auto returned = actions(command.reply);
    evidence("companion_command", {{"request_id", command.reply.request_id}, {"status", static_cast<std::uint64_t>(command.reply.status)},
        {"actions", static_cast<std::uint64_t>(returned.size())}, {"dialogue_lines", static_cast<std::uint64_t>(dialogues(command.reply).size())}});
    CHECK(command.reply.status == client::RequestStatus::complete && returned.size() == 1);
    CHECK(std::ranges::none_of(dialogues(command.reply), [](const auto* said) { return said->native_turn; }));
    const auto& wait = returned.front();
    CHECK(wait.name == "wait_here" && wait.capability == "action.wait_here" && !wait.target);
    CHECK(same_form(wait.actor.form_id, piper_form) && wait.actor.origin_plugin == "Fallout4.esm" && wait.actor.playthrough_id == s.playthrough);
    core::CancellationSource turn{s.owner.token()};
    client::WaitActionLifetime lifetime{s.owner.token()};
    const auto now = core::SnapshotClock::now();
    const auto capture = [&](core::RuntimeVariant variant) {
        return core::RuntimeSnapshot{core::Game::fallout4, variant, s.generation, 1, now,
            core::WorldPose{{}, core::UnitVector3::from({1, 0, 0}), core::UnitVector3::from({0, 0, 1})}, std::nullopt, std::nullopt,
            std::nullopt, core::ActorSnapshot{0x14, "Sole Survivor", {}, "Fallout4.esm", s.playthrough},
            {core::ActorSnapshot{0x2F1F, "Piper", {96.0, 128.0, 0.0}, "Fallout4.esm", s.playthrough}}};
    };
    const auto scene = capture(core::RuntimeVariant::flat);
    const auto request = client::prepare_wait_action(scene, wait, turn.token(), lifetime.token(),
        runtime::action_start_deadline(wait.deadline_ms, 30'000));
    CHECK(request.name == runtime::RuntimeActionName::wait_here && request.actor.form_id == 0x2F1F && request.action_id == wait.action_id);
    // Pure negatives of the same validator, all local: another actor, another plugin, a VR scene, and a cancelled turn
    // while its session is still live. VR negotiates no Wait capability at all.
    const auto refused_wait = [&](const core::RuntimeSnapshot& at, const protocol_native::Action& action, const core::CancellationToken& owner) {
        try {
            (void)client::prepare_wait_action(at, action, owner, lifetime.token(), runtime::action_start_deadline(action.deadline_ms, 30'000));
        } catch (const std::invalid_argument&) { return true; }
        return false;
    };
    auto absent = wait;
    absent.actor.form_id = std::string{target_form};
    auto other_plugin = wait;
    other_plugin.actor.origin_plugin = "DLCCoast.esm";
    core::CancellationSource cancelled_turn{s.owner.token()};
    cancelled_turn.cancel();
    std::vector<std::string> vr_capabilities;
    client::append_wait_capabilities(vr_capabilities, core::RuntimeVariant::vr);
    CHECK(refused_wait(scene, absent, turn.token()) && refused_wait(scene, other_plugin, turn.token()));
    CHECK(refused_wait(capture(core::RuntimeVariant::vr), wait, turn.token()) && vr_capabilities.empty());
    CHECK(!s.owner.is_cancelled() && refused_wait(scene, wait, cancelled_turn.token()));
    evidence("companion_wait_negatives", {{"absent_actor", json::Value{true}}, {"other_plugin", json::Value{true}},
        {"vr_scene", json::Value{true}}, {"vr_capabilities", std::uint64_t{0}}, {"cancelled_turn", json::Value{true}}});
    client::WaitActionReplayGuard replays;
    CHECK(replays.claim(wait) && !replays.claim(wait));
    phase("companion_action", {{"session", s.session}, {"request_id", command.reply.request_id}, {"turn_id", command.reply.turn_id},
        {"context_sequence", command.context.context_binding.sequence}, {"action_id", wait.action_id},
        {"idempotency_key", wait.idempotency_key}, {"name", wait.name}, {"actor_form", wait.actor.form_id}});

    // 6. SIMULATED executor outcome through the real result API: the honest "unavailable", never a claimed success.
    // An identical repeat is idempotent; a contradicting outcome for the same action is refused.
    const auto binding = command.context.context_binding;
    const auto result = [&](std::string status) {
        return s.synth.send_action_result(wait.action_id, wait.idempotency_key, std::move(status), std::string{simulated_detail}, {}, binding);
    };
    json::Value::Array results;
    for (int attempt = 1; attempt <= 2; ++attempt) {
        const auto sent = result("unavailable");
        evidence("companion_result", {{"attempt", static_cast<std::uint64_t>(attempt)}, {"simulated", json::Value{true}},
            {"request_id", sent.request_id}, {"status", static_cast<std::uint64_t>(sent.status)},
            {"lines", static_cast<std::uint64_t>(sent.lines.size())}});
        CHECK(sent.status == client::RequestStatus::complete);
        results.emplace_back(text(sent.request_id));
    }
    const auto conflict = result("failed");
    evidence("companion_conflict", {{"request_id", conflict.request_id}, {"detail", conflict.detail}});
    check_refused(conflict, "action_result_conflict");
    // Capability negative, refused locally before any request: no action inventory support was acknowledged.
    bool unnegotiated{};
    try {
        (void)s.synth.send_action_result(wait.action_id, wait.idempotency_key, "unavailable", std::string{simulated_detail}, {},
            binding, protocol_native::ActionInventoryObservation{.actor = piper});
    } catch (const std::invalid_argument&) { unnegotiated = true; }
    CHECK(unnegotiated);
    phase("companion_result", {{"session", s.session}, {"action_id", wait.action_id}, {"status", text("unavailable")},
        {"simulated", json::Value{true}}, {"result_request_ids", json::Value{std::move(results)}},
        {"conflict_request_id", conflict.request_id}});

    // 7. A newer conversation supersedes the command's turn, so a late result for it is stale.
    const auto newer = s.talk("Piper, are you still with me?");
    const auto stale = result("unavailable");
    evidence("companion_stale", {{"request_id", stale.request_id}, {"detail", stale.detail}});
    check_refused(stale, "superseded_turn");

    // 8. Cancelled work: the server cancels a turn anchored by its context, then refuses a reply bound to it. The local
    // token is deliberately left live to probe the server; production cancels it and never sends.
    const auto anchor = s.publish();
    const auto cancel = s.synth.cancel(anchor.request_id, s.generation.value());
    const auto* cancelled = only_status(cancel);
    CHECK(cancel.status == client::RequestStatus::complete && cancelled && cancelled->code == "cancel_accepted");
    const auto refused = s.synth.send_text("Piper, never mind.", {}, anchor.context_binding, {}, protocol_native::TextIntent::conversation);
    evidence("companion_cancel", {{"cancel_request_id", cancel.request_id}, {"anchor_request_id", anchor.request_id},
        {"refused_request_id", refused.request_id}, {"detail", refused.detail}});
    check_refused(refused, "turn_cancelled");

    // 9. Local cancellation, as on load/new game/main menu, with two shipped engine-free boundaries. send_action_result
    // has no owner argument: the owner travels in the ContextBinding, because publish() acknowledged the command's
    // context under owner.token(). SynthClient::send_event checks it before the transport, so nothing is sent. The
    // action's turn is a child of the owner, so prepare_wait_action refuses it too. The driver cancels the owner
    // directly. It doesn't prove that the Windows-only coordinator does.
    s.owner.cancel();
    CHECK(refused_wait(scene, wait, turn.token()));
    const auto retired = result("unavailable");
    evidence("companion_local_cancel", {{"request_id", retired.request_id}, {"detail", retired.detail}});
    CHECK(retired.status == client::RequestStatus::cancelled && retired.http_status == 0 && retired.detail == "request cancelled before send");
    // 10. Generation invalidation: after re-initialization, a binding from generation 1 is refused before encoding. It
    // carries a live generation 1 owner, so this refusal comes from the generation check alone, not from step 9.
    const auto reinit = s.synth.initialize(2, false);
    CHECK(reinit.status == client::RequestStatus::complete);
    const core::CancellationSource live_old_owner{s.generation};
    auto old_binding = binding;
    old_binding.cancellation = live_old_owner.token();
    std::string stale_generation;
    try {
        (void)s.synth.send_action_result(wait.action_id, wait.idempotency_key, "unavailable", std::string{simulated_detail}, {}, old_binding);
    } catch (const std::invalid_argument& error) { stale_generation = error.what(); }
    evidence("companion_generation", {{"generation", std::uint64_t{2}}, {"refusal", stale_generation}});
    CHECK(stale_generation == "turn owner belongs to another runtime generation");
    phase("companion_done", {{"session", s.session}, {"superseding_request_id", newer.reply.request_id},
        {"stale_request_id", stale.request_id}, {"cancel_request_id", cancel.request_id},
        {"cancelled_context_request_id", anchor.request_id}, {"refused_request_id", refused.request_id},
        {"reinit_request_id", reinit.request_id}, {"generation", std::uint64_t{2}}});
    std::cout << "DONE" << std::endl;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view scenario = argc == 3 ? argv[2] : "dialogue";
    if ((argc != 2 && argc != 3) || (scenario != "dialogue" && scenario != "companion")) {
        std::cerr << "usage: loopback_dialogue_driver http://127.0.0.1:PORT [dialogue|companion]\n";
        return 2;
    }
    try {
        return scenario == "companion" ? run_companion(argv[1]) : run(argv[1]);
    } catch (const std::exception& error) {
        std::cout << "FAILED " << error.what() << std::endl;
        return 1;
    }
}
