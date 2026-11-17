#pragma once

#include "adapters/commonlib_capture.hpp"
#include "core/reading_event.hpp"

#include <RE/B/BGSNote.h>
#include <RE/B/BGSTerminal.h>
#include <RE/T/TESActivateEvent.h>
#include <RE/T/TESObjectBOOK.h>
#include <RE/T/TerminalMenu.h>

#include <atomic>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace synth::adapters {

using namespace std::literals;

// Flat-only observer of what the player reads. It runs entirely on the game thread and copies text into
// a core::ReadingEvent; nothing here talks to the network. A reading is only reported when the item it
// belongs to can be named exactly, so an unknown selection reports nothing rather than a guess.
//
// Covered: a book, note or magazine the player activates in the world (the book menu opens on it), and
// the text entry or holotape a terminal shows. Not covered: books, magazines and holotapes opened from
// the Pip-Boy inventory, which expose no item identity to this adapter; audio holotapes are only reported
// when a terminal launches them, and then by title alone.
class FlatReadingEvents final {
public:
    // Idempotent. The activation sink only copies the base form of what the player just activated.
    [[nodiscard]] static bool install() {
        if (installed_) return true;
        auto* source = RE::TESActivateEvent::GetEventSource();
        if (!source) return false;
        source->RegisterSink(&sink_);
        installed_ = true;
        return true;
    }
    [[nodiscard]] static bool available() noexcept { return installed_; }

    // Session retirement or a new save: forget half-observed state so nothing crosses a load.
    static void reset() noexcept {
        candidate_form_.store(0, std::memory_order_release);
        book_open_ = false;
        terminal_open_ = false;
        terminal_key_.clear();
        terminal_mode_ = -1;
    }

