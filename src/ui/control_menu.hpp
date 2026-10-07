#pragma once

// SYNTH's in-game Control menu.
//
// This is the second - and last - F4SE Menu Framework window SYNTH owns. It
// replaces the old "open the local Synthserver management UI" meaning of the
// SYNTH Control hotkey with a keyboard-navigable in-game menu: chat mode, LLM
// model slot, dynamic profile refreshes, and the "wait here" / "release wait"
// pair for the NPC the player is talking to.
//
// Threading. Exactly the arrangement `Chatbox` already uses, for the same
// reasons. The framework renders on the present thread, so nothing below
// touches an `RE::*` object, a live session, the network, or the filesystem.
// The game thread asks for the window and consumes selections; the render
// thread owns the menu cursor and never reads game state back.
//
// Deferred creation. The window is created from the framework's first
// `kBeforeRender`, not from F4SE kPostLoad: `AddWindow` mutates the framework's
// window registry and this build can stall if that happens while the renderer
// is still starting. `menu_framework_ui.hpp` owns the single lifecycle listener
// and calls `control_menu_tick()` from it, so both windows are created on the
// thread that owns that registry and SYNTH still registers one listener.
//
// The bridge. `ControlMenu` is plain C++ with no framework, F4SE or `RE::*`
// type in it, so flat and VR share it and it compiles and is testable with
// neither the framework nor the game present. It carries one bounded pending
// request: a second selection is refused while the first is unread rather than
// overwriting it, so a slow consumer can neither lose a selection nor gain a
// duplicate.
//
// Staleness. Every draft carries the runtime generation it was opened in plus a
// SYNTH-owned revision. A renderer still holding an obsolete receipt - because
// the save was reloaded, the session was retired, or the menu was reopened -
// cannot queue a selection against whatever replaced it, and every request the
// game thread takes names the generation it was made in, so the consumer can
// reject it a second time on its own terms.
//
// Confirmation. A selection the render thread submits shows as pending until
// the game thread calls `publish_status`. That call is the confirmation point:
// whatever it publishes becomes what the menu displays, and its status line is
// where a refusal is explained. The menu is therefore never a second store of
// the chat mode or the model slot - it echoes an in-flight write and nothing
// more, the same way the settings pages echo an in-flight INI write.
//
// Closing on submission. The two wait rows are the exception to that echo: the
// consumer applies a targeted action only once this menu is down, so keeping it
// up after an accepted submission would only ask the player to close it
// themselves. They therefore take the ordinary close path, which drops the
// draft and leaves the queued request alone - the generation-tagged selection
// outlives the window it was made in and the game thread still takes it exactly
// once.

#include "ui/settings_catalog.hpp"

#if defined(SYNTH_WITH_F4SE) && defined(__has_include)
#  if __has_include("F4SEMenuFramework.h")
#    define SYNTH_HAS_MENU_FRAMEWORK 1
#  endif
#endif
#ifndef SYNTH_HAS_MENU_FRAMEWORK
#  define SYNTH_HAS_MENU_FRAMEWORK 0
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#if SYNTH_HAS_MENU_FRAMEWORK
// The consumer header declares an input-event callback over `RE::InputEvent`,
// so the CommonLibF4 types must be visible first.
#include <F4SE/F4SE.h>

#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4099)  // struct/class mismatch inside the vendor header
#  pragma warning(disable : 5054)  // deprecated enum-to-enum operators in the ImGui shim
#  pragma warning(disable : 4996)  // vendor FontAwesome helper still uses <codecvt>
#endif
#ifndef SYNTH_MENU_FRAMEWORK_API_INCLUDED
#define SYNTH_MENU_FRAMEWORK_API_INCLUDED
#include "F4SEMenuFramework.h"
#endif
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif
#endif

