#pragma once

#include "json/json.hpp"
#include "transport/game_rpc.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

// Optional wake-up channel (protocol v2 capability notify.wake). A frame is a content-free hint that the server's
// stored speech state changed; it only shortens a wait. Polling remains the default and the fallback: nothing here
// is required for correctness, and every failure path leaves the client polling exactly as it did without it.
namespace synth::transport {

// Shared by the channel thread (writer) and the speech poll loop (waiter).
class WakeSignal final {
public:
    using Clock = std::chrono::steady_clock;

    [[nodiscard]] std::uint64_t epoch() const {
        std::scoped_lock lock{mutex_};
        return epoch_;
    }

    void notify() {
        {
            std::scoped_lock lock{mutex_};
            ++epoch_;
        }
        changed_.notify_all();
    }

    // Waits until a notification newer than `seen` arrives or the deadline passes. Read epoch() before the work that
    // the wait follows, so a hint that lands in between is never lost.
    [[nodiscard]] bool wait_until(Clock::time_point deadline, std::uint64_t seen) {
        std::unique_lock lock{mutex_};
        return changed_.wait_until(lock, deadline, [&] { return epoch_ != seen; });
    }

    void set_connected(bool value) noexcept { connected_.store(value, std::memory_order_release); }
    [[nodiscard]] bool connected() const noexcept { return connected_.load(std::memory_order_acquire); }

    // How long a poll loop sleeps between checks: short on its own, longer while the channel is live and will cut the
    // wait short. A dropped channel returns the loop to the short interval at once.
    [[nodiscard]] std::chrono::milliseconds poll_interval() const noexcept {
        return connected() ? std::chrono::milliseconds{250} : std::chrono::milliseconds{100};
    }

