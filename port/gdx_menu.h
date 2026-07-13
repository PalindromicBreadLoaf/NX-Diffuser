// port/gdx_menu.h — G-Diffuser in-game enhancement menu (ImGui menu bar).
//
// WHAT THIS IS
// ------------
// This is the P0 "menu shell" the whole enhancement overlay rides on (see
// docs/IMGUI_MENU_SCOPE.md). Until this class is registered, F1 opens NOTHING:
// libultraship (LUS) already wires the F1 / Esc / Gamepad-Back toggle
// (libultraship/src/ship/window/gui/Gui.cpp:206-213) and renders ImGui every frame,
// but the port registers no GuiMenuBar and no windows. Registering one GdxMenuBar via
// Gui::SetMenuBar (Gui.cpp:355) is what makes F1 actually surface a menu.
//
// HOW IT PLUGS INTO LUS
// ---------------------
// We subclass Ship::GuiMenuBar (ship/window/gui/GuiMenuBar.h). Its base Draw() early-returns
// when the bar is hidden and otherwise calls our DrawElement() (verified in
// GuiMenuBar.cpp:20-28). IMPORTANT: despite the header docstring in GuiMenuBar.h claiming the
// base "wraps ImGui::BeginMainMenuBar()/EndMainMenuBar()", the .cpp does NOT — it only calls
// DrawElement(). So DrawElement() itself must open/close the main menu bar with
// ImGui::BeginMainMenuBar()/EndMainMenuBar(). (The stale docstring is why this is spelled out.)
//
// GuiElement (the grandparent) declares InitElement()/UpdateElement()/DrawElement() pure-virtual
// (GuiElement.h:74-81); GuiMenuBar overrides only Draw()+SetVisibility(), so this subclass MUST
// implement all three of InitElement/UpdateElement/DrawElement.
//
// PHASING NOTE
// ------------
// Features that already exist (Audio LLE/HLE + reconstruction filter, the LUS graphics
// "courtesy" CVars, the surfaced LUS dev/input windows) are wired to real controls. Everything
// still parity-blocked or unbuilt is shown as an ImGui::TextDisabled("Coming soon") line so the
// menu documents the roadmap without pretending a feature is live. Every default reproduces
// today's confirmed-good behavior (the "optionality constitution": every default 1:1).

#pragma once

#include <string>
#include "ship/window/gui/GuiMenuBar.h"

/**
 * @brief The G-Diffuser enhancement menu bar (top-of-screen ImGui menu, toggled by F1).
 *
 * Construct with no arguments and hand to Gui::SetMenuBar():
 *   pgui->SetMenuBar(std::make_shared<GdxMenuBar>());
 * The constructor pins the visibility CVar to "gOpenMenuBar" (the same CVar the LUS F1 toggle
 * flips — Gui.cpp) and starts hidden, and registers the port's gEnhancements.* CVars at their
 * 1:1 defaults so a fresh gdiffuser.cfg.json behaves exactly like today.
 */
class GdxMenuBar : public Ship::GuiMenuBar {
  public:
    GdxMenuBar();

    // GuiElement lifecycle (all pure-virtual on the base — must be provided here).
    void InitElement() override;
    void UpdateElement() override;
    void DrawElement() override;

  private:
    // One top-level menu per tab in docs/menu/*.md.
    void DrawGraphicsMenu();
    void DrawAudioMenu();
    void DrawGameplayMenu();
    void DrawPracticeMenu();
    void DrawControlsMenu();
    void DrawWorkshopMenu();
    void DrawOnlineMenu();
    void DrawDeveloperMenu();
    void DrawAboutMenu();

    // Remembers the last non-zero reconstruction-filter cutoff so the "off" checkbox can restore
    // it when re-enabled (0 in the CVar means "filter disabled"). Seeded to the 1:1 default.
    int mLastLowPassHz = 15000;
};
