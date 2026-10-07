#pragma once

// SYNTH's native F4SE Menu Framework surface.
//
// SYNTH registers its own pages and hotkeys through the framework's official
// consumer API (DCCStudios/F4SEMenuFramework `resources/F4SEMenuFramework.h`).
// It ships no MCM JSON, so it never appears under "MCM Mod Configs (Legacy)".
//
// Threading. Framework render callbacks run on the renderer's present thread,
// never on the game thread, so this adapter touches no `RE::*` object and no
// live session. The game thread publishes an immutable configuration snapshot;
// the render thread only reads that snapshot and appends keyed INI writes.
//
// Persistence. Edits are written to `Data/F4SE/Plugins/SYNTH_custom.ini` and
// then read back through the plugin's normal configuration reload, so the INI
// chain stays the one authoritative settings source. A hotkey selected on
// SYNTH's page is checked and mirrored into the framework registry during that
// explicit edit only; the render lifecycle never polls or reconciles bindings.
//
// Hotkey registry ownership. That registry is framework-owned and not
// internally synchronized. Registration happens during F4SE kPostLoad, and a
// binding is queried or changed only inside the framework-rendered Hotkeys page
// as a direct result of a selection. Neither the gameplay pump nor the
// per-frame render lifecycle walks the registry.
//
// Framework state ownership generally. The same applies to every framework
// entry point, not just the hotkey registry: each one resolves and caches a
// framework export behind a function-local static and then reads state the
// renderer owns. So the gameplay thread makes no framework call at all. Where
// it needs an answer from the framework - today that is only whether a window
// is capturing input - the render thread republishes it into a SYNTH-owned
// atomic on `kBeforeRender` and the gameplay thread reads the atomic. Those
// answers therefore lag by at most one rendered frame, which is the same
// resolution the polled hotkeys already run at.
//
// Gestures. The framework dispatches a hotkey once per physical press. SYNTH
// needs held and tap/double-tap/hold gestures, so it keeps polling key state
// itself from SYNTH's authoritative configuration. The registered framework
// entries provide conflict awareness; their callbacks are intentionally inert.
// The chatbox is the one exception: opening a window is a plain one-shot press,
// so its callback opens the window directly while the polled route stays in
// place unchanged.
//
// Chatbox. SYNTH's first framework window, for text input. Window creation is
// deferred to the framework's first render lifecycle event: adding a consumer
// window from F4SE kPostLoad can stall this framework build while its renderer
// is still initializing. The game thread asks for the window and consumes the
// submitted message; the render thread owns the edit buffer and never reads
// back. SYNTH owns only its own windows: it never opens or closes the
// framework's own main menu.
//
// Control menu. SYNTH's second and last framework window lives in
// `ui/control_menu.hpp`, built the same way and for the same reasons. This file
// keeps the one lifecycle listener and drives that window's deferred creation
// from it, so both windows are created on the thread that owns the framework's
// window registry and SYNTH still registers exactly one listener.
//
// The window carries one other control the game thread acts on: Stop, which
// asks for the reply currently being spoken to end. It travels the same bridge
// as a message and under the same generation rules, so a stop the player
// pressed cannot outlive the session it was pressed in. Opening and closing the
// window queues nothing by itself - only Send and Stop do.

#include "config/config.hpp"
#include "integration/prompt_target.hpp"
#include "ui/control_menu.hpp"
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
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#if SYNTH_HAS_MENU_FRAMEWORK
#include "ui/settings_store.hpp"

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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>
#endif

namespace synth::ui {

using ConfigSnapshot = std::shared_ptr<const config::Config>;

struct ChatboxSubmission final {
    std::string text;
    std::optional<integration::PromptTarget> target;
};

// What the Tools page has asked the game thread to do about voice imports. The
// render thread only ever names one of these; every filesystem, network and
// game step the request implies belongs to the thread that takes it.
enum class VoiceAction : unsigned char {
    none,
    import_all,
    cancel,
};

namespace detail {

// Why a submitted draft was, or was not, handed to the game thread. The render
// thread turns this into the line it shows the player.
enum class ChatboxSubmit : unsigned char {
    accepted,
    empty,     // nothing but whitespace
    too_long,  // beyond the transport's character bound
    busy,      // the previous message has not been read yet
    stale,     // the draft was closed or its session retired
};

// Engine-free hand-off between the framework render thread, which owns the edit
// buffer, and the game thread, which owns everything that can act on a message.
// It holds no `RE::*` object, no F4SE handle and no session, so both sides can
// touch it without either reaching into the other's world - and so it compiles
// and is testable with neither the framework nor the game present.
class ChatboxBridge final {
public:
    // The same 1-4096 character window `PluginSession::submit_text` enforces, so
    // the UI can never build a message the transport would reject.
    static constexpr std::size_t message_limit = 4096;

    // A draft receipt is copied atomically with its target, never read separately.
    struct Draft final {
        std::uint64_t revision{};
        std::optional<integration::PromptTarget> target;
    };

    bool request_open(std::optional<integration::PromptTarget> target = {}) {
        std::scoped_lock lock{mutex_};
        if (open_pending_ || draft_ || message_) return false;
        if (target && target->session.is_cancelled()) return false;
        draft_ = Draft{++revision_, std::move(target)};
        open_pending_ = true;
        refresh_open_locked();
        return true;
    }

    // Render thread: one receipt per accepted open request.
    [[nodiscard]] std::optional<Draft> consume_open_request() {
        std::scoped_lock lock{mutex_};
        if (!open_pending_) return {};
        open_pending_ = false;
        return draft_;
    }

