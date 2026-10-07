#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace synth::client {

enum class HttpMethod : unsigned char { get, post, put };

struct HttpRequest final {
    HttpMethod method{HttpMethod::get};
    std::string target;
    std::string content_type;
    std::string body;
    std::uint32_t timeout_ms{};
    std::string accept{"application/x-ndjson"};
    std::size_t maximum_response_bytes{4U * 1024U * 1024U};
    // Called on the sending worker, never the WinHTTP callback or game thread.
    std::function<bool(std::string_view)> response_chunk;
};

struct HttpResponse final {
    int status{};
    std::string content_type;
    std::string body;
};

enum class TransportFailure : unsigned char {
    none,
    timeout,
    cancelled,
    server_unavailable,
    protocol_error,
    response_too_large,
};

struct HttpResult final {
    TransportFailure failure{TransportFailure::none};
    HttpResponse response;
    [[nodiscard]] bool delivered() const noexcept { return failure == TransportFailure::none; }
};

// Predicates may be polled by a transport cancellation watcher: use thread-safe state.
using Cancelled = std::function<bool()>;

class IHttpTransport {
public:
    virtual ~IHttpTransport() = default;
    [[nodiscard]] virtual HttpResult send(const HttpRequest& request,
                                          const Cancelled& cancelled) = 0;
    virtual void cancel_all() noexcept = 0;
};

}  // namespace synth::client