namespace synth::ui {

// What the Control menu has asked the game thread to do. The render thread only
// ever names one of these; every session, network and game step the request
// implies belongs to the thread that takes it.
enum class ControlAction : unsigned char {
    chat_mode,         // value: a `chat_modes` catalog value
    model_slot,        // value: an `llm_models` catalog value
    profile_target,    // value: the target's display name, for the caller's log line
    profile_nearby,    // value: empty
    profile_narrator,  // value: empty
    wait_here,         // value: the target's display name, for the caller's log line
    release_wait,      // value: the target's display name, for the caller's log line
    open_dashboard,    // value: empty
};

// One selection, handed over exactly once.
struct ControlRequest final {
    // The runtime generation the menu was opened in. The consumer rejects the
    // request outright when its own generation has moved on, so a selection made
    // before a load can never be applied after one.
    std::uint64_t generation{};
    ControlAction action{};
    std::string value;
};

namespace detail {

// Why a submitted selection was, or was not, handed to the game thread. The
// render thread turns this into the line it shows the player.
enum class ControlSubmit : unsigned char {
    accepted,
    busy,   // the previous selection has not been read yet
    stale,  // the menu was closed, reopened, or its generation retired
};

// Engine-free hand-off between the framework render thread, which owns the menu
// cursor, and the game thread, which owns everything that can act on a
// selection. It holds no `RE::*` object, no F4SE handle and no session.
class ControlMenuBridge final {
public:
    // A menu row is a caption, not a payload: bound every string the render
    // thread copies each frame so a runaway publisher cannot grow them.
    static constexpr std::size_t label_limit = 128;
    static constexpr std::size_t status_limit = 240;

    // A draft receipt is copied atomically with its generation and target, never
    // read separately.
    struct Draft final {
        std::uint64_t revision{};
        std::uint64_t generation{};
        std::string target_name;
    };

    // What the render thread draws. One copy per frame, taken under the lock and
    // read outside it, so no SYNTH lock is held while ImGui draws.
    struct View final {
        std::string mode;           // authoritative chat mode
        std::string model;          // authoritative model slot
        std::string status;         // last line the game thread published
        std::string pending_mode;   // empty unless a mode change is unconfirmed
        std::string pending_model;  // empty unless a slot change is unconfirmed
        std::optional<ControlAction> pending_action;  // unconfirmed one-shot
    };

    // Game thread: ask for the menu. Refused while a draft is still up or a
    // selection is still unread, so the player cannot open a second menu over an
    // in-flight one.
    bool request_open(std::uint64_t generation, std::string target_name, std::string mode,
                      std::string model) {
        auto name = clamp(std::move(target_name), label_limit);
        auto mode_text = clamp(std::move(mode), label_limit);
        auto model_text = clamp(std::move(model), label_limit);
        std::scoped_lock lock{mutex_};
        if (open_pending_ || draft_ || request_) return false;
        draft_ = Draft{++revision_, generation, std::move(name)};
        mode_ = std::move(mode_text);
        model_ = std::move(model_text);
        status_.clear();
        clear_pending_locked();
        open_pending_ = true;
        open_.store(true, std::memory_order_relaxed);
        return true;
    }

    // Render thread: one receipt per accepted open request.
    [[nodiscard]] std::optional<Draft> consume_open_request() {
        std::scoped_lock lock{mutex_};
        if (!open_pending_) return {};
        open_pending_ = false;
        return draft_;
    }

    // Render thread: offer the game thread one selection. Bound to the receipt
    // that drew the row, so a renderer holding an obsolete one cannot act on the
    // draft that replaced it. A second selection is refused while the first is
    // unread rather than overwriting it.
    [[nodiscard]] ControlSubmit submit(ControlAction action, std::string value,
                                       std::uint64_t revision) {
        auto payload = clamp(std::move(value), label_limit);
        std::scoped_lock lock{mutex_};
        if (!draft_ || draft_->revision != revision) return ControlSubmit::stale;
        if (request_) return ControlSubmit::busy;
        request_ = ControlRequest{draft_->generation, action, std::move(payload)};
        note_pending_locked(action, request_->value);
        return ControlSubmit::accepted;
    }

