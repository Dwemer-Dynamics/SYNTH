#pragma once

#include "client/synth_client.hpp"
#include "core/cancellation.hpp"
#include "core/readable_note_gate.hpp"
#include "core/snapshot.hpp"
#include "integration/prompt_target.hpp"
#include "protocol_native/v1_codec.hpp"
#include "tasks/task_lanes.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace synth::client {

// Readable paper notes for one flat runtime session, free of engine types: the bounded queue of reads the native
// BookMenu capture admitted, and at most one explicit `/share-note`. Items are delivered strictly in admission order,
// one at a time: a share runs after every read admitted before the command and before every later read, so the server
// shares the read the player last opened when typing it and a later read stays private. Each item publishes a fresh owned
// context and sends its event bound to that acknowledgement inside one context transaction; the event is encoded
// once, and an ambiguous failure resends exactly those bytes on a later attempt. Bodies are never logged.
//
// Threads: observe, share, due and deliver run on the game thread with copied values; requests run on the session's
// network lane, which the session joins before this object is destroyed. A new runtime generation is a new object, so
// loading, a new game, the main menu and retirement drop every queued read and share.
class ReadableNotes final {
public:
    using Snapshot = std::shared_ptr<const core::RuntimeSnapshot>;
    using Read = core::ReadableNoteGate::Read;
    using Notify = std::function<void(std::string)>;
    // Network worker, holding begin_context_turn(): publish the copied scene and return its exact binding.
    using Publish = std::function<RequestOutcome(const Snapshot&, const std::optional<protocol_native::Identity>&,
                                                 const protocol_native::ControlSelection&, core::CancellationToken)>;
    // Network worker: an accepted read or share must be covered by a later saved context ACK.
    using Changed = std::function<void(std::uint64_t, const core::CancellationToken&)>;
    // Game thread: the retained recipient is still the selectable actor in this fresh copied scene.
    using Eligible = std::function<bool(const core::RuntimeSnapshot&, const integration::PromptTarget&)>;

    static constexpr std::size_t read_capacity = 4;
    static constexpr unsigned max_attempts = 3;

    struct Dependencies final {
        SynthClient& client;
        tasks::TaskLanes& network;
        const std::atomic_bool& terminal;
        const std::atomic_bool& paused;
        core::CancellationToken session;
        Publish publish;
        Changed changed;
        Eligible eligible;
        Notify notify;
        std::chrono::milliseconds retry_delay{5'000};
    };

    // The share owner fixed at admission: the exact addressed actor, its full retained target and the confirmed mode.
    struct Recipient final {
        protocol_native::Identity actor;
        integration::PromptTarget target;
        protocol_native::ControlSelection controls;
    };

    enum class Command { none, share, malformed };

    explicit ReadableNotes(Dependencies dependencies)
        : client_{dependencies.client}, network_{dependencies.network}, terminal_{dependencies.terminal},
          paused_{dependencies.paused}, session_{std::move(dependencies.session)},
          publish_{std::move(dependencies.publish)}, changed_{std::move(dependencies.changed)},
          eligible_{std::move(dependencies.eligible)}, notify_{std::move(dependencies.notify)},
          retry_delay_{dependencies.retry_delay} {
        if (!publish_ || !changed_ || !eligible_ || !notify_)
            throw std::invalid_argument{"readable notes require publication, frontier, recipient and notification sinks"};
    }

    ReadableNotes(const ReadableNotes&) = delete;
    ReadableNotes& operator=(const ReadableNotes&) = delete;

    // `/share-note` alone shares; anything after it and any whitespace is refused, never read as a recipient, a note
    // name or speech.
    [[nodiscard]] static Command parse(std::string_view text) noexcept {
        constexpr std::string_view space{" \t\r\n\v\f"}, token{"/share-note"};
        const auto begin = text.find_first_not_of(space);
        if (begin == std::string_view::npos) return Command::none;
        text = text.substr(begin, text.find_last_not_of(space) - begin + 1);
        if (!text.starts_with(token)) return Command::none;
        if (text.size() == token.size()) return Command::share;
        return space.find(text[token.size()]) != std::string_view::npos ? Command::malformed : Command::none;
    }

    [[nodiscard]] bool ready() const noexcept {
        return client_.readable_notes_ready() && !terminal_.load(std::memory_order_acquire);
    }

    // Game thread: take one admitted read of this generation, also before the init grant; delivery waits for it. A
    // read that cannot be queued still becomes the latest one, so a later share fails clearly instead of falling back
    // to an older note. False only when this session cannot own the read (another generation, or halted).
    bool observe(Read read) {
        if (terminal_.load(std::memory_order_acquire) || read.generation != session_.generation().value()) return false;
        std::scoped_lock lock{mutex_};
        const auto serial = ++latest_serial_;
        try {
            if (reads_.size() < read_capacity) reads_.push_back(Item{serial, std::move(read), std::nullopt, std::nullopt, 0});
        } catch (...) {
        }
        return true;
    }

