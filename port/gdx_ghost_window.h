// port/gdx_ghost_window.h — "Ghost Browser" GuiWindow (G-Diffuser Practice tab, v1).
//
// WHAT THIS IS
// ------------
// v1 of the Practice tab's ghost browser (docs/menu/PRACTICE_TAB.md, item #1 "GHOST
// BROWSER"; docs/COMING_SOON_ROADMAP.md Tier 2 "Practice > Ghost browser"). Modeled on
// the same Ship::GuiWindow idiom port/main.cpp already uses for LUS::GfxDebuggerWindow /
// LUS::InputEditorWindow (see main.cpp's two AddGuiWindow calls) and on port/gdx_menu.h's
// class-shape conventions (include paths, "GdxXxx" naming) for a port-owned window.
//
// GROUND TRUTH THIS SCOPE IS BUILT AROUND (full detail in gdx_ghost_window.cpp)
// -------------------------------------------------------------------------------
// - The SRAM ghost slot (gSaveContext.ghostSave, decomp/include/fzx_save.h:108-115) is a
//   SINGLE GhostRecord+GhostData pair, not an array -- at most ONE ghost persists across
//   sessions on this port. port/gdx_ghost_io.c's .gdg export/import already operates on
//   exactly this one slot; this window reads the same slot for display, read-only.
// - gGhosts[3] / gGhostRacers[3] (decomp/src/game/racer.c:55,69) are the in-RACE ghost
//   opponent slots, but decomp/src/overlays/ovl_i2/race.c's Race_Init only calls
//   Save_LoadGhost (which fills gGhosts[]) when gGameMode == GAMEMODE_TIME_ATTACK, and
//   decomp/src/game/racer.c's race-init ghost-racer population loop is likewise gated on
//   `gGameMode == GAMEMODE_TIME_ATTACK` (racer.c:1878). GAMEMODE_PRACTICE never runs
//   either path -- Practice mode races with zero ghost opponents today. Picking "up to 3
//   ghosts to race in Practice" therefore needs a population-path change this window does
//   NOT make (see the .cpp and the reported follow-up scope); v1 does not offer that
//   control, so it never ships a toggle that silently does nothing.
//
// WHAT V1 ACTUALLY DOES
// ----------------------
// Read-only viewer over the one persisted SRAM ghost slot -- course index, ghost type,
// total time, best lap, and machine livery -- via the same Save_ReadGhostRecord /
// Save_ReadGhostData / Save_CalculateGhost*Checksum entry points port/gdx_ghost_io.c
// calls (mirror structs/externs redeclared locally using that file's documented
// PORT/DECOMP boundary idiom -- no decomp header is #included here either; see the .cpp
// for why this window deliberately does NOT call Save_LoadGhostInfo). An "Export to
// .gdg" button calls straight into the existing port/gdx_ghost_io.h public API (no
// export logic duplicated). A permanently-visible note explains why Practice-mode
// "apply" is not offered yet, instead of a dead/no-op control.

#pragma once

#include "ship/window/gui/GuiWindow.h"

/**
 * @brief "Ghost Browser" GuiWindow -- Practice tab, v1 (read-only SRAM ghost slot viewer
 * + a ".gdg" export shortcut). See the file header above for the ground-truth
 * investigation this scope is based on.
 *
 * Construct with the two-arg Ship::GuiWindow(consoleVariable, name) form (visibility
 * derived from the CVar, default hidden) and register via Gui::AddGuiWindow, the same
 * pattern main.cpp already uses for LUS::GfxDebuggerWindow / LUS::InputEditorWindow:
 *
 *   pgui->AddGuiWindow(std::make_shared<GdxGhostWindow>(
 *       "gEnhancements.Practice.GhostBrowserOpen", "Ghost Browser"));
 *
 * The Practice-tab menu toggle mirrors DrawControlsMenu's existing
 * GdxWindowVisible("Ghost Browser") / GdxToggleWindow("Ghost Browser") idiom
 * (see gdx_menu.cpp's GdxToggleWindow/GdxWindowVisible helpers).
 */
class GdxGhostWindow : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

  protected:
    /** @brief No one-time setup needed -- the window has no cached state. */
    void InitElement() override;

    /** @brief No per-frame polling needed -- the SRAM slot is only read while drawing. */
    void UpdateElement() override;

    /** @brief Renders the ghost-slot summary table, export button, and the Practice-mode
     *         ghost-opponent scope note. */
    void DrawElement() override;
};