    // Render thread: offer the game thread one message. A second draft is
    // refused while the first is still unread rather than overwriting it, so a
    // slow consumer can lose neither message nor gain a duplicate.
    [[nodiscard]] ChatboxSubmit submit(std::string text, std::uint64_t revision) {
        auto trimmed = trim(std::move(text));
        if (trimmed.empty()) return ChatboxSubmit::empty;
        if (trimmed.size() > message_limit) return ChatboxSubmit::too_long;
        std::scoped_lock lock{mutex_};
        if (message_) return ChatboxSubmit::busy;
        if (!draft_ || draft_->revision != revision ||
            (draft_->target && draft_->target->session.is_cancelled())) return ChatboxSubmit::stale;
        message_ = ChatboxSubmission{std::move(trimmed), std::move(draft_->target)};
        draft_.reset();
        refresh_open_locked();
        return ChatboxSubmit::accepted;
    }

    // Render thread: the player pressed Stop. Bound to the receipt that drew the
    // button, so a renderer holding an obsolete one cannot stop the dialogue a
    // replacement draft belongs to, and a draft whose session has already been
    // retired can stop nothing at all. Stop is deliberately independent of the
    // draft: it neither submits nor discards what has been typed, and it does
    // not close the window.
    bool request_stop(std::uint64_t revision) {
        std::scoped_lock lock{mutex_};
        if (!draft_ || draft_->revision != revision) return false;
        if (draft_->target && draft_->target->session.is_cancelled()) return false;
        stop_pending_ = true;
        return true;
    }

    [[nodiscard]] bool has_stop_request() const {
        std::scoped_lock lock{mutex_};
        return stop_pending_;
    }

    // Game thread: take the pending stop exactly once. False on every pump but
    // the one that follows a press.
    [[nodiscard]] bool take_stop_request() {
        std::scoped_lock lock{mutex_};
        return std::exchange(stop_pending_, false);
    }

    [[nodiscard]] bool has_message() const noexcept {
        std::scoped_lock lock{mutex_};
        return message_.has_value();
    }

    [[nodiscard]] bool has_draft() const {
        std::scoped_lock lock{mutex_};
        return refresh_open_locked();
    }

    // Game thread, lock-free: whether the player is composing right now. A
    // cached mirror of `has_draft()`, not a framework or ImGui read, so the
    // gameplay pump can pause on it without touching renderer-owned state.
    // Written at every state change and refreshed again by `has_draft()`, which
    // the render lifecycle evaluates once a frame, so an answer is never more
    // than one rendered frame behind a session retirement - the same resolution
    // `framework_input_captured` already publishes at.
    [[nodiscard]] bool is_open() const noexcept {
        return open_.load(std::memory_order_relaxed);
    }

    // Game thread: take the pending message exactly once.
    [[nodiscard]] std::optional<std::string> take_message() {
        std::scoped_lock lock{mutex_};
        if (!message_ || message_->target) return {};
        auto result = std::move(message_->text);
        message_.reset();
        return result;
    }

    [[nodiscard]] std::optional<ChatboxSubmission> take_submission() {
        std::scoped_lock lock{mutex_};
        return std::exchange(message_, std::nullopt);
    }

    // A renderer holding an obsolete receipt cannot cancel a replacement draft or a sent message.
    void cancel(std::uint64_t revision) {
        std::scoped_lock lock{mutex_};
        if (draft_ && draft_->revision == revision) {
            draft_.reset();
            open_pending_ = false;
            refresh_open_locked();
        }
        // A pending stop deliberately survives: closing the window is not a way
        // to take back a Stop the player already pressed, and conversely merely
        // opening and closing the window never queues one.
    }

    // Drop every queued request and message. Used when the session the message
    // was meant for is gone, so nothing survives into the next one.
    void discard() {
        std::scoped_lock lock{mutex_};
        open_pending_ = false;
        draft_.reset();
        message_.reset();
        // The stop goes with the session it was pressed in. Bumping the
        // revision alongside it means a renderer still holding the old receipt
        // cannot queue a replacement stop against whatever comes next either.
        stop_pending_ = false;
        ++revision_;
        refresh_open_locked();
    }

private:
    // Caller holds `mutex_`. Publishes and returns the same answer, so the
    // lock-free `is_open()` can never disagree with `has_draft()` by more than
    // the frame in which the session was retired.
    bool refresh_open_locked() const noexcept {
        const auto open = draft_.has_value() &&
                          (!draft_->target || !draft_->target->session.is_cancelled());
        open_.store(open, std::memory_order_relaxed);
        return open;
    }

    [[nodiscard]] static std::string trim(std::string text) {
        const auto blank = [](char character) {
            return std::isspace(static_cast<unsigned char>(character)) != 0;
        };
        while (!text.empty() && blank(text.back())) text.pop_back();
        std::size_t begin = 0;
        while (begin < text.size() && blank(text[begin])) ++begin;
        text.erase(0, begin);
        return text;
    }

    mutable std::mutex mutex_;
    std::optional<ChatboxSubmission> message_;
    std::optional<Draft> draft_;
    std::uint64_t revision_{};
    bool open_pending_{};
    bool stop_pending_{};
    mutable std::atomic<bool> open_{false};
};

inline ChatboxBridge chatbox{};

// Engine-free hand-off for the Tools page's voice-import controls, built like
// `ChatboxBridge`: no `RE::*` object, no F4SE handle and no session, so flat and
// VR share it and it compiles with neither the framework nor the game present.
//
// The request travels render thread -> game thread as a single atomic, and the
// status line travels back under a mutex. The render thread only ever takes a
// copy of that line, so no SYNTH lock is held while ImGui draws.
class VoiceImportBridge final {
public:
    // A status line is a caption, not a log: bound it so a runaway publisher
    // cannot grow the string the render thread copies every frame.
    static constexpr std::size_t status_limit = 240;