    // Game thread, before retirement: this generation whenever this session ever observed or inherited a read. A receipt
    // alone is not enough: a halt may suppress the saved ACK that would cover it, and a same-generation replacement then
    // binds an older anchor whose latest read is an older note. Conservative, so the replacement asks for a fresh read
    // even when the last one was persisted; a session that never saw a read leaves nothing.
    [[nodiscard]] std::optional<std::uint64_t> unconfirmed_read() const {
        std::scoped_lock lock{mutex_};
        if (latest_serial_ == 0) return std::nullopt;
        return session_.generation().value();
    }

    // Game thread: a retired session of this generation left its latest read unconfirmed. It stays the latest read,
    // ahead of every later one, until a later read's receipt; false when this session cannot own it.
    bool inherit_unconfirmed_read(std::uint64_t generation) {
        if (terminal_.load(std::memory_order_acquire) || generation != session_.generation().value()) return false;
        std::scoped_lock lock{mutex_};
        ++latest_serial_;
        return true;
    }

    // Only the Normal audience shares. ControlSelection gains `audience` (normal, everyone, everyone_unconfirmed) with
    // the Everyone mode; until then every selection is Normal.
    template <class Selection>
    [[nodiscard]] static bool normal_audience(const Selection& selection) noexcept {
        if constexpr (requires { selection.audience; })
            return selection.audience == std::remove_cvref_t<decltype(selection.audience)>::normal;
        else
            return true;
    }

    // Game thread. Seam for broadcast/Everyone and frozen-audience drafts: only a confirmed addressed mode whose one
    // recipient is the addressed actor may share, and a draft's frozen mode must still be the confirmed one.
    bool share(std::optional<Recipient> recipient, std::string_view frozen_mode, bool controls_pending, bool narrator) {
        const auto mode = recipient ? std::string_view{recipient->controls.mode} : std::string_view{};
        std::string_view refusal;
        if (!client_.uses_readable_notes_contract()) refusal = "SYNTH note sharing is not available in this game version";
        else if (!ready()) refusal = "SYNTH note sharing is unavailable on this server";
        else if (narrator) refusal = "Notes are not shared with the private narrator; use /narrator npcs first";
        else if (controls_pending) refusal = "SYNTH conversation mode is still changing; try /share-note again";
        else if (!recipient) refusal = "Choose one listener first";
        else if (!normal_audience(recipient->controls)) refusal = "Notes are shared with one listener; switch the audience to Normal first";
        else if (mode != "STANDARD" && mode != "WHISPER" && mode != "CLOSE")
            refusal = "Notes can only be shared in Standard, Whisper or Close mode";
        else if (!frozen_mode.empty() && frozen_mode != mode) refusal = "Conversation mode changed; reopen chat and try again";
        if (refusal.empty()) {
            std::scoped_lock lock{mutex_};
            // The intended note is fixed now: the latest read admitted before this command.
            if (share_) refusal = "A note share is already in progress";
            else share_ = Item{latest_serial_, {}, std::move(recipient), std::nullopt, 0};
        }
        if (!refusal.empty()) notify_(std::string{refusal});
        return refusal.empty();
    }

    // Game thread: the next item may start now; the caller then captures a fresh scene for deliver().
    [[nodiscard]] bool due() const {
        if (!ready() || paused_.load(std::memory_order_acquire)) return false;
        std::scoped_lock lock{mutex_};
        return !busy_ && (!reads_.empty() || share_) && tasks::TaskLanes::Clock::now() >= next_attempt_;
    }

    // Game thread: the actor a pending share needs in the fresh scene, when the share is next.
    [[nodiscard]] std::uint32_t capture_actor() const {
        std::scoped_lock lock{mutex_};
        return share_next() ? share_->recipient->target.form_id : 0;
    }

