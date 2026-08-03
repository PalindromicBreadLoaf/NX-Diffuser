// port/gdx_menu_internal.h — helpers shared between the menu shell and its registry.
//
// The G-Diffuser menu is two translation units by design:
//   port/gdx_menu.cpp          the shell: window, header, sidebar, search, MenuDrawItem, and the
//                              WIDGET_CUSTOM blocks that no generic widget can express;
//   port/gdx_menu_registry.cpp the contents: one declaration per control (see port/ui/MenuTypes.h).
// The registry's callbacks are where the live side effects happen — applying a resolution
// multiplier, flipping fullscreen, toggling a GuiWindow, queueing the CVar flush — so both TUs
// need the same small set of libultraship accessors. They are declared here, and defined once in
// gdx_menu.cpp, so there is exactly one copy of each. Everything is main-thread only: the whole
// menu draws inside Gui::StartDraw/EndDraw.
//
// Not a public header. Nothing outside the menu should include it.

#pragma once

#include <memory>
#include <string>

namespace Ship {
class Gui;
class Window;
} // namespace Ship
namespace Fast {
class Fast3dWindow;
} // namespace Fast

namespace gdxmenu {

// Returns the live Gui / top-level window, or nullptr if the window/gui is not up yet. Every caller
// must null-check: these are reachable during early startup and on backends that never construct a
// Fast3d window.
std::shared_ptr<Ship::Gui> GdxGui();
std::shared_ptr<Ship::Window> GdxWindow();
// The window downcast to Fast::Fast3dWindow, or nullptr on any non-Fast3d backend. Needed only for
// SetTextureFilter, which takes a Fast::FilteringMode the Ship::Window base does not know.
std::shared_ptr<Fast::Fast3dWindow> GdxFast3dWindow();

// Schedules a CVar flush to gdiffuser.cfg.json at end of frame (coalesced — safe to call often).
void GdxSaveCvars();

// Flips a registered GuiWindow's LIVE visibility by name, and raises it. Returns false when no such
// window exists, so a caller can say so rather than leaving a button that silently does nothing.
// A bare CVarSetInteger on the visibility CVar would NOT move an already-constructed window.
bool GdxToggleWindow(const char* name);
// True if the named window exists and is currently shown.
bool GdxWindowVisible(const char* name);

// A "Coming soon" roadmap line: a greyed, non-interactive entry naming a planned feature.
void GdxComingSoon(const char* label);
// The subtle accent asterisk marking the item just submitted as differing from its stock default.
void GdxModifiedMarker(bool changed);

// Opens a filesystem directory in the host file browser (created first if absent).
void GdxOpenFolder(const std::string& dir);

std::string GdxLowercase(std::string value);

// "Data & Files": the collapsing on-disk state panel for the three original setup inputs.
void GdxDrawDataAndFilesPanel();

} // namespace gdxmenu