    // Render thread: name the action the game thread should take. A newer
    // request replaces an unread one, so pressing Cancel after Send All cancels
    // rather than queueing both.
    [[nodiscard]] bool request(VoiceAction action, std::uint64_t ticket) noexcept {
        if (ticket == 0 || (ticket & 4) == 0 ||
            (action != VoiceAction::import_all && action != VoiceAction::cancel)) return false;
        auto observed = state_.load(std::memory_order_acquire);
        while ((observed & ~std::uint64_t{3}) == ticket) {
            const auto next = ticket | static_cast<std::uint64_t>(action);
            if (state_.compare_exchange_weak(observed,next,std::memory_order_acq_rel)) return true;
        }
        return false;
    }

    // A rendered frame retains this revision; retirement cannot authorize its buttons in a new session.
    [[nodiscard]] std::uint64_t ticket() const noexcept {
        const auto value = state_.load(std::memory_order_acquire);
        return (value & 4) != 0 ? value & ~std::uint64_t{3} : 0;
    }

    // Game thread: changing session availability advances the revision and drops unread actions atomically.
    void set_available(bool available) noexcept {
        auto observed = state_.load(std::memory_order_acquire);
        for (;;) {
            if (((observed & 4) != 0) == available) return;
            const auto next = ((observed & ~std::uint64_t{7}) + 8) | (available ? 4 : 0);
            if (state_.compare_exchange_weak(observed,next,std::memory_order_acq_rel)) return;
        }
    }

    // Game thread: take the pending action exactly once. Returns `none` on every
    // pump but the one that follows a press.
    [[nodiscard]] VoiceAction take() noexcept {
        const auto value = state_.fetch_and(~std::uint64_t{3},std::memory_order_acq_rel);
        return (value & 4) != 0 ? static_cast<VoiceAction>(value & 3) : VoiceAction::none;
    }

    // Game thread: publish one line of progress for the page to show.
    void publish_status(std::string status) {
        if (status.size() > status_limit) {
            auto cut = status_limit;
            // Never split a UTF-8 sequence: step back off any continuation byte.
            while (cut > 0 && (static_cast<unsigned char>(status[cut]) & 0xC0U) == 0x80U) --cut;
            status.resize(cut);
        }
        std::scoped_lock lock{mutex_};
        status_ = std::move(status);
    }