    // The same for a queued message: a live channel pushes `message_available`, so the loop only keeps a slow safety poll
    // beside it; a dropped channel returns the loop to the short interval at once (the poll fallback).
    [[nodiscard]] std::chrono::milliseconds message_poll_interval() const noexcept {
        return connected() ? std::chrono::milliseconds{1'000} : std::chrono::milliseconds{100};
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t epoch_{};
    std::atomic_bool connected_{};
};

enum class HintKind : unsigned char { welcome, wake, message_available };

struct Hint final {
    HintKind kind{HintKind::welcome};
    std::uint64_t message_id{}; // Only for message_available: the queued message to fetch.
};

// Strict reading of protocol/v2/schemas/notify-hint.schema.json; anything else is ignored by the caller.
[[nodiscard]] inline std::optional<Hint> parse_notify_frame(std::string_view frame) noexcept {
    constexpr std::size_t maximum_frame_bytes = 256;
    constexpr std::uint64_t maximum_message_id = 9'007'199'254'740'991ULL;
    if (frame.empty() || frame.size() > maximum_frame_bytes) return std::nullopt;
    try {
        const auto value = json::parse(frame);
        if (!value.is_object()) return std::nullopt;
        const auto* schema = value.find("schema");
        const auto* type = value.find("type");
        const auto* topic = value.find("topic");
        const auto* message_id = value.find("message_id");
        if (!schema || !schema->is_string() || schema->as_string() != "synth.notify.hint.v1") return std::nullopt;
        if (!type || !type->is_string()) return std::nullopt;
        const auto members = value.as_object().size();
        if (type->as_string() == "welcome") {
            if (members != 2 || topic || message_id) return std::nullopt;
            return Hint{HintKind::welcome, 0};
        }
        if (type->as_string() == "wake") {
            if (members != 3 || message_id || !topic || !topic->is_string() || topic->as_string() != "speech") return std::nullopt;
            return Hint{HintKind::wake, 0};
        }
        if (type->as_string() == "message_available") {
            if (members != 3 || topic || !message_id) return std::nullopt;
            std::uint64_t id{};
            if (const auto* unsigned_id = std::get_if<std::uint64_t>(&message_id->storage())) id = *unsigned_id;
            else if (const auto* signed_id = std::get_if<std::int64_t>(&message_id->storage()); signed_id && *signed_id > 0)
                id = static_cast<std::uint64_t>(*signed_id);
            if (id == 0 || id > maximum_message_id) return std::nullopt;
            return Hint{HintKind::message_available, id};
        }
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<HintKind> parse_notify_hint(std::string_view frame) noexcept {
    const auto hint = parse_notify_frame(frame);
    if (!hint) return std::nullopt;
    return hint->kind;
}

// Delay before each reconnect: starts at `initial`, doubles per consecutive failure up to `maximum`, and restarts
// from `initial` only after a connection proved itself with a welcome frame.
class ReconnectBackoff final {
public:
    using Milliseconds = std::chrono::milliseconds;

    ReconnectBackoff(Milliseconds initial, Milliseconds maximum)
        : initial_{std::max(initial, Milliseconds{1})}, maximum_{std::max(maximum, initial_)}, next_{initial_} {}

    [[nodiscard]] Milliseconds next() noexcept {
        const auto current = next_;
        next_ = std::min(next_ * 2, maximum_);
        return current;
    }

    void reset() noexcept { next_ = initial_; }

private:
    Milliseconds initial_;
    Milliseconds maximum_;
    Milliseconds next_;
};

enum class ReceiveResult : unsigned char { frame, closed, failed };

class INotifyConnection {
public:
    virtual ~INotifyConnection() = default;
    // Blocks for one complete text frame; closed and failed both end the connection. abort() may be called from
    // another thread and must make a blocked receive return promptly.
    [[nodiscard]] virtual ReceiveResult receive(std::string& frame) = 0;
    // Sends one text frame from the thread that also receives (game RPC only; the wake-up channel itself sends nothing).
    // A transport that cannot send reports false, and the RPC part then stays unused.
    [[nodiscard]] virtual bool send(std::string_view /*frame*/) { return false; }
    virtual void abort() noexcept = 0;
};

class INotifyTransport {
public:
    virtual ~INotifyTransport() = default;
    // Returns nullptr when the channel cannot be reached; the attempt must be bounded by a short connect timeout.
    [[nodiscard]] virtual std::unique_ptr<INotifyConnection> connect(std::string_view host, std::uint16_t port) = 0;
};

struct NotifyTiming final {
    std::chrono::milliseconds initial_delay{1'000};
    std::chrono::milliseconds maximum_delay{60'000};
};

// One background thread that keeps a connection to the channel and turns each wake hint into WakeSignal::notify().
// It never touches game state and never blocks a caller: start() and stop() only manage the thread.
class NotifyChannel final {
public:
    using Log = std::function<void(std::string_view)>;

    NotifyChannel(INotifyTransport& transport, WakeSignal& signal, Log log = {}, NotifyTiming timing = {})
        : transport_{transport}, signal_{signal}, log_{std::move(log)}, timing_{timing} {}

    NotifyChannel(const NotifyChannel&) = delete;
    NotifyChannel& operator=(const NotifyChannel&) = delete;
    ~NotifyChannel() { shutdown(); }

    // Idempotent for the same endpoint; a different endpoint replaces the running connection. Ignored after shutdown().
    void start(std::string host, std::uint16_t port) {
        std::unique_lock lock{mutex_};
        if (closed_) return;
        if (thread_.joinable() && host_ == host && port_ == port) return;
        stop_locked(lock);
        host_ = std::move(host);
        port_ = port;
        stopping_ = false;
        thread_ = std::thread{[this, host = host_, port = port_] { run(host, port); }};
    }

    // Binds game RPC to this channel (capability rpc.game): after each welcome the channel sends `hello` with the session's token and
    // then answers requests through the binding's handler on the channel thread. A changed or removed binding drops a running
    // connection, so the caller starts the channel again afterwards; with no binding the channel never sends anything.
    void set_rpc(std::optional<RpcBinding> binding) {
        std::unique_lock lock{mutex_};
        if (closed_ || (binding.has_value() == rpc_.has_value() && (!binding || *binding == *rpc_))) return;
        rpc_ = std::move(binding);
        if (thread_.joinable()) stop_locked(lock);
    }

    // Returns the client to plain polling: the connection is dropped and the thread ends.
    void stop() {
        std::unique_lock lock{mutex_};
        stop_locked(lock);
    }

    // stop() that also refuses any later start(), for session teardown.
    void shutdown() {
        std::unique_lock lock{mutex_};
        closed_ = true;
        stop_locked(lock);
    }

private:
    void stop_locked(std::unique_lock<std::mutex>& lock) {
        stopping_ = true;
        if (connection_) connection_->abort();
        wake_.notify_all();
        if (thread_.joinable()) {
            auto thread = std::move(thread_);
            lock.unlock();
            thread.join();
            lock.lock();
        }
        signal_.set_connected(false);
    }

    [[nodiscard]] bool stopping() const {
        std::scoped_lock lock{mutex_};
        return stopping_;
    }

    [[nodiscard]] std::optional<RpcBinding> rpc_binding() const {
        std::scoped_lock lock{mutex_};
        return rpc_;
    }

    // One RPC frame from the server. False ends the connection (a send failed). A request before the hello_ack is ignored, and a
    // method the table does not hold answers method_unavailable, so an unknown or unready request never reaches game state.
    static bool handle_rpc_frame(INotifyConnection& connection, const RpcBinding& binding, std::string_view frame, bool& ready) {
        const auto parsed = parse_rpc_frame(frame);
        if (!parsed) return true;
        if (std::holds_alternative<RpcHelloAck>(*parsed)) {
            ready = true;
            return true;
        }
        if (!ready) return true;
        const auto& request = std::get<RpcRequest>(*parsed);
        RpcResult outcome = RpcResult::failure("method_unavailable");
        if (binding.handler) {
            try {
                outcome = binding.handler(request.method, request.params);
            } catch (...) {
                outcome = RpcResult::failure("failed");
            }
        }
        return connection.send(rpc_response_frame(request.id, outcome));
    }

    void say(std::string_view message) const {
        if (log_) log_(message);
    }

    // Waits out a reconnect delay; false when stop() cut it short.
    [[nodiscard]] bool sleep(std::chrono::milliseconds delay) {
        std::unique_lock lock{mutex_};
        return !wake_.wait_for(lock, delay, [&] { return stopping_; });
    }

    void run(const std::string& host, std::uint16_t port) {
        ReconnectBackoff backoff{timing_.initial_delay, timing_.maximum_delay};
        bool reported_unavailable = false;
        while (!stopping()) {
            std::unique_ptr<INotifyConnection> connection;
            try {
                connection = transport_.connect(host, port);
            } catch (...) {
                connection.reset();
            }
            bool welcomed = false;
            if (connection) {
                {
                    std::scoped_lock lock{mutex_};
                    if (stopping_) break;
                    connection_ = connection.get();
                }
                std::string frame;
                const auto rpc = rpc_binding();
                bool rpc_ready = false;
                while (connection->receive(frame) == ReceiveResult::frame) {
                    const auto hint = parse_notify_hint(frame);
                    if (!hint) {
                        // Not a hint: with game RPC bound it may be the hello_ack or a request; anything else is ignored.
                        if (rpc && welcomed && !handle_rpc_frame(*connection, *rpc, frame, rpc_ready)) break;
                        continue;
                    }
                    if (*hint == HintKind::welcome) {
                        if (!welcomed) {
                            welcomed = true;
                            backoff.reset();
                            signal_.set_connected(true);
                            reported_unavailable = false;
                            if (rpc && !connection->send(rpc_hello_frame(*rpc))) break;
                        }
                        // Anything that happened while the channel was down is found by one extra look.
                        signal_.notify();
                    } else if (welcomed) {
                        signal_.notify();
                    }
                }
                {
                    std::scoped_lock lock{mutex_};
                    connection_ = nullptr;
                }
                signal_.set_connected(false);
                connection.reset();
                if (welcomed && !stopping()) {
                    say("SYNTH: wake-up channel lost; polling continues.");
                    reported_unavailable = true;
                }
            } else if (!reported_unavailable) {
                say("SYNTH: wake-up channel unavailable; polling continues.");
                reported_unavailable = true;
            }
            // Always wait before the next attempt, even after a connection that ended at once: no busy loop.
            if (!sleep(backoff.next())) break;
        }
        signal_.set_connected(false);
    }

    INotifyTransport& transport_;
    WakeSignal& signal_;
    Log log_;
    NotifyTiming timing_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    INotifyConnection* connection_{};
    std::string host_;
    std::uint16_t port_{};
    bool stopping_{};
    bool closed_{};
    std::optional<RpcBinding> rpc_;
};

}  // namespace synth::transport
