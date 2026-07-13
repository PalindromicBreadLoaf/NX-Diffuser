// port/gdx_menu.cpp — implementation of the G-Diffuser enhancement menu bar.
//
// See gdx_menu.h for the high-level design. This file is pure port-side wiring against LUS's
// public ImGui + CVar API; it edits no submodule and changes no existing behavior (purely
// additive). All controls read/write CVars; every default reproduces today's behavior.
//
// CVar NAMES ARE STRING LITERALS ON PURPOSE
// -----------------------------------------
// libultraship defines CVAR_* macros (e.g. CVAR_MENU_BAR_OPEN) in cmake/cvars.cmake, but that
// file is include()d only inside libultraship/src (libultraship/src/CMakeLists.txt:1), so its
// add_compile_definitions() do NOT reach the port/ target. We therefore spell the CVar names as
// literals here; each matches cvars.cmake exactly (cross-checked against
// libultraship/cmake/cvars.cmake). The port's own knobs use the gEnhancements.* convention.

#include "gdx_menu.h"

#include <imgui.h> // vendored in libultraship's imgui; already on the port target's include path
                   // (main.cpp already pulls it transitively via GuiWindow.h). Mirrors the
                   // <imgui.h> include used across LUS (e.g. GuiWindow.h:4).

#include "ship/Context.h"           // Ship::Context::GetInstance()
#include "ship/window/Window.h"     // Ship::Window::GetGui() + the SetResolutionMultiplier/
                                    // SetMsaaLevel virtuals used to apply the graphics knobs live
#include "ship/window/gui/Gui.h"    // Ship::Gui::{GetGuiWindow, SaveConsoleVariablesNextFrame}
#include "fast/Fast3dWindow.h"      // Fast::Fast3dWindow::SetTextureFilter + Fast::FilteringMode
                                    // (the texture-filter setter is Fast3d-only, not on the base
                                    // Ship::Window, so it needs a downcast — see DrawGraphicsMenu)

#include "libultraship/bridge/consolevariablebridge.h" // CVarGet/Set/Register*

#include <memory> // std::dynamic_pointer_cast (null-safe downcast to Fast::Fast3dWindow)
#include <cstdio> // snprintf (Practice-tab ghost import/export status line)

#include "gdx_ghost_io.h" // .gdg ghost import/export C API (Practice tab Export / Import buttons)

// From port/input_bridge.c: nonzero while an on-track race is live. The ghost Import writes to the
// SRAM ghost slot, which must not race the game fiber, so the Import button is disabled in-race.
extern "C" int gdx_input_in_gameplay(void);

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

} // namespace

// ─────────────────────────────────────────────────────────────────────────────────────────────
// Construction — pin visibility CVar + register the port's gEnhancements.* CVars at 1:1 defaults.
// ─────────────────────────────────────────────────────────────────────────────────────────────

// Base ctor: (visibilityConsoleVariable, isVisible). "gOpenMenuBar" is the CVar the LUS F1 / Esc /
// Gamepad-Back toggle flips (Gui.cpp), so binding to it makes those keys open THIS bar. Start
// hidden (isVisible=false) — the menu is opt-in via F1.
GdxMenuBar::GdxMenuBar() : Ship::GuiMenuBar("gOpenMenuBar", false) {
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
    //   gEnhancements.Graphics.DrawDistance       = 100 -> per-venue far-render scale in %. 100 = stock
    //                                                      (1.0x, bit-exact). course.c Course_Draw,
    //                                                      clamped 100..300.
    //   gEnhancements.Graphics.ForceMaxMachineLOD = 0   -> 0 = stock distance-based machine LOD; 1 pins
    //                                                      the highest-detail model. racer.c Racer_Draw.
    CVarRegisterInteger("gEnhancements.Graphics.Widescreen", 1);
    CVarRegisterInteger("gEnhancements.Graphics.DrawDistance", 100);
    CVarRegisterInteger("gEnhancements.Graphics.ForceMaxMachineLOD", 0);
    //   gEnhancements.Graphics.FramePacing = 0 -> off = stock. libultraship's Fast3D backend already
    //                                             limits the loop to ~60fps; when on, port/gdx_frame_
    //                                             pacer.c pins it to the N64 NTSC rate (~59.94Hz).
    //                                             Recommend VSync OFF when enabled (avoids beating).
    CVarRegisterInteger("gEnhancements.Graphics.FramePacing", 0);

    // GAMEPLAY tab CVars. Every default reproduces stock behavior (the optionality constitution):
    //   gEnhancements.Gameplay.FastRestart       = 0    -> off. Changes in-race input (holding a
    //                                                      combo retries the race), so it is opt-in.
    //                                                      Consumed in port/input_bridge.c.
    //   gEnhancements.Gameplay.FastRestartHoldMs = 1000 -> hold dwell (ms) before a retry fires;
    //                                                      guards against accidental mid-race resets.
    //   gEnhancements.Gameplay.AutosaveOnRecord  = 0    -> off. Stock F-Zero X already commits the
    //                                                      NUMERIC records (times/best-lap/max-speed/
    //                                                      death-race) to SRAM immediately on finish
    //                                                      (menus.c:252-268); this toggle only adds
    //                                                      auto-persisting the best GHOST replay,
    //                                                      which stock saves solely via the manual
    //                                                      Save-Ghost prompt. 0 keeps ghosts manual =
    //                                                      stock behavior. Consumed in menus.c
    //                                                      (Gdx_AutosaveGhostOnRecord).
    CVarRegisterInteger("gEnhancements.Gameplay.FastRestart", 0);
    CVarRegisterInteger("gEnhancements.Gameplay.FastRestartHoldMs", 1000);
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

    // Seed the "last cutoff" restore value from whatever is persisted (falls back to 15000).
    int hz = CVarGetInteger("gEnhancements.Audio.LowPassHz", 15000);
    if (hz > 0) {
        mLastLowPassHz = hz;
    }
}