    // Game thread: take the pending selection exactly once. Empty on every pump
    // but the one that follows a selection.
    [[nodiscard]] std::optional<ControlRequest> take_request() {
        std::scoped_lock lock{mutex_};
        return std::exchange(request_, std::nullopt);
    }

    // Game thread: publish what is now true. This is the confirmation point for
    // every pending echo - including a refused one, which the status line
    // explains - so a selection the consumer declined cannot stay on screen as
    // though it had been applied.
    void publish_status(std::string mode, std::string model, std::string status) {
        auto mode_text = clamp(std::move(mode), label_limit);
        auto model_text = clamp(std::move(model), label_limit);
        auto status_text = clamp(std::move(status), status_limit);
        std::scoped_lock lock{mutex_};
        mode_ = std::move(mode_text);
        model_ = std::move(model_text);
        status_ = std::move(status_text);
        clear_pending_locked();
    }

    // Render thread: everything the menu draws, in one copy.
    [[nodiscard]] View status() const {
        std::scoped_lock lock{mutex_};
        return View{mode_, model_, status_, pending_mode_, pending_model_, pending_action_};
    }

    // Render thread: the player closed the menu. A renderer holding an obsolete
    // receipt cannot close the draft that replaced it. A submitted selection
    // deliberately survives: closing the menu is not a way to take back a choice
    // the player already made.
    void cancel(std::uint64_t revision) {
        std::scoped_lock lock{mutex_};
        if (!draft_ || draft_->revision != revision) return;
        draft_.reset();
        open_pending_ = false;
        open_.store(false, std::memory_order_relaxed);
    }

    // Game thread, lock-free: whether the menu is up right now, for pausing on
    // it. A cached mirror written at every state change, not a framework read.
    [[nodiscard]] bool is_open() const noexcept {
        return open_.load(std::memory_order_relaxed);
    }

    // Game thread: drop every queued request and echo. Used when the session the
    // selection was meant for is gone, so nothing survives into the next one.
    // Bumping the revision alongside means a renderer still holding the old
    // receipt cannot queue a replacement selection against whatever comes next.
    void invalidate() {
        std::scoped_lock lock{mutex_};
        open_pending_ = false;
        draft_.reset();
        request_.reset();
        mode_.clear();
        model_.clear();
        status_.clear();
        clear_pending_locked();
        ++revision_;
        open_.store(false, std::memory_order_relaxed);
    }

private:
    void clear_pending_locked() noexcept {
        pending_mode_.clear();
        pending_model_.clear();
        pending_action_.reset();
    }

    void note_pending_locked(ControlAction action, const std::string& value) {
        switch (action) {
        case ControlAction::chat_mode:
            pending_mode_ = value;
            pending_action_.reset();
            break;
        case ControlAction::model_slot:
            pending_model_ = value;
            pending_action_.reset();
            break;
        default:
            // The one-shots have no slot to echo, so the menu names the action
            // itself until the game thread answers.
            pending_action_ = action;
            break;
        }
    }

    // Never split a UTF-8 sequence: step back off any continuation byte.
    [[nodiscard]] static std::string clamp(std::string text, std::size_t limit) {
        if (text.size() <= limit) return text;
        auto cut = limit;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) --cut;
        text.resize(cut);
        return text;
    }

    mutable std::mutex mutex_;
    std::optional<Draft> draft_;
    std::optional<ControlRequest> request_;
    std::string mode_;
    std::string model_;
    std::string status_;
    std::string pending_mode_;
    std::string pending_model_;
    std::optional<ControlAction> pending_action_;
    std::uint64_t revision_{};
    bool open_pending_{};
    mutable std::atomic<bool> open_{false};
};

inline ControlMenuBridge control_menu{};

}  // namespace detail

#if SYNTH_HAS_MENU_FRAMEWORK

