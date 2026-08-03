// port/gdx_menu_registry.cpp — the G-Diffuser menu, declared as data.
//
// This file is the entire CONTENT of the menu: which header tabs exist, which pages each tab has,
// how many columns a page is laid out in, and every individual control on it. The shell that walks
// this tree (window, sidebar, search, MenuDrawItem) lives in port/gdx_menu.cpp; the data model
// lives in port/ui/MenuTypes.h.
//
// WHY THE CONTENT IS DATA
// -----------------------
// Because the search box has to be able to find a CONTROL, not just a page. When each page was a
// function full of straight-line widget calls, nothing could enumerate the controls, so search
// matched a separate hand-typed table of page keywords — a second description of the menu that
// could (and did) fall out of step with the first. Now there is one description. DrawSearchResults
// walks exactly what MenuDrawItem draws, so a control cannot be visible-but-unsearchable, and
// adding a control here makes it appear on its page AND in search, with its tooltip and its
// disable reasons, with no edit to the shell.
//
// HOW TO READ AN ENTRY
// --------------------
//     AddWidget("Settings", "Graphics", GdxUI::SECTION_COLUMN_1,
//               GdxUI::WidgetInfo{ .name  = "VSync",
//                                  .cVar  = "gVsyncEnabled",
//                                  .type  = GdxUI::WIDGET_CVAR_CHECKBOX }
//                   .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip("..."))
//                   .DisableWhen({ GdxUI::DISABLE_FOR_NO_WINDOW }));
//
//   .Options(...)      the UIWidgets Options struct for this widget type — tooltip, defaults,
//                      range, label position. Type-checked at compile time (MenuTypes.h ADAPTATION #2).
//   .DisableWhen(...)  named reasons; a greyed control then STATES why, and can state several.
//   .HideWhen(...)     same evaluations, but the control disappears instead of greying out.
//   .PreFunc/.Callback for the controls whose truth is not a stored CVar (live window state, a
//                      derived boolean, an index that is not the CVar value) and for live side
//                      effects. This is where the old per-call-site apply blocks went.
//   .Note("(restart)") the greyed suffix that used to be a hand-written SameLine + TextDisabled.
//   .ModifiedMarker()  the "changed from stock" asterisk (the old GdxCVarCheckboxMarked helper).
//   .SearchTerms(...)  extra keywords, beyond the label and tooltip.
//
// EVERY CVar NAME, DEFAULT, RANGE, TOOLTIP AND SIDE EFFECT BELOW IS CARRIED OVER UNCHANGED from
// the per-page draw functions this replaces. Several tooltips are load-bearing documentation (the
// Frame Interpolation one names both known artifacts; the shader-cache one explains the stall it
// fixes) and are reproduced verbatim, line breaks included.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "gdx_menu.h"
#include "gdx_menu_internal.h"

#include <imgui.h>

#include "ship/window/Window.h" // SetResolutionMultiplier / SetMsaaLevel / IsFullscreen / SetFullscreen
#include "fast/Fast3dWindow.h"  // Fast::Fast3dWindow::SetTextureFilter + Fast::FilteringMode

#include "libultraship/bridge/consolevariablebridge.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace gdxmenu;

// From port/input_bridge.c: nonzero while an on-track race is live. Declared here rather than
// including the bridge header (this TU needs nothing else from it); the signature matches exactly.
// Drives DISABLE_FOR_RACE_IN_PROGRESS.
extern "C" int gdx_input_in_gameplay(void);

// From port/n64_gfx_bridge.cpp: frame-interpolation telemetry for the "subframes last tick" line.
extern "C" int gdx_gfx_interp_last_subframes(void);
extern "C" double gdx_gfx_interp_last_t(void);

namespace {

// gMSAAValue stores the SAMPLE COUNT (1/2/4/8), not a list index, so the dropdown needs an explicit
// index <-> value mapping. Kept next to the labels it pairs with.
const int kMsaaValues[] = { 1, 2, 4, 8 };

} // namespace

