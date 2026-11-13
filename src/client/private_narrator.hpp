#pragma once

#include "client/dialogue_requests.hpp"
#include "client/synth_client.hpp"
#include "core/cancellation.hpp"
#include "core/snapshot.hpp"
#include "media_fetch/media_fetch.hpp"
#include "presentation/dialogue_presentation.hpp"
#include "protocol_native/v1_codec.hpp"
#include "tasks/task_lanes.hpp"
#include "ui/narrator_transcript.hpp"

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
#include <utility>
#include <variant>
#include <vector>

namespace synth::client {

// Private narrator owner for one runtime session, shared by flat and VR and free of engine types: whether typed and
// push-to-talk input goes to the narrator or to NPCs, the last private state the server returned, the one live
// narrator turn, its reply receipt and the save checkpoint a private change makes due. The server owns definitions,
// revisions, the transcript and its save branching; this object only mirrors the latest acknowledged view.
//
// Threads: entry points run on the game thread and copy their snapshot; requests, receipts and checkpoints run on
// the session's network lane, which the session joins before this object is destroyed. Each of those tasks is
// counted by the session's borrowed DialogueRequests response accounting from admission until it is destroyed. The
// reply's terminal hook runs in playback/presentation teardown and only enqueues a copy. A new runtime generation is a new session and object.
class PrivateNarrator final {
public:
    using Snapshot = std::shared_ptr<const core::RuntimeSnapshot>;
    using Notify = std::function<void(std::string)>;
    using TerminalHook = std::function<void(presentation::DialogueDelivery, bool)>;
    // Network worker, holding begin_context_turn(): publish the copied snapshot and return its exact binding.
    using Publish = std::function<RequestOutcome(const Snapshot&, core::CancellationToken)>;
    // Network worker: hand one reply to speech delivery with a caption hook for its terminal delivery.
    using Present = std::function<void(const protocol_native::Line&, const protocol_native::Dialogue&, const Snapshot&,
                                       const core::CancellationToken&, TerminalHook)>;
    // Network worker: a committed private change must be covered by a later saved context ACK.
    using Changed = std::function<void(std::uint64_t, const core::CancellationToken&)>;

    struct Dependencies final {
        SynthClient& client;
        tasks::TaskLanes& network;
        DialogueRequests& requests;
        const std::atomic_bool& terminal;
        core::CancellationToken session;
        Publish publish;
        Present present;
        Changed changed;
        Notify notify;
        std::uint32_t timeout_ms{30'000};
    };

    enum class Command { help, list, select, talk, npcs, scene, transcript, older, cancel };
    struct TextCommand final {
        Command command{Command::help};
        std::string argument;
    };

    struct View final {
        bool available{};
        bool talking{};
        bool busy{};
        bool loaded{};
        protocol_native::NarratorState state;
    };

    explicit PrivateNarrator(Dependencies dependencies)
        : client_{dependencies.client}, network_{dependencies.network}, requests_{dependencies.requests},
          terminal_{dependencies.terminal},
          session_{std::move(dependencies.session)}, publish_{std::move(dependencies.publish)},
          present_{std::move(dependencies.present)}, changed_{std::move(dependencies.changed)},
          notify_{std::move(dependencies.notify)}, timeout_ms_{dependencies.timeout_ms}, transcript_lease_{ui::narrator_transcript.claim()} {
        if (!publish_ || !present_ || !changed_ || !notify_ || timeout_ms_ == 0)
            throw std::invalid_argument{"private narrator requires publication, presentation, frontier and notification sinks"};
    }

    PrivateNarrator(const PrivateNarrator&) = delete;
    PrivateNarrator& operator=(const PrivateNarrator&) = delete;

