// port/gdx_menu.cpp — implementation of the G-Diffuser modern full-screen menu.
//
// See gdx_menu.h for the high-level design. This file is pure port-side wiring against LUS's
// public ImGui + CVar API. All feature controls retain their original CVar names, defaults, and
// callbacks; this file changes their presentation and information architecture only.
//
// CVar NAMES ARE STRING LITERALS ON PURPOSE
// -----------------------------------------
// libultraship defines CVAR_* macros (e.g. CVAR_MENU_BAR_OPEN) in cmake/cvars.cmake, but that
// file is include()d only inside libultraship/src (libultraship/src/CMakeLists.txt:1), so its
// add_compile_definitions() do NOT reach the port/ target. We therefore spell the CVar names as
// literals here; each matches cvars.cmake exactly (cross-checked against
// libultraship/cmake/cvars.cmake). The port's own knobs use the gEnhancements.* convention.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // ShellExecuteA for the Workshop "Open folder" buttons
#include <shellapi.h> // ShellExecuteA (excluded by WIN32_LEAN_AND_MEAN)
#endif

#include "gdx_menu.h"

#include <imgui.h> // vendored in libultraship's imgui; already on the port target's include path
                   // (main.cpp already pulls it transitively via GuiWindow.h). Mirrors the
                   // <imgui.h> include used across LUS (e.g. GuiWindow.h:4).

#include "ship/Context.h"           // Ship::Context::GetInstance()
#include "ship/window/Window.h"     // Ship::Window::GetGui() + the SetResolutionMultiplier/
                                    // SetMsaaLevel virtuals used to apply the graphics knobs live
#include "ship/window/gui/Gui.h"    // Ship::Gui::{GetGuiWindow, SaveConsoleVariablesNextFrame}
#include "ship/window/gui/IconsFontAwesome4.h"
#include "fast/Fast3dWindow.h"      // Fast::Fast3dWindow::SetTextureFilter + Fast::FilteringMode
                                    // (the texture-filter setter is Fast3d-only, not on the base
                                    // Ship::Window, so it needs a downcast — see DrawGraphicsMenu)

#include "libultraship/bridge/consolevariablebridge.h" // CVarGet/Set/Register*
#include "libultraship/bridge/audiobridge.h"           // AudioPlayerBuffered (Audio tab status line)

#include <cstring> // strcmp (Audio tab: SDL driver-name check)

// SDL audio driver name for the Audio tab's output-status line. Declared here rather than
// pulling in <SDL2/SDL.h> (this TU builds inside libultraship's include environment where the
// SDL umbrella clashes); the signature matches SDL_audio.h exactly.
extern "C" const char* SDL_GetCurrentAudioDriver(void);

#include <algorithm>
#include <cctype>
#include <cstdio> // snprintf (Practice-tab ghost import/export status line)
#include <cstdlib> // std::system (non-Windows open-folder fallback)
#include <memory> // std::dynamic_pointer_cast (null-safe downcast to Fast::Fast3dWindow)
#include <string>

#include "gdx_ghost_io.h" // .gdg ghost import/export C API (Practice tab Export / Import buttons)
#include "gdx_gui.h"
#include "gdx_workshop.h"    // Workshop tab: texture-pack listing, override count, reload, dump dir
#include "disk_savefile.h"   // Workshop tab "DD Save" subsection: sidecar status + one-shot format

#include <vector>

// From port/input_bridge.c: nonzero while an on-track race is live. The ghost Import writes to the
// SRAM ghost slot, which must not race the game fiber, so the Import button is disabled in-race.
extern "C" int gdx_input_in_gameplay(void);
extern "C" void gdx_game_request_reset(void);

// In-session save-state API (port/gdx_savestate.c). save/load are armed requests fulfilled at the
// frame-loop yield boundary under the audio lock; exists() reports whether a RAM slot is held.
// Gated by gEnhancements.Gameplay.SaveStates (default 0 = strict no-op). Same-race/same-course only.
extern "C" void gdx_savestate_save(void);
extern "C" void gdx_savestate_load(void);
extern "C" int gdx_savestate_exists(void);

// ─────────────────────────────────────────────────────────────────────────────────────────────
// Small helpers (main-thread only — the whole menu draws inside Gui::StartDraw/EndDraw).
// ─────────────────────────────────────────────────────────────────────────────────────────────

namespace {

// Returns the live Gui, or nullptr if the window/gui is not up yet (defensive; the menu only
// draws once the Gui exists, so this is essentially always non-null while visible).
std::shared_ptr<Ship::Gui> GdxGui() {
    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return nullptr;
    }
    auto window = ctx->GetWindow();
    if (window == nullptr) {
        return nullptr;
    }
    return window->GetGui();
}

// Returns the live top-level window, or nullptr if it is not up yet. The window exposes the
// render-backend setters the graphics "read-once" trio needs to apply live (SetResolutionMultiplier
// and SetMsaaLevel are virtuals on the Ship::Window base — Window.h:140,145 — so a plain window
// pointer is enough; SetTextureFilter is Fast3d-only and needs the downcast helper below).
std::shared_ptr<Ship::Window> GdxWindow() {
    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return nullptr;
    }
    return ctx->GetWindow();
}

// Returns the window downcast to Fast::Fast3dWindow, or nullptr if the window is not up yet or the
// active backend is not Fast3d. dynamic_pointer_cast is null-safe: on any non-Fast3d backend it
// yields nullptr and callers simply skip the live apply (the CVar is still saved, so it takes
// effect on the next restart). Used only for SetTextureFilter, which lives on Fast3dWindow (it
// takes a Fast::FilteringMode, a type the Ship::Window base does not know).
std::shared_ptr<Fast::Fast3dWindow> GdxFast3dWindow() {
    return std::dynamic_pointer_cast<Fast::Fast3dWindow>(GdxWindow());
}

// Schedules a CVar flush to gdiffuser.cfg.json at end of frame (coalesced — safe to call often).
// This is exactly the pattern LUS uses after a visibility change (GuiMenuBar.cpp:46).
void GdxSaveCvars() {
    auto gui = GdxGui();
    if (gui != nullptr) {
        gui->SaveConsoleVariablesNextFrame();
    }
}

// Flips a registered GuiWindow's LIVE visibility by name. NOTE (see DEVELOPER_TAB.md): a bare
// CVarSetInteger on the visibility CVar is a NO-OP for an already-constructed window, because the
// window checks its in-memory mIsVisible each frame (the CVar is read only once, at construction).
// ToggleVisibility() flips mIsVisible AND mirrors+persists the CVar (GuiWindow.cpp), which is what
// we want for a live toggle.
void GdxToggleWindow(const char* name) {
    auto gui = GdxGui();
    if (gui == nullptr) {
        return;
    }
    auto window = gui->GetGuiWindow(name);
    if (window != nullptr) {
        window->ToggleVisibility();
    }
}

// True if the named window exists and is currently shown (drives the menu-item checkmark).
bool GdxWindowVisible(const char* name) {
    auto gui = GdxGui();
    if (gui == nullptr) {
        return false;
    }
    auto window = gui->GetGuiWindow(name);
    return window != nullptr && window->IsVisible();
}

// A "Coming soon" roadmap line: a greyed, non-interactive entry naming a planned feature.
void GdxComingSoon(const char* label) {
    ImGui::TextDisabled("%s  -  Coming soon", label);
}

// Opens a filesystem directory in the host file browser (Workshop "Open ... folder" buttons). The
// directory is created first if absent. Windows uses ShellExecute; other hosts fall back to xdg-open.
void GdxOpenFolder(const std::string& dir) {
    if (dir.empty()) {
        return;
    }
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string cmd = "xdg-open '" + dir + "' >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
#endif
}

const ImVec4 kGdxBlue = ImVec4(0.035f, 0.25f, 0.82f, 1.0f);
const ImVec4 kGdxBlueHovered = ImVec4(0.055f, 0.31f, 0.96f, 1.0f);
const ImVec4 kGdxBlueActive = ImVec4(0.025f, 0.18f, 0.67f, 1.0f);

void GdxPushModernStyle() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 7.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 7.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 3.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.93f, 0.97f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.55f, 0.57f, 0.64f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, kGdxBlue);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kGdxBlueHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kGdxBlueActive);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.035f, 0.15f, 0.43f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.045f, 0.22f, 0.65f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.04f, 0.27f, 0.78f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.63f, 0.76f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.48f, 0.64f, 0.96f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.72f, 0.82f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Header, kGdxBlue);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kGdxBlueHovered);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, kGdxBlueActive);
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.63f, 0.65f, 0.72f, 0.65f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.27f, 0.34f, 0.85f));
}

void GdxPopModernStyle() {
    ImGui::PopStyleColor(16);
    ImGui::PopStyleVar(8);
}

bool GdxNavigationButton(const char* label, bool selected, const ImVec2& size) {
    if (!selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    }
    const bool pressed = ImGui::Button(label, size);
    if (!selected) {
        ImGui::PopStyleColor();
    }
    return pressed;
}

std::string GdxLowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────────────────────
// Construction — pin visibility CVar + register the port's gEnhancements.* CVars at 1:1 defaults.
// ─────────────────────────────────────────────────────────────────────────────────────────────

