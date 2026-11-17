#pragma once

// SYNTH's Yes/Cancel box for a confirmed NPC sale, drawn by the F4SE Menu Framework.
//
// The box is only a view of `trade::prompt()`. The worker that owns the offer opens it and waits;
// this window copies what is open, draws it, and submits one answer against the token it drew. It reads
// no session, no game object and no configuration, so it needs no lock of its own and adds no state to
// the framework callback beyond one window. Cancel has the keyboard focus when the box appears and
// Escape cancels: only a deliberate press of Yes can buy anything.
//
// Created from `menu_framework_ui.hpp`'s single lifecycle listener on `kBeforeRender`, for the same
// reason the chatbox and control menu are: `AddWindow` mutates the framework's window registry and must
// run on the thread that owns it. Without the framework (or without its consumer header at build time)
// nothing here exists and `trade::prompt()` stays uninstalled, so no offer is ever advertised.

#include "trade/confirmed_trade.hpp"

#if defined(SYNTH_WITH_F4SE) && defined(__has_include)
#  if __has_include("F4SEMenuFramework.h")
#    define SYNTH_HAS_MENU_FRAMEWORK 1
#  endif
#endif
#ifndef SYNTH_HAS_MENU_FRAMEWORK
#  define SYNTH_HAS_MENU_FRAMEWORK 0
#endif

#if SYNTH_HAS_MENU_FRAMEWORK
#include <F4SE/F4SE.h>

#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4099)
#  pragma warning(disable : 5054)
#  pragma warning(disable : 4996)
#endif
#ifndef SYNTH_MENU_FRAMEWORK_API_INCLUDED
#define SYNTH_MENU_FRAMEWORK_API_INCLUDED
#include "F4SEMenuFramework.h"
#endif
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <algorithm>
#include <atomic>
#endif

namespace synth::ui::detail {
#if SYNTH_HAS_MENU_FRAMEWORK

// Published by the render thread; read only by it.
inline std::atomic<F4SEMenuFramework::Model::WindowInterface*> trade_window{nullptr};
inline bool trade_window_requested = false;   // render thread only
inline std::uint64_t trade_drawn_token = 0;   // render thread only

// Render thread. Draws the box that is open right now and answers it at most once.
inline void __stdcall render_trade_prompt() {
    auto* window = trade_window.load(std::memory_order_acquire);
    if (window == nullptr || !window->IsOpen.load()) return;
    const auto view = trade::prompt().view();
    if (!view) { window->IsOpen.store(false); return; }

    const auto* viewport = ImGuiMCP::GetMainViewport();
    if (!viewport || viewport->WorkSize.x <= 0.0f || viewport->WorkSize.y <= 0.0f) return;
    const auto scale = std::clamp(viewport->WorkSize.y / 1080.0f, 1.0f, 1.5f);
    const auto width = std::min(640.0f * scale, std::max(1.0f, viewport->WorkSize.x - 48.0f));
    ImGuiMCP::SetNextWindowPos(
        ImGuiMCP::ImVec2{viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f},
        ImGuiMCP::ImGuiCond_Always, ImGuiMCP::ImVec2{0.5f, 0.5f});
    ImGuiMCP::SetNextWindowSize(ImGuiMCP::ImVec2{width, 0.0f}, ImGuiMCP::ImGuiCond_Always);

    std::optional<trade::Answer> chosen;
    bool keep_open = true;
    const auto title = view->title + "##SYNTH.Trade";
    if (ImGuiMCP::Begin(title.c_str(), &keep_open,
                        ImGuiMCP::ImGuiWindowFlags_NoSavedSettings | ImGuiMCP::ImGuiWindowFlags_NoMove |
                            ImGuiMCP::ImGuiWindowFlags_NoResize | ImGuiMCP::ImGuiWindowFlags_NoCollapse |
                            ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGuiMCP::SetWindowFontScale(1.25f * scale);
        ImGuiMCP::PushTextWrapPos(0.0f);
        ImGuiMCP::TextWrapped("%s", view->body.c_str());
        ImGuiMCP::PopTextWrapPos();
        ImGuiMCP::Separator();
        if (ImGuiMCP::Button("Yes##SYNTH.Trade.Yes")) chosen = trade::Answer::yes;
        ImGuiMCP::SameLine();
        // A new box starts on Cancel, so Enter or a gamepad press cannot buy by accident.
        if (view->token != trade_drawn_token) { ImGuiMCP::SetKeyboardFocusHere(); trade_drawn_token = view->token; }
        if (ImGuiMCP::Button("Cancel##SYNTH.Trade.Cancel")) chosen = trade::Answer::cancel;
        if (ImGuiMCP::IsWindowFocused(ImGuiMCP::ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_Escape, false)) chosen = trade::Answer::cancel;
    }
    ImGuiMCP::End();
    // Closing the window is a decline, never a purchase.
    if (!keep_open && !chosen) chosen = trade::Answer::cancel;
    if (chosen) {
        (void)trade::prompt().answer(view->token, *chosen);
        window->IsOpen.store(false);
    }
}

// Render lifecycle, every frame (called from the one framework listener). Creates the window once and
// shows it exactly while a box is open.
inline void trade_prompt_tick() {
    if (!trade_window_requested) {
        trade_window_requested = true;
        auto* window = F4SEMenuFramework::AddWindow(render_trade_prompt, true);
        trade_window.store(window, std::memory_order_release);
        if (window != nullptr) {
            REX::INFO("SYNTH UI: trade confirmation registered on the framework render lifecycle");
        } else {
            // No presenter: offers are refused before any box is needed.
            trade::prompt().set_installed(false);
            REX::WARN("SYNTH UI: framework rejected the trade confirmation window");
        }
    }
    if (auto* window = trade_window.load(std::memory_order_acquire))
        window->IsOpen.store(trade::prompt().view().has_value());
}

#endif  // SYNTH_HAS_MENU_FRAMEWORK
}  // namespace synth::ui::detail
