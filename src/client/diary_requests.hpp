#pragma once

#include "client/context_publication.hpp"
#include "client/player_dialogue.hpp"
#include "client/synth_client.hpp"
#include "config/config.hpp"
#include "core/automatic_diary_queue.hpp"
#include "core/rest_event_mailbox.hpp"
#include "core/snapshot.hpp"
#include "tasks/task_lanes.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace synth::client {

// Owns one session's diary workflows: manual NPC/narrator/player admission, the bounded automatic
// sleep/wait queue and its admission cadence, the single pending diary (admission receipt, status
// polling, completion deadline) and the save-checkpoint frontier that confirms a finished diary.
// Every diary RPC runs as background work on the shared network lane behind diary_tasks_, so at most
// one diary task is outstanding and a new manual or automatic admission waits for the pending diary.
//
// The session keeps cross-feature admission: the checkpoint gate over responses, actions, speech and
// the microphone, the ordered pump position (after actions, captions and the delivered rechat parent),
// manual target selection (passed per call) and the checkpoint target. It calls in on the game thread
// with copied snapshots and identities; this owner never reads engine state.
//
// Locks: diary_mutex_ guards the pending diary, the automatic queue and its next attempt. It is
// never held across HTTP or context publication; notifications (a leaf mutex) and the automatic
// queue's lane submission happen under it exactly as before the extraction.
//
// Lifetimes: the borrowed configuration, client, network lane, context publication and flags
// outlive this object; the session declares it after them. Its network tasks capture this object
// and copied snapshots, identities and admission receipts only. The session joins the network lane in
// its destructor body before any member is destroyed; a rejected, expired or discarded task is
// destroyed unrun and releases diary_tasks_ once.
class DiaryRequests final {
public:
    using Snapshot = std::shared_ptr<const core::RuntimeSnapshot>;
    using Notify = std::function<void(std::string)>;

    struct Dependencies final {
        const config::Config& configuration;
        SynthClient& client;
        tasks::TaskLanes& network;
        ContextPublication& context;
        const std::atomic_bool& terminal;
        const std::atomic_bool& paused;
        const std::atomic_bool& halting;
        core::CancellationToken session;
        bool automatic_enabled;
        Notify notify;
    };

    explicit DiaryRequests(Dependencies dependencies)
        : configuration_{dependencies.configuration},
          client_{dependencies.client},
          network_{dependencies.network},
          context_{dependencies.context},
          terminal_{dependencies.terminal},
          paused_{dependencies.paused},
          halting_{dependencies.halting},
          session_{std::move(dependencies.session)},
          notify_{std::move(dependencies.notify)},
          automatic_diary_enabled_{dependencies.automatic_enabled},
          automatic_diaries_{session_.generation()} {
        if (!notify_) throw std::invalid_argument{"diary requests require a notification sink"};
    }

    DiaryRequests(const DiaryRequests&) = delete;
    DiaryRequests& operator=(const DiaryRequests&) = delete;

    // Session stop: queued rest batches never outlive admission.
    void stop() noexcept { std::scoped_lock lock{diary_mutex_}; automatic_diaries_.clear(); }

    [[nodiscard]] bool automatic_ready() const {
        return automatic_diary_enabled_ && client_.ready() && !terminal_.load(std::memory_order_acquire) &&
            !halting_.load(std::memory_order_acquire);
    }

    // The safe pump supplies one real copied frame, not a fabricated event-start snapshot or a later retarget.
    [[nodiscard]] std::size_t queue_automatic(Snapshot snapshot, const core::RestEventBatch& starts) {
        if (!automatic_ready() || !snapshot || starts.size > starts.events.size()) return 0;
        const auto now = core::SnapshotClock::now();
        std::scoped_lock lock{diary_mutex_};
        if (terminal_.load(std::memory_order_acquire) || session_.is_cancelled()) return 0;
        if (automatic_diaries_.expire(now) != 0) notify_("SYNTH expired queued automatic diaries before admission");
        std::size_t accepted{};
        for (std::size_t i = 0; i < starts.size; ++i)
            if (automatic_diaries_.push(snapshot, starts.events[i], now)) ++accepted;
        if (accepted != starts.size) notify_("SYNTH automatic diary queue rejected stale or excess rest events");
        return accepted;
    }