// Base ctor: (visibilityConsoleVariable, isVisible). "gOpenMenuBar" is the compatibility CVar the
// LUS F1 / Esc / Gamepad-Back toggle flips, so binding to it makes those keys open this menu. Start
// hidden (isVisible=false) — the menu is opt-in via F1.
GdxMenu::GdxMenu() : Ship::GuiWindow("gOpenMenuBar", false, "G-Diffuser Menu") {
    CVarRegisterFloat("gSettings.Menu.BackgroundOpacity", 0.85f);
    CVarRegisterInteger("gSettings.Menu.ActiveHeader", static_cast<int>(Header::Settings));
    CVarRegisterInteger("gSettings.Menu.ActivePage", static_cast<int>(Page::General));
    // Gamepad menu navigation ON by default. This is the LUS "gControlNav" CVar; enabling it lets a
    // connected pad both OPEN the menu (Gamepad Back) and navigate it, and blocks game input while
    // the menu is up (see libultraship Gui.cpp / ControlDeck.cpp). Essential on the ROG Ally (no
    // keyboard). The port also feeds ImGui nav from the SDL controller directly (port/gdx_imgui_nav)
    // so this works with any SDL pad — including a raw DualSense — regardless of the ImGui backend's
    // own gamepad reading (ImGui's Win32 backend only sees XInput). CVarRegisterInteger is a no-op
    // if the user already set it, so an explicit OFF in the config is preserved.
    CVarRegisterInteger("gControlNav", 1);
    // One-time migration: configs written before gamepad nav worked have gControlNav stored as 0
    // (the checkbox existed but navigation was broken, so turning it off was the only sane choice).
    // A stored value beats the register default above, which would leave nav permanently dead for
    // exactly the users who tried it early. Flip it ON once; the marker keeps any later deliberate
    // OFF choice intact.
    if (CVarGetInteger("gdx.Migrations.ControlNavDefaultOn", 0) == 0) {
        CVarSetInteger("gdx.Migrations.ControlNavDefaultOn", 1);
        CVarSetInteger("gControlNav", 1);
        CVarSave();
    }
    // Register the port's own feature CVars so a fresh gdiffuser.cfg.json reproduces today's
    // confirmed-good behavior. CVarRegisterInteger is a no-op when the CVar already has a value
    // (i.e. it was loaded from config), so existing user settings are never clobbered.
    //
    // AUDIO tab CVars (all live-read on the audio thread except BufferFrames). Defaults:
    //   gEnhancements.Audio.LLE          = 1     -> LLE (accurate cxd4 RSP) engine, current default.
    //   gEnhancements.Audio.LowPassHz    = 15000 -> output reconstruction low-pass cutoff in Hz;
    //                                               0 disables the filter. (Decided value per the
    //                                               task; AUDIO_SETTINGS_SCOPE.md's text says 11000
    //                                               — flagged for verification.)
    //   gEnhancements.Audio.MasterVolume = 100   -> final-stage output gain 0..100 (%). 100 = no-op
    //                                               (applied on the s16 copy in os.cpp's
    //                                               osAiSetNextBuffer; 100 skips the multiply so the
    //                                               default is bit-exact).
    //   gEnhancements.Audio.Reverb       = 1     -> HLE reverb wet->dry return ON (1) / OFF (0).
    //                                               Honored live by n64_audio_hle.c's A_MIXER kill
    //                                               switch. Affects the HLE path only (LLE reverb is
    //                                               the ucode's own).
    //   gEnhancements.Audio.BufferFrames = 4096  -> dedicated-audio-thread reservoir size in frames.
    //                                               Read ONCE at InitAudio (main.cpp) -> a change
    //                                               applies on restart. 4096 = today's hardcoded value.
    // Every default reproduces today's confirmed-good behavior (the optionality constitution: every
    // default 1:1). CVarRegisterInteger is a no-op when the CVar already has a value (loaded from
    // config), so existing user settings are never clobbered.
    //
    // The GRAPHICS controls bind to LUS-owned g* CVars (gInternalResolution, gMSAAValue,
    // gTextureFilter, ...), which libultraship registers itself; we must NOT re-register those.
    CVarRegisterInteger("gEnhancements.Audio.LLE", 1);
    CVarRegisterInteger("gEnhancements.Audio.LowPassHz", 15000);
    CVarRegisterInteger("gEnhancements.Audio.MasterVolume", 100);
    CVarRegisterInteger("gEnhancements.Audio.Reverb", 1);
    CVarRegisterInteger("gEnhancements.Audio.BufferFrames", 4096);
    // Adaptive final-lap audio uses the Audio.* namespace but is surfaced in the Gameplay tab.
    //   gEnhancements.Audio.FinalLapAdaptive = 0 -> off = stock. When on, a subtle +15% BGM lift on
    //                                               the final lap (audio/disk/external.c NA_SE_18 hook).
    CVarRegisterInteger("gEnhancements.Audio.FinalLapAdaptive", 0);

    // GRAPHICS tab enhancement CVars (port-owned; distinct from the LUS-owned g* CVars used in
    // DrawGraphicsMenu). Every default reproduces today's rendering (the optionality constitution):
    //   gEnhancements.Graphics.Widescreen         = 1   -> today's always-on aspect correction. Read
    //                                                      live in interpreter.cpp AdjXForAspectRatio;
    //                                                      1 = current 16:9 hor+ (byte-identical),
    //                                                      0 = 4:3 pillarbox.
    //   gEnhancements.Graphics.WidescreenUI       = 0   -> stock proportional 4:3 UI placement;
    //                                                      1 = true-corner 1P HUD + selected
    //                                                      full-width 2D scopes.
    //   gEnhancements.Graphics.DrawDistance       = 100 -> per-venue far-render scale in %. 100 = stock
    //                                                      (1.0x, bit-exact). course.c Course_Draw,
    //                                                      clamped 100..300.
    //   gEnhancements.Graphics.ForceMaxMachineLOD = 0   -> 0 = stock distance-based machine LOD; 1 pins
    //                                                      the highest-detail model. racer.c Racer_Draw.
    CVarRegisterInteger("gEnhancements.Graphics.Widescreen", 1);
    // Opt-in 2D widescreen layout: true-corner 1P HUD, full-width SELECT MACHINE blue
    // background, and full-width race transitions. Other menu artwork remains 4:3.
    CVarRegisterInteger("gEnhancements.Graphics.WidescreenUI", 0);
    CVarRegisterInteger("gEnhancements.Graphics.DrawDistance", 100);
    CVarRegisterInteger("gEnhancements.Graphics.ForceMaxMachineLOD", 0);
    //   gEnhancements.Graphics.FramePacing = 0 -> off = stock. libultraship's Fast3D backend already
    //                                             limits the loop to ~60fps; when on, port/gdx_frame_
    //                                             pacer.c pins it to the N64 NTSC rate (~59.94Hz).
    //                                             Recommend VSync OFF when enabled (avoids beating).
    CVarRegisterInteger("gEnhancements.Graphics.FramePacing", 0);

    // GAMEPLAY tab CVars. Every default reproduces stock behavior (the optionality constitution):
    //   gEnhancements.Gameplay.AutosaveOnRecord  = 0    -> off. Stock F-Zero X already commits the
    //                                                      NUMERIC records (times/best-lap/max-speed/
    //                                                      death-race) to SRAM immediately on finish
    //                                                      (menus.c:252-268); this toggle only adds
    //                                                      auto-persisting the best GHOST replay,
    //                                                      which stock saves solely via the manual
    //                                                      Save-Ghost prompt. 0 keeps ghosts manual =
    //                                                      stock behavior. Consumed in menus.c
    //                                                      (Gdx_AutosaveGhostOnRecord).
    CVarRegisterInteger("gEnhancements.Gameplay.AutosaveOnRecord", 0);
    //   gEnhancements.Gameplay.SaveStates = 0 -> off = stock (strict no-op: no allocation, no
    //                                            capture). When on, Quick Save/Load snapshot the race
    //                                            (RDRAM + curated racer/camera/RNG/game globals) to a
    //                                            single RAM slot at the frame-loop yield boundary
    //                                            under the audio lock. SAME-RACE/SAME-COURSE only;
    //                                            audio does not rewind. Consumed in gdx_savestate.c.
    CVarRegisterInteger("gEnhancements.Gameplay.SaveStates", 0);
    //   gEnhancements.Gameplay.SkippableTransitions = 0 -> off = stock (transitions play fully). When
    //                                                      on, the transition overlay fast-completes
    //                                                      (transition.c Transition_Update). [PB].
    CVarRegisterInteger("gEnhancements.Gameplay.SkippableTransitions", 0);
    //   gEnhancements.Gameplay.ReduceEditorFlashing = 1 -> on by default: the node blink/checker
    //                                                      parity and the flagged-node size pulse
    //                                                      advance at half rate, halving the ~20 Hz
    //                                                      strobe on modern displays. Off restores the
    //                                                      stock N64 Course Edit strobe bit-identical.
    //                                                      Consumed in course_edit/191080.c
    //                                                      func_xk2_800E04E0 (#ifdef PORT).
    CVarRegisterInteger("gEnhancements.Gameplay.ReduceEditorFlashing", 1);
    // One-time migration: the CVar originally registered (and therefore persisted) as 0 before the
    // default flipped to ON, so existing configs pin the old value. Same marker pattern as
    // gdx.Migrations.ControlNavDefaultOn above; a later deliberate OFF stays untouched.
    if (CVarGetInteger("gdx.Migrations.ReduceEditorFlashingOn", 0) == 0) {
        CVarSetInteger("gdx.Migrations.ReduceEditorFlashingOn", 1);
        CVarSetInteger("gEnhancements.Gameplay.ReduceEditorFlashing", 1);
        CVarSave();
    }

    // PRACTICE tab CVars. Every default reproduces stock behavior:
    //   gEnhancements.Practice.ShowLapDeltas = 0 -> off = stock (nothing drawn). When on, a small
    //                                               lap-split delta vs the session best (or a loaded
    //                                               ghost's same lap, once ghosts run in Practice) is
    //                                               drawn in Practice mode only. hud.c (#ifdef PORT).
    CVarRegisterInteger("gEnhancements.Practice.ShowLapDeltas", 0);
    //   gEnhancements.Practice.PhotoMode = 0 -> off = stock. When on, pausing during a race hides the
    //                                           HUD and frees the camera; camera.c saves/restores
    //                                           eye/at/fov each frame so unpausing is 1:1. camera.c +
    //                                           hud.c (#ifdef PORT). (GhostBrowserOpen is auto-
    //                                           registered by the GuiWindow ctor in main.cpp.)
    CVarRegisterInteger("gEnhancements.Practice.PhotoMode", 0);

    // Workshop W0 (texture packs + dump). Every knob defaults OFF/empty per the optionality
    // constitution: a fresh config mounts no override behavior and renders bit-identically to stock.
    //   gEnhancements.Workshop.TexturePacks = 0 -> off = stock rendering. When on, the Tier-B shim
    //                                              (n64_gfx_bridge.cpp) rewrites a common-asset load
    //                                              to a mounted pack's "textures/pack/<key>" resource.
    CVarRegisterInteger("gEnhancements.Workshop.TexturePacks", 0);
    //   gEnhancements.Workshop.TextureDump = 0 -> off. When on, every decoded texture is written to
    //                                             dump/<key>.png + dump/manifest.tsv (first-seen-wins).
    CVarRegisterInteger("gEnhancements.Workshop.TextureDump", 0);
    //   gEnhancements.Workshop.DisabledPacks = "" -> comma-joined mods/*.o2r basenames to skip at
    //                                               mount time (per-pack enable toggles in the tab).
    CVarRegisterString("gEnhancements.Workshop.DisabledPacks", "");
    //   gEnhancements.Workshop.AllowDDFormatOnce = 0 -> one-shot: when set to 1 (persisted), the D6
    //                                                  disk-format guard consumes it at the NEXT boot
    //                                                  to authorize a single MFS format into the
    //                                                  sidecar (never the .ndd), then clears it.
    CVarRegisterInteger("gEnhancements.Workshop.AllowDDFormatOnce", 0);

    // Seed the "last cutoff" restore value from whatever is persisted (falls back to 15000).
    int hz = CVarGetInteger("gEnhancements.Audio.LowPassHz", 15000);
    if (hz > 0) {
        mLastLowPassHz = hz;
    }
}

