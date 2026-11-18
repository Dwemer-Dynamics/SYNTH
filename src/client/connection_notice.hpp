#pragma once

#include "client/http.hpp"
#include "config/config.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace synth::client {

// Tells the player when the SYNTH server stops answering and when it answers again. The monitor polls one cheap,
// database-free server route (`/health.php?check=live`), so it never competes with dialogue and works before any
// session exists. Each outage gives exactly one "lost" notice and one "restored" notice; a game load that finds the
// outage still in progress adds one reminder. An outage is reported after two failed probes in a row. "Reachable"
// means an HTTP answer arrived: an unhealthy server is the health page's business, not an outage of the link.
//
// ConnectionNotice is the engine-free state machine (every method is thread-safe). ConnectionMonitor runs its probe
// on a short-lived worker, never the game thread; notices queue until the game thread drains them, so nothing is
// shown behind a loading screen.
class ConnectionNotice final {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr const char* unreachable = "SYNTH: cannot reach the server.";
    static constexpr const char* lost = "SYNTH: lost connection to the server.";
    static constexpr const char* restored = "SYNTH: connected to the server again.";
    static constexpr const char* still_down = "SYNTH: the server is still not reachable.";

    struct Timing final {
        std::chrono::seconds up_interval{20};   // between probes while the server answers
        std::chrono::seconds down_initial{5};   // first retry after a failure, doubled per failure up to the cap
        std::chrono::seconds down_maximum{60};
        // Consecutive failed probes before an outage is reported, so one dropped connection is not announced.
        unsigned failures_before_lost{2};
    };

    ConnectionNotice() = default;
    explicit ConnectionNotice(Timing timing) : timing_{timing} {}

    // True when no probe is running and the next one is due.
    [[nodiscard]] bool probe_due(Clock::time_point now) const {
        std::scoped_lock lock{mutex_};
        return !in_flight_ && now >= next_probe_;
    }

    void probe_started() {
        std::scoped_lock lock{mutex_};
        in_flight_ = true;
    }

    // Records one probe result; returns the notice to show for it, if any. Repeated failures and repeated successes
    // return nothing: only a change of state does.
    [[nodiscard]] std::optional<std::string> probe_finished(bool reachable, Clock::time_point now) {
        std::scoped_lock lock{mutex_};
        in_flight_ = false;
        if (reachable) {
            failures_ = 0;
            next_probe_ = now + timing_.up_interval;
            const bool announce = down_reported_;
            down_reported_ = false;
            reminder_due_ = false;
            ever_reachable_ = true;
            if (announce) return std::string{restored};
            return std::nullopt;
        }
        ++failures_;
        auto delay = timing_.down_initial;
        for (unsigned step = 1; step < failures_ && delay < timing_.down_maximum; ++step) delay *= 2;
        next_probe_ = now + std::min(delay, timing_.down_maximum);
        if (!down_reported_) {
            if (failures_ < timing_.failures_before_lost) return std::nullopt;
            down_reported_ = true;
            reminder_due_ = false; // The lost notice itself is the news; no reminder for the same outage.
            return std::string{ever_reachable_ ? lost : unreachable};
        }
        if (reminder_due_) {
            reminder_due_ = false;
            return std::string{still_down};
        }
        return std::nullopt;
    }

    // A game was loaded or started: probe at once, and remind once if the outage is still in progress.
    void game_loaded(Clock::time_point now) {
        std::scoped_lock lock{mutex_};
        next_probe_ = now;
        reminder_due_ = down_reported_;
    }

    [[nodiscard]] bool down_reported() const {
        std::scoped_lock lock{mutex_};
        return down_reported_;
    }

private:
    Timing timing_;
    mutable std::mutex mutex_;
    Clock::time_point next_probe_{};
    unsigned failures_{};
    bool in_flight_{};
    bool down_reported_{};
    bool reminder_due_{};
    bool ever_reachable_{};
};

class ConnectionMonitor final {
public:
    using Clock = ConnectionNotice::Clock;

    static constexpr const char* probe_target = "/health.php?check=live";