    // Player-owned roles use the copied player, never a fabricated NPC or a game-thread provider call.
    // `select` resolves the session's NPC target from the copied snapshot and is invoked at most once,
    // during this call, after the busy check.
    template <class Select>
    [[nodiscard]] bool request(Snapshot snapshot, protocol_native::DiaryRole role, Select&& select) {
        if (!snapshot || !client_.ready() || terminal_.load(std::memory_order_acquire) ||
            paused_.load(std::memory_order_acquire)) return false;
        {
            std::scoped_lock lock{diary_mutex_};
            if (pending_diary_ || diary_tasks_.load(std::memory_order_acquire) != 0) {
                notify_("SYNTH is still tracking the previous diary request");
                return false;
            }
        }
        const std::optional<protocol_native::Identity> selected = std::forward<Select>(select)(*snapshot);
        const auto player_owned = role != protocol_native::DiaryRole::npc;
        if (!player_owned && !selected) { notify_("SYNTH diary needs a pointed NPC"); return false; }
        const auto actor = player_owned ? PlayerDialogue::identity(snapshot->player()) : *selected;
        const auto deadline = tasks::TaskLanes::Clock::now() + std::chrono::milliseconds{request_timeout_ms() * 2ULL};
        const auto accepted = network_.try_submit("network", tasks::TaskClass::background, deadline, session_,
            tasks::TaskLanes::track_activity(diary_tasks_,
            [this, actor, role, player_owned, snapshot = std::move(snapshot)](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                try {
                    const auto context_turn = client_.begin_context_turn();
                    const auto published = context_.publish(snapshot, player_owned ? std::nullopt : std::optional{actor}, cancellation);
                    if (published.status != RequestStatus::complete) { notify_("SYNTH diary context was not acknowledged"); return; }
                    auto admission = client_.request_diary(actor, published.context_binding, role);
                    const auto& outcome = admission.outcome();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    if (outcome.status != RequestStatus::complete) notify_("SYNTH diary request failed; check logs");
                    else if (outcome.detail == "diary_queued") {
                        std::scoped_lock lock{diary_mutex_};
                        const auto now = tasks::TaskLanes::Clock::now();
                        pending_diary_ = PendingDiary{std::move(admission), now + std::chrono::minutes{30}, now + std::chrono::seconds{5}};
                        notify_("SYNTH diary queued for background generation");
                    } else notify_("SYNTH diary disabled: check the selected role's diary setting and connector");
                } catch (const std::exception&) { notify_("SYNTH diary request failed; check logs"); }
            }));
        if (accepted != tasks::SubmitResult::accepted) notify_("SYNTH diary queue is full");
        return accepted == tasks::SubmitResult::accepted;
    }

    // Diary half of the session's checkpoint gate: no diary task in flight and a finished diary due.
    [[nodiscard]] bool checkpoint_due() {
        if (diary_tasks_.load(std::memory_order_acquire) != 0) return false;
        std::scoped_lock lock{diary_mutex_};
        return pending_diary_ && pending_diary_->ready && tasks::TaskLanes::Clock::now() >= pending_diary_->next_poll;
    }

    // Game thread, only after the session's checkpoint gate passed for this generation's snapshot;
    // `target` is the session's copied selected actor for the published frame.
    [[nodiscard]] bool checkpoint(Snapshot snapshot, std::optional<protocol_native::Identity> target) {
        {
            std::scoped_lock lock{diary_mutex_};
            pending_diary_->next_poll = tasks::TaskLanes::Clock::now() + std::chrono::seconds{5};
        }
        const auto accepted = network_.try_submit("network", tasks::TaskClass::background,
            tasks::TaskLanes::Clock::now() + std::chrono::milliseconds{request_timeout_ms()}, session_,
            tasks::TaskLanes::track_activity(diary_tasks_,
            [this, snapshot = std::move(snapshot), target = std::move(target)](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                bool saved{};
                try {
                    const auto context_turn = client_.begin_context_turn();
                    saved = context_.publish(snapshot, target, cancellation).save_checkpoint_acknowledged;
                } catch (const std::exception&) {}
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                std::scoped_lock lock{diary_mutex_};
                if (!pending_diary_) return;
                if (saved) {
                    const auto partial = pending_diary_->partial;
                    pending_diary_.reset();
                    notify_(partial ? "SYNTH diary batch partly completed; successful entries included in your next save" :
                        "SYNTH diary complete; included in your next save");
                } else if (++pending_diary_->checkpoint_failures >= 3) {
                    const auto partial = pending_diary_->partial;
                    pending_diary_.reset();
                    notify_(partial ? "SYNTH diary batch partly generated, but its save checkpoint was not acknowledged" :
                        "SYNTH diary generated, but its save checkpoint was not acknowledged");
                }
            }));
        return accepted == tasks::SubmitResult::accepted;
    }

