#pragma once

#include "client/context_publication.hpp"
#include "client/dialogue_requests.hpp"
#include "client/observed_event_delivery.hpp"
#include "client/player_dialogue.hpp"
#include "client/synth_client.hpp"
#include "config/config.hpp"
#include "context/context.hpp"
#include "core/snapshot.hpp"
#include "media_capture/window_capture.hpp"
#include "media_fetch/media_fetch.hpp"
#include "targeting/targeting.hpp"
#include "tasks/task_lanes.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace synth::client {

// Owns one session's PipVision capture request: mode and subject validation (the pointed subject is
// resolved by the session per call; the nearest describing NPC here), the copied nearby audience and
// world, the frozen pixel buffer, and the network task that encodes, hashes and uploads the image,
// publishes context, sends the visual capture and, for "describe", requests the subject's reaction
// and advances the save frontier. visual_tasks_ admits one capture at a time.
//
// The session keeps the flat/VR hotkey entry, selected-actor targeting, player turn replacement
// (passed per call and invoked at most once, only for "describe") and response line routing. The
// existing window capture primitive freezes pixels on the game thread at the same point as before;
// encoding and upload run only on the worker.
//
// Locks: none of its own. visual_tasks_ is an admission counter released by the task guard; the
// worker holds no lock across encoding, HTTP or a callback.
//
// Lifetimes: the borrowed configuration, client, network lane, response accounting, context
// publication, dialogue, observed-event owner and flags outlive this object; the session declares
// it after them. Its network task captures this object, the copied snapshot, identities, world and
// the shared immutable pixel buffer; its line callback routes into the session. The session joins
// the network lane in its destructor body before any member is destroyed; a rejected, expired or
// discarded task is destroyed unrun and releases both visual_tasks_ and the response count once.
class VisualCapture final {
public:
    using Snapshot = std::shared_ptr<const core::RuntimeSnapshot>;
    using Notify = std::function<void(std::string)>;
    // Session response routing for the "describe" reaction's lines.
    using LineSink = std::function<void(const protocol_native::Line&, const Snapshot&, const core::CancellationToken&)>;

    struct Dependencies final {
        const config::Config& configuration;
        SynthClient& client;
        tasks::TaskLanes& network;
        DialogueRequests& requests;
        ContextPublication& context;
        PlayerDialogue& dialogue;
        ObservedEventDelivery& observations;
        const std::atomic_bool& terminal;
        const std::atomic_bool& paused;
        const std::atomic_bool& halting;
        core::CancellationToken session;
        LineSink line;
        Notify notify;
    };

    explicit VisualCapture(Dependencies dependencies)
        : configuration_{dependencies.configuration},
          client_{dependencies.client},
          network_{dependencies.network},
          requests_{dependencies.requests},
          context_{dependencies.context},
          dialogue_{dependencies.dialogue},
          observations_{dependencies.observations},
          terminal_{dependencies.terminal},
          paused_{dependencies.paused},
          halting_{dependencies.halting},
          session_{std::move(dependencies.session)},
          line_{std::move(dependencies.line)},
          notify_{std::move(dependencies.notify)} {
        if (!line_ || !notify_) throw std::invalid_argument{"visual capture requires line and notification sinks"};
    }

    VisualCapture(const VisualCapture&) = delete;
    VisualCapture& operator=(const VisualCapture&) = delete;