    explicit ConnectionMonitor(std::unique_ptr<IHttpTransport> transport, ConnectionNotice::Timing timing = {},
                               std::uint32_t probe_timeout_ms = 3'000)
        : transport_{std::move(transport)}, notice_{timing}, probe_timeout_ms_{probe_timeout_ms} {
        if (!transport_ || probe_timeout_ms_ == 0) throw std::invalid_argument{"connection monitor needs a transport"};
    }

    ConnectionMonitor(const ConnectionMonitor&) = delete;
    ConnectionMonitor& operator=(const ConnectionMonitor&) = delete;
    ~ConnectionMonitor() { stop(); }

    // Game thread: starts at most one probe, and only when one is due.
    void pump(Clock::time_point now) {
        if (stopping_.load(std::memory_order_acquire) || !notice_.probe_due(now)) return;
        if (worker_.joinable()) worker_.join(); // The previous probe finished: probe_due() was false until then.
        notice_.probe_started();
        worker_ = std::thread{[this] { probe(); }};
    }

    void game_loaded(Clock::time_point now) { notice_.game_loaded(now); }

    // Game thread: hands every queued notice to `present`.
    template <class Present>
    void drain(Present&& present) {
        std::vector<std::string> notices;
        {
            std::scoped_lock lock{queue_mutex_};
            notices.swap(queue_);
        }
        for (const auto& notice : notices) present(notice);
    }

    void stop() noexcept {
        stopping_.store(true, std::memory_order_release);
        transport_->cancel_all();
        if (worker_.joinable()) worker_.join();
    }

    [[nodiscard]] const ConnectionNotice& notice() const noexcept { return notice_; }

private:
    void probe() {
        bool reachable = false;
        try {
            const auto result = transport_->send(HttpRequest{
                .method = HttpMethod::get,
                .target = probe_target,
                .content_type = {},
                .body = {},
                .timeout_ms = probe_timeout_ms_,
                .accept = "application/json",
                .maximum_response_bytes = 4U * 1024U,
                .response_chunk = {},
            }, [this] { return stopping_.load(std::memory_order_acquire); });
            reachable = result.delivered();
            // Shutdown, not an outage: leave the state alone.
            if (result.failure == TransportFailure::cancelled) return;
        } catch (...) {
            reachable = false;
        }
        // The queue lock spans the state change, so a reader that sees the probe finished also sees its notice.
        std::scoped_lock lock{queue_mutex_};
        if (auto message = notice_.probe_finished(reachable, Clock::now())) queue_.push_back(std::move(*message));
    }

    std::unique_ptr<IHttpTransport> transport_;
    ConnectionNotice notice_;
    std::uint32_t probe_timeout_ms_;
    std::atomic_bool stopping_{};
    std::thread worker_;
    std::mutex queue_mutex_;
    std::vector<std::string> queue_;
};

// Owns the monitor for the current server settings. Rebuilt when the base URL or timeout changes and dropped when
// the SYNTH.ini toggle is off. The factory makes the platform transport (WinHTTP in the plugins, a fake in tests).
class ConnectionWatch final {
public:
    using Clock = ConnectionNotice::Clock;
    using Factory = std::function<std::unique_ptr<IHttpTransport>(const config::ServerConfig&)>;

    explicit ConnectionWatch(Factory factory, ConnectionNotice::Timing timing = {})
        : factory_{std::move(factory)}, timing_{timing} {}

    // Game thread, once per gameplay frame: follows the settings, probes when due, shows queued notices.
    template <class Present>
    void pump(const config::ServerConfig& server, Clock::time_point now, Present&& present) {
        if (!server.connection_notice) {
            monitor_.reset();
            return;
        }
        if (!monitor_ || server_ != server) {
            monitor_.reset();
            server_ = server;
            try {
                monitor_ = std::make_unique<ConnectionMonitor>(factory_(server), timing_);
            } catch (...) {
                return; // An unusable transport (bad URL) is reported by the session itself.
            }
        }
        monitor_->pump(now);
        monitor_->drain(std::forward<Present>(present));
    }

    // Lifecycle hook: a save was loaded or a new game started.
    void game_loaded(Clock::time_point now) {
        if (monitor_) monitor_->game_loaded(now);
    }

    void stop() noexcept { monitor_.reset(); }

private:
    Factory factory_;
    ConnectionNotice::Timing timing_;
    config::ServerConfig server_;
    std::unique_ptr<ConnectionMonitor> monitor_;
};

}  // namespace synth::client