    // Render thread: a copy of the last published line, empty until there is one.
    [[nodiscard]] std::string status() const {
        std::scoped_lock lock{mutex_};
        return status_;
    }

private:
    mutable std::mutex mutex_;
    std::string status_;
    // Low two bits: action; bit two: availability; remaining bits: session revision.
    std::atomic<std::uint64_t> state_{};
};

inline VoiceImportBridge voice_import{};

}  // namespace detail

#if SYNTH_HAS_MENU_FRAMEWORK

namespace detail {

// Published by the game thread when a session is created, reloaded or dropped.
inline std::atomic<ConfigSnapshot> published_configuration{};

// The framework's input-capture answer, republished by the render thread on
// every `kBeforeRender` so the gameplay pump never has to ask the framework
// itself. Relaxed on both sides: this gates nothing but SYNTH's own polled
// hotkeys, and neither thread reads anything else through it.
//
// It stays false while the framework is absent, because the lifecycle listener
// that writes it is only registered once the framework is installed - the same
// answer the direct call gave.
inline std::atomic<bool> framework_input_captured{false};

// Values the player has changed in the UI but that the authoritative reload has
// not surfaced yet. Purely a transient echo of an in-flight write: an entry is
// dropped as soon as the published configuration agrees with it, so this never
// becomes a parallel settings store.
inline std::mutex edit_mutex;
inline std::map<std::string, double> pending_edits;
inline std::string last_write_error;

[[nodiscard]] inline std::string edit_key(const Setting& setting) {
    return std::string{setting.section} + "." + std::string{setting.key};
}

[[nodiscard]] inline std::string widget_id(const Setting& setting) {
    return std::string{setting.label} + "##SYNTH." + edit_key(setting);
}

[[nodiscard]] inline bool matches(const Setting& setting, double left, double right) {
    return setting.kind == SettingKind::decimal ? std::fabs(left - right) < 1e-6
                                                : left == right;
}

// Snaps to the same discrete steps the previous settings surface exposed, so a
// continuous ImGui slider cannot persist an off-step value.
[[nodiscard]] inline double snap(const Setting& setting, double value) {
    if (setting.step <= 0.0) return value;
    const auto steps = std::round((value - setting.minimum) / setting.step);
    const auto snapped = setting.minimum + steps * setting.step;
    return std::clamp(snapped, setting.minimum, setting.maximum);
}

inline void note_pending(const Setting& setting, double value) {
    std::scoped_lock lock{edit_mutex};
    pending_edits[edit_key(setting)] = value;
}

inline bool commit(const Setting& setting, double value) {
    const auto identifier = edit_key(setting);
    if (write_setting(setting, value)) {
        std::scoped_lock lock{edit_mutex};
        last_write_error.clear();
        return true;
    }
    std::scoped_lock lock{edit_mutex};
    pending_edits.erase(identifier);
    last_write_error = "SYNTH could not write " + identifier + " to SYNTH_custom.ini";
    return false;
}

// Returns the value to show: the authoritative one, unless an edit is still in
// flight toward the INI.
[[nodiscard]] inline double displayed(const Setting& setting, double authoritative) {
    const auto identifier = edit_key(setting);
    std::scoped_lock lock{edit_mutex};
    const auto entry = pending_edits.find(identifier);
    if (entry == pending_edits.end()) return authoritative;
    if (matches(setting, entry->second, authoritative)) {
        pending_edits.erase(entry);
        return authoritative;
    }
    return entry->second;
}

inline void describe(const Setting& setting) {
    if (setting.help == nullptr || *setting.help == '\0') return;
    if (ImGuiMCP::IsItemHovered()) {
        ImGuiMCP::SetTooltip("%s", setting.help);
    }
}

#if F4SEMENUFRAMEWORK_HAS_DIK
struct KeyOption final {
    const char* name;
    unsigned int code;
};

// Built from the framework's own canonical DIK table so SYNTH and the framework
// can never disagree about a code or its display name. Mouse and other codes
// above 255 are excluded: SYNTH persists scan codes as 0-255.
[[nodiscard]] inline const std::vector<KeyOption>& key_options() {
    static const std::vector<KeyOption> options = [] {
        std::vector<KeyOption> result;
#define SYNTH_DIK_ENTRY(ident, name, code) result.push_back(KeyOption{name, (code)});
        F4SEMF_DIK_LIST(SYNTH_DIK_ENTRY)
#undef SYNTH_DIK_ENTRY
        std::erase_if(result, [](const KeyOption& option) { return option.code > 255; });
        return result;
    }();
    return options;
}

[[nodiscard]] inline const char* key_name(unsigned int code) {
    for (const auto& option : key_options()) {
        if (option.code == code) return option.name;
    }
    return "UNKNOWN";
}
#endif

[[nodiscard]] inline const HotkeyBinding* binding_for(std::string_view key) {
    for (const auto& binding : hotkey_bindings) {
        if (binding.key == key) return &binding;
    }
    return nullptr;
}

// Render-thread UI action only: keep the framework registry and SYNTH's
// authoritative INI aligned without any lifecycle-time registry polling.
inline void commit_hotkey_selection(const Setting& setting, const HotkeyBinding& binding,
                                    unsigned int previous, unsigned int selected) {
    if (selected == previous) return;
    if (selected != 0 &&
        F4SEMenuFramework::Hotkeys::HasConflict(selected, binding.framework_id)) {
        std::scoped_lock lock{edit_mutex};
        last_write_error = std::string{setting.label} +
                           " was not changed because that key is already in use.";
        return;
    }

    F4SEMenuFramework::Hotkeys::SetBinding(binding.framework_id, selected);
    if (F4SEMenuFramework::Hotkeys::GetBinding(binding.framework_id) != selected) {
        std::scoped_lock lock{edit_mutex};
        last_write_error = std::string{setting.label} +
                           " was not changed because the menu framework rejected it.";
        return;
    }

    note_pending(setting, selected);
    if (!commit(setting, selected)) {
        F4SEMenuFramework::Hotkeys::SetBinding(binding.framework_id, previous);
    }
}

inline void draw_keybind(const Setting& setting, double authoritative) {
    const auto* binding = binding_for(setting.key);
    if (binding == nullptr) return;
    const auto code = static_cast<unsigned int>(displayed(setting, authoritative));
    const auto identifier = widget_id(setting);

#if F4SEMENUFRAMEWORK_HAS_DIK
    if (ImGuiMCP::BeginCombo(identifier.c_str(), key_name(code))) {
        for (const auto& option : key_options()) {
            const auto selected = option.code == code;
            if (ImGuiMCP::Selectable(option.name, selected)) {
                commit_hotkey_selection(setting, *binding, code, option.code);
            }
            if (selected) ImGuiMCP::SetItemDefaultFocus();
        }
        ImGuiMCP::EndCombo();
    }
    describe(setting);
#else
    auto value = static_cast<int>(code);
    if (ImGuiMCP::SliderInt(identifier.c_str(), &value, 0, 255)) {
        commit_hotkey_selection(setting, *binding, code, static_cast<unsigned int>(value));
    }
    describe(setting);
#endif
}

inline void draw_setting(const Setting& setting, const config::Config& configuration) {
    switch (setting.kind) {
    case SettingKind::section:
        ImGuiMCP::Spacing();
        ImGuiMCP::SeparatorText(setting.label);
        return;
    case SettingKind::note:
        ImGuiMCP::TextDisabled("%s", setting.label);
        if (setting.help != nullptr && *setting.help != '\0') {
            if (ImGuiMCP::IsItemHovered()) ImGuiMCP::SetTooltip("%s", setting.help);
        }
        return;
    default:
        break;
    }

    const auto authoritative = current_value(configuration, setting);
    if (setting.kind == SettingKind::keybind) {
        draw_keybind(setting, authoritative);
        return;
    }

    const auto identifier = widget_id(setting);
    const auto shown = displayed(setting, authoritative);
    switch (setting.kind) {
    case SettingKind::toggle: {
        auto value = shown != 0.0;
        if (ImGuiMCP::Checkbox(identifier.c_str(), &value)) {
            note_pending(setting, value ? 1.0 : 0.0);
            commit(setting, value ? 1.0 : 0.0);
        }
        describe(setting);
        break;
    }
    case SettingKind::action: {
        if (ImGuiMCP::Button(identifier.c_str())) {
            // The runtime observes the request on its next configuration
            // reload and re-arms the control, exactly as before.
            commit(setting, 1.0);
        }
        describe(setting);
        break;
    }
    case SettingKind::integer: {
        auto value = static_cast<int>(shown + 0.5);
        if (ImGuiMCP::SliderInt(identifier.c_str(), &value, static_cast<int>(setting.minimum),
                                static_cast<int>(setting.maximum))) {
            note_pending(setting, snap(setting, value));
        }
        describe(setting);
        if (ImGuiMCP::IsItemDeactivatedAfterEdit()) commit(setting, snap(setting, value));
        break;
    }
    case SettingKind::decimal: {
        auto value = static_cast<float>(shown);
        if (ImGuiMCP::SliderFloat(identifier.c_str(), &value, static_cast<float>(setting.minimum),
                                  static_cast<float>(setting.maximum), "%.1f")) {
            note_pending(setting, snap(setting, value));
        }
        describe(setting);
        if (ImGuiMCP::IsItemDeactivatedAfterEdit()) commit(setting, snap(setting, value));
        break;
    }
    default:
        break;
    }
}

inline void draw_page(const Page& page) {
    const auto configuration = published_configuration.load();
    if (!configuration) {
        ImGuiMCP::TextWrapped(
            "SYNTH has not loaded its configuration yet. Load a save or start a new game, then "
            "reopen this page. Every setting also remains editable in "
            "Data/F4SE/Plugins/SYNTH_custom.ini.");
        return;
    }
    {
        std::scoped_lock lock{edit_mutex};
        if (!last_write_error.empty()) {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4{1.0f, 0.45f, 0.35f, 1.0f}, "%s",
                                  last_write_error.c_str());
        }
    }
    for (const auto& setting : page.settings) {
        ImGuiMCP::PushID(setting.label);
        try {
            draw_setting(setting, *configuration);
        } catch (const std::exception&) {
            // A catalog row with no configuration backing must not take down the
            // renderer; skip it and keep the rest of the page usable.
            ImGuiMCP::TextDisabled("%s (unavailable)", setting.label);
        }
        ImGuiMCP::PopID();
    }
}

// The Tools page's voice-import controls. Render thread only, and deliberately
// the cheapest possible callback: each button stores one action on the bridge
// and the status line is a copy of what the game thread last published. Nothing
// here opens a file, sends a request, touches a `RE::*` object or writes the
// INI, so a press costs the renderer nothing beyond an atomic store.
inline void draw_voice_import_controls() {
    const auto ticket = voice_import.ticket();
    ImGuiMCP::Spacing();
    ImGuiMCP::SeparatorText("Voice Import");

    if (ImGuiMCP::Button("Send All Voice Samples##SYNTH.Voice.ImportAll")) {
        (void)voice_import.request(VoiceAction::import_all,ticket);
    }
    if (ImGuiMCP::IsItemHovered()) {
        ImGuiMCP::SetTooltip(
            "Finds one sample per supported voice in active game files and imports missing samples "
            "in the background. Existing samples are kept. Preview imported voices in TTS Studio.");
    }
    ImGuiMCP::SameLine();
    if (ImGuiMCP::Button("Cancel##SYNTH.Voice.Cancel")) {
        (void)voice_import.request(VoiceAction::cancel,ticket);
    }
    if (ImGuiMCP::IsItemHovered()) {
        ImGuiMCP::SetTooltip(
            "Stops an import that is still running. Samples already uploaded are kept.");
    }

    // One compact line, wrapped so a long message cannot widen the page.
    const auto status = voice_import.status();
    if (status.empty()) {
        ImGuiMCP::TextDisabled("Voice import: idle.");
    } else {
        ImGuiMCP::TextWrapped("%s", status.c_str());
    }
}

inline void __stdcall render_hotkeys() { draw_page(pages[0]); }
inline void __stdcall render_auto_activate() { draw_page(pages[1]); }
inline void __stdcall render_behavior() { draw_page(pages[2]); }
inline void __stdcall render_sound() { draw_page(pages[3]); }

// `draw_page` returns early before a configuration is published; the controls
// still draw, because queueing an action needs no settings and the status line
// is the one place the player can see what an earlier press did.
inline void __stdcall render_tools() {
    draw_page(pages[4]);
    draw_voice_import_controls();
}

// SYNTH polls key state itself to preserve hold, tap and double-tap gestures,
// so the framework only needs a binding entry, not a dispatch target.
inline void __stdcall on_hotkey() {}

// ---------------------------------------------------------------- chatbox ---

// The one window SYNTH registers. Owned by the framework; SYNTH only flips the
// atomics on it and never touches the framework's own main menu.
//
// The pointer itself is atomic because the render thread is what publishes it -
// window creation is deferred to `kBeforeRender` - while the game thread and the
// framework's hotkey dispatch both read it. Release/acquire, so a reader that
// sees the pointer also sees the `WindowInterface` the framework built behind it.
inline std::atomic<F4SEMenuFramework::Model::WindowInterface*> chatbox_window{nullptr};
inline bool chatbox_window_requested = false;  // render thread only
inline std::int64_t chatbox_lifecycle_listener = -1;

// Render thread only, so it needs no lock: an extra byte holds the terminator
// for a full-length message.
inline std::array<char, ChatboxBridge::message_limit + 1> chatbox_buffer{};
inline bool chatbox_focus_pending = false;
inline const char* chatbox_status = nullptr;
inline std::optional<ChatboxBridge::Draft> chatbox_draft;

// No bridge lock is held while the framework's visibility flag is changed.
inline bool open_chatbox(std::optional<integration::PromptTarget> target = {}) {
    auto* window = chatbox_window.load(std::memory_order_acquire);
    if (window == nullptr) return false;
    if (!chatbox.request_open(std::move(target))) return false;
    // The render lifecycle publishes visibility, so an older render close
    // cannot lose this request between the bridge and the framework flag.
    return true;
}

inline void clear_chatbox_draft() noexcept {
    chatbox_buffer[0] = '\0';
    chatbox_focus_pending = false;
    chatbox_status = nullptr;
}

// Render thread. Reads the published configuration snapshot and its own buffer
// and nothing else: no `RE::*` object, no session, no framework close call.
inline void __stdcall render_chatbox() {
    auto* window = chatbox_window.load(std::memory_order_acquire);
    if (window == nullptr) return;
    // Defensive: a framework that polls closed windows must not draw one.
    if (!window->IsOpen.load()) return;

    if (auto draft = chatbox.consume_open_request()) {
        chatbox_draft = std::move(draft);
        chatbox_buffer[0] = '\0';
        chatbox_focus_pending = true;
        chatbox_status = nullptr;
    }
    if (!chatbox_draft) { window->IsOpen.store(false); return; }

    // Center against the current viewport every frame, including resolution changes.
    // Scale only this dialog; do not change the framework's fonts or other menus.
    const auto* viewport = ImGuiMCP::GetMainViewport();
    if (!viewport || viewport->WorkSize.x <= 0.0f || viewport->WorkSize.y <= 0.0f) return;
    const auto scale = std::clamp(viewport->WorkSize.y / 1080.0f, 1.0f, 1.5f);
    const auto width = std::min(900.0f * scale, std::max(1.0f, viewport->WorkSize.x - 48.0f));
    const auto height = std::min(460.0f * scale, std::max(1.0f, viewport->WorkSize.y - 48.0f));
    ImGuiMCP::SetNextWindowPos(
        ImGuiMCP::ImVec2{viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                        viewport->WorkPos.y + viewport->WorkSize.y * 0.5f},
        ImGuiMCP::ImGuiCond_Always, ImGuiMCP::ImVec2{0.5f, 0.5f});
    ImGuiMCP::SetNextWindowSize(ImGuiMCP::ImVec2{width, height}, ImGuiMCP::ImGuiCond_Always);
    auto keep_open = true;
    auto submit_requested = false;
    if (ImGuiMCP::Begin("SYNTH Chatbox##SYNTH.Chatbox", &keep_open,
                        ImGuiMCP::ImGuiWindowFlags_NoSavedSettings |
                            ImGuiMCP::ImGuiWindowFlags_NoMove |
                            ImGuiMCP::ImGuiWindowFlags_NoResize |
                            ImGuiMCP::ImGuiWindowFlags_NoCollapse)) {
        ImGuiMCP::SetWindowFontScale(1.25f * scale);
        ImGuiMCP::PushTextWrapPos(0.0f);
        ImGuiMCP::TextDisabled(
            "Message the NPC you are talking to. Enter sends, Ctrl+Enter starts a new line, "
            "Escape closes.");
        if (chatbox_draft->target)
            ImGuiMCP::TextWrapped("To: %s", chatbox_draft->target->display_name.c_str());
        if (!published_configuration.load()) {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4{1.0f, 0.78f, 0.25f, 1.0f},
                                  "SYNTH has not loaded a session yet, so there may be nobody to "
                                  "receive this message.");
        }

        if (chatbox_focus_pending) {
            ImGuiMCP::SetKeyboardFocusHere();
            chatbox_focus_pending = false;
        }
        ImGuiMCP::ImVec2 area{};
        ImGuiMCP::GetContentRegionAvail(&area);
        // Reserve scaled buttons and two wrapped status lines below the editor.
        const auto footer = ImGuiMCP::GetFrameHeightWithSpacing() +
                            ImGuiMCP::GetTextLineHeightWithSpacing() * 2.0f;
        const auto input_height = std::max(area.y - footer, ImGuiMCP::GetFrameHeightWithSpacing() * 3.0f);
        submit_requested = ImGuiMCP::InputTextMultiline(
            "##SYNTH.Chatbox.Text", chatbox_buffer.data(), chatbox_buffer.size(),
            ImGuiMCP::ImVec2{-1.0f, input_height},
            ImGuiMCP::ImGuiInputTextFlags_EnterReturnsTrue |
                ImGuiMCP::ImGuiInputTextFlags_CtrlEnterForNewLine,
            nullptr, nullptr);

        const auto length = std::strlen(chatbox_buffer.data());

        // Evaluated into one request: Enter and the button can never queue the
        // same draft twice in a frame.
        if (ImGuiMCP::Button("Send##SYNTH.Chatbox.Send")) submit_requested = true;
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Cancel##SYNTH.Chatbox.Cancel")) keep_open = false;
        ImGuiMCP::SameLine();
        // Stop only asks the game thread to end the reply that is playing. It
        // leaves the window open and the typed draft untouched, so it is not a
        // second Cancel, and it is refused outright once the draft's session is
        // gone rather than being queued for the next one.
        if (ImGuiMCP::Button("Stop##SYNTH.Chatbox.Stop")) {
            chatbox_status = chatbox.request_stop(chatbox_draft->revision)
                                 ? "Stopping the current reply."
                                 : "This prompt expired, so there is nothing to stop.";
        }
        if (ImGuiMCP::IsItemHovered()) {
            ImGuiMCP::SetTooltip("Stop the reply SYNTH is speaking. Keeps this window open.");
        }

        // Closing wins over Send/Enter in the same frame and only applies while focused.
        if (ImGuiMCP::IsWindowFocused(ImGuiMCP::ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_Escape, false)) {
            keep_open = false;
        }

        if (submit_requested && keep_open) {
            switch (chatbox.submit(std::string{chatbox_buffer.data(), length}, chatbox_draft->revision)) {
            case ChatboxSubmit::accepted:
                chatbox_buffer[0] = '\0';
                chatbox_status = nullptr;
                keep_open = false;
                break;
            case ChatboxSubmit::empty:
                chatbox_status = "Type a message first.";
                chatbox_focus_pending = true;
                break;
            case ChatboxSubmit::too_long:
                chatbox_status = "That message is too long to send.";
                break;
            case ChatboxSubmit::busy:
                chatbox_status = "SYNTH is still reading the last message. Try again in a moment.";
                break;
            case ChatboxSubmit::stale:
                chatbox_status = "This prompt expired. Close it and open a new prompt.";
                break;
            }
        }
        if (chatbox_status != nullptr) {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4{1.0f, 0.45f, 0.35f, 1.0f}, "%s", chatbox_status);
        }
        ImGuiMCP::PopTextWrapPos();
    }
    ImGuiMCP::End();

    // Released after `End()` and outside every SYNTH critical section: this flag
    // is the framework's, it is what drops the window's input capture, and the
    // framework reads it on its own schedule.
    if (!keep_open) {
        chatbox.cancel(chatbox_draft->revision);
        chatbox_draft.reset();
        clear_chatbox_draft();
        window->IsOpen.store(false);
    }
}

