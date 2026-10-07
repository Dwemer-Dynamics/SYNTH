#pragma once

#include "adapters/commonlib_capture.hpp"
#include <RE/U/UIMessageQueue.h>

namespace synth::adapters {

// An invisible native menu contributes its own engine-managed pause count. The framework
// still draws and owns input; no global time toggle or other menu's pause is overwritten.
class FlatMenuPause final {
    class PauseMenu final : public RE::IMenu {
    public:
        PauseMenu() {
            menuName = "SYNTHPause";
            menuFlags.set(RE::UI_MENU_FLAGS::kPausesGame);
        }
        bool ShouldHandleEvent(const RE::InputEvent*) override { return false; }
        RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage&) override {
            return RE::UI_MESSAGE_RESULTS::kHandled;
        }
    };
    inline static bool registered_{};
    inline static bool requested_{};

public:
    // Called on every engine-update pump, including while menus pause world simulation.
    static void sync(bool open) {
        if (REX::FModule::GetExecutingModule().GetFileVersion().pack() !=
            REL::Version{1, 11, 240, 0}.pack()) return;
        auto* ui = RE::UI::GetSingleton();
        auto* queue = RE::UIMessageQueue::GetSingleton();
        if (!ui || !queue) return;
        if (!registered_) {
            if (!open) return;
            ui->RegisterMenu("SYNTHPause", [](const RE::UIMessage&) -> RE::IMenu* { return new PauseMenu{}; });
            registered_ = true;
        }
        if (open == requested_) return;
        queue->AddMessage("SYNTHPause", open ? RE::UI_MESSAGE_TYPE::kShow : RE::UI_MESSAGE_TYPE::kForceHide);
        requested_ = open;
    }
};
}
