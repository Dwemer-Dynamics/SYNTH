#pragma once

#include "audio/dialogue_audio.hpp"
#include "audio/native_playback.hpp"
#include "client/rechat_prefetch.hpp"
#include "client/speech_facing_binding.hpp"
#include "client/speech_receipts.hpp"
#include "client/synth_client.hpp"
#include "config/config.hpp"
#include "diagnostics/conversation_trace.hpp"
#include "media_fetch/media_fetch.hpp"
#include "presentation/dialogue_presentation.hpp"
#include "runtime/fallout_runtime.hpp"
#include "tasks/task_lanes.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace synth::client {

// Owns ordered speech delivery for one session: the single-worker FIFO speech lane and its activity
// count, TTS/media preparation, native playback, captions, the active spatial frame and the bounded
// receipt outbox. Native work runs only through the IFalloutRuntime passed to game-thread methods.
// Borrowed dependencies (client, network lane, terminal flag) outlive this object; the session's
// network lane is joined before destruction.
class SpeechDelivery final {
public:
    using Snapshot = std::shared_ptr<const core::RuntimeSnapshot>;
    using Notify = std::function<void(std::string)>;
    using TerminalHook = std::function<void(presentation::DialogueDelivery, bool)>;

    struct Dependencies final {
        SynthClient& client;
        // Receipts keep background admission on the session's network lane, never the control lane.
        tasks::TaskLanes& network;
        core::CancellationToken session;
        const std::atomic_bool& terminal;
        std::shared_ptr<diagnostics::ConversationTrace> trace;
        Notify notify;
    };

    SpeechDelivery(const config::Config& configuration, Dependencies dependencies)
        : audio_{configuration.audio},
          interior_distance_{configuration.activation.interior_distance},
          exterior_distance_{configuration.activation.exterior_distance},
          client_{dependencies.client},
          network_{dependencies.network},
          session_{std::move(dependencies.session)},
          terminal_{dependencies.terminal},
          trace_{std::move(dependencies.trace)},
          notify_{std::move(dependencies.notify)} {
        if (!trace_ || !notify_) throw std::invalid_argument{"speech delivery requires trace and notification sinks"};
    }

    SpeechDelivery(const SpeechDelivery&) = delete;
    SpeechDelivery& operator=(const SpeechDelivery&) = delete;

    // Fallback only: the owning session normally joins the lane and halts playback in its own
    // destructor order. Both operations are idempotent, so repeating them here is a no-op; when the
    // session never got that far (a later member threw) this still joins the worker before any
    // member a task can touch is destroyed. The borrowed network lane may still hold a receipt drain
    // that captures this object, so the owner must join it before this destructor runs.
    ~SpeechDelivery() {
        join_jobs();
        halt_playback();
    }

    // Narrow operations: the session interleaves these with its other owners, so none groups them.
    [[nodiscard]] std::size_t discard_cancelled_jobs() noexcept { return speech_.discard_cancelled(); }
    [[nodiscard]] std::size_t discard_pending_jobs() noexcept { return speech_.discard_pending(); }
    void join_jobs() noexcept { speech_.shutdown(tasks::ShutdownMode::cancel_pending); }
    void interrupt_playback() noexcept { playback_.interrupt(); }
    void halt_playback() noexcept { playback_.halt(); }

    [[nodiscard]] bool jobs_active() const noexcept { return speech_jobs_.load(std::memory_order_acquire) != 0; }
    [[nodiscard]] bool playback_idle() const noexcept { return playback_.idle(); }
    [[nodiscard]] bool captions_idle() { return captions_.idle(); }
    [[nodiscard]] std::optional<audio::PlaybackFrame> playback_frame() const noexcept { return playback_.active_frame(); }
    void set_paused(bool paused) noexcept { playback_.set_paused(paused); }

    [[nodiscard]] presentation::DialogueCaptions::Caption validation_candidate(core::SnapshotClock::time_point now) {
        auto candidate = playback_.validation_candidate(now);
        return candidate ? candidate : captions_.validation_candidate(now);
    }

    [[nodiscard]] presentation::DialogueCaptions::Caption subtitle_frame(
        std::uint64_t generation, const std::optional<audio::PlaybackFrame>& playback,
        bool hidden, core::SnapshotClock::time_point now) {
        return captions_.frame(generation, playback ? playback->caption : nullptr, hidden, now);
    }

    // Release local speech synchronously before coordinator retirement; no network or worker joins.
    [[nodiscard]] bool stop_on_game_thread(runtime::IFalloutRuntime& runtime, bool discard) noexcept {
        try {
            runtime.assert_game_thread();
            return playback_.stop_on_game_thread([&runtime](const auto& owner, bool forget) {
                return runtime.set_speech_owner(owner, forget);
            }, discard);
        } catch (...) { return false; }
    }