    // Game pump only. Returns at most one reading per call, serial and clock left for the caller to stamp.
    [[nodiscard]] static std::optional<core::ReadingEvent> poll(core::SnapshotClock::time_point now) {
        auto* ui = RE::UI::GetSingleton();
        if (!installed_ || !ui) return std::nullopt;
        std::optional<core::ReadingEvent> reading;
        const bool book = ui->GetMenuOpen("BookMenu"sv);
        // A world activation is only a candidate for a moment: a candidate no book menu claimed is dropped.
        expire_candidate(now);
        if (book && !book_open_) reading = book_reading(*ui, now);
        book_open_ = book;
        if (!reading) reading = terminal_reading(*ui, now);
        return reading;
    }

private:
    class ActivateSink final : public RE::BSTEventSink<RE::TESActivateEvent> {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent& event, RE::BSTEventSource<RE::TESActivateEvent>*) override {
            const auto* player = RE::PlayerCharacter::GetSingleton();
            if (player && event.actionRef.get() == player && event.objectActivated) {
                const auto* base = event.objectActivated->GetObjectReference();
                if (base && base->GetFormType() == RE::ENUM_FORM_ID::kBOOK) {
                    candidate_form_.store(base->GetFormID(), std::memory_order_release);
                    candidate_at_.store(core::SnapshotClock::now().time_since_epoch().count(), std::memory_order_release);
                } else {
                    // The player activated something else: an earlier book can no longer be what opens next.
                    candidate_form_.store(0, std::memory_order_release);
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    static std::string text_of(const RE::BGSLocalizedString& value) {
        const std::string_view text{value};
        // An unresolved localization placeholder is not the text the player sees.
        return text.starts_with("<ID=") ? std::string{} : std::string{text};
    }

    static std::optional<core::ReadingEvent> make(core::ReadingKind kind, const RE::TESForm& form, std::string_view title,
                                                  std::string_view raw_text, core::SnapshotClock::time_point now) {
        core::ReadingEvent event;
        event.kind = kind;
        event.form_id = form.GetFormID();
        event.origin_plugin = commonlib::origin_plugin(form);
        event.title = core::bound_reading_title(title);
        auto bounded = core::bound_reading_text(raw_text);
        event.text = std::move(bounded.text);
        event.text_truncated = bounded.truncated && !event.text.empty();
        event.observed_at = now;
        event.serial = 1;  // Placeholder so valid() can judge the rest; the session stamps the real serial.
        if (!event.valid()) return std::nullopt;
        event.serial = 0;
        return event;
    }

    // The book menu just opened: the item is the book the player activated a moment ago, if there is one.
    static void expire_candidate(core::SnapshotClock::time_point now) noexcept {
        if (candidate_form_.load(std::memory_order_acquire) == 0) return;
        const core::SnapshotClock::time_point at{core::SnapshotClock::duration{candidate_at_.load(std::memory_order_acquire)}};
        if (now < at || now - at > candidate_window) candidate_form_.store(0, std::memory_order_release);
    }

    static std::optional<core::ReadingEvent> book_reading(RE::UI& ui, core::SnapshotClock::time_point now) {
        const auto form_id = candidate_form_.exchange(0, std::memory_order_acq_rel);
        // A book menu opened from the Pip-Boy is another item; the claim is only for a world activation.
        if (form_id == 0 || ui.GetMenuOpen("PipboyMenu"sv)) return std::nullopt;
        const core::SnapshotClock::time_point at{core::SnapshotClock::duration{candidate_at_.load(std::memory_order_acquire)}};
        if (now < at || now - at > candidate_window) return std::nullopt;
        auto* book = RE::TESForm::GetFormByID<RE::TESObjectBOOK>(form_id);
        if (!book) return std::nullopt;
        RE::BSString description;
        static_cast<RE::TESDescription*>(book)->GetDescription(description, book);
        const std::string_view name{RE::TESFullName::GetFullName(*book)};
        // Paper notes are private (shared only on request) and never captured here. Magazines and notes are book
        // records; the vanilla keywords tell them apart when present.
        if (book->HasKeywordString("ObjectTypeNote"sv)) return std::nullopt;
        auto kind = core::ReadingKind::book;
        if (book->HasKeywordString("ObjectTypeMagazine"sv)) kind = core::ReadingKind::magazine;
        return make(kind, *book, name, description.c_str() ? std::string_view{description.c_str()} : std::string_view{}, now);
    }

    // One reading per terminal entry opened: a text entry, or a holotape the terminal launches (title only).
    static std::optional<core::ReadingEvent> terminal_reading(RE::UI& ui, core::SnapshotClock::time_point now) {
        if (!ui.GetMenuOpen("TerminalMenu"sv)) {
            terminal_open_ = false;
            terminal_key_.clear();
            terminal_mode_ = -1;
            return std::nullopt;
        }
        auto menu = ui.GetMenu<RE::TerminalMenu>();
        if (!menu) return std::nullopt;
        const auto mode = static_cast<int>(menu->mode.get());
        const bool reading_mode = mode == static_cast<int>(RE::TerminalMenu::Mode::kText) ||
            mode == static_cast<int>(RE::TerminalMenu::Mode::kHolotape);
        const bool entered = !terminal_open_ || mode != terminal_mode_;
        terminal_open_ = true;
        terminal_mode_ = mode;
        if (!reading_mode || !entered || menu->history.empty()) return std::nullopt;
        const RE::BGSTerminal* terminal = menu->history.back();
        if (!terminal) return std::nullopt;
        // The selection is only known when the menu's own last-selection record names an entry of this list.
        const RE::BGSTerminal::MenuItem* chosen = nullptr;
        for (const auto& pair : menu->lastSelectionMap) {
            if (pair.first != static_cast<const void*>(terminal) || pair.second >= menu->menuItemList.size()) continue;
            const auto* item = menu->menuItemList[pair.second].menuItem;
            const auto flag = mode == static_cast<int>(RE::TerminalMenu::Mode::kText)
                ? RE::BGSTerminal::MenuItem::Flag::kText : RE::BGSTerminal::MenuItem::Flag::kHolotape;
            if (item && item->flags.all(flag)) chosen = item;
        }
        if (!chosen) return std::nullopt;
        const auto title = text_of(chosen->itemText);
        const auto key = std::to_string(terminal->GetFormID()) + ":" + title + ":" + std::to_string(mode);
        if (key == terminal_key_) return std::nullopt;
        terminal_key_ = key;
        if (mode == static_cast<int>(RE::TerminalMenu::Mode::kHolotape)) {
            const auto* note = chosen->selectionResult.holotape;
            if (!note) return std::nullopt;
            return make(core::ReadingKind::holotape, *note, RE::TESFullName::GetFullName(*note), {}, now);
        }
        const auto* display = chosen->selectionResult.displayText;
        if (!display) return std::nullopt;
        const auto body = text_of(*display);
        return make(core::ReadingKind::terminal, *terminal, title.empty() ? RE::TESFullName::GetFullName(*terminal) : std::string_view{title},
                    body, now);
    }

    static constexpr std::chrono::seconds candidate_window{3};
    static inline ActivateSink sink_;
    static inline std::atomic_bool installed_{};
    static inline std::atomic_uint32_t candidate_form_{};
    static inline std::atomic<core::SnapshotClock::rep> candidate_at_{};
    static inline bool book_open_{}, terminal_open_{};
    static inline int terminal_mode_{-1};
    static inline std::string terminal_key_;
};

} // namespace synth::adapters
