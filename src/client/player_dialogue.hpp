#pragma once

#include "client/ambient_exchange.hpp"
#include "client/automatic_activation.hpp"
#include "client/synth_client.hpp"
#include "client/wire_identity.hpp"
#include "config/config.hpp"
#include "diagnostics/conversation_trace.hpp"
#include "input/native_microphone.hpp"
#include "integration/external_requests.hpp"
#include "integration/prompt_target.hpp"
#include "targeting/targeting.hpp"
#include "tasks/task_lanes.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace synth::client {

// Owns player dialogue input for one session: the player-turn cancellation source and its reserved
// server-cancel dispatch, persistent external conversation ownership with actor end-conversation
// cooldowns and rechat suppression, push-to-talk/open-microphone recording with the target and
// controls bound at speech admission, typed text admission, player conversation target selection and
// the shared exact selected/pointed/external target policy.
// Recording, binding and admission run on the game thread; the turn source and the conversation state
// are locked separately (turn before conversation, never the reverse) and workers may use them.
// Request submission, response routing and cross-feature Stop/replacement sequencing stay with the
// session, which passes operation-specific state (current controls, the turn sequence) per call.
// Borrowed client, configuration and control lane outlive this object; the session joins the control
// lane before destruction.
class PlayerDialogue final {
public:
    using Snapshot = std::shared_ptr<const core::RuntimeSnapshot>;
    using Notify = std::function<void(std::string)>;
    // Per-call views of session state; never retained beyond the call.
    using Controls = std::function<protocol_native::ControlSelection()>;

    enum class MicrophoneEvent { none, speech_started, speech_finished };
    enum class TextCommand { text, diary_npc, diary_narrator, diary_player };

    static constexpr std::size_t maximum_text_bytes = 4096;

    // A typed turn routed by the session; diary commands carry no text, controls or target.
    struct TextTurn final {
        TextCommand command{TextCommand::text};
        std::string text;
        protocol_native::ControlSelection controls;
        protocol_native::TextIntent intent{protocol_native::TextIntent::unmarked};
        std::optional<context::TargetContext> target; // Empty only for untargeted modes.
    };

    // A finished recording bound to the turn and target retained at speech admission.
    struct VoiceTurn final {
        std::vector<std::byte> wave;
        protocol_native::ControlSelection controls;
        std::optional<context::TargetContext> target; // Empty only for untargeted modes.
        core::CancellationToken turn;
    };

    struct OpenMicrophonePump final {
        MicrophoneEvent event{MicrophoneEvent::none};
        bool voiced{};
    };

    PlayerDialogue(const config::Config& configuration, SynthClient& client, tasks::TaskLanes& control,
                   core::CancellationToken session, std::shared_ptr<diagnostics::ConversationTrace> trace,
                   Notify notify)
        : configuration_{configuration},
          client_{client},
          control_{control},
          session_{std::move(session)},
          turn_cancellation_{session_},
          trace_{std::move(trace)},
          notify_{std::move(notify)} {
        if (!trace_ || !notify_) throw std::invalid_argument{"player dialogue requires trace and notification sinks"};
    }

    PlayerDialogue(const PlayerDialogue&) = delete;
    PlayerDialogue& operator=(const PlayerDialogue&) = delete;

    [[nodiscard]] static bool text_within_bounds(std::string_view text) noexcept {
        return !text.empty() && text.size() <= maximum_text_bytes;
    }

    // Wire identity and actor key forward to the single engine-free authority in wire_identity.hpp.
    [[nodiscard]] static protocol_native::Identity identity(const core::ActorSnapshot& actor) {
        return wire_identity(actor);
    }

    [[nodiscard]] static protocol_native::Identity identity(const context::TargetContext& actor) {
        return wire_identity(actor);
    }

    [[nodiscard]] static std::string actor_key(const protocol_native::Identity& actor) {
        return wire_actor_key(actor);
    }

    // Player turn ownership. Replacement is game-thread admission only: no HTTP or worker joins.
    [[nodiscard]] core::CancellationToken current_turn() {
        std::scoped_lock lock{turn_mutex_};
        return turn_cancellation_.token();
    }