void GdxMenu::InitElement() {
    const int header = CVarGetInteger("gSettings.Menu.ActiveHeader", static_cast<int>(Header::Settings));
    const int page = CVarGetInteger("gSettings.Menu.ActivePage", static_cast<int>(Page::General));
    if (header >= static_cast<int>(Header::Settings) && header <= static_cast<int>(Header::DevTools)) {
        mActiveHeader = static_cast<Header>(header);
    }
    if (page >= static_cast<int>(Page::General) && page <= static_cast<int>(Page::GfxDebugger)) {
        mActivePage = static_cast<Page>(page);
    }
    if (HeaderForPage(mActivePage) != mActiveHeader) {
        mActivePage = FirstPageForHeader(mActiveHeader);
    }
}

void GdxMenu::UpdateElement() {
}

void GdxMenu::Draw() {
    if (!IsVisible()) {
        // Menu just closed (or was never open this frame): undo any nav-repeat tuning we applied and
        // clear the open-transition latch so focus is re-seeded the next time the menu opens.
        RestoreNavRepeatTuning();
        mMenuWasVisible = false;
        return;
    }
    DrawElement();
    SyncVisibilityConsoleVariable();
}

void GdxMenu::RestoreNavRepeatTuning() {
    if (!mNavTuningApplied) {
        return;
    }
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGuiIO& io = ImGui::GetIO();
        io.KeyRepeatDelay = mSavedKeyRepeatDelay;
        io.KeyRepeatRate = mSavedKeyRepeatRate;
    }
    mNavTuningApplied = false;
}