namespace detail {

// Which list the menu is showing. Render thread only.
enum class ControlLevel : unsigned char {
    root,
    chat_mode,
    llm_model,
    profiles,
};

// Owned by the framework; SYNTH only flips the atomics on it and never touches
// the framework's own main menu. The pointer is atomic because the render thread
// publishes it - creation is deferred to `kBeforeRender` - while the game thread
// reads it. Release/acquire, so a reader that sees the pointer also sees the
// `WindowInterface` the framework built behind it.
inline std::atomic<F4SEMenuFramework::Model::WindowInterface*> control_menu_window{nullptr};
inline bool control_menu_window_requested = false;  // render thread only

// Render thread only, so none of this needs a lock.
inline std::optional<ControlMenuBridge::Draft> control_menu_draft;
inline ControlLevel control_menu_level = ControlLevel::root;
inline const char* control_menu_notice = nullptr;

inline void clear_control_menu_state() noexcept {
    control_menu_level = ControlLevel::root;
    control_menu_notice = nullptr;
}

// Game thread. No bridge lock is held while the window pointer is read, and the
// framework's visibility flag is published by the render lifecycle, so an older
// render close cannot lose this request between the bridge and that flag.
inline bool open_control_menu(std::uint64_t generation, std::string target_name, std::string mode,
                              std::string model) {
    if (control_menu_window.load(std::memory_order_acquire) == nullptr) return false;
    return control_menu.request_open(generation, std::move(target_name), std::move(mode),
                                     std::move(model));
}

// The player-facing name of a value, or the raw value when the catalog does not
// know it - a server that grows a mode before the client does still reads.
[[nodiscard]] inline const char* control_label(std::span<const ControlOption> options,
                                               const std::string& value) {
    for (const auto& option : options) {
        if (value == option.value) return option.label;
    }
    return value.empty() ? "unset" : value.c_str();
}

[[nodiscard]] inline const char* pending_action_label(ControlAction action) {
    switch (action) {
    case ControlAction::profile_target: return "Refreshing this NPC's profile...";
    case ControlAction::profile_nearby: return "Refreshing nearby NPC profiles...";
    case ControlAction::profile_narrator: return "Refreshing The Narrator's profile...";
    // The wait rows close the menu as soon as the game thread owns the
    // selection, so in practice these two are drawn for no frame at all. They
    // are still named here: the label belongs to the action, not to the row that
    // happened to queue it, and a caller that submits one without closing - a
    // test, or a future row - must not fall through to "Applying...".
    case ControlAction::wait_here: return "Asking this NPC to wait here...";
    case ControlAction::release_wait: return "Asking this NPC to stop waiting...";
    case ControlAction::open_dashboard: return "Opening the server dashboard...";
    default: return "Applying...";
    }
}

// One keyboard-navigable row. The framework's ImGui build drives arrow keys and
// Enter across `Selectable`s itself; SYNTH only says which row a freshly opened
// list should start on.
[[nodiscard]] inline bool control_row(const std::string& label, bool focus) {
    const auto activated = ImGuiMCP::Selectable(label.c_str(), false);
    if (focus) ImGuiMCP::SetItemDefaultFocus();
    return activated;
}

inline void describe_option(const ControlOption& option) {
    if (option.help == nullptr || *option.help == '\0') return;
    if (ImGuiMCP::IsItemHovered()) ImGuiMCP::SetTooltip("%s", option.help);
}

// Queue a selection and say what happened. Never called without a draft.
// Returns whether the game thread now owns the selection, so a row that closes
// the menu on submission closes it only when there is something to close it
// for - a refused selection keeps the window and its explanation up.
inline bool control_submit(ControlAction action, std::string value) {
    switch (control_menu.submit(action, std::move(value), control_menu_draft->revision)) {
    case ControlSubmit::accepted:
        control_menu_level = ControlLevel::root;
        control_menu_notice = nullptr;
        return true;
    case ControlSubmit::busy:
        control_menu_notice = "SYNTH is still applying the last selection. Try again in a moment.";
        break;
    case ControlSubmit::stale:
        control_menu_notice = "This menu expired. Close it and open a new one.";
        break;
    }
    return false;
}

// Render thread. Reads its own cursor and one copied bridge view and nothing
// else: no `RE::*` object, no session, no configuration snapshot, no framework
// close call, no filesystem and no network.
inline void __stdcall render_control_menu() {
    auto* window = control_menu_window.load(std::memory_order_acquire);
    if (window == nullptr) return;
    // Defensive: a framework that polls closed windows must not draw one.
    if (!window->IsOpen.load()) return;

    if (auto draft = control_menu.consume_open_request()) {
        control_menu_draft = std::move(draft);
        clear_control_menu_state();
    }
    if (!control_menu_draft) { window->IsOpen.store(false); return; }

    const auto view = control_menu.status();
    const auto& target = control_menu_draft->target_name;

    ImGuiMCP::SetNextWindowSize(ImGuiMCP::ImVec2{420.0f, 380.0f},
                                ImGuiMCP::ImGuiCond_FirstUseEver);
    auto keep_open = true;
    auto go_back = false;
    if (ImGuiMCP::Begin("SYNTH Control##SYNTH.Control", &keep_open,
                        ImGuiMCP::ImGuiWindowFlags_NoSavedSettings)) {
        ImGuiMCP::TextDisabled(
            "Arrow keys move, Enter selects, Escape goes back and closes this menu.");
        if (!target.empty()) ImGuiMCP::TextWrapped("Talking to: %s", target.c_str());
        ImGuiMCP::Spacing();

        switch (control_menu_level) {
        case ControlLevel::root: {
            ImGuiMCP::SeparatorText("SYNTH Control");
            // Each row shows the pending value while one is unconfirmed, so the
            // player reads what they chose rather than what is still in force.
            const auto mode_row =
                std::string{"Chat Mode  ["} +
                control_label(chat_modes,
                              view.pending_mode.empty() ? view.mode : view.pending_mode) +
                (view.pending_mode.empty() ? "]" : "] (pending)") + "##SYNTH.Control.ChatMode";
            if (control_row(mode_row, true)) control_menu_level = ControlLevel::chat_mode;

            const auto model_row =
                std::string{"LLM Mode  ["} +
                control_label(llm_models,
                              view.pending_model.empty() ? view.model : view.pending_model) +
                (view.pending_model.empty() ? "]" : "] (pending)") + "##SYNTH.Control.LlmMode";
            if (control_row(model_row, false)) control_menu_level = ControlLevel::llm_model;

            if (control_row("Dynamic Profiles##SYNTH.Control.Profiles", false)) {
                control_menu_level = ControlLevel::profiles;
            }

            // The wait pair, side by side, so asking an NPC to stay put and
            // letting them go are read as the one decision they are. Both name
            // the NPC the draft was opened against and neither can retarget:
            // the value is the draft's target, and the game thread revalidates
            // that actor before it acts.
            //
            // Both drop `keep_open` on an accepted submission. That runs the
            // ordinary close below - `cancel` on this draft's own revision -
            // which is deliberately not a way to take back a selection already
            // made, so the generation-tagged request stays queued for the game
            // thread and is still taken exactly once.
            const auto wait_row = target.empty()
                                      ? std::string{"Wait Here##SYNTH.Control.WaitHere"}
                                      : "Wait Here  [" + target + "]##SYNTH.Control.WaitHere";
            if (control_row(wait_row, false) && control_submit(ControlAction::wait_here, target)) {
                keep_open = false;
            }

            const auto release_row =
                target.empty()
                    ? std::string{"Release Wait##SYNTH.Control.ReleaseWait"}
                    : "Release Wait  [" + target + "]##SYNTH.Control.ReleaseWait";
            if (control_row(release_row, false) &&
                control_submit(ControlAction::release_wait, target)) {
                keep_open = false;
            }

            // Explicit, and kept apart from every in-game control: this is the
            // one row that asks for something outside the game. The game thread
            // owns whether and how the dashboard is opened.
            if (control_row("Open Server Dashboard##SYNTH.Control.Dashboard", false)) {
                control_submit(ControlAction::open_dashboard, {});
            }

            ImGuiMCP::Spacing();
            if (control_row("Close##SYNTH.Control.Close", false)) keep_open = false;
            break;
        }
        case ControlLevel::chat_mode: {
            ImGuiMCP::SeparatorText("Chat Mode");
            auto first = true;
            for (const auto& option : chat_modes) {
                const auto current = view.mode == option.value;
                auto row = std::string{option.label};
                if (view.pending_mode == option.value) row += "  (pending)";
                else if (current) row += "  (current)";
                row += "##SYNTH.Control.ChatMode.";
                row += option.value;
                const auto activated = control_row(row, first || current);
                describe_option(option);
                if (activated) {
                    control_submit(ControlAction::chat_mode, option.value);
                    break;
                }
                first = false;
            }
            ImGuiMCP::Spacing();
            if (control_row("Back##SYNTH.Control.ChatMode.Back", false)) go_back = true;
            break;
        }
        case ControlLevel::llm_model: {
            ImGuiMCP::SeparatorText("LLM Mode");
            auto first = true;
            for (const auto& option : llm_models) {
                const auto current = view.model == option.value;
                auto row = std::string{option.label};
                if (view.pending_model == option.value) row += "  (pending)";
                else if (current) row += "  (current)";
                row += "##SYNTH.Control.LlmMode.";
                row += option.value;
                const auto activated = control_row(row, first || current);
                describe_option(option);
                if (activated) {
                    control_submit(ControlAction::model_slot, option.value);
                    break;
                }
                first = false;
            }
            ImGuiMCP::Spacing();
            if (control_row("Back##SYNTH.Control.LlmMode.Back", false)) go_back = true;
            break;
        }
        case ControlLevel::profiles: {
            ImGuiMCP::SeparatorText("Dynamic Profiles");
            auto first = true;
            for (const auto& scope : profile_scopes) {
                const auto is_target = std::string_view{scope.value} == "target";
                auto row = std::string{scope.label};
                if (is_target && !target.empty()) row += "  [" + target + "]";
                row += "##SYNTH.Control.Profiles.";
                row += scope.value;
                const auto activated = control_row(row, first);
                describe_option(scope);
                if (activated) {
                    const auto action = is_target ? ControlAction::profile_target
                                        : std::string_view{scope.value} == "narrator"
                                            ? ControlAction::profile_narrator
                                            : ControlAction::profile_nearby;
                    control_submit(action, is_target ? target : std::string{});
                    break;
                }
                first = false;
            }
            ImGuiMCP::Spacing();
            if (control_row("Back##SYNTH.Control.Profiles.Back", false)) go_back = true;
            break;
        }
        }

        ImGuiMCP::Spacing();
        if (view.pending_action) {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4{1.0f, 0.78f, 0.25f, 1.0f}, "%s",
                                  pending_action_label(*view.pending_action));
        }
        if (!view.status.empty()) ImGuiMCP::TextWrapped("%s", view.status.c_str());
        if (control_menu_notice != nullptr) {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4{1.0f, 0.45f, 0.35f, 1.0f}, "%s",
                                  control_menu_notice);
        }