// Framework render thread, once per frame. This callback is the owning thread
// for the framework's window registry. AddWindow mutates that registry, so
// SYNTH's window is created on this lifecycle after renderer startup and before
// that frame iterates it. Hotkey registry work is intentionally absent here;
// it happens only from an explicit Hotkeys-page edit. The permanent, guarded
// listener also avoids unregistering from inside the framework traversal.
inline void __stdcall on_framework_lifecycle(F4SEMenuFramework::Events::Type type) {
    if (type != F4SEMenuFramework::Events::kBeforeRender) return;
    // Republish what the gameplay thread would otherwise have asked the
    // framework for. This is the only thread allowed to ask.
    framework_input_captured.store(F4SEMenuFramework::IsAnyBlockingWindowOpened(),
                                   std::memory_order_relaxed);
    if (!chatbox_window_requested) {
        // Exactly one attempt. This fires every frame, so retrying would keep
        // mutating the framework's window registry, and keep writing the log,
        // for as long as the framework refused.
        chatbox_window_requested = true;
        auto* window = F4SEMenuFramework::AddWindow(render_chatbox, true);
        chatbox_window.store(window, std::memory_order_release);
        if (window != nullptr) {
            REX::INFO("SYNTH UI: chatbox registered on the framework render lifecycle");
        } else {
            REX::WARN("SYNTH UI: framework rejected the chatbox window");
        }
    }
    if (auto* window = chatbox_window.load(std::memory_order_acquire)) {
        const auto visible = chatbox.has_draft();
        window->IsOpen.store(visible);
        if (!visible) { clear_chatbox_draft(); chatbox_draft.reset(); }
    }
    // SYNTH's other window, created and pumped on this same lifecycle for the
    // same reason. It reads only its own bridge, so it adds no framework state
    // read and no configuration read to this callback.
    control_menu_tick();
}

}  // namespace detail

