#pragma once

#include "client/winhttp_transport.hpp"
#include "transport/notify_wake.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace synth::client {

// Blocking WinHTTP WebSocket receive for the optional wake-up channel. It uses its own synchronous session, never the
// request transport's, so it cannot delay or be delayed by a protocol request. Plain ws:// on purpose: frames are
// content-free hints, and a refused or failed connection only means the client keeps polling.
class WinHttpNotifyConnection final : public transport::INotifyConnection {
public:
    static constexpr std::size_t maximum_message_bytes = 1024;

    WinHttpNotifyConnection(detail::InternetHandle session, detail::InternetHandle connection, HINTERNET socket) noexcept
        : session_{std::move(session)}, connection_{std::move(connection)}, socket_{socket} {}

    ~WinHttpNotifyConnection() override {
        abort();
        // The thread that owns this object is the only receiver, so no receive can still be using a handle here.
    }

    [[nodiscard]] transport::ReceiveResult receive(std::string& frame) override {
        frame.clear();
        std::array<char, 256> buffer;
        for (;;) {
            const HINTERNET socket = socket_.load(std::memory_order_acquire);
            if (socket == nullptr) return transport::ReceiveResult::closed;
            DWORD read{};
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            const auto error = WinHttpWebSocketReceive(socket, buffer.data(), static_cast<DWORD>(buffer.size()), &read, &type);
            if (error != ERROR_SUCCESS) return transport::ReceiveResult::failed;
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return transport::ReceiveResult::closed;
            if (type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE && type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
                return transport::ReceiveResult::failed; // The channel never sends binary frames.
            if (frame.size() + read > maximum_message_bytes) return transport::ReceiveResult::failed;
            frame.append(buffer.data(), read);
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) return transport::ReceiveResult::frame;
        }
    }

    // Game RPC replies: one UTF-8 text frame, sent by the thread that owns the receive loop. Anything unusual reports false.
    [[nodiscard]] bool send(std::string_view frame) override {
        if (frame.empty() || frame.size() > transport::maximum_rpc_frame_bytes) return false;
        const HINTERNET socket = socket_.load(std::memory_order_acquire);
        if (socket == nullptr) return false;
        return WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                    const_cast<char*>(frame.data()), static_cast<DWORD>(frame.size())) == ERROR_SUCCESS;
    }

    // Closing the socket handle makes a receive blocked on another thread return with an error.
    void abort() noexcept override {
        if (const HINTERNET socket = socket_.exchange(nullptr, std::memory_order_acq_rel)) WinHttpCloseHandle(socket);
    }

private:
    detail::InternetHandle session_;
    detail::InternetHandle connection_;
    std::atomic<HINTERNET> socket_;
};

class WinHttpNotifyTransport final : public transport::INotifyTransport {
public:
    explicit WinHttpNotifyTransport(std::uint32_t connect_timeout_ms = 2'000) : connect_timeout_ms_{connect_timeout_ms} {}

    [[nodiscard]] std::unique_ptr<transport::INotifyConnection> connect(std::string_view host, std::uint16_t port) override {
        if (host.empty() || port == 0) return nullptr;
        try {
            const auto wide_host = detail::utf8_to_wide(host);
            detail::InternetHandle session{WinHttpOpen(L"SYNTH/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                                       WINHTTP_NO_PROXY_BYPASS, 0)};
            if (!session) return nullptr;
            // Bounded connect and send; the receive timeout stays unlimited because the channel is quiet between hints.
            WinHttpSetTimeouts(session.get(), static_cast<int>(connect_timeout_ms_), static_cast<int>(connect_timeout_ms_),
                               static_cast<int>(connect_timeout_ms_), 0);
            detail::InternetHandle connection{WinHttpConnect(session.get(), wide_host.c_str(), port, 0)};
            if (!connection) return nullptr;
            detail::InternetHandle request{WinHttpOpenRequest(connection.get(), L"GET", L"/", nullptr, WINHTTP_NO_REFERER,
                                                              WINHTTP_DEFAULT_ACCEPT_TYPES, 0)};
            if (!request) return nullptr;
            if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) return nullptr;
            if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
                !WinHttpReceiveResponse(request.get(), nullptr))
                return nullptr;
            const HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request.get(), 0);
            if (socket == nullptr) return nullptr;
            return std::make_unique<WinHttpNotifyConnection>(std::move(session), std::move(connection), socket);
        } catch (...) {
            return nullptr;
        }
    }

private:
    std::uint32_t connect_timeout_ms_;
};

}  // namespace synth::client