    // Game-thread presentation pump: start/retire clips, surface failed audio as text, refresh the
    // active spatial frame and trigger one bounded receipt drain.
    void pump(std::uint64_t generation, runtime::IFalloutRuntime& runtime) {
        captions_.enqueue_failed(failed_playback_caption_);
        if (!failed_playback_caption_ && !playback_.pump(generation,
            [&runtime](const auto& owner, bool discard) { return runtime.set_speech_owner(owner, discard); },
            &failed_playback_caption_)) {
            notify_("SYNTH voice playback failed; dialogue remains available as text");
        }
        refresh_active_speech_spatial(generation, runtime);
        captions_.enqueue_failed(failed_playback_caption_);
        flush_speech_receipts(generation);
    }

    // Terminal caption hook for native v2 turns. Only completed native audio becomes audio_completed.
    [[nodiscard]] TerminalHook receipt_hook(const protocol_native::Line& line, const protocol_native::Dialogue& dialogue) {
        const auto* speaker = std::get_if<protocol_native::Identity>(&dialogue.speaker);
        if (!speaker || !dialogue.native_turn || !client_.native_turn_ready() ||
            line.protocol_version != 2 || line.context_sequence == 0) return {};
        return [outbox = std::weak_ptr<SpeechReceiptOutbox>{speech_receipts_},
                pending = PendingSpeechReceipt{line.generation, line.context_sequence, line.turn_id,
                    {line.request_id, line.line_id, *speaker, {}}}](
                    presentation::DialogueDelivery terminal, bool cancelled) mutable {
            const auto outcome = speech_receipt_outcome(terminal, cancelled);
            const auto target = outbox.lock();
            if (!outcome || !target) return;
            pending.receipt.outcome = *outcome;
            (void)target->push(std::move(pending));
        };
    }

    // Text-only and failed speech go straight to captions; ready/pending speech joins the ordered lane.
    void deliver(const protocol_native::Line& line, const protocol_native::Dialogue& dialogue,
                 const Snapshot& snapshot, const core::CancellationToken& cancellation,
                 const presentation::DialogueCaptions::Caption& caption,
                 const std::shared_ptr<RechatPrefetch>& prefetch = {}) {
        if (!dialogue.speech || dialogue.speech->status == "failed")
            captions_.enqueue(caption);
        if (dialogue.speech) {
            queue_speech(line, dialogue, snapshot, cancellation, caption, prefetch);
        }
    }

