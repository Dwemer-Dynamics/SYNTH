#pragma once

#if !defined(SYNTH_WITH_F4SE) || defined(SYNTH_WITH_F4SEVR)
#error "Native HUD subtitles require the independently verified flat ABI"
#endif

#include "adapters/commonlib_capture.hpp"
#include "presentation/dialogue_presentation.hpp"

namespace synth::adapters {

// Feed Fallout's own HUDSubtitleText value sink, not the notification or dialogue menu.
// 1.11.240 constructor RVA A2FE08 binds the sink at +258; update RVA A301F0
// consumes its optional value/eventReceived under the +280 spin lock. No hooks,
// engine priority-array edits, actor pointers, or arbitrary event callbacks are needed.
class FlatDialogueSubtitles final {
public:
    void present(const presentation::DialogueCaptions::Caption& caption) {
        if (!caption && !owned_) return;
        static const bool supported = REX::FModule::GetExecutingModule().GetFileVersion().pack() ==
                                      REL::Version{1, 11, 240, 0}.pack();
        if (!supported) return;
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;
        commonlib::TryReadLock menus{RE::UI::GetMenuMapRWLock()};
        if (!menus.owns_lock()) return;
        const auto entry = ui->menuMap.find(RE::BSFixedString{RE::HUDMenu::MENU_NAME});
        if (entry == ui->menuMap.end() || !entry->second.menu) return;
        auto* hud = static_cast<RE::HUDMenu*>(entry->second.menu.get());
        if (*reinterpret_cast<const std::uintptr_t*>(hud) != RE::HUDMenu::VTABLE[0].address() ||
            hud->hudObjects.size() > 128) return;
        for (const auto& component : hud->hudObjects) {
            auto* object = component.get();
            if (!object || *reinterpret_cast<const std::uintptr_t*>(object) !=
                               RE::VTABLE::HUDSubtitleText[0].address()) continue;
            using Sink = RE::BSTValueEventSink<RE::HUDSubtitleDisplayEvent>;
            static_assert(sizeof(Sink) == 0x30);
            auto* sink = reinterpret_cast<Sink*>(reinterpret_cast<std::byte*>(object) + 0x258);
            if (*reinterpret_cast<const std::uintptr_t*>(sink) !=
                RE::VTABLE::BSTValueEventSink_HUDSubtitleDisplayEvent_[0].address()) return;
            commonlib::TrySpinLock data{sink->dataLock};
            if (!data.owns_lock()) return;
            auto& current = sink->eventDataStruct.optionalValue;
            const auto ours = current && owned_ &&
                current->speakerName == owned_->speaker.c_str() && current->subtitleText == owned_->text.c_str();
            // A native line or another mod has priority. Never clear someone else's text.
            if (current && !ours) { owned_.reset(); return; }
            if (caption) {
                if (!ours || owned_ != caption) {
                    current = RE::HUDSubtitleDisplayData{
                        RE::BSFixedStringCS{caption->speaker.c_str()}, RE::BSFixedStringCS{caption->text.c_str()}};
                    sink->eventDataStruct.eventReceived = true;
                    REX::INFO("SYNTH subtitle: {}: {}", caption->speaker, caption->text);
                }
                owned_ = caption;
            } else {
                if (ours) {
                    current.reset();
                    sink->eventDataStruct.eventReceived = true;
                }
                owned_.reset();
            }
            return;
        }
    }

private:
    presentation::DialogueCaptions::Caption owned_;
};

}  // namespace synth::adapters
