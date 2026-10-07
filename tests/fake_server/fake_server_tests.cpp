#include "client/fake_http.hpp"
#include "client/synth_client.hpp"
#include "client/rechat_prefetch.hpp"

#include <iostream>
#include <array>
#include <future>
#include <chrono>
#include <stdexcept>
#include <string>

namespace {
int checks{};
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error{std::string{"check failed: "} + #x}; } while (false)

// Echoes correlation fields so native v2 tests can corrupt a late line without hard-coded request counters.
class BindingEchoTransport final : public synth::client::IHttpTransport {
public:
    enum class Fault { none, sequence, version, truncated, timeout, failed, duplicate_id, http_error, request, turn, generation, variant };
    Fault fault{};
    std::function<void()> before_reply;
    std::size_t chunk_size{};
    bool response_returned{};
    std::string diary_ack{"diary_queued"};
    bool quest_support{};
    bool quest_tracking_support{};
    bool quest_reaction_support{};
    bool player_support{};
    bool player_reaction_support{};
    bool actor_support{};
    bool inventory_support{};
    bool action_inventory_support{};
    bool consumption_support{};
    bool transfer_support{};
    bool caps_support{};
    bool player_caps_support{};
    bool rechat_scene_support{};
    std::optional<synth::protocol_native::Identity> dialogue_listener;
    std::string player_ack{"context_accepted"};
    std::vector<synth::client::HttpRequest> requests;
    synth::client::HttpResult send(const synth::client::HttpRequest& request,
                                   const synth::client::Cancelled&) override {
        using namespace synth;
        requests.push_back(request);
        if (fault == Fault::timeout) return {client::TransportFailure::timeout, {}};
        if (fault == Fault::http_error) return {client::TransportFailure::none, {409, "application/json",
            R"({"schema":"synth.error.response.v1","code":"unknown_context_snapshot","error":"Do not reflect this text"})"}};
        const auto event = json::parse(request.body);
        if (request.target == "/itt.php") {
            if (before_reply) before_reply();
            return {client::TransportFailure::none, {200, "application/json", json::write(json::Value{protocol_native::object({
                {"schema", "synth.visual_context.response.v1"},
                {"request_id", protocol_native::text(event, "request_id")},
                {"turn_id", protocol_native::text(event, "turn_id")},
                {"generation", protocol_native::integer(event, "generation")},
                {"capture_id", fault == Fault::sequence ? "wrong:capture" : protocol_native::text(event, "capture_id")},
                {"ok", true}, {"status", "success"}})})}};
        }
        const auto line = [&](std::string type, json::Value payload, bool last = false) {
            const std::uint64_t version = last && fault == Fault::version ? 1 : protocol_native::integer(event,"protocol_version");
            auto fields = protocol_native::object({
                {"schema", "synth.response.line.v" + std::to_string(version)}, {"protocol_version", version},
                {"type", type}, {"line_id", fault == Fault::duplicate_id && last ? "response_start" : type},
                {"request_id", last && fault == Fault::request ? "wrong:request" : protocol_native::text(event, "request_id")},
                {"turn_id", last && fault == Fault::turn ? "wrong:turn" : protocol_native::text(event, "turn_id")},
                {"generation", protocol_native::integer(event, "generation") + std::uint64_t{last && fault == Fault::generation ? 1U : 0U}},
                {"game", "fo4"}, {"runtime_variant", last && fault == Fault::variant ? "vr" : protocol_native::text(event, "runtime_variant")},
                {"payload", std::move(payload)}});
            if (version == 2) fields.emplace_back("context_sequence",
                protocol_native::integer(event, "context_sequence") + std::uint64_t{last && fault == Fault::sequence ? 1U : 0U});
            return json::write(json::Value{std::move(fields)}) + "\n";
        };
        std::string body = line("response_start", json::Value{protocol_native::object({{"server_version", "test"}})});
        if ((rechat_scene_support || player_caps_support || caps_support || transfer_support || consumption_support || action_inventory_support || inventory_support || actor_support || quest_support || quest_tracking_support || quest_reaction_support || player_support || player_reaction_support) && protocol_native::text(event, "type") == "init") {
            const auto code = player_caps_support ? "init_player_caps_inventory_accepted" : caps_support ? "init_caps_inventory_accepted" : transfer_support ? "init_transfer_inventory_accepted" : consumption_support ? "init_consume_inventory_accepted" : action_inventory_support ? "init_action_inventory_accepted" : inventory_support ? "init_inventory_512_accepted" : actor_support ? "init_actor_events_accepted" : player_reaction_support ? "init_player_reactions_accepted" : player_support ? "init_player_events_accepted" : quest_reaction_support ? "init_quest_reactions_accepted" : quest_support ? (quest_tracking_support ? "init_quest_events_tracking_accepted" : "init_quest_events_accepted")
                                            : quest_tracking_support ? "init_quest_tracking_accepted" : "init_accepted";
            auto payload=protocol_native::object({{"code", code}});
            if (rechat_scene_support) payload.emplace_back("rechat_scene",true);
            body += line("status", json::Value{std::move(payload)});
        }
        if (protocol_native::text(event, "type") == "input_text") {
            auto payload=protocol_native::object({
                {"speaker", protocol_native::identity_value({"0x0001A4D7", "Fallout4.esm", "save:one", "Preston"})},
                {"text", "Bound response"}});
            if (dialogue_listener) payload.emplace_back("listener",protocol_native::identity_value(*dialogue_listener));
            body += line("dialogue",json::Value{std::move(payload)});
        }
        if (protocol_native::text(event, "type") == "diary_request" && !diary_ack.empty())
            body += line("status", json::Value{protocol_native::object({{"code", diary_ack}})});
        if ((actor_support || player_support) && protocol_native::text(event, "type") == "context" && !player_ack.empty())
            body += line("status", json::Value{protocol_native::object({{"code", player_ack}})});
        body += line("response_end", json::Value{protocol_native::object({
            {"status", fault == Fault::failed ? "error" : "complete"}})}, true);
        if (fault == Fault::truncated) body.pop_back();
        if (before_reply) before_reply();
        response_returned = false;
        if (chunk_size && request.response_chunk) {
            for (std::size_t offset = 0; offset < body.size(); offset += chunk_size) {
                if (!request.response_chunk(std::string_view{body}.substr(offset, chunk_size)))
                    return {client::TransportFailure::protocol_error, {}};
            }
        }
        response_returned = true;
        return {client::TransportFailure::none, {200, "application/x-ndjson", std::move(body)}};
    }
    void cancel_all() noexcept override {}
};

// Exercise speech waits without a provider; even an uncooperative late reply must be discarded.
class SpeechProbeTransport final : public synth::client::IHttpTransport {
public:
    BindingEchoTransport echo;
    std::vector<synth::client::HttpRequest> requests;
    std::function<void(const synth::client::Cancelled&)> during_send;
    synth::client::HttpResult send(const synth::client::HttpRequest& request,
                                  const synth::client::Cancelled& cancelled) override {
        if (request.method != synth::client::HttpMethod::get) return echo.send(request, cancelled);
        requests.push_back(request);
        if (during_send) during_send(cancelled);
        if (request.target.starts_with("/media.php/"))
            return {synth::client::TransportFailure::none, {200, "audio/wav", "audio"}};
        return {synth::client::TransportFailure::none, {200, "application/json",
            R"({"schema":"synth.tts_status.v1","cache_key":"0123456789abcdef0123456789abcdef","status":"pending"})"}};
    }
    void cancel_all() noexcept override {}
};

// Hold a dialogue RPC open while a separate control RPC must complete.
class BlockingTurnTransport final : public synth::client::IHttpTransport {
    std::atomic_bool block_next_{true};
public:
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> gate{release.get_future()};
    BindingEchoTransport echo;
    std::mutex mutex;
    synth::client::HttpResult send(const synth::client::HttpRequest& request,
                                  const synth::client::Cancelled& cancelled) override {
        const auto type = request.target == "/itt.php" ? "visual_capture"
            : synth::protocol_native::text(synth::json::parse(request.body), "type");
        if ((type == "input_text" || type == "visual_capture") && block_next_.exchange(false)) { entered.set_value(); gate.wait(); }
        std::scoped_lock lock{mutex};
        return echo.send(request, cancelled);
    }
    void cancel_all() noexcept override {}
};
}

int main() {
    {
        using namespace synth;
        const auto now = client::RechatPrefetch::Clock::now();
        core::CancellationSource turn{core::RuntimeGeneration::initial()};
        auto parent = std::make_shared<presentation::DialogueCaption>();
        parent->cancellation = turn.token();
        parent->mark_delivery(presentation::DialogueDelivery::playing);
        client::RechatPrefetch buffer{parent, now + std::chrono::minutes{1}};
        protocol_native::Line line{};
        buffer.append(line);
        CHECK(!buffer.take(now));
        buffer.finish(true);
        CHECK(!buffer.take(now));
        parent->mark_delivery(presentation::DialogueDelivery::spoken);
        auto delivered = buffer.take(now);
        CHECK(delivered && delivered->size() == 1);
        CHECK(!buffer.take(now));
        for (const auto failure : {0, 1, 2, 3}) {
            client::RechatPrefetch rejected{parent, now + std::chrono::minutes{1}};
            rejected.append(line);
            if (failure == 0) rejected.finish(false);
            if (failure == 1) parent->mark_delivery(presentation::DialogueDelivery::discarded);
            if (failure == 2) CHECK(rejected.invalid(now + std::chrono::minutes{2}));
            if (failure == 3) for (int i = 0; i < 128; ++i) rejected.append(line);
            CHECK(!rejected.take(now));
            CHECK(rejected.token().is_cancelled());
            parent->mark_delivery(presentation::DialogueDelivery::spoken);
        }
        client::RechatPrefetch interrupted{parent, now + std::chrono::minutes{1}};
        interrupted.append(line);
        interrupted.finish(true);
        turn.cancel();
        CHECK(!interrupted.take(now));
        CHECK(interrupted.token().is_cancelled());
        core::CancellationSource next_turn{core::RuntimeGeneration::initial()};
        parent->cancellation = next_turn.token();
        client::RechatPrefetch scene_rejected{parent, now + std::chrono::minutes{1}};
        scene_rejected.append(line);
        scene_rejected.finish(true);
        CHECK(scene_rejected.take(now).has_value());
        scene_rejected.cancel(); // Release capture rejects scene/actor ownership after the buffer is drained.
        CHECK(scene_rejected.token().is_cancelled() && !next_turn.is_cancelled());

        // The first child's media can finish during parent playback but cannot be consumed before release.
        parent->mark_delivery(presentation::DialogueDelivery::playing);
        protocol_native::Line voiced{};
        voiced.line_id="child:dialogue:1";
        protocol_native::Dialogue dialogue{};
        dialogue.speech.emplace();
        dialogue.speech->status="pending";
        voiced.payload=dialogue;
        auto bytes=std::make_shared<const std::vector<std::byte>>(44,std::byte{0});
        client::RechatPrefetch warmed{parent,now+std::chrono::minutes{1}};
        warmed.append(voiced);
        CHECK(warmed.claim_media(voiced));
        CHECK(!warmed.claim_media(voiced));
        warmed.finish_media(bytes);
        CHECK(!warmed.prepared_media(voiced.line_id));
        CHECK(!warmed.release(now));
        warmed.finish(true);
        CHECK(!warmed.take(now));
        parent->mark_delivery(presentation::DialogueDelivery::spoken);
        CHECK(warmed.take(now).has_value());
        CHECK(!warmed.prepared_media(voiced.line_id)); // Taken is not yet scene-admitted delivery.
        CHECK(warmed.release(now));
        CHECK(!warmed.release(now));
        const auto reused=warmed.prepared_media(voiced.line_id);
        CHECK(reused && *reused==bytes); // Same bytes; no second fetch or preparation.
        CHECK(!warmed.prepared_media("child:dialogue:2"));
        CHECK(!warmed.invalid(now+std::chrono::minutes{2})); // Delivery must not inherit the speculation timeout.
        warmed.cancel();
        CHECK(!warmed.prepared_media(voiced.line_id));

        for (const bool oversized : {false,true}) {
            client::RechatPrefetch failed_media{parent,now+std::chrono::minutes{1}};
            failed_media.append(voiced);
            CHECK(failed_media.claim_media(voiced));
            failed_media.finish_media(oversized
                ? std::make_shared<const std::vector<std::byte>>(client::RechatPrefetch::media_capacity+1)
                : nullptr);
            failed_media.finish(true);
            CHECK(failed_media.take(now).has_value());
            CHECK(failed_media.release(now));
            const auto fallback=failed_media.prepared_media(voiced.line_id);
            CHECK(fallback && !*fallback); // Failure is retained, not retried as another provider request.
        }
        client::RechatPrefetch late_media{parent,now+std::chrono::minutes{1}};
        late_media.append(voiced);
        CHECK(late_media.claim_media(voiced));
        late_media.finish(false);
        late_media.finish_media(bytes);
        CHECK(!late_media.take(now));
        CHECK(!late_media.prepared_media(voiced.line_id));
        client::RechatPrefetch late_release{parent,now+std::chrono::minutes{1}};
        late_release.append(voiced);
        late_release.finish(true);
        CHECK(late_release.take(now).has_value());
        CHECK(!late_release.release(now+std::chrono::minutes{2}));
        CHECK(late_release.token().is_cancelled());
        client::RechatPrefetch delivered_then_cancelled{parent,now+std::chrono::minutes{1}};
        delivered_then_cancelled.append(voiced);
        delivered_then_cancelled.finish(true);
        CHECK(delivered_then_cancelled.take(now).has_value());
        CHECK(delivered_then_cancelled.release(now));
        next_turn.cancel();
        CHECK(delivered_then_cancelled.invalid(now));
    }
    try {
        using namespace synth::client;
        {
            using Clock = std::chrono::steady_clock;
            using namespace std::chrono_literals;
            SpeechProbeTransport wire;
            SynthClient speech_client{wire, SessionOptions{
                .runtime_session_id = "session:speech-budget", .runtime_variant = "flat",
                .client_version = "test", .runtime_version = "1.11.240", .protocol_version = 2}};
            CHECK(speech_client.initialize(1, false).status == RequestStatus::complete);
            const std::string cache_key = "0123456789abcdef0123456789abcdef";
            const std::string media_id = "media/" + std::string(64, 'a');
            const auto expired = Clock::now() - 1ms;
            CHECK(speech_client.fetch_media(media_id, {}, expired).failure == TransportFailure::timeout);
            CHECK(speech_client.fetch_tts_status(cache_key, {}, expired).status == RequestStatus::transport_failure);
            CHECK(wire.requests.empty());

            // Audio status/media finish while dialogue still owns the serial publication lane.
            for (const bool media : {false, true}) {
                for (const bool cancel : {false, true}) {
                    auto owner = speech_client.begin_context_turn();
                    std::atomic_bool cancelled{cancel};
                    auto pending = std::async(std::launch::async, [&] {
                        const auto deadline = Clock::now() + (cancel ? 2s : 40ms);
                        const Cancelled stopped = [&] { return cancelled.load(); };
                        if (media) return speech_client.fetch_media(media_id, stopped, deadline).failure ==
                            (cancel ? TransportFailure::cancelled : TransportFailure::none);
                        return speech_client.fetch_tts_status(cache_key, stopped, deadline).status ==
                            (cancel ? RequestStatus::cancelled : RequestStatus::complete);
                    });
                    const auto finished = pending.wait_for(500ms);
                    owner.unlock(); // Always release before assertions/future destruction, even on regression.
                    CHECK(finished == std::future_status::ready);
                    CHECK(pending.get());
                }
            }
            CHECK(wire.requests.size() == 2);
            for (const bool media : {false, true}) {
                for (const bool cooperative : {false, true}) {
                    const auto deadline = Clock::now() + 40ms;
                    bool observed_stop{};
                    wire.during_send = [&](const Cancelled& stopped) {
                        if (cooperative) {
                            while (!stopped() && Clock::now() < deadline + 500ms) std::this_thread::sleep_for(1ms);
                            observed_stop = stopped();
                        } else std::this_thread::sleep_until(deadline + 5ms);
                    };
                    if (media) CHECK(speech_client.fetch_media(media_id, {}, deadline).failure == TransportFailure::timeout);
                    else CHECK(speech_client.fetch_tts_status(cache_key, {}, deadline).status == RequestStatus::transport_failure);
                    CHECK(!cooperative || observed_stop);
                    CHECK(wire.requests.back().timeout_ms > 0 && wire.requests.back().timeout_ms <= 40);
                }
            }
            wire.during_send = {};
            CHECK(speech_client.fetch_media(media_id, {}, Clock::now() + 1s).delivered());
            CHECK(speech_client.fetch_tts_status(cache_key, {}, Clock::now() + 1s).status == RequestStatus::complete);
            CHECK(speech_client.fetch_media(media_id, [] { return true; }).failure == TransportFailure::cancelled);
            CHECK(speech_client.fetch_tts_status(cache_key, [] { return true; }).status == RequestStatus::cancelled);
        }
        FakeHttpServer server;
        FakeHttpTransport transport{server};
        const HttpRequest get{HttpMethod::get, "/health", {}, {}, 100};
        server.enqueue({FakeScenarioKind::respond, {204, "application/json", ""}, 0});
        auto result = transport.send(get, [] { return false; });
        CHECK(result.delivered());
        CHECK(result.response.status == 204);
        CHECK(server.requests().front().method == HttpMethod::get);

        server.enqueue({FakeScenarioKind::timeout});
        CHECK(transport.send(get, {}).failure == TransportFailure::timeout);
        CHECK(transport.send(get, [] { return true; }).failure == TransportFailure::cancelled);
        server.enqueue({FakeScenarioKind::server_loss});
        CHECK(transport.send(get, {}).failure == TransportFailure::server_unavailable);
        CHECK(!server.running());
        CHECK(transport.send(get, {}).failure == TransportFailure::server_unavailable);
        server.restart();
        CHECK(server.restart_count() == 1);
        server.enqueue({FakeScenarioKind::malformed});
        CHECK(transport.send(get, {}).response.body == "{\"truncated\":");
        server.enqueue({FakeScenarioKind::oversized, {}, 4097});
        CHECK(transport.send(get, {}).response.body.size() == 4097);
        server.stop();
        CHECK(transport.send(get, {}).failure == TransportFailure::server_unavailable);

        bool rejected = false;
        try { (void)transport.send({HttpMethod::get, "relative", {}, {}, 10}, {}); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);

        FakeHttpServer protocol_server;
        FakeHttpTransport protocol_transport{protocol_server};
        const FakeScenario init_response{FakeScenarioKind::respond, {200, "application/x-ndjson",
            "{\"schema\":\"synth.response.line.v1\",\"protocol_version\":1,\"type\":\"response_start\",\"line_id\":\"init:1:1:start\",\"request_id\":\"init:1:1\",\"turn_id\":\"turn:init:1\",\"generation\":1,\"game\":\"fo4\",\"runtime_variant\":\"flat\",\"payload\":{\"server_version\":\"source\"}}\n"
            "{\"schema\":\"synth.response.line.v1\",\"protocol_version\":1,\"type\":\"status\",\"line_id\":\"init:1:1:status\",\"request_id\":\"init:1:1\",\"turn_id\":\"turn:init:1\",\"generation\":1,\"game\":\"fo4\",\"runtime_variant\":\"flat\",\"payload\":{\"code\":\"init_accepted\"}}\n"
            "{\"schema\":\"synth.response.line.v1\",\"protocol_version\":1,\"type\":\"response_end\",\"line_id\":\"init:1:1:end\",\"request_id\":\"init:1:1\",\"turn_id\":\"turn:init:1\",\"generation\":1,\"game\":\"fo4\",\"runtime_variant\":\"flat\",\"payload\":{\"status\":\"complete\"}}\n"}, 0};
        protocol_server.enqueue(init_response);
        SynthClient client{protocol_transport, SessionOptions{
            .runtime_session_id = "session:flat:test",
            .runtime_variant = "flat",
            .client_version = "0.1.0",
            .runtime_version = "1.11.240",
            .capabilities = {"dialogue.text"},
        }};
        const auto initialized = client.initialize(1, false);
        CHECK(initialized.status == RequestStatus::complete);
        CHECK(client.ready());
        CHECK(protocol_server.requests().size() == 1);
        CHECK(protocol_server.requests().front().body.find("\"type\":\"init\"") != std::string::npos);
        // The shipped SynthClient must not treat a server error, cancellation,
        // or broken stream as a successful connection just because HTTP is 200.
        for (const auto status : {"error", "cancelled", "missing_start", "after_end", "timeout"}) {
            auto reply = init_response;
            const auto first_line = reply.response.body.find('\n') + 1;
            if (std::string_view{status} == "missing_start") {
                reply.response.body.erase(0, first_line);
            } else if (std::string_view{status} == "after_end") {
                reply.response.body += reply.response.body.substr(0, first_line);
            } else if (std::string_view{status} == "timeout") {
                reply.kind = FakeScenarioKind::timeout;
            } else {
                reply.response.body.replace(reply.response.body.find("complete"), 8, status);
            }
            protocol_server.enqueue(std::move(reply));
            SynthClient retry{protocol_transport, SessionOptions{
                .runtime_session_id = "session:flat:retry",
                .runtime_variant = "flat", .client_version = "0.1.0", .runtime_version = "1.11.240"}};
            const auto failed = retry.initialize(1, false);
            const auto expected = std::string_view{status} == "error" ? RequestStatus::server_failure
                : std::string_view{status} == "cancelled" ? RequestStatus::cancelled
                : std::string_view{status} == "timeout" ? RequestStatus::transport_failure
                                                      : RequestStatus::malformed_response;
            CHECK(failed.status == expected);
            CHECK(!failed.detail.empty());
            CHECK(!retry.ready());
        }
        protocol_server.enqueue(init_response);
        SynthClient recovered{protocol_transport, SessionOptions{
            .runtime_session_id = "session:flat:recovered",
            .runtime_variant = "flat", .client_version = "0.1.0", .runtime_version = "1.11.240"}};
        CHECK(recovered.initialize(1, false).status == RequestStatus::complete);
        CHECK(recovered.ready());

        // A v1 context turn must retain request ownership across multiple HTTP
        // methods, while a competing worker waits outside that ownership scope.
        protocol_server.enqueue({FakeScenarioKind::timeout, {}, 0});
        protocol_server.enqueue({FakeScenarioKind::timeout, {}, 0});
        const auto before_turn = protocol_server.requests().size();
        std::promise<void> attempting;
        auto attempted = attempting.get_future();
        std::future<RequestOutcome> competitor;
        bool blocked{};
        RequestOutcome owned;
        {
            const auto turn = recovered.begin_context_turn();
            competitor = std::async(std::launch::async, [&] {
                attempting.set_value();
                return recovered.send_text("competing context owner");
            });
            attempted.wait();
            blocked = competitor.wait_for(std::chrono::milliseconds{50}) == std::future_status::timeout;
            owned = recovered.send_text("original context owner");
        }
        const auto competing = competitor.get();
        CHECK(blocked);
        CHECK(owned.status == RequestStatus::transport_failure);
        CHECK(competing.status == RequestStatus::transport_failure);
        CHECK(protocol_server.requests().size() == before_turn + 2);
        CHECK(protocol_server.requests()[before_turn].body.find("original context owner") != std::string::npos);
        CHECK(protocol_server.requests()[before_turn + 1].body.find("competing context owner") != std::string::npos);
        BindingEchoTransport bound_transport;
        SynthClient bound_client{bound_transport, SessionOptions{
            .runtime_session_id = "session:v2", .runtime_variant = "flat", .client_version = "test",
            .runtime_version = "1.11.240", .capabilities = {"diary.manual.npc", "diary.manual.narrator", "diary.manual.player"}, .protocol_version = 2}};
        CHECK(bound_client.initialize(1, false).status == RequestStatus::complete);
        const synth::protocol_native::Identity player{"0x00000014", "Fallout4.esm", "save:one", "Player"};
        const synth::protocol_native::Identity actor{"0x0001A4D7", "Fallout4.esm", "save:one", "Preston"};
        for (const std::string lane : {"flat","vr"}) {
            for (int mode=0;mode<14;++mode) {
                BindingEchoTransport wire;
                wire.dialogue_listener=player;
                auto caps=std::vector<std::string>{"dialogue.listener_identity","dialogue.turn_ownership"};
                if(mode==1) caps.erase(caps.begin());
                if(mode==2) caps.pop_back();
                if(mode==3) wire.dialogue_listener->playthrough_id="Save:one";
                if(mode==4) wire.dialogue_listener=actor;
                if(mode==5) wire.dialogue_listener->form_id="0x00000xyz";
                if(mode==6) wire.dialogue_listener->form_id="0x00000000";
                if(mode==7) wire.dialogue_listener->origin_plugin="../Fallout4.esm";
                if(mode==8) wire.dialogue_listener->origin_plugin="";
                if(mode==9) wire.dialogue_listener->origin_plugin="Fallout4.txt";
                if(mode==10) wire.dialogue_listener->origin_plugin=std::string{"Fallout\0.esm",11};
                if(mode==11) wire.dialogue_listener->playthrough_id="save one";
                if(mode==12) wire.dialogue_listener->display_name=std::string(256,'x');
                if(mode==13) { wire.dialogue_listener=actor; wire.dialogue_listener->form_id="0x0001a4d7"; wire.dialogue_listener->origin_plugin="FALLOUT4.ESM"; }
                SynthClient listener_client{wire,SessionOptions{
                    .runtime_session_id="session:listener",.runtime_variant=lane,.client_version="test",
                    .runtime_version=lane=="vr"?"1.2.72":"1.11.240",.capabilities=caps,.protocol_version=2}};
                CHECK(listener_client.initialize(1,false).status==RequestStatus::complete);
                const auto scene=listener_client.publish_context(0,"save:one",player,actor,{}, {},std::nullopt,{}, {},{});
                CHECK(scene.status==RequestStatus::complete);
                int callbacks{};
                const auto response=listener_client.send_text("Facing transport",[&](const auto& value) {
                    if(const auto* dialogue=std::get_if<synth::protocol_native::Dialogue>(&value.payload)) {
                        CHECK(dialogue->listener && dialogue->listener->form_id==player.form_id);
                    }
                    ++callbacks;
                },scene.context_binding);
                const auto expected=mode==0?RequestStatus::complete:RequestStatus::malformed_response;
                if(response.status!=expected) throw std::runtime_error{"listener case "+std::to_string(mode)+": "+response.detail};
                CHECK(response.status==expected);
                CHECK(callbacks==(mode==0?3:0));
            }
        }
        const auto publish = [&] {
            return bound_client.publish_context(0, "save:one", player, actor, {}, {}, std::nullopt, {}, {}, {});
        };
        const auto first = publish();
        CHECK(first.status == RequestStatus::complete);
        CHECK(first.context_binding.sequence == 1);
        CHECK(first.context_binding.turn_id == first.turn_id);
        for(const std::string kind:{"external_tts","external_comment","external_reaction"}) {
            const auto text=kind=="external_comment" ? std::nullopt : std::optional{std::string{"Keep  this exact text."}};
            CHECK(bound_client.trigger(kind,actor,{},std::nullopt,text,first.context_binding).status==RequestStatus::complete);
            CHECK(bound_transport.requests.back().target==(kind=="external_tts" ? "/processor/npc_tts_play.php" : "/main.php"));
            const auto event=synth::json::parse(bound_transport.requests.back().body);
            const auto& payload=synth::protocol_native::required(event,"payload");
            CHECK(synth::protocol_native::text(payload,"kind")==kind);
            CHECK(synth::protocol_native::text(synth::protocol_native::required(payload,"actor"),"form_id")==actor.form_id);
            CHECK(synth::protocol_native::integer(event,"context_sequence")==first.context_binding.sequence);
            if(text) CHECK(synth::protocol_native::text(payload,kind=="external_tts" ? "text" : "instruction")==*text);
        }
        const std::string image_hash(64, 'a');
        const synth::protocol_native::Media image_media{"media/" + image_hash, "image/png", image_hash, 100};
        const auto captured = bound_client.send_visual_capture("describe", "first_person", image_media,
            std::nullopt, actor, {}, {}, first.context_binding);
        CHECK(captured.status == RequestStatus::complete);
        const auto capture_wire = synth::json::parse(bound_transport.requests.back().body);
        CHECK(synth::protocol_native::text(capture_wire, "schema") == "synth.visual_context.capture.v2");
        CHECK(synth::protocol_native::integer(capture_wire, "context_sequence") == first.context_binding.sequence);
        CHECK(captured.turn_id == first.turn_id);
        CHECK(bound_client.trigger("external_reaction", actor, {}, std::nullopt,
            std::string{"Describe the bound capture."}, first.context_binding, captured.capture_id).status == RequestStatus::complete);
        const auto visual_reaction = synth::json::parse(bound_transport.requests.back().body);
        CHECK(synth::protocol_native::text(synth::protocol_native::required(visual_reaction, "payload"), "capture_id") == captured.capture_id);
        const auto visual_checkpoint = publish();
        CHECK(visual_checkpoint.status == RequestStatus::complete);
        CHECK(visual_checkpoint.context_binding.sequence == first.context_binding.sequence + 1);
        CHECK(visual_checkpoint.context_binding.turn_id != captured.turn_id);
        bound_transport.fault = BindingEchoTransport::Fault::sequence;
        CHECK(bound_client.send_visual_capture("describe", "first_person", image_media,
            std::nullopt, actor, {}, {}, first.context_binding).status == RequestStatus::stale_response);
        const auto before_unbound_capture = bound_transport.requests.size();
        rejected = false;
        try { (void)bound_client.send_visual_capture("describe", "first_person", image_media, std::nullopt, actor, {}); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected && bound_transport.requests.size() == before_unbound_capture);
        bool visual_cancelled{};
        bound_transport.before_reply = [&] { visual_cancelled = true; };
        CHECK(bound_client.send_visual_capture("describe", "first_person", image_media,
            std::nullopt, actor, {}, [&] { return visual_cancelled; }, first.context_binding).status == RequestStatus::cancelled);
        bound_transport.before_reply = {};
        const auto before_cancelled_capture = bound_transport.requests.size();
        CHECK(bound_client.send_visual_capture("describe", "first_person", image_media,
            std::nullopt, actor, {}, [] { return true; }, first.context_binding).status == RequestStatus::cancelled);
        CHECK(bound_transport.requests.size() == before_cancelled_capture);
        bound_transport.fault = BindingEchoTransport::Fault::timeout;
        CHECK(publish().status == RequestStatus::transport_failure);
        bound_transport.fault = BindingEchoTransport::Fault::none;
        const auto newer = publish();
        CHECK(newer.context_binding.sequence == 4); // Capture checkpoint plus a lost acknowledgement never reuse a sequence.
        std::size_t callbacks{};
        const auto receive = [&](const synth::protocol_native::Line&) { ++callbacks; };
        CHECK(bound_client.send_text("older captured turn", receive, first.context_binding).status == RequestStatus::complete);
        CHECK(callbacks == 3);
        CHECK(bound_transport.requests.back().body.find("\"context_sequence\":1") != std::string::npos);
        // Speech is admitted before HTTP EOF, even when every NDJSON line spans reads.
        auto streaming_binding = first.context_binding;
        std::size_t invalidations{};
        streaming_binding.invalidate_response = [&] { ++invalidations; };
        for (const auto chunk_size : {std::size_t{1}, std::size_t{17}, std::size_t{65536}}) {
            bound_transport.chunk_size = chunk_size;
            callbacks = 0;
            const auto streamed = bound_client.send_text("incremental response", [&](const auto& line) {
                if (std::holds_alternative<synth::protocol_native::Dialogue>(line.payload))
                    CHECK(!bound_transport.response_returned);
                else CHECK(bound_transport.response_returned);
                ++callbacks;
            }, streaming_binding);
            CHECK(streamed.status == RequestStatus::complete);
            CHECK(callbacks == 3);
            CHECK(invalidations == 0);
        }
        for (const auto fault : {BindingEchoTransport::Fault::sequence, BindingEchoTransport::Fault::version,
                                BindingEchoTransport::Fault::truncated, BindingEchoTransport::Fault::failed,
                                BindingEchoTransport::Fault::duplicate_id}) {
            bound_transport.fault = fault;
            callbacks = 0;
            const auto before_invalidations = invalidations;
            const auto bad = bound_client.send_text("partial speech then failure", receive, streaming_binding);
            CHECK(bad.status != RequestStatus::complete);
            CHECK(callbacks == 1); // No start/end/action admission on an invalid stream.
            CHECK(invalidations == before_invalidations + 1);
        }
        bound_transport.chunk_size = 0;
        bound_transport.fault = BindingEchoTransport::Fault::none;
        for (const auto fault : {BindingEchoTransport::Fault::sequence, BindingEchoTransport::Fault::version,
                                BindingEchoTransport::Fault::truncated, BindingEchoTransport::Fault::failed,
                                BindingEchoTransport::Fault::duplicate_id}) {
            callbacks = 0;
            bound_transport.fault = fault;
            const auto bad = bound_client.send_text("late invalid line", receive, first.context_binding);
            const auto expected = fault == BindingEchoTransport::Fault::sequence || fault == BindingEchoTransport::Fault::version
                ? RequestStatus::stale_response : fault == BindingEchoTransport::Fault::failed
                ? RequestStatus::server_failure : RequestStatus::malformed_response;
            CHECK(bad.status == expected);
            CHECK(callbacks == 0);
        }
        bound_transport.fault = BindingEchoTransport::Fault::http_error;
        const auto http_error = bound_client.send_text("rejected context", receive, first.context_binding);
        CHECK(http_error.status == RequestStatus::http_failure);
        CHECK(http_error.detail == "HTTP 409: unknown_context_snapshot");
        bound_transport.fault = BindingEchoTransport::Fault::none;
        CHECK(bound_client.trigger("rechat", actor, {}, std::nullopt, std::nullopt, first.context_binding).status == RequestStatus::complete);
        CHECK(!bound_client.uses_rechat_parent_contract());
        CHECK(bound_transport.requests.back().body.find("reply_to_") == std::string::npos);
        {
            BindingEchoTransport parent_transport;
            SynthClient parent_client{parent_transport, SessionOptions{
                .runtime_session_id="session:rechat", .runtime_variant="flat", .client_version="test",
                .runtime_version="1.11.240", .capabilities={"dialogue.turn_ownership","dialogue.rechat.ownership"}, .protocol_version=2}};
            CHECK(parent_client.initialize(1,false).status==RequestStatus::complete);
            CHECK(parent_client.uses_rechat_parent_contract());
            const auto rechat_capture=parent_client.publish_context(0,"save:one",player,actor,{}, {},std::nullopt,{}, {},{});
            const auto parent_reply=parent_client.send_text("Original turn",{},rechat_capture.context_binding);
            CHECK(parent_reply.status==RequestStatus::complete);
            const synth::protocol_native::RechatParent parent{parent_reply.request_id,"line:dialogue"};
            CHECK(parent_client.trigger("rechat",actor,{},std::nullopt,std::nullopt,rechat_capture.context_binding,
                std::nullopt,std::nullopt,std::nullopt,parent).status==RequestStatus::complete);
            const auto wire=synth::json::parse(parent_transport.requests.back().body);
            const auto& payload=synth::protocol_native::required(wire,"payload");
            CHECK(synth::protocol_native::text(payload,"reply_to_request_id")==parent.request_id);
            CHECK(synth::protocol_native::text(payload,"reply_to_line_id")==parent.line_id);
            CHECK(synth::protocol_native::text(wire,"turn_id")==rechat_capture.turn_id);
            const auto before=parent_transport.requests.size();
            bool missing=false;
            try { (void)parent_client.trigger("rechat",actor,{},std::nullopt,std::nullopt,rechat_capture.context_binding); }
            catch(const std::invalid_argument&) { missing=true; }
            CHECK(missing && parent_transport.requests.size()==before);
        }
        for (const auto* lane : {"flat","vr"}) for (const auto fault : {BindingEchoTransport::Fault::none, BindingEchoTransport::Fault::generation,
                BindingEchoTransport::Fault::request, BindingEchoTransport::Fault::turn,
                BindingEchoTransport::Fault::truncated, BindingEchoTransport::Fault::failed}) {
            BindingEchoTransport scene_transport;
            scene_transport.rechat_scene_support=true; scene_transport.fault=fault;
            SynthClient scene_client{scene_transport,SessionOptions{
                .runtime_session_id="session:scene",.runtime_variant=lane,.client_version="test",
                .runtime_version="1.11.240",.capabilities={"dialogue.turn_ownership","dialogue.rechat.ownership","dialogue.rechat.scene"},.protocol_version=2}};
            const auto init=scene_client.initialize(1,false);
            CHECK(scene_client.rechat_scene_ready()==(fault==BindingEchoTransport::Fault::none));
            CHECK((init.status==RequestStatus::complete)==scene_client.rechat_scene_ready());
            scene_transport.fault=BindingEchoTransport::Fault::none; scene_transport.rechat_scene_support=false;
            CHECK(scene_client.initialize(2,false).status==RequestStatus::malformed_response);
            CHECK(!scene_client.rechat_scene_ready());
        }
        CHECK(bound_client.send_action_result("action:one", "idem:one", "succeeded", "done", {}, first.context_binding).status == RequestStatus::complete);
        {
            BindingEchoTransport observed_transport;
            observed_transport.action_inventory_support=true;
            SynthClient observed_client{observed_transport, SessionOptions{
                .runtime_session_id="session:inventory", .runtime_variant="flat", .client_version="test",
                .runtime_version="1.11.240", .capabilities={"dialogue.turn_ownership","action.inventory_observation"}, .protocol_version=2}};
            CHECK(observed_client.initialize(1,false).status==RequestStatus::complete);
            const auto capture=observed_client.publish_context(0,"save:one",player,actor,{}, {},std::nullopt,{}, {},{});
            const synth::protocol_native::ActionInventoryObservation observation{actor,{},"unavailable"};
            CHECK(observed_client.send_action_result("action:inventory","idem:inventory","failed","capture unavailable",{},
                capture.context_binding,observation).status==RequestStatus::complete);
            const auto wire=synth::json::parse(observed_transport.requests.back().body);
            const auto& receipt=synth::protocol_native::required(synth::protocol_native::required(wire,"payload"),"result");
            CHECK(synth::protocol_native::text(receipt,"schema")=="synth.action.result.v2");
            CHECK(synth::protocol_native::text(synth::protocol_native::required(receipt,"inventory"),"observation")=="unavailable");
            CHECK(synth::protocol_native::text(wire,"turn_id")==capture.turn_id);
            synth::core::CancellationSource cancelled_owner{synth::core::RuntimeGeneration::initial()};
            auto cancelled_binding=capture.context_binding;
            cancelled_binding.cancellation=cancelled_owner.token();
            cancelled_owner.cancel();
            const auto before=observed_transport.requests.size();
            CHECK(observed_client.send_action_result("action:inventory","idem:inventory","failed","late",{},
                cancelled_binding,observation).status==RequestStatus::cancelled);
            CHECK(observed_transport.requests.size()==before);
        }
        CHECK(bound_client.activate(actor, "manual", first.context_binding).status == RequestStatus::complete);
        CHECK(bound_client.refresh_dynamic_profiles({actor}, true, first.context_binding).status == RequestStatus::complete);
        const auto diary_admission = bound_client.request_npc_diary(actor, first.context_binding);
        CHECK(diary_admission.outcome().detail == "diary_queued");
        const auto diary_body = bound_transport.requests.back().body;
        CHECK(synth::protocol_native::integer(synth::json::parse(bound_transport.requests.back().body), "context_sequence") == 1);
        for (const auto& code : {"diary_queued", "diary_running", "diary_ready", "diary_failed", "diary_cancelled", "diary_disabled"}) {
            bound_transport.diary_ack = code;
            const auto polled = bound_client.fetch_diary_status(diary_admission);
            CHECK(polled.status == RequestStatus::complete && polled.detail == code);
            CHECK(bound_transport.requests.back().target == "/diary_status.php");
            CHECK(bound_transport.requests.back().timeout_ms == 2000);
            CHECK(bound_transport.requests.back().body == diary_body);
        }
        for (const auto& code : {"", "diary_complete", "diary_partial", "unrecognized"}) {
            bound_transport.diary_ack = code;
            CHECK(bound_client.fetch_diary_status(diary_admission).status == RequestStatus::malformed_response);
        }
        bound_transport.diary_ack = "diary_ready";
        bound_transport.fault = BindingEchoTransport::Fault::sequence;
        CHECK(bound_client.fetch_diary_status(diary_admission).status == RequestStatus::stale_response);
        bound_transport.fault = BindingEchoTransport::Fault::truncated;
        CHECK(bound_client.fetch_diary_status(diary_admission).status == RequestStatus::malformed_response);
        bound_transport.fault = BindingEchoTransport::Fault::none;
        bound_transport.diary_ack = "diary_disabled";
        CHECK(bound_client.request_npc_diary(actor, first.context_binding).outcome().detail == "diary_disabled");
        for (const auto& bad_ack : {"", "diary_complete"}) {
            bound_transport.diary_ack = bad_ack;
            CHECK(bound_client.request_npc_diary(actor, first.context_binding).outcome().status == RequestStatus::malformed_response);
        }
        bound_transport.diary_ack = "diary_queued";
        bound_transport.fault = BindingEchoTransport::Fault::sequence;
        CHECK(bound_client.request_npc_diary(actor, first.context_binding).outcome().status == RequestStatus::stale_response);
        bound_transport.fault = BindingEchoTransport::Fault::none;
        const auto hash = std::string(64, 'a');
        const auto narrator_admission = bound_client.request_diary(player, first.context_binding, synth::protocol_native::DiaryRole::narrator);
        CHECK(narrator_admission.outcome().status == RequestStatus::complete && narrator_admission.outcome().detail == "diary_queued");
        const auto narrator_body = bound_transport.requests.back().body;
        const auto narrator_wire = synth::json::parse(narrator_body);
        const auto& narrator_payload = synth::protocol_native::required(narrator_wire, "payload");
        CHECK(synth::protocol_native::required(narrator_payload, "actors").as_array().empty());
        CHECK(synth::protocol_native::required(narrator_payload, "include_narrator").as_bool());
        bound_transport.diary_ack = "diary_ready";
        CHECK(bound_client.fetch_diary_status(narrator_admission).detail == "diary_ready");
        CHECK(bound_transport.requests.back().body == narrator_body);
        bound_transport.diary_ack = "diary_queued";
        const auto player_admission = bound_client.request_diary(player, first.context_binding, synth::protocol_native::DiaryRole::player);
        CHECK(player_admission.outcome().status == RequestStatus::complete && player_admission.outcome().detail == "diary_queued");
        const auto player_body = bound_transport.requests.back().body;
        const auto player_wire = synth::json::parse(player_body);
        const auto& player_payload = synth::protocol_native::required(player_wire, "payload");
        CHECK(synth::protocol_native::required(player_payload, "actors").as_array().empty());
        CHECK(synth::protocol_native::required(player_payload, "include_player").as_bool());
        CHECK(!synth::protocol_native::required(player_payload, "include_narrator").as_bool());
        bound_transport.diary_ack = "diary_ready";
        CHECK(bound_client.fetch_diary_status(player_admission).detail == "diary_ready");
        CHECK(bound_transport.requests.back().body == player_body);
        bound_transport.diary_ack = "diary_queued";
        CHECK(bound_client.send_audio({"media/" + hash, "audio/wav", hash, 44}, {}, first.context_binding).status == RequestStatus::complete);
        CHECK(synth::protocol_native::text(synth::json::parse(bound_transport.requests.back().body), "turn_id") == first.turn_id);
        const auto before_unbound = bound_transport.requests.size();
        rejected = false;
        try { (void)bound_client.send_text("unbound"); } catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
        CHECK(bound_transport.requests.size() == before_unbound);
        // The owner travels with the acknowledged context, not with latest session state.
        synth::core::CancellationSource session_owner{synth::core::RuntimeGeneration::initial()};
        synth::core::CancellationSource turn_owner{session_owner.token()};
        const auto owned_context = bound_client.publish_context(0, "save:one", player, actor,
            {}, {}, std::nullopt, {}, {}, {}, {}, turn_owner.token());
        CHECK(owned_context.status == RequestStatus::complete);
        const auto owner_binding = owned_context.context_binding;
        callbacks = 0;
        bound_transport.before_reply = [&] { turn_owner.cancel(); };
        CHECK(bound_client.send_text("cancel during HTTP", receive, owner_binding).status == RequestStatus::cancelled);
        CHECK(callbacks == 0);
        bound_transport.before_reply = {};
        const auto before_cancelled = bound_transport.requests.size();
        CHECK(bound_client.trigger("rechat", actor, {}, std::nullopt, std::nullopt, owner_binding).status == RequestStatus::cancelled);
        CHECK(bound_client.send_action_result("action:old", "idem:old", "succeeded", "", {}, owner_binding).status == RequestStatus::cancelled);
        CHECK(bound_client.send_audio({"media/" + hash, "audio/wav", hash, 44}, {}, owner_binding).status == RequestStatus::cancelled);
        CHECK(bound_transport.requests.size() == before_cancelled);
        synth::core::CancellationSource next_owner{session_owner.token()};
        auto next_binding = first.context_binding;
        next_binding.cancellation = next_owner.token();
        CHECK(bound_client.send_text("new turn survives", receive, next_binding).status == RequestStatus::complete);
        CHECK(callbacks == 3);
        callbacks = 0;
        CHECK(bound_client.send_text("cancel between lines", [&](const auto&) {
            ++callbacks;
            session_owner.cancel();
        }, next_binding).status == RequestStatus::cancelled);
        CHECK(callbacks == 1);
        CHECK(next_owner.is_cancelled());
        CHECK(bound_client.initialize(2, false).status == RequestStatus::complete);
        CHECK(publish().context_binding.sequence == 1);
        rejected = false;
        try { (void)bound_client.send_text("stale generation", {}, first.context_binding); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
        BlockingTurnTransport control_transport;
        control_transport.echo.quest_support=true;
        control_transport.echo.quest_tracking_support=true;
        control_transport.echo.quest_reaction_support=true;
        SynthClient control_client{control_transport, SessionOptions{
            .runtime_session_id = "session:control", .runtime_variant = "flat", .client_version = "test",
            .runtime_version = "1.11.240", .capabilities = {"dialogue.text", "dialogue.turn_ownership", "dialogue.turn_cancel",
                "context.quest_events", "context.quest_tracking", "dialogue.quest_reactions"}, .protocol_version = 2}};
        CHECK(control_client.initialize(1, false).status == RequestStatus::complete);
        synth::core::CancellationSource control_owner{synth::core::RuntimeGeneration::initial()};
        const auto anchor_context = control_client.publish_context(0, "save:one", player, actor, {}, {}, std::nullopt,
            {}, {}, {}, {}, control_owner.token());
        CHECK(anchor_context.context_binding.context_request_id == anchor_context.request_id);
        auto inflight = std::async(std::launch::async, [&] {
            return control_client.send_text("blocked dialogue", {}, anchor_context.context_binding);
        });
        control_transport.entered.get_future().wait();
        control_owner.cancel();
        const auto anchor = control_client.take_interrupted_turn();
        auto cancel_rpc = std::async(std::launch::async, [&] {
            return control_client.cancel(anchor_context.request_id, 1);
        });
        const auto overtook = cancel_rpc.wait_for(std::chrono::milliseconds{500}) == std::future_status::ready;
        control_transport.release.set_value();
        CHECK(overtook);
        CHECK(cancel_rpc.get().status == RequestStatus::complete);
        CHECK(inflight.get().status == RequestStatus::cancelled);
        CHECK(anchor && anchor->context_request_id == anchor_context.request_id);
        CHECK(!control_client.take_interrupted_turn());
        CHECK(control_client.cancel(anchor_context.request_id, 2).status == RequestStatus::cancelled);
        CHECK(control_transport.echo.requests[2].timeout_ms == 2000);
        CHECK(control_client.ready() && control_client.quest_events_ready() && control_client.quest_tracking_ready() &&
              control_client.quest_reactions_ready());
        synth::core::CancellationSource resumed_owner{synth::core::RuntimeGeneration::initial()};
        const auto resumed_context=control_client.publish_context(0,"save:one",player,actor,{}, {},std::nullopt,
            {}, {}, {}, {},resumed_owner.token());
        CHECK(resumed_context.status==RequestStatus::complete && resumed_context.context_binding.generation==1);
        CHECK(resumed_context.context_binding.sequence>anchor_context.context_binding.sequence);
        CHECK(control_client.send_text("after combat interruption",{},resumed_context.context_binding).status==RequestStatus::complete);
        CHECK(control_client.send_text("stale interrupted dialogue",{},anchor_context.context_binding).status==RequestStatus::cancelled);
        CHECK(std::ranges::none_of(control_transport.echo.requests,[](const auto& request) {
            return synth::protocol_native::text(synth::json::parse(request.body),"type")=="halt";
        }));
        BlockingTurnTransport image_control_transport;
        SynthClient image_control_client{image_control_transport, SessionOptions{
            .runtime_session_id = "session:visual-control", .runtime_variant = "flat", .client_version = "test",
            .runtime_version = "1.11.240", .capabilities = {"dialogue.turn_ownership", "dialogue.turn_cancel"}, .protocol_version = 2}};
        CHECK(image_control_client.initialize(1, false).status == RequestStatus::complete);
        synth::core::CancellationSource image_owner{synth::core::RuntimeGeneration::initial()};
        const auto image_context = image_control_client.publish_context(0, "save:one", player, actor, {}, {}, std::nullopt,
            {}, {}, {}, {}, image_owner.token());
        auto image_inflight = std::async(std::launch::async, [&] {
            return image_control_client.send_visual_capture("describe", "first_person", image_media,
                std::nullopt, actor, {}, {}, image_context.context_binding);
        });
        image_control_transport.entered.get_future().wait();
        image_owner.cancel();
        const auto image_anchor = image_control_client.take_interrupted_turn();
        auto image_cancel = std::async(std::launch::async, [&] { return image_control_client.cancel(image_context.request_id, 1); });
        const auto image_overtook = image_cancel.wait_for(std::chrono::milliseconds{500}) == std::future_status::ready;
        image_control_transport.release.set_value();
        CHECK(image_overtook);
        CHECK(image_cancel.get().status == RequestStatus::complete);
        CHECK(image_inflight.get().status == RequestStatus::cancelled);
        CHECK(image_anchor && image_anchor->context_request_id == image_context.request_id);
        CHECK(!image_control_client.take_interrupted_turn());
        BindingEchoTransport retiring_transport;
        SynthClient retiring_client{retiring_transport, SessionOptions{
            .runtime_session_id = "session:retirement", .runtime_variant = "flat", .client_version = "test",
            .runtime_version = "1.11.240", .capabilities = {"runtime.retirement"}, .protocol_version = 2}};
        CHECK(!retiring_client.retire_session());
        CHECK(retiring_transport.requests.empty());
        retiring_transport.fault = BindingEchoTransport::Fault::timeout;
        CHECK(retiring_client.initialize(1, false).status == RequestStatus::transport_failure);
        retiring_transport.fault = BindingEchoTransport::Fault::none;
        const auto uncertain_retirement = retiring_client.retire_session();
        CHECK(uncertain_retirement && uncertain_retirement->status == RequestStatus::complete);
        CHECK(!retiring_client.ready());
        CHECK(retiring_transport.requests.back().timeout_ms == 2000);
        CHECK(synth::protocol_native::text(synth::json::parse(retiring_transport.requests.back().body), "type") == "halt");
        const auto after_retirement = retiring_transport.requests.size();
        CHECK(!retiring_client.retire_session());
        CHECK(retiring_transport.requests.size() == after_retirement);
        CHECK(retiring_client.initialize(2, false).status == RequestStatus::complete);
        retiring_transport.fault = BindingEchoTransport::Fault::timeout;
        CHECK(retiring_client.retire_session()->status == RequestStatus::transport_failure);
        CHECK(!retiring_client.retire_session());
        // Batch receipts freeze selection, reason and original context for every later status RPC.
        using synth::protocol_native::DiaryReason;
        using synth::protocol_native::DiaryRequest;
        using synth::protocol_native::DiaryRole;
        const std::vector<std::string> diary_capabilities{"diary.manual.npc", "diary.manual.player", "diary.manual.narrator", "diary.batch.status",
            "diary.automatic.sleep", "diary.automatic.wait"};
        BindingEchoTransport batch_transport;
        SynthClient batch_client{batch_transport, SessionOptions{
            .runtime_session_id = "session:diary-batch", .runtime_variant = "flat", .client_version = "test",
            .runtime_version = "1.11.240", .capabilities = diary_capabilities, .protocol_version = 2}};
        CHECK(batch_client.initialize(1, false).status == RequestStatus::complete);
        auto second_actor = actor;
        second_actor.form_id = "0x0001A4D8"; // Same display name, different canonical identity.
        synth::core::CancellationSource batch_owner{synth::core::RuntimeGeneration::initial()};
        const auto batch_context = batch_client.publish_context(0, "save:one", player, actor, {second_actor}, {}, std::nullopt,
            {}, {}, {}, {}, batch_owner.token());
        CHECK(batch_context.status == RequestStatus::complete);
        for (const auto reason : {DiaryReason::manual, DiaryReason::sleep, DiaryReason::wait}) {
            DiaryRequest selection{{actor, second_actor}, true, true, reason};
            const auto receipt = batch_client.request_diary(selection, batch_context.context_binding);
            CHECK(receipt.outcome().status == RequestStatus::complete && receipt.outcome().detail == "diary_queued");
            const auto original_body = batch_transport.requests.back().body;
            const auto wire = synth::json::parse(original_body);
            const auto& payload = synth::protocol_native::required(wire, "payload");
            CHECK(synth::protocol_native::required(payload, "actors").as_array().size() == 2);
            CHECK(synth::protocol_native::required(payload, "include_player").as_bool());
            CHECK(synth::protocol_native::required(payload, "include_narrator").as_bool());
            CHECK(synth::protocol_native::text(payload, "reason") ==
                (reason == DiaryReason::manual ? "manual" : reason == DiaryReason::sleep ? "sleep" : "wait"));
            selection = {{}, false, true, DiaryReason::manual};
            CHECK(batch_client.publish_context(0, "save:one", player, second_actor, {}, {}, std::nullopt,
                {}, {}, {}).status == RequestStatus::complete);
            for (const auto& status : {"diary_running", "diary_ready", "diary_partial", "diary_failed", "diary_cancelled"}) {
                batch_transport.diary_ack = status;
                const auto polled = batch_client.fetch_diary_status(receipt);
                CHECK(polled.status == RequestStatus::complete && polled.detail == status);
                CHECK(batch_transport.requests.back().body == original_body);
                CHECK(batch_transport.requests.back().target == "/diary_status.php");
                CHECK(batch_transport.requests.back().timeout_ms == 2000);
            }
            batch_transport.diary_ack = "diary_queued";
        }
        const DiaryRequest all_roles{{actor}, true, true, DiaryReason::wait};
        std::vector<synth::protocol_native::Identity> maximum_actors;
        for (std::size_t i = 0; i < 16; ++i) {
            auto candidate = actor;
            candidate.form_id = std::string{"0x0001000"} + "0123456789ABCDEF"[i];
            maximum_actors.push_back(std::move(candidate));
        }
        CHECK(batch_client.request_diary(DiaryRequest{maximum_actors, true, true, DiaryReason::sleep},
            batch_context.context_binding).outcome().status == RequestStatus::complete);
        CHECK(synth::protocol_native::required(synth::protocol_native::required(
            synth::json::parse(batch_transport.requests.back().body), "payload"), "actors").as_array().size() == 16);
        // Every selected role and the exact automatic reason must have initialized capabilities.
        for (const auto& missing : diary_capabilities) {
            auto caps = diary_capabilities;
            std::erase(caps, missing);
            BindingEchoTransport denied_transport;
            SynthClient denied_client{denied_transport, SessionOptions{
                .runtime_session_id = "session:diary-denied", .runtime_variant = "flat", .client_version = "test",
                .runtime_version = "1.11.240", .capabilities = caps, .protocol_version = 2}};
            CHECK(denied_client.initialize(1, false).status == RequestStatus::complete);
            const auto denied_context = denied_client.publish_context(0, "save:one", player, actor, {}, {}, std::nullopt, {}, {}, {});
            auto selection = all_roles;
            selection.reason = missing == "diary.automatic.sleep" ? DiaryReason::sleep : DiaryReason::wait;
            const auto before = denied_transport.requests.size();
            rejected = false;
            try { (void)denied_client.request_diary(selection, denied_context.context_binding); }
            catch (const std::invalid_argument&) { rejected = true; }
            CHECK(rejected && denied_transport.requests.size() == before);
        }
        auto alias = actor;
        alias.form_id = "0x0001a4d7";
        alias.origin_plugin = "fallout4.esm";
        alias.display_name = "Different presentation";
        auto foreign = second_actor;
        foreign.playthrough_id = "save:other";
        auto malformed = actor;
        malformed.form_id = "0xGGGGGGGG";
        for (const auto& invalid : std::vector<DiaryRequest>{
            {}, {{actor, alias}}, {{actor, foreign}}, {{malformed}},
            {std::vector<synth::protocol_native::Identity>(17, actor)},
            {{actor}, false, false, static_cast<DiaryReason>(99)}}) {
            const auto before = batch_transport.requests.size();
            rejected = false;
            try { (void)batch_client.request_diary(invalid, batch_context.context_binding); }
            catch (const std::invalid_argument&) { rejected = true; }
            CHECK(rejected && batch_transport.requests.size() == before);
        }
        const auto before_bad_role = batch_transport.requests.size();
        rejected = false;
        try { (void)batch_client.request_diary(actor, batch_context.context_binding, static_cast<DiaryRole>(99)); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected && batch_transport.requests.size() == before_bad_role);
        const auto receipt = batch_client.request_diary(all_roles, batch_context.context_binding);
        batch_transport.diary_ack = "diary_partial";
        CHECK(batch_client.request_diary(all_roles, batch_context.context_binding).outcome().status == RequestStatus::malformed_response);
        batch_transport.diary_ack = "diary_queued";
        const auto single_receipt = batch_client.request_npc_diary(actor, batch_context.context_binding);
        batch_transport.diary_ack = "diary_partial";
        CHECK(batch_client.fetch_diary_status(single_receipt).status == RequestStatus::malformed_response);
        batch_transport.diary_ack = "diary_queued";
        const auto before_foreign = bound_transport.requests.size();
        rejected = false;
        try { (void)bound_client.fetch_diary_status(receipt); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected && bound_transport.requests.size() == before_foreign);
        batch_transport.diary_ack = "diary_disabled";
        const auto disabled_receipt = batch_client.request_diary(all_roles, batch_context.context_binding);
        CHECK(disabled_receipt.outcome().detail == "diary_disabled");
        const auto before_disabled_poll = batch_transport.requests.size();
        rejected = false;
        try { (void)batch_client.fetch_diary_status(disabled_receipt); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected && batch_transport.requests.size() == before_disabled_poll);
        batch_transport.diary_ack = "diary_ready";
        batch_transport.before_reply = [&] { batch_owner.cancel(); };
        CHECK(batch_client.fetch_diary_status(receipt).status == RequestStatus::cancelled);
        batch_transport.before_reply = {};
        const auto before_cancelled_batch = batch_transport.requests.size();
        CHECK(batch_client.fetch_diary_status(receipt).status == RequestStatus::cancelled);
        CHECK(batch_client.request_diary(all_roles, batch_context.context_binding).outcome().status == RequestStatus::cancelled);
        CHECK(batch_transport.requests.size() == before_cancelled_batch);
        CHECK(batch_client.initialize(2, false).status == RequestStatus::complete);
        const auto before_stale = batch_transport.requests.size();
        rejected = false;
        try { (void)batch_client.fetch_diary_status(receipt); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected && batch_transport.requests.size() == before_stale);
        BindingEchoTransport quest_transport;
        quest_transport.quest_support=true;
        SynthClient quest_client{quest_transport, SessionOptions{
            .runtime_session_id="session:quest",.runtime_variant="flat",.client_version="test",
            .runtime_version="1.11.240",.capabilities={"context.quest_events"},.protocol_version=2}};
        CHECK(quest_client.initialize(1,false).status==RequestStatus::complete);
        CHECK(quest_client.quest_events_ready());
        const synth::protocol_native::NativeQuestBatch quest_batch{"quest:1",{
            {{synth::core::QuestEventKind::stage,0x229E5,20,1,false},"Fallout4.esm",0}}};
        synth::core::CancellationSource quest_owner{synth::core::RuntimeGeneration::initial()};
        const auto publish_quest=[&] {
            return quest_client.publish_context(0,"save:one",player,std::nullopt,{}, {},std::nullopt,
                {{"0x000229E5","Fallout4.esm","Quest","QuestEditor",45,1,{"Find the settler"},"complete"}},
                {}, {}, {},quest_owner.token(),std::nullopt,"complete",{}, {}, {},quest_batch);
        };
        quest_transport.fault=BindingEchoTransport::Fault::timeout;
        const auto lost_quest=publish_quest();
        CHECK(lost_quest.status==RequestStatus::transport_failure && lost_quest.context_binding.sequence==0);
        const auto first_quest=synth::json::parse(quest_transport.requests.back().body);
        quest_transport.fault=BindingEchoTransport::Fault::none;
        const auto retried_quest=publish_quest();
        CHECK(retried_quest.status==RequestStatus::complete && retried_quest.context_binding.sequence==2);
        const auto second_quest=synth::json::parse(quest_transport.requests.back().body);
        CHECK(synth::protocol_native::text(first_quest,"request_id")!=synth::protocol_native::text(second_quest,"request_id"));
        CHECK(synth::json::write(synth::protocol_native::required(synth::protocol_native::required(first_quest,"payload"),"quest_events"))==
              synth::json::write(synth::protocol_native::required(synth::protocol_native::required(second_quest,"payload"),"quest_events")));
        quest_transport.fault=BindingEchoTransport::Fault::http_error;
        const auto denied_quest=publish_quest();
        CHECK(denied_quest.status==RequestStatus::http_failure && denied_quest.http_status==409);
        quest_transport.fault=BindingEchoTransport::Fault::truncated;
        CHECK(publish_quest().status==RequestStatus::malformed_response);
        quest_transport.fault=BindingEchoTransport::Fault::none;
        quest_transport.before_reply=[&] { quest_owner.cancel(); };
        CHECK(publish_quest().status==RequestStatus::cancelled);
        quest_transport.before_reply={};
        CHECK(quest_client.initialize(2,false).status==RequestStatus::complete);
        const auto before_old_quest=quest_transport.requests.size();
        rejected=false;try{(void)publish_quest();}catch(const std::invalid_argument&){rejected=true;}
        CHECK(rejected && quest_transport.requests.size()==before_old_quest);
        BindingEchoTransport old_quest_transport;
        SynthClient old_quest_client{old_quest_transport,SessionOptions{
            .runtime_session_id="session:old-quest",.runtime_variant="flat",.client_version="test",
            .runtime_version="1.11.240",.capabilities={"context.quest_events"},.protocol_version=2}};
        CHECK(old_quest_client.initialize(1,false).status==RequestStatus::complete && !old_quest_client.quest_events_ready());
        const auto old_context=old_quest_client.publish_context(0,"save:one",player,actor,{}, {},std::nullopt,{}, {}, {});
        CHECK(old_context.status==RequestStatus::complete);
        CHECK(old_quest_client.send_text("Still works",{},old_context.context_binding).status==RequestStatus::complete);
        const auto before_unnegotiated=old_quest_transport.requests.size();
        rejected=false;
        try{(void)old_quest_client.publish_context(0,"save:one",player,actor,{}, {},std::nullopt,{}, {}, {},
            {},{},std::nullopt,{}, {}, {}, {},quest_batch);}catch(const std::invalid_argument&){rejected=true;}
        CHECK(rejected && old_quest_transport.requests.size()==before_unnegotiated);
        quest_transport.fault=BindingEchoTransport::Fault::truncated;
        CHECK(quest_client.initialize(3,false).status==RequestStatus::malformed_response && !quest_client.quest_events_ready());
        for (const auto server_tracking : {false,true}) {
            BindingEchoTransport tracking_transport;
            tracking_transport.quest_support=true;
            tracking_transport.quest_tracking_support=server_tracking;
            SynthClient tracking_client{tracking_transport,SessionOptions{
                .runtime_session_id="session:tracking",.runtime_variant="flat",.client_version="test",
                .runtime_version="1.11.240",.capabilities={"context.quest_events","context.quest_tracking"},.protocol_version=2}};
            CHECK(tracking_client.initialize(1,false).status==RequestStatus::complete);
            CHECK(tracking_client.quest_events_ready());
            CHECK(tracking_client.quest_tracking_ready()==server_tracking);
            for (const auto tracked : {false,true}) {
                CHECK(tracking_client.publish_context(0,"save:one",player,std::nullopt,{}, {},std::nullopt,
                    {{"0x000229E5","Fallout4.esm","Quest","QuestEditor",45,1,{"Find settler"},"complete",tracked}},
                    {}, {}).status==RequestStatus::complete);
                const auto wire=synth::json::parse(tracking_transport.requests.back().body);
                const auto& quest=synth::protocol_native::required(synth::protocol_native::required(wire,"payload"),"active_quests").as_array().front();
                const auto* observed=quest.find("tracked");
                CHECK((observed!=nullptr)==server_tracking);
                if (observed) CHECK(observed->as_bool()==tracked);
            }
            tracking_transport.fault=BindingEchoTransport::Fault::truncated;
            CHECK(tracking_client.initialize(2,false).status==RequestStatus::malformed_response);
            CHECK(!tracking_client.quest_tracking_ready() && !tracking_client.quest_events_ready());
        }
        for (const auto support : {false,true}) {
            BindingEchoTransport reaction_transport;
            reaction_transport.quest_support=true;
            reaction_transport.quest_tracking_support=true;
            reaction_transport.quest_reaction_support=support;
            SynthClient reaction_client{reaction_transport,SessionOptions{
                .runtime_session_id="session:reaction",.runtime_variant="flat",.client_version="test",
                .runtime_version="1.11.240",.capabilities={"context.quest_events","context.quest_tracking","dialogue.quest_reactions"},.protocol_version=2}};
            CHECK(reaction_client.initialize(1,false).status==RequestStatus::complete);
            CHECK(reaction_client.quest_events_ready() && reaction_client.quest_tracking_ready());
            CHECK(reaction_client.quest_reactions_ready()==support);
            CHECK(reaction_client.publish_context(0,"save:one",player,actor,{}, {},std::nullopt,{}, {}, {}).status==RequestStatus::complete);
            const auto bound=reaction_client.publish_context(0,"save:one",player,actor,{}, {},std::nullopt,{}, {}, {}).context_binding;
            const auto before=reaction_transport.requests.size();
            rejected=false;
            try {
                CHECK(reaction_client.trigger("quest_updated",actor,{},std::nullopt,std::nullopt,bound,std::nullopt,1).status==RequestStatus::complete);
            } catch (const std::invalid_argument&) { rejected=true; }
            CHECK(rejected!=support);
            CHECK(reaction_transport.requests.size()==before+(support?1:0));
            if (support) {
                const auto wire=synth::json::parse(reaction_transport.requests.back().body);
                CHECK(synth::protocol_native::integer(synth::protocol_native::required(wire,"payload"),"previous_context_sequence")==1);
                for (const auto previous : {std::uint64_t{0},bound.sequence,bound.sequence+1}) {
                    rejected=false;
                    try { (void)reaction_client.trigger("quest_updated",actor,{},std::nullopt,std::nullopt,bound,std::nullopt,previous); }
                    catch (const std::invalid_argument&) { rejected=true; }
                    CHECK(rejected);
                }
            }
            reaction_transport.fault=BindingEchoTransport::Fault::truncated;
            CHECK(reaction_client.initialize(2,false).status==RequestStatus::malformed_response);
            CHECK(!reaction_client.quest_reactions_ready());
        }
        for (int mode=0; mode<5; ++mode) {
            BindingEchoTransport reaction_transport;reaction_transport.player_support=true;reaction_transport.player_reaction_support=mode!=1;
            SessionOptions options{.runtime_session_id="session:player-reaction",.runtime_variant=mode==4?"vr":"flat",
                .client_version="test",.runtime_version="1.11.240",.capabilities={"context.player_events","dialogue.player_reactions",
                    "context.quest_events","context.quest_tracking","dialogue.quest_reactions"},.protocol_version=2};
            if (mode==2) options.capabilities.erase(options.capabilities.begin());
            if (mode==3) options.capabilities.erase(options.capabilities.begin()+1);
            SynthClient reaction_client{reaction_transport,options};
            CHECK(reaction_client.initialize(1,false).status==RequestStatus::complete);
            CHECK(reaction_client.player_reactions_ready()==(mode==0));
            const synth::protocol_native::Identity reaction_player{"0x00000014","Fallout4.esm","save:one","Player"};
            const synth::protocol_native::Identity reaction_actor{"0x0001A4D7","Fallout4.esm","save:one","Preston"};
            const auto published=reaction_client.publish_context(0,"save:one",reaction_player,reaction_actor,{reaction_actor},{},std::nullopt,{},{},{});
            if (mode!=0) {
                bool refused=false;try{(void)reaction_client.trigger("player_reaction",reaction_actor,{},std::nullopt,std::nullopt,
                    published.context_binding,std::nullopt,std::nullopt,1);}catch(const std::invalid_argument&){refused=true;}
                CHECK(refused);continue;
            }
            CHECK(reaction_client.player_events_ready() && reaction_client.quest_events_ready() && reaction_client.quest_tracking_ready() && reaction_client.quest_reactions_ready());
            CHECK(reaction_client.trigger("player_reaction",reaction_actor,{},std::nullopt,std::nullopt,published.context_binding,
                std::nullopt,std::nullopt,1).status==RequestStatus::complete);
            auto encoded=synth::json::parse(reaction_transport.requests.back().body);
            CHECK(synth::protocol_native::text(synth::protocol_native::required(encoded,"payload"),"event_id")=="player:1");
            for (const auto serial : {0ULL,9007199254740992ULL}) {
                bool refused=false;try{(void)reaction_client.trigger("player_reaction",reaction_actor,{},std::nullopt,std::nullopt,
                    published.context_binding,std::nullopt,std::nullopt,serial);}catch(const std::invalid_argument&){refused=true;}
                CHECK(refused);
            }
            bool refused=false;try{(void)reaction_client.trigger("bored",reaction_actor,{},std::nullopt,std::nullopt,
                published.context_binding,std::nullopt,std::nullopt,1);}catch(const std::invalid_argument&){refused=true;}CHECK(refused);
            reaction_transport.fault=BindingEchoTransport::Fault::truncated;
            CHECK(reaction_client.initialize(2,false).status==RequestStatus::malformed_response && !reaction_client.player_reactions_ready());
        }
        for (int mode=0; mode<4; ++mode) {
            BindingEchoTransport player_transport;player_transport.player_support=mode!=0;
            SessionOptions options{.runtime_session_id="session:player-event",.runtime_variant=mode==3?"vr":"flat",
                .client_version="test",.runtime_version="1.11.240",.capabilities={"context.player_events","context.quest_events",
                    "context.quest_tracking","dialogue.quest_reactions"},.protocol_version=2};
            if (mode==2) options.capabilities.erase(options.capabilities.begin());
            SynthClient player_client{player_transport,options};
            CHECK(player_client.initialize(1,false).status==RequestStatus::complete);
            CHECK(player_client.player_events_ready()==(mode==1));
            const synth::protocol_native::Identity observer{"0x00000014","Fallout4.esm","save:one","Player"};
            const auto now=synth::core::SnapshotClock::now();
            synth::core::PlayerEventSample before{synth::core::RuntimeGeneration::initial(),synth::core::RuntimeVariant::flat,
                0x14,"Fallout4.esm","save:one",now,100,10,true};
            auto after=before;after.observed_at+=std::chrono::seconds{1};after.game_time_ticks=101;after.level=11;after.in_combat=false;
            synth::protocol_native::NativePlayerEvent observed{{1,synth::core::PlayerEventKind::level_up,before,after},200};
            const auto publish_player=[&](synth::core::CancellationToken token={}) {
                return player_client.publish_context(0,"save:one",observer,std::nullopt,{}, {},std::nullopt,
                    {}, {}, {}, {},token,std::nullopt,{}, {}, {}, {},std::nullopt,observed);
            };
            if (mode!=1) {
                bool refused=false;try{(void)publish_player();}catch(const std::invalid_argument&){refused=true;}
                CHECK(refused && player_transport.requests.size()==1);continue;
            }
            CHECK(player_client.quest_events_ready() && player_client.quest_tracking_ready() && player_client.quest_reactions_ready());
            CHECK(publish_player().status==RequestStatus::complete);
            const auto encoded=synth::json::parse(player_transport.requests.back().body);
            const auto evidence=synth::protocol_native::required(synth::protocol_native::required(encoded,"payload"),"player_event");
            CHECK(synth::protocol_native::text(evidence,"event_id")=="player:1");
            player_transport.fault=BindingEchoTransport::Fault::timeout;
            CHECK(publish_player().status==RequestStatus::transport_failure);
            player_transport.fault=BindingEchoTransport::Fault::none;
            CHECK(publish_player().status==RequestStatus::complete);
            CHECK(synth::json::write(evidence)==synth::json::write(synth::protocol_native::required(
                synth::protocol_native::required(synth::json::parse(player_transport.requests.back().body),"payload"),"player_event")));
            player_transport.player_ack="wrong_ack";
            CHECK(publish_player().status==RequestStatus::malformed_response);
            player_transport.player_ack.clear();
            CHECK(publish_player().status==RequestStatus::malformed_response);
            player_transport.player_ack="context_accepted";
            synth::core::CancellationSource cancelled{synth::core::RuntimeGeneration::initial()};cancelled.cancel();
            CHECK(publish_player(cancelled.token()).status==RequestStatus::cancelled);
            observed.transition.after.level=10;
            bool refused=false;try{(void)publish_player();}catch(const std::invalid_argument&){refused=true;}CHECK(refused);
            player_transport.fault=BindingEchoTransport::Fault::truncated;
            CHECK(player_client.initialize(2,false).status==RequestStatus::malformed_response);
            CHECK(!player_client.player_events_ready());
        }
        for (int mode=0;mode<7;++mode) {
            BindingEchoTransport wire;wire.actor_support=mode!=0;
            SessionOptions options{.runtime_session_id="session:actor-event",.runtime_variant=mode==3?"vr":"flat",
                .client_version="test",.runtime_version="1.11.240",.capabilities={"context.actor_events","context.player_events",
                    "dialogue.player_reactions","context.quest_events","context.quest_tracking","dialogue.quest_reactions"},.protocol_version=2};
            if (mode==2) options.capabilities.erase(options.capabilities.begin());
            if (mode==4) wire.fault=BindingEchoTransport::Fault::truncated;
            if (mode==5) wire.fault=BindingEchoTransport::Fault::sequence;
            if (mode==6) wire.fault=BindingEchoTransport::Fault::duplicate_id;
            SynthClient actor_client{wire,options};
            const auto actor_initialized=actor_client.initialize(1,false);
            CHECK((actor_initialized.status==RequestStatus::complete)==(mode<4));
            CHECK(actor_client.actor_events_ready()==(mode==1));
            if (mode!=1) continue;
            CHECK(actor_client.player_events_ready() && actor_client.player_reactions_ready() && actor_client.quest_events_ready() &&
                actor_client.quest_tracking_ready() && actor_client.quest_reactions_ready());
            const auto now=synth::core::SnapshotClock::now();
            const synth::core::WorldPose pose{{},synth::core::UnitVector3::from({1,0,0}),synth::core::UnitVector3::from({0,0,1})};
            auto capture=std::make_shared<synth::client::BoundActorEventScene>();
            capture->session_id=options.runtime_session_id;capture->native_epoch=3;
            capture->scene=std::make_shared<const synth::core::RuntimeSnapshot>(synth::core::Game::fallout4,synth::core::RuntimeVariant::flat,
                synth::core::RuntimeGeneration::initial(),1,now,pose,std::nullopt,std::nullopt,std::nullopt,
                synth::core::ActorSnapshot{0x14,"Player",{},"Fallout4.esm","save:one"},std::vector<synth::core::ActorSnapshot>{},42);
            capture->events.push_back({{synth::core::ActorEventKind::equipped,0x14,0,0x4321,0,0,now},
                synth::context::identity_of(capture->scene->player()),std::nullopt,
                synth::client::ActorEventItem{0x4321,"Fallout4.esm","Rifle",43},synth::client::ActorEventKnowledge::player_equipment});
            synth::protocol_native::NativeActorBatch batch{1,0,capture};
            const auto publish_actor=[&](synth::core::CancellationToken token={}) {
                return actor_client.publish_context(0,"save:one",{"0x00000014","Fallout4.esm","save:one","Player"},std::nullopt,{}, {},
                    std::nullopt,{}, {}, {}, {},token,std::nullopt,{}, {}, {}, {},std::nullopt,std::nullopt,batch);
            };
            CHECK(publish_actor().status==RequestStatus::complete);
            const auto body=synth::json::parse(wire.requests.back().body);
            const auto evidence=synth::protocol_native::required(synth::protocol_native::required(body,"payload"),"actor_events");
            CHECK(synth::protocol_native::text(evidence,"batch_id")=="actor:1");
            wire.fault=BindingEchoTransport::Fault::timeout;CHECK(publish_actor().status==RequestStatus::transport_failure);
            wire.fault=BindingEchoTransport::Fault::none;batch.age_ms=1000;CHECK(publish_actor().status==RequestStatus::complete);
            const auto retry=synth::protocol_native::required(synth::protocol_native::required(synth::json::parse(wire.requests.back().body),"payload"),"actor_events");
            CHECK(synth::json::write(synth::protocol_native::required(evidence,"events"))==
                synth::json::write(synth::protocol_native::required(retry,"events")));
            CHECK(synth::protocol_native::integer(retry,"age_ms")==1000);
            wire.player_ack="wrong_ack";CHECK(publish_actor().status==RequestStatus::malformed_response);
            wire.player_ack.clear();CHECK(publish_actor().status==RequestStatus::malformed_response);
            wire.player_ack="context_accepted";
            synth::core::CancellationSource cancelled{synth::core::RuntimeGeneration::initial()};cancelled.cancel();
            CHECK(publish_actor(cancelled.token()).status==RequestStatus::cancelled);
            wire.actor_support=false;CHECK(actor_client.initialize(2,false).status==RequestStatus::complete && !actor_client.actor_events_ready());
            const auto sent=wire.requests.size();bool refused=false;
            try{(void)publish_actor();}catch(const std::invalid_argument&){refused=true;}
            CHECK(refused && wire.requests.size()==sent);
        }
        // Larger inventories require a generation-owned ACK; old peers still receive valid partial v1-sized rows.
        for (int mode=0;mode<9;++mode) {
            BindingEchoTransport wire;wire.inventory_support=mode!=0 && mode!=8;wire.actor_support=mode==8;
            SessionOptions options{.runtime_session_id="inventory:wire",.runtime_variant=mode==3?"vr":"flat",
                .client_version="test",.runtime_version="1.11.240",
                .capabilities={"context.inventory_512","context.actor_events","context.player_events","dialogue.player_reactions",
                    "context.quest_events","context.quest_tracking","dialogue.quest_reactions"},.protocol_version=2};
            if (mode==2) options.capabilities.erase(options.capabilities.begin());
            if (mode==7) options.capabilities={"context.inventory_512"};
            if (mode==4) wire.fault=BindingEchoTransport::Fault::truncated;
            if (mode==5) wire.fault=BindingEchoTransport::Fault::sequence;
            if (mode==6) wire.fault=BindingEchoTransport::Fault::duplicate_id;
            SynthClient inventory_client{wire,options};
            const auto inventory_initialized=inventory_client.initialize(1,false);
            CHECK((inventory_initialized.status==RequestStatus::complete)==(mode<4 || mode>6));
            const bool extended=mode==1 || mode==7;
            CHECK(inventory_client.extended_inventory_ready()==extended);
            if (mode>=4 && mode<=6) continue;
            if (mode==1 || mode==8) CHECK(inventory_client.actor_events_ready() && inventory_client.player_events_ready() &&
                inventory_client.player_reactions_ready() && inventory_client.quest_events_ready() &&
                inventory_client.quest_tracking_ready() && inventory_client.quest_reactions_ready());
            if (mode==7) CHECK(!inventory_client.actor_events_ready() && !inventory_client.player_events_ready() &&
                !inventory_client.quest_events_ready() && !inventory_client.quest_tracking_ready());
            synth::protocol_native::ActorState state;
            state.actor={"0x00000014","Fallout4.esm","save:one","Player"};
            state.base_form_id="0x00000007";state.base_origin_plugin="Fallout4.esm";
            state.inventory.assign(512,{"0x00004822","Fallout4.esm","Pistol",1,43,50,4.2,false});
            state.inventory_observation="complete";
            const auto publish_inventory=[&](const std::vector<synth::protocol_native::ActorState>& states) {
                return inventory_client.publish_context(0,"save:one",state.actor,std::nullopt,{},states,{}, {}, {}, {});
            };
            CHECK(publish_inventory({state}).status==RequestStatus::complete);
            auto encoded=synth::json::parse(wire.requests.back().body);
            const auto& observed=synth::protocol_native::required(synth::protocol_native::required(encoded,"payload"),"actor_states").as_array().front();
            CHECK(synth::protocol_native::required(observed,"inventory").as_array().size()==(extended?512:32));
            CHECK(synth::protocol_native::text(observed,"inventory_observation")== (extended?"complete":"partial"));
            CHECK(state.inventory.size()==512 && state.inventory_observation=="complete");
            if (!extended) continue;
            for(auto& item:state.inventory) {item.display_name=std::string(255,'"');item.origin_plugin=std::string(251,'x')+".esp";}
            CHECK(publish_inventory({state,state,state,state}).status==RequestStatus::complete);
            CHECK(wire.requests.back().body.size()<=1'048'576);
            encoded=synth::json::parse(wire.requests.back().body);
            bool trimmed=false;
            for(const auto& inventory_actor:synth::protocol_native::required(synth::protocol_native::required(encoded,"payload"),"actor_states").as_array()) {
                const auto size=synth::protocol_native::required(inventory_actor,"inventory").as_array().size();
                CHECK(synth::protocol_native::text(inventory_actor,"inventory_observation")== (size<512?"partial":"complete"));
                trimmed=trimmed || size<512;
            }
            CHECK(trimmed && state.inventory.size()==512);
            wire.inventory_support=false;
            CHECK(inventory_client.initialize(2,false).status==RequestStatus::complete && !inventory_client.extended_inventory_ready());
            CHECK(publish_inventory({state}).status==RequestStatus::complete);
            encoded=synth::json::parse(wire.requests.back().body);
            CHECK(synth::protocol_native::required(synth::protocol_native::required(synth::protocol_native::required(encoded,"payload"),"actor_states").as_array().front(),"inventory").as_array().size()==32);
        }
        // Inventory receipts require the exact completed init ACK, not just a requested wire capability.
        for (int mode=0;mode<15;++mode) {
            BindingEchoTransport wire;wire.action_inventory_support=mode!=0 && mode!=13;
            wire.inventory_support=mode==13;
            SessionOptions options{.runtime_session_id="action-inventory:ready",.runtime_variant=mode==4?"vr":"flat",
                .client_version="test",.runtime_version="1.11.240",
                .capabilities={"action.inventory_observation","dialogue.turn_ownership","context.inventory_512",
                    "context.actor_events","context.player_events","dialogue.player_reactions",
                    "context.quest_events","context.quest_tracking","dialogue.quest_reactions"},.protocol_version=2};
            if(mode==2) options.capabilities.erase(options.capabilities.begin());
            if(mode==3) options.capabilities.erase(options.capabilities.begin()+1);
            if(mode==12) options.capabilities={"action.inventory_observation","dialogue.turn_ownership"};
            if(mode==14) options.protocol_version=1;
            if(mode==5) wire.fault=BindingEchoTransport::Fault::truncated;
            if(mode==6) wire.fault=BindingEchoTransport::Fault::sequence;
            if(mode==7) wire.fault=BindingEchoTransport::Fault::duplicate_id;
            if(mode==8) wire.fault=BindingEchoTransport::Fault::timeout;
            if(mode==9) wire.fault=BindingEchoTransport::Fault::failed;
            if(mode==10) wire.fault=BindingEchoTransport::Fault::http_error;
            if(mode==11) wire.fault=BindingEchoTransport::Fault::version;
            SynthClient candidate{wire,options};CHECK(!candidate.action_inventory_ready());
            const bool legacy_inventory=mode==2 || mode==3 || mode==14;
            CHECK(candidate.equipment_actions_ready()==legacy_inventory);
            const auto inventory_init=candidate.initialize(1,false);
            const bool ready=mode==1 || mode==12;
            CHECK(candidate.action_inventory_ready()==ready);
            CHECK(candidate.equipment_actions_ready()==(legacy_inventory || ready));
            if(mode==1 || mode==13) CHECK(candidate.extended_inventory_ready() && candidate.actor_events_ready() &&
                candidate.player_events_ready() && candidate.player_reactions_ready() && candidate.quest_events_ready() &&
                candidate.quest_tracking_ready() && candidate.quest_reactions_ready());
            if(mode==12) CHECK(!candidate.extended_inventory_ready() && !candidate.actor_events_ready() &&
                !candidate.player_events_ready() && !candidate.quest_events_ready());
            if(inventory_init.status!=RequestStatus::complete) continue;
            const auto inventory_context=candidate.publish_context(0,"save:one",player,actor,{}, {},std::nullopt,{}, {},{});
            CHECK(inventory_context.status==RequestStatus::complete);
            const auto sent=wire.requests.size();bool inventory_rejected=false;
            const synth::protocol_native::ActionInventoryObservation observed{actor,{},"unavailable"};
            try { (void)candidate.send_action_result("inventory:action","inventory:key","failed","unavailable",{},
                inventory_context.context_binding,observed); }
            catch(const std::invalid_argument&) { inventory_rejected=true; }
            CHECK(inventory_rejected==!ready);CHECK(wire.requests.size()==sent+(ready?1:0));
            if(!ready) continue;
            CHECK(candidate.hard_halt().status==RequestStatus::complete && !candidate.action_inventory_ready());
            CHECK(!candidate.equipment_actions_ready());
            wire.action_inventory_support=false;
            wire.before_reply=[&]{CHECK(!candidate.action_inventory_ready() && !candidate.equipment_actions_ready());};
            CHECK(candidate.initialize(2,false).status==RequestStatus::complete && !candidate.action_inventory_ready());
            CHECK(!candidate.equipment_actions_ready());
            wire.before_reply={};
            wire.action_inventory_support=true;wire.fault=BindingEchoTransport::Fault::failed;
            CHECK(candidate.initialize(3,false).status!=RequestStatus::complete && !candidate.action_inventory_ready());
            CHECK(!candidate.equipment_actions_ready());
            wire.fault=BindingEchoTransport::Fault::none;
            CHECK(candidate.initialize(4,false).status==RequestStatus::complete && candidate.action_inventory_ready());
            CHECK(candidate.equipment_actions_ready());
        }
        // Consumption is distinct from equipment readiness, including older ACK, partial stream, halt and reinit.
        for (int mode=0;mode<17;++mode) {
            BindingEchoTransport wire;wire.consumption_support=mode!=0 && mode!=2;wire.action_inventory_support=mode==2;
            SessionOptions options{.runtime_session_id="consume:ready",.runtime_variant=mode==7?"vr":"flat",
                .client_version="test",.runtime_version="1.11.240",
                .capabilities={"action.consume_inventory","action.consume","action.inventory_observation","dialogue.turn_ownership",
                    "context.inventory_512","context.actor_events","context.quest_events"},.protocol_version=2};
            if(mode>=3 && mode<=6) options.capabilities.erase(options.capabilities.begin()+mode-3);
            if(mode==8) options.protocol_version=1;
            if(mode==9) wire.fault=BindingEchoTransport::Fault::truncated;
            if(mode==10) wire.fault=BindingEchoTransport::Fault::sequence;
            if(mode==11) wire.fault=BindingEchoTransport::Fault::timeout;
            if(mode==12) wire.fault=BindingEchoTransport::Fault::failed;
            if(mode==13) wire.fault=BindingEchoTransport::Fault::http_error;
            if(mode==14) wire.fault=BindingEchoTransport::Fault::version;
            if(mode==15) wire.fault=BindingEchoTransport::Fault::duplicate_id;
            if(mode==16) options.capabilities.resize(4);
            SynthClient candidate{wire,options};CHECK(!candidate.consumption_ready());
            const auto consume_initialized=candidate.initialize(1,false);
            const bool ready=mode==1 || mode==16;
            CHECK(candidate.consumption_ready()==ready);
            CHECK(candidate.action_inventory_ready()==(ready || mode==2));
            if(mode==1) CHECK(candidate.extended_inventory_ready() && candidate.actor_events_ready() && candidate.quest_events_ready());
            if(mode==16) CHECK(!candidate.extended_inventory_ready() && !candidate.actor_events_ready() && !candidate.quest_events_ready());
            if(!ready) continue;
            CHECK(consume_initialized.status==RequestStatus::complete);
            const auto context=candidate.publish_context(0,"save:one",player,actor,{}, {},std::nullopt,{}, {},{});
            const synth::protocol_native::ActionInventoryObservation observed{actor,{},"complete"};
            CHECK(candidate.send_action_result("consume:action","consume:key","succeeded","one consumed",{},context.context_binding,observed).status==RequestStatus::complete);
            CHECK(candidate.hard_halt().status==RequestStatus::complete && !candidate.consumption_ready());
            wire.consumption_support=false;wire.action_inventory_support=true;
            wire.before_reply=[&]{CHECK(!candidate.consumption_ready() && !candidate.action_inventory_ready());};
            CHECK(candidate.initialize(2,false).status==RequestStatus::complete && !candidate.consumption_ready() && candidate.action_inventory_ready());
            wire.before_reply={};wire.consumption_support=true;wire.fault=BindingEchoTransport::Fault::failed;
            CHECK(candidate.initialize(3,false).status!=RequestStatus::complete && !candidate.consumption_ready());
            wire.fault=BindingEchoTransport::Fault::none;
            CHECK(candidate.initialize(4,false).status==RequestStatus::complete && candidate.consumption_ready());
        }
        // Paired transfers require their own complete acknowledgement and cannot inherit Consume readiness.
        for (int mode=0;mode<19;++mode) {
            BindingEchoTransport wire;wire.transfer_support=mode!=0 && mode!=2;wire.action_inventory_support=mode==2;
            SessionOptions options{.runtime_session_id="transfer:ready",.runtime_variant=mode==7?"vr":"flat",
                .client_version="test",.runtime_version="1.11.240",
                .capabilities={"action.transfer_inventory","action.give_item_to","action.inventory_observation","dialogue.turn_ownership",
                    "context.inventory_512","context.actor_events","context.quest_events"},.protocol_version=2};
            if(mode>=3 && mode<=6) options.capabilities.erase(options.capabilities.begin()+mode-3);
            if(mode==8) options.protocol_version=1;
            if(mode==9) wire.fault=BindingEchoTransport::Fault::truncated;
            if(mode==10) wire.fault=BindingEchoTransport::Fault::sequence;
            if(mode==11) wire.fault=BindingEchoTransport::Fault::timeout;
            if(mode==12) wire.fault=BindingEchoTransport::Fault::failed;
            if(mode==13) wire.fault=BindingEchoTransport::Fault::http_error;
            if(mode==14) wire.fault=BindingEchoTransport::Fault::version;
            if(mode==15) wire.fault=BindingEchoTransport::Fault::duplicate_id;
            if(mode==16) options.capabilities.resize(4);
            if(mode==17) {wire.transfer_support=false;wire.consumption_support=true;options.capabilities.push_back("action.consume_inventory");options.capabilities.push_back("action.consume");}
            if(mode==18) {options.capabilities.push_back("action.consume_inventory");options.capabilities.push_back("action.consume");}
            SynthClient candidate{wire,options};CHECK(!candidate.transfer_ready());
            const auto transfer_initialized=candidate.initialize(1,false);
            const bool ready=mode==1 || mode==16 || mode==18;
            CHECK(candidate.transfer_ready()==ready);
            CHECK(candidate.consumption_ready()==(mode==17 || mode==18));
            CHECK(candidate.action_inventory_ready()==(ready || mode==2 || mode==17));
            if(mode==1) CHECK(candidate.extended_inventory_ready() && candidate.actor_events_ready() && candidate.quest_events_ready());
            if(mode==16) CHECK(!candidate.extended_inventory_ready() && !candidate.actor_events_ready() && !candidate.quest_events_ready());
            if(!ready) {
                CHECK(!candidate.transfer_ready());
                if(transfer_initialized.status==RequestStatus::complete) {
                    const auto binding=candidate.publish_context(0,"save:one",player,actor,{}, {},std::nullopt,{}, {},{}).context_binding;
                    const auto before=wire.requests.size();bool refused=false;
                    try{(void)candidate.send_action_result("transfer:action","transfer:key","failed","unknown",{},binding,std::nullopt,
                        synth::protocol_native::ActionTransferObservation{{actor,{},"unavailable"},{player,{},"unavailable"}});}
                    catch(const std::invalid_argument&){refused=true;}
                    CHECK(refused && wire.requests.size()==before);
                }
                continue;
            }
            CHECK(transfer_initialized.status==RequestStatus::complete);
            const auto context=candidate.publish_context(0,"save:one",player,actor,{}, {},std::nullopt,{}, {},{});
            const synth::protocol_native::ActionTransferObservation observed{{actor,{},"complete"},{player,{},"unavailable"}};
            CHECK(candidate.send_action_result("transfer:action","transfer:key","failed","unknown recipient",{},context.context_binding,std::nullopt,observed).status==RequestStatus::complete);
            const auto transfer_receipt=synth::protocol_native::required(synth::protocol_native::required(synth::json::parse(wire.requests.back().body),"payload"),"result");
            CHECK(synth::protocol_native::text(transfer_receipt,"schema")=="synth.action.transfer-result.v2");
            CHECK(synth::protocol_native::text(synth::protocol_native::required(transfer_receipt,"inventory"),"observation")=="complete");
            CHECK(synth::protocol_native::text(synth::protocol_native::required(transfer_receipt,"recipient_inventory"),"observation")=="unavailable");
            const auto requests_before=wire.requests.size();bool mixed_refused=false;
            try{(void)candidate.send_action_result("transfer:action","transfer:key","failed","mixed receipt",{},context.context_binding,observed.donor,observed);}
            catch(const std::invalid_argument&){mixed_refused=true;}
            CHECK(mixed_refused && wire.requests.size()==requests_before);
            CHECK(candidate.hard_halt().status==RequestStatus::complete && !candidate.transfer_ready());
            wire.transfer_support=false;wire.action_inventory_support=true;
            wire.before_reply=[&]{CHECK(!candidate.transfer_ready() && !candidate.action_inventory_ready());};
            CHECK(candidate.initialize(2,false).status==RequestStatus::complete && !candidate.transfer_ready() && candidate.action_inventory_ready());
            wire.before_reply={};wire.transfer_support=true;wire.fault=BindingEchoTransport::Fault::failed;
            CHECK(candidate.initialize(3,false).status!=RequestStatus::complete && !candidate.transfer_ready());
            wire.fault=BindingEchoTransport::Fault::none;
            CHECK(candidate.initialize(4,false).status==RequestStatus::complete && candidate.transfer_ready());
        }
        // Currency acknowledgement is separately correlated; every lower ACK and malformed owner fails closed.
        for (int mode=0;mode<25;++mode) {
            BindingEchoTransport wire;wire.caps_support=mode==0 || mode>=5;
            wire.transfer_support=mode==1;wire.consumption_support=mode==2;wire.action_inventory_support=mode==3;
            SessionOptions options{.runtime_session_id="caps:ready",.runtime_variant=mode==11?"vr":"flat",
                .client_version="test",.runtime_version="1.11.240",
                .capabilities={"action.caps_inventory","action.give_caps_to","action.transfer_inventory","action.give_item_to",
                    "action.inventory_observation","dialogue.turn_ownership"},.protocol_version=mode==12?1ULL:2ULL};
            if(mode>=5 && mode<=10) options.capabilities.erase(options.capabilities.begin()+mode-5);
            if(mode>=13 && mode<=19) wire.fault=std::array{BindingEchoTransport::Fault::truncated,BindingEchoTransport::Fault::sequence,
                BindingEchoTransport::Fault::timeout,BindingEchoTransport::Fault::failed,BindingEchoTransport::Fault::http_error,
                BindingEchoTransport::Fault::version,BindingEchoTransport::Fault::duplicate_id}[mode-13];
            if(mode==20) for(const auto* cap:{"action.consume","action.consume_inventory","context.inventory_512","context.actor_events"})
                options.capabilities.emplace_back(cap);
            if(mode>=21) wire.fault=std::array{BindingEchoTransport::Fault::request,BindingEchoTransport::Fault::turn,
                BindingEchoTransport::Fault::generation,BindingEchoTransport::Fault::variant}[mode-21];
            SynthClient candidate{wire,options};CHECK(!candidate.caps_ready());
            const auto caps_initialized=candidate.initialize(1,false);const bool ready=mode==0 || mode==20;
            CHECK(candidate.caps_ready()==ready);
            CHECK(candidate.transfer_ready()==(ready || mode==1));
            CHECK(candidate.consumption_ready()==(mode==20));
            if(mode==20) CHECK(candidate.extended_inventory_ready() && candidate.actor_events_ready());
            if(caps_initialized.status!=RequestStatus::complete) continue;
            const auto scene=candidate.publish_context(0,"save:one",player,actor,{}, {},std::nullopt,{}, {},{});
            const synth::protocol_native::ActionTransferObservation observed{{actor,{},"complete"},{player,{},"unavailable"}};
            const auto before=wire.requests.size();bool refused=false;
            try{(void)candidate.send_action_result("caps:action","caps:key","failed","unknown",{},scene.context_binding,std::nullopt,observed,true);}
            catch(const std::invalid_argument&){refused=true;}
            CHECK(refused==!ready);CHECK(wire.requests.size()==before+(ready?1:0));
            if(!ready)continue;
            const auto caps_result=synth::protocol_native::required(synth::protocol_native::required(synth::json::parse(wire.requests.back().body),"payload"),"result");
            CHECK(synth::protocol_native::text(caps_result,"schema")=="synth.action.transfer-result.v2");
            bool missing=false;const auto sent=wire.requests.size();
            try{(void)candidate.send_action_result("caps:missing","caps:missing-key","failed","no pair",{},scene.context_binding,std::nullopt,std::nullopt,true);}
            catch(const std::invalid_argument&){missing=true;}
            CHECK(missing && wire.requests.size()==sent);
            CHECK(candidate.hard_halt().status==RequestStatus::complete && !candidate.caps_ready());
            wire.caps_support=false;wire.transfer_support=true;
            wire.before_reply=[&]{CHECK(!candidate.caps_ready() && !candidate.transfer_ready());};
            CHECK(candidate.initialize(2,false).status==RequestStatus::complete && !candidate.caps_ready() && candidate.transfer_ready());
            wire.before_reply={};wire.caps_support=true;wire.fault=BindingEchoTransport::Fault::failed;
            CHECK(candidate.initialize(3,false).status!=RequestStatus::complete && !candidate.caps_ready());
            wire.fault=BindingEchoTransport::Fault::none;
            CHECK(candidate.initialize(4,false).status==RequestStatus::complete && candidate.caps_ready());
        }
        // Player payments cannot borrow NPC-giving-caps readiness, including after a save-generation change.
        for (int mode=0;mode<28;++mode) {
            BindingEchoTransport wire;wire.player_caps_support=mode==0 || mode>=6;
            wire.caps_support=mode==1;wire.transfer_support=mode==2;
            wire.consumption_support=mode==3;wire.action_inventory_support=mode==4;
            SessionOptions options{.runtime_session_id="player-caps:ready",.runtime_variant=mode==14?"vr":"flat",
                .client_version="test",.runtime_version="1.11.240",
                .capabilities={"action.player_caps_inventory","action.take_caps_from_player","action.caps_inventory","action.give_caps_to",
                    "action.transfer_inventory","action.give_item_to","action.inventory_observation","dialogue.turn_ownership"},
                .protocol_version=mode==15?1ULL:2ULL};
            if(mode>=6 && mode<=13) options.capabilities.erase(options.capabilities.begin()+mode-6);
            if(mode>=16 && mode<=22) wire.fault=std::array{BindingEchoTransport::Fault::truncated,BindingEchoTransport::Fault::sequence,
                BindingEchoTransport::Fault::timeout,BindingEchoTransport::Fault::failed,BindingEchoTransport::Fault::http_error,
                BindingEchoTransport::Fault::version,BindingEchoTransport::Fault::duplicate_id}[mode-16];
            if(mode==23) for(const auto* cap:{"action.consume","action.consume_inventory","context.inventory_512","context.actor_events"})
                options.capabilities.emplace_back(cap);
            if(mode>=24) wire.fault=std::array{BindingEchoTransport::Fault::request,BindingEchoTransport::Fault::turn,
                BindingEchoTransport::Fault::generation,BindingEchoTransport::Fault::variant}[mode-24];
            SynthClient candidate{wire,options};CHECK(!candidate.player_caps_ready());
            const auto payment_initialized=candidate.initialize(1,false);const bool ready=mode==0 || mode==23;
            CHECK(candidate.player_caps_ready()==ready);
            CHECK(candidate.caps_ready()==(ready || mode==1));
            CHECK(candidate.transfer_ready()==(ready || mode==1 || mode==2));
            CHECK(candidate.consumption_ready()==(mode==23));
            if(mode==23) CHECK(candidate.extended_inventory_ready() && candidate.actor_events_ready());
            if(payment_initialized.status!=RequestStatus::complete) continue;
            const auto scene=candidate.publish_context(0,"save:one",player,actor,{}, {},std::nullopt,{}, {},{});
            const synth::protocol_native::ActionTransferObservation observed{{player,{},"unavailable"},{actor,{},"complete"}};
            const auto before=wire.requests.size();bool refused=false;
            try{(void)candidate.send_action_result("payment:action","payment:key","failed","unknown",{},scene.context_binding,std::nullopt,observed,true,true);}
            catch(const std::invalid_argument&){refused=true;}
            CHECK(refused==!ready);CHECK(wire.requests.size()==before+(ready?1:0));
            if(!ready)continue;
            const auto payment_result=synth::protocol_native::required(synth::protocol_native::required(synth::json::parse(wire.requests.back().body),"payload"),"result");
            CHECK(synth::protocol_native::text(payment_result,"schema")=="synth.action.transfer-result.v2");
            const auto donor=synth::protocol_native::required(synth::protocol_native::required(payment_result,"inventory"),"actor");
            const auto receiver=synth::protocol_native::required(synth::protocol_native::required(payment_result,"recipient_inventory"),"actor");
            CHECK(synth::protocol_native::text(donor,"form_id")==player.form_id);
            CHECK(synth::protocol_native::text(receiver,"form_id")==actor.form_id);
            for(bool missing_pair:{false,true}) {
                bool missing=false;const auto sent=wire.requests.size();
                try{(void)candidate.send_action_result("payment:missing","payment:missing-key","failed","missing evidence",{},scene.context_binding,
                    std::nullopt,missing_pair?std::nullopt:std::optional{observed},missing_pair,true);}
                catch(const std::invalid_argument&){missing=true;}
                CHECK(missing && wire.requests.size()==sent);
            }
            CHECK(candidate.hard_halt().status==RequestStatus::complete && !candidate.player_caps_ready());
            wire.player_caps_support=false;wire.caps_support=true;
            wire.before_reply=[&]{CHECK(!candidate.player_caps_ready() && !candidate.caps_ready());};
            CHECK(candidate.initialize(2,false).status==RequestStatus::complete && !candidate.player_caps_ready() && candidate.caps_ready());
            wire.before_reply={};wire.player_caps_support=true;wire.fault=BindingEchoTransport::Fault::failed;
            CHECK(candidate.initialize(3,false).status!=RequestStatus::complete && !candidate.player_caps_ready());
            wire.fault=BindingEchoTransport::Fault::none;
            CHECK(candidate.initialize(4,false).status==RequestStatus::complete && candidate.player_caps_ready());
        }
        std::cout << "fake server tests passed (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