    // `/narrator` alone shows the controls; anything else that does not start with `/narrator` is not a command.
    [[nodiscard]] static std::optional<TextCommand> parse(std::string_view text) {
        const auto trim = [](std::string_view value) {
            const auto begin = value.find_first_not_of(" \t\r\n");
            if (begin == std::string_view::npos) return std::string_view{};
            return value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
        };
        text = trim(text);
        if (text != "/narrator" && !text.starts_with("/narrator ")) return std::nullopt;
        auto rest = trim(text.substr(9));
        const auto space = rest.find(' ');
        const auto word = rest.substr(0, space);
        const auto argument = space == std::string_view::npos ? std::string_view{} : trim(rest.substr(space));
        constexpr std::pair<std::string_view, Command> words[]{{"", Command::help}, {"help", Command::help},
            {"list", Command::list}, {"select", Command::select}, {"talk", Command::talk}, {"npcs", Command::npcs},
            {"npc", Command::npcs}, {"scene", Command::scene}, {"transcript", Command::transcript},
            {"older", Command::older},
            {"cancel", Command::cancel}};
        for (const auto& [name, command] : words)
            if (word == name && (command == Command::select) == !argument.empty()) return TextCommand{command, std::string{argument}};
        return TextCommand{Command::help, {}};
    }

    [[nodiscard]] static constexpr std::string_view help_text() noexcept {
        return "SYNTH private narrator: /narrator list, /narrator select <number or name>, /narrator talk, "
               "/narrator npcs, /narrator scene, /narrator transcript, /narrator older, /narrator cancel";
    }

    [[nodiscard]] bool ready() const noexcept { return client_.private_narrator_ready() && !terminal_.load(std::memory_order_acquire); }
    [[nodiscard]] bool talking() const noexcept { return talking_.load(std::memory_order_acquire) && ready(); }

    [[nodiscard]] View view() const {
        std::scoped_lock lock{mutex_};
        return View{ready(), talking(), turn_.has_value() && !turn_->is_cancelled(), known_.has_value(), state_};
    }

    // Game thread. Routing preference only; it never touches the network.
    bool set_talking(bool narrator) {
        if (narrator && !ready()) { notify_("SYNTH private narrator is unavailable on this server"); return false; }
        talking_.store(narrator, std::memory_order_release);
        if (!narrator) { notify_("SYNTH is talking to NPCs"); return true; }
        const auto selected = view().state.selected;
        notify_(selected ? "SYNTH is talking privately to " + selected->display_name
                         : "SYNTH is talking privately to the narrator; select one with /narrator select <number>");
        return true;
    }

    // Game thread: a `/narrator` command, or ordinary text while talking to the narrator. False leaves the text to NPCs.
    [[nodiscard]] bool handle_text(Snapshot snapshot, std::string_view text) {
        auto parsed = parse(text);
        if (!parsed) return talking() && say(std::move(snapshot), std::string{text});
        if (!ready()) { notify_("SYNTH private narrator is unavailable on this server"); return true; }
        switch (parsed->command) {
        case Command::help: notify_(std::string{help_text()}); (void)refresh(std::move(snapshot)); break;
        case Command::list: (void)refresh(std::move(snapshot), true); break;
        case Command::select: (void)select_named(std::move(snapshot), parsed->argument); break;
        case Command::talk: (void)set_talking(true); break;
        case Command::npcs: (void)set_talking(false); break;
        case Command::scene: (void)scene(std::move(snapshot)); break;
        case Command::transcript: (void)submit_command(std::move(snapshot), protocol_native::NarratorAction::transcript, {}, false); break;
        case Command::older:
            if (const auto cursor = view().state.next_cursor; !cursor.empty())
                (void)submit_command(std::move(snapshot), protocol_native::NarratorAction::transcript, {}, false, cursor);
            else notify_("No older transcript page; use /narrator transcript to start again");
            break;
        case Command::cancel: (void)cancel(std::move(snapshot)); break;
        }
        return true;
    }

    [[nodiscard]] bool refresh(Snapshot snapshot, bool announce = false) {
        return submit_command(std::move(snapshot), protocol_native::NarratorAction::state, {}, announce);
    }