    // A single speculative job uses the existing speech lane and is cancelled with its exact parent.
    void prepare_rechat_media(const protocol_native::Line& line, const std::shared_ptr<RechatPrefetch>& prefetch) {
        const auto speech = *std::get<protocol_native::Dialogue>(line.payload).speech;
        const auto deadline = tasks::TaskLanes::Clock::now() + std::chrono::milliseconds{speech.deadline_ms};
        const auto accepted = speech_.try_submit("speech", tasks::TaskClass::current_turn, deadline, prefetch->token(),
            tasks::TaskLanes::track_activity(speech_jobs_, [this, line, speech, deadline, prefetch](const core::CancellationToken& token) {
                RechatPrefetch::MediaBytes bytes;
                try { bytes = prepare_speech_media(line, speech, token, deadline); }
                catch (const std::exception&) { /* Failure stays invisible until normal text delivery is admitted. */ }
                if (!bytes && !token.is_cancelled()) trace_->record(diagnostics::ConversationStage::media_failed,
                    line.generation,line.context_sequence,line.request_id,line.line_id);
                prefetch->finish_media(std::move(bytes));
            }));
        // Queue saturation leaves the line unprepared; normal release can attempt it once.
        if (accepted != tasks::SubmitResult::accepted)
            trace_->record(diagnostics::ConversationStage::media_failed,line.generation,line.context_sequence,line.request_id,line.line_id);
    }

private:
    [[nodiscard]] bool stopped(const core::CancellationToken& cancellation) const noexcept {
        return cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire);
    }

    // Game-thread trigger only: one background worker drains copied receipts. Receipts never use the
    // control lane reserved for cancellation, are never retried, and are revalidated before encoding.
    void flush_speech_receipts(std::uint64_t generation) {
        auto lease = SpeechReceiptOutbox::try_begin_drain(speech_receipts_);
        if (!lease) return;
        auto pending = speech_receipts_->take(generation);
        if (pending.empty() || !client_.native_turn_ready()) return;
        const auto deadline = tasks::TaskLanes::Clock::now() + std::chrono::seconds{10};
        (void)network_.try_submit("network", tasks::TaskClass::background, deadline, session_,
            [this, lease = std::move(lease), pending = std::move(pending)](const core::CancellationToken& cancellation) {
                for (const auto& value : pending) {
                    if (stopped(cancellation) || value.generation != cancellation.generation().value()) break;
                    try {
                        const auto outcome = client_.send_speech_receipt(value.receipt,
                            {value.generation, value.context_sequence, {}, value.turn_id});
                        trace_->record(outcome.status == RequestStatus::complete
                            ? diagnostics::ConversationStage::receipt_accepted : diagnostics::ConversationStage::receipt_rejected,
                            value.generation, value.context_sequence, value.receipt.dialogue_request_id, value.receipt.line_id);
                    } catch (const std::exception&) {
                        trace_->record(diagnostics::ConversationStage::receipt_rejected, value.generation,
                            value.context_sequence, value.receipt.dialogue_request_id, value.receipt.line_id);
                    }
                }
            });
    }

    void queue_speech(const protocol_native::Line& line,
                      const protocol_native::Dialogue& dialogue,
                      const Snapshot& snapshot, const core::CancellationToken& cancellation,
                      presentation::DialogueCaptions::Caption caption,
                      std::shared_ptr<RechatPrefetch> prefetch = {}) {
        const auto& speech = *dialogue.speech;
        if (line.generation != snapshot->generation().value() || speech.status == "failed" ||
            terminal_.load(std::memory_order_acquire)) {
            return;
        }
        const auto deadline = tasks::TaskLanes::Clock::now() +
                              std::chrono::milliseconds{speech.deadline_ms};
        const auto accepted = speech_.try_submit(
            "speech", tasks::TaskClass::current_turn, deadline, cancellation,
            tasks::TaskLanes::track_activity(speech_jobs_,
            [this, line, dialogue, snapshot, deadline, caption, prefetch](const core::CancellationToken& cancellation) {
                try {
                    // Preparation is ahead of this task on the same single-worker FIFO lane.
                    const auto prepared = prefetch ? prefetch->prepared_media(line.line_id) : std::nullopt;
                    auto bytes = prepared ? *prepared : prepare_speech_media(line, *dialogue.speech, cancellation, deadline);
                    if (stopped(cancellation)) return;
                    if (tasks::TaskLanes::Clock::now() >= deadline || !bytes) {
                        trace_->record(diagnostics::ConversationStage::media_failed,
                            line.generation,line.context_sequence,line.request_id,line.line_id);
                        notify_("SYNTH voice failed; delivering text only");
                        captions_.enqueue(caption);
                        return;
                    }
                    if (prepared) trace_->record(diagnostics::ConversationStage::media_reused,
                        line.generation,line.context_sequence,line.request_id,line.line_id);
                    enqueue_prepared_speech(line, dialogue, std::move(bytes), snapshot, cancellation, deadline, caption);
                } catch (const std::exception& error) {
                    if (stopped(cancellation)) return;
                    trace_->record(diagnostics::ConversationStage::media_failed,
                        line.generation,line.context_sequence,line.request_id,line.line_id);
                    notify_(std::string{"SYNTH voice unavailable; delivering text only: "} +
                            error.what());
                    captions_.enqueue(caption);
                }
            }));
        if (accepted != tasks::SubmitResult::accepted) {
            notify_("SYNTH speech queue is full; delivering text only");
            captions_.enqueue(caption);
        }
    }

    // Poll and verify immutable WAV bytes on a worker; this never presents text/audio or executes actions.
    [[nodiscard]] RechatPrefetch::MediaBytes prepare_speech_media(
        const protocol_native::Line& line, protocol_native::Speech speech,
        const core::CancellationToken& cancellation, tasks::TaskLanes::Clock::time_point deadline) {
        trace_->record(diagnostics::ConversationStage::media_prepare_started,
            line.generation,line.context_sequence,line.request_id,line.line_id);
        while (!stopped(cancellation) &&
               tasks::TaskLanes::Clock::now() < deadline && speech.status != "ready") {
            const auto status = client_.fetch_tts_status(speech.cache_key, [this, cancellation] {
                return stopped(cancellation);
            }, deadline);
            if (status.status == RequestStatus::complete && status.value) {
                if (status.value->status == "failed") return {};
                if (status.value->status == "ready" && status.value->media) {
                    speech.status = "ready";
                    speech.media = status.value->media;
                    break;
                }
            }
            std::this_thread::sleep_until(std::min(
                deadline, tasks::TaskLanes::Clock::now() + std::chrono::milliseconds{100}));
        }
        if (stopped(cancellation) ||
            tasks::TaskLanes::Clock::now() >= deadline || speech.status != "ready" || !speech.media) return {};
        trace_->record(diagnostics::ConversationStage::speech_ready,
            line.generation,line.context_sequence,line.request_id,line.line_id);
        const auto& media = *speech.media;
        if (media.content_type != "audio/wav" || media.media_id != "media/" + media.sha256 ||
            media.size == 0 || media.size > 16U * 1024U * 1024U) {
            throw std::invalid_argument{"unsupported or invalid TTS media reference"};
        }
        const auto response = client_.fetch_media(media.media_id, [this, cancellation] {
            return stopped(cancellation);
        }, deadline);
        if (!response.delivered() || response.response.status != 200 ||
            response.response.content_type != media.content_type ||
            response.response.body.size() != media.size) {
            throw std::runtime_error{"native TTS media fetch failed"};
        }
        media_fetch::Bytes bytes;
        bytes.reserve(response.response.body.size());
        for (const auto value : response.response.body) {
            bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
        }
        if (media_fetch::sha256_hex(bytes) != media.sha256) {
            throw std::runtime_error{"native TTS media hash mismatch"};
        }
        if (stopped(cancellation) ||
            tasks::TaskLanes::Clock::now() >= deadline) return {};
        trace_->record(diagnostics::ConversationStage::media_prepared,
            line.generation,line.context_sequence,line.request_id,line.line_id);
        return std::make_shared<const std::vector<std::byte>>(std::move(bytes));
    }

    void enqueue_prepared_speech(const protocol_native::Line& line,
                                const protocol_native::Dialogue& dialogue,
                                RechatPrefetch::MediaBytes shared,
                                const Snapshot& snapshot, const core::CancellationToken& cancellation,
                                tasks::TaskLanes::Clock::time_point deadline,
                                presentation::DialogueCaptions::Caption caption) {

        float volume = static_cast<float>(audio_.head_voice_volume_percent) / 100.0F;
        float pan{};
        std::string speaker_id{"narrator"};
        std::optional<runtime::RuntimeSpeechOwner> speech_owner;
        std::optional<audio::SpatialPlaybackSettings> spatial_settings;
        if (const auto* actor = std::get_if<protocol_native::Identity>(&dialogue.speaker)) {
            volume = static_cast<float>(audio_.voice_volume_percent) / 100.0F;
            speaker_id = actor->form_id;
            const core::ActorSnapshot* source{};
            const auto matches = [&](const core::ActorSnapshot& value) {
                char form_id[11]{};
                std::snprintf(form_id, sizeof(form_id), "0x%08X", value.form_id());
                return form_id == actor->form_id &&
                       value.origin_plugin() == actor->origin_plugin &&
                       value.playthrough_id() == actor->playthrough_id;
            };
            if (matches(snapshot->player())) source = &snapshot->player();
            for (const auto& candidate : snapshot->actors()) {
                if (source == nullptr && matches(candidate)) source = &candidate;
            }
            if (source) speech_owner = runtime::RuntimeSpeechOwner{
                line.generation, line.request_id, line.turn_id, dialogue.speech->utterance_id,
                {source->form_id(), source->origin_plugin(), source->playthrough_id()}, cancellation};
            if (source && source->form_id() != 0x14 && audio_.spatial_playback) {
                spatial_settings = audio::SpatialPlaybackSettings{
                    volume, audio_.distance_scale,
                    std::max(1.0, static_cast<double>(interior_distance_)),
                    std::max(1.0, static_cast<double>(exterior_distance_)),
                    audio_.interior_dropoff_percent / 100.0,
                    audio_.exterior_dropoff_percent / 100.0,
                    audio_.invert_heading};
            }
            const auto* listener = snapshot->effective_listener_pose(core::SnapshotClock::now());
            if (source != nullptr && listener != nullptr && audio_.spatial_playback) {
                audio::SpatialScene scene;
                const auto make_pose = [](const core::WorldPose& pose) {
                    const auto& forward = pose.forward().value();
                    const auto& up = pose.up().value();
                    return audio::Pose{{pose.position().x, pose.position().y, pose.position().z},
                                       {forward.x, forward.y, forward.z}, {up.x, up.y, up.z}, 1};
                };
                const auto listener_pose = make_pose(*listener);
                const auto view = snapshot->variant() == core::RuntimeVariant::vr
                                      ? audio::RuntimeView::vr
                                      : audio::RuntimeView::flat;
                (void)scene.update_listener(view, listener_pose,
                                            view == audio::RuntimeView::vr
                                                ? std::optional{listener_pose}
                                                : std::nullopt,
                                            1);
                (void)scene.set_source(speaker_id,
                                       {source->position().x, source->position().y, source->position().z});
                if (const auto azimuth = scene.azimuth_sine(speaker_id)) {
                    pan = static_cast<float>(audio_.invert_heading ? -*azimuth : *azimuth);
                }
                if (const auto distance = scene.distance_to(speaker_id)) {
                    const auto interior = snapshot->world().has_value() && snapshot->world()->interior;
                    const auto maximum = std::max(
                        1.0, static_cast<double>(interior
                                                     ? interior_distance_
                                                     : exterior_distance_));
                    const auto dropoff = static_cast<double>(
                        interior ? audio_.interior_dropoff_percent
                                 : audio_.exterior_dropoff_percent) / 100.0;
                    const auto scaled_distance = *distance * audio_.distance_scale;
                    volume *= static_cast<float>(
                        std::clamp(1.0 - (scaled_distance / maximum) * dropoff, 0.0, 1.0));
                }
            }
        }
        if (stopped(cancellation)) return;
        if (tasks::TaskLanes::Clock::now() >= deadline) {
            notify_("SYNTH voice deadline reached; delivering text only");
            captions_.enqueue(caption);
            return;
        }
        if (!playback_.enqueue(audio::NativeClip{line.generation, line.request_id, line.turn_id,
                                                 dialogue.speech->utterance_id, std::move(speaker_id),
                                                 std::move(shared), volume, pan,
                                                 audio_.pre_clip_ms,
                                                 audio_.post_clip_ms, cancellation,
                                                 bind_speech_facing(line, dialogue, *snapshot, cancellation),
                                                 caption, {}, std::move(speech_owner), std::move(spatial_settings)})) {
            throw std::runtime_error{"native playback queue rejected speech"};
        }
        trace_->record(diagnostics::ConversationStage::audio_queued,line.generation,line.context_sequence,line.request_id,line.line_id);
    }

    void refresh_active_speech_spatial(std::uint64_t generation, runtime::IFalloutRuntime& runtime) {
        const auto request = playback_.active_spatial_request(generation);
        if (!request) {
            spatial_playback_session_ = 0;
            spatial_playback_token_ = 0;
            return;
        }
        const auto now = core::SnapshotClock::now();
        const bool new_clip = spatial_playback_session_ != request->playback_session || spatial_playback_token_ != request->playback_token;
        if (!new_clip && now - last_spatial_capture_ < std::chrono::milliseconds{50}) return;
        spatial_playback_session_ = request->playback_session;
        spatial_playback_token_ = request->playback_token;
        last_spatial_capture_ = now;
        std::optional<audio::SpatialMix> mix;
        try {
            if (const auto captured = runtime.capture_speech_spatial(request->owner)) {
                const auto& forward = captured->listener_pose.forward().value();
                const auto& up = captured->listener_pose.up().value();
                const auto& listener = captured->listener_pose.position();
                const auto& source = captured->speaker_position;
                mix = audio::spatial_mix(request->settings, {source.x, source.y, source.z},
                    audio::Pose{{listener.x, listener.y, listener.z}, {forward.x, forward.y, forward.z}, {up.x, up.y, up.z}, 1},
                    captured->interior);
            }
        } catch (const std::exception&) {}
        // The request's generation and exact clip token are checked again after native capture.
        (void)playback_.set_spatial_mix(*request, mix, generation);
    }

    const config::AudioConfig audio_;
    const std::uint32_t interior_distance_;
    const std::uint32_t exterior_distance_;
    SynthClient& client_;
    tasks::TaskLanes& network_;
    const core::CancellationToken session_;
    const std::atomic_bool& terminal_;
    const std::shared_ptr<diagnostics::ConversationTrace> trace_;
    const Notify notify_;
    // Declared before the lane: activity, captions, playback and the outbox outlive every task copy.
    std::atomic_size_t speech_jobs_{};
    std::shared_ptr<SpeechReceiptOutbox> speech_receipts_{std::make_shared<SpeechReceiptOutbox>()};
    audio::NativeAudioPlayback playback_;
    presentation::DialogueCaptions captions_;
    presentation::DialogueCaptions::Caption failed_playback_caption_;
    core::SnapshotClock::time_point last_spatial_capture_{};
    std::uint64_t spatial_playback_session_{}, spatial_playback_token_{};
    // A single delivery worker preserves streamed sentence order while network/control work continues independently.
    tasks::TaskLanes speech_{{tasks::LaneConfig{"speech", 16, 4, 3}}, 1};
};

}  // namespace synth::client