// One-time init (GuiElement::Init -> InitElement). Nothing to build: all state is CVar-backed and
// read live each frame. Present for the pure-virtual contract.
void GdxMenuBar::InitElement() {
}

// Per-frame pre-draw update. The menu is stateless (reads CVars in DrawElement), so this is empty.
void GdxMenuBar::UpdateElement() {
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// The menu bar. Base Draw() only calls this when the bar is visible, so no visibility check here.
// DrawElement MUST open/close the main menu bar itself (the base does not — see gdx_menu.h).
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenuBar::DrawElement() {
    // BeginMainMenuBar returns false when the bar is clipped/not renderable; only pair a true
    // return with EndMainMenuBar (ImGui contract).
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    DrawGraphicsMenu();
    DrawAudioMenu();
    DrawGameplayMenu();
    DrawPracticeMenu();
    DrawControlsMenu();
    DrawWorkshopMenu();
    DrawOnlineMenu();
    DrawDeveloperMenu();
    DrawAboutMenu();

    ImGui::EndMainMenuBar();
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
//    / windowed-fullscreen / z-fighting are read live by the backend, so a plain write is enough.
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenuBar::DrawGraphicsMenu() {
    if (!ImGui::BeginMenu("Graphics")) {
        return;
    }

    ImGui::TextDisabled("libultraship renderer");
    ImGui::Separator();

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

    // Windowed fullscreen — CVar gSdlWindowedFullscreen (bool), default 0. CAVEAT: read only by the
    // SDL2 window backend (gfx_sdl2.cpp); if the Windows build runs the DXGI window backend this is
    // inert. Surfaced with a note rather than hidden, since we can't detect the backend here.
    {
        bool on = CVarGetInteger("gSdlWindowedFullscreen", 0) != 0;
        if (ImGui::Checkbox("Windowed fullscreen", &on)) {
            CVarSetInteger("gSdlWindowedFullscreen", on ? 1 : 0);
            GdxSaveCvars();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(SDL2 backend only)");
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

    ImGui::Separator();
    ImGui::TextDisabled("Enhancements");

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

    ImGui::EndMenu();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 2) AUDIO — the P0 pilot. Engine LLE/HLE radio + reconstruction filter enable/cutoff (READY).
//    docs/AUDIO_SETTINGS_SCOPE.md. These write port-owned CVars; the live-read plumbing on the
//    audio thread (gdx_audio_lle.c / os.cpp via extern) is a separate slice — here we only own
//    the UI + CVar state.
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenuBar::DrawAudioMenu() {
    if (!ImGui::BeginMenu("Audio")) {
        return;
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

    ImGui::EndMenu();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 3) GAMEPLAY — docs/menu/GAMEPLAY_TAB.md §4. Two Tier-1 quick wins are LIVE (Fast restart,
//    Autosave-on-record); the rest stay parity-blocked / unbuilt (Coming soon). Every control
//    writes a port-owned gEnhancements.Gameplay.* CVar at a 1:1 default (feature off / stock
//    behavior). The actual behavior lives in the game tick, not here:
//      - Fast restart      -> port/input_bridge.c (gdx_fast_restart_tick) sets the game's own
//                             MENU_CHANGE_RETRY when the combo is held; this tab only owns the
//                             enable + hold-dwell CVars.
//      - Autosave-on-record-> decomp/src/overlays/ovl_i3/menus.c (Gdx_AutosaveGhostOnRecord) auto-
//                             persists the best ghost at race finish; this tab owns the toggle.
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenuBar::DrawGameplayMenu() {
    if (!ImGui::BeginMenu("Gameplay")) {
        return;
    }

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

    ImGui::Separator();

    // Fast restart (hold-to-retry) — CVar gEnhancements.Gameplay.FastRestart, default 0 (off: it
    // changes in-race input). When on, holding L+R+Z for the dwell below during a race triggers the
    // game's OWN retry (the fast in-place RELOAD path, the exact pause-menu RETRY —
    // decomp/src/game/game.c:200-211). The hold detection + trigger live in port/input_bridge.c;
    // this checkbox only owns the CVar.
    {
        bool on = CVarGetInteger("gEnhancements.Gameplay.FastRestart", 0) != 0;
        if (ImGui::Checkbox("Fast restart (hold L+R+Z to retry)", &on)) {
            CVarSetInteger("gEnhancements.Gameplay.FastRestart", on ? 1 : 0);
            GdxSaveCvars();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Hold L + R + Z during a race to instantly re-run it, skipping the\n"
                              "pause menu. Off by default (it changes in-race input behavior).");
        }

        // Hold dwell — CVar gEnhancements.Gameplay.FastRestartHoldMs (ms, default 1000). Prevents
        // accidental mid-race resets. Only meaningful while Fast restart is enabled.
        int holdMs = CVarGetInteger("gEnhancements.Gameplay.FastRestartHoldMs", 1000);
        if (holdMs < 100) {
            holdMs = 100;
        }
        if (holdMs > 3000) {
            holdMs = 3000;
        }
        ImGui::BeginDisabled(!on);
        if (ImGui::SliderInt("Hold time (ms)", &holdMs, 100, 3000)) {
            if (holdMs < 100) {
                holdMs = 100;
            }
            if (holdMs > 3000) {
                holdMs = 3000;
            }
            CVarSetInteger("gEnhancements.Gameplay.FastRestartHoldMs", holdMs);
            GdxSaveCvars();
        }
        ImGui::EndDisabled();
    }

    ImGui::Separator();

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

    ImGui::Separator();

    // Autosave-on-record — CVar gEnhancements.Gameplay.AutosaveOnRecord, default 0.
    // SCOPE (important): stock F-Zero X ALREADY commits numeric records (best times / best lap /
    // max speed / death-race stats) to SRAM immediately on finishing a race (menus.c:252-268), and
    // the port's SRAM is write-through to fzerox.sav (sram_buffer.cpp) — those autosave regardless
    // of this toggle. What this toggle adds is auto-persisting the best GHOST replay, which stock
    // F-Zero X saves only via the manual "Save Ghost" prompt (menus.c:2085-2101 / 2562-2581) — so
    // quitting before that prompt loses the ghost. When on, the port writes the ghost via the
    // game's own Save_SaveGhost on a new same-course best. Off by default so a fresh config
    // reproduces stock behavior (ghosts stay manual-save).
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

    ImGui::EndMenu();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 4) PRACTICE / TOOLS — all future (docs/menu/PRACTICE_TAB.md).
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenuBar::DrawPracticeMenu() {
    if (!ImGui::BeginMenu("Practice")) {
        return;
    }
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
    // Export is read-only w.r.t. the save; Import WRITES the SRAM ghost slot, so it is disabled while
    // an on-track race is live (gdx_input_in_gameplay) to avoid racing the game fiber. Both use the
    // default path next to the exe (ghost_export.gdg); a proper file picker is a later slice.
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
                    snprintf(sGhostStatus, sizeof(sGhostStatus), "Imported %s", path);
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
    // read-only browser of the single saved SRAM ghost + an Export-to-.gdg button. Same live
    // show/hide idiom as the Developer-tab windows (GdxWindowVisible reflects state, click flips it).
    if (ImGui::MenuItem("Ghost Browser...", nullptr, GdxWindowVisible("Ghost Browser"))) {
        GdxToggleWindow("Ghost Browser");
    }

    // Photo mode (free camera) — CVar gEnhancements.Practice.PhotoMode, default 0. When on, pausing
    // during a race hides the HUD (Hud_DrawHud) and lets you free-fly the camera; unpausing or
    // disabling restores the game camera exactly (camera.c saves/restores eye/at/fov each frame).
    // v1 gates only Hud_DrawHud — the minimap and pause overlay still draw.
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
    ImGui::EndMenu();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 5) CONTROLS / INPUT — surface the LUS Input Editor window (READY). docs/menu/CONTROLS_TAB.md.
//    The InputEditorWindow is registered in main.cpp at boot under the name "Input Editor"; here
//    we just toggle its live visibility. Keyboard remap is a separate port-side workstream.
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenuBar::DrawControlsMenu() {
    if (!ImGui::BeginMenu("Controls")) {
        return;
    }

    // "Input Editor" is the name the LUS window expects (InputEditorWindow.h:25). The checkmark
    // reflects the window's current visibility; the click flips it live (see GdxToggleWindow).
    bool open = GdxWindowVisible("Input Editor");
    if (ImGui::MenuItem("Controller Configuration...", nullptr, open)) {
        GdxToggleWindow("Input Editor");
    }

    ImGui::Separator();
    /* Keyboard remapping IS implemented -- it lives inside the same Input Editor window
     * (its "Keyboard" device section), fully rebindable per port. Point the user there
     * rather than showing a misleading "coming soon". */
    ImGui::TextDisabled("Keyboard is remappable too:");
    ImGui::TextDisabled("Controller Configuration > Keyboard");

    ImGui::EndMenu();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 6) WORKSHOP — all future / parity-blocked (docs/menu/WORKSHOP_TAB.md).
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenuBar::DrawWorkshopMenu() {
    if (!ImGui::BeginMenu("Workshop")) {
        return;
    }
    GdxComingSoon("Texture-pack manager (dump / load / hot reload)");
    GdxComingSoon("Track / cup / machine browser + install");
    GdxComingSoon("Installed-content library + quota manager");
    ImGui::EndMenu();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 7) ONLINE / GHOSTS — all future / parity-blocked; netplay additionally decision-gated
//    (docs/menu/ONLINE_TAB.md).
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenuBar::DrawOnlineMenu() {
    if (!ImGui::BeginMenu("Online")) {
        return;
    }
    GdxComingSoon("Leaderboards (per course)");
    GdxComingSoon("Ghost upload / download");
    GdxComingSoon("Netplay lobbies (after decision gate)");
    GdxComingSoon("Spectator / director cam");
    ImGui::EndMenu();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 8) DEVELOPER — surface existing LUS dev windows (READY). docs/menu/DEVELOPER_TAB.md.
//    Console + Stats are auto-registered by the LUS Gui ctor; Gfx Debugger is registered in
//    main.cpp at boot. Each menu item toggles the LIVE window via GetGuiWindow(name)->
//    ToggleVisibility() (a bare CVarSet would not move an already-constructed window).
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenuBar::DrawDeveloperMenu() {
    if (!ImGui::BeginMenu("Developer")) {
        return;
    }

    // Console (auto-registered, name "Console", CVar gConsoleEnabled).
    if (ImGui::MenuItem("Console", nullptr, GdxWindowVisible("Console"))) {
        GdxToggleWindow("Console");
    }
    // Stats / FPS (auto-registered, name "Stats", CVar gStatsEnabled).
    if (ImGui::MenuItem("Stats", nullptr, GdxWindowVisible("Stats"))) {
        GdxToggleWindow("Stats");
    }
    // Gfx Debugger (registered in main.cpp, name "Gfx Debugger", CVar gGfxDebuggerEnabled).
    if (ImGui::MenuItem("Gfx Debugger", nullptr, GdxWindowVisible("Gfx Debugger"))) {
        GdxToggleWindow("Gfx Debugger");
    }

    ImGui::Separator();

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

    ImGui::EndMenu();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 9) ABOUT — static text only (docs/menu/ABOUT_TAB.md). No version string exists in the port
//    today, so we show a fixed pre-alpha label, the EK-required boot notice, and credits.
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenuBar::DrawAboutMenu() {
    if (!ImGui::BeginMenu("About")) {
        return;
    }

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

    ImGui::Separator();
    ImGui::TextDisabled("https://github.com/Zorkats/G-Diffuser");

    ImGui::EndMenu();
}
