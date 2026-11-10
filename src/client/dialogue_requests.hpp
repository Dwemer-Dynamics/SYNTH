#pragma once

#include "client/conversation_holds.hpp"
#include "client/synth_client.hpp"
#include "media_fetch/media_fetch.hpp"
#include "tasks/task_lanes.hpp"

#include <atomic>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace synth::client {

// Owns typed and STT/voice dialogue submission for one session on the session's single bounded
// network lane: the player-input conversation hold, microphone upload, context publication then
// request, line and status callbacks, failure notices and cleanup. It also owns the session's
// outstanding response count; every other response producer on that lane (activation, context
// triggers, initialization/context publication, visual capture, rechat and its prefetch delivery,
// social progress) tracks through track_response() and reads the acquire count through responses(),
// so admission and idle checks see one exact total.
//
// The session keeps game-thread target enrichment, turn admission and replacement, external owner
// claims, activity marks, hold policy (clearing, suspension on actions, the game-thread frame) and
// cross-feature invalidation. It passes the admitted turn token and deadline per call. Context
// triggers stay with the session: their ambient, composing, pause and quest-receipt gates read
// session state between publication and the trigger request.
//
// Lifetimes: the borrowed client, network lane, terminal flag and holds outlive this object. The
// publish, line-routing and social-input callbacks capture the session and run only on network
// workers inside a task this object queued. The session joins the network lane (shutdown with
// cancel_pending) in its destructor body, before any member is destroyed, so no task and no line
// callback can outlive this object or the session. A rejected task is destroyed inside try_submit,
// and an expired, discarded or cancelled-pending task is destroyed by the lane without running; each
// closure owns only copies (snapshot, text or wave, identity, controls) plus this object's counter
// guard, so destroying it unrun releases the copied media and decrements the response count exactly once.
class DialogueRequests final {
public:
    using Snapshot = std::shared_ptr<const core::RuntimeSnapshot>;
    using Notify = std::function<void(std::string)>;
    // Publishes the copied scene for the exact actor (empty for untargeted modes) under the turn.
    using Publish = std::function<RequestOutcome(const Snapshot&, const std::optional<protocol_native::Identity>&,
                                                 core::CancellationToken, const protocol_native::ControlSelection&)>;
    // Routes one validated response line with the turn's snapshot and cancellation.
    using RouteLine = std::function<void(const protocol_native::Line&, const Snapshot&, const core::CancellationToken&)>;
    // Reports social-task acceptance or rejection; true when a rejection notice was already shown.
    using SocialInput = std::function<bool(const RequestOutcome&)>;

    struct Dependencies final {
        SynthClient& client;
        tasks::TaskLanes& network;
        const std::atomic_bool& terminal;
        ConversationHolds& holds;
        Publish publish;
        RouteLine route_line;
        SocialInput social_input;
        Notify notify;
    };

    explicit DialogueRequests(Dependencies dependencies)
        : client_{dependencies.client},
          network_{dependencies.network},
          terminal_{dependencies.terminal},
          holds_{dependencies.holds},
          publish_{std::move(dependencies.publish)},
          route_line_{std::move(dependencies.route_line)},
          social_input_{std::move(dependencies.social_input)},
          notify_{std::move(dependencies.notify)} {
        if (!publish_ || !route_line_ || !social_input_ || !notify_)
            throw std::invalid_argument{"dialogue requests require publication, line, social and notification sinks"};
    }

    DialogueRequests(const DialogueRequests&) = delete;
    DialogueRequests& operator=(const DialogueRequests&) = delete;

    // Shared by every response producer on the network lane; see the class comment. Producers
    // wrap their task with track_response(); everything else only reads the acquire count.
    [[nodiscard]] std::size_t responses() const noexcept { return responses_.load(std::memory_order_acquire); }
    template <class F>
        requires std::constructible_from<tasks::TaskLanes::Task, F&&>
    [[nodiscard]] tasks::TaskLanes::Task track_response(F&& task) {
        return tasks::TaskLanes::track_activity(responses_, std::forward<F>(task));
    }