#endif  // SYNTH_HAS_MENU_FRAMEWORK

// Game-thread facade for the native chatbox window. Every entry point is safe
// to call when the framework is not installed, or when SYNTH was built without
// its consumer header; the chatbox is then simply never shown.
//
// Nothing here reaches into the render thread's buffer and nothing on the render
// thread reaches into the session: the two meet only at `ChatboxBridge`.
class Chatbox final {
public:
    static constexpr std::size_t message_limit = detail::ChatboxBridge::message_limit;

    // Game thread. The window pointer is only ever published from the framework's
    // own render lifecycle, so a non-null pointer already means the framework is
    // installed; asking it again would be a framework call from this thread for
    // no extra answer.
    [[nodiscard]] static bool available() noexcept {
#if SYNTH_HAS_MENU_FRAMEWORK
        return detail::chatbox_window.load(std::memory_order_acquire) != nullptr;
#else
        return false;
#endif
    }

    // Ask for an empty chatbox. Returns false when there is no window to show,
    // so the caller can fall back to its own notification.
    static bool request_open() noexcept {
#if SYNTH_HAS_MENU_FRAMEWORK
        return detail::open_chatbox();
#else
        return false;
#endif
    }

    static bool request_open(integration::PromptTarget target) {
#if SYNTH_HAS_MENU_FRAMEWORK
        return detail::open_chatbox(std::move(target));
#else
        (void)target;
        return false;
#endif
    }