    [[nodiscard]] bool select(Snapshot snapshot, std::string definition_id) {
        if (!protocol_native::narrator_definition_id(definition_id)) return false;
        cancel_local();
        return submit_command(std::move(snapshot), protocol_native::NarratorAction::select, std::move(definition_id), true);
    }

    // 1-based number from the last list, else a case-insensitive name.
    [[nodiscard]] bool select_named(Snapshot snapshot, std::string_view argument) {
        const auto state = view().state;
        const auto lower = [](std::string_view value) {
            std::string out{value};
            std::ranges::transform(out, out.begin(), [](unsigned char ch) { return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch); });
            return out;
        };
        std::optional<std::string> found;
        std::size_t number{};
        for (const auto ch : argument) {
            if (ch < '0' || ch > '9' || number > 99) { number = 0; break; }
            number = number * 10 + static_cast<std::size_t>(ch - '0');
        }
        if (number >= 1 && number <= state.available.size()) found = state.available[number - 1].definition_id;
        for (const auto& definition : state.available)
            if (!found && lower(definition.display_name) == lower(argument)) found = definition.definition_id;
        if (!found) {
            notify_("SYNTH private narrator not found; use /narrator list");
            (void)refresh(std::move(snapshot));
            return false;
        }
        return select(std::move(snapshot), *found);
    }

    // Cancel Reply: the live turn and its playback stop locally at once; the server retires every unfinished reply.
    [[nodiscard]] bool cancel(Snapshot snapshot) {
        cancel_local();
        return submit_command(std::move(snapshot), protocol_native::NarratorAction::cancel, {}, false);
    }

    [[nodiscard]] bool scene(Snapshot snapshot) {
        return submit_turn(std::move(snapshot), protocol_native::NarratorTurnKind::scene, {}, {});
    }

    [[nodiscard]] bool say(Snapshot snapshot, std::string text) {
        if (text.empty() || text.size() > 4096) return false;
        return submit_turn(std::move(snapshot), protocol_native::NarratorTurnKind::typed, std::move(text), {});
    }

    [[nodiscard]] bool say_voice(Snapshot snapshot, std::vector<std::byte> wave) {
        if (wave.empty()) return false;
        return submit_turn(std::move(snapshot), protocol_native::NarratorTurnKind::voice, {}, std::move(wave));
    }

    // Dialogue Stop, combat interruption or hard halt: the live narrator turn ends locally; nothing is sent.
    void cancel_local() {
        std::scoped_lock lock{mutex_};
        if (turn_) turn_->cancel();
        turn_.reset();
    }

    void show_transcript() {
        const auto current = view();
        if (!current.loaded) { notify_("SYNTH private narrator transcript is not loaded yet; try /narrator list"); return; }
        if (current.state.transcript.empty()) { notify_("SYNTH private narrator transcript is empty"); return; }
        ui::NarratorTranscript::Page page;
        page.older = !current.state.next_cursor.empty();
        for (const auto& entry : current.state.transcript) page.entries.emplace_back(entry.display_name, entry.text);
        ui::narrator_transcript.show(transcript_lease_, std::move(page));
        notify_("Private transcript is in the chat window; /narrator older opens the previous page");
    }

    // Game thread: one bounded background drain of copied reply receipts.
    void pump(std::uint64_t generation) {
        std::deque<PendingReceipt> pending;
        {
            std::scoped_lock lock{mutex_};
            if (receipts_.empty() || draining_) return;
            for (auto& value : receipts_) if (value.generation == generation) pending.push_back(std::move(value));
            receipts_.clear();
            if (pending.empty()) return;
            draining_ = true;
        }
        // Owned by the submitted task: a rejected, expired, cancelled or finished drain releases the slot once.
        struct Release final {
            explicit Release(PrivateNarrator* value) noexcept : owner{value} {}
            Release(const Release&) = delete;
            Release& operator=(const Release&) = delete;
            ~Release() { std::scoped_lock lock{owner->mutex_}; owner->draining_ = false; }
            PrivateNarrator* owner;
        };
        auto release = std::make_shared<Release>(this);
        const auto accepted = network_.try_submit("network", tasks::TaskClass::background,
            tasks::TaskLanes::Clock::now() + std::chrono::seconds{10}, session_,
            requests_.track_response([this, release, pending = std::move(pending)](const core::CancellationToken& cancellation) {
                for (const auto& value : pending) {
                    if (stopped(cancellation) || value.generation != cancellation.generation().value()) break;
                    try { send_receipt(value, cancellation); } catch (const std::exception&) {}
                }
            }));
        (void)accepted;
    }

