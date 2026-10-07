#pragma once

#include "client/plugin_session.hpp"

#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace synth::client {

// Owns session construction and destruction on a long-lived coordinator thread.
// Fallout's game thread only submits immutable snapshots and adopts completed
// sessions; it never starts, joins, or cancels network worker threads itself.
class PluginSessionHost final {
public:
    using Snapshot = PluginSession::Snapshot;

    struct Result final {
        std::uint64_t request_id{};
        std::unique_ptr<PluginSession> session;
        std::string error;
        bool runtime_disabled{};
    };

    using RetirementObserver = std::function<void(const RequestOutcome&)>;

    struct Settings final {
        std::shared_ptr<const config::Config> configuration;
        std::string error;
    };

    [[nodiscard]] std::optional<Settings> take_settings() {
        std::scoped_lock lock{mutex_};
        return std::exchange(settings_, std::nullopt);
    }

    PluginSessionHost(core::RuntimeVariant variant, std::string runtime_version,
                      RetirementObserver retirement_observer = {}, core::SaveContextStore* save_context = nullptr)
        : variant_{variant},
          runtime_version_{std::move(runtime_version)},
          retirement_observer_{std::move(retirement_observer)},
          save_context_{save_context},
          worker_{[this] { worker_loop(); }} {}

    PluginSessionHost(const PluginSessionHost&) = delete;
    PluginSessionHost& operator=(const PluginSessionHost&) = delete;