// ─────────────────────────────────────────────────────────────────────────────────────────────
// Named disable / hide reasons.
//
// Each evaluation runs EXACTLY ONCE PER FRAME, at the top of GdxMenu::DrawElement, and the result
// is cached in DisabledInfo::active. That is the whole point of the indirection: several of these
// are shared by more than one control, and evaluating them per widget would re-read the same CVar
// (or re-query the window) several times a frame for no benefit.
//
// `reason` is what the user actually reads. It is written as a complete sentence naming the thing
// to change and, where it is not on the same page, where to find it — a greyed control that says
// only "disabled" is a support ticket waiting to happen.
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenu::RegisterDisableReasons() {
    mDisabledInfo.assign(GdxUI::DISABLE_OPTION_COUNT, GdxUI::DisabledInfo{});

    mDisabledInfo[GdxUI::DISABLE_FOR_NO_WINDOW] = {
        [](GdxUI::DisabledInfo&) { return GdxWindow() == nullptr; },
        "The render window is not available yet."
    };

    mDisabledInfo[GdxUI::DISABLE_FOR_WIDESCREEN_OFF] = {
        [](GdxUI::DisabledInfo&) { return CVarGetInteger("gEnhancements.Graphics.Widescreen", 1) == 0; },
        "Widescreen (16:9) is off. Turn it on above to use the widescreen HUD."
    };

    // Strict subset: gdx_widescreen_split_ui_active() (port/input_bridge.c) requires
    // gdx_widescreen_ui_active() as well, so the split-screen switch is inert while the 1P one is
    // off. Saying that in a disabled tooltip beats a checkbox that silently does nothing.
    mDisabledInfo[GdxUI::DISABLE_FOR_WIDESCREEN_UI_OFF] = {
        [](GdxUI::DisabledInfo&) { return CVarGetInteger("gEnhancements.Graphics.WidescreenUI", 1) == 0; },
        "True widescreen HUD/UI is off. Turn it on above to anchor the split-screen HUD."
    };

    mDisabledInfo[GdxUI::DISABLE_FOR_INTERPOLATION_ON] = {
        [](GdxUI::DisabledInfo&) { return CVarGetInteger("gEnhancements.Graphics.FrameInterpolation", 0) != 0; },
        "Frame Interpolation owns frame pacing while it is on."
    };

    // Hide condition, not a disable: interpolation's sub-controls are meaningless while the master
    // toggle is off, and the page used to omit them outright rather than grey them.
    mDisabledInfo[GdxUI::DISABLE_FOR_INTERPOLATION_OFF] = {
        [](GdxUI::DisabledInfo&) { return CVarGetInteger("gEnhancements.Graphics.FrameInterpolation", 0) == 0; },
        "Frame Interpolation is off."
    };

    mDisabledInfo[GdxUI::DISABLE_FOR_INTERP_OVERLAY_OFF] = {
        [](GdxUI::DisabledInfo&) { return CVarGetInteger("gEnhancements.Graphics.InterpDebugOverlay", 0) == 0; },
        "The interpolation debug overlay is off."
    };

    mDisabledInfo[GdxUI::DISABLE_FOR_MATCH_REFRESH_RATE_ON] = {
        [](GdxUI::DisabledInfo&) { return CVarGetInteger("gEnhancements.Graphics.InterpTargetMode", 0) == 0; },
        "Match Refresh Rate is on; the target follows your monitor instead. Turn it off to set a "
        "fixed target here."
    };

    mDisabledInfo[GdxUI::DISABLE_FOR_LOW_PASS_FILTER_OFF] = {
        [](GdxUI::DisabledInfo&) { return CVarGetInteger("gEnhancements.Audio.LowPassHz", 15000) <= 0; },
        "The reconstruction filter is disabled. Enable it above to set a cutoff."
    };

    mDisabledInfo[GdxUI::DISABLE_FOR_RACE_IN_PROGRESS] = {
        [](GdxUI::DisabledInfo&) { return gdx_input_in_gameplay() != 0; },
        "A race is in progress. Ghost state must not be changed alongside the running game."
    };
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// The menu.
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenu::RegisterMenu() {
    using GdxUI::SECTION_COLUMN_1;
    using GdxUI::SECTION_COLUMN_2;
    using GdxUI::WidgetInfo;

    // Sections, in tab order. Each remembers its own last-viewed page, so switching tabs and back
    // returns you where you were — the single global "active page" integer this replaces could only
    // ever remember one place.
    AddMenuEntry("Settings", "gSettings.Menu.Sidebar.Settings");
    AddMenuEntry("Enhancements", "gSettings.Menu.Sidebar.Enhancements");
    AddMenuEntry("Workshop", "gSettings.Menu.Sidebar.Workshop");
    AddMenuEntry("Online", "gSettings.Menu.Sidebar.Online");
    AddMenuEntry("Dev Tools", "gSettings.Menu.Sidebar.DevTools");

    // Page-level search terms. These are the exact keyword strings the old hand-maintained
    // `static const SearchPage pages[]` table carried, moved here so that everything findable
    // before is still findable — widget-level matching is added on top of them, not instead.
    AddSidebarEntry("Settings", "General", 1, "general menu opacity controller navigation about credits licenses");
    AddSidebarEntry("Settings", "Graphics", 1,
                    "graphics internal resolution msaa texture filter vsync fullscreen z fighting");
    AddSidebarEntry("Settings", "Audio", 2, "audio lle hle filter low pass volume reverb latency buffer");
    AddSidebarEntry("Settings", "Controls", 1,
                    "controls controller configuration keyboard gamepad mouse bindings remap");
    AddSidebarEntry("Settings", "Input Viewer", 2, "input viewer overlay analog stick buttons speedrun");

    AddSidebarEntry("Enhancements", "Visuals", 2,
                    "visuals graphics enhancements widescreen hud ui draw distance lod frame pacing "
                    "interpolation smoothing target fps refresh rate");
    AddSidebarEntry("Enhancements", "Gameplay", 1, "gameplay transitions autosave ghost");
    AddSidebarEntry("Enhancements", "Practice", 1,
                    "practice lap delta ghost import export photo mode free camera replay");
    AddSidebarEntry("Enhancements", "Ghosts", 1, "ghost browser replay library opponents import export staff player");

    AddSidebarEntry("Workshop", "Content", 1,
                    "workshop content texture packs track cup machine mods dump reload hi-res font");

    AddSidebarEntry("Online", "Overview", 1, "online leaderboard ghost upload download netplay spectator");

    AddSidebarEntry("Dev Tools", "General", 1,
                    "developer multi viewport tools gates logging diagnostics behavior overrides "
                    "interpolation camera");
    AddSidebarEntry("Dev Tools", "Stats", 1, "stats fps frame timing performance interpolation sub-frames presented");
    AddSidebarEntry("Dev Tools", "Console", 1, "console commands log reset");
    AddSidebarEntry("Dev Tools", "Gfx Debugger", 1, "gfx graphics debugger display list rendering");

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // SETTINGS -> General
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Settings", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Menu Settings", .type = GdxUI::WIDGET_SEPARATOR_TEXT });

    // Stored as a 0.35..1.0 float but presented as a percentage. IsPercentage() scales only the
    // DISPLAY copy (UIWidgets.cpp:716), so the stored value never picks up rounding from an x100
    // round trip. It also rewrites format/min/max as a side effect, so it MUST come before the
    // explicit .Min()/.Max() (UIWidgets.hpp:603-611). The AlwaysClamp flag has no fluent setter on
    // FloatSliderOptions, hence the designated initialiser: it is what keeps a Ctrl+click typed
    // value inside 35..100.
    AddWidget("Settings", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Menu background opacity",
                          .cVar = "gSettings.Menu.BackgroundOpacity",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_FLOAT }
                  .Options(UIWidgets::FloatSliderOptions{ .flags = ImGuiSliderFlags_AlwaysClamp }
                               .IsPercentage()
                               .Min(0.35f)
                               .Max(1.0f)
                               .Step(0.01f)
                               .DefaultValue(0.85f)
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("How opaque this menu's backdrop is over the game "
                                        "(35% = most see-through)."))
                  .SearchTerms("transparency backdrop alpha"));

    AddWidget("Settings", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Menu controller navigation",
                          .cVar = "gControlNav",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Lets a connected gamepad navigate the menu. Game input is blocked "
                      "while the menu is open."))
                  .SearchTerms("gamepad pad joystick nav"));

    AddWidget("Settings", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Open or close this menu with F1, Escape, or Gamepad Back.",
                          .type = GdxUI::WIDGET_TEXT_DISABLED });

    AddWidget("Settings", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Data & Files", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([](WidgetInfo&) { GdxDrawDataAndFilesPanel(); })
                  .SearchTerms("rom z64 ipl 64dd disk ndd archive o2r coverage fallback delete deletable setup"));

    AddWidget("Settings", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "About G-Diffuser", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) { DrawAboutMenu(); })
                  .SearchTerms("about version credits licenses expansion kit legal"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // SETTINGS -> Graphics
    //
    // LUS "courtesy panel" CVars. The read-once trio (internal res / MSAA / texture filter) is
    // consumed by the backend only at window Init, so a plain CVar write is inert until the
    // matching LUS setter is called. Each therefore carries a Callback that applies it LIVE (the
    // standard SoH apply pattern: CVarSet + CVarSave + Set...()):
    //   - internal res -> Ship::Window::SetResolutionMultiplier(float)  (Window.h:140, base virtual)
    //   - MSAA         -> Ship::Window::SetMsaaLevel(uint32_t)          (Window.h:145, base virtual)
    //   - tex filter   -> Fast::Fast3dWindow::SetTextureFilter(FilteringMode) (Fast3dWindow.h:81 —
    //                     Fast3d-only, so a null-safe downcast; skipped w/ CVar-only fallback if
    //                     the backend is not Fast3d).
    // The setters run on the render/GUI thread the menu already draws on (no new thread path).
    // VSync and z-fighting are read live by the backend; fullscreen uses the active Window API.
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Settings", "Graphics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Renderer", .type = GdxUI::WIDGET_SEPARATOR_TEXT });

    // Internal resolution — CVar gInternalResolution (float multiplier), default 1.0.
    // FloatSliderOptions::clamp (default true) performs the lower-bound guard. The callback re-reads
    // the CVar because UIWidgets' CVar widgets report "edited this frame", not the new value.
    AddWidget("Settings", "Graphics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Internal resolution (x)",
                          .cVar = "gInternalResolution",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_FLOAT }
                  .Options(UIWidgets::FloatSliderOptions()
                               .Min(0.5f)
                               .Max(4.0f)
                               .Step(0.01f)
                               .DefaultValue(1.0f)
                               .Format("%.2f")
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Render scale relative to the window size. 1.00x = native; higher is\n"
                                        "sharper but costs GPU. Applies immediately."))
                  .Callback([](WidgetInfo&) {
                      auto window = GdxWindow();
                      if (window != nullptr) {
                          // apply live (Fast3dWindow.cpp:315)
                          window->SetResolutionMultiplier(CVarGetFloat("gInternalResolution", 1.0f));
                      }
                  })
                  .SearchTerms("supersampling render scale sharpness upscale"));

    // MSAA — CVar gMSAAValue (int sample count), default 1 (= off). NOT a CVar-bound combobox: the
    // CVar stores the SAMPLE COUNT, not the list index, so the mapping is explicit. The
    // vector overload keeps the rows in the declared order (the unordered_map one would scramble
    // them — see UIWidgets.hpp's gap list).
    AddWidget("Settings", "Graphics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "MSAA", .type = GdxUI::WIDGET_COMBOBOX }
                  .ValuePointer(&mMsaaIndex)
                  .ComboItems({ "Off (1x)", "2x", "4x", "8x" })
                  .Options(UIWidgets::ComboboxOptions()
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Multi-sample anti-aliasing. Higher smooths edges at a GPU cost.\n"
                                        "Off (1x) = stock. Applies immediately."))
                  .PreFunc([this](WidgetInfo&) {
                      const int cur = CVarGetInteger("gMSAAValue", 1);
                      mMsaaIndex = 0;
                      for (int i = 0; i < 4; ++i) {
                          if (kMsaaValues[i] == cur) {
                              mMsaaIndex = i;
                          }
                      }
                  })
                  .Callback([this](WidgetInfo&) {
                      CVarSetInteger("gMSAAValue", kMsaaValues[mMsaaIndex]);
                      GdxSaveCvars();
                      auto window = GdxWindow();
                      if (window != nullptr) {
                          window->SetMsaaLevel((uint32_t)kMsaaValues[mMsaaIndex]); // live (Fast3dWindow.cpp:319)
                      }
                  })
                  .SearchTerms("anti aliasing antialiasing samples jaggies edges"));

    // Texture filtering — CVar gTextureFilter (enum FilteringMode), default FILTER_THREE_POINT.
    // Enum order is fixed by LUS: gfx_rendering_api.h -> { FILTER_THREE_POINT=0, FILTER_LINEAR=1,
    // FILTER_NONE=2 }. Index == enum value, so the CVar-bound combobox owns read/write/persist and
    // the library's own out-of-range guard previews entry 0 without rewriting the CVar
    // (UIWidgets.hpp:878-880). The live apply needs Fast::Fast3dWindow::SetTextureFilter
    // (Fast3dWindow.cpp:162), which is Fast3d-only — null-safe: on another backend the CVar is
    // still saved and takes effect on the next restart.
    AddWidget("Settings", "Graphics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Texture filter", .cVar = "gTextureFilter", .type = GdxUI::WIDGET_CVAR_COMBOBOX }
                  .ComboItems({
                      "Three-point (N64)", // FILTER_THREE_POINT = 0 (the 1:1 default)
                      "Linear",            // FILTER_LINEAR      = 1
                      "None (sharp)"       // FILTER_NONE        = 2
                  })
                  .Options(UIWidgets::ComboboxOptions()
                               .DefaultIndex(0 /* FILTER_THREE_POINT */)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("How textures are sampled. Three-point mimics the N64 (stock);\n"
                                        "Linear is smoother; None is sharp/pixelated. Applies immediately."))
                  .Callback([](WidgetInfo&) {
                      auto fast = GdxFast3dWindow();
                      if (fast != nullptr) {
                          fast->SetTextureFilter(
                              static_cast<Fast::FilteringMode>(CVarGetInteger("gTextureFilter", 0)));
                      }
                  })
                  .SearchTerms("filtering bilinear smoothing sharp pixelated nearest"));

    AddWidget("Settings", "Graphics", SECTION_COLUMN_1, WidgetInfo{ .type = GdxUI::WIDGET_SEPARATOR });

    // VSync — CVar gVsyncEnabled (bool), default 1 (on). Read live per-present, so a plain write
    // takes effect immediately.
    AddWidget("Settings", "Graphics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "VSync", .cVar = "gVsyncEnabled", .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Syncs presentation to the display refresh to avoid tearing.\n"
                      "On = stock. Turn off if you use Frame pacing."))
                  .SearchTerms("vertical sync tearing"));

    // Fullscreen is live window state, not a CVar: the source of truth is Window::IsFullscreen(),
    // so there is nothing for a CVar widget to read or write. Ship::Window routes the change
    // through the active backend (DXGI borderless on Windows, SDL fullscreen elsewhere) and
    // persists the result via Fast3dWindow's fullscreen-changed callback, exactly like F11.
    AddWidget("Settings", "Graphics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Fullscreen", .type = GdxUI::WIDGET_CHECKBOX }
                  .ValuePointer(&mFullscreen)
                  .Options(UIWidgets::CheckboxOptions().Tooltip(
                      "Uses the active window backend (borderless fullscreen on DX11).\n"
                      "The F11 shortcut controls the same setting."))
                  .PreFunc([this](WidgetInfo&) {
                      auto window = GdxWindow();
                      mFullscreen = window != nullptr && window->IsFullscreen();
                  })
                  .Callback([this](WidgetInfo&) {
                      auto window = GdxWindow();
                      if (window != nullptr) {
                          window->SetFullscreen(mFullscreen);
                      }
                  })
                  .DisableWhen({ GdxUI::DISABLE_FOR_NO_WINDOW })
                  .SearchTerms("borderless window f11 screen"));

    // Z-fighting mode — CVar gZFightingMode (enum), default 0 (= 1:1). Consumed live by the active
    // Fast3D backend when it builds the rasterizer state for DECAL z-mode polygons: it sets the
    // SlopeScaledDepthBias applied to coplanar decal surfaces (track markings, shadows, surface
    // text) so they don't z-fight against the geometry they sit on (gfx_direct3d11.cpp:724, and the
    // matching gfx_opengl/gfx_metal switches). Mode 1 scales the bias by render height to mimic the
    // N64's own decal offset; Mode 2 uses a stronger bias that stops far decals from vanishing.
    // Only visible where decal geometry coexists with its base surface, so the effect is subtle.
    // Index == enum value, so this is a straight CVar-bound combobox.
    AddWidget("Settings", "Graphics", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Z-fighting reduction", .cVar = "gZFightingMode",
                          .type = GdxUI::WIDGET_CVAR_COMBOBOX }
                  .ComboItems({ "Disabled", "N64-style (scaled)", "No vanishing decals" })
                  .Options(UIWidgets::ComboboxOptions()
                               .DefaultIndex(0)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Adjusts the depth bias on decal surfaces (track markings, shadows)\n"
                                        "so they don't shimmer against the road. Disabled = stock."))
                  .SearchTerms("depth bias decal shimmer flicker markings shadows"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // SETTINGS -> Audio (2 columns: the signal path on the left, levels and latency on the right)
    //
    // These write port-owned CVars; the live-read plumbing on the audio thread lives in
    // gdx_audio_lle.c and os.cpp.
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Settings", "Audio", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Output status", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) { DrawAudioStatus(); })
                  .SearchTerms("backend wasapi sdl driver dummy queued samples diagnostic no sound silent"));

    AddWidget("Settings", "Audio", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Output Device", .type = GdxUI::WIDGET_SEPARATOR_TEXT });

    // Output backend — CVar gEnhancements.Audio.Backend (0=Auto, 1=WASAPI, 2=SDL). Applied at
    // startup in main.cpp's InitAudio; Auto keeps libultraship's per-platform default (WASAPI on
    // Windows, SDL on Linux). Only backends that exist on this platform are offered, so on
    // non-Windows hosts the reduced two-entry list maps index 1 -> CVar 2 (SDL) — which is why this
    // is a plain Combobox over an index rather than a CVar-bound one.
    AddWidget("Settings", "Audio", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Output backend", .type = GdxUI::WIDGET_COMBOBOX }
                  .ValuePointer(&mAudioBackendIndex)
#ifdef _WIN32
                  .ComboItems({ "Auto", "WASAPI", "SDL" })
                  .PreFunc([this](WidgetInfo&) {
                      const int sel = CVarGetInteger("gEnhancements.Audio.Backend", 0);
                      mAudioBackendIndex = (sel >= 0 && sel <= 2) ? sel : 0;
                  })
                  .Callback([this](WidgetInfo&) {
                      CVarSetInteger("gEnhancements.Audio.Backend", mAudioBackendIndex);
                      GdxSaveCvars();
                  })
#else
                  .ComboItems({ "Auto", "SDL" })
                  .PreFunc([this](WidgetInfo&) {
                      // map the stored CVar (2 = SDL) into the reduced list
                      mAudioBackendIndex = (CVarGetInteger("gEnhancements.Audio.Backend", 0) == 2) ? 1 : 0;
                  })
                  .Callback([this](WidgetInfo&) {
                      CVarSetInteger("gEnhancements.Audio.Backend", mAudioBackendIndex == 1 ? 2 : 0);
                      GdxSaveCvars();
                  })
#endif
                  .Options(UIWidgets::ComboboxOptions()
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Which OS audio output path to use. Auto keeps the platform default\n"
                                        "(WASAPI on Windows, SDL elsewhere). Applies on restart."))
                  .SearchTerms("wasapi sdl device api output"));

    AddWidget("Settings", "Audio", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Applies on restart.", .type = GdxUI::WIDGET_TEXT_DISABLED });

    // Engine — CVar gEnhancements.Audio.LLE, default 1. LLE = accurate (cxd4 RSP), HLE = fast.
    // CVarRadioButton does the read / write / persist for both entries; `radioValue` is the value
    // each button writes, so the two share one CVar with no explicit state here.
    // KNOWN QUIRK (UIWidgets.cpp:1126-1136): it draws the radio with an invisible label and then
    // the visible text as a separate item, and hangs the tooltip off THAT text. Hovering the circle
    // does not raise the tooltip; hovering the label does.
    AddWidget("Settings", "Audio", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Synthesis Engine", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Settings", "Audio", SECTION_COLUMN_1,
              WidgetInfo{ .name = "LLE (accurate)", .cVar = "gEnhancements.Audio.LLE",
                          .type = GdxUI::WIDGET_CVAR_RADIO_BUTTON }
                  .RadioValue(1)
                  .Options(UIWidgets::RadioButtonsOptions().DefaultIndex(1).Tooltip(
                      "Low-level RSP emulation (cxd4). Most accurate; the default."))
                  .SearchTerms("engine rsp microcode accuracy audio synthesis"));
    AddWidget("Settings", "Audio", SECTION_COLUMN_1,
              WidgetInfo{ .name = "HLE (fast)", .cVar = "gEnhancements.Audio.LLE",
                          .type = GdxUI::WIDGET_CVAR_RADIO_BUTTON }
                  .RadioValue(0)
                  .Options(UIWidgets::RadioButtonsOptions().DefaultIndex(1).Tooltip(
                      "High-level audio emulation. Faster, less accurate."))
                  // The asterisk belongs to the PAIR, not to either button, so it is drawn once
                  // after the second one. The default is LLE, i.e. CVar == 1.
                  .PostFunc([](WidgetInfo&) { GdxModifiedMarker(CVarGetInteger("gEnhancements.Audio.LLE", 1) == 0); })
                  .SearchTerms("engine fast performance audio synthesis"));

    // Output reconstruction filter — CVar gEnhancements.Audio.LowPassHz, default 15000. A value of
    // 0 disables the filter; any value 500..16000 is the low-pass cutoff.
    //
    // Both controls are non-CVar on purpose: "on" is not the stored value but `stored > 0`, and
    // turning it off must stash the live cutoff in mLastLowPassHz so re-enabling restores the same
    // frequency. A CVar checkbox would write a bare 0/1 into a Hz field and lose the cutoff.
    AddWidget("Settings", "Audio", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Reconstruction Filter", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Settings", "Audio", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Enable filter", .type = GdxUI::WIDGET_CHECKBOX }
                  .ValuePointer(&mLowPassFilterOn)
                  .Options(UIWidgets::CheckboxOptions().Tooltip(
                      "Low-pass filter on the reconstructed output, softening high-frequency\n"
                      "aliasing. On = stock. Off disables the filter entirely."))
                  .PreFunc([this](WidgetInfo&) {
                      mLowPassFilterOn = CVarGetInteger("gEnhancements.Audio.LowPassHz", 15000) > 0;
                  })
                  .Callback([this](WidgetInfo&) {
                      if (mLowPassFilterOn) {
                          const int restore = mLastLowPassHz > 0 ? mLastLowPassHz : 15000;
                          CVarSetInteger("gEnhancements.Audio.LowPassHz", restore);
                      } else {
                          const int hz = CVarGetInteger("gEnhancements.Audio.LowPassHz", 15000);
                          if (hz > 0) {
                              mLastLowPassHz = hz; // remember so re-enabling restores the same cutoff
                          }
                          CVarSetInteger("gEnhancements.Audio.LowPassHz", 0);
                      }
                      GdxSaveCvars();
                  })
                  .PostFunc([this](WidgetInfo&) { GdxModifiedMarker(!mLowPassFilterOn); }) // default is on
                  .SearchTerms("low pass lowpass reconstruction aliasing treble"));

    // While the filter is off the CVar holds 0, but the slider must keep showing the remembered
    // cutoff instead of snapping to the bottom of the range — hence the non-CVar slider and the
    // preFunc that restores the display value. IntSliderOptions::clamp covers the lower bound.
    AddWidget("Settings", "Audio", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Cutoff (Hz)", .type = GdxUI::WIDGET_SLIDER_INT }
                  .ValuePointer(&mLowPassCutoffHz)
                  .Options(UIWidgets::IntSliderOptions()
                               .Min(500)
                               .Max(16000)
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Cutoff frequency of the reconstruction low-pass. Lower = softer/darker.\n"
                                        "Enable the filter above to adjust this."))
                  .PreFunc([this](WidgetInfo&) {
                      const int hzNow = CVarGetInteger("gEnhancements.Audio.LowPassHz", 15000);
                      mLowPassCutoffHz = hzNow > 0 ? hzNow : (mLastLowPassHz > 0 ? mLastLowPassHz : 15000);
                  })
                  .Callback([this](WidgetInfo&) {
                      CVarSetInteger("gEnhancements.Audio.LowPassHz", mLowPassCutoffHz);
                      mLastLowPassHz = mLowPassCutoffHz;
                      GdxSaveCvars();
                  })
                  .DisableWhen({ GdxUI::DISABLE_FOR_LOW_PASS_FILTER_OFF })
                  .SearchTerms("frequency hz low pass lowpass darker brighter"));

    // ── Column 2: levels and latency ─────────────────────────────────────────────────────────
    // Master volume — CVar gEnhancements.Audio.MasterVolume (0..100 %, default 100). Applied as a
    // final-stage gain multiply on the s16 output copy in os.cpp's osAiSetNextBuffer, read live
    // there each buffer (same live-CVar pattern as the low-pass). 100 = no-op (the multiply is
    // skipped entirely), so the default is bit-exact. IntSliderOptions::clamp replaces the old
    // 0..100 guards; the tooltip's "%%" is a plain "%" because UIWidgets renders tooltips through
    // SetTooltip("%s", ...) rather than as a format string.
    AddWidget("Settings", "Audio", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Levels", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Settings", "Audio", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Master volume (%)", .cVar = "gEnhancements.Audio.MasterVolume",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_INT }
                  .Options(UIWidgets::IntSliderOptions()
                               .Min(0)
                               .Max(100)
                               .DefaultValue(100)
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Final output gain. 100% = stock (bit-exact, no gain applied)."))
                  .SearchTerms("volume loudness gain level"));

    // Reverb — CVar gEnhancements.Audio.Reverb (default 1 = on). Wired to the HLE reverb kill switch
    // in n64_audio_hle.c (the A_MIXER wet->dry return), read live there. NOTE: this affects the HLE
    // audio engine ONLY; under the default LLE engine reverb is produced by the audio microcode
    // itself, so toggling this has no audible effect while LLE is selected. Still wired correctly
    // for the HLE fallback path.
    AddWidget("Settings", "Audio", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Reverb", .cVar = "gEnhancements.Audio.Reverb",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Affects the HLE audio engine only.\n"
                      "Under the default LLE engine, reverb is the microcode's own."))
                  .ModifiedMarker()
                  .SearchTerms("echo wet dry ambience"));

    // Latency / buffer size — CVar gEnhancements.Audio.BufferFrames (frames, default 4096, range
    // 1024..8192). Read ONCE at InitAudio (main.cpp), so a change applies only on the next restart
    // (hence the note). A larger reservoir rides out host scheduling jitter better but adds output
    // latency; a smaller one is snappier but more underrun-prone.
    AddWidget("Settings", "Audio", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Latency", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Settings", "Audio", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Buffer size (frames)", .cVar = "gEnhancements.Audio.BufferFrames",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_INT }
                  .Options(UIWidgets::IntSliderOptions()
                               .Min(1024)
                               .Max(8192)
                               .DefaultValue(4096)
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Audio buffer size. Larger rides out host jitter (fewer dropouts) but\n"
                                        "adds latency; smaller is snappier but more underrun-prone. "
                                        "Applies on restart."))
                  .Note("(applies on restart)")
                  .SearchTerms("latency buffer dropouts underrun crackle stutter"));

    AddWidget("Settings", "Audio", SECTION_COLUMN_2,
              WidgetInfo{ .name = "More", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Settings", "Audio", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Sound test / jukebox", .type = GdxUI::WIDGET_COMING_SOON });

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // SETTINGS -> Controls
    // The InputEditorWindow is registered in main.cpp at boot under the name "Input Editor"; this
    // page embeds it (or pops it out). Keyboard remap is a separate port-side workstream.
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Settings", "Controls", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Input Editor", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) {
                      DrawToolWindowPage("Input Editor", "Configure controllers, keyboard, mouse, deadzones, "
                                                         "sensitivity, and per-port mappings.");
                  })
                  .SearchTerms("controller keyboard mouse deadzone sensitivity mapping bindings remap port"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // SETTINGS -> Input Viewer (2 columns)
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Input Viewer", .type = GdxUI::WIDGET_SEPARATOR_TEXT });

    // Non-CVar: an already-constructed GuiWindow reads its visibility CVar only at construction, so
    // the live state is the window's own and the flip must go through ToggleVisibility (see the
    // GdxToggleWindow comment in gdx_menu.cpp).
    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Show input viewer overlay", .type = GdxUI::WIDGET_CHECKBOX }
                  .ValuePointer(&mInputViewerVisible)
                  .Options(UIWidgets::CheckboxOptions().Tooltip(
                      "Shows the exact mapped N64 input state delivered to F-Zero X."))
                  .PreFunc([this](WidgetInfo&) { mInputViewerVisible = GdxWindowVisible("Input Viewer"); })
                  .Callback([](WidgetInfo&) { GdxToggleWindow("Input Viewer"); })
                  .SearchTerms("overlay hud speedrun display controller"));

    // AlwaysClamp has no fluent setter on FloatSliderOptions, hence the designated initialiser on
    // both overlay sliders; FloatSliderOptions::clamp additionally snaps the stored value to the
    // step's decimal precision, which a raw CVarSetFloat did not.
    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Overlay scale", .cVar = "gInputViewer.Scale",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_FLOAT }
                  .Options(UIWidgets::FloatSliderOptions{ .flags = ImGuiSliderFlags_AlwaysClamp }
                               .Min(0.5f)
                               .Max(2.5f)
                               .Step(0.01f)
                               .DefaultValue(1.0f)
                               .Format("%.2fx")
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Size of the on-screen input overlay."))
                  .SearchTerms("size zoom"));
    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Overlay opacity", .cVar = "gInputViewer.Opacity",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_FLOAT }
                  .Options(UIWidgets::FloatSliderOptions{ .flags = ImGuiSliderFlags_AlwaysClamp }
                               .Min(0.2f)
                               .Max(1.0f)
                               .Step(0.01f)
                               .DefaultValue(1.0f)
                               .Format("%.2f")
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Transparency of the input overlay."))
                  .SearchTerms("transparency alpha"));
    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Enable dragging", .cVar = "gInputViewer.EnableDragging",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Lets you reposition the overlay by dragging it with the mouse."))
                  .SearchTerms("move position mouse drag"));

    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Appearance", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Show background layer", .cVar = "gInputViewer.ShowBackground",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Draws the controller-body backdrop behind the buttons.")));
    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Show D-pad layers", .cVar = "gInputViewer.ShowDpad",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Includes the D-pad in the overlay (off by default; F-Zero X does not use it).")));
    // Index == stored value, so this is a straight CVar-bound combobox; the library's out-of-range
    // guard previews entry 0 without rewriting the CVar.
    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Button outlines", .cVar = "gInputViewer.ButtonOutlineMode",
                          .type = GdxUI::WIDGET_CVAR_COMBOBOX }
                  .ComboItems({ "Always shown", "Shown while released", "Shown while pressed", "Hidden" })
                  .Options(UIWidgets::ComboboxOptions()
                               .DefaultIndex(1)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("When each button's outline is drawn relative to its pressed state.")));
    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Show analog values", .cVar = "gInputViewer.ShowAnalogValues",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Prints the raw analog-stick X/Y numbers next to the stick."))
                  .SearchTerms("numbers coordinates stick x y"));
    AddWidget("Settings", "Input Viewer", SECTION_COLUMN_2,
              WidgetInfo{ .name = "The viewer reads G-Diffuser's final mapped N64 state, after controller "
                                  "bindings and analog curves. Inputs intentionally read neutral while this "
                                  "menu owns game input.",
                          .type = GdxUI::WIDGET_TEXT });

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // ENHANCEMENTS -> Visuals (2 columns: aspect/detail on the left, pacing on the right)
    //
    // Every default reproduces today's rendering (the optionality constitution). Registration of
    // the CVars themselves stays in the GdxMenu constructor, next to the comments explaining what
    // each one does at 1:1.
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Visual Enhancements", .type = GdxUI::WIDGET_SEPARATOR_TEXT });

    // Widescreen (16:9) — CVar gEnhancements.Graphics.Widescreen, default 1 (= today's behavior).
    // Read live in interpreter.cpp AdjXForAspectRatio: 1 keeps the current 16:9 hor+ aspect
    // correction (byte-identical default), 0 renders 4:3 with pillarbox bars. OFF has two documented
    // edge cases (MSAA>1 at exactly 1x internal res; AdvancedResolution takes precedence).
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Widescreen (16:9)", .cVar = "gEnhancements.Graphics.Widescreen",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "On: fills the window in 16:9 (hor+).\n"
                      "Off: renders 4:3 with pillarbox bars on the sides."))
                  .ModifiedMarker()
                  .SearchTerms("aspect ratio 4:3 pillarbox hor+ 16:9"));

    // True widescreen HUD/UI — gEnhancements.Graphics.WidescreenUI, default 1 (see the registration
    // and the one-time migration in gdx_menu.cpp: it ships with Widescreen because a widescreen
    // world behind a 4:3-placed HUD reads as a defect). DefaultValue must track the registration or
    // the "changed from stock" asterisk lies. Requires Widescreen, which is a NAMED disable reason
    // rather than a bare greyed checkbox: the disabled tooltip says what to turn on and where.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "True widescreen HUD/UI", .cVar = "gEnhancements.Graphics.WidescreenUI",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Anchors the single-player HUD to the true screen edges and extends\n"
                      "the SELECT MACHINE blue background and race transitions. Other\n"
                      "menu artwork stays proportional in 4:3. Requires Widescreen."))
                  .ModifiedMarker()
                  .DisableWhen({ GdxUI::DISABLE_FOR_WIDESCREEN_OFF })
                  .SearchTerms("hud ui corner select machine transitions widescreen"));

    // Split-screen HUD anchoring — gEnhancements.Graphics.WidescreenSplitUI, default 1. Registered
    // in gdx_menu.cpp next to WidescreenUI, whose reasoning for a separate switch is recorded there.
    // Disabled (not hidden) when the 1P switch is off, because the reason is worth reading.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Widescreen split-screen HUD", .cVar = "gEnhancements.Graphics.WidescreenSplitUI",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Anchors the 2P/3P/4P race HUD to the edges of each player's view instead\n"
                      "of letting it bunch toward the middle of a 16:9 screen. Covers the timer,\n"
                      "lap counter, energy gauge, minimap, position and speed.\n\n"
                      "Elements the game centres inside a column (interval, reverse, the 3P spare\n"
                      "minimap) deliberately stay on the stock path. The VS machine-select screen\n"
                      "is not covered yet. Requires True widescreen HUD/UI."))
                  .ModifiedMarker()
                  .DisableWhen({ GdxUI::DISABLE_FOR_WIDESCREEN_UI_OFF })
                  .SearchTerms("split screen vs battle death race 2p 3p 4p hud multiplayer anchor"));

    // Draw distance — CVar gEnhancements.Graphics.DrawDistance (%, default 100 = stock). Scales each
    // course's own far-render cutoff per-venue (course.c Course_Draw); 100% is bit-exact.
    //
    // SLIDER CAPPED AT 200% ON PURPOSE (effective ceiling, honest UI). The CVar multiplies the
    // per-chunk cull threshold (sCourseFarRenderDistance * scale), but the track itself is streamed
    // as a fixed set of chunks that Course_SegmentsInit builds only out to a bounded horizon
    // (gSegmentChunks, capped at SEGMENT_CHUNK_COUNT — course.c). By ~200% the raised cull threshold
    // already clears the depth of the furthest chunk that was ever built, so pushing the scale
    // higher un-culls nothing: there is no loaded geometry beyond that point to draw. This is a
    // content/streaming limit, not a code clamp that could simply be raised.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Draw distance (%)", .cVar = "gEnhancements.Graphics.DrawDistance",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_INT }
                  .Options(UIWidgets::IntSliderOptions()
                               .Min(100)
                               .Max(200)
                               .DefaultValue(100)
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Extends how far each track's own geometry renders (100% = stock,\n"
                                        "scales per-venue). 200% is the effective max: beyond it the track's\n"
                                        "streamed geometry runs out, so there is nothing further to draw."))
                  .SearchTerms("render distance fog pop-in culling horizon"));

    // Force max machine detail — CVar gEnhancements.Graphics.ForceMaxMachineLOD (default 0 = stock
    // distance-based LOD). When on, every machine draws at its highest-detail model (racer.c).
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Force max machine detail", .cVar = "gEnhancements.Graphics.ForceMaxMachineLOD",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Always renders every machine at its highest-detail model,\n"
                      "ignoring distance. Off = stock distance-based detail."))
                  .ModifiedMarker()
                  .SearchTerms("lod level of detail model quality machines"));

    // ── Column 2: pacing / interpolation ─────────────────────────────────────────────────────
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Enhancements (parity-gated)", .type = GdxUI::WIDGET_SEPARATOR_TEXT });

    // Frame pacing — CVar gEnhancements.Graphics.FramePacing, default 0. libultraship's Fast3D
    // backend already caps the loop to ~60fps, so this is opt-in: when on, port/gdx_frame_pacer.c
    // holds the host loop to the true N64 NTSC field rate (~59.94Hz) with a wall-clock sleep+spin.
    // Recommend VSync OFF while on (a display-refresh present beats against the fixed schedule).
    // Mutually exclusive with Frame Interpolation (both are pacing owners) — expressed as the named
    // reason DISABLE_FOR_INTERPOLATION_ON.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Frame pacing (59.94 Hz)", .cVar = "gEnhancements.Graphics.FramePacing",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Experimental. The renderer already limits the game to ~60 fps; this\n"
                      "pins the loop to the true N64 rate (59.94 Hz). Turn VSync OFF when using it."))
                  .ModifiedMarker()
                  .DisableWhen({ GdxUI::DISABLE_FOR_INTERPOLATION_ON })
                  .SearchTerms("ntsc 59.94 pacing timing judder"));

    // Frame interpolation — CVar gEnhancements.Graphics.FrameInterpolation, default 0. EXPERIMENTAL.
    // Read LIVE every tick (gdx_interp::P2HostActive / port/gdx_frame_pacer.c), so this toggle takes
    // effect on the next tick like FramePacing above — no restart needed.
    //
    // The tooltip below is load-bearing documentation (it names both known artifacts), carried
    // across verbatim. Every line is under UIWidgets' 80-character wrap width, so WrappedText
    // (UIWidgets.cpp:59) leaves the manual line breaks exactly as written.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Frame Interpolation (EXPERIMENTAL)",
                          .cVar = "gEnhancements.Graphics.FrameInterpolation",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Interpolates rendering between 60Hz logic ticks for smoother motion on\n"
                      "high-refresh displays. VSync ON is recommended. Adds about half a tick\n"
                      "of latency. Bypasses Frame pacing while on. Default OFF.\n"
                      "\n"
                      "EXPERIMENTAL - two known artifacts, both inherent to matrix-only\n"
                      "interpolation (SoH-class ports share them):\n"
                      "  - Strobing on flicker-blend effects. The game alternates certain\n"
                      "    transparencies every 60Hz tick (low-energy body gradient, pursuit\n"
                      "    marker pulse); each phase is held for a whole tick's sub-frames, and\n"
                      "    the sub-frame count oscillates, so phases get unequal screen time.\n"
                      "  - Static scenery and HUD do not tween. Only matrices the game rebuilds\n"
                      "    each frame are interpolated; baked asset display lists stay at 60Hz,\n"
                      "    so they can judder against smoothly moving geometry."))
                  .ModifiedMarker()
                  .SearchTerms("smooth motion high refresh 120hz 144hz tween subframe"));

    // The four controls below belong to Frame Interpolation and are HIDDEN while it is off, exactly
    // as the old page omitted them. Hiding is driven by the same once-per-frame evaluation cache as
    // disabling (DISABLE_FOR_INTERPOLATION_OFF), so the CVar behind it is read once, not four times.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Debug overlay", .cVar = "gEnhancements.Graphics.InterpDebugOverlay",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Show live sub-frame statistics."))
                  .HideWhen({ GdxUI::DISABLE_FOR_INTERPOLATION_OFF })
                  .SearchTerms("interpolation subframes statistics diagnostic"));

    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Sub-frames last tick", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([](WidgetInfo&) {
                      const int sub = gdx_gfx_interp_last_subframes();
                      const double t = gdx_gfx_interp_last_t();
                      ImGui::TextDisabled("subframes last tick: %d (t=%.2f)", sub, t);
                  })
                  .HideWhen({ GdxUI::DISABLE_FOR_INTERPOLATION_OFF, GdxUI::DISABLE_FOR_INTERP_OVERLAY_OFF })
                  .HideInSearch()); // a read-out with no setting behind it; the toggle above is the control

    // Target-rate mode — CVar gEnhancements.Graphics.InterpTargetMode, default 0 (Match Refresh
    // Rate). Read LIVE by main.cpp's per-tick M derivation. Non-CVar variant: the checkbox reads
    // TRUE for CVar value 0 (Match Refresh Rate) and FALSE for 1 (Capped), and CVarCheckbox has no
    // inversion, so the mapping stays here and the widget supplies styling and tooltip only.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Match Refresh Rate", .type = GdxUI::WIDGET_CHECKBOX }
                  .ValuePointer(&mInterpMatchRefresh)
                  .Options(UIWidgets::CheckboxOptions().Tooltip("Targets your monitor's current refresh rate."))
                  .PreFunc([this](WidgetInfo&) {
                      mInterpMatchRefresh = CVarGetInteger("gEnhancements.Graphics.InterpTargetMode", 0) == 0;
                  })
                  .Callback([this](WidgetInfo&) {
                      CVarSetInteger("gEnhancements.Graphics.InterpTargetMode", mInterpMatchRefresh ? 0 : 1);
                      GdxSaveCvars();
                  })
                  .HideWhen({ GdxUI::DISABLE_FOR_INTERPOLATION_OFF })
                  .SearchTerms("monitor hz refresh target interpolation"));

    // Target FPS — CVar gEnhancements.Graphics.InterpTargetFps, default 120. Only consulted in
    // Capped mode (Match Refresh Rate off); the named reason states exactly that when greyed.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Target FPS", .cVar = "gEnhancements.Graphics.InterpTargetFps",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_INT }
                  .Options(UIWidgets::IntSliderOptions()
                               .Min(60)
                               .Max(480)
                               .DefaultValue(120)
                               .Format("%d FPS")
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("Interpolation target frame rate. Values above your refresh rate\n"
                                        "waste GPU without improving output. Each 60fps of target adds a\n"
                                        "full render pass per tick."))
                  .HideWhen({ GdxUI::DISABLE_FOR_INTERPOLATION_OFF })
                  .DisableWhen({ GdxUI::DISABLE_FOR_MATCH_REFRESH_RATE_ON })
                  .SearchTerms("frame rate cap interpolation target"));

    // Interpolate camera — CVar gEnhancements.Graphics.InterpolateCamera, default ON. Decides what
    // interpolation covers rather than whether it runs, so it is hidden unless interpolation is on.
    // With it off, the camera and the whole track sit at 60 Hz while machines tween against them,
    // which separates CPU-baked effects (booster flames) from the machines they belong to.
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Interpolate Camera", .cVar = "gEnhancements.Graphics.InterpolateCamera",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Smooths the camera and the track, not just the machines.\n"
                      "Turning this off interpolates vehicles against a static world,\n"
                      "which makes engine effects appear to separate from the machines.\n"
                      "Leave on unless you are comparing the two."))
                  .HideWhen({ GdxUI::DISABLE_FOR_INTERPOLATION_OFF })
                  .SearchTerms("camera track projection interpolation smooth"));

    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_2,
              WidgetInfo{ .name = "Mirror mode", .type = GdxUI::WIDGET_COMING_SOON });
    AddWidget("Enhancements", "Visuals", SECTION_COLUMN_2,
              WidgetInfo{ .name = "FLX reflection quality", .type = GdxUI::WIDGET_COMING_SOON });

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // ENHANCEMENTS -> Gameplay
    // Every control writes a port-owned gEnhancements.Gameplay.* CVar at a 1:1 default (feature
    // off / stock behavior). The actual behavior lives in the game tick, not here.
    // ═════════════════════════════════════════════════════════════════════════════════════════

    // Skippable transitions — default 0 (stock: transitions play fully). When on, transition.c
    // Transition_Update re-runs its same per-tick logic in one call until finished (up to 128x), so
    // screen wipes resolve near-instantly. The stock per-tick switch is byte-unchanged; only the
    // surrounding loop budget differs. [PB].
    AddWidget("Enhancements", "Gameplay", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Skip/shorten transitions", .cVar = "gEnhancements.Gameplay.SkippableTransitions",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Fast-completes screen-transition wipes instead of playing them in full.\n"
                      "Off by default (parity)."))
                  .ModifiedMarker()
                  .SearchTerms("wipe fade loading speed"));

    // Reduce Course Edit flashing — default 1 (matching CVarRegisterInteger + the
    // gdx.Migrations.ReduceEditorFlashingOn one-shot in the ctor). When on, the Course Edit node
    // blink/checker parity and the flagged-node size pulse advance at half rate
    // (course_edit/191080.c func_xk2_800E04E0, #ifdef PORT). Off is bit-identical to stock.
    AddWidget("Enhancements", "Gameplay", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Reduce Course Edit flashing",
                          .cVar = "gEnhancements.Gameplay.ReduceEditorFlashing",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Halves the Course Edit blink/checker cadence. The 20Hz strobe is\n"
                      "authentic N64 behavior; this calms it on modern displays."))
                  .ModifiedMarker()
                  .SearchTerms("strobe epilepsy photosensitive blink editor accessibility"));

    AddWidget("Enhancements", "Gameplay", SECTION_COLUMN_1, WidgetInfo{ .type = GdxUI::WIDGET_SEPARATOR });

    // Autosave-on-record — default 0.
    // SCOPE (important): stock F-Zero X ALREADY commits numeric records (best times / best lap /
    // max speed / death-race stats) to SRAM immediately on finishing a race (menus.c:252-268), and
    // the port's SRAM is write-through to fzerox.sav (sram_buffer.cpp) — those autosave regardless
    // of this toggle. What this toggle adds is auto-persisting the best GHOST replay, which stock
    // F-Zero X saves only via the manual "Save Ghost" prompt (menus.c:2085-2101 / 2562-2581) — so
    // quitting before that prompt loses the ghost.
    AddWidget("Enhancements", "Gameplay", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Autosave ghost on new record", .cVar = "gEnhancements.Gameplay.AutosaveOnRecord",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Auto-save your best Time Attack ghost replay when you beat it,\n"
                      "without the manual Save-Ghost prompt.\n"
                      "(Record TIMES already autosave in stock F-Zero X.) Off by default."))
                  .ModifiedMarker()
                  .SearchTerms("ghost replay save record time attack"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // ENHANCEMENTS -> Practice
    // ═════════════════════════════════════════════════════════════════════════════════════════

    // Lap-split deltas — default 0 (stock: nothing drawn). When on, Practice mode draws how the last
    // completed lap compares to the session best (or a loaded ghost's same lap, once ghosts populate
    // outside Time Attack). Drawn in hud.c under #ifdef PORT.
    AddWidget("Enhancements", "Practice", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Show lap deltas", .cVar = "gEnhancements.Practice.ShowLapDeltas",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "In Practice mode, shows your last lap vs your session best\n"
                      "(green = faster, red = slower). Off by default."))
                  .ModifiedMarker()
                  .SearchTerms("split time comparison lap delta"));

    AddWidget("Enhancements", "Practice", SECTION_COLUMN_1, WidgetInfo{ .type = GdxUI::WIDGET_SEPARATOR });

    AddWidget("Enhancements", "Practice", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Ghost replay (.gdg)", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) { DrawGhostIo(); })
                  .SearchTerms("export import ghost gdg replay file share"));

    AddWidget("Enhancements", "Practice", SECTION_COLUMN_1, WidgetInfo{ .type = GdxUI::WIDGET_SEPARATOR });

    // Ghost Browser window toggle (GdxGhostWindow, registered via AddGuiWindow in main.cpp). Same
    // live show/hide idiom as the Dev Tools windows; the label reflects the current state, so it is
    // recomputed in the preFunc each frame.
    AddWidget("Enhancements", "Practice", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Open Ghost Browser window", .type = GdxUI::WIDGET_BUTTON }
                  .Options(UIWidgets::ButtonOptions()
                               .Size(UIWidgets::Sizes::Inline)
                               .Tooltip("Browse your per-course player-ghost library and export ghosts to .gdg."))
                  .PreFunc([](WidgetInfo& widget) {
                      widget.name = GdxWindowVisible("Ghost Browser") ? "Return Ghost Browser to menu"
                                                                      : "Open Ghost Browser window";
                  })
                  .Callback([](WidgetInfo&) { GdxToggleWindow("Ghost Browser"); })
                  .SearchTerms("ghost browser library window open return"));

    // Photo mode (free camera) is available in every race mode. When enabled, pausing suppresses
    // all race HUD/pause overlays and reserves their controls for the free camera. Disabling the
    // toggle restores the normal paused UI; unpausing restores the game camera exactly.
    AddWidget("Enhancements", "Practice", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Photo mode (free camera)", .cVar = "gEnhancements.Practice.PhotoMode",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Pause during a race to hide the HUD and free-fly the camera.\n"
                      "Stick: dolly/truck  -  C-buttons: look  -  L/R: FOV  -  hold Z: raise/lower.\n"
                      "Unpausing or turning this off restores the game camera exactly. Off by default."))
                  .ModifiedMarker()
                  .SearchTerms("photo camera free fly screenshot fov"));

    AddWidget("Enhancements", "Practice", SECTION_COLUMN_1, WidgetInfo{ .type = GdxUI::WIDGET_SEPARATOR });
    AddWidget("Enhancements", "Practice", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Replay theater", .type = GdxUI::WIDGET_COMING_SOON });
    AddWidget("Enhancements", "Practice", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Diagnostic overlay", .type = GdxUI::WIDGET_COMING_SOON });

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // ENHANCEMENTS -> Ghosts
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Enhancements", "Ghosts", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Ghost Browser", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) {
                      DrawToolWindowPage("Ghost Browser",
                                         "Manage multiple local and imported player ghosts per exact course and "
                                         "select up to three Time Attack opponents. Staff ghosts remain "
                                         "controlled by the base game.");
                  })
                  .SearchTerms("ghost library opponents time attack staff player import export"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // WORKSHOP -> Content
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Workshop", "Content", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Texture Packs", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Workshop", "Content", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Enable texture packs", .cVar = "gEnhancements.Workshop.TexturePacks",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(false).Tooltip(
                      "Overrides game textures from mods/*.o2r packs.\nOff = stock rendering."))
                  .ModifiedMarker()
                  .SearchTerms("mods o2r hi-res retexture override"));
    AddWidget("Workshop", "Content", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Installed texture packs", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) { DrawTexturePacks(); })
                  .SearchTerms("packs list reload mods folder manifest priority order"));

    AddWidget("Workshop", "Content", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Asset Dump", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Workshop", "Content", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Asset Dump", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) { DrawAssetDump(); })
                  .SearchTerms("dump extract textures assets classes folder gdx-extract"));

    AddWidget("Workshop", "Content", SECTION_COLUMN_1,
              WidgetInfo{ .name = "DD Save (64DD sidecar)", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Workshop", "Content", SECTION_COLUMN_1,
              WidgetInfo{ .name = "DD Save", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) { DrawDdSave(); })
                  .SearchTerms("64dd disk save sidecar mfs format journal course edit machine create"));

    AddWidget("Workshop", "Content", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Content Installs", .type = GdxUI::WIDGET_SEPARATOR_TEXT });
    AddWidget("Workshop", "Content", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Track / cup / machine install (blocked: disk write-through)",
                          .type = GdxUI::WIDGET_COMING_SOON });
    AddWidget("Workshop", "Content", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Installed-content library + quota manager", .type = GdxUI::WIDGET_COMING_SOON });

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // ONLINE -> Overview — all future / parity-blocked; netplay additionally decision-gated.
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Online", "Overview", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Leaderboards (per course)", .type = GdxUI::WIDGET_COMING_SOON });
    AddWidget("Online", "Overview", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Ghost upload / download", .type = GdxUI::WIDGET_COMING_SOON });
    AddWidget("Online", "Overview", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Netplay lobbies (after decision gate)", .type = GdxUI::WIDGET_COMING_SOON });
    AddWidget("Online", "Overview", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Spectator / director cam", .type = GdxUI::WIDGET_COMING_SOON });

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // DEV TOOLS -> General
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Dev Tools", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Developer tools can be embedded in this menu or popped out into "
                                  "independent windows.",
                          .type = GdxUI::WIDGET_TEXT });
    AddWidget("Dev Tools", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "A popped-out window opens on top of this menu; close the menu (F1) to use it.",
                          .type = GdxUI::WIDGET_TEXT_DISABLED });
    AddWidget("Dev Tools", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Developer tool windows", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) { DrawDevToolButtons(); })
                  .SearchTerms("stats console gfx debugger open window popout"));

    AddWidget("Dev Tools", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Windowing", .type = GdxUI::WIDGET_SEPARATOR_TEXT });

    // Multi-viewport — CVar gEnableMultiViewports, default 1. The ImGui viewport flag is applied
    // ONCE at Gui::Init(), so flipping the CVar at runtime persists the preference but only takes
    // effect after a restart (we deliberately do not poke ImGui::GetIO() here). Hence the note.
    AddWidget("Dev Tools", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Multi-viewport docking", .cVar = "gEnableMultiViewports",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Lets popped-out tool windows leave the main window (multi-monitor docking).\n"
                      "Applies on restart."))
                  .Note("(restart)")
                  .SearchTerms("docking monitor viewport window detach"));

    AddWidget("Dev Tools", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Shader cache", .type = GdxUI::WIDGET_SEPARATOR_TEXT });

    // Escape hatch, not a preference — hence Dev Tools rather than Enhancements. The cache is
    // read once at renderer init (gfx_direct3d11.cpp / gfx_opengl.cpp Init), so the CVar cannot
    // take effect mid-session; GDX_SHADER_CACHE=0 does the same for a single run without
    // persisting.
    AddWidget("Dev Tools", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Reuse compiled shaders across runs", .cVar = "gDevTools.ShaderCache",
                          .type = GdxUI::WIDGET_CVAR_CHECKBOX }
                  .Options(UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip(
                      "Stores compiled shaders next to the executable so each one is built once per\n"
                      "install instead of once per launch. Building them costs 9-15ms each and they\n"
                      "arrive in bursts, which is what produced the large frame stalls on first\n"
                      "visiting a venue.\n\n"
                      "Turn this off only to rule the cache out while diagnosing a rendering fault.\n"
                      "To rebuild it from scratch, delete gdiffuser-shadercache-*.bin.\n"
                      "Applies on restart."))
                  .Note("(restart)")
                  .SearchTerms("shader cache stutter hitching compile pipeline"));

    AddWidget("Dev Tools", "General", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Developer gates", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) { DrawDevGates(); })
                  .SearchTerms("gdx environment variables logging diagnostics behavior overrides gates env"));

    // ═════════════════════════════════════════════════════════════════════════════════════════
    // DEV TOOLS -> Stats / Console / Gfx Debugger
    // ═════════════════════════════════════════════════════════════════════════════════════════
    AddWidget("Dev Tools", "Stats", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Show FPS counter overlay", .type = GdxUI::WIDGET_CHECKBOX }
                  .ValuePointer(&mFpsCounterVisible)
                  .Options(UIWidgets::CheckboxOptions().Tooltip(
                      "Toggles a small always-on-top frames-per-second overlay."))
                  .PreFunc([this](WidgetInfo&) { mFpsCounterVisible = GdxWindowVisible("FPS Counter"); })
                  .Callback([](WidgetInfo&) { GdxToggleWindow("FPS Counter"); })
                  .Note("Uses the same frame metrics as Stats")
                  .SearchTerms("fps counter framerate overlay"));
    AddWidget("Dev Tools", "Stats", SECTION_COLUMN_1, WidgetInfo{ .type = GdxUI::WIDGET_SEPARATOR });
    AddWidget("Dev Tools", "Stats", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Frame interpolation statistics", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) { DrawInterpStats(); })
                  .SearchTerms("presented fps subframes interpolated snapped sim"));
    AddWidget("Dev Tools", "Stats", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Stats window", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) {
                      DrawToolWindowPage("Stats", "Live frame timing and renderer statistics.");
                  })
                  .SearchTerms("frame timing renderer statistics"));

    AddWidget("Dev Tools", "Console", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Console", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) {
                      DrawToolWindowPage("Console", "Port log, developer console and command history.");
                  })
                  .SearchTerms("log command history reset"));

    AddWidget("Dev Tools", "Gfx Debugger", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Gfx Debugger", .type = GdxUI::WIDGET_CUSTOM }
                  .CustomFunction([this](WidgetInfo&) {
                      DrawToolWindowPage("Gfx Debugger",
                                         "Inspect Fast3D display-list execution and rendering state.");
                  })
                  .SearchTerms("display list fast3d rendering debug"));
}