    // Game thread: start the next item with this fresh scene of the same generation. A share not yet encoded first
    // re-checks its full retained recipient in this scene; there is no nearest or crosshair fallback.
    bool deliver(Snapshot snapshot) {
        if (!snapshot || snapshot->generation() != session_.generation() || !due()) return false;
        std::optional<integration::PromptTarget> target;
        {
            std::scoped_lock lock{mutex_};
            if (share_next() && !share_->request) target = share_->recipient->target;
        }
        // Only this thread starts work and no worker runs while due(), so the next item cannot change meanwhile.
        if (target && !eligible_(*snapshot, *target)) {
            finish(true, Result::refused, "note_share_target_missing", session_);
            return false;
        }
        {
            std::scoped_lock lock{mutex_};
            busy_ = true;
            next_attempt_ = tasks::TaskLanes::Clock::now() + std::chrono::seconds{1};
        }
        // Owned by the submitted task: a rejected, expired, cancelled or finished task releases the slot once.
        struct Release final {
            explicit Release(ReadableNotes* value) noexcept : owner{value} {}
            Release(const Release&) = delete;
            Release& operator=(const Release&) = delete;
            ~Release() { std::scoped_lock lock{owner->mutex_}; owner->busy_ = false; }
            ReadableNotes* owner;
        };
        auto release = std::make_shared<Release>(this);
        return network_.try_submit("network", tasks::TaskClass::background,
            tasks::TaskLanes::Clock::now() + std::chrono::seconds{30}, session_,
            [this, release, snapshot = std::move(snapshot)](const core::CancellationToken& cancellation) {
                if (!stopped(cancellation)) run(snapshot, cancellation);
            }) == tasks::SubmitResult::accepted;
    }

private:
    struct Item final {
        std::uint64_t serial{};
        Read read;
        std::optional<Recipient> recipient;
        std::optional<SynthClient::NoteRequest> request;
        unsigned attempts{};
    };
    enum class Result { accepted, refused, ambiguous };

    [[nodiscard]] bool stopped(const core::CancellationToken& cancellation) const noexcept {
        return cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire);
    }

    // The share goes next once every read admitted before it is final, and stays ahead of every read admitted after
    // it while it is encoded, in flight or awaiting an ambiguous retry.
    [[nodiscard]] bool share_next() const noexcept {
        return share_ && (reads_.empty() || reads_.front().serial > share_->serial);
    }

    // The item a worker owns, in admission order.
    [[nodiscard]] Item* front(std::optional<bool> share = std::nullopt) noexcept {
        if (share.value_or(share_next())) return share_ ? &*share_ : nullptr;
        return reads_.empty() ? nullptr : &reads_.front();
    }

    // Network worker, the only owner of the front item while busy_ is held.
    void run(const Snapshot& snapshot, const core::CancellationToken& cancellation) {
        Item item;
        bool eligible{true};
        {
            std::scoped_lock lock{mutex_};
            const auto* next = front();
            if (!next) return;
            item = *next;
            // Every earlier read is final and no later one was sent; the server shares its latest visible read, so the
            // read the player last opened before the command must be the latest accepted one.
            if (item.recipient) eligible = accepted_serial_ == item.serial;
        }
        const bool share = item.recipient.has_value();
        if (!eligible) { finish(share, Result::refused, "note_read_unconfirmed", cancellation); return; }
        std::string code;
        try {
            const auto context_turn = client_.begin_context_turn();
            if (stopped(cancellation)) return;
            if (!item.request) {
                if (core::SnapshotClock::now() - snapshot->captured_at() > std::chrono::seconds{2}) return;
                const auto context = publish_(snapshot, share ? std::optional{item.recipient->actor} : std::nullopt,
                    share ? item.recipient->controls : protocol_native::ControlSelection{}, cancellation);
                if (stopped(cancellation)) return;
                if (context.status != RequestStatus::complete) { finish(share, Result::ambiguous, {}, cancellation); return; }
                item.request = client_.prepare_note(share, context.context_binding, [&](const protocol_native::EventContext& event) {
                    return share ? protocol_native::encode_note_share(event, item.recipient->controls.mode)
                                 : protocol_native::encode_note_read(event, {item.read.book_form_id, item.read.body});
                });
                std::scoped_lock lock{mutex_};
                if (auto* next = front(share)) next->request = item.request;
            }
            const auto outcome = client_.send_note(*item.request, cancellation);
            if (stopped(cancellation)) return;
            finish(share, classify(outcome, share, code), code, cancellation);
        } catch (const std::invalid_argument&) {
            if (!stopped(cancellation)) finish(share, Result::refused, {}, cancellation);
        } catch (const std::exception&) {
            if (!stopped(cancellation)) finish(share, Result::ambiguous, {}, cancellation);
        }
    }

    // Only the exact three-line acknowledgement is a receipt. A transient transport or HTTP failure is ambiguous,
    // because the server may already have committed the claim; anything else is a final refusal or conflict.
    [[nodiscard]] static Result classify(const RequestOutcome& outcome, bool share, std::string& code) {
        const auto* status = outcome.lines.size() == 3 ? std::get_if<protocol_native::Status>(&outcome.lines[1].payload) : nullptr;
        if (status) code = status->code;
        if (outcome.status == RequestStatus::complete)
            return status && status->code == (share ? "note_share_accepted" : "note_read_accepted") &&
                status->detail == (share ? "shared_with_target" : "player_private") ? Result::accepted : Result::refused;
        if (outcome.status == RequestStatus::transport_failure) return Result::ambiguous;
        if (outcome.status == RequestStatus::http_failure)
            return outcome.http_status == 408 || outcome.http_status == 409 || outcome.http_status == 429 ||
                outcome.http_status >= 500 ? Result::ambiguous : Result::refused;
        return Result::refused;
    }