    ~PluginSessionHost() {
        {
            std::scoped_lock lock{mutex_};
            stopping_ = true;
            pending_.reset();
            if (ready_ && ready_->session) {
                retired_.push_back(std::move(ready_->session));
            }
            ready_.reset();
        }
        condition_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    [[nodiscard]] std::uint64_t request_initialize(
        Snapshot snapshot,
        runtime::GameThreadDispatcher& dispatcher, bool rest_events_available = false, bool quest_events_available = false,
        bool actor_events_available = false) {
        if (!snapshot) return 0;
        std::scoped_lock lock{mutex_};
        if (stopping_) return 0;
        const auto request_id = ++next_request_id_;
        auto save_binding = save_context_ ? save_context_->bind(snapshot->player().playthrough_id())
                                         : core::SaveContextStore::Binding{};
        pending_ = Request{request_id, epoch_, std::move(snapshot), &dispatcher, std::move(save_binding), rest_events_available, quest_events_available, actor_events_available};
        condition_.notify_one();
        return request_id;
    }

    // Invalidates queued and completed initialization without waiting for any
    // in-flight network operation. Stale sessions are destroyed by the host.
    void invalidate() noexcept {
        std::scoped_lock lock{mutex_};
        ++epoch_;
        pending_.reset();
        if (ready_ && ready_->session) {
            retired_.push_back(std::move(ready_->session));
        }
        ready_.reset();
        condition_.notify_one();
    }

    // Transfers an active session away from the game thread before its
    // destructor cancels WinHTTP, joins task workers, and halts audio.
    void retire(std::unique_ptr<PluginSession> session) noexcept {
        if (!session) return;
        std::scoped_lock lock{mutex_};
        retired_.push_back(std::move(session));
        condition_.notify_one();
    }

    [[nodiscard]] std::optional<Result> take_result() {
        std::scoped_lock lock{mutex_};
        if (!ready_) return std::nullopt;
        return std::exchange(ready_, std::nullopt);
    }

    // The game thread consumes a copied observation; virtual-filesystem calls
    // belong to the coordinator and must never run inside Main::Update.
    [[nodiscard]] std::optional<config::FileRevision> take_settings_revision() {
        std::scoped_lock lock{mutex_};
        return std::exchange(settings_revision_, std::nullopt);
    }

private:
    // Network retirement and destruction stay outside the host mutex on its coordinator thread.
    void retire_and_destroy(std::unique_ptr<PluginSession>& session) noexcept {
        if (!session) return;
        try {
            const auto outcome = session->retire_remote_session();
            if (outcome && retirement_observer_) retirement_observer_(*outcome);
        } catch (...) {
            try {
                if (retirement_observer_) retirement_observer_(RequestOutcome{
                    RequestStatus::transport_failure, {}, {}, "retirement failed"});
            } catch (...) {}
        }
        session.reset();
    }

    struct Request final {
        std::uint64_t request_id{};
        std::uint64_t epoch{};
        Snapshot snapshot;
        runtime::GameThreadDispatcher* dispatcher{};
        core::SaveContextStore::Binding save_binding;
        bool rest_events_available{};
        bool quest_events_available{};
        bool actor_events_available{};
    };

    void worker_loop() noexcept {
        auto next_settings_probe = std::chrono::steady_clock::time_point{};
        std::optional<config::FileRevision> loaded_revision;
        for (;;) {
            std::unique_ptr<PluginSession> retired;
            std::optional<Request> request;
            {
                std::unique_lock lock{mutex_};
                condition_.wait_until(lock, next_settings_probe, [this] {
                    return stopping_ || !retired_.empty() || pending_.has_value();
                });
                if (!retired_.empty()) {
                    retired = std::move(retired_.front());
                    retired_.pop_front();
                } else if (pending_) {
                    request = std::exchange(pending_, std::nullopt);
                } else if (stopping_) {
                    return;
                }
            }

            // Do not hold the host mutex across filesystem access: a delayed
            // MO2 hook must not prevent the game thread from adopting/retiring.
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_settings_probe) {
                next_settings_probe = now + std::chrono::seconds{1};
                try {
                    const auto revision = config::inspect_file_revision(
                        std::filesystem::current_path() / "Data/F4SE/Plugins/SYNTH_custom.ini");
                    if (!loaded_revision || revision != *loaded_revision) {
                        Settings result;
                        try {
                            result.configuration = std::make_shared<const config::Config>(PluginSession::load_configuration());
                            loaded_revision = revision;
                        } catch (const std::exception& error) {
                            result.error = error.what();
                        }
                        std::scoped_lock lock{mutex_};
                        settings_ = std::move(result);
                    }
                    std::scoped_lock lock{mutex_};
                    settings_revision_ = revision;
                } catch (...) {
                    // A transient path failure is not an observed edit.
                }
            }

            // Destruction happens outside the host lock because it may wait for
            // WinHTTP cancellation and task-worker shutdown.
            if (retired) {
                retire_and_destroy(retired);
                continue;
            }
            if (!request) continue;

            Result result{.request_id = request->request_id};
            try {
                auto configuration = PluginSession::load_configuration();
                if (!configuration.runtime.enabled) {
                    result.runtime_disabled = true;
                } else if (request->dispatcher == nullptr) {
                    result.error = "game-thread dispatcher is unavailable";
                } else {
                    result.session = std::make_unique<PluginSession>(
                        std::move(configuration), variant_, runtime_version_, *request->dispatcher,
                        request->snapshot->generation(), save_context_, std::move(request->save_binding),
                        request->rest_events_available, request->quest_events_available, request->actor_events_available);
                    if (!result.session->initialize(request->snapshot)) {
                        result.error = "session initialization was rejected";
                        retire_and_destroy(result.session);
                    }
                }
            } catch (const std::exception& error) {
                result.error = error.what();
                retire_and_destroy(result.session);
            } catch (...) {
                result.error = "session initialization failed";
                retire_and_destroy(result.session);
            }

            std::unique_ptr<PluginSession> replaced;
            {
                std::scoped_lock lock{mutex_};
                if (stopping_ || request->epoch != epoch_) {
                    replaced = std::move(result.session);
                } else {
                    if (ready_ && ready_->session) {
                        replaced = std::move(ready_->session);
                    }
                    ready_ = std::move(result);
                }
            }
            // A superseded or stale session is also torn down here, never on
            // Fallout's update thread.
            retire_and_destroy(replaced);
        }
    }

    core::RuntimeVariant variant_;
    std::string runtime_version_;
    RetirementObserver retirement_observer_;
    core::SaveContextStore* save_context_{};
    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<Request> pending_;
    std::optional<Result> ready_;
    std::optional<config::FileRevision> settings_revision_;
    std::optional<Settings> settings_;
    std::deque<std::unique_ptr<PluginSession>> retired_;
    std::uint64_t next_request_id_{};
    std::uint64_t epoch_{};
    bool stopping_{};
    // Initialized last: the worker may enter worker_loop immediately and must
    // never observe an unconstructed mutex, condition variable, or queue.
    std::thread worker_;
};

}  // namespace synth::client