    [[nodiscard]] core::CancellationToken replace_turn(bool close_capture = false) {
        std::scoped_lock lock{turn_mutex_};
        turn_cancellation_.cancel();
        const core::CancellationSource next{session_};
        turn_cancellation_ = next;
        close_capture_ = close_capture;
        return turn_cancellation_.token();
    }

    // The original turn's capture policy survives prefetch children, but never an unrelated or replaced owner.
    [[nodiscard]] bool close_capture(const core::CancellationToken& owner) {
        std::scoped_lock lock{turn_mutex_};
        return close_capture_ && !owner.is_cancelled() && turn_cancellation_.contains(owner);
    }

    [[nodiscard]] bool cancel_turn_if_owner(const core::CancellationToken& cancellation) {
        std::scoped_lock lock{turn_mutex_};
        return turn_cancellation_.cancel_if_owner(cancellation);
    }

    // Applies a turn-owned effect while replacement is excluded, so a cancelled turn cannot apply it late.
    template <class Effect>
    [[nodiscard]] bool while_turn_current(const core::CancellationToken& cancellation, Effect&& effect) {
        std::scoped_lock lock{turn_mutex_};
        if (cancellation.is_cancelled()) return false;
        std::forward<Effect>(effect)();
        return true;
    }

    // The control lane can overtake dialogue HTTP without blocking its caller.
    void cancel_interrupted_turn() {
        if (const auto anchor = client_.take_interrupted_turn()) {
            const auto queued = control_.try_submit("control", tasks::TaskClass::current_turn,
                tasks::TaskLanes::Clock::now() + std::chrono::seconds{2}, session_,
                [this, anchor = *anchor](const core::CancellationToken&) {
                    trace_->record(diagnostics::ConversationStage::cancel_sent,anchor.generation,0,anchor.context_request_id);
                    try {
                        const auto outcome = client_.cancel(anchor.context_request_id, anchor.generation);
                        trace_->record(outcome.status == RequestStatus::complete ? diagnostics::ConversationStage::cancel_acknowledged :
                            diagnostics::ConversationStage::cancel_unconfirmed,anchor.generation,0,anchor.context_request_id);
                        if (outcome.status != RequestStatus::complete && outcome.status != RequestStatus::cancelled)
                            notify_("SYNTH interrupted locally; server cancellation was not acknowledged");
                    } catch (const std::exception&) {
                        trace_->record(diagnostics::ConversationStage::cancel_unconfirmed,anchor.generation,0,anchor.context_request_id);
                        notify_("SYNTH interrupted locally; server cancellation failed");
                    }
                });
            if (queued != tasks::SubmitResult::accepted)
                notify_("SYNTH interrupted locally; server cancellation queue is full");
        }
    }

    // Persistent external conversation ownership is distinct from outstanding work. It shares one
    // mutex with actor cooldowns and rechat suppression so ending a conversation is atomic.
    void begin_external(std::optional<std::string> actor) {
        std::scoped_lock lock{conversation_mutex_};
        external_owner_.begin(std::move(actor));
    }

    void claim_external(std::optional<std::string> actor) {
        std::scoped_lock lock{conversation_mutex_};
        external_owner_.claim(std::move(actor));
    }

    void own_external_work(const std::string& actor) {
        std::scoped_lock lock{conversation_mutex_};
        external_owner_.begin(actor);
        external_owner_.claim(actor);
    }

    // Ends the owner, installs the actor cooldown and suppresses its rechat in one lock scope, so
    // game-thread admission cannot observe the released owner before the cooldown.
    void end_conversation(const std::string& actor, core::SnapshotClock::time_point cooldown_until) {
        std::scoped_lock lock{conversation_mutex_};
        external_owner_.end(actor);
        conversation_cooldowns_.insert_or_assign(actor, cooldown_until);
        suppressed_rechat_actors_.insert(actor);
    }