    // Admit one copied rest batch behind existing diary work; saturation retains it only until its bounded expiry.
    void pump_automatic() {
        if (!automatic_ready() || diary_tasks_.load(std::memory_order_acquire) != 0) return;
        std::scoped_lock lock{diary_mutex_};
        const auto now = tasks::TaskLanes::Clock::now();
        if (automatic_diaries_.expire(now) != 0) notify_("SYNTH expired queued automatic diaries before admission");
        if (pending_diary_ || !automatic_diaries_.front() || now < next_automatic_diary_attempt_) return;
        next_automatic_diary_attempt_ = now + std::chrono::seconds{5};
        const auto item = *automatic_diaries_.front();
        const auto accepted = network_.try_submit("network", tasks::TaskClass::background,
            item.expires, session_, tasks::TaskLanes::track_activity(diary_tasks_,
            [this, item](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                if (tasks::TaskLanes::Clock::now() >= item.expires) {
                    notify_("SYNTH automatic diary expired before context publication"); return;
                }
                try {
                    const auto context_turn = client_.begin_context_turn();
                    if (tasks::TaskLanes::Clock::now() >= item.expires || cancellation.is_cancelled()) return;
                    std::vector<protocol_native::Identity> audience;
                    const auto published = context_.publish(item.snapshot, std::nullopt, cancellation, &audience);
                    if (published.status != RequestStatus::complete) {
                        notify_("SYNTH automatic diary context was not acknowledged"); return;
                    }
                    if (tasks::TaskLanes::Clock::now() >= item.expires || cancellation.is_cancelled()) {
                        notify_("SYNTH automatic diary expired or was cancelled before admission"); return;
                    }
                    auto admission = client_.request_diary(protocol_native::DiaryRequest{
                        std::move(audience), true, true, item.reason == core::RestStart::sleep ?
                            protocol_native::DiaryReason::sleep : protocol_native::DiaryReason::wait}, published.context_binding);
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    const auto& outcome = admission.outcome();
                    if (outcome.status != RequestStatus::complete) {
                        notify_("SYNTH automatic diary admission failed; check server logs");
                    } else if (outcome.detail == "diary_queued") {
                        std::scoped_lock diary_lock{diary_mutex_};
                        if (terminal_.load(std::memory_order_acquire) || cancellation.is_cancelled()) return;
                        const auto admitted_at = tasks::TaskLanes::Clock::now();
                        pending_diary_ = PendingDiary{std::move(admission), admitted_at + std::chrono::minutes{30},
                            admitted_at + std::chrono::seconds{5}};
                        notify_("SYNTH automatic diary batch queued for background generation");
                    }
                    // Disabled/cooldown roles are normal automatic policy, not a dialogue failure.
                } catch (const std::exception&) { notify_("SYNTH automatic diary request failed; check server logs"); }
            }));
        if (accepted == tasks::SubmitResult::accepted) automatic_diaries_.pop();
    }

    // One short background RPC per interval; the saved receipt retains every selected role and original context.
    void pump_status() {
        if (diary_tasks_.load(std::memory_order_acquire) != 0) return;
        std::optional<PendingDiary> pending;
        {
            std::scoped_lock lock{diary_mutex_};
            if (!pending_diary_) return;
            const auto now = tasks::TaskLanes::Clock::now();
            if (now >= pending_diary_->deadline) {
                pending_diary_.reset();
                notify_("SYNTH diary completion tracking timed out; completion is not confirmed");
                return;
            }
            if (pending_diary_->ready || now < pending_diary_->next_poll) return;
            pending_diary_->next_poll = now + std::chrono::seconds{5};
            pending = pending_diary_;
        }
        (void)network_.try_submit("network", tasks::TaskClass::background,
            tasks::TaskLanes::Clock::now() + std::chrono::seconds{3}, session_,
            tasks::TaskLanes::track_activity(diary_tasks_,
            [this, pending = std::move(*pending)](const core::CancellationToken& cancellation) {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                RequestOutcome outcome;
                try { outcome = client_.fetch_diary_status(pending.admission); }
                catch (const std::exception&) { outcome.status = RequestStatus::malformed_response; }
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                std::scoped_lock lock{diary_mutex_};
                if (!pending_diary_) return;
                if (outcome.status == RequestStatus::complete) {
                    if (outcome.detail == "diary_queued" || outcome.detail == "diary_running") return;
                    if (outcome.detail == "diary_ready" || outcome.detail == "diary_partial") {
                        pending_diary_->ready = true;
                        pending_diary_->partial = outcome.detail == "diary_partial";
                        return;
                    }
                    pending_diary_.reset();
                    notify_("SYNTH diary did not complete; the server reported a terminal diary state");
                } else if (outcome.status != RequestStatus::transport_failure &&
                    !(outcome.status == RequestStatus::http_failure && outcome.detail.starts_with("HTTP 503"))) {
                    pending_diary_.reset();
                    notify_("SYNTH diary completion could not be verified; check server logs");
                }
            }));
    }

private:
    struct PendingDiary final {
        DiaryAdmission admission;
        tasks::TaskLanes::Deadline deadline;
        tasks::TaskLanes::Deadline next_poll;
        bool ready{};
        bool partial{};
        unsigned checkpoint_failures{};
    };

    [[nodiscard]] std::uint64_t request_timeout_ms() const noexcept {
        return static_cast<std::uint64_t>(configuration_.behavior.ai_response_timeout_seconds) * 1000ULL;
    }

    const config::Config& configuration_;
    SynthClient& client_;
    tasks::TaskLanes& network_;
    ContextPublication& context_;
    const std::atomic_bool& terminal_;
    const std::atomic_bool& paused_;
    const std::atomic_bool& halting_;
    const core::CancellationToken session_;
    Notify notify_;
    std::atomic_size_t diary_tasks_{};
    std::mutex diary_mutex_;
    std::optional<PendingDiary> pending_diary_;
    const bool automatic_diary_enabled_;
    core::AutomaticDiaryQueue automatic_diaries_;
    tasks::TaskLanes::Deadline next_automatic_diary_attempt_{};
};

}  // namespace synth::client