    [[nodiscard]] static std::optional<ChatboxSubmission> take_submission() {
#if SYNTH_HAS_MENU_FRAMEWORK
        return detail::chatbox.take_submission();
#else
        return {};
#endif
    }

    [[nodiscard]] static bool visible() noexcept {
#if SYNTH_HAS_MENU_FRAMEWORK
        const auto* window = detail::chatbox_window.load(std::memory_order_acquire);
        return window != nullptr && window->IsOpen.load();
#else
        return false;
#endif
    }

    // Game thread: whether the player is composing right now, for pausing while
    // the chatbox is up. Unlike `visible()` this reads SYNTH's own cached flag
    // rather than the framework's window, so it costs one relaxed load and
    // touches no renderer-owned state. Unconditional like the voice-import
    // entry points: without the framework nothing ever opens a draft, so it is
    // permanently false.
    [[nodiscard]] static bool is_open() noexcept {
        return detail::chatbox.is_open();
    }

    // Keep playback paused until a submitted draft reaches game-thread turn admission.
    [[nodiscard]] static bool has_submission() noexcept {
        return detail::chatbox.has_message();
    }

    // Game thread: take the player's Stop request exactly once. False on every
    // pump but the one that follows a press. `close()` clears it along with the
    // rest of the session's queued work, so a stop pressed in a session that
    // has gone away cannot reach the next one.
    [[nodiscard]] static bool take_stop_request() {
        return detail::chatbox.take_stop_request();
    }

    // Game thread: take the message the player submitted, once. Returns an empty
    // optional when there is nothing new, which is every frame but one.
    [[nodiscard]] static std::optional<std::string> take_message() {
#if SYNTH_HAS_MENU_FRAMEWORK
        return detail::chatbox.take_message();
#else
        return std::nullopt;
#endif
    }