        // Escape backs out of a list and closes the root, and only while focused.
        if (ImGuiMCP::IsWindowFocused(ImGuiMCP::ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_Escape, false)) {
            if (control_menu_level == ControlLevel::root) keep_open = false;
            else go_back = true;
        }
    }
    ImGuiMCP::End();

    // Closing wins over Back in the same frame.
    if (go_back && keep_open) {
        control_menu_level = ControlLevel::root;
        control_menu_notice = nullptr;
    }

    // Released after `End()` and outside every SYNTH critical section: this flag
    // is the framework's, it is what drops the window's input capture, and the
    // framework reads it on its own schedule.
    if (!keep_open) {
        control_menu.cancel(control_menu_draft->revision);
        control_menu_draft.reset();
        clear_control_menu_state();
        window->IsOpen.store(false);
    }
}

// Framework render thread, once per frame, from the single lifecycle listener
// `menu_framework_ui.hpp` registers. That callback is the owning thread for the
// framework's window registry, so this is where the window is created - once,
// never retried, because it fires every frame and a refused registration would
// otherwise keep mutating that registry and keep writing the log.
inline void control_menu_tick() {
    if (!control_menu_window_requested) {
        control_menu_window_requested = true;
        auto* window = F4SEMenuFramework::AddWindow(render_control_menu, true);
        control_menu_window.store(window, std::memory_order_release);
        if (window != nullptr) {
            REX::INFO("SYNTH UI: control menu registered on the framework render lifecycle");
        } else {
            REX::WARN("SYNTH UI: framework rejected the control menu window");
        }
    }
    if (auto* window = control_menu_window.load(std::memory_order_acquire)) {
        const auto visible = control_menu.is_open();
        window->IsOpen.store(visible);
        if (!visible && control_menu_draft) {
            control_menu_draft.reset();
            clear_control_menu_state();
        }
    }
}

}  // namespace detail