    // Native adapters capture only when a private change still needs a covering saved ACK.
    [[nodiscard]] bool checkpoint_due() const {
        std::scoped_lock lock{mutex_};
        return changed_serial_ > covered_serial_ && !checkpointing_ && tasks::TaskLanes::Clock::now() >= next_checkpoint_;
    }

    [[nodiscard]] bool checkpoint(Snapshot snapshot) {
        std::uint64_t serial{};
        {
            std::scoped_lock lock{mutex_};
            if (changed_serial_ <= covered_serial_ || checkpointing_) return false;
            serial = changed_serial_;
            checkpointing_ = true;
            next_checkpoint_ = tasks::TaskLanes::Clock::now() + std::chrono::seconds{5};
        }
        struct Release final {
            explicit Release(PrivateNarrator* value) : owner{value} {}
            PrivateNarrator* owner;
            ~Release() { std::scoped_lock lock{owner->mutex_}; owner->checkpointing_ = false; }
        };
        auto release = std::make_shared<Release>(this);
        const auto accepted = network_.try_submit("network", tasks::TaskClass::background,
            tasks::TaskLanes::Clock::now() + std::chrono::milliseconds{timeout_ms_}, session_,
            requests_.track_response([this, release, serial, snapshot = std::move(snapshot)](const core::CancellationToken& cancellation) {
                bool saved{};
                try {
                    if (!stopped(cancellation)) {
                        const auto context_turn = client_.begin_context_turn();
                        saved = publish_(snapshot, cancellation).save_checkpoint_acknowledged;
                    }
                } catch (const std::exception&) {}
                std::scoped_lock lock{mutex_};
                if (saved && !stopped(cancellation)) covered_serial_ = std::max(covered_serial_, serial);
            }));
        if (accepted != tasks::SubmitResult::accepted) {
            return false;
        }
        return true;
    }

private:
    struct PendingReceipt final {
        std::uint64_t generation{};
        ContextBinding binding;
        protocol_native::NarratorReceipt receipt;
    };