    // Game thread: hide the window and drop anything still queued for a session
    // that is going away. The framework flag is cleared first and on its own, so
    // no SYNTH lock is ever held across a call the framework acts on.
    static void close() {
#if SYNTH_HAS_MENU_FRAMEWORK
        auto* window = detail::chatbox_window.load(std::memory_order_acquire);
        if (window != nullptr) window->IsOpen.store(false);
        detail::chatbox.discard();
#endif
    }

    // Voice import, Tools page. The render thread queues a request; the game
    // thread takes it and owns every filesystem, network and game step it
    // implies. Both entry points are unconditional: the bridge behind them is
    // plain C++ with no framework, F4SE or `RE::*` type in it, so flat and VR
    // build them identically and a build without the consumer header simply
    // never has anything queued.

    // Game thread: take the pending request exactly once, consuming it.
    [[nodiscard]] static VoiceAction take_voice_action() noexcept {
        return detail::voice_import.take();
    }

    static void set_voice_session_available(bool available) noexcept {
        detail::voice_import.set_available(available);
    }

    // Game thread: publish the line the Tools page shows. Not `noexcept`: it
    // takes the bridge's mutex and owns a string.
    static void publish_voice_status(std::string status) {
        detail::voice_import.publish_status(std::move(status));
    }
};

// Adapter facade. Every entry point is safe to call when the framework is not
// installed, or when SYNTH was built without its consumer header.
class MenuFramework final {
public:
    // `IsInstalled` is a plain `GetModuleHandleW` probe: it resolves no framework
    // export and reads no renderer-owned state, so it is the one framework entry
    // point with no owning thread. It is still not for the per-frame pump - the
    // pump has `input_captured` for that.
    [[nodiscard]] static bool available() noexcept {
#if SYNTH_HAS_MENU_FRAMEWORK
        return F4SEMenuFramework::IsInstalled();
#else
        return false;
#endif
    }

    // Call from the F4SE kPostLoad message: the framework DLL is only mapped
    // once every plugin has finished loading, so registering earlier silently
    // does nothing when SYNTH sorts first in plugins.txt.
    static void register_surface() noexcept {
#if SYNTH_HAS_MENU_FRAMEWORK
        static bool registered = false;
        if (registered || !F4SEMenuFramework::IsInstalled()) return;
        try {
            F4SEMenuFramework::SetSection("SYNTH");
            F4SEMenuFramework::AddSectionItem("Hotkeys", detail::render_hotkeys);
            F4SEMenuFramework::AddSectionItem("Auto Activate", detail::render_auto_activate);
            F4SEMenuFramework::AddSectionItem("Behavior", detail::render_behavior);
            F4SEMenuFramework::AddSectionItem("Sound", detail::render_sound);
            F4SEMenuFramework::AddSectionItem("Tools", detail::render_tools);

            // Defer SYNTH's windows - the chatbox and the control menu - to the
            // framework's render lifecycle. Both block game input while open so
            // typing or navigating cannot also drive the player. One listener
            // creates and pumps both. The framework keeps ownership of its own
            // main menu: SYNTH neither opens nor closes that.
            detail::chatbox_lifecycle_listener =
                F4SEMenuFramework::Events::Register(detail::on_framework_lifecycle);

            // Publish every SYNTH hotkey into the framework-wide registry for
            // conflict detection. The framework keeps its persisted value; an
            // edit on SYNTH's Hotkeys page updates both it and SYNTH_custom.ini.
            // The runtime itself always reads SYNTH's configuration.
            for (const auto& binding : hotkey_bindings) {
                // Registration is for conflict detection only. The game-thread
                // poll owns every dispatch, including capture-before-pause targeting.
                (void)F4SEMenuFramework::Hotkeys::Register(binding.framework_id, 0, detail::on_hotkey);
            }
            registered = true;
        } catch (...) {
            // Registration is best effort: without it SYNTH simply has no
            // in-game pages, which is the same as the framework being absent.
        }
#endif
    }

    // Game thread: hand the render thread an immutable view of the authoritative
    // configuration. Pass an empty snapshot when no session is loaded.
    static void publish(ConfigSnapshot configuration) {
        if (!configuration) {
            detail::voice_import.set_available(false);
            detail::voice_import.publish_status({});
            // Unconditional like the voice-import reset above: the bridge behind
            // it is plain C++, so a build without the consumer header simply has
            // nothing queued to drop. Nothing the player chose in the session
            // that just ended survives into the next one.
            ControlMenu::invalidate();
        }
#if SYNTH_HAS_MENU_FRAMEWORK
        const auto dropped = !configuration;
        if (dropped) {
            std::scoped_lock lock{detail::edit_mutex};
            detail::pending_edits.clear();
        }
        detail::published_configuration.store(std::move(configuration));
        // Outside that lock: with the session gone nothing is left to read a
        // queued message, and the window must not keep input captured.
        if (dropped) Chatbox::close();
#else
        (void)configuration;
#endif
    }

    // True while a framework window is capturing input. SYNTH suppresses its own
    // polled hotkeys then, matching how the framework suppresses its own.
    //
    // Game thread, and called from the hotkey poll on every pump: a plain read of
    // the state the render thread republished on its last `kBeforeRender`, never
    // a call into the framework. Asking `IsAnyBlockingWindowOpened` here instead
    // put a framework export resolve and a renderer-owned read inside
    // `PlayerCharacter::Update` and hung the frame after a load.
    [[nodiscard]] static bool input_captured() noexcept {
#if SYNTH_HAS_MENU_FRAMEWORK
        return detail::framework_input_captured.load(std::memory_order_relaxed);
#else
        return false;
#endif
    }
};

}  // namespace synth::ui
