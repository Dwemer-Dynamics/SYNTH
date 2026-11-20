#pragma once

#include "json/json.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>

// Optional game RPC (protocol v2 capability rpc.game), carried on the wake-up channel's WebSocket. The server may ask the running
// game a small closed set of questions; the client answers only methods that were registered for a feature it really has and
// fails closed (method_unavailable) for everything else. Frames: protocol/v2/schemas/rpc-frame.schema.json. Nothing here touches
// game state: a method that needs it must be registered by code that owns a thread-safe way to read it.
namespace synth::transport {

inline constexpr std::size_t maximum_rpc_frame_bytes = 16'384;
inline constexpr std::string_view rpc_schema = "synth.rpc.v1";

// The closed method list of the schema; a request for anything else is not even parsed as a request.
inline constexpr std::string_view rpc_methods[] = {"ping", "session.info", "inventory.player", "world.nearby_items"};

[[nodiscard]] constexpr bool is_rpc_method(std::string_view method) noexcept {
    for (const auto known : rpc_methods) if (known == method) return true;
    return false;
}

// The outcome of one request: a result object, or a closed error code (never free text).
struct RpcResult final {
    bool ok{};
    json::Value result{json::Value::Object{}};
    std::string error;

    [[nodiscard]] static RpcResult success(json::Value value) { return RpcResult{true, std::move(value), {}}; }
    [[nodiscard]] static RpcResult failure(std::string code) { return RpcResult{false, json::Value::Object{}, std::move(code)}; }
};

using RpcHandler = std::function<RpcResult(std::string_view method, const json::Value& params)>;

// What the channel needs to bind its connection to the session and answer requests.
struct RpcBinding final {
    std::string session_id;
    std::string token;
    RpcHandler handler;

    [[nodiscard]] bool operator==(const RpcBinding& other) const noexcept {
        return session_id == other.session_id && token == other.token;
    }
};

struct RpcRequest final {
    std::uint32_t id{};
    std::string method;
    json::Value params{json::Value::Object{}};
    std::uint32_t deadline_ms{};
};

struct RpcHelloAck final {};

[[nodiscard]] inline bool valid_rpc_token(std::string_view token) noexcept {
    if (token.size() != 64) return false;
    for (const char c : token) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

[[nodiscard]] inline std::string rpc_hello_frame(const RpcBinding& binding) {
    return json::write(json::Value{json::Value::Object{{"schema", std::string{rpc_schema}},
                                                       {"type", "hello"},
                                                       {"session_id", binding.session_id},
                                                       {"token", binding.token}}});
}

[[nodiscard]] inline std::string rpc_response_frame(std::uint32_t id, const RpcResult& outcome) {
    json::Value::Object frame{{"schema", std::string{rpc_schema}}, {"type", "response"}, {"id", static_cast<std::uint64_t>(id)}, {"ok", outcome.ok}};
    if (outcome.ok) frame.emplace_back("result", outcome.result);
    else frame.emplace_back("error", outcome.error);
    return json::write(json::Value{std::move(frame)});
}

// Strict reading of a server frame: a hello_ack or a request. Anything else (including a hint frame) is nullopt.
[[nodiscard]] inline std::optional<std::variant<RpcHelloAck, RpcRequest>> parse_rpc_frame(std::string_view frame) noexcept {
    if (frame.empty() || frame.size() > maximum_rpc_frame_bytes) return std::nullopt;
    try {
        const auto value = json::parse(frame);
        if (!value.is_object()) return std::nullopt;
        const auto* schema = value.find("schema");
        const auto* type = value.find("type");
        if (!schema || !schema->is_string() || schema->as_string() != rpc_schema || !type || !type->is_string()) return std::nullopt;
        const auto members = value.as_object().size();
        if (type->as_string() == "hello_ack") {
            if (members != 2) return std::nullopt;
            return RpcHelloAck{};
        }
        if (type->as_string() != "request" || members != 6) return std::nullopt;
        const auto* id = value.find("id");
        const auto* method = value.find("method");
        const auto* params = value.find("params");
        const auto* deadline = value.find("deadline_ms");
        if (!id || !method || !params || !deadline || !method->is_string() || !params->is_object()) return std::nullopt;
        const auto* id_number = std::get_if<std::uint64_t>(&id->storage());
        const auto* deadline_number = std::get_if<std::uint64_t>(&deadline->storage());
        if (!id_number || *id_number < 1 || *id_number > 4'294'967'295ULL) return std::nullopt;
        if (!deadline_number || *deadline_number < 100 || *deadline_number > 10'000) return std::nullopt;
        if (!is_rpc_method(method->as_string()) || params->as_object().size() > 8) return std::nullopt;
        return RpcRequest{static_cast<std::uint32_t>(*id_number), method->as_string(), *params,
                          static_cast<std::uint32_t>(*deadline_number)};
    } catch (...) {
        return std::nullopt;
    }
}

// The methods this client really answers. A method is answerable only if code that can read its game data registered it; any
// other listed method is method_unavailable. A handler that throws is `failed`, never a crash of the channel thread.
class RpcMethodTable final {
public:
    using Method = std::function<RpcResult(const json::Value& params)>;

    void add(std::string name, Method method) {
        if (is_rpc_method(name) && method) methods_[std::move(name)] = std::move(method);
    }

    [[nodiscard]] RpcResult handle(std::string_view method, const json::Value& params) const {
        const auto found = methods_.find(std::string{method});
        if (found == methods_.end()) return RpcResult::failure("method_unavailable");
        try {
            return found->second(params);
        } catch (...) {
            return RpcResult::failure("failed");
        }
    }

    [[nodiscard]] RpcHandler handler() const {
        return [table = *this](std::string_view method, const json::Value& params) { return table.handle(method, params); };
    }

private:
    std::unordered_map<std::string, Method> methods_;
};

struct SessionFacts final {
    std::string client_version;
    std::string runtime_variant;
    std::string runtime_version;
};

// ping and session.info only touch immutable facts, so they are safe on the channel thread. Inventory and nearby-item queries
// read the game and are not registered here (see docs/GAME-RPC.md): they answer method_unavailable until a game-thread provider
// adds them.
[[nodiscard]] inline RpcMethodTable basic_rpc_methods(SessionFacts facts) {
    RpcMethodTable table;
    table.add("ping", [](const json::Value& params) {
        if (!params.as_object().empty()) return RpcResult::failure("invalid_params");
        return RpcResult::success(json::Value::Object{{"pong", true}});
    });
    table.add("session.info", [facts = std::move(facts)](const json::Value& params) {
        if (!params.as_object().empty()) return RpcResult::failure("invalid_params");
        return RpcResult::success(json::Value::Object{{"client_version", facts.client_version},
                                                      {"runtime_variant", facts.runtime_variant},
                                                      {"runtime_version", facts.runtime_version}});
    });
    return table;
}

}  // namespace synth::transport