    // Network worker: retire the front item unless an ambiguous attempt may still be retried with the same bytes.
    void finish(bool share, Result result, const std::string& code, const core::CancellationToken& cancellation) {
        std::string name;
        {
            std::scoped_lock lock{mutex_};
            auto* item = front(share);
            if (!item) return;
            if (result == Result::ambiguous && ++item->attempts < max_attempts) {
                next_attempt_ = tasks::TaskLanes::Clock::now() + retry_delay_;
                return;
            }
            next_attempt_ = {};
            if (share) {
                name = item->recipient->actor.display_name;
                share_.reset();
            } else {
                if (result == Result::accepted) accepted_serial_ = item->serial;
                reads_.pop_front();
            }
        }
        if (result == Result::accepted) changed_(session_.generation().value(), cancellation);
        if (share) notify_(share_message(result, code, name));
    }

    [[nodiscard]] static std::string share_message(Result result, std::string_view code, const std::string& name) {
        if (result == Result::accepted) return "Note shared with " + (name.empty() ? std::string{"your listener"} : name);
        if (result == Result::ambiguous) return "SYNTH could not confirm the note share; it may still have been delivered";
        if (code == "note_share_no_visible_read") return "Read a note first";
        if (code == "note_read_unconfirmed") return "SYNTH could not record your last note; read it again before sharing";
        if (code == "note_share_target_missing") return "Choose one listener first";
        if (code == "note_share_target_unplaced") return "Notes cannot be shared with that listener";
        if (code == "note_share_mode_unsupported" || code == "note_share_mode_mismatch")
            return "Notes can only be shared in Standard, Whisper or Close mode";
        if (code == "note_share_context_stale") return "The scene changed; try /share-note again";
        return "SYNTH could not share the note";
    }

    SynthClient& client_;
    tasks::TaskLanes& network_;
    const std::atomic_bool& terminal_;
    const std::atomic_bool& paused_;
    const core::CancellationToken session_;
    const Publish publish_;
    const Changed changed_;
    const Eligible eligible_;
    const Notify notify_;
    const std::chrono::milliseconds retry_delay_;
    mutable std::mutex mutex_;
    std::deque<Item> reads_;
    std::optional<Item> share_;
    std::uint64_t latest_serial_{}, accepted_serial_{};
    bool busy_{};
    tasks::TaskLanes::Clock::time_point next_attempt_{};
};

// Game thread, host-owned: reads admitted before a session of their generation could take them. Bounded by dropping the
// oldest, so the latest read is never lost; a read of another generation is discarded, never replayed. It also carries
// a retired session's read taint to a replacement of the same generation (a halt keeps the generation), so the
// replacement refuses to share until a later read is confirmed; that replacement carries it again when it retires. A new
// generation clears it. Reads are copied values and survive startup dwell and unready-world frames.
class PendingNoteReads final {
public:
    void push(ReadableNotes::Read read) {
        if (reads_.size() == ReadableNotes::read_capacity) reads_.pop_front();
        reads_.push_back(std::move(read));
    }

    // Before a session retires: its ReadableNotes::unconfirmed_read(). Nothing keeps a taint not yet handed over.
    void retire(std::optional<std::uint64_t> unconfirmed) noexcept {
        if (unconfirmed) unconfirmed_ = unconfirmed;
    }

    // Hands a carried taint to `inherit` (a session's inherit_unconfirmed_read), then reads in order to `accept` (its
    // observe) until it declines one. Every buffered read is newer than the taint.
    template <class Accept, class Inherit>
    void drain(std::uint64_t generation, Accept&& accept, Inherit&& inherit) {
        std::erase_if(reads_, [&](const ReadableNotes::Read& read) { return read.generation != generation; });
        if (unconfirmed_ && *unconfirmed_ != generation) unconfirmed_.reset();
        if (unconfirmed_ && !inherit(*unconfirmed_)) return;
        unconfirmed_.reset();
        while (!reads_.empty() && accept(reads_.front())) reads_.pop_front();
    }

    // Drops buffered reads only; a carried taint clears on the next drain of another generation.
    void clear() noexcept { reads_.clear(); }

private:
    std::deque<ReadableNotes::Read> reads_;
    std::optional<std::uint64_t> unconfirmed_;
};

}  // namespace synth::client