#endif  // SYNTH_HAS_MENU_FRAMEWORK

// Game-thread facade for the native Control menu. Every entry point is safe to
// call when the framework is not installed, or when SYNTH was built without its
// consumer header; the menu is then simply never shown and nothing is ever
// queued. Nothing here reaches into the render thread's cursor and nothing on
// the render thread reaches into the session: the two meet only at
// `ControlMenuBridge`.
class ControlMenu final {
public:
    static constexpr std::size_t label_limit = detail::ControlMenuBridge::label_limit;
    static constexpr std::size_t status_limit = detail::ControlMenuBridge::status_limit;

    // Game thread. The window pointer is only ever published from the framework's
    // own render lifecycle, so a non-null pointer already means the framework is
    // installed; asking it again would be a framework call from this thread for
    // no extra answer.
    [[nodiscard]] static bool available() noexcept {
#if SYNTH_HAS_MENU_FRAMEWORK
        return detail::control_menu_window.load(std::memory_order_acquire) != nullptr;
#else
        return false;
#endif
    }

    // Ask for the menu, in the runtime generation the caller is on and showing
    // the mode and model it currently believes are in force. Returns false when
    // there is no window to show or a selection is still in flight, so the caller
    // can fall back to its own notification.
    static bool request_open(std::uint64_t generation, std::string target_name, std::string mode,
                             std::string model) {
#if SYNTH_HAS_MENU_FRAMEWORK
        return detail::open_control_menu(generation, std::move(target_name), std::move(mode),
                                         std::move(model));
#else
        (void)generation;
        (void)target_name;
        (void)mode;
        (void)model;
        return false;
#endif
    }