    [[nodiscard]] bool stopped(const core::CancellationToken& cancellation) const noexcept {
        return cancellation.is_cancelled() || terminal_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool admissible(const Snapshot& snapshot) {
        if (!snapshot || snapshot->generation() != session_.generation() || session_.is_cancelled()) return false;
        if (!ready()) { notify_("SYNTH private narrator is unavailable on this server"); return false; }
        return true;
    }

    [[nodiscard]] bool submit_command(Snapshot snapshot, protocol_native::NarratorAction action, std::string definition_id,
                                      bool announce, std::string cursor = {}) {
        if (!admissible(snapshot)) return false;
        const auto accepted = network_.try_submit("network", tasks::TaskClass::background,
            tasks::TaskLanes::Clock::now() + std::chrono::milliseconds{timeout_ms_}, session_,
            requests_.track_response([this, snapshot = std::move(snapshot), action, definition_id = std::move(definition_id), announce,
                                      cursor = std::move(cursor)](const core::CancellationToken& cancellation) {
                if (stopped(cancellation)) return;
                try {
                    const auto context_turn = client_.begin_context_turn();
                    const auto context = publish_(snapshot, cancellation);
                    if (context.status != RequestStatus::complete) {
                        if (!stopped(cancellation)) notify_("SYNTH private narrator could not publish the scene: " + context.detail);
                        return;
                    }
                    const auto outcome = client_.send_narrator("narrator_command", context.context_binding,
                        [&](const protocol_native::EventContext& event) {
                            return protocol_native::encode_narrator_command(event, action, definition_id, cursor);
                        });
                    const auto mutating = action == protocol_native::NarratorAction::select;
                    if (!absorb(outcome, cancellation, mutating) || stopped(cancellation)) {
                        if (!stopped(cancellation)) notify_("SYNTH private narrator: " + failure(outcome));
                        return;
                    }
                    if (action == protocol_native::NarratorAction::cancel) notify_("SYNTH private narrator reply cancelled");
                    else if (action == protocol_native::NarratorAction::transcript) show_transcript();
                    else if (announce) announce_state(action == protocol_native::NarratorAction::select);
                } catch (const std::exception& error) {
                    if (!stopped(cancellation)) notify_(std::string{"SYNTH private narrator: "} + error.what());
                }
            }));
        if (accepted != tasks::SubmitResult::accepted) { notify_("SYNTH network queue is full; narrator request discarded"); return false; }
        return true;
    }

    [[nodiscard]] bool submit_turn(Snapshot snapshot, protocol_native::NarratorTurnKind kind, std::string text,
                                   std::vector<std::byte> wave) {
        if (!admissible(snapshot)) return false;
        protocol_native::NarratorTurn turn{kind, {}, {}, std::move(text), std::nullopt};
        std::optional<core::CancellationToken> owner;
        {
            std::scoped_lock lock{mutex_};
            if (!known_ || !state_.selected) {
                // Selection belongs to the save; never guess one.
            } else {
                turn.definition_id = state_.selected->definition_id;
                turn.state_id = state_.state_id;
                if (turn_) turn_->cancel();
                turn_.emplace(session_);
                owner = turn_->token();
            }
        }
        if (!owner) {
            notify_("SYNTH private narrator: select a narrator first (/narrator list, then /narrator select <number>)");
            (void)refresh(std::move(snapshot));
            return false;
        }
        const auto accepted = network_.try_submit("network", tasks::TaskClass::current_turn,
            tasks::TaskLanes::Clock::now() + std::chrono::milliseconds{timeout_ms_ * 2ULL}, *owner,
            requests_.track_response([this, snapshot = std::move(snapshot), turn = std::move(turn), wave = std::move(wave)](
                const core::CancellationToken& cancellation) mutable {
                if (stopped(cancellation)) return;
                try { run_turn(snapshot, std::move(turn), std::move(wave), cancellation); }
                catch (const std::exception& error) {
                    if (!stopped(cancellation)) notify_(std::string{"SYNTH private narrator: "} + error.what());
                }
            }));
        if (accepted != tasks::SubmitResult::accepted) { notify_("SYNTH network queue is full; narrator input discarded"); return false; }
        return true;
    }

    // Network worker. Upload, publish and the turn share one context transaction; the player's input is recorded by the
    // server before its provider call, and the reply is presented only while this turn still owns the narrator.
    void run_turn(const Snapshot& snapshot, protocol_native::NarratorTurn turn, std::vector<std::byte> wave,
                  const core::CancellationToken& cancellation) {
        if (turn.kind == protocol_native::NarratorTurnKind::voice) {
            const auto sha256 = media_fetch::sha256_hex(wave);
            turn.media = protocol_native::Media{"media/" + sha256, "audio/wav", sha256, static_cast<std::uint64_t>(wave.size())};
            std::string body(reinterpret_cast<const char*>(wave.data()), wave.size());
            const auto upload = client_.upload_media(*turn.media, std::move(body), [this, cancellation] { return stopped(cancellation); });
            if (!upload.delivered() || upload.response.status < 200 || upload.response.status >= 300) {
                if (!stopped(cancellation)) notify_("SYNTH narrator microphone upload failed");
                return;
            }
        }
        const auto context_turn = client_.begin_context_turn();
        if (stopped(cancellation)) return;
        const auto context = publish_(snapshot, cancellation);
        if (context.status != RequestStatus::complete) {
            if (!stopped(cancellation)) notify_("SYNTH private narrator could not publish the scene: " + context.detail);
            return;
        }
        auto binding = context.context_binding;
        binding.cancellation = cancellation;
        const auto outcome = client_.send_narrator("narrator_turn", binding, [&](const protocol_native::EventContext& event) {
            return protocol_native::encode_narrator_turn(event, turn);
        });
        (void)absorb(outcome, cancellation, true);
        if (stopped(cancellation)) return;
        const protocol_native::Line* reply_line{};
        for (const auto& line : outcome.lines)
            if (std::holds_alternative<protocol_native::NarratorReply>(line.payload)) reply_line = &line;
        if (outcome.status != RequestStatus::complete || !reply_line) {
            notify_("SYNTH private narrator: " + failure(outcome));
            if (failure_code(outcome) == "narrator_state_changed" || failure_code(outcome) == "narrator_selection_changed")
                resync(context.context_binding, cancellation);
            return;
        }
        const auto& reply = std::get<protocol_native::NarratorReply>(reply_line->payload);
        if (reply.narrator.definition_id != turn.definition_id) return;
        auto line = *reply_line;
        protocol_native::Dialogue dialogue{protocol_native::Narrator{"narrator", reply.narrator.display_name}, reply.text,
                                           std::nullopt, reply.speech, std::nullopt, false};
        line.payload = dialogue;
        PendingReceipt pending{line.generation, ContextBinding{line.generation, line.context_sequence, {}, outcome.turn_id},
                               {line.request_id, reply.narrator, reply.parent_state_id, {}}};
        present_(line, dialogue, snapshot, cancellation,
            [this, pending = std::move(pending)](presentation::DialogueDelivery terminal, bool cancelled) mutable {
                using presentation::DialogueDelivery;
                if (terminal == DialogueDelivery::text_complete) return; // Text alone is never heard.
                pending.receipt.outcome = terminal == DialogueDelivery::spoken && !cancelled
                    ? protocol_native::NarratorReceiptOutcome::audio_completed
                    : cancelled ? protocol_native::NarratorReceiptOutcome::interrupted : protocol_native::NarratorReceiptOutcome::failed;
                try {
                    std::scoped_lock lock{mutex_};
                    if (receipts_.size() < 4) receipts_.push_back(std::move(pending));
                } catch (...) {}
            });
    }

    // Network worker. Receipts are never retried; only an exact accepted completed-audio receipt raises the frontier.
    void send_receipt(const PendingReceipt& value, const core::CancellationToken& cancellation) {
        auto binding = value.binding;
        binding.cancellation = cancellation;
        const auto outcome = client_.send_narrator("narrator_receipt", binding, [&](const protocol_native::EventContext& event) {
            return protocol_native::encode_narrator_receipt(event, value.receipt);
        });
        const auto* status = outcome.lines.size() == 4 ? std::get_if<protocol_native::Status>(&outcome.lines[1].payload) : nullptr;
        const auto accepted = outcome.status == RequestStatus::complete && status && status->code == "narrator_receipt_accepted";
        const auto spoken = accepted && status->detail == "spoken" &&
            value.receipt.outcome == protocol_native::NarratorReceiptOutcome::audio_completed;
        (void)absorb(outcome, cancellation, spoken);
        if (!accepted && !stopped(cancellation) && failure_code(outcome) == "narrator_reply_stale")
            notify_("SYNTH private narrator reply was superseded and is not in the transcript");
    }

    // Network worker, still in the refused turn's context transaction: reload the authoritative state under the same
    // acknowledged binding so the player's next attempt names the current checkpoint.
    void resync(ContextBinding binding, const core::CancellationToken& cancellation) {
        binding.cancellation = cancellation;
        binding.turn_id.clear();
        try {
            (void)absorb(client_.send_narrator("narrator_command", binding, [](const protocol_native::EventContext& event) {
                return protocol_native::encode_narrator_command(event, protocol_native::NarratorAction::state);
            }), cancellation, false);
        } catch (const std::exception&) {}
    }

    // Adopt the newest narrator_state of this generation. A mutating request whose state differs from the one this
    // client last saw committed a private change, which makes a covering checkpoint due.
    bool absorb(const RequestOutcome& outcome, const core::CancellationToken& cancellation, bool mutating) {
        const protocol_native::NarratorState* latest{};
        std::uint64_t generation{};
        for (const auto& line : outcome.lines)
            if (const auto* state = std::get_if<protocol_native::NarratorState>(&line.payload)) { latest = state; generation = line.generation; }
        if (!latest || generation != session_.generation().value() || stopped(cancellation)) return latest != nullptr;
        bool changed{};
        {
            std::scoped_lock lock{mutex_};
            changed = mutating && (!known_ || *known_ != latest->state_id);
            known_ = latest->state_id;
            state_ = *latest;
            if (changed) {
                ++changed_serial_;
                next_checkpoint_ = tasks::TaskLanes::Clock::now();
            }
        }
        if (changed) changed_(generation, cancellation);
        return true;
    }

    void announce_state(bool selected) {
        const auto state = view().state;
        if (selected && state.selected) { notify_("SYNTH private narrator selected: " + state.selected->display_name); return; }
        if (state.available.empty()) { notify_("SYNTH has no private narrators; create one on the server Narrator page"); return; }
        std::string list{"SYNTH private narrators:"};
        for (std::size_t index = 0; index < state.available.size(); ++index) {
            const auto& definition = state.available[index];
            list += " " + std::to_string(index + 1) + ". " + definition.display_name;
            if (state.selected && state.selected->definition_id == definition.definition_id) {
                list += state.selected->revision_id == definition.revision_id ? " (selected)" : " (selected, older revision)";
            }
        }
        notify_(list);
    }

    [[nodiscard]] static std::string failure_code(const RequestOutcome& outcome) {
        for (const auto& line : outcome.lines)
            if (const auto* status = std::get_if<protocol_native::Status>(&line.payload); status && status->code != "narrator_receipt_accepted")
                return status->code == "narrator_failed" && !status->detail.empty() ? status->detail : status->code;
        return {};
    }

    [[nodiscard]] static std::string failure(const RequestOutcome& outcome) {
        const auto code = failure_code(outcome);
        if (code == "narrator_not_selected") return "select a narrator first";
        if (code == "narrator_scene_unavailable") return "no current scene to comment on";
        if (code == "narrator_cursor_stale") return "the transcript changed; use /narrator transcript again";
        if (code == "narrator_save_definition_limit") return "this save already uses 16 narrator definitions";
        if (code == "narrator_definition_changed") return "the narrator was edited while replying; try again";
        if (code == "narrator_state_changed" || code == "narrator_selection_changed") return "the narrator changed; try again";
        if (code == "narrator_speech_unavailable" || code == "speech_unavailable") return "the narrator's profile has no voice";
        if (code == "narrator_provider_unsupported") return "the narrator's profile has no supported language model";
        if (code == "speech_not_recognized") return "speech was not recognized; try again";
        if (!code.empty()) return code;
        return outcome.detail.empty() ? std::string{"request failed"} : outcome.detail;
    }

    SynthClient& client_;
    tasks::TaskLanes& network_;
    DialogueRequests& requests_;
    const std::atomic_bool& terminal_;
    const core::CancellationToken session_;
    const Publish publish_;
    const Present present_;
    const Changed changed_;
    const Notify notify_;
    const std::uint32_t timeout_ms_;
    const std::uint64_t transcript_lease_;
    std::atomic_bool talking_{};
    mutable std::mutex mutex_;
    std::optional<std::uint64_t> known_;
    protocol_native::NarratorState state_;
    std::optional<core::CancellationSource> turn_;
    std::deque<PendingReceipt> receipts_;
    bool draining_{};
    bool checkpointing_{};
    std::uint64_t changed_serial_{}, covered_serial_{};
    tasks::TaskLanes::Clock::time_point next_checkpoint_{};
};

}  // namespace synth::client