void GdxMenu::DrawElement() {
    const bool navActive = CVarGetInteger("gControlNav", 0) != 0;

    // On each open, seed nav focus onto the active sidebar page (consumed in DrawSidebar). Only when
    // gamepad nav is on, so mouse/keyboard users are not force-focused away from the search box.
    if (!mMenuWasVisible) {
        mMenuWasVisible = true;
        mFocusSidebar = navActive;
    }

    // Snappier held-direction navigation while the pad drives our menu. ImGui derives nav-move repeat
    // from io.KeyRepeatDelay/Rate (NavMove = Delay*0.72, Rate*0.80). Tightening them a touch makes
    // holding a direction feel responsive instead of laggy when scrolling a long sidebar/page. Values
    // are conservative (defaults are 0.275 / 0.050) and applied only while THIS menu is open with
    // gamepad nav on; restored on close (Draw) or when the user turns nav off. Game input is blocked
    // while the menu is up, so this never affects gameplay. Tune here if it still feels off.
    if (navActive && !mNavTuningApplied) {
        ImGuiIO& io = ImGui::GetIO();
        mSavedKeyRepeatDelay = io.KeyRepeatDelay;
        mSavedKeyRepeatRate = io.KeyRepeatRate;
        io.KeyRepeatDelay = 0.22f;
        io.KeyRepeatRate = 0.045f;
        mNavTuningApplied = true;
    } else if (!navActive && mNavTuningApplied) {
        RestoreNavRepeatTuning();
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float opacity = std::clamp(CVarGetFloat("gSettings.Menu.BackgroundOpacity", 0.85f), 0.35f, 1.0f);

    GdxPushModernStyle();
    ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.006f, 0.008f, 0.018f, opacity));
    const ImGuiWindowFlags outerFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("G-Diffuser Menu##Modern", nullptr, outerFlags)) {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        ImVec2 menuSize = available;
        if (available.x > 1280.0f) {
            menuSize.x = (std::min)(available.x * 0.90f, available.y * 1.78f);
        }
        if (available.y > 800.0f) {
            menuSize.y = available.y * 0.90f;
        }
        menuSize.x = (std::max)(menuSize.x, (std::min)(available.x, 640.0f));
        menuSize.y = (std::max)(menuSize.y, (std::min)(available.y, 480.0f));

        ImGui::SetCursorPos((available - menuSize) * 0.5f);
        // NavFlattened on the block + sidebar + content children: ImGui gamepad/keyboard
        // navigation cannot cross a child-window border without it, so a pad could move
        // within the sidebar but NEVER reach the content pane's widgets ("can't enter the
        // sub-menus to edit settings"). Flattened, the whole panel is one nav surface:
        // Right from a sidebar page crosses into the content and A activates widgets.
        if (ImGui::BeginChild("##ModernMenuBlock", menuSize, ImGuiChildFlags_NavFlattened,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            DrawHeader();
            ImGui::Separator();

            const float bodyHeight = ImGui::GetContentRegionAvail().y;
            const float sidebarWidth = menuSize.x > 1500.0f ? menuSize.x * 0.15f : 210.0f;
            if (ImGui::BeginChild("##ModernSidebar", ImVec2(sidebarWidth, bodyHeight), ImGuiChildFlags_NavFlattened)) {
                DrawSidebar();
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImVec2 dividerMin = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(dividerMin, dividerMin + ImVec2(3.0f, bodyHeight),
                                                       ImGui::GetColorU32(ImGuiCol_Separator));
            ImGui::Dummy(ImVec2(3.0f, bodyHeight));
            ImGui::SameLine();

            const float contentWidth = ImGui::GetContentRegionAvail().x;
            if (ImGui::BeginChild("##ModernContent", ImVec2(contentWidth, bodyHeight), ImGuiChildFlags_NavFlattened,
                                  ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
                if (GdxGuiFontLarge() != nullptr) {
                    ImGui::PushFont(GdxGuiFontLarge());
                }
                ImGui::TextUnformatted(mSearch[0] != '\0' ? "Search Results" : PageTitle(mActivePage));
                if (GdxGuiFontLarge() != nullptr) {
                    ImGui::PopFont();
                }
                ImGui::Separator();
                DrawCurrentPage();
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();

        DrawQuitModal();

        // B / Circle = "back". If a widget is actively being edited or a popup (e.g. the quit modal,
        // a combo) is open, ImGui already uses B to cancel that — leave it alone. Otherwise, at the
        // top level, B closes the menu, matching console expectations. Edge-triggered so a held B does
        // not re-fire. Only when gamepad nav is on.
        if (navActive && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) &&
            !ImGui::IsAnyItemActive() &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
            Hide();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    GdxPopModernStyle();
}

void GdxMenu::DrawHeader() {
    const bool navActive = CVarGetInteger("gControlNav", 0) != 0;

    // Shoulder buttons cycle the header tabs (wrapping). ImGui only reads the D-pad for menu
    // movement; it uses L1/R1 for window-cycling ONLY while the Menu button (FaceLeft) is held, and
    // as slider tweak-speed ONLY while a slider is actively being dragged. A bare shoulder tap in
    // this single fullscreen window hits neither of those paths, so an edge-triggered read is safe
    // and needs no SetKeyOwner juggling. Suppressed while any item is active so we never yank focus
    // out of a slider/text field mid-edit.
    if (navActive && !ImGui::IsAnyItemActive()) {
        int dir = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) dir += 1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) dir -= 1;
        if (dir != 0) {
            const int count = 5;
            const int idx = (static_cast<int>(mActiveHeader) + dir + count) % count;
            mSearch[0] = '\0';
            SelectHeader(static_cast<Header>(idx)); // sets mFocusSidebar -> focus lands on the new tab
        }
    }

    const char* labels[] = { "Settings", "Enhancements", "Workshop", "Online", "Dev Tools" };
    const float height = ImGui::GetFrameHeight() + 4.0f;
    const float controlsWidth = ImGui::GetFrameHeight() * 3.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    const float searchWidth = ImGui::GetContentRegionAvail().x >= 900.0f ? 210.0f : 140.0f;

    if (ImGui::BeginTable("##ModernHeader", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX)) {
        ImGui::TableSetupColumn("Navigation", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Search", ImGuiTableColumnFlags_WidthFixed, searchWidth);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, controlsWidth);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (ImGui::BeginChild("##HeaderNavigation", ImVec2(0, height), ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            // Discoverability: flank the tab strip with shoulder-button hints when gamepad nav is on,
            // so the L1/R1 tab-cycling is visible rather than hidden.
            if (navActive) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled(ICON_FA_CHEVRON_LEFT " LB");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous tab (L1 / LB)");
            }
            for (int i = 0; i < 5; ++i) {
                if (i > 0 || navActive) {
                    ImGui::SameLine();
                }
                const Header header = static_cast<Header>(i);
                const ImVec2 buttonSize(ImGui::CalcTextSize(labels[i]).x + 20.0f, ImGui::GetFrameHeight());
                if (GdxNavigationButton(labels[i], mActiveHeader == header, buttonSize)) {
                    mSearch[0] = '\0';
                    SelectHeader(header);
                }
            }
            if (navActive) {
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("RB " ICON_FA_CHEVRON_RIGHT);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Next tab (R1 / RB)");
            }
        }
        ImGui::EndChild();

        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##MenuSearch", "Search...", mSearch, sizeof(mSearch));

        ImGui::TableSetColumnIndex(2);
        const ImVec2 actionSize(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.0f, 0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.03f, 0.03f, 1.0f));
        if (ImGui::Button(ICON_FA_POWER_OFF "##Quit", actionSize)) {
            mOpenQuitModal = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Quit G-Diffuser");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_UNDO "##Reset", actionSize)) {
            // The menu is already on the host/UI side of the bridge; request the reset directly.
            // Ctrl+R still uses the console command, and both converge on the same deferred flag.
            gdx_game_request_reset();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset game (Ctrl+R)");
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.31f, 0.32f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.42f, 0.43f, 0.47f, 1.0f));
        if (ImGui::Button(ICON_FA_TIMES_CIRCLE "##Close", actionSize)) {
            Hide();
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close menu (Esc or F1)");
        ImGui::EndTable();
    }
}

void GdxMenu::DrawSidebar() {
    // When flagged (menu just opened or the tab changed) and gamepad nav is on, park the nav cursor
    // on the active page so the pad has a sensible starting point. SetKeyboardFocusHere() targets the
    // NEXT submitted item and is the reliable idiom for moving nav focus with NavEnableGamepad;
    // SetItemDefaultFocus() covers the very first appearance of the child. From here, pressing Right
    // hands off to the content pane via ImGui's spatial nav.
    const bool wantFocus = mFocusSidebar && CVarGetInteger("gControlNav", 0) != 0;

    auto pageButton = [&](Page page) {
        const char* title = PageTitle(page);
        const bool isActive = mSearch[0] == '\0' && mActivePage == page;
        if (wantFocus && isActive) {
            ImGui::SetKeyboardFocusHere();
        }
        if (GdxNavigationButton(title, isActive, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
            mSearch[0] = '\0';
            SelectPage(page);
        }
        if (wantFocus && isActive) {
            ImGui::SetItemDefaultFocus();
        }
    };

    switch (mActiveHeader) {
        case Header::Settings:
            pageButton(Page::General);
            pageButton(Page::Audio);
            pageButton(Page::Graphics);
            pageButton(Page::Controls);
            pageButton(Page::InputViewer);
            break;
        case Header::Enhancements:
            pageButton(Page::EnhancementGraphics);
            pageButton(Page::Gameplay);
            pageButton(Page::Practice);
            break;
        case Header::Workshop:
            pageButton(Page::Ghosts);
            pageButton(Page::Content);
            break;
        case Header::Online:
            pageButton(Page::OnlineOverview);
            break;
        case Header::DevTools:
            pageButton(Page::DeveloperGeneral);
            pageButton(Page::Stats);
            pageButton(Page::Console);
            pageButton(Page::GfxDebugger);
            break;
    }

    // One-shot: focus request (if any) has now been submitted for this frame.
    mFocusSidebar = false;
}

void GdxMenu::DrawCurrentPage() {
    if (mSearch[0] != '\0') {
        DrawSearchResults();
        return;
    }

    switch (mActivePage) {
        case Page::General: DrawGeneralPage(); break;
        case Page::Audio: DrawAudioMenu(); break;
        case Page::Graphics: DrawGraphicsMenu(false); break;
        case Page::Controls: DrawControlsMenu(); break;
        case Page::InputViewer: DrawInputViewerMenu(); break;
        case Page::EnhancementGraphics: DrawGraphicsMenu(true); break;
        case Page::Gameplay: DrawGameplayMenu(); break;
        case Page::Practice: DrawPracticeMenu(); break;
        case Page::Ghosts: DrawGhostsMenu(); break;
        case Page::Content: DrawWorkshopMenu(); break;
        case Page::OnlineOverview: DrawOnlineMenu(); break;
        case Page::DeveloperGeneral: DrawDeveloperMenu(); break;
        case Page::Stats:
            DrawToolWindowPage("Stats", "Live frame timing and renderer statistics.");
            break;
        case Page::Console:
            DrawToolWindowPage("Console", "Developer console and command history.");
            break;
        case Page::GfxDebugger:
            DrawToolWindowPage("Gfx Debugger", "Inspect Fast3D display-list execution and rendering state.");
            break;
    }
}

void GdxMenu::DrawSearchResults() {
    struct SearchPage {
        Page page;
        const char* terms;
    };
    static const SearchPage pages[] = {
        { Page::General, "general menu opacity controller navigation about credits licenses" },
        { Page::Audio, "audio lle hle filter low pass volume reverb latency buffer" },
        { Page::Graphics, "graphics internal resolution msaa texture filter vsync fullscreen z fighting" },
        { Page::Controls, "controls controller configuration keyboard gamepad mouse bindings remap" },
        { Page::InputViewer, "input viewer overlay analog stick buttons speedrun" },
        { Page::EnhancementGraphics, "graphics enhancements widescreen hud ui draw distance lod frame pacing" },
        { Page::Gameplay, "gameplay save states transitions autosave ghost adaptive final lap" },
        { Page::Practice, "practice lap delta ghost import export photo mode free camera replay" },
        { Page::Ghosts, "ghost browser replay library opponents import export staff player" },
        { Page::Content, "workshop content texture packs track cup machine mods dump reload hi-res font" },
        { Page::OnlineOverview, "online leaderboard ghost upload download netplay spectator" },
        { Page::DeveloperGeneral, "developer multi viewport tools" },
        { Page::Stats, "stats fps frame timing performance" },
        { Page::Console, "console commands log reset" },
        { Page::GfxDebugger, "gfx graphics debugger display list rendering" },
    };

    const std::string query = GdxLowercase(mSearch);
    int matches = 0;
    for (const SearchPage& entry : pages) {
        const std::string haystack = GdxLowercase(std::string(PageTitle(entry.page)) + " " + entry.terms);
        if (haystack.find(query) == std::string::npos) {
            continue;
        }
        ++matches;
        ImGui::PushID(static_cast<int>(entry.page));
        if (ImGui::Button(PageTitle(entry.page),
                          ImVec2((std::min)(430.0f, ImGui::GetContentRegionAvail().x), 0.0f))) {
            mSearch[0] = '\0';
            SelectPage(entry.page);
        }
        ImGui::SameLine();
        const Header header = HeaderForPage(entry.page);
        const char* headerName = header == Header::Settings       ? "Settings"
                                 : header == Header::Enhancements ? "Enhancements"
                                 : header == Header::Workshop     ? "Workshop"
                                 : header == Header::Online       ? "Online"
                                                                    : "Dev Tools";
        ImGui::TextDisabled("%s", headerName);
        ImGui::PopID();
    }
    if (matches == 0) {
        ImGui::TextDisabled("No settings or tools match \"%s\".", mSearch);
    }
}

void GdxMenu::DrawQuitModal() {
    if (mOpenQuitModal) {
        ImGui::OpenPopup("Quit G-Diffuser");
        mOpenQuitModal = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Quit G-Diffuser", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("Are you sure you want to quit G-Diffuser?");
        ImGui::Spacing();
        if (ImGui::Button("Quit", ImVec2(90.0f, 0.0f))) {
            Hide();
            if (auto window = GdxWindow()) {
                window->Close();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void GdxMenu::SelectHeader(Header header) {
    mActiveHeader = header;
    mActivePage = FirstPageForHeader(header);
    CVarSetInteger("gSettings.Menu.ActiveHeader", static_cast<int>(mActiveHeader));
    CVarSetInteger("gSettings.Menu.ActivePage", static_cast<int>(mActivePage));
    GdxSaveCvars();
    // A tab change moves the whole page list; re-park the nav cursor on the new tab's first page so
    // the pad does not end up focused on a now-hidden item. Harmless with mouse/keyboard (gated in
    // DrawSidebar on gControlNav).
    mFocusSidebar = true;
}

void GdxMenu::SelectPage(Page page) {
    mActivePage = page;
    mActiveHeader = HeaderForPage(page);
    CVarSetInteger("gSettings.Menu.ActiveHeader", static_cast<int>(mActiveHeader));
    CVarSetInteger("gSettings.Menu.ActivePage", static_cast<int>(mActivePage));
    GdxSaveCvars();
}

GdxMenu::Header GdxMenu::HeaderForPage(Page page) const {
    if (page <= Page::InputViewer) return Header::Settings;
    if (page <= Page::Practice) return Header::Enhancements;
    if (page <= Page::Content) return Header::Workshop;
    if (page == Page::OnlineOverview) return Header::Online;
    return Header::DevTools;
}

GdxMenu::Page GdxMenu::FirstPageForHeader(Header header) const {
    switch (header) {
        case Header::Settings: return Page::General;
        case Header::Enhancements: return Page::EnhancementGraphics;
        case Header::Workshop: return Page::Ghosts;
        case Header::Online: return Page::OnlineOverview;
        case Header::DevTools: return Page::DeveloperGeneral;
    }
    return Page::General;
}

const char* GdxMenu::PageTitle(Page page) const {
    switch (page) {
        case Page::General: return "General";
        case Page::Audio: return "Audio";
        case Page::Graphics: return "Graphics";
        case Page::Controls: return "Controls";
        case Page::InputViewer: return "Input Viewer";
        case Page::EnhancementGraphics: return "Graphics";
        case Page::Gameplay: return "Gameplay";
        case Page::Practice: return "Practice";
        case Page::Ghosts: return "Ghosts";
        case Page::Content: return "Content";
        case Page::OnlineOverview: return "Overview";
        case Page::DeveloperGeneral: return "General";
        case Page::Stats: return "Stats";
        case Page::Console: return "Console";
        case Page::GfxDebugger: return "Gfx Debugger";
    }
    return "General";
}

void GdxMenu::DrawGeneralPage() {
    ImGui::SeparatorText("Menu Settings");
    float opacity = std::clamp(CVarGetFloat("gSettings.Menu.BackgroundOpacity", 0.85f), 0.35f, 1.0f);
    int opacityPercent = static_cast<int>(opacity * 100.0f + 0.5f);
    if (ImGui::SliderInt("Menu background opacity", &opacityPercent, 35, 100, "%d%%",
                         ImGuiSliderFlags_AlwaysClamp)) {
        CVarSetFloat("gSettings.Menu.BackgroundOpacity", static_cast<float>(opacityPercent) / 100.0f);
        GdxSaveCvars();
    }
    bool controllerNav = CVarGetInteger("gControlNav", 0) != 0;
    if (ImGui::Checkbox("Menu controller navigation", &controllerNav)) {
        CVarSetInteger("gControlNav", controllerNav ? 1 : 0);
        GdxSaveCvars();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Lets a connected gamepad navigate the menu. Game input is blocked while the menu is open.");
    }
    ImGui::TextDisabled("Open or close this menu with F1, Escape, or Gamepad Back.");
    ImGui::Spacing();
    DrawAboutMenu();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 1) GRAPHICS — LUS "courtesy panel" CVars (READY, wired) + port features (Coming soon).
//    docs/menu/GRAPHICS_TAB.md. The read-once trio (internal res / MSAA / texture filter) is
//    consumed by the backend only at window Init, so a plain CVar write is inert until the matching
//    LUS setter is called. We now call those setters ON CHANGE so the controls apply LIVE (the
//    standard SoH apply pattern: CVarSet + CVarSave + Set...()):
//      - internal res -> Ship::Window::SetResolutionMultiplier(float)  (Window.h:140, base virtual)
//      - MSAA         -> Ship::Window::SetMsaaLevel(uint32_t)          (Window.h:145, base virtual)
//      - tex filter   -> Fast::Fast3dWindow::SetTextureFilter(FilteringMode) (Fast3dWindow.h:81 —
//                        Fast3d-only, so a null-safe downcast; skipped w/ CVar-only fallback if the
//                        backend is not Fast3d).
//    The setters run on the render/GUI thread the menu already draws on (no new thread path). VSync
//    and z-fighting are read live by the backend, while fullscreen uses the active Window API.
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenu::DrawGraphicsMenu(bool enhancementsOnly) {
    if (!enhancementsOnly) {
    ImGui::SeparatorText("Renderer");

    // Internal resolution — CVar gInternalResolution (float multiplier), default 1.0. Read once at
    // Init (interpreter.cpp); applied LIVE here via SetResolutionMultiplier (base Ship::Window
    // virtual — no downcast needed).
    {
        float mult = CVarGetFloat("gInternalResolution", 1.0f);
        if (ImGui::SliderFloat("Internal resolution (x)", &mult, 0.5f, 4.0f, "%.2f")) {
            if (mult < 0.5f) {
                mult = 0.5f;
            }
            CVarSetFloat("gInternalResolution", mult);
            GdxSaveCvars();
            auto window = GdxWindow();
            if (window != nullptr) {
                window->SetResolutionMultiplier(mult); // apply live (Fast3dWindow.cpp:315)
            }
        }
    }

    // MSAA — CVar gMSAAValue (int sample count), default 1 (= off). Read once at Init; applied LIVE
    // here via SetMsaaLevel (base Ship::Window virtual — no downcast needed).
    {
        static const int kMsaaValues[] = { 1, 2, 4, 8 };
        static const char* const kMsaaLabels[] = { "Off (1x)", "2x", "4x", "8x" };
        int cur = CVarGetInteger("gMSAAValue", 1);
        int idx = 0;
        for (int i = 0; i < 4; ++i) {
            if (kMsaaValues[i] == cur) {
                idx = i;
            }
        }
        if (ImGui::Combo("MSAA", &idx, kMsaaLabels, 4)) {
            CVarSetInteger("gMSAAValue", kMsaaValues[idx]);
            GdxSaveCvars();
            auto window = GdxWindow();
            if (window != nullptr) {
                window->SetMsaaLevel((uint32_t)kMsaaValues[idx]); // apply live (Fast3dWindow.cpp:319)
            }
        }
    }

    // Texture filtering — CVar gTextureFilter (enum FilteringMode), default FILTER_THREE_POINT.
    // Enum order is fixed by LUS: gfx_rendering_api.h -> { FILTER_THREE_POINT=0, FILTER_LINEAR=1,
    // FILTER_NONE=2 }. Combo index maps 1:1 to the enum value. Read once at Init; applied LIVE here
    // via Fast::Fast3dWindow::SetTextureFilter (Fast3dWindow.cpp:162). That setter is Fast3d-only
    // (it takes a Fast::FilteringMode), so it needs the downcast — null-safe: on a non-Fast3d
    // backend the CVar is still saved and takes effect on the next restart.
    {
        static const char* const kFilterLabels[] = {
            "Three-point (N64)", // FILTER_THREE_POINT = 0 (the 1:1 default)
            "Linear",            // FILTER_LINEAR      = 1
            "None (sharp)"       // FILTER_NONE        = 2
        };
        int idx = CVarGetInteger("gTextureFilter", 0 /* FILTER_THREE_POINT */);
        if (idx < 0 || idx > 2) {
            idx = 0;
        }
        if (ImGui::Combo("Texture filter", &idx, kFilterLabels, 3)) {
            CVarSetInteger("gTextureFilter", idx);
            GdxSaveCvars();
            auto fast = GdxFast3dWindow();
            if (fast != nullptr) {
                fast->SetTextureFilter(static_cast<Fast::FilteringMode>(idx)); // apply live
            }
        }
    }

    ImGui::Separator();

    // VSync — CVar gVsyncEnabled (bool), default 1 (on). Read live per-present, so a plain write
    // takes effect immediately.
    {
        bool on = CVarGetInteger("gVsyncEnabled", 1) != 0;
        if (ImGui::Checkbox("VSync", &on)) {
            CVarSetInteger("gVsyncEnabled", on ? 1 : 0);
            GdxSaveCvars();
        }
    }

    // Fullscreen is live window state, not a CVar. Ship::Window routes this through the active
    // backend (DXGI borderless on Windows, SDL fullscreen elsewhere) and persists the result via
    // Fast3dWindow's fullscreen-changed callback, exactly like the F11 shortcut.
    {
        auto window = GdxWindow();
        bool on = window != nullptr && window->IsFullscreen();
        ImGui::BeginDisabled(window == nullptr);
        if (ImGui::Checkbox("Fullscreen", &on) && window != nullptr) {
            window->SetFullscreen(on);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Uses the active window backend (borderless fullscreen on DX11).\n"
                              "The F11 shortcut controls the same setting.");
        }
    }

    // Z-fighting mode — CVar gZFightingMode (enum), default 0. Read live per-draw. The exact
    // non-zero semantics are backend-specific and unverified here, so labels stay neutral; the
    // default 0 is the 1:1 value.
    {
        static const char* const kZLabels[] = { "Disabled", "Mode 1", "Mode 2" };
        int idx = CVarGetInteger("gZFightingMode", 0);
        if (idx < 0 || idx > 2) {
            idx = 0;
        }
        if (ImGui::Combo("Z-fighting reduction", &idx, kZLabels, 3)) {
            CVarSetInteger("gZFightingMode", idx);
            GdxSaveCvars();
        }
    }

        return;
    }

    ImGui::SeparatorText("Visual Enhancements");

    // Widescreen (16:9) — CVar gEnhancements.Graphics.Widescreen, default 1 (= today's behavior).
    // Read live in interpreter.cpp AdjXForAspectRatio: 1 keeps the current 16:9 hor+ aspect
    // correction (byte-identical default), 0 renders 4:3 with pillarbox bars. OFF has two documented
    // edge cases (MSAA>1 at exactly 1x internal res; AdvancedResolution takes precedence).
    {
        bool on = CVarGetInteger("gEnhancements.Graphics.Widescreen", 1) != 0;
        if (ImGui::Checkbox("Widescreen (16:9)", &on)) {
            CVarSetInteger("gEnhancements.Graphics.Widescreen", on ? 1 : 0);
            GdxSaveCvars();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("On: fills the window in 16:9 (hor+).\n"
                              "Off: renders 4:3 with pillarbox bars on the sides.");
        }
    }

    {
        bool widescreenOn = CVarGetInteger("gEnhancements.Graphics.Widescreen", 1) != 0;
        bool on = CVarGetInteger("gEnhancements.Graphics.WidescreenUI", 0) != 0;
        ImGui::BeginDisabled(!widescreenOn);
        if (ImGui::Checkbox("True widescreen HUD/UI", &on)) {
            CVarSetInteger("gEnhancements.Graphics.WidescreenUI", on ? 1 : 0);
            GdxSaveCvars();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Anchors the single-player HUD to the true screen edges and extends\n"
                              "the SELECT MACHINE blue background and race transitions. Other\n"
                              "menu artwork stays proportional in 4:3. Requires Widescreen.");
        }
    }

    // Draw distance — CVar gEnhancements.Graphics.DrawDistance (%, default 100 = stock). Scales each
    // course's own far-render cutoff per-venue (course.c Course_Draw); 100% is bit-exact. Very high
    // values saturate against the game's fixed far clip plane with no further visible change.
    {
        int dd = CVarGetInteger("gEnhancements.Graphics.DrawDistance", 100);
        if (dd < 100) {
            dd = 100;
        }
        if (dd > 300) {
            dd = 300;
        }
        if (ImGui::SliderInt("Draw distance (%)", &dd, 100, 300)) {
            if (dd < 100) {
                dd = 100;
            }
            if (dd > 300) {
                dd = 300;
            }
            CVarSetInteger("gEnhancements.Graphics.DrawDistance", dd);
            GdxSaveCvars();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Extends how far each track's own geometry renders (100%% = stock,\n"
                              "scales per-venue). Very high values may hit the fixed far clip.");
        }
    }

    // Force max machine detail — CVar gEnhancements.Graphics.ForceMaxMachineLOD (default 0 = stock
    // distance-based LOD). When on, every machine draws at its highest-detail model (racer.c).
    {
        bool on = CVarGetInteger("gEnhancements.Graphics.ForceMaxMachineLOD", 0) != 0;
        if (ImGui::Checkbox("Force max machine detail", &on)) {
            CVarSetInteger("gEnhancements.Graphics.ForceMaxMachineLOD", on ? 1 : 0);
            GdxSaveCvars();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Always renders every machine at its highest-detail model,\n"
                              "ignoring distance. Off = stock distance-based detail.");
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Enhancements (parity-gated)");
    // Frame pacing — CVar gEnhancements.Graphics.FramePacing, default 0. libultraship's Fast3D
    // backend already caps the loop to ~60fps, so this is opt-in: when on, port/gdx_frame_pacer.c
    // holds the host loop to the true N64 NTSC field rate (~59.94Hz) with a wall-clock sleep+spin.
    // Recommend VSync OFF while on (a display-refresh present beats against the fixed schedule).
    {
        bool on = CVarGetInteger("gEnhancements.Graphics.FramePacing", 0) != 0;
        if (ImGui::Checkbox("Frame pacing (59.94 Hz)", &on)) {
            CVarSetInteger("gEnhancements.Graphics.FramePacing", on ? 1 : 0);
            GdxSaveCvars();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Experimental. The renderer already limits the game to ~60 fps; this\n"
                              "pins the loop to the true N64 rate (59.94 Hz). Turn VSync OFF when using it.");
        }
    }
    GdxComingSoon("Mirror mode");
    GdxComingSoon("FLX reflection quality");

}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 2) AUDIO — the P0 pilot. Engine LLE/HLE radio + reconstruction filter enable/cutoff (READY).
//    docs/AUDIO_SETTINGS_SCOPE.md. These write port-owned CVars; the live-read plumbing on the
//    audio thread (gdx_audio_lle.c / os.cpp via extern) is a separate slice — here we only own
//    the UI + CVar state.
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenu::DrawAudioMenu() {
    // Live output-path status. Diagnostic first-class citizen: a "no audio" report is
    // undebuggable remotely without knowing which backend the session picked and whether
    // samples are actually queued. Reports the ACTUAL active AudioPlayer backend (via
    // AudioPlayerBackendName) rather than SDL_GetCurrentAudioDriver(), which returns "none" for
    // the WASAPI/CoreAudio backends even when they are working — misleading on Windows. For the
    // SDL backend we additionally surface SDL_GetCurrentAudioDriver() (e.g. "pipewire"/"pulse"),
    // where a "dummy" driver means the launch environment lost the audio socket (sandboxed/naked
    // launcher env): the game synthesizes fine but the samples go nowhere.
    {
        const char* backend = AudioPlayerBackendName();
        const bool isSdl = std::strcmp(backend, "SDL") == 0;
        const char* sdlDriver = isSdl ? SDL_GetCurrentAudioDriver() : nullptr;
        const int32_t buffered = AudioPlayerBuffered();
        const int32_t desired = AudioPlayerGetDesiredBuffered();

        ImGui::TextDisabled("Output status");
        if (isSdl) {
            ImGui::Text("Active backend: SDL (%s)", sdlDriver != nullptr ? sdlDriver : "no driver");
        } else {
            ImGui::Text("Active backend: %s", backend);
        }
        ImGui::Text("Queued samples: %d / %d desired", buffered, desired);

        // The red warning is meaningful ONLY when SDL is the active backend and its underlying
        // driver is missing or "dummy". WASAPI/CoreAudio legitimately report no SDL driver, so
        // suppress the warning for them (it would be a false alarm).
        if (isSdl && sdlDriver == nullptr) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                               "No SDL audio device is open. Audio is synthesized but discarded.");
        } else if (isSdl && std::strcmp(sdlDriver, "dummy") == 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                               "SDL fell back to the dummy driver: this launch environment has no\n"
                               "audio socket. Launch from a terminal or fix the launcher's env.");
        }
        ImGui::Separator();
    }

    // Output backend selection — CVar gEnhancements.Audio.Backend (0=Auto, 1=WASAPI, 2=SDL).
    // Applied at startup in main.cpp's InitAudio; Auto keeps libultraship's per-platform default
    // (WASAPI on Windows, SDL on Linux). Only backends that exist on this platform are offered:
    // WASAPI is Windows-only, and on Linux SDL routes to PipeWire/PulseAudio/ALSA.
    ImGui::TextDisabled("Backend");
    {
        int sel = CVarGetInteger("gEnhancements.Audio.Backend", 0);
#ifdef _WIN32
        const char* const items[] = { "Auto", "WASAPI", "SDL" };
        int uiIndex = (sel >= 0 && sel <= 2) ? sel : 0;
        if (ImGui::Combo("Output backend", &uiIndex, items, 3)) {
            CVarSetInteger("gEnhancements.Audio.Backend", uiIndex);
            GdxSaveCvars();
        }
#else
        const char* const items[] = { "Auto", "SDL" };
        int uiIndex = (sel == 2) ? 1 : 0; // map stored CVar (2 = SDL) into the reduced list
        if (ImGui::Combo("Output backend", &uiIndex, items, 2)) {
            CVarSetInteger("gEnhancements.Audio.Backend", uiIndex == 1 ? 2 : 0);
            GdxSaveCvars();
        }
#endif
        ImGui::TextDisabled("Applies on restart.");
        ImGui::Separator();
    }

    // Engine — CVar gEnhancements.Audio.LLE, default 1. LLE = accurate (cxd4 RSP), HLE = fast.
    ImGui::TextDisabled("Engine");
    {
        int lle = CVarGetInteger("gEnhancements.Audio.LLE", 1);
        if (ImGui::RadioButton("LLE (accurate)", lle == 1)) {
            CVarSetInteger("gEnhancements.Audio.LLE", 1);
            GdxSaveCvars();
        }
        if (ImGui::RadioButton("HLE (fast)", lle == 0)) {
            CVarSetInteger("gEnhancements.Audio.LLE", 0);
            GdxSaveCvars();
        }
    }

    ImGui::Separator();

    // Output reconstruction filter — CVar gEnhancements.Audio.LowPassHz, default 15000. A value of
    // 0 disables the filter; any value 500..16000 is the low-pass cutoff. The enable checkbox
    // toggles between 0 (off) and the remembered/last cutoff (on).
    ImGui::TextDisabled("Output reconstruction filter");
    {
        int hz = CVarGetInteger("gEnhancements.Audio.LowPassHz", 15000);
        bool filterOn = hz > 0;

        if (ImGui::Checkbox("Enable filter", &filterOn)) {
            if (filterOn) {
                int restore = mLastLowPassHz > 0 ? mLastLowPassHz : 15000;
                CVarSetInteger("gEnhancements.Audio.LowPassHz", restore);
            } else {
                if (hz > 0) {
                    mLastLowPassHz = hz; // remember so re-enabling restores the same cutoff
                }
                CVarSetInteger("gEnhancements.Audio.LowPassHz", 0);
            }
            GdxSaveCvars();
        }

        // Re-read after the checkbox so the slider reflects the change within the same frame.
        int hzNow = CVarGetInteger("gEnhancements.Audio.LowPassHz", 15000);
        int cutoff = hzNow > 0 ? hzNow : (mLastLowPassHz > 0 ? mLastLowPassHz : 15000);

        ImGui::BeginDisabled(!filterOn);
        if (ImGui::SliderInt("Cutoff (Hz)", &cutoff, 500, 16000)) {
            if (cutoff < 500) {
                cutoff = 500;
            }
            CVarSetInteger("gEnhancements.Audio.LowPassHz", cutoff);
            mLastLowPassHz = cutoff;
            GdxSaveCvars();
        }
        ImGui::EndDisabled();
    }

    ImGui::Separator();

    // Master volume — CVar gEnhancements.Audio.MasterVolume (0..100 %, default 100). Applied as a
    // final-stage gain multiply on the s16 output copy in os.cpp's osAiSetNextBuffer, read live
    // there each buffer (same live-CVar pattern as the low-pass). 100 = no-op (the multiply is
    // skipped entirely), so the default is bit-exact.
    ImGui::TextDisabled("Output");
    {
        int vol = CVarGetInteger("gEnhancements.Audio.MasterVolume", 100);
        if (vol < 0) {
            vol = 0;
        }
        if (vol > 100) {
            vol = 100;
        }
        if (ImGui::SliderInt("Master volume (%)", &vol, 0, 100)) {
            if (vol < 0) {
                vol = 0;
            }
            if (vol > 100) {
                vol = 100;
            }
            CVarSetInteger("gEnhancements.Audio.MasterVolume", vol);
            GdxSaveCvars();
        }
    }

    // Reverb — CVar gEnhancements.Audio.Reverb (default 1 = on). Wired to the HLE reverb kill switch
    // in n64_audio_hle.c (the A_MIXER wet->dry return), read live there. NOTE: this affects the HLE
    // audio engine ONLY; under the default LLE engine reverb is produced by the audio microcode
    // itself, so toggling this has no audible effect while LLE is selected. Still wired correctly
    // for the HLE fallback path.
    {
        bool reverbOn = CVarGetInteger("gEnhancements.Audio.Reverb", 1) != 0;
        if (ImGui::Checkbox("Reverb", &reverbOn)) {
            CVarSetInteger("gEnhancements.Audio.Reverb", reverbOn ? 1 : 0);
            GdxSaveCvars();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Affects the HLE audio engine only.\n"
                              "Under the default LLE engine, reverb is the microcode's own.");
        }
    }

    ImGui::Separator();

    // Latency / buffer size — CVar gEnhancements.Audio.BufferFrames (frames, default 4096, range
    // 1024..8192). Read ONCE at InitAudio (main.cpp), so a change applies only on the next restart
    // (hence the note). A larger reservoir rides out host scheduling jitter better but adds output
    // latency; a smaller one is snappier but more underrun-prone.
    ImGui::TextDisabled("Latency");
    {
        int frames = CVarGetInteger("gEnhancements.Audio.BufferFrames", 4096);
        if (frames < 1024) {
            frames = 1024;
        }
        if (frames > 8192) {
            frames = 8192;
        }
        if (ImGui::SliderInt("Buffer size (frames)", &frames, 1024, 8192)) {
            if (frames < 1024) {
                frames = 1024;
            }
            if (frames > 8192) {
                frames = 8192;
            }
            CVarSetInteger("gEnhancements.Audio.BufferFrames", frames);
            GdxSaveCvars();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(applies on restart)");
    }

    ImGui::Separator();
    ImGui::TextDisabled("More (planned)");
    GdxComingSoon("Sound test / jukebox");

}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 3) GAMEPLAY — docs/menu/GAMEPLAY_TAB.md §4. Autosave-on-record and the other shipped controls are
//    live; the owner removed the custom fast-restart shortcut in favor of vanilla retry behavior.
//    Every control
//    writes a port-owned gEnhancements.Gameplay.* CVar at a 1:1 default (feature off / stock
//    behavior). The actual behavior lives in the game tick, not here:
//      - Autosave-on-record-> decomp/src/overlays/ovl_i3/menus.c (Gdx_AutosaveGhostOnRecord) auto-
//                             persists the best ghost per course at race finish; this tab owns the toggle.
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenu::DrawGameplayMenu() {
    // Save-states (in-session RAM rewind) — CVar gEnhancements.Gameplay.SaveStates, default 0 (strict
    // no-op). EXPERIMENTAL. When on, Quick Save/Load snapshot & restore the race (RDRAM + curated
    // racer/camera/RNG/game-mode globals) to a single RAM slot at the frame-loop yield boundary under
    // the audio lock. SAME-RACE / SAME-COURSE only (cross-course load is unguarded and may glitch);
    // audio does not rewind. Buttons call gdx_savestate_save/load (armed, fulfilled at the boundary).
    {
        bool on = CVarGetInteger("gEnhancements.Gameplay.SaveStates", 0) != 0;
        if (ImGui::Checkbox("Save-states (experimental)", &on)) {
            CVarSetInteger("gEnhancements.Gameplay.SaveStates", on ? 1 : 0);
            GdxSaveCvars();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Experimental in-race rewind to a single RAM slot.\n"
                              "Save, then Load during the SAME race on the SAME course.\n"
                              "Audio does not rewind; loading after a course change may glitch. Off by default.");
        }

        ImGui::BeginDisabled(!on);
        if (ImGui::Button("Quick Save (RAM)")) {
            gdx_savestate_save();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(gdx_savestate_exists() == 0);
        if (ImGui::Button("Quick Load (RAM)")) {
            gdx_savestate_load();
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled(gdx_savestate_exists() != 0 ? "(slot held)" : "(no slot yet)");
    }

    // Skippable transitions — CVar gEnhancements.Gameplay.SkippableTransitions, default 0 (stock:
    // transitions play fully). When on, transition.c Transition_Update re-runs its same per-tick
    // logic in one call until finished (up to 128x), so screen wipes resolve near-instantly. The
    // stock per-tick switch is byte-unchanged; only the surrounding loop budget differs. [PB].
    {
        bool on = CVarGetInteger("gEnhancements.Gameplay.SkippableTransitions", 0) != 0;
        if (ImGui::Checkbox("Skip/shorten transitions", &on)) {
            CVarSetInteger("gEnhancements.Gameplay.SkippableTransitions", on ? 1 : 0);
            GdxSaveCvars();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Fast-completes screen-transition wipes instead of playing them in full.\n"
                              "Off by default (parity).");
        }
    }

    // Reduce Course Edit flashing — CVar gEnhancements.Gameplay.ReduceEditorFlashing, default 0
    // (stock N64 strobe). When on, the Course Edit node blink/checker parity and the flagged-node
    // size pulse advance at half rate (course_edit/191080.c func_xk2_800E04E0, #ifdef PORT). Off is
    // bit-identical to stock.
    {
        bool on = CVarGetInteger("gEnhancements.Gameplay.ReduceEditorFlashing", 0) != 0;
        if (ImGui::Checkbox("Reduce Course Edit flashing", &on)) {
            CVarSetInteger("gEnhancements.Gameplay.ReduceEditorFlashing", on ? 1 : 0);
            GdxSaveCvars();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Halves the Course Edit blink/checker cadence. The 20Hz strobe is\n"
                              "authentic N64 behavior; this calms it on modern displays.");
        }
    }

    ImGui::Separator();

    // Autosave-on-record — CVar gEnhancements.Gameplay.AutosaveOnRecord, default 0.
    // SCOPE (important): stock F-Zero X ALREADY commits numeric records (best times / best lap /
    // max speed / death-race stats) to SRAM immediately on finishing a race (menus.c:252-268), and
    // the port's SRAM is write-through to fzerox.sav (sram_buffer.cpp) — those autosave regardless
    // of this toggle. What this toggle adds is auto-persisting the best GHOST replay, which stock
    // F-Zero X saves only via the manual "Save Ghost" prompt (menus.c:2085-2101 / 2562-2581) — so
    // quitting before that prompt loses the ghost. When on, the port writes the ghost via the
    // per-course PC ghost library on a new best. The cartridge-compatible SRAM slot remains a
    // mirror when it is empty or already holds the same course. Off by default so a fresh config
    // keeps ghosts manual-save.
    {
        bool on = CVarGetInteger("gEnhancements.Gameplay.AutosaveOnRecord", 0) != 0;
        if (ImGui::Checkbox("Autosave ghost on new record", &on)) {
            CVarSetInteger("gEnhancements.Gameplay.AutosaveOnRecord", on ? 1 : 0);
            GdxSaveCvars();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Auto-save your best Time Attack ghost replay when you beat it,\n"
                              "without the manual Save-Ghost prompt.\n"
                              "(Record TIMES already autosave in stock F-Zero X.) Off by default.");
        }
    }

    ImGui::Separator();

    // Adaptive final-lap audio — CVar gEnhancements.Audio.FinalLapAdaptive, default 0. When on, a
    // subtle +15% BGM volume lift triggers on the final lap via the game's own NA_SE_18 final-lap
    // cue (audio/disk/external.c). Music-only (seqPlayer 1); auto-resets on the next BGM load.
    {
        bool on = CVarGetInteger("gEnhancements.Audio.FinalLapAdaptive", 0) != 0;
        if (ImGui::Checkbox("Adaptive final-lap audio", &on)) {
            CVarSetInteger("gEnhancements.Audio.FinalLapAdaptive", on ? 1 : 0);
            GdxSaveCvars();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("A subtle music lift on the final lap (opt-in). Off by default.");
        }
    }

}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 4) PRACTICE / TOOLS — all future (docs/menu/PRACTICE_TAB.md).
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenu::DrawPracticeMenu() {
    // Lap-split deltas — CVar gEnhancements.Practice.ShowLapDeltas, default 0 (stock: nothing drawn).
    // When on, Practice mode draws how the last completed lap compares to the session best (or a
    // loaded ghost's same lap, once ghosts populate outside Time Attack). Drawn in hud.c under
    // #ifdef PORT; default 0 draws nothing. Owner-visual: on-screen position/colors to confirm.
    {
        bool on = CVarGetInteger("gEnhancements.Practice.ShowLapDeltas", 0) != 0;
        if (ImGui::Checkbox("Show lap deltas", &on)) {
            CVarSetInteger("gEnhancements.Practice.ShowLapDeltas", on ? 1 : 0);
            GdxSaveCvars();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("In Practice mode, shows your last lap vs your session best\n"
                              "(green = faster, red = slower). Off by default.");
        }
    }

    ImGui::Separator();

    // Ghost import / export (.gdg) — calls the port's gdx_ghost_io C API (port/gdx_ghost_io.c).
    // Export is read-only. Import adds a validated player replay to the per-course PC library and
    // mirrors it into SRAM only when that does not evict another course. It stays disabled while an
    // on-track race is live to avoid mutating ghost state alongside the game fiber. Both use the
    // default path next to the exe (ghost_export.gdg); a proper file picker remains future work.
    ImGui::TextDisabled("Ghost replay (.gdg)");
    {
        static char sGhostStatus[192] = { 0 };
        char path[1024];
        bool haveDefault = gdx_ghost_default_path(path, sizeof(path)) != 0;

        if (ImGui::Button("Export saved ghost")) {
            if (!haveDefault) {
                snprintf(sGhostStatus, sizeof(sGhostStatus), "Export failed: could not resolve output path.");
            } else {
                int rc = gdx_ghost_export(GDX_GHOST_ANY_COURSE, path);
                if (rc == GDX_GHOST_OK) {
                    snprintf(sGhostStatus, sizeof(sGhostStatus), "Exported to %s", path);
                } else if (rc == GDX_GHOST_ERR_NO_GHOST) {
                    snprintf(sGhostStatus, sizeof(sGhostStatus), "Export: no ghost is saved yet.");
                } else {
                    snprintf(sGhostStatus, sizeof(sGhostStatus), "Export failed (code %d).", rc);
                }
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Writes the currently-saved ghost replay to:\n%s",
                              haveDefault ? path : "(unavailable)");
        }

        ImGui::SameLine();

        bool inGame = gdx_input_in_gameplay() != 0;
        ImGui::BeginDisabled(inGame);
        if (ImGui::Button("Import ghost")) {
            if (!haveDefault) {
                snprintf(sGhostStatus, sizeof(sGhostStatus), "Import failed: could not resolve input path.");
            } else {
                int rc = gdx_ghost_import(path);
                if (rc == GDX_GHOST_OK) {
                    snprintf(sGhostStatus, sizeof(sGhostStatus), "Imported into the player ghost library: %s", path);
                } else if (rc == GDX_GHOST_ERR_COURSE_MISMATCH) {
                    snprintf(sGhostStatus, sizeof(sGhostStatus),
                             "Import refused: the save slot holds a different course's ghost.");
                } else if (rc == GDX_GHOST_ERR_BAD_MAGIC || rc == GDX_GHOST_ERR_BAD_VERSION) {
                    snprintf(sGhostStatus, sizeof(sGhostStatus), "Import failed: not a valid .gdg file (code %d).", rc);
                } else {
                    snprintf(sGhostStatus, sizeof(sGhostStatus), "Import failed (code %d).", rc);
                }
            }
        }
        ImGui::EndDisabled();
        if (inGame) {
            ImGui::SameLine();
            ImGui::TextDisabled("(disabled in-race)");
        }

        if (sGhostStatus[0] != '\0') {
            ImGui::TextWrapped("%s", sGhostStatus);
        }
    }

    ImGui::Separator();

    // Ghost Browser window toggle (GdxGhostWindow, registered via AddGuiWindow in main.cpp). A
    // browser of the per-course player-ghost library with an Export-to-.gdg button. Same live
    // show/hide idiom as the Developer-tab windows (GdxWindowVisible reflects state, click flips it).
    if (ImGui::Button(GdxWindowVisible("Ghost Browser") ? "Return Ghost Browser to menu"
                                                         : "Open Ghost Browser window")) {
        GdxToggleWindow("Ghost Browser");
    }

    // Photo mode (free camera) is available in every race mode. When enabled, pausing suppresses
    // all race HUD/pause overlays and reserves their controls for the free camera. Disabling the
    // toggle restores the normal paused UI; unpausing restores the game camera exactly.
    {
        bool on = CVarGetInteger("gEnhancements.Practice.PhotoMode", 0) != 0;
        if (ImGui::Checkbox("Photo mode (free camera)", &on)) {
            CVarSetInteger("gEnhancements.Practice.PhotoMode", on ? 1 : 0);
            GdxSaveCvars();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Pause during a race to hide the HUD and free-fly the camera.\n"
                              "Stick: dolly/truck  -  C-buttons: look  -  L/R: FOV  -  hold Z: raise/lower.\n"
                              "Unpausing or turning this off restores the game camera exactly. Off by default.");
        }
    }

    ImGui::Separator();

    GdxComingSoon("Replay theater");
    GdxComingSoon("Diagnostic overlay");
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 5) CONTROLS / INPUT — surface the LUS Input Editor window (READY). docs/menu/CONTROLS_TAB.md.
//    The InputEditorWindow is registered in main.cpp at boot under the name "Input Editor"; here
//    we just toggle its live visibility. Keyboard remap is a separate port-side workstream.
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenu::DrawControlsMenu() {
    DrawToolWindowPage("Input Editor",
                       "Configure controllers, keyboard, mouse, deadzones, sensitivity, and per-port mappings.");
}

