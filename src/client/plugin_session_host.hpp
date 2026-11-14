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
        std::uint64_t save_epoch{};
        std::uint64_t save_lifetime{};
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
            stop_bound_writer_locked();
            pending_.reset();
            if (ready_ && ready_->session) {
                retired_.push_back(std::move(ready_->session));
            }
            ready_.reset();
        }
        condition_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    // `save_lifetime` is the store's lifetime stamp read immediately before the snapshot was captured.
    [[nodiscard]] std::uint64_t request_initialize(
        Snapshot snapshot,
        runtime::GameThreadDispatcher& dispatcher, std::uint64_t save_lifetime, bool rest_events_available = false,
        bool quest_events_available = false, bool actor_events_available = false,
        bool observed_dialogue_available = false, bool readable_notes_available = false) {
        if (!snapshot) return 0;
        std::scoped_lock lock{mutex_};
        if (stopping_) return 0;
        core::SaveContextStore::Binding save_binding;
        if (save_context_) {
            // A snapshot of any other, unresolved or earlier lifetime of the playthrough cannot start a writer,
            // even when a reload restored the same UUID.
            auto bound = save_context_->bind(snapshot->player().playthrough_id(), save_lifetime);
            if (!bound) return 0;
            save_binding = std::move(*bound);
            bound_save_epoch_ = save_binding.epoch;
        }
        const auto request_id = ++next_request_id_;
        pending_ = Request{request_id, epoch_, save_lifetime, std::move(snapshot), &dispatcher, std::move(save_binding), rest_events_available, quest_events_available, actor_events_available,
            observed_dialogue_available, readable_notes_available};
        condition_.notify_one();
        return request_id;
    }

    // Invalidates queued and completed initialization without waiting for any
    // in-flight network operation. Stale sessions are destroyed by the host. The latest binding's exact
    // writer is revoked too, so a request already popped by the worker cannot start its HTTP exchange.
    void invalidate() noexcept {
        std::scoped_lock lock{mutex_};
        ++epoch_;
        stop_bound_writer_locked();
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

    // A live session whose save writer was revoked after it became ready is retired here, never adopted;
    // in the same world lifetime its request still completes with an error so the game thread can retry.
    [[nodiscard]] std::optional<Result> take_result() {
        std::scoped_lock lock{mutex_};
        if (!ready_) return std::nullopt;
        if (!admit_locked(*ready_)) {
            ready_.reset();
            condition_.notify_one();
            return std::nullopt;
        }
        return std::exchange(ready_, std::nullopt);
    }

    // The game thread consumes a copied observation; virtual-filesystem calls
    // belong to the coordinator and must never run inside Main::Update.
    [[nodiscard]] std::optional<config::FileRevision> take_settings_revision() {
        std::scoped_lock lock{mutex_};
        return std::exchange(settings_revision_, std::nullopt);
    }

private:
    // Exact-epoch only: a newer bind (replacement or another world) is never revoked, and the UUID and
    // latest ACK stay in the store for the next binding.
    void stop_bound_writer_locked() noexcept {
        const auto epoch = std::exchange(bound_save_epoch_, 0);
        if (save_context_ && epoch != 0) {
            try { save_context_->stop_writer(epoch); } catch (...) {}
        }
    }

    [[nodiscard]] bool save_writer_current(std::uint64_t epoch) const {
        return !save_context_ || save_context_->writer_current(epoch);
    }

    // The world lifetime the request was admitted in. It authorizes terminal results without a session,
    // even after a same-world replacement or the failed session's own teardown revoked its writer.
    [[nodiscard]] bool save_world_current(std::uint64_t lifetime) const {
        return !save_context_ || save_context_->lifetime() == lifetime;
    }

    // A live session additionally needs its exact writer. One that lost it in the same world becomes an
    // error result (its session retired); any other world drops the result. Returns whether to publish.
    [[nodiscard]] bool admit_locked(Result& result) {
        if (!save_world_current(result.save_lifetime)) {
            if (result.session) retired_.push_back(std::move(result.session));
            return false;
        }
        if (result.session && !save_writer_current(result.save_epoch)) {
            retired_.push_back(std::move(result.session));
            result.error = "save writer revoked";
        }
        return true;
    }

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
        std::uint64_t save_lifetime{};
        Snapshot snapshot;
        runtime::GameThreadDispatcher* dispatcher{};
        core::SaveContextStore::Binding save_binding;
        bool rest_events_available{};
        bool quest_events_available{};
        bool actor_events_available{};
        bool observed_dialogue_available{};
        bool readable_notes_available{};
    };

    // Host epoch and request id: a request invalidated or superseded after the worker popped it.
    [[nodiscard]] bool request_stale_locked(const Request& request) const noexcept {
        return stopping_ || request.epoch != epoch_ || request.request_id != next_request_id_;
    }

    [[nodiscard]] bool request_stale(const Request& request) {
        std::scoped_lock lock{mutex_};
        return request_stale_locked(request);
    }

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

            Result result{.request_id = request->request_id, .save_epoch = request->save_binding.epoch,
                          .save_lifetime = request->save_lifetime};
            // Another world lifetime owns any result now; its own lifecycle invalidation clears the request.
            if (!save_world_current(result.save_lifetime) || request_stale(*request)) continue;
            // A same-world writer lost before creation (or before queueing initialization) ends as an error.
            const auto writer_lost = [&] {
                if (save_writer_current(result.save_epoch) && save_world_current(result.save_lifetime)) return false;
                result.error = "save writer revoked";
                return true;
            };
            try {
                auto configuration = PluginSession::load_configuration();
                if (!configuration.runtime.enabled) {
                    result.runtime_disabled = true;
                } else if (request->dispatcher == nullptr) {
                    result.error = "game-thread dispatcher is unavailable";
                } else if (!request_stale(*request) && !writer_lost()) {
                    result.session = std::make_unique<PluginSession>(
                        std::move(configuration), variant_, runtime_version_, *request->dispatcher,
                        request->snapshot->generation(), save_context_, std::move(request->save_binding),
                        request->rest_events_available, request->quest_events_available, request->actor_events_available,
                        request->observed_dialogue_available, request->readable_notes_available);
                    if (request_stale(*request) || writer_lost()) {
                        retire_and_destroy(result.session);
                    } else if (!result.session->initialize(request->snapshot)) {
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
                // PluginSession::initialize only queues the init request; the writer is rechecked here and
                // again at adoption, while the queued HTTP checks it on its own worker.
                if (request_stale_locked(*request) || !admit_locked(result)) {
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
    // Writer epoch of the latest successful bind; 0 once revoked by invalidate or destruction.
    std::uint64_t bound_save_epoch_{};
    bool stopping_{};
    // Initialized last: the worker may enter worker_loop immediately and must
    // never observe an unconstructed mutex, condition variable, or queue.
    std::thread worker_;
};

}  // namespace synth::client