    // Bind subject/context and freeze pixels on the pump; encode/hash/upload only on the worker.
    // `pointed` resolves the session's pointed subject for "npc_portrait"; `begin_turn` replaces the
    // player turn for "describe". Each is invoked at most once, during this call.
    template <class Pointed, class BeginTurn>
    [[nodiscard]] bool submit(Snapshot snapshot, std::string interaction_mode, Pointed&& pointed, BeginTurn&& begin_turn) {
        if (!snapshot || snapshot->generation() != session_.generation() || !client_.ready() ||
            terminal_.load(std::memory_order_acquire) ||
            paused_.load(std::memory_order_acquire) || halting_.load(std::memory_order_acquire)) {
            return false;
        }
        // Admission is game-thread-only; a worker only releases this lifetime counter.
        if (visual_tasks_.load(std::memory_order_acquire) != 0) {
            notify_("SYNTH PipVision capture is still processing");
            return false;
        }
        std::optional<protocol_native::Identity> subject;
        if (interaction_mode == "npc_portrait") {
            subject = std::forward<Pointed>(pointed)(*snapshot);
            if (!subject) {
                notify_("SYNTH PipVision: target an NPC first");
                return false;
            }
        } else if (interaction_mode == "describe") {
            if (auto target = nearest_subject(*snapshot)) subject = identity(*target);
            if (!subject) {
                notify_("SYNTH PipVision: no nearby NPC can describe the scene");
                return false;
            }
        } else if (interaction_mode != "store_only") {
            return false;
        }

        if (subject && !observations_.enrich_selected_actor(snapshot, *subject)) return false;
        std::vector<protocol_native::Identity> nearby;
        const auto hearing_distance = snapshot->world() && snapshot->world()->interior
            ? configuration_.activation.interior_hearing_distance
            : configuration_.activation.exterior_hearing_distance;
        nearby.reserve(std::min<std::size_t>(snapshot->actors().size(), 16));
        for (const auto& actor : snapshot->actors()) {
            if (nearby.size() == 16) break;
            if (((subject && actor_key(identity(actor)) == actor_key(*subject)) ||
                 targeting::distance(snapshot->player_pose().position(), actor.position()) <= hearing_distance) &&
                targeting::dialogue_actor_available(actor)) {
                nearby.push_back(identity(actor));
            }
        }
        std::optional<protocol_native::WorldState> world;
        if (snapshot->world()) {
            world = protocol_native::WorldState{
                snapshot->world()->location, snapshot->world()->cell, snapshot->world()->worldspace,
                snapshot->world()->weather, snapshot->world()->interior,
                snapshot->world()->game_time_ticks};
        }
        // Share one immutable buffer through std::function copies, not many 128 MB pixel copies.
        const auto pixels = std::make_shared<const media_capture::CapturedPixels>(
            media_capture::capture_game_window_pixels());
        const auto perspective = snapshot->variant() == core::RuntimeVariant::vr ? "hmd" : "first_person";
        const auto deadline = tasks::TaskLanes::Clock::now() +
                              std::chrono::milliseconds{request_timeout_ms() * 2ULL};
        const auto turn = interaction_mode == "describe"
            ? std::forward<BeginTurn>(begin_turn)(subject ? actor_key(*subject) : std::string{}) : dialogue_.current_turn();
        const auto accepted = network_.try_submit(
            "network", tasks::TaskClass::current_turn, deadline, turn,
            tasks::TaskLanes::track_activity(visual_tasks_,
            requests_.track_response(
            [this, snapshot = std::move(snapshot), interaction_mode = std::move(interaction_mode),
             pixels = pixels, world = std::move(world), deadline,
             subject, nearby = std::move(nearby),
             perspective = std::string{perspective}](const core::CancellationToken& cancellation) mutable {
                if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;

                try {
                    const auto image = media_capture::encode_jpeg(*pixels);
                    pixels.reset();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire) ||
                        tasks::TaskLanes::Clock::now() >= deadline) return;
                    const auto sha256 = media_fetch::sha256_hex(image.bytes);
                    protocol_native::Media media{"media/" + sha256, image.content_type, sha256,
                                                 static_cast<std::uint64_t>(image.bytes.size())};
                    std::string body(reinterpret_cast<const char*>(image.bytes.data()), image.bytes.size());
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire) ||
                        tasks::TaskLanes::Clock::now() >= deadline) return;
                    const auto upload = client_.upload_media(media, std::move(body), [this, &cancellation] {
                        return cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire);
                    });
                    if (!upload.delivered() || upload.response.status < 200 || upload.response.status >= 300) {
                        notify_("SYNTH PipVision image upload failed");
                        return;
                    }
                    const auto context_turn = client_.begin_context_turn();
                    if (cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire)) return;
                    const auto context_outcome = context_.publish(snapshot, subject, cancellation);
                    if (context_outcome.status != RequestStatus::complete) {
                        notify_("SYNTH PipVision capture context failed: " + context_outcome.detail);
                        return;
                    }
                    const auto outcome = client_.send_visual_capture(
                        interaction_mode, perspective, media, world, subject, nearby,
                        [this, &cancellation] {
                            return cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire);
                        }, context_outcome.context_binding);
                    if (outcome.status != RequestStatus::complete) {
                        notify_("SYNTH PipVision failed: " + outcome.detail);
                        return;
                    }
                    if (interaction_mode == "describe" && subject) {
                        const auto reaction = client_.trigger(
                            "external_reaction", *subject,
                            [this, snapshot, cancellation](const protocol_native::Line& line) {
                                line_(line, snapshot, cancellation);
                            }, std::nullopt,
                            "Describe the bound PipVision capture naturally from your own point of view.",
                            context_outcome.context_binding, outcome.capture_id);
                        if (reaction.status != RequestStatus::complete) {
                            notify_("SYNTH PipVision description dialogue failed");
                            return;
                        }
                        // The original snapshot predates the accepted visual result. Advance the
                        // co-save frontier after completion without inventing a new game observation.
                        const auto checkpoint = context_.publish(snapshot, subject, cancellation);
                        if (checkpoint.status != RequestStatus::complete) {
                            notify_("SYNTH PipVision described; save checkpoint is not acknowledged");
                            return;
                        }
                    }
                    notify_(interaction_mode == "npc_portrait"
                                ? "SYNTH PipVision NPC portrait updated"
                                : interaction_mode == "describe"
                                      ? "SYNTH PipVision scene described"
                                      : "SYNTH PipVision scene captured");
                } catch (const std::exception& error) {
                    notify_(std::string{"SYNTH PipVision: "} + error.what());
                }
            })));
        if (accepted != tasks::SubmitResult::accepted) {
            notify_("SYNTH PipVision queue is full");
            return false;
        }
        dialogue_.claim_external(subject ? std::optional{actor_key(*subject)} : std::nullopt);
        return true;
    }