void GdxMenu::DrawInputViewerMenu() {
    ImGui::SeparatorText("Input Viewer");
    bool visible = GdxWindowVisible("Input Viewer");
    if (ImGui::Checkbox("Show input viewer overlay", &visible)) {
        GdxToggleWindow("Input Viewer");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Shows the exact mapped N64 input state delivered to F-Zero X.");
    }

    float scale = std::clamp(CVarGetFloat("gInputViewer.Scale", 1.0f), 0.5f, 2.5f);
    if (ImGui::SliderFloat("Overlay scale", &scale, 0.5f, 2.5f, "%.2fx", ImGuiSliderFlags_AlwaysClamp)) {
        CVarSetFloat("gInputViewer.Scale", scale);
        GdxSaveCvars();
    }
    float opacity = std::clamp(CVarGetFloat("gInputViewer.Opacity", 1.0f), 0.2f, 1.0f);
    if (ImGui::SliderFloat("Overlay opacity", &opacity, 0.2f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
        CVarSetFloat("gInputViewer.Opacity", opacity);
        GdxSaveCvars();
    }
    bool dragging = CVarGetInteger("gInputViewer.EnableDragging", 1) != 0;
    if (ImGui::Checkbox("Enable dragging", &dragging)) {
        CVarSetInteger("gInputViewer.EnableDragging", dragging ? 1 : 0);
        GdxSaveCvars();
    }
    bool background = CVarGetInteger("gInputViewer.ShowBackground", 1) != 0;
    if (ImGui::Checkbox("Show background layer", &background)) {
        CVarSetInteger("gInputViewer.ShowBackground", background ? 1 : 0);
        GdxSaveCvars();
    }
    bool dpad = CVarGetInteger("gInputViewer.ShowDpad", 0) != 0;
    if (ImGui::Checkbox("Show D-pad layers", &dpad)) {
        CVarSetInteger("gInputViewer.ShowDpad", dpad ? 1 : 0);
        GdxSaveCvars();
    }
    int outlineMode = std::clamp(CVarGetInteger("gInputViewer.ButtonOutlineMode", 1), 0, 3);
    const char* outlineLabels[] = { "Always shown", "Shown while released", "Shown while pressed", "Hidden" };
    if (ImGui::Combo("Button outlines", &outlineMode, outlineLabels, IM_ARRAYSIZE(outlineLabels))) {
        CVarSetInteger("gInputViewer.ButtonOutlineMode", outlineMode);
        GdxSaveCvars();
    }
    bool analogValues = CVarGetInteger("gInputViewer.ShowAnalogValues", 0) != 0;
    if (ImGui::Checkbox("Show analog values", &analogValues)) {
        CVarSetInteger("gInputViewer.ShowAnalogValues", analogValues ? 1 : 0);
        GdxSaveCvars();
    }
    ImGui::TextWrapped("The viewer reads G-Diffuser's final mapped N64 state, after controller bindings and analog "
                       "curves. Inputs intentionally read neutral while this menu owns game input.");
}

void GdxMenu::DrawGhostsMenu() {
    DrawToolWindowPage("Ghost Browser",
                       "Manage multiple local and imported player ghosts per exact course and select up to three "
                       "Time Attack opponents. Staff ghosts remain controlled by the base game.");
}

void GdxMenu::DrawToolWindowPage(const char* name, const char* description) {
    auto gui = GdxGui();
    auto window = gui != nullptr ? gui->GetGuiWindow(name) : nullptr;
    if (window == nullptr) {
        ImGui::TextDisabled("%s is unavailable.", name);
        return;
    }

    ImGui::TextWrapped("%s", description);
    const bool poppedOut = window->IsVisible();
    std::string buttonLabel = poppedOut ? std::string("Return to menu##") + name
                                        : std::string("Pop out ") + name + "##" + name;
    if (ImGui::Button(buttonLabel.c_str())) {
        window->ToggleVisibility();
    }
    ImGui::Separator();
    if (window->IsVisible()) {
        ImGui::TextDisabled("%s is open in a separate window.", name);
    } else {
        window->DrawElement();
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 6) WORKSHOP — all future / parity-blocked (docs/menu/WORKSHOP_TAB.md).
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenu::DrawWorkshopMenu() {
    static char sReloadStatus[160] = "";
    const ImVec4 kRed = ImVec4(0.90f, 0.25f, 0.25f, 1.0f);

    // ── Texture Packs ────────────────────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Texture Packs");
    {
        bool on = CVarGetInteger("gEnhancements.Workshop.TexturePacks", 0) != 0;
        if (ImGui::Checkbox("Enable texture packs", &on)) {
            CVarSetInteger("gEnhancements.Workshop.TexturePacks", on ? 1 : 0);
            GdxSaveCvars();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Overrides game textures from mods/*.o2r packs.\nOff = stock rendering.");
        }
    }

    std::vector<GdxWorkshopPackInfo> packs = GdxWorkshopListPacks();
    ImGui::TextDisabled("%d override(s) available across mounted packs.", GdxWorkshopOverrideCount());

    if (packs.empty()) {
        ImGui::TextDisabled("No packs found. Drop .o2r packs into the mods/ folder.");
    } else if (ImGui::BeginTable("##WorkshopPacks", 3,
                                 ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 32.0f);
        ImGui::TableSetupColumn("Pack", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& p : packs) {
            ImGui::TableNextRow();
            ImGui::PushID(p.basename.c_str());

            ImGui::TableSetColumnIndex(0);
            bool enabled = !p.disabled;
            if (ImGui::Checkbox("##en", &enabled)) {
                // Toggling the checkbox rewrites the persisted disable list; the change takes effect
                // on the next Reload (or the next boot) since the archive set is mounted once.
                GdxWorkshopSetPackDisabled(p.basename.c_str(), enabled ? 0 : 1);
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(p.basename.c_str());

            ImGui::TableSetColumnIndex(2);
            if (p.manifestPresent) {
                ImGui::Text("v%s by %s", p.version.empty() ? "?" : p.version.c_str(),
                            p.author.empty() ? "?" : p.author.c_str());
                if (p.gameVersionMismatch) {
                    ImGui::TextColored(kRed, "game_version mismatch (%s)", p.gameVersion.c_str());
                }
                if (p.keySchemeMismatch) {
                    ImGui::TextColored(kRed, "key_scheme_version mismatch (%s)", p.keySchemeVersion.c_str());
                }
            } else {
                ImGui::TextDisabled("(no manifest.json)");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
        ImGui::TextDisabled("Rename with a numeric prefix (e.g. 10-, 20-) to order pack priority; "
                            "later packs win per-file.");
    }

    if (ImGui::Button("Reload packs")) {
        GdxWorkshopReload(sReloadStatus, sizeof(sReloadStatus));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Re-scans mods/, re-mounts packs, and clears the texture cache so edits\n"
                          "appear without restarting.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Open mods folder")) {
        GdxOpenFolder(GdxWorkshopModsDir(true));
    }
    if (sReloadStatus[0] != '\0') {
        ImGui::TextDisabled("%s", sReloadStatus);
    }

    // ── Texture Dump ─────────────────────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Texture Dump");
    {
        bool on = CVarGetInteger("gEnhancements.Workshop.TextureDump", 0) != 0;
        if (ImGui::Checkbox("Dump textures while playing", &on)) {
            CVarSetInteger("gEnhancements.Workshop.TextureDump", on ? 1 : 0);
            GdxSaveCvars();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Writes every decoded texture to dump/<key>.png (first-seen-wins),\n"
                              "with dump/manifest.tsv recording key, size, and format.");
        }
    }
    ImGui::TextDisabled("Dumped %d texture(s) this session -> dump/", gdx_workshop_dump_count());
    if (ImGui::Button("Open dump folder")) {
        GdxOpenFolder(GdxWorkshopDumpDir(true));
    }

    // ── DD Save (64DD durable-save sidecar status) ────────────────────────────────────────────────
    ImGui::SeparatorText("DD Save (64DD sidecar)");
    ImGui::Text("Sidecar: %s", gdx_disk_sidecar_present() ? "present" : "none yet");
    ImGui::Text("Journal records: %d", gdx_disk_sidecar_record_count());
    ImGui::Text("Last flush: %s", gdx_disk_last_flush_ok() ? "ok" : "FAILED");
    if (gdx_disk_format_refused_this_boot()) {
        ImGui::TextColored(kRed, "The disk's MFS save area is uninitialized.");
        if (ImGui::Button("Initialize DD save area")) {
            ImGui::OpenPopup("##ddformat");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Authorizes a one-time format of the 64DD MFS save area on the NEXT boot.");
        }
        if (ImGui::BeginPopupModal("##ddformat", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(
                "Initialize the 64DD MFS save area?\n\n"
                "This authorizes a one-time format the NEXT time the game boots. The format is\n"
                "written to the durable save sidecar only -- your original .ndd disk file is never\n"
                "modified. This is needed before Course Edit / Machine Create can save to disk.");
            ImGui::Separator();
            if (ImGui::Button("Authorize (next boot)")) {
                CVarSetInteger("gEnhancements.Workshop.AllowDDFormatOnce", 1);
                GdxSaveCvars();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // ── Content Installs (W1/W2 — parity/infra gated) ─────────────────────────────────────────────
    ImGui::SeparatorText("Content Installs");
    GdxComingSoon("Track / cup / machine install (blocked: disk write-through, W1)");
    GdxComingSoon("Installed-content library + quota manager (W2)");
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 7) ONLINE / GHOSTS — all future / parity-blocked; netplay additionally decision-gated
//    (docs/menu/ONLINE_TAB.md).
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenu::DrawOnlineMenu() {
    GdxComingSoon("Leaderboards (per course)");
    GdxComingSoon("Ghost upload / download");
    GdxComingSoon("Netplay lobbies (after decision gate)");
    GdxComingSoon("Spectator / director cam");
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 8) DEVELOPER — surface existing LUS dev windows (READY). docs/menu/DEVELOPER_TAB.md.
//    Console + Stats are auto-registered by the LUS Gui ctor; Gfx Debugger is registered in
//    main.cpp at boot. Each menu item toggles the LIVE window via GetGuiWindow(name)->
//    ToggleVisibility() (a bare CVarSet would not move an already-constructed window).
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenu::DrawDeveloperMenu() {
    ImGui::TextWrapped("Developer tools can be embedded in this menu or popped out into independent windows.");
    if (ImGui::Button("Open Stats")) GdxToggleWindow("Stats");
    ImGui::SameLine();
    if (ImGui::Button("Open Console")) GdxToggleWindow("Console");
    ImGui::SameLine();
    if (ImGui::Button("Open Gfx Debugger")) GdxToggleWindow("Gfx Debugger");
    ImGui::SeparatorText("Windowing");

    // Multi-viewport — CVar gEnableMultiViewports, default 1. The ImGui viewport flag is applied
    // ONCE at Gui::Init(), so flipping the CVar at runtime persists the preference but only takes
    // effect after a restart (we deliberately do not poke ImGui::GetIO() here). Hence the note.
    {
        bool mv = CVarGetInteger("gEnableMultiViewports", 1) != 0;
        if (ImGui::Checkbox("Multi-viewport docking", &mv)) {
            CVarSetInteger("gEnableMultiViewports", mv ? 1 : 0);
            GdxSaveCvars();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(restart)");
    }

}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 9) ABOUT — static text only (docs/menu/ABOUT_TAB.md). No version string exists in the port
//    today, so we show a fixed pre-alpha label, the EK-required boot notice, and credits.
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenu::DrawAboutMenu() {
    ImGui::Text("G-Diffuser (pre-alpha)");
    ImGui::TextDisabled("A native PC source port of F-Zero X (N64) + Expansion Kit (64DD)");

    ImGui::Separator();

    // EK-required boot policy (VISION_X_EVOLVED.md F2). The Expansion Kit is REQUIRED; the
    // supported config is cart ROM + EK disk image. Informational here (by the time the menu is
    // reachable the ROM has loaded), restating the supported install.
    ImGui::TextWrapped("Requires the F-Zero X Expansion Kit disk image (.ndd). Supported "
                       "configuration: cart ROM + EK disk image. Obtaining the images is the "
                       "user's responsibility.");

    ImGui::Separator();

    ImGui::TextDisabled("Credits / licenses");
    ImGui::BulletText("F-Zero X decompilation (inspectredc/fzerox) - CC0 1.0");
    ImGui::BulletText("cxd4 RSP interpreter (Iconoclast) - CC0");
    ImGui::BulletText("libultraship (fork of Kenix3/libultraship) - MIT");
    ImGui::BulletText("Torch asset tool (HarbourMasters) - MIT");
    ImGui::BulletText("StormLib (Ladislav Zezula) - MIT");
    ImGui::BulletText("Dear ImGui (Omar Cornut) - MIT");
    ImGui::BulletText("SDL2 (Sam Lantinga) - zlib");
    ImGui::BulletText("Montserrat and Inconsolata fonts - SIL Open Font License 1.1");

    ImGui::Separator();
    ImGui::TextDisabled("https://github.com/Zorkats/G-Diffuser");

}