    // Game thread: take the player's selection exactly once. Empty on every pump
    // but the one that follows a selection. The caller owns every session,
    // network and game step the action implies, and must re-check the request's
    // generation against its own before acting on it.
    [[nodiscard]] static std::optional<ControlRequest> take_request() {
        return detail::control_menu.take_request();
    }

    // Game thread: publish what is now in force, plus one line explaining the
    // last selection. This is what clears the menu's pending echo, so call it
    // whether the selection was applied or refused.
    static void publish_status(std::string mode, std::string model, std::string status) {
        detail::control_menu.publish_status(std::move(mode), std::move(model), std::move(status));
    }

    // Game thread: drop the menu and everything queued for a session that is
    // going away. The framework flag is cleared first and on its own, so no SYNTH
    // lock is ever held across a call the framework acts on.
    static void invalidate() {
#if SYNTH_HAS_MENU_FRAMEWORK
        auto* window = detail::control_menu_window.load(std::memory_order_acquire);
        if (window != nullptr) window->IsOpen.store(false);
#endif
        detail::control_menu.invalidate();
    }

    // Game thread, lock-free: whether the menu is up right now, for pausing on
    // it. Reads SYNTH's own cached flag rather than the framework's window, so it
    // costs one relaxed load and touches no renderer-owned state. Unconditional:
    // without the framework nothing ever opens a draft, so it is permanently
    // false.
    [[nodiscard]] static bool is_open() noexcept {
        return detail::control_menu.is_open();
    }
};

}  // namespace synth::ui