    // Typed input and external Ask retain the same exact actor through context publication and
    // response delivery. `turn` is the token the session admitted for this submission.
    [[nodiscard]] bool submit_text(Snapshot snapshot, std::string text, protocol_native::Identity target,
                                   protocol_native::ControlSelection controls, protocol_native::TextIntent intent,
                                   tasks::TaskLanes::Deadline deadline, core::CancellationToken turn) {
        const auto accepted = network_.try_submit(
            "network", tasks::TaskClass::current_turn, deadline, std::move(turn),
            tasks::TaskLanes::track_activity(responses_,
            [this, snapshot = std::move(snapshot), text = std::move(text), target, controls, intent](
                const core::CancellationToken& cancellation) mutable {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;

                try {
                    hold_conversation_actor(target,cancellation,controls.mode);
                    const auto context_turn = client_.begin_context_turn();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    const auto context_outcome = publish_(snapshot,
                        controls.mode == "NARRATOR" || controls.mode == "INJECTION_LOG" ? std::nullopt : std::optional{target},
                        cancellation, controls);
                    if (context_outcome.status != RequestStatus::complete) {
                        if (context_outcome.status != RequestStatus::cancelled)
                            notify_("SYNTH text context failed: " + context_outcome.detail);
                        return;
                    }
                    const auto outcome = client_.send_text(
                        std::move(text), [this, snapshot, cancellation](const protocol_native::Line& line) {
                            route_line_(line, snapshot, cancellation);
                        }, context_outcome.context_binding, controls, intent);
                    const bool social_rejected=social_input_(outcome);
                    if (outcome.status != RequestStatus::complete && outcome.status != RequestStatus::cancelled) {
                        if(!social_rejected)notify_("SYNTH text request failed: " + outcome.detail);
                    }
                } catch (const std::exception& error) {
                    notify_(std::string{"SYNTH text request: "} + error.what());
                }
            }));
        if (accepted != tasks::SubmitResult::accepted) {
            notify_("SYNTH network queue is full; text input discarded");
            return false;
        }
        return true;
    }

    // Uploads the recording, then publishes context and sends the opaque media reference. An empty
    // recognition result arrives as a `speech_not_recognized` status line; transport or server
    // failures release the conversation hold and report separately.
    [[nodiscard]] bool submit_voice(Snapshot snapshot, std::vector<std::byte> wave, protocol_native::Identity target,
                                    protocol_native::ControlSelection controls, tasks::TaskLanes::Deadline deadline,
                                    core::CancellationToken turn) {
        const auto accepted = network_.try_submit(
            "network", tasks::TaskClass::current_turn, deadline, std::move(turn),
            tasks::TaskLanes::track_activity(responses_,
            [this, wave = std::move(wave), snapshot = std::move(snapshot), target, controls](
                const core::CancellationToken& cancellation) mutable {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;

                try {
                    hold_conversation_actor(target,cancellation,controls.mode);
                    const auto sha256 = media_fetch::sha256_hex(wave);
                    protocol_native::Media media{"media/" + sha256, "audio/wav", sha256,
                                                 static_cast<std::uint64_t>(wave.size())};
                    std::string body(reinterpret_cast<const char*>(wave.data()), wave.size());
                    const auto upload = client_.upload_media(media, std::move(body), [this, cancellation] {
                        return cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire);
                    });
                    if (!upload.delivered() || upload.response.status < 200 ||
                        upload.response.status >= 300) {
                        if (cancellation.is_cancelled()) return;
                        notify_("SYNTH microphone upload failed");
                        return;
                    }
                    const auto context_turn = client_.begin_context_turn();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    const auto context_outcome = publish_(snapshot,
                        controls.mode == "NARRATOR" || controls.mode == "INJECTION_LOG" ? std::nullopt : std::optional{target},
                        cancellation, controls);
                    if (context_outcome.status != RequestStatus::complete) {
                        if (context_outcome.status != RequestStatus::cancelled)
                            notify_("SYNTH voice context failed: " + context_outcome.detail);
                        return;
                    }
                    const auto outcome = client_.send_audio(
                        media, [this, snapshot, cancellation](const protocol_native::Line& line) {
                            route_line_(line, snapshot, cancellation);
                        }, context_outcome.context_binding, controls);
                    const bool social_rejected=social_input_(outcome);
                    if (outcome.status != RequestStatus::complete && outcome.status != RequestStatus::cancelled) {
                        holds_.suspend(cancellation);
                        if(!social_rejected)notify_("SYNTH voice request failed: " + outcome.detail);
                    }
                } catch (const std::exception& error) {
                    notify_(std::string{"SYNTH voice request: "} + error.what());
                }
            }));
        if (accepted != tasks::SubmitResult::accepted) {
            notify_("SYNTH network queue is full; voice input discarded");
            return false;
        }
        return true;
    }

private:
    // Player input only: registers the exact copied identity; the game thread applies the frame.
    void hold_conversation_actor(const protocol_native::Identity& actor,core::CancellationToken cancellation,std::string_view mode) {
        std::uint32_t form{};
        if (actor.form_id.size()!=10 || !actor.form_id.starts_with("0x")) return;
        const auto parsed=std::from_chars(actor.form_id.data()+2,actor.form_id.data()+actor.form_id.size(),form,16);
        if (parsed.ec==std::errc{} && parsed.ptr==actor.form_id.data()+actor.form_id.size())
            holds_.add({form,actor.origin_plugin,actor.playthrough_id},cancellation,core::SnapshotClock::now(),conversation_approach_mode(mode));
    }

    SynthClient& client_;
    tasks::TaskLanes& network_;
    const std::atomic_bool& terminal_;
    ConversationHolds& holds_;
    Publish publish_;
    RouteLine route_line_;
    SocialInput social_input_;
    Notify notify_;
    std::atomic_size_t responses_{};
};

}  // namespace synth::client