private:
    [[nodiscard]] std::optional<context::TargetContext> nearest_subject(const core::RuntimeSnapshot& snapshot) const {
        const context::PlaythroughIdentity playthrough{snapshot.player().playthrough_id()};
        const auto eligible = [](const core::ActorSnapshot& actor) {
            return targeting::dialogue_actor_available(actor);
        };
        return targeting::select_nearest(
                   snapshot, playthrough, snapshot.player_pose().position(),
                   configuration_.activation.automatic_hearing_distance, eligible)
            .target;
    }

    [[nodiscard]] static protocol_native::Identity identity(const core::ActorSnapshot& actor) {
        return PlayerDialogue::identity(actor);
    }
    [[nodiscard]] static protocol_native::Identity identity(const context::TargetContext& actor) {
        char form_id[11]{};
        std::snprintf(form_id, sizeof(form_id), "0x%08X", actor.identity().form().form_id());
        return {form_id, actor.identity().form().plugin(), actor.identity().playthrough().value(),
                actor.name()};
    }
    [[nodiscard]] static std::string actor_key(const protocol_native::Identity& actor) {
        return PlayerDialogue::actor_key(actor);
    }
    [[nodiscard]] std::uint64_t request_timeout_ms() const noexcept {
        return static_cast<std::uint64_t>(configuration_.behavior.ai_response_timeout_seconds) * 1000ULL;
    }

    const config::Config& configuration_;
    SynthClient& client_;
    tasks::TaskLanes& network_;
    DialogueRequests& requests_;
    ContextPublication& context_;
    PlayerDialogue& dialogue_;
    ObservedEventDelivery& observations_;
    const std::atomic_bool& terminal_;
    const std::atomic_bool& paused_;
    const std::atomic_bool& halting_;
    const core::CancellationToken session_;
    LineSink line_;
    Notify notify_;
    std::atomic_size_t visual_tasks_{};
};

}  // namespace synth::client
