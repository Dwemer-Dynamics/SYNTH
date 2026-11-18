#pragma once

#include "json/json.hpp"

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

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t epoch_{};
    std::atomic_bool connected_{};
};

enum class HintKind : unsigned char { welcome, wake };

// Strict reading of protocol/v2/schemas/notify-hint.schema.json; anything else is ignored by the caller.
[[nodiscard]] inline std::optional<HintKind> parse_notify_hint(std::string_view frame) noexcept {
    constexpr std::size_t maximum_frame_bytes = 256;
    if (frame.empty() || frame.size() > maximum_frame_bytes) return std::nullopt;
    try {
        const auto value = json::parse(frame);
        if (!value.is_object()) return std::nullopt;
        const auto* schema = value.find("schema");
        const auto* type = value.find("type");
        const auto* topic = value.find("topic");
        if (!schema || !schema->is_string() || schema->as_string() != "synth.notify.hint.v1") return std::nullopt;
        if (!type || !type->is_string()) return std::nullopt;
        const auto members = value.as_object().size();
        if (type->as_string() == "welcome") {
            if (members != 2 || topic) return std::nullopt;
            return HintKind::welcome;
        }
        if (type->as_string() == "wake") {
            if (members != 3 || !topic || !topic->is_string() || topic->as_string() != "speech") return std::nullopt;
            return HintKind::wake;
        }
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
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
                while (connection->receive(frame) == ReceiveResult::frame) {
                    const auto hint = parse_notify_hint(frame);
                    if (!hint) continue;
                    if (*hint == HintKind::welcome) {
                        if (!welcomed) {
                            welcomed = true;
                            backoff.reset();
                            signal_.set_connected(true);
                            reported_unavailable = false;
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
};

}  // namespace synth::transport