    [[nodiscard]] bool on_cooldown(const std::string& actor, core::SnapshotClock::time_point now) {
        std::scoped_lock lock{conversation_mutex_};
        const auto found = conversation_cooldowns_.find(actor);
        if (found == conversation_cooldowns_.end()) return false;
        if (now >= found->second) {
            conversation_cooldowns_.erase(found);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool consume_rechat_suppression(const std::string& actor) {
        std::scoped_lock lock{conversation_mutex_};
        return suppressed_rechat_actors_.erase(actor) != 0;
    }

    void clear_rechat_suppression(const std::string& actor) {
        std::scoped_lock lock{conversation_mutex_};
        suppressed_rechat_actors_.erase(actor);
    }

    void clear_cooldowns() {
        std::scoped_lock lock{conversation_mutex_};
        conversation_cooldowns_.clear();
        suppressed_rechat_actors_.clear();
    }

    [[nodiscard]] std::optional<std::string> external_owner() {
        std::scoped_lock lock{conversation_mutex_};
        return external_owner_.owner();
    }

    // Nonblocking game-pump reads: contention reports false, as busy admission did before.
    // `select(owner, cooling)` runs under the lock; `cooling(actor, now)` reads without expiring.
    template <class Select>
    [[nodiscard]] bool try_with_conversation(Select&& select) {
        std::unique_lock lock{conversation_mutex_, std::try_to_lock};
        if (!lock.owns_lock()) return false;
        const auto cooling = [this](const std::string& actor, core::SnapshotClock::time_point now) {
            const auto found = conversation_cooldowns_.find(actor);
            return found != conversation_cooldowns_.end() && now < found->second;
        };
        std::forward<Select>(select)(external_owner_.owner(), cooling);
        return true;
    }

    [[nodiscard]] bool try_idle_external() {
        std::unique_lock lock{conversation_mutex_, std::try_to_lock};
        if (!lock.owns_lock()) return false;
        external_owner_.idle();
        return true;
    }

    [[nodiscard]] bool try_allows_ask(const std::string& actor, bool pipeline_idle) {
        std::unique_lock lock{conversation_mutex_, std::try_to_lock};
        return lock.owns_lock() && external_owner_.allows_ask(actor, pipeline_idle);
    }

    // Microphone state. Recording, the open-microphone flags and target binding are game-thread only;
    // every transition publishes voice activity as one value, so worker reads never sample them apart.
    [[nodiscard]] bool recording() const noexcept { return microphone_.recording(); }
    // Inactive teardown: hard_halt on the game thread, or ~PluginSession on the retirement worker.
    // Capture has definitively stopped, so this publishes false without reading game-thread flags.
    void cancel_microphone() noexcept {
        microphone_.cancel();
        publish_voice_inactive();
    }

    // Any thread: the last published recording && (!passive || voice detected).
    [[nodiscard]] bool voice_active() const noexcept { return voice_active_.load(std::memory_order_acquire); }

    [[nodiscard]] std::uint32_t voice_target_form_id() const noexcept {
        return voice_target_ ? voice_target_->form_id : 0;
    }

    template <class BeginTurn, class Started>
    [[nodiscard]] bool begin_voice_capture(const Snapshot& snapshot, bool suspended, BeginTurn&& begin_turn,
                                           Started&& on_started, const Controls& controls) {
        if (!snapshot || snapshot->generation() != session_.generation() || suspended || microphone_.recording()) {
            return false;
        }
        const auto started = microphone_.start();
        if (started) {
            open_mic_passive_recording_ = false;
            publish_voice_activity();
            voice_turn_ = begin_turn();
            bind_voice_target(snapshot, controls);
            std::forward<Started>(on_started)();
        }
        notify_(started ? "SYNTH voice capture started"
                        : "SYNTH microphone is unavailable");
        return started;
    }

    void cancel_voice_capture() {
        microphone_.cancel();
        open_mic_passive_recording_ = false;
        open_mic_voice_detected_ = false;
        publish_voice_inactive();
        voice_target_.reset();
        if (!voice_turn_.is_cancelled()) {
            std::scoped_lock lock{conversation_mutex_};
            external_owner_.end("");
        }
    }

    // Called once on the game thread at speech admission, never from the microphone worker.
    void bind_voice_target(const Snapshot& snapshot, const Controls& controls) {
        voice_target_.reset();
        voice_controls_ = controls();
        if (!snapshot || snapshot->generation() != session_.generation() ||
            voice_turn_.is_cancelled() || !microphone_.recording()) return;
        {
            std::scoped_lock lock{turn_mutex_};
            if (!voice_turn_.is_cancelled() && turn_cancellation_.contains(voice_turn_))
                close_capture_ = voice_controls_.mode == "CLOSE";
        }
        voice_target_ = prepare_chat_target(snapshot, [selection = voice_controls_] { return selection; });
    }

    // Empty and target-less recordings are reported here; the session submits an admitted turn.
    template <class Suspended>
    [[nodiscard]] std::optional<VoiceTurn> end_voice_capture(const Snapshot& snapshot, Suspended&& suspended) {
        std::optional<std::vector<std::byte>> wave;
        try {
            wave = microphone_.stop();
        } catch (...) {
            publish_voice_inactive(); // stop() ends recording before its wave allocation can throw.
            throw;
        }
        open_mic_passive_recording_ = false;
        publish_voice_inactive();
        const auto retained = std::exchange(voice_target_, std::nullopt);
        if (!voice_turn_.is_cancelled()) {
            std::scoped_lock lock{conversation_mutex_};
            external_owner_.end(""); // Empty/failed recording must not leave an unresolved owner behind.
        }
        if (!wave || !snapshot || snapshot->generation() != session_.generation() ||
            voice_turn_.is_cancelled() || suspended()) {
            notify_("SYNTH voice capture was empty");
            return std::nullopt;
        }
        const auto controls = voice_controls_;
        const auto selected = retained ? conversation_target(*snapshot, controls, &*retained, nullptr) : std::nullopt;
        const auto untargeted = controls.mode == "NARRATOR" || controls.mode == "INJECTION_LOG";
        if (!selected && !untargeted) {
            if (!refuse_everyone(controls, retained ? &*retained : nullptr))
                notify_("SYNTH voice input needs an eligible target");
            return std::nullopt;
        }
        return VoiceTurn{std::move(*wave), controls, untargeted ? std::nullopt : selected, voice_turn_};
    }

    // Only speech boundaries request a game-thread snapshot; idle/recording frames do no scene work.
    template <class BeginTurn>
    [[nodiscard]] OpenMicrophonePump pump_open_microphone(core::SnapshotClock::time_point now, bool suspended,
                                                          BeginTurn&& begin_turn) {
        if (!configuration_.input.open_microphone || configuration_.audio.open_mic_muted || suspended) {
            return {};
        }
        if (!microphone_.recording()) {
            if (microphone_.start(30)) {
                open_mic_passive_recording_ = true;
                open_mic_started_at_ = now;
                open_mic_voice_detected_ = false;
                publish_voice_activity();
                voice_target_.reset();
            }
            return {};
        }
        if (microphone_.recent_peak() >= configuration_.audio.open_mic_sensitivity) {
            if (!open_mic_voice_detected_) {
                // Publish before the callbacks: the network worker may recheck activation during them.
                open_mic_voice_detected_ = true;
                publish_voice_activity();
                voice_turn_ = begin_turn();
                voice_target_.reset();
                notify_("SYNTH open microphone detected speech");
                open_mic_last_voice_at_ = now;
                return {MicrophoneEvent::speech_started, true};
            }
            open_mic_voice_detected_ = true;
            open_mic_last_voice_at_ = now;
            return {MicrophoneEvent::none, true};
        }
        if (open_mic_voice_detected_) {
            const auto silence = std::chrono::duration<double>{now - open_mic_last_voice_at_}.count();
            if (silence >= configuration_.audio.open_mic_end_delay_seconds) {
                open_mic_voice_detected_ = false;
                publish_voice_activity();
                return {MicrophoneEvent::speech_finished, false};
            }
        } else if (open_mic_started_at_.time_since_epoch().count() != 0 &&
                   now - open_mic_started_at_ >= std::chrono::seconds{29}) {
            microphone_.cancel();
            open_mic_passive_recording_ = false;
            publish_voice_inactive();
            open_mic_started_at_ = {};
        }
        return {};
    }

    // `/talk <message>` explicitly selects the native conversation-only turn; unmarked text keeps
    // existing action-capable routing. The prefix is stripped here and never reaches the server.
    [[nodiscard]] std::optional<TextTurn> admit_text(const core::RuntimeSnapshot& snapshot, std::string text,
                                                     const integration::PromptTarget* retained, bool native_turn_ready,
                                                     const Controls& current_controls) {
        if (text == "/diary") return TextTurn{TextCommand::diary_npc};
        if (text == "/diary narrator") return TextTurn{TextCommand::diary_narrator};
        if (text == "/diary player") return TextTurn{TextCommand::diary_player};
        auto intent = protocol_native::TextIntent::unmarked;
        if (text == "/talk" || text.starts_with("/talk ")) {
            const auto begin = text.find_first_not_of(" \t\r\n", 5);
            const auto end = text.find_last_not_of(" \t\r\n");
            if (begin == std::string::npos) {
                notify_("SYNTH /talk needs a message: /talk <what you say>");
                return std::nullopt;
            }
            if (!native_turn_ready) {
                notify_("SYNTH /talk is unavailable: the server did not accept native conversation turns");
                return std::nullopt;
            }
            text = text.substr(begin, end - begin + 1);
            intent = protocol_native::TextIntent::conversation;
        }
        // Everyone is frozen here, at Send, from the acknowledged selection: the paused draft's actor and mode
        // do not apply. Normal keeps the draft's retained actor and controls exactly as before.
        const auto current = current_controls();
        const bool everyone = current.audience != protocol_native::AudienceIntent::normal;
        const auto controls = !everyone && retained && !retained->conversation_mode.empty()
            ? protocol_native::ControlSelection{retained->conversation_mode, retained->conversation_model_slot}
            : current;
        const auto selected = conversation_target(snapshot, controls, everyone ? nullptr : retained, nullptr);
        const auto untargeted = controls.mode == "NARRATOR" || controls.mode == "INJECTION_LOG";
        if (intent == protocol_native::TextIntent::conversation && untargeted) {
            notify_("SYNTH /talk needs a targeted conversation mode, not " + controls.mode);
            return std::nullopt;
        }
        if (!selected && !untargeted) {
            if (!refuse_everyone(controls, nullptr)) notify_("SYNTH text input needs an eligible target");
            return std::nullopt;
        }
        return TextTurn{TextCommand::text, std::move(text), controls, intent, untargeted ? std::nullopt : selected};
    }

    // Retain only identity before opening the pausing UI, never the old world snapshot.
    [[nodiscard]] std::optional<integration::PromptTarget> prepare_chat_target(
        const Snapshot& snapshot, const Controls& current_controls) {
        if (!snapshot || snapshot->generation() != session_.generation()) return {};
        const auto controls = current_controls();
        bool nearby{};
        const auto selected = conversation_target(*snapshot, controls, nullptr, &nearby);
        if (!selected) return {};
        for (const auto& actor : snapshot->actors()) {
            if (actor.form_id() == selected->identity().form().form_id()) {
                return integration::PromptTarget{session_, actor.form_id(), actor.base_form_id(),
                    actor.origin_plugin(), actor.base_origin_plugin(), actor.playthrough_id(), actor.name(), true,
                    controls.mode, controls.model_slot, nearby};
            }
        }
        return {};
    }

    // Game-thread caller supplies one fresh immutable scene. The renderer gets
    // copied canonical recipients only; it never resolves actors or target names.
    [[nodiscard]] std::vector<integration::PromptTarget> prepare_chat_targets(
        const Snapshot& snapshot, const Controls& current_controls) {
        const auto now=core::SnapshotClock::now();
        if(!snapshot||snapshot->generation()!=session_.generation()||
            now<snapshot->captured_at()||now-snapshot->captured_at()>std::chrono::seconds{2})return {};
        const auto controls=current_controls();
        if(controls.mode=="NARRATOR"||controls.mode=="INJECTION_LOG")return {};
        const auto current=prepare_chat_target(snapshot,current_controls);
        std::optional<integration::PromptTarget> preferred;
        std::vector<std::pair<double,integration::PromptTarget>> nearest;
        nearest.reserve(16);
        for(const auto& actor:snapshot->actors()) {
            if(!actor.alive()||actor.disabled()||actor.in_combat()||actor.hostile_to_player()||!actor.conversation_area_available())continue;
            integration::PromptTarget candidate{session_,actor.form_id(),actor.base_form_id(),actor.origin_plugin(),
                actor.base_origin_plugin(),actor.playthrough_id(),actor.name(),true,controls.mode,controls.model_slot,true};
            if(!conversation_target(*snapshot,controls,&candidate,nullptr))continue;
            if(current&&current->matches(actor)) {preferred=std::move(candidate);continue;}
            const auto distance=targeting::distance(snapshot->player_pose().position(),actor.position());
            const auto place=std::ranges::find_if(nearest,[&](const auto& value) {
                return distance<value.first||(distance==value.first&&actor.form_id()<value.second.form_id);
            });
            if(nearest.size()==16&&place==nearest.end())continue;
            nearest.insert(place,{distance,std::move(candidate)});
            if(nearest.size()>16)nearest.pop_back();
        }
        std::vector<integration::PromptTarget> result;result.reserve(16);
        if(preferred)result.push_back(std::move(*preferred));
        for(auto& [distance,target]:nearest) {
            (void)distance;
            if(result.size()==16)break;
            result.push_back(std::move(target));
        }
        return result;
    }

    // Shared exact target for activation, context, diary, profile and rechat revalidation. A retained
    // target never retargets; automatic flat selection uses the physical automatic-hearing limit.
    [[nodiscard]] std::optional<context::TargetContext> selected_target(
        const core::RuntimeSnapshot& snapshot, bool automatic, const integration::PromptTarget* retained = nullptr) {
        const context::PlaythroughIdentity playthrough{snapshot.player().playthrough_id()};
        const auto now = core::SnapshotClock::now();
        const auto eligible = [&](const core::ActorSnapshot& actor) {
            if (!targeting::dialogue_actor_available(actor) ||
                (configuration_.behavior.scene_safety && actor.talking_to_player()) ||
                (automatic && !configuration_.activation.include_hostile &&
                 actor.hostile_to_player())) {
                return false;
            }
            if (on_cooldown(actor_key(identity(actor)), now)) return false;
            if (!automatic || configuration_.activation.include_creatures) return true;
            return targeting::automatic_actor_class_supported(actor.race(), false);
        };
        if(automatic&&snapshot.variant()==core::RuntimeVariant::flat)
            return select_flat_automatic_activation(snapshot,now,session_.generation(),
                configuration_.activation.automatic_hearing_distance,eligible).target;
        if (retained) {
            if (retained->session.is_cancelled() || retained->session.generation() != snapshot.generation() ||
                snapshot.generation() != session_.generation()) return {};
            for (const auto& actor : snapshot.actors()) {
                if (!retained->matches(actor) || !eligible(actor)) continue;
                const auto distance = targeting::distance(snapshot.player_pose().position(), actor.position());
                if (distance > targeting::SelectionBounds{}.maximum_distance) return {};
                return context::TargetContext{context::identity_of(actor), actor.name(), actor.position(),
                    distance, snapshot.generation(), snapshot.frame()};
            }
            return {}; // Never silently retarget a draft after pause or actor replacement.
        }
        if (snapshot.variant() == core::RuntimeVariant::flat) {
            auto selected = targeting::select_flat_crosshair(snapshot, playthrough, {}, eligible);
            if (selected.target || !automatic) return std::move(selected.target);
        } else {
            auto selected = targeting::select_vr_controller_ray(
                snapshot, playthrough, targeting::Hand::right, now, {}, eligible);
            if (!selected.target) {
                selected = targeting::select_vr_hmd_gaze(snapshot, playthrough, now, {}, eligible);
            }
            if (selected.target) return std::move(selected.target);
        }
        auto selected = targeting::select_nearest(
            snapshot, playthrough, snapshot.player_pose().position(),
            automatic ? configuration_.activation.automatic_hearing_distance
                      : (snapshot.world() && snapshot.world()->interior
                             ? configuration_.activation.interior_distance
                             : configuration_.activation.exterior_distance),
            eligible);
        return std::move(selected.target);
    }

    // Revalidates the actor retained at speech admission under that admission's frozen controls; never retargets.
    [[nodiscard]] std::optional<context::TargetContext> revalidate_target(const core::RuntimeSnapshot& snapshot,
        const protocol_native::ControlSelection& controls, const integration::PromptTarget& retained) {
        return conversation_target(snapshot, controls, &retained, nullptr);
    }

    // Pointed subject for diary and portrait commands: availability only, no cooldown or range policy.
    [[nodiscard]] static std::optional<context::TargetContext> pointed_subject(const core::RuntimeSnapshot& snapshot) {
        const context::PlaythroughIdentity playthrough{snapshot.player().playthrough_id()};
        const auto eligible = [](const core::ActorSnapshot& actor) {
            return targeting::dialogue_actor_available(actor);
        };
        if (snapshot.variant() == core::RuntimeVariant::flat) {
            return targeting::select_flat_crosshair(snapshot, playthrough, {}, eligible).target;
        }
        const auto now = core::SnapshotClock::now();
        auto selected = targeting::select_vr_controller_ray(
            snapshot, playthrough, targeting::Hand::right, now, {}, eligible);
        if (!selected.target) {
            selected = targeting::select_vr_hmd_gaze(snapshot, playthrough, now, {}, eligible);
        }
        return std::move(selected.target);
    }

    // External request/prompt actor: the bounded scene lookup plus cooldown, hostility and combat policy.
    [[nodiscard]] const core::ActorSnapshot* external_actor(const core::RuntimeSnapshot& snapshot, std::uint32_t form_id,
                                                            core::SnapshotClock::time_point now) {
        const auto* actor = integration::external_actor(snapshot, form_id, now,
            configuration_.activation.automatic_hearing_distance);
        if (!actor || on_cooldown(actor_key(identity(*actor)), now) ||
            (!configuration_.activation.include_hostile && actor->hostile_to_player()) ||
            (actor->in_combat() && !configuration_.behavior.enable_combat_dialogue)) return nullptr;
        return actor;
    }

private:
    // Game-thread classification after capture starts or open-microphone speech flags change.
    void publish_voice_activity() noexcept {
        voice_active_.store(ambient_player_voice_active(microphone_.recording(), open_mic_passive_recording_,
            open_mic_voice_detected_), std::memory_order_release);
    }

    // Capture has definitively stopped; safe from teardown without reading game-thread state.
    void publish_voice_inactive() noexcept { voice_active_.store(false, std::memory_order_release); }

    // Explains a failed Everyone admission; nothing is sent. False leaves Normal's existing message to the caller.
    bool refuse_everyone(const protocol_native::ControlSelection& controls, const integration::PromptTarget* anchor) {
        if (controls.audience == protocol_native::AudienceIntent::normal) return false;
        if (controls.audience == protocol_native::AudienceIntent::everyone_unconfirmed)
            notify_("SYNTH Everyone audience is not confirmed by the server; nothing was sent");
        else if (anchor)
            notify_("SYNTH Everyone: " + anchor->display_name + " is no longer an eligible listener; nothing was sent");
        else
            notify_("SYNTH Everyone: no eligible NPC is within hearing range; nothing was sent");
        return true;
    }

    // Player conversation only: commands retain their existing pointed/explicit selection semantics.
    [[nodiscard]] std::optional<context::TargetContext> conversation_target(
        const core::RuntimeSnapshot& snapshot, const protocol_native::ControlSelection& controls,
        const integration::PromptTarget* retained, bool* nearby) {
        if (snapshot.generation() != session_.generation()) return {};
        const context::PlaythroughIdentity playthrough{snapshot.player().playthrough_id()};
        const auto now = core::SnapshotClock::now();
        auto origin = snapshot.player_pose().position();
        if (snapshot.variant() == core::RuntimeVariant::vr) {
            if (!snapshot.hmd_pose() || !snapshot.hmd_pose()->fresh_at(now) ||
                snapshot.hmd_pose()->frame() != snapshot.frame()) return {};
            origin = snapshot.hmd_pose()->pose().position();
        }
        const auto range = targeting::conversation_range(snapshot.world() && snapshot.world()->interior
            ? configuration_.activation.interior_hearing_distance
            : configuration_.activation.exterior_hearing_distance, controls.mode);
        // VR selects from the HMD, but the server-visible Whisper/Close radius is measured from the body pose.
        // A narrow addressed actor must satisfy both so context capture cannot omit the selected target.
        const bool narrow = controls.mode == "WHISPER" || controls.mode == "CLOSE";
        const auto direct_eligible = [&](const core::ActorSnapshot& actor) {
            return actor.conversation_area_available() && targeting::dialogue_actor_available(actor) &&
                (!narrow || targeting::distance(snapshot.player_pose().position(), actor.position()) <= range) &&
                !(configuration_.behavior.scene_safety && actor.talking_to_player()) &&
                !on_cooldown(actor_key(identity(actor)), now);
        };
        const auto nearby_eligible = [&](const core::ActorSnapshot& actor) {
            return direct_eligible(actor) &&
                (configuration_.activation.include_hostile || !actor.hostile_to_player()) &&
                (configuration_.behavior.enable_combat_dialogue || !actor.in_combat()) &&
                targeting::automatic_actor_class_supported(actor.race(), configuration_.activation.include_creatures) &&
                targeting::player_speech_audible(range, targeting::distance(origin, actor.position()));
        };
        // Everyone ignores crosshair, ray and gaze. Its one transport anchor must be an actual Standard/Shout hearer
        // under the nearby rules, from both the selection origin and the body pose the published context measures.
        const bool everyone = controls.audience == protocol_native::AudienceIntent::everyone;
        if (controls.audience == protocol_native::AudienceIntent::everyone_unconfirmed ||
            (everyone && !protocol_native::everyone_audience_mode(controls.mode))) return {};
        const auto everyone_hearer = [&](const core::ActorSnapshot& actor) {
            return nearby_eligible(actor) &&
                targeting::player_speech_audible(range, targeting::distance(snapshot.player_pose().position(), actor.position()));
        };
        if (retained) {
            if (retained->session.is_cancelled() || retained->session.generation() != snapshot.generation()) return {};
            for (const auto& actor : snapshot.actors()) {
                if (!retained->matches(actor) || !direct_eligible(actor) ||
                    (retained->nearby_conversation && !nearby_eligible(actor)) || (everyone && !everyone_hearer(actor))) continue;
                const auto distance = targeting::distance(origin, actor.position());
                if (distance > range) return {};
                return context::TargetContext{context::identity_of(actor), actor.name(), actor.position(),
                    distance, snapshot.generation(), snapshot.frame()};
            }
            return {}; // A draft/recording never silently switches to another nearby actor.
        }
        if (everyone) {
            // Deterministic: nearest by body distance, then form identity; never one request per hearer.
            auto selected = targeting::select_nearest(snapshot, playthrough, snapshot.player_pose().position(), range,
                everyone_hearer);
            if (nearby) *nearby = selected.target.has_value();
            return std::move(selected.target);
        }
        const targeting::SelectionBounds bounds{range, 75.0};
        auto selected = targeting::select_conversation(snapshot, playthrough, now, bounds,
            direct_eligible, nearby_eligible, nearby);
        return std::move(selected.target);
    }

    const config::Config& configuration_;
    SynthClient& client_;
    tasks::TaskLanes& control_;
    const core::CancellationToken session_;
    std::mutex turn_mutex_;
    core::CancellationSource turn_cancellation_;
    bool close_capture_{};
    std::mutex conversation_mutex_;
    integration::ExternalConversationOwner external_owner_;
    std::unordered_map<std::string, core::SnapshotClock::time_point> conversation_cooldowns_;
    std::unordered_set<std::string> suppressed_rechat_actors_;
    std::shared_ptr<diagnostics::ConversationTrace> trace_;
    Notify notify_;
    input::NativeMicrophone microphone_;
    core::CancellationToken voice_turn_;
    std::optional<integration::PromptTarget> voice_target_;
    protocol_native::ControlSelection voice_controls_;
    core::SnapshotClock::time_point open_mic_started_at_{};
    core::SnapshotClock::time_point open_mic_last_voice_at_{};
    bool open_mic_voice_detected_{};
    bool open_mic_passive_recording_{};
    std::atomic_bool voice_active_{};
};

}  // namespace synth::client
