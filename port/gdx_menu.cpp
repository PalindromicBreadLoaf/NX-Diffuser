// port/gdx_menu.cpp — implementation of the G-Diffuser modern full-screen menu.
//
// See gdx_menu.h for the high-level design. This file is pure port-side wiring against LUS's
// public ImGui + CVar API. All feature controls retain their original CVar names, defaults, and
// callbacks; this file changes their presentation and information architecture only.
//
// WHAT LIVES WHERE
// ----------------
// This file is the SHELL and the draw dispatcher:
//   - the window, header tab strip, sidebar, content pane, search box and quit modal;
//   - MenuDrawItem(), which turns one registered GdxUI::WidgetInfo into ImGui calls;
//   - DrawSearchResults(), which walks the same registry to find INDIVIDUAL controls;
//   - the WIDGET_CUSTOM blocks (status read-outs, tables, modals) that no generic widget can
//     express.
// The menu's CONTENTS — every section, page and control — are declared as data in
// port/gdx_menu_registry.cpp against the model in port/ui/MenuTypes.h. Adding a control means
// adding one AddWidget() entry there; it then appears on its page AND in search, with its disable
// reasons and tooltip, with no edit to this file.
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
#include "gdx_menu_internal.h" // the helpers below, shared with port/gdx_menu_registry.cpp

#include <imgui.h> // vendored in libultraship's imgui; already on the port target's include path
                   // (main.cpp already pulls it transitively via GuiWindow.h). Mirrors the
                   // <imgui.h> include used across LUS (e.g. GuiWindow.h:4).

#include "ship/Context.h"           // Ship::Context::GetInstance()
#include "ship/window/Window.h"     // Ship::Window::GetGui() + the SetResolutionMultiplier/
                                    // SetMsaaLevel virtuals used to apply the graphics knobs live
#include "ship/window/gui/Gui.h"    // Ship::Gui::{GetGuiWindow, SaveConsoleVariablesNextFrame}
#include "ship/window/gui/IconsFontAwesome4.h"
#include "fast/Fast3dWindow.h"      // Fast::Fast3dWindow::SetTextureFilter + Fast::FilteringMode
#include "fast/Fast3dGui.h"         // About page: LoadTextureFromRawImage + GetTextureByName (logo)
                                    // (the texture-filter setter is Fast3d-only, not on the base
                                    // Ship::Window, so it needs a downcast — see DrawGraphicsMenu)
#include "ship/resource/ResourceManager.h"       // Data & Files: ResourceManager::GetArchiveManager
#include "ship/resource/archive/ArchiveManager.h" // Data & Files: ArchiveManager::GetArchives
#include "ship/resource/archive/Archive.h"        // Data & Files: Archive::GetPath (basename match)

#include "libultraship/bridge/consolevariablebridge.h" // CVarGet/Set/Register*
#include "libultraship/bridge/audiobridge.h"           // AudioPlayerBuffered (Audio tab status line)

#include <cstring> // strcmp (Audio tab: SDL driver-name check)

// SDL audio driver name for the Audio tab's output-status line. Declared here rather than
// pulling in <SDL2/SDL.h> (this TU builds inside libultraship's include environment where the
// SDL umbrella clashes); the signature matches SDL_audio.h exactly.
extern "C" const char* SDL_GetCurrentAudioDriver(void);

#include <algorithm>
#include <cctype>
#include <cmath>  // std::sin (the search-navigation highlight pulse in MenuDrawItem)
#include <cstdio> // snprintf (Practice-tab ghost import/export status line)
#include <cstdlib> // std::system (non-Windows open-folder fallback)
#include <memory> // std::dynamic_pointer_cast (null-safe downcast to Fast::Fast3dWindow)
#include <string>

#include "ui/UIWidgets.hpp" // CVar-bound ImGui widgets: read + draw + write + persist + tooltip in
                           // one call, replacing the hand-written CVarGet/CVarSet/GdxSaveCvars/
                           // SetTooltip quadruple this file used to spell out per option. port/ is
                           // already an include dir on this target, so the "ui/" prefix needs no
                           // CMake change (port/CMakeLists.txt:325-329).

#include "gdx_console_log.h" // Console page: drains the queued port-log lines into the LUS console
#include "gdx_ghost_io.h" // .gdg ghost import/export C API (Practice tab Export / Import buttons)
#include "gdx_gui.h"
#include "gdx_workshop.h"    // Workshop tab: texture-pack listing, override count, reload, dump dir
#include "gdx_dump_launch.h" // Workshop tab "Asset Dump" section: per-class offline dump launcher
#include "disk_savefile.h"   // Workshop tab "DD Save" subsection: sidecar status + one-shot format
#include "rom_buffer.h"      // Data & Files: gdx_rom_buffer/gdx_rom_path (live ROM residency signal)
#include "gdx_firstboot.h"   // Data & Files: canonical file names + gdx::ManagedDiskPath
#include "gdx_segment_source.h" // Data & Files: archive-coverage telemetry (fallback counters)
#include "gdx_dev_gates.h"   // Dev Tools: the developer-gate table driving DrawDevGates()

#include <vector>
#include <filesystem>

// From port/input_bridge.c: nonzero while an on-track race is live. The ghost Import writes to the
// SRAM ghost slot, which must not race the game fiber, so the Import button is disabled in-race.
extern "C" int gdx_input_in_gameplay(void);
extern "C" void gdx_game_request_reset(void);
// Deletion-gate verdict (port/disk_buffer.cpp). 1 iff this boot reconstructed the EK disk from
// fzerox-disk.o2r AND proved it byte-identical to the managed copy. The Data & Files panel
// offers disk deletion ONLY on a passed verdict; it never deletes anything itself.
extern "C" int gdx_disk_archive_verified(void);

// From port/n64_gfx_bridge.cpp: frame-interpolation telemetry for the Graphics tab's
// "subframes last tick" status line. Declared here rather than pulling in n64_gfx_bridge.h (this
// TU doesn't otherwise need the gfx bridge's internals) — signatures match the header exactly.
extern "C" int gdx_gfx_interp_last_subframes(void);
extern "C" double gdx_gfx_interp_last_t(void);
// Real-FPS visibility: true presented frames/sec, the live master
// toggle state, and last-tick per-slot tween/snap counts — shown on the Stats page.
extern "C" int gdx_gfx_interp_host_active(void);
extern "C" double gdx_gfx_interp_presents_per_sec(void);
extern "C" int gdx_gfx_interp_last_lerped(void);
extern "C" int gdx_gfx_interp_last_snapped(void);
// Per-tick truth (n64_gfx_bridge.cpp): main.cpp forces interpolation off THIS tick (Course Edit /
// Create Machine editors) even while the raw CVar above is still on. The Stats block below must
// show the paused truth in that case rather than the (stale) live numbers.
extern "C" int gdx_gfx_interp_tick_active(void);

// ─────────────────────────────────────────────────────────────────────────────────────────────
// Small helpers (main-thread only — the whole menu draws inside Gui::StartDraw/EndDraw).
//
// These used to live in an anonymous namespace. They are now a named one, declared in
// port/gdx_menu_internal.h, because port/gdx_menu_registry.cpp needs them too: the registry's
// callbacks are what apply the live side effects (SetResolutionMultiplier, SetFullscreen,
// ToggleVisibility, the CVar flush), and duplicating a second copy of each in that TU is exactly
// how two copies drift apart. The `using namespace` below keeps every call site in this file
// spelling them unqualified, as before.
// ─────────────────────────────────────────────────────────────────────────────────────────────

namespace gdxmenu {

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
// Returns false when the named window does not exist, so callers can say so instead of leaving a
// button that silently does nothing.
bool GdxToggleWindow(const char* name) {
    auto gui = GdxGui();
    if (gui == nullptr) {
        return false;
    }
    auto window = gui->GetGuiWindow(name);
    if (window == nullptr) {
        return false;
    }
    window->ToggleVisibility();

    // Raise it. Gui::Draw() draws the full-screen menu BEFORE the tool windows, and libultraship
    // registers the Console with ImGuiWindowFlags_NoFocusOnAppearing, so a window turned on from
    // inside the menu appears UNFOCUSED and therefore behind the menu that opened it. The window is
    // genuinely open and genuinely drawing -- it is just underneath an opaque full-screen panel,
    // which is indistinguishable from a dead button. Focusing it on the frame it becomes visible
    // puts it on top; on the way back to hidden there is nothing to focus.
    if (window->IsVisible()) {
        ImGui::SetWindowFocus(name);
    }
    return true;
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

// Marks the item just submitted as differing from its stock default: a subtle accent asterisk
// drawn inline to the right, with an explanatory tooltip (the SoH "modified" cue). No-op when the
// value is unchanged. Call immediately after a widget (and after its own hover tooltip, if any) so
// the SameLine anchors to that widget. Purely presentational — reads no state of its own.
void GdxModifiedMarker(bool changed) {
    if (!changed) {
        return;
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.63f, 0.76f, 1.0f, 1.0f), "*");
    UIWidgets::Tooltip("Changed from the default (stock) value.");
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

// ─────────────────────────────────────────────────────────────────────────────────────────────
// "Data & Files" (General tab): live on-disk state for the three original setup inputs. Every
// line below is backed by a live, cheaply-rechecked signal rather than static copy -- see the
// per-row comments for exactly what is and is not determinable from this file. gdx_menu.cpp
// cannot reach disk_buffer.cpp's IPL/EK disk load state, so it reports filesystem +
// archive-mount facts instead of guessing which source was actually read.
// ─────────────────────────────────────────────────────────────────────────────────────────────

// True if any mounted archive's filename (case-insensitive) matches `basename` exactly. Mirrors
// main.cpp's own basename-match pattern (main.cpp:427-431) for the version-gate scan.
bool GdxArchiveMounted(const char* basename) {
    auto ctx = Ship::Context::GetInstance();
    auto rm = (ctx != nullptr) ? ctx->GetResourceManager() : nullptr;
    auto am = (rm != nullptr) ? rm->GetArchiveManager() : nullptr;
    auto archives = (am != nullptr) ? am->GetArchives() : nullptr;
    if (archives == nullptr) {
        return false;
    }
    const std::string want = GdxLowercase(basename);
    for (const auto& archive : *archives) {
        if (archive == nullptr) {
            continue;
        }
        std::string name = GdxLowercase(std::filesystem::path(archive->GetPath()).filename().string());
        if (name == want) {
            return true;
        }
    }
    return false;
}

void GdxDrawDataAndFilesPanel() {
    if (!ImGui::CollapsingHeader("Data & Files")) {
        return; // collapsed by default -- this is diagnostic detail, not a control most players need
    }
    ImGui::TextWrapped(
        "What G-Diffuser currently has on disk for the three original setup inputs, and which of "
        "them are safe to delete once setup has completed.");
    ImGui::Spacing();

    // G-Diffuser is always portable and the current working directory is set to the data directory
    // at boot (gdx_firstboot.cpp's FirstBootRun chdir; see also gdx_workshop.cpp's exeDir() helper,
    // which relies on the same fact). A failed query degrades to "." -- still correct in practice
    // since every canonical name below is looked up relative to the CWD either way.
    std::error_code dirEc;
    std::filesystem::path dataDir = std::filesystem::current_path(dirEc);
    if (dirEc) {
        dataDir = std::filesystem::path(".");
    }

    const ImVec4 kGdxOk = ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
    const ImVec4 kGdxWarn = ImVec4(0.90f, 0.70f, 0.30f, 1.0f);
    const ImVec4 kGdxBad = ImVec4(1.0f, 0.40f, 0.40f, 1.0f);

    // ── F-Zero X ROM ─────────────────────────────────────────────────────────────────────────
    // Live signal: gdx_rom_buffer (rom_buffer.h). Non-null means the port genuinely has the raw
    // cartridge image resident in memory THIS session -- the one row here with a true "in use"
    // fact rather than an inference, because rom_buffer.cpp exports the pointer directly.
    ImGui::SeparatorText("F-Zero X ROM (.z64)");
    if (gdx_rom_buffer != nullptr) {
        ImGui::TextColored(kGdxWarn, "In use from: %s", gdx_rom_path[0] != '\0' ? gdx_rom_path : "(unknown path)");
    } else {
        ImGui::TextColored(kGdxOk, "Not loaded -- served from the game archive this session.");
    }
    ImGui::TextDisabled(
        "Deletable once setup has completed and Archive coverage below reads 0 fallbacks across a "
        "full play session.");

    // ── 64DD IPL ROM ─────────────────────────────────────────────────────────────────────────
    // No live "which source is loaded" getter is exposed outside disk_buffer.cpp, so this reports
    // the two independently-checkable facts instead of guessing: whether the
    // original file is present next to the game, and whether the dedicated archive is mounted.
    ImGui::SeparatorText("64DD IPL ROM (N64DDIPLROM.n64)");
    {
        std::error_code ec;
        bool iplFilePresent =
            std::filesystem::is_regular_file(dataDir / gdx::SetupIplFileName(), ec);
        ec.clear();
        bool iplArchiveMounted = GdxArchiveMounted("n64ddipl.o2r");
        ImGui::Text("Original file: %s", iplFilePresent ? "present" : "not present");
        ImGui::Text("Archive (n64ddipl.o2r): %s", iplArchiveMounted ? "mounted" : "not mounted");
        if (!iplFilePresent && iplArchiveMounted) {
            ImGui::TextColored(kGdxOk, "Served from the archive.");
        } else if (!iplFilePresent && !iplArchiveMounted) {
            ImGui::TextColored(kGdxBad, "Neither the file nor the archive is present -- do not delete anything here.");
        }
    }
    ImGui::TextDisabled("Deletable once setup has completed (the port never reads the IPL file again after that).");

    // ── Expansion Kit disk ───────────────────────────────────────────────────────────────────
    // Reports presence of the original, of the managed copy (gdx::ManagedDiskPath), and of the
    // disk archive, plus the boot-time deletion-gate verdict. The deletable line is shown ONLY on a
    // passed verdict (gdx_disk_archive_verified) -- otherwise the panel never suggests deletion.
    ImGui::SeparatorText("Expansion Kit disk (.ndd)");
    {
        std::error_code ec;
        bool diskFilePresent =
            std::filesystem::is_regular_file(dataDir / gdx::SetupDiskFileName(), ec);
        ec.clear();
        std::string managedPath = gdx::ManagedDiskPath(dataDir.string());
        bool managedPresent = !managedPath.empty() && std::filesystem::is_regular_file(managedPath, ec);
        ec.clear();
        bool diskArchiveMounted = GdxArchiveMounted("fzerox-disk.o2r");
        bool archiveVerified = gdx_disk_archive_verified() != 0;
        ImGui::Text("Original file: %s", diskFilePresent ? "present" : "not present");
        ImGui::Text("Managed copy (media/): %s", managedPresent ? "present" : "not present");
        ImGui::Text("Archive (fzerox-disk.o2r): %s", diskArchiveMounted ? "mounted" : "not mounted");
        if (archiveVerified) {
            // Hard gate passed: this boot reconstructed the disk from the archive AND proved it
            // byte-identical to the managed copy. Only here may the panel state that the disk is deletable.
            ImGui::TextColored(kGdxOk,
                               "Disk archive: verified byte-identical -- original and managed copy deletable.");
        } else if (diskArchiveMounted && !diskFilePresent && managedPresent) {
            ImGui::TextColored(kGdxOk, "Served from the archive or managed copy.");
        } else if (!diskFilePresent && managedPresent) {
            ImGui::TextColored(kGdxOk, "Served from the managed copy.");
        } else if (!managedPresent && !diskArchiveMounted) {
            ImGui::TextColored(kGdxBad, "No managed copy or archive yet -- do NOT delete the original disk.");
        }
    }
    ImGui::TextDisabled(
        "The original .ndd and the managed copy are deletable ONLY after the row above reads "
        "\"verified byte-identical\" (a boot reconstructed the disk from the archive and proved it "
        "matches). Your saves live in saves/*.gdd -- back those up regardless.");

    // ── Archive coverage (gdx_segment_source_fallback_total / FamilyStats) ─────────────────────
    ImGui::Spacing();
    ImGui::SeparatorText("Archive coverage");
    unsigned int fallbackTotal = gdx_segment_source_fallback_total();
    if (fallbackTotal == 0) {
        ImGui::TextColored(kGdxOk, "Raw-ROM fallback reads this session: 0");
    } else {
        ImGui::TextColored(kGdxWarn, "Raw-ROM fallback reads this session: %u", fallbackTotal);
        ImFont* monoFont = GdxGuiFontMono(); // gdx_gui.h -- optional bundled mono font, null-safe
        if (monoFont != nullptr) {
            ImGui::PushFont(monoFont);
        }
        for (unsigned int i = 0;; ++i) {
            const char* key = nullptr;
            unsigned int count = 0;
            if (!GdxSegmentSourceFamilyStats(i, &key, &count)) {
                break;
            }
            if (count > 0) {
                ImGui::TextUnformatted((std::string("  ") + (key != nullptr ? key : "?") + ": " +
                                        std::to_string(count)).c_str());
            }
        }
        if (monoFont != nullptr) {
            ImGui::PopFont();
        }
    }
    ImGui::TextDisabled("Run with GDX_STRICT_ARCHIVE=1 during a soak to log every raw-ROM fallback.");
}

} // namespace gdxmenu

using namespace gdxmenu;

// ─────────────────────────────────────────────────────────────────────────────────────────────
// Construction — pin visibility CVar + register the port's gEnhancements.* CVars at 1:1 defaults.
// ─────────────────────────────────────────────────────────────────────────────────────────────

// Base ctor: (visibilityConsoleVariable, isVisible). "gOpenMenuBar" is the compatibility CVar the
// LUS F1 / Esc / Gamepad-Back toggle flips, so binding to it makes those keys open this menu. Start
// hidden (isVisible=false) — the menu is opt-in via F1.
GdxMenu::GdxMenu() : Ship::GuiWindow("gOpenMenuBar", false, "G-Diffuser Menu") {
    CVarRegisterFloat("gSettings.Menu.BackgroundOpacity", 0.85f);
    // Section and per-section sidebar selection are persisted BY NAME (see gdx_menu.h). The
    // integer gSettings.Menu.ActiveHeader / ActivePage pair this replaces stored positions in an
    // enum, so any page reorder silently re-pointed a stored selection at a different page — which
    // is what the kMenuLayoutVersion reset existed to paper over. A name cannot drift; at worst it
    // names a page that no longer exists, which falls back to the section's first page.
    CVarRegisterString("gSettings.Menu.ActiveSection", "Settings");
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

    // GRAPHICS tab enhancement CVars (port-owned; distinct from the LUS-owned g* CVars used in
    // DrawGraphicsMenu). Every default reproduces today's rendering (the optionality constitution):
    //   gEnhancements.Graphics.Widescreen         = 1   -> today's always-on aspect correction. Read
    //                                                      live in interpreter.cpp AdjXForAspectRatio;
    //                                                      1 = current 16:9 hor+ (byte-identical),
    //                                                      0 = 4:3 pillarbox.
    //   gEnhancements.Graphics.WidescreenUI       = 1   -> true-corner 1P HUD + selected full-width
    //                                                      2D scopes. 0 = stock proportional 4:3 UI
    //                                                      placement. See the default note below.
    //   gEnhancements.Graphics.DrawDistance       = 100 -> per-venue far-render scale in %. 100 = stock
    //                                                      (1.0x, bit-exact). course.c Course_Draw,
    //                                                      clamped 100..300.
    //   gEnhancements.Graphics.ForceMaxMachineLOD = 0   -> 0 = stock distance-based machine LOD; 1 pins
    //                                                      the highest-detail model. racer.c Racer_Draw.
    CVarRegisterInteger("gEnhancements.Graphics.Widescreen", 1);
    // 2D widescreen layout: true-corner 1P HUD, full-width SELECT MACHINE blue background, and
    // full-width race transitions. Other menu artwork remains 4:3.
    //
    // DEFAULT ON, and this is the second deliberate exception to the "every default reproduces
    // stock" rule above (Widescreen itself is the first). Shipping 3D widescreen ON while the HUD
    // stays proportional 4:3 is the jarring half-state: the world fills the screen and the HUD
    // floats inboard of the corners it belongs in, which reads as a defect rather than a choice.
    // The two switches describe one feature, so they ship together.
    CVarRegisterInteger("gEnhancements.Graphics.WidescreenUI", 1);
    // One-time migration: this registered (and therefore persisted) as 0 before the default
    // flipped, so existing configs pin the old value and would never see the new default. Same
    // marker pattern as gdx.Migrations.ControlNavDefaultOn / ReduceEditorFlashingOn below; a later
    // deliberate OFF stays untouched because the marker is only ever written once.
    if (CVarGetInteger("gdx.Migrations.WidescreenUiDefaultOn", 0) == 0) {
        CVarSetInteger("gdx.Migrations.WidescreenUiDefaultOn", 1);
        CVarSetInteger("gEnhancements.Graphics.WidescreenUI", 1);
        CVarSave();
    }
    // Split-screen HUD anchoring (2P/3P/4P). Consumed by gdx_widescreen_split_ui_active()
    // (port/input_bridge.c), which requires gdx_widescreen_ui_active() as well — this is a strict
    // subset, not an independent feature, and it is inert while the switch above is off.
    //
    // Its own switch rather than an overload of WidescreenUI: the split-screen HUD is authored to a
    // per-column grid rather than to screen edges, so a handful of mid-column elements (interval,
    // reverse, the 3P spare minimap) stay on the stock centred path by design. That is a judgment
    // call about layout, and a judgment call deserves its own opt-out.
    CVarRegisterInteger("gEnhancements.Graphics.WidescreenSplitUI", 1);
    CVarRegisterInteger("gEnhancements.Graphics.DrawDistance", 100);
    CVarRegisterInteger("gEnhancements.Graphics.ForceMaxMachineLOD", 0);
    //   gEnhancements.Graphics.FramePacing = 0 -> off = stock. libultraship's Fast3D backend already
    //                                             limits the loop to ~60fps; when on, port/gdx_frame_
    //                                             pacer.c pins it to the N64 NTSC rate (~59.94Hz).
    //                                             Recommend VSync OFF when enabled (avoids beating).
    CVarRegisterInteger("gEnhancements.Graphics.FramePacing", 0);
    //   gEnhancements.Graphics.FrameInterpolation = 0 -> off = stock single-pass render. EXPERIMENTAL:
    //                                             when on, port/n64_gfx_bridge.cpp + port/gdx_interp.cpp
    //                                             tween M sub-frame presents per 60 Hz logic tick
    //                                             (dual-pool matrix lerp) for smoother motion on
    //                                             high-refresh displays. Read LIVE (gdx_interp::
    //                                             P2HostActive), same idiom as FramePacing above; the
    //                                             two are mutually exclusive pacing owners.
    CVarRegisterInteger("gEnhancements.Graphics.FrameInterpolation", 0);
    //   gEnhancements.Graphics.InterpDebugOverlay = 0 -> off = no overlay (default). When on AND
    //                                             FrameInterpolation is on, shows the live
    //                                             "subframes last tick" stat line below the toggle.
    //                                             Purely diagnostic; no effect on rendering/pacing.
    CVarRegisterInteger("gEnhancements.Graphics.InterpDebugOverlay", 0);
    //   gEnhancements.Graphics.InterpTargetMode = 0 -> Match Refresh Rate (default): the sub-frame
    //                                             target follows the display's current refresh rate,
    //                                             read live via Ship::Window::GetCurrentRefreshRate().
    //                                             1 -> Capped: the target is InterpTargetFps below
    //                                             instead. Only consulted while FrameInterpolation is
    //                                             on; read LIVE by main.cpp's per-tick M derivation
    //                                             (M = clamp(ceil(target/60), 1, kGdxInterpMaxSubframes)).
    //   gEnhancements.Graphics.InterpTargetFps  = 120 -> Capped-mode target frame rate, UI range
    //                                             60..480. Values above the display's own refresh
    //                                             rate waste GPU without improving output.
    CVarRegisterInteger("gEnhancements.Graphics.InterpTargetMode", 0);
    CVarRegisterInteger("gEnhancements.Graphics.InterpTargetFps", 120);
    //   gEnhancements.Graphics.InterpolateCamera = 1 -> ON. Also interpolates G_MTX_PROJECTION pool
    //                                             matrices. Only consulted while FrameInterpolation
    //                                             is on, so the default cannot alter stock behavior;
    //                                             it selects what interpolation MEANS once enabled.
    //                                             race.c loads the combined projection*view camera
    //                                             with G_MTX_PROJECTION and course.c emits no
    //                                             gSPMatrix at all, so with this off the camera AND
    //                                             the entire track stay at 60 Hz while machines
    //                                             interpolate -- objects smoothed against a static
    //                                             world. Off is offered only as an escape hatch.
    CVarRegisterInteger("gEnhancements.Graphics.InterpolateCamera", 1);

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

    // Workshop (texture packs + dump). Every knob defaults OFF/empty per the optionality
    // constitution: a fresh config mounts no override behavior and renders bit-identically to stock.
    //   gEnhancements.Workshop.TexturePacks = 0 -> off = stock rendering. When on, the Tier-B shim
    //                                              (n64_gfx_bridge.cpp) rewrites a common-asset load
    //                                              to a mounted pack's "textures/pack/<key>" resource.
    CVarRegisterInteger("gEnhancements.Workshop.TexturePacks", 0);
    //   gEnhancements.Workshop.TextureDump — RETIRED. Asset Dump supersedes it: that path decodes
    //   named assets straight from the archive instead of waiting for gameplay to walk past each
    //   texture, so it is both complete and reproducible. The runtime hook in gdx_workshop.cpp still
    //   reads this CVar, so it is forced to 0 here rather than merely un-registered -- a user who had
    //   it ON would otherwise keep dumping forever with no surviving checkbox to turn it off.
    CVarSetInteger("gEnhancements.Workshop.TextureDump", 0);
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
    // Build the registry once. InitElement runs on the ImGui thread after the Gui exists, which is
    // what the CVar reads inside the registration (defaults, combo lists) and the disable-reason
    // evaluations both assume.
    if (!mRegistered) {
        mRegistered = true;
        RegisterDisableReasons();
        RegisterMenu();
    }

    // Restore the last section by NAME, falling back to the first registered section when the
    // stored name is unknown (a renamed or removed tab) rather than to a stale index.
    const std::string storedSection = CVarGetString("gSettings.Menu.ActiveSection", "Settings");
    if (mMenuEntries.count(storedSection) != 0) {
        mActiveSection = storedSection;
    } else if (!mMenuOrder.empty()) {
        mActiveSection = mMenuOrder.front();
    }
}

void GdxMenu::UpdateElement() {
    // Gui::DrawMenu calls this before it draws the registered windows, and always on the ImGui
    // thread — the one place the queued log lines can safely reach the Console window.
    GdxConsoleLogDrain();
}

void GdxMenu::Draw() {
    if (!IsVisible()) {
        // Menu just closed (or was never open this frame): undo any nav tuning we applied and clear
        // the open-transition latches so focus is re-seeded the next time the menu opens.
        RestoreNavTuning();
        mMenuWasVisible = false;
        mNavCancelHadTarget = false;
        return;
    }
    DrawElement();
    SyncVisibilityConsoleVariable();
}

void GdxMenu::RestoreNavTuning() {
    if (!mNavTuningApplied) {
        return;
    }
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGuiIO& io = ImGui::GetIO();
        io.KeyRepeatDelay = mSavedKeyRepeatDelay;
        io.KeyRepeatRate = mSavedKeyRepeatRate;
        io.ConfigNavCursorVisibleAlways = mSavedNavCursorAlways;
    }
    mNavTuningApplied = false;
}

void GdxMenu::DrawElement() {
    // Defensive: the registry is built in InitElement(), which libultraship calls before any Draw.
    // Building it here too costs one bool test per frame and removes the possibility of drawing an
    // empty menu (or indexing an empty mDisabledInfo) if that ordering ever changes.
    if (!mRegistered) {
        InitElement();
    }

    // ONCE PER FRAME, before anything draws. Every disable/hide reason is evaluated here and the
    // answer cached in DisabledInfo::active; MenuDrawItem only ever reads the cached bool. This is
    // the whole reason DisabledInfo exists (port/ui/MenuTypes.h): several controls share the same
    // condition, and without the cache each would re-read the same CVar (or re-query the window)
    // once per frame per widget.
    for (GdxUI::DisabledInfo& info : mDisabledInfo) {
        if (info.evaluation != nullptr) {
            info.active = info.evaluation(info);
        }
    }

    // Consume a pending "jump to this control" request from the search results. Deferred to the top
    // of the next frame rather than applied inside DrawSearchResults, because that runs mid-layout:
    // switching section and sidebar there would tear down the child window the result button was
    // just submitted into.
    if (mNavigateRequested) {
        mNavigateRequested = false;
        auto section = mMenuEntries.find(mNavigateSection);
        if (section != mMenuEntries.end() && section->second.sidebars.count(mNavigateSidebar) != 0) {
            mSearch[0] = '\0';
            mActiveSection = mNavigateSection;
            CVarSetString("gSettings.Menu.ActiveSection", mActiveSection.c_str());
            CVarSetString(section->second.sidebarCvar, mNavigateSidebar.c_str());
            GdxSaveCvars();
            // Arm the highlight: MenuDrawItem outlines the named control and scrolls it into view
            // for the next few seconds. Without this, "navigate" drops you on a page of thirty
            // controls with no indication which one you were looking for.
            mHighlightWidget = mNavigateWidget;
            mHighlightUntil = ImGui::GetTime() + 3.0;
            mHighlightScrollPending = true;
        }
    }

    const bool navActive = CVarGetInteger("gControlNav", 0) != 0;

    // On each open, seed nav focus onto the active sidebar page (consumed in DrawSidebar). Only when
    // gamepad nav is on, so mouse/keyboard users are not force-focused away from the search box.
    if (!mMenuWasVisible) {
        mMenuWasVisible = true;
        mFocusSidebar = navActive;
    }

    // Nav feel, applied only while THIS menu is open with gamepad nav on and restored on close
    // (Draw) or when the user turns nav off. Tune here if it still feels off.
    //
    // ImGui derives its repeat rates from io.KeyRepeat*: nav moves at Delay*0.72 / Rate*0.80, slider
    // tweaks at Delay*0.72 / Rate*0.30. The stock 0.275/0.050 gives 25 selection moves per second,
    // which overshoots badly on a pad; 0.40/0.105 lands at ~12 moves/s while still tweaking a slider
    // at ~32 steps/s (and L1/R1 remain the fine/coarse modifiers).
    //
    // ConfigNavCursorVisibleAlways is what makes the focus rectangle actually appear: both of the
    // ways this menu parks focus hide it. SetKeyboardFocusHere passes NoSetNavCursorVisible, and
    // SetFocusID hides the cursor outright unless the last activation came from a pad — so the menu
    // opened with a real NavId and nothing drawn, and the first press looked like it did nothing.
    if (navActive && !mNavTuningApplied) {
        ImGuiIO& io = ImGui::GetIO();
        mSavedKeyRepeatDelay = io.KeyRepeatDelay;
        mSavedKeyRepeatRate = io.KeyRepeatRate;
        mSavedNavCursorAlways = io.ConfigNavCursorVisibleAlways;
        io.KeyRepeatDelay = 0.40f;
        io.KeyRepeatRate = 0.105f;
        io.ConfigNavCursorVisibleAlways = true;
        mNavTuningApplied = true;
    } else if (!navActive && mNavTuningApplied) {
        RestoreNavTuning();
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
                ImGui::TextUnformatted(mSearch[0] != '\0' ? "Search Results" : ActiveSidebar().c_str());
                if (GdxGuiFontLarge() != nullptr) {
                    ImGui::PopFont();
                }
                ImGui::Separator();
                DrawCurrentPage();

                // Bring a search-navigated control into view. Done here, in the scrolling content
                // pane and after the page has laid out, because the control itself may have been
                // drawn inside a non-scrolling column child. Screen-space Y is converted back to
                // this window's scroll space, then centred.
                if (mHighlightScrollPending && mHighlightScreenY != 0.0f) {
                    const float local = mHighlightScreenY - ImGui::GetWindowPos().y + ImGui::GetScrollY();
                    ImGui::SetScrollY(local - ImGui::GetWindowHeight() * 0.5f);
                    mHighlightScrollPending = false;
                    mHighlightScreenY = 0.0f;
                }
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();

        DrawQuitModal();

        // B / Circle = "back": close the menu, but only when ImGui had nothing of its own to cancel.
        // Testing that here is too late — NavUpdateCancelRequest ran back in NewFrame and has already
        // dropped the slider or popup the press was meant to leave, so a live check sees a clean menu
        // and one B would both back out and close. The end-of-frame snapshot below is the state ImGui
        // itself saw. Edge-triggered so a held B does not re-fire.
        const bool cancelWasConsumed = mNavCancelHadTarget;
        const bool popupOpen =
            ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        if (navActive && !cancelWasConsumed && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) &&
            !ImGui::IsAnyItemActive() && !popupOpen) {
            Hide();
        }
        mNavCancelHadTarget = ImGui::IsAnyItemActive() || popupOpen;
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
    // Tab count and labels come from the registry now (mMenuOrder), not from a hardcoded array that
    // had to be kept in step with the Header enum by hand.
    const int tabCount = static_cast<int>(mMenuOrder.size());
    if (navActive && !ImGui::IsAnyItemActive() && tabCount > 0) {
        int dir = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) dir += 1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) dir -= 1;
        if (dir != 0) {
            int cur = 0;
            for (int i = 0; i < tabCount; ++i) {
                if (mMenuOrder[i] == mActiveSection) {
                    cur = i;
                }
            }
            const int idx = (cur + dir + tabCount) % tabCount;
            mSearch[0] = '\0';
            SelectSection(mMenuOrder[idx]); // sets mFocusSidebar -> focus lands on the new tab
        }
    }

    const float height = ImGui::GetFrameHeight() + 4.0f;
    const float controlsWidth = ImGui::GetFrameHeight() * 3.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    const float searchWidth = ImGui::GetContentRegionAvail().x >= 900.0f ? 210.0f : 140.0f;

    if (ImGui::BeginTable("##ModernHeader", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX)) {
        ImGui::TableSetupColumn("Navigation", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Search", ImGuiTableColumnFlags_WidthFixed, searchWidth);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, controlsWidth);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        // Flattened like the sidebar and content children: an unflattened child is a single nav item
        // that a pad has to press A to enter and B to leave, which made the tab strip feel like a
        // dead block. Flattened, Up from the sidebar lands straight on a tab.
        if (ImGui::BeginChild("##HeaderNavigation", ImVec2(0, height), ImGuiChildFlags_NavFlattened,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            // Discoverability: flank the tab strip with shoulder-button hints when gamepad nav is on,
            // so the L1/R1 tab-cycling is visible rather than hidden.
            if (navActive) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled(ICON_FA_CHEVRON_LEFT " LB");
                // UIWidgets::Tooltip is the IsItemHovered + SetTooltip pair (UIWidgets.cpp:98), so
                // these stay attached to the item just submitted exactly as before.
                UIWidgets::Tooltip("Previous tab (L1 / LB)");
            }
            for (int i = 0; i < tabCount; ++i) {
                if (i > 0 || navActive) {
                    ImGui::SameLine();
                }
                const char* label = mMenuOrder[i].c_str();
                const ImVec2 buttonSize(ImGui::CalcTextSize(label).x + 20.0f, ImGui::GetFrameHeight());
                if (GdxNavigationButton(label, mActiveSection == mMenuOrder[i], buttonSize)) {
                    mSearch[0] = '\0';
                    SelectSection(mMenuOrder[i]);
                }
            }
            if (navActive) {
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("RB " ICON_FA_CHEVRON_RIGHT);
                UIWidgets::Tooltip("Next tab (R1 / RB)");
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
        UIWidgets::Tooltip("Quit G-Diffuser");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_UNDO "##Reset", actionSize)) {
            // The menu is already on the host/UI side of the bridge; request the reset directly.
            // Ctrl+R still uses the console command, and both converge on the same deferred flag.
            gdx_game_request_reset();
        }
        UIWidgets::Tooltip("Reset game (Ctrl+R)");
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.31f, 0.32f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.42f, 0.43f, 0.47f, 1.0f));
        if (ImGui::Button(ICON_FA_TIMES_CIRCLE "##Close", actionSize)) {
            Hide();
        }
        ImGui::PopStyleColor(2);
        UIWidgets::Tooltip("Close menu (Esc or F1)");
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

    auto sidebarButton = [&](const std::string& sidebar) {
        const bool isActive = mSearch[0] == '\0' && ActiveSidebar() == sidebar;
        if (wantFocus && isActive) {
            ImGui::SetKeyboardFocusHere();
        }
        if (GdxNavigationButton(sidebar.c_str(), isActive, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
            mSearch[0] = '\0';
            SelectSidebar(sidebar);
        }
        if (wantFocus && isActive) {
            ImGui::SetItemDefaultFocus();
        }
    };

    // The page list IS the registry's sidebarOrder for the active section. The switch over ~15
    // Page enum values this replaces had to be edited in lockstep with the enum, PageTitle() and
    // HeaderForPage(); the three could and did drift.
    auto section = mMenuEntries.find(mActiveSection);
    if (section != mMenuEntries.end()) {
        for (const std::string& sidebar : section->second.sidebarOrder) {
            sidebarButton(sidebar);
        }
    }

    // One-shot: focus request (if any) has now been submitted for this frame.
    mFocusSidebar = false;
}

// Draws the active page: its registered widgets, laid out across SidebarEntry::columnCount columns.
//
// Columns are what stop a dense page from being one long scroll. The registration decides how many
// (1-3) and which column each control belongs to; this only performs the layout, and collapses to a
// single column on a narrow content pane, where two columns would be narrower than the widgets in
// them.
void GdxMenu::DrawCurrentPage() {
    if (mSearch[0] != '\0') {
        if (DrawSearchResults() == 0) {
            ImGui::TextDisabled("No settings or tools match \"%s\".", mSearch);
        }
        return;
    }

    GdxUI::SidebarEntry* entry = ActiveSidebarEntry();
    if (entry == nullptr) {
        return;
    }

    const float available = ImGui::GetContentRegionAvail().x;
    int columns = static_cast<int>(entry->columnCount);
    if (columns < 1) {
        columns = 1;
    }
    // 420px per column is the width below which this menu's widest controls (a slider with its
    // label positioned Near, or a combobox sized to its longest entry) start truncating.
    while (columns > 1 && available / static_cast<float>(columns) < 420.0f) {
        --columns;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float columnWidth = (available - style.ItemSpacing.x * static_cast<float>(columns - 1)) /
                              static_cast<float>(columns);
    const int columnGroups = static_cast<int>(entry->columnWidgets.size());

    for (int i = 0; i < columnGroups; ++i) {
        const bool useColumns = columns > 1 && i < columns;
        if (useColumns) {
            // NavFlattened for the same reason as every other child in this menu (see DrawElement):
            // an unflattened child is one nav stop a pad must press A to enter, so the second column
            // would be unreachable by gamepad.
            ImGui::BeginChild(("##PageColumn" + std::to_string(i)).c_str(), ImVec2(columnWidth, 0.0f),
                              ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened);
        }
        for (GdxUI::WidgetInfo& widget : entry->columnWidgets[i]) {
            MenuDrawItem(widget);
        }
        if (useColumns) {
            ImGui::EndChild();
            if (i < columns - 1) {
                ImGui::SameLine();
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// WIDGET-LEVEL SEARCH.
//
// This walks the SAME registry MenuDrawItem draws from, so anything on a page is findable and
// anything findable is really on a page. What it replaces was a hand-typed table of PAGE keywords:
// searching "reverb" could at best offer you "the Audio page", and a control whose keyword nobody
// remembered to type was invisible to search entirely.
//
// Each hit draws the LIVE control (so it can be changed right there, without navigating) followed
// by a button naming where it lives — "Enhancements -> Visuals, Col 2" — which jumps the menu to
// that page and briefly outlines the control.
//
// Page-level hits are still produced, from SidebarEntry::searchTerms, which is where the old
// keyword table's terms were carried to. Nothing that used to be findable stopped being findable.
// Returns the number of results drawn.
// ─────────────────────────────────────────────────────────────────────────────────────────────
uint32_t GdxMenu::DrawSearchResults() {
    // Spaces are stripped from BOTH sides of the comparison (upstream does the same), so "lowpass"
    // finds "Low-pass"-adjacent wording and "framepacing" finds "Frame pacing".
    std::string query = GdxLowercase(mSearch);
    query.erase(std::remove(query.begin(), query.end(), ' '), query.end());
    if (query.empty()) {
        return 0;
    }

    auto normalise = [](std::string value) {
        value = GdxLowercase(std::move(value));
        value.erase(std::remove(value.begin(), value.end(), ' '), value.end());
        return value;
    };

    uint32_t matches = 0;

    // ── Page hits ────────────────────────────────────────────────────────────────────────────
    for (const std::string& sectionName : mMenuOrder) {
        GdxUI::MainMenuEntry& section = mMenuEntries.at(sectionName);
        for (const std::string& sidebarName : section.sidebarOrder) {
            const GdxUI::SidebarEntry& sidebar = section.sidebars.at(sidebarName);
            if (normalise(sectionName + " " + sidebarName + " " + sidebar.searchTerms).find(query) ==
                std::string::npos) {
                continue;
            }
            ++matches;
            ImGui::PushID(("page_" + sectionName + sidebarName).c_str());
            if (ImGui::Button(sidebarName.c_str(),
                              ImVec2((std::min)(430.0f, ImGui::GetContentRegionAvail().x), 0.0f))) {
                mSearch[0] = '\0';
                mActiveSection = sectionName;
                CVarSetString("gSettings.Menu.ActiveSection", mActiveSection.c_str());
                SelectSidebar(sidebarName);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", sectionName.c_str());
            ImGui::PopID();
        }
    }

    // ── Control hits ─────────────────────────────────────────────────────────────────────────
    for (const std::string& sectionName : mMenuOrder) {
        GdxUI::MainMenuEntry& section = mMenuEntries.at(sectionName);
        for (const std::string& sidebarName : section.sidebarOrder) {
            GdxUI::SidebarEntry& sidebar = section.sidebars.at(sidebarName);
            for (size_t col = 0; col < sidebar.columnWidgets.size(); ++col) {
                for (GdxUI::WidgetInfo& widget : sidebar.columnWidgets[col]) {
                    // Decoration carries no setting, so it is never a search result.
                    if (widget.hideInSearch || widget.type == GdxUI::WIDGET_SEPARATOR ||
                        widget.type == GdxUI::WIDGET_SEPARATOR_TEXT || widget.type == GdxUI::WIDGET_TEXT ||
                        widget.type == GdxUI::WIDGET_TEXT_DISABLED) {
                        continue;
                    }
                    // A control its own page would not show is a dead end: MenuDrawItem would draw
                    // nothing and the "go there" button would land on a page without it. The hide
                    // conditions are re-checked here rather than trusting WidgetInfo::isHidden,
                    // which is only refreshed for widgets that were drawn this frame — i.e. for the
                    // ACTIVE page. Reads the same once-per-frame cache, so it costs nothing.
                    bool hidden = false;
                    for (GdxUI::DisableOption reason : widget.hideWhen) {
                        if (mDisabledInfo[reason].active) {
                            hidden = true;
                        }
                    }
                    if (hidden) {
                        continue;
                    }
                    const char* tooltip = widget.options != nullptr ? widget.options->tooltip : "";
                    const std::string haystack = normalise(
                        widget.name + " " + (tooltip != nullptr ? tooltip : "") + " " + widget.searchTerms);
                    if (haystack.find(query) == std::string::npos) {
                        continue;
                    }
                    ++matches;

                    ImGui::PushID(("hit_" + sectionName + sidebarName + std::to_string(col) + widget.name).c_str());
                    if (widget.type == GdxUI::WIDGET_CUSTOM) {
                        // A custom block is a whole sub-panel (a table, a modal owner, a status
                        // read-out). Rendering one inside the result list would duplicate its
                        // ImGui IDs against the copy on its own page and, for the dev-gate table,
                        // dwarf every other result. Name it and offer the jump instead.
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(widget.name.c_str());
                    } else {
                        MenuDrawItem(widget);
                    }

                    // "Go there" affordance. Deliberately a button rather than plain text: this is
                    // the navigate half of the feature, and a label that merely tells you where to
                    // look is not navigation.
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    const std::string origin = "  " ICON_FA_ARROW_RIGHT "  " + sectionName + " -> " + sidebarName +
                                               ", Col " + std::to_string(col + 1);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.57f, 0.64f, 1.0f));
                    const bool go = ImGui::Button(origin.c_str());
                    ImGui::PopStyleColor(2);
                    UIWidgets::Tooltip("Go to this setting on its own page.");
                    if (go) {
                        mNavigateRequested = true;
                        mNavigateSection = sectionName;
                        mNavigateSidebar = sidebarName;
                        mNavigateWidget = widget.name;
                    }
                    ImGui::PopID();
                }
            }
        }
    }

    return matches;
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

// ─────────────────────────────────────────────────────────────────────────────────────────────
// Section / sidebar selection and registry construction.
// ─────────────────────────────────────────────────────────────────────────────────────────────

void GdxMenu::SelectSection(const std::string& section) {
    if (mMenuEntries.count(section) == 0) {
        return;
    }
    mActiveSection = section;
    CVarSetString("gSettings.Menu.ActiveSection", mActiveSection.c_str());
    GdxSaveCvars();
    // A tab change moves the whole page list; re-park the nav cursor on the new tab's first page so
    // the pad does not end up focused on a now-hidden item. Harmless with mouse/keyboard (gated in
    // DrawSidebar on gControlNav).
    mFocusSidebar = true;
}

void GdxMenu::SelectSidebar(const std::string& sidebar) {
    auto section = mMenuEntries.find(mActiveSection);
    if (section == mMenuEntries.end() || section->second.sidebars.count(sidebar) == 0) {
        return;
    }
    // Persisted per SECTION, so each tab remembers where you were in it independently — the single
    // global "active page" integer could only remember one.
    CVarSetString(section->second.sidebarCvar, sidebar.c_str());
    GdxSaveCvars();
}

// The active page of the active section, falling back to that section's first page when the stored
// name is unknown (page renamed, removed, or never set).
const std::string& GdxMenu::ActiveSidebar() {
    static const std::string kNone;
    auto section = mMenuEntries.find(mActiveSection);
    if (section == mMenuEntries.end() || section->second.sidebarOrder.empty()) {
        return kNone;
    }
    const std::string stored = CVarGetString(section->second.sidebarCvar, "");
    for (const std::string& name : section->second.sidebarOrder) {
        if (name == stored) {
            return section->second.sidebars.find(name)->first;
        }
    }
    return section->second.sidebarOrder.front();
}

GdxUI::SidebarEntry* GdxMenu::ActiveSidebarEntry() {
    auto section = mMenuEntries.find(mActiveSection);
    if (section == mMenuEntries.end()) {
        return nullptr;
    }
    auto sidebar = section->second.sidebars.find(ActiveSidebar());
    return sidebar != section->second.sidebars.end() ? &sidebar->second : nullptr;
}

void GdxMenu::AddMenuEntry(const std::string& label, const char* sidebarCvar) {
    GdxUI::MainMenuEntry entry;
    entry.label = label;
    entry.sidebarCvar = sidebarCvar;
    mMenuEntries.emplace(label, std::move(entry));
    mMenuOrder.push_back(label);
}

void GdxMenu::AddSidebarEntry(const std::string& section, const std::string& sidebar, uint32_t columnCount,
                              const std::string& searchTerms) {
    auto it = mMenuEntries.find(section);
    if (it == mMenuEntries.end()) {
        return;
    }
    GdxUI::SidebarEntry entry;
    entry.columnCount = columnCount == 0 ? 1 : columnCount;
    // One widget vector per declared column, allocated up front so AddWidget can index straight in
    // and a page may legally leave a column empty.
    entry.columnWidgets.resize(entry.columnCount);
    entry.searchTerms = searchTerms;
    it->second.sidebars.emplace(sidebar, std::move(entry));
    it->second.sidebarOrder.push_back(sidebar);
}

void GdxMenu::AddWidget(const std::string& section, const std::string& sidebar, GdxUI::SectionColumns column,
                        GdxUI::WidgetInfo widget) {
    auto sectionIt = mMenuEntries.find(section);
    if (sectionIt == mMenuEntries.end()) {
        return;
    }
    auto sidebarIt = sectionIt->second.sidebars.find(sidebar);
    if (sidebarIt == sectionIt->second.sidebars.end()) {
        return;
    }
    // Every widget must carry an Options struct: MenuDrawItem writes options->disabled /
    // ->disabledTooltip for the named disable reasons, and the search reads options->tooltip.
    //
    // It must be the struct MATCHING widget.type, not the base. MenuDrawItem reaches its options
    // through static_pointer_cast, which does no checking: handed a plain WidgetOptions for a
    // WIDGET_TEXT, `options->color` reads past the end of the allocation, and the garbage enum that
    // comes back is then fed to ColorValues.at() -- which throws std::out_of_range. Nothing catches
    // it, so the process dies.
    //
    // That is exactly what happened: the first control on Dev Tools -> General is a WIDGET_TEXT
    // registered with no .Options() (gdx_menu_registry.cpp:1061), so opening Dev Tools killed the
    // game the instant the page drew. Settings -> Controls (:695) carried the same latent crash.
    // Allocating by type fixes the whole class, rather than requiring every future registration to
    // remember .Options() on precisely the subset of types that dereference it.
    if (widget.options == nullptr) {
        switch (widget.type) {
            case GdxUI::WIDGET_TEXT:
                widget.options = std::make_shared<UIWidgets::TextOptions>();
                break;
            case GdxUI::WIDGET_BUTTON:
                widget.options = std::make_shared<UIWidgets::ButtonOptions>();
                break;
            case GdxUI::WIDGET_CHECKBOX:
            case GdxUI::WIDGET_CVAR_CHECKBOX:
                widget.options = std::make_shared<UIWidgets::CheckboxOptions>();
                break;
            case GdxUI::WIDGET_COMBOBOX:
            case GdxUI::WIDGET_CVAR_COMBOBOX:
                widget.options = std::make_shared<UIWidgets::ComboboxOptions>();
                break;
            case GdxUI::WIDGET_SLIDER_INT:
            case GdxUI::WIDGET_CVAR_SLIDER_INT:
                widget.options = std::make_shared<UIWidgets::IntSliderOptions>();
                break;
            case GdxUI::WIDGET_SLIDER_FLOAT:
            case GdxUI::WIDGET_CVAR_SLIDER_FLOAT:
                widget.options = std::make_shared<UIWidgets::FloatSliderOptions>();
                break;
            case GdxUI::WIDGET_CVAR_RADIO_BUTTON:
                widget.options = std::make_shared<UIWidgets::RadioButtonsOptions>();
                break;
            default:
                // Decorative and custom types (separators, custom blocks) never downcast; they only
                // ever read the base fields, so the base struct is the correct allocation.
                widget.options = std::make_shared<UIWidgets::WidgetOptions>();
                break;
        }
    }
    auto& columns = sidebarIt->second.columnWidgets;
    size_t index = static_cast<size_t>(column);
    if (index >= columns.size()) {
        index = columns.empty() ? 0 : columns.size() - 1; // declared fewer columns than requested
    }
    if (columns.empty()) {
        columns.resize(1);
    }
    columns[index].push_back(std::move(widget));
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// MenuDrawItem — one registered WidgetInfo -> ImGui.
//
// Order of operations, and why:
//   1. hideWhen / preFunc            decide whether the control exists this frame at all
//   2. disableWhen                   turn the active named reasons into ONE tooltip that lists
//                                    every reason, so a control greyed for two reasons says both
//   3. the widget itself             via the UIWidgets CVar-bound library
//   4. callback                      side effects, only when the widget reported a change
//   5. modified marker / note        the "* changed from default" cue and the "(restart)" suffix
//   6. postFunc                      anything that has to react to state the widget cannot report
//   7. highlight                     the search-navigation outline
//
// NOTE: unlike Lighthouse's MenuDrawItem, this does NOT overwrite options->color with a global
// theme colour. Each Options struct's own default (LightBlue for checkboxes, Gray for sliders) is
// what every one of this menu's call sites already used, and forcing one colour on all of them
// would silently restyle the entire menu.
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenu::MenuDrawItem(GdxUI::WidgetInfo& widget) {
    widget.ResetDisables();

    // Hide conditions are evaluated from the same once-per-frame cache as the disable conditions.
    for (GdxUI::DisableOption reason : widget.hideWhen) {
        if (mDisabledInfo[reason].active) {
            widget.isHidden = true;
        }
    }
    if (widget.preFunc != nullptr) {
        widget.preFunc(widget);
    }
    if (widget.isHidden) {
        return;
    }

    for (GdxUI::DisableOption reason : widget.disableWhen) {
        if (mDisabledInfo[reason].active) {
            widget.activeDisables.push_back(reason);
        }
    }
    if (!widget.activeDisables.empty()) {
        // The named-reason payoff: a greyed control states WHY, and can state several reasons at
        // once. Built into a member string because UIWidgets' Options structs borrow the pointer.
        mDisabledTooltip = "This setting is unavailable because:";
        for (GdxUI::DisableOption reason : widget.activeDisables) {
            mDisabledTooltip += "\n  - ";
            mDisabledTooltip += mDisabledInfo[reason].reason;
        }
        widget.options->disabled = true;
        widget.options->disabledTooltip = mDisabledTooltip.c_str();
    }

    if (widget.sameLine) {
        ImGui::SameLine();
    }

    const bool highlight = !mHighlightWidget.empty() && widget.name == mHighlightWidget &&
                           ImGui::GetTime() < mHighlightUntil;
    if (highlight) {
        // Group the whole control so the outline below covers the label + widget + any inline
        // buttons, not just whichever ImGui item happened to be submitted last.
        ImGui::BeginGroup();
    }

    // A widget registered WITHOUT .Options() must fall back to that widget type's defaults, never
    // dereference null. WidgetInfo::options is a shared_ptr populated only by .Options(), and it is
    // legitimately absent for the decorative types (WIDGET_SEPARATOR, WIDGET_CUSTOM, ...) — so
    // "absent" cannot be treated as a programming error the switch is allowed to assume away.
    //
    // It bit us immediately: the first control on Dev Tools -> General is a WIDGET_TEXT registered
    // with no Options (gdx_menu_registry.cpp:1061), and the unguarded
    //   auto options = std::static_pointer_cast<TextOptions>(widget.options); options->color
    // dereferenced null the instant that page drew, so opening Dev Tools killed the process. The
    // same latent crash sat on Settings -> Controls (gdx_menu_registry.cpp:695).
    //
    // Fixing it here rather than only at those two call sites is deliberate: the registry is meant
    // to be edited by hand and grown, and "you must remember .Options() on this subset of types or
    // the game dies on that page" is not a contract worth shipping.
    // The argument is a type tag only; the fallback it names is a function-local static, so the
    // returned reference stays valid after the call (returning a reference to the caller's
    // temporary would trade the null deref for a dangling one).
    const auto optionsOr = [&widget](auto tag) -> const decltype(tag)& {
        using T = decltype(tag);
        static const T kDefaults{};
        return widget.options != nullptr ? *std::static_pointer_cast<T>(widget.options) : kDefaults;
    };

    bool changed = false;
    switch (widget.type) {
        case GdxUI::WIDGET_SEPARATOR:
            ImGui::Separator();
            break;
        case GdxUI::WIDGET_SEPARATOR_TEXT:
            ImGui::SeparatorText(widget.name.c_str());
            break;
        case GdxUI::WIDGET_TEXT: {
            const UIWidgets::TextOptions& options = optionsOr(UIWidgets::TextOptions{});
            const bool coloured = options.color != UIWidgets::Colors::NoColor;
            if (coloured) {
                ImGui::PushStyleColor(ImGuiCol_Text, UIWidgets::ColorValues.at(options.color));
            }
            ImGui::TextWrapped("%s", widget.name.c_str());
            if (coloured) {
                ImGui::PopStyleColor();
            }
            break;
        }
        case GdxUI::WIDGET_TEXT_DISABLED:
            ImGui::TextDisabled("%s", widget.name.c_str());
            break;
        case GdxUI::WIDGET_COMING_SOON:
            GdxComingSoon(widget.name.c_str());
            break;
        case GdxUI::WIDGET_CHECKBOX: {
            bool* value = std::get<bool*>(widget.valuePointer);
            if (value == nullptr) {
                break;
            }
            changed = UIWidgets::Checkbox(widget.name.c_str(), value,
                                          optionsOr(UIWidgets::CheckboxOptions{}));
            break;
        }
        case GdxUI::WIDGET_CVAR_CHECKBOX:
            changed = UIWidgets::CVarCheckbox(widget.name.c_str(), widget.cVar,
                                              optionsOr(UIWidgets::CheckboxOptions{}));
            break;
        case GdxUI::WIDGET_COMBOBOX: {
            int32_t* value = std::get<int32_t*>(widget.valuePointer);
            if (value == nullptr) {
                break;
            }
            changed = UIWidgets::Combobox<int32_t>(widget.name.c_str(), value, widget.comboItems,
                                                   optionsOr(UIWidgets::ComboboxOptions{}));
            break;
        }
        case GdxUI::WIDGET_CVAR_COMBOBOX:
            changed = UIWidgets::CVarCombobox(widget.name.c_str(), widget.cVar, widget.comboItems,
                                              optionsOr(UIWidgets::ComboboxOptions{}));
            break;
        case GdxUI::WIDGET_SLIDER_INT: {
            int32_t* value = std::get<int32_t*>(widget.valuePointer);
            if (value == nullptr) {
                break;
            }
            changed = UIWidgets::SliderInt(widget.name.c_str(), value,
                                           optionsOr(UIWidgets::IntSliderOptions{}));
            break;
        }
        case GdxUI::WIDGET_CVAR_SLIDER_INT:
            changed = UIWidgets::CVarSliderInt(widget.name.c_str(), widget.cVar,
                                               optionsOr(UIWidgets::IntSliderOptions{}));
            break;
        case GdxUI::WIDGET_SLIDER_FLOAT: {
            float* value = std::get<float*>(widget.valuePointer);
            if (value == nullptr) {
                break;
            }
            changed = UIWidgets::SliderFloat(widget.name.c_str(), value,
                                             optionsOr(UIWidgets::FloatSliderOptions{}));
            break;
        }
        case GdxUI::WIDGET_CVAR_SLIDER_FLOAT:
            changed =
                UIWidgets::CVarSliderFloat(widget.name.c_str(), widget.cVar,
                                           optionsOr(UIWidgets::FloatSliderOptions{}));
            break;
        case GdxUI::WIDGET_BUTTON: {
            // Plain ImGui::Button, NOT UIWidgets::Button: this menu's shell already styles buttons
            // through GdxPushModernStyle (the G-Diffuser blue), and UIWidgets::Button unconditionally
            // pushes its own palette from ButtonOptions::color, which would repaint every button in
            // the menu grey. Only the disabled handling and tooltip are taken from the Options.
            const UIWidgets::ButtonOptions& options = optionsOr(UIWidgets::ButtonOptions{});
            ImGui::BeginDisabled(options.disabled);
            changed = ImGui::Button(widget.name.c_str(), options.size);
            ImGui::EndDisabled();
            // AllowWhenDisabled so a greyed button is exactly the one that explains itself.
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                const char* text = options.disabled && !UIWidgets::IsCStringEmpty(options.disabledTooltip)
                                       ? options.disabledTooltip
                                       : options.tooltip;
                if (!UIWidgets::IsCStringEmpty(text)) {
                    ImGui::SetTooltip("%s", UIWidgets::WrappedText(text).c_str());
                }
            }
            break;
        }
        case GdxUI::WIDGET_CVAR_RADIO_BUTTON:
            changed = UIWidgets::CVarRadioButton(widget.name.c_str(), widget.cVar, widget.radioValue,
                                                 optionsOr(UIWidgets::RadioButtonsOptions{}));
            break;
        case GdxUI::WIDGET_CUSTOM:
            if (widget.customFunction != nullptr) {
                widget.customFunction(widget);
            }
            break;
    }

    if (changed && widget.callback != nullptr) {
        widget.callback(widget);
    }

    // "Changed from the stock default" asterisk. The default comes from the widget's own
    // CheckboxOptions::defaultValue, so the marker and the control can never disagree about what
    // stock is (this is what the old GdxCVarCheckboxMarked helper guaranteed by hand).
    if (widget.modifiedMarker && widget.type == GdxUI::WIDGET_CVAR_CHECKBOX) {
        const bool def = optionsOr(UIWidgets::CheckboxOptions{}).defaultValue;
        GdxModifiedMarker((CVarGetInteger(widget.cVar, def) != 0) != def);
    }

    // The greyed suffix ("(restart)", "(applies on restart)", "(disabled in-race)"). UIWidgets has
    // no slot for one, and every one of these was a deliberate SameLine + TextDisabled at the old
    // call site, so it stays exactly that — just declared rather than written out.
    if (!UIWidgets::IsCStringEmpty(widget.note)) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", widget.note);
    }

    if (widget.postFunc != nullptr) {
        widget.postFunc(widget);
    }

    if (highlight) {
        ImGui::EndGroup();
        // Pulsing outline around the control the search sent us to. ImGui has no "flash this item"
        // primitive, and Lighthouse's own highlightWidget/navigateWidgetName globals (Menu.cpp:29-33)
        // are declared but never read, so this is ours.
        const ImVec2 min = ImGui::GetItemRectMin() - ImVec2(4.0f, 3.0f);
        const ImVec2 max = ImGui::GetItemRectMax() + ImVec2(4.0f, 3.0f);
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
        ImGui::GetWindowDrawList()->AddRect(min, max,
                                            ImGui::GetColorU32(ImVec4(0.63f, 0.76f, 1.0f, 0.35f + 0.55f * pulse)),
                                            3.0f, 0, 2.0f);
        if (mHighlightScrollPending) {
            // Record where it landed; DrawElement scrolls the content pane to it once this page has
            // finished laying out. Deliberately NOT ImGui::SetScrollHereY(): on a multi-column page
            // this runs inside a non-scrolling column child, which has no scroll to set.
            mHighlightScreenY = min.y;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// Audio output status — WIDGET_CUSTOM (Settings -> Audio).
//
// Not expressible as a widget: it reads three live values and picks between three different
// renderings. Diagnostic first-class citizen — a "no audio" report is undebuggable remotely
// without knowing which backend the session picked and whether samples are actually queued.
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenu::DrawAudioStatus() {
    // Reports the ACTUAL active AudioPlayer backend (via AudioPlayerBackendName) rather than
    // SDL_GetCurrentAudioDriver(), which returns "none" for the WASAPI/CoreAudio backends even
    // when they are working — misleading on Windows. For the SDL backend we additionally surface
    // SDL_GetCurrentAudioDriver() (e.g. "pipewire"/"pulse"), where a "dummy" driver means the
    // launch environment lost the audio socket (sandboxed/naked launcher env): the game
    // synthesizes fine but the samples go nowhere.
    const char* backend = AudioPlayerBackendName();
    const bool isSdl = std::strcmp(backend, "SDL") == 0;
    const char* sdlDriver = isSdl ? SDL_GetCurrentAudioDriver() : nullptr;
    const int32_t buffered = AudioPlayerBuffered();
    const int32_t desired = AudioPlayerGetDesiredBuffered();

    ImGui::SeparatorText("Output status");
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
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// Ghost import / export — WIDGET_CUSTOM (Enhancements -> Practice).
//
// Calls the port's gdx_ghost_io C API (port/gdx_ghost_io.c). Export is read-only. Import adds a
// validated player replay to the per-course PC library and mirrors it into SRAM only when that does
// not evict another course. It stays disabled while an on-track race is live to avoid mutating
// ghost state alongside the game fiber. Both use the default path next to the exe
// (ghost_export.gdg); a proper file picker remains future work.
//
// Stays a custom block rather than becoming two WIDGET_BUTTONs: the pair shares a status buffer and
// a resolved path that both branches format into, and the export tooltip interpolates that path
// (see the note at that call), which no Options struct can carry.
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenu::DrawGhostIo() {
    ImGui::TextDisabled("Ghost replay (.gdg)");

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
    // Left as a raw SetTooltip rather than UIWidgets::Tooltip: the text interpolates a runtime
    // filesystem path, and UIWidgets::Tooltip runs every tooltip through WrappedText at 80
    // columns (UIWidgets.cpp:98-102), which would insert newlines at the spaces inside a path
    // like "...\Proyectos Mios\..." and make the destination unreadable.
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Writes the currently-saved ghost replay to:\n%s",
                          haveDefault ? path : "(unavailable)");
    }

    ImGui::SameLine();

    // The in-race lockout is a NAMED disable reason on this button
    // (DISABLE_FOR_RACE_IN_PROGRESS, evaluated once per frame in RegisterDisableReasons), so the
    // greyed button now states why it is greyed instead of only being annotated beside it. The
    // "(disabled in-race)" label is kept as well, because it is visible without hovering.
    const bool inGame = mDisabledInfo[GdxUI::DISABLE_FOR_RACE_IN_PROGRESS].active;
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
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", inGame ? "This setting is unavailable because:\n  - A race is in progress."
                                       : "Reads ghost_export.gdg back into your per-course ghost library.");
    }
    if (inGame) {
        ImGui::SameLine();
        ImGui::TextDisabled("(disabled in-race)");
    }

    if (sGhostStatus[0] != '\0') {
        ImGui::TextWrapped("%s", sGhostStatus);
    }
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
// Frame-interpolation live statistics — WIDGET_CUSTOM (Dev Tools -> Stats).
//
// Real presented-FPS visibility. When Frame Interpolation is on the sim runs at 60 Hz but the
// renderer presents multiple sub-frames per tick, so ImGui's own io.Framerate (which counts one
// frame per present) is the true presented rate. We show it alongside the fixed 60 Hz logic rate
// ("144 fps (sim 60 Hz)") plus the live sub-frame count and the previous tick's tween/snap
// breakdown, so a "cost without benefit" regression (lerped == 0) is visible at a glance. These
// reuse the existing bridge getters — no extra per-frame cost.
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenu::DrawInterpStats() {
    if (gdx_gfx_interp_host_active() == 0) {
        return;
    }
    ImGui::SeparatorText("Frame Interpolation");
    if (gdx_gfx_interp_tick_active() == 0) {
        // The menu's toggle is on, but main.cpp forced interpolation off THIS tick (Course
        // Edit / Create Machine editors force the stock single-present path). The live
        // numbers below are from before the editor was entered, so show the paused truth
        // instead of stale/misleading figures.
        ImGui::TextDisabled("Interpolation paused (editor active)");
    } else {
        const double presentedFps = gdx_gfx_interp_presents_per_sec();
        const float imguiFps = ImGui::GetIO().Framerate;
        const int m = gdx_gfx_interp_last_subframes();
        const int lerped = gdx_gfx_interp_last_lerped();
        const int snapped = gdx_gfx_interp_last_snapped();
        // presents/s meter is bridge-measured; fall back to ImGui's rate before the first window fills.
        const double shownFps = (presentedFps > 0.0) ? presentedFps : static_cast<double>(imguiFps);
        ImGui::Text("Presented: %.0f fps (sim 60 Hz)", shownFps);
        ImGui::Text("Sub-frames/tick (M): %d", m);
        if (lerped == 0) {
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.20f, 1.0f),
                               "Interpolated slots: 0 (no tween - snapping every tick)");
        } else {
            ImGui::Text("Interpolated slots: %d   Snapped: %d", lerped, snapped);
        }
    }
    ImGui::Separator();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// WORKSHOP — the three custom blocks. The "Enable texture packs" toggle and the Content Installs
// roadmap lines are plain registry widgets; everything below needs a table, a subprocess snapshot
// or a modal, so each is one WIDGET_CUSTOM entry.
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenu::DrawTexturePacks() {
    static char sReloadStatus[160] = "";
    const ImVec4 kRed = ImVec4(0.90f, 0.25f, 0.25f, 1.0f);

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
            // Deliberately still ImGui::Checkbox, not UIWidgets::Checkbox: this one lives in a
            // 32px WidthFixed column, and UIWidgets' re-implementation always adds
            // ItemInnerSpacing.x * 2 of label gutter to its bounding box even for an empty
            // "##" label (UIWidgets.cpp:372-373), which would push it out of the column. Only the
            // tooltip is folded. The pack state is not a CVar either -- it lives in the
            // comma-joined DisabledPacks list that GdxWorkshopSetPackDisabled maintains.
            if (ImGui::Checkbox("##en", &enabled)) {
                // Toggling the checkbox rewrites the persisted disable list; the change takes effect
                // on the next Reload (or the next boot) since the archive set is mounted once.
                GdxWorkshopSetPackDisabled(p.basename.c_str(), enabled ? 0 : 1);
            }
            UIWidgets::Tooltip("Enable or disable this pack. Takes effect on the next Reload or boot.");

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
    UIWidgets::Tooltip("Re-scans mods/, re-mounts packs, and clears the texture cache so edits\n"
                       "appear without restarting.");
    ImGui::SameLine();
    if (ImGui::Button("Open mods folder")) {
        GdxOpenFolder(GdxWorkshopModsDir(true));
    }
    UIWidgets::Tooltip("Open the mods/ folder in your file browser (created if absent).");
    if (sReloadStatus[0] != '\0') {
        ImGui::TextDisabled("%s", sReloadStatus);
    }

    // Texture Dump (the play-until-you-see-it runtime dumper) was retired here: Asset Dump below
    // supersedes it, decoding named assets straight from the archive instead of depending on which
    // textures gameplay happened to walk past. Its CVar is forced to 0 at registration so an
    // already-on setting cannot outlive the checkbox. "Open dump folder" survives as part of the
    // Asset Dump section -- both dumpers wrote to the same dump/ directory.
}

// Offline per-class asset dump, native-first via the bundled gdx-extract (falls back to
// tools/gen_dump_all.py in dev checkouts without the native binary). Implementation lives in
// port/gdx_dump_launch.{h,cpp}; this block only READS the shared snapshot -- every subprocess runs
// on a detached worker thread, one child PER CLASS so a broken class never aborts the rest.
//
// Custom rather than registry data because the class list is DISCOVERED AT RUNTIME (an async
// `--list-classes` probe), so the per-class checkboxes cannot be declared ahead of time; their
// disabled state is therefore driven from the batch snapshot this block already reads, not from
// the named-reason map (a reason evaluated once per frame would have to take the same snapshot a
// second time, and the snapshot is a locked copy).
void GdxMenu::DrawAssetDump() {
    const ImVec4 kRed = ImVec4(0.90f, 0.25f, 0.25f, 1.0f);

    // Discover the backend once (pure filesystem/PATH -- no subprocess), and kick the async
    // `--list-classes` probe once. Both cached across frames via function-local statics.
    static gdx::DumpEnvironment sDumpEnv = gdx::GdxDumpDiscover();
    static bool sProbeKicked = (gdx::GdxDumpBeginClassListProbe(sDumpEnv), true);
    (void)sProbeKicked;

    ImGui::TextWrapped("Decode named assets straight from the extracted archive - no gameplay "
                       "needed. Runs the bundled dump tool once per selected class; results land "
                       "in dump/ (same place as the runtime texture dump).");
    if (!sDumpEnv.available) {
        ImGui::TextDisabled("%s", sDumpEnv.reason.c_str());
    }

    gdx::DumpBatchSnapshot snap = gdx::GdxDumpSnapshot();
    const bool running = snap.running;
    const std::vector<std::string> dumpClasses = gdx::GdxDumpCurrentClasses();

    // -- Per-class checkboxes (persisted as gEnhancements.Workshop.DumpClass.<name>, default on) --
    ImGui::BeginDisabled(!sDumpEnv.available || running);
    if (ImGui::BeginTable("##DumpClasses", 2, ImGuiTableFlags_SizingStretchProp)) {
        for (const auto& cls : dumpClasses) {
            ImGui::TableNextColumn();
            // Per-class CVar name is built at runtime, but CVarCheckbox takes the name as a
            // plain const char* so the dynamic key works unchanged. DefaultValue(true) is the
            // old `CVarGetInteger(key, 1)` default: every class starts checked. Nested inside
            // the outer BeginDisabled above -- ImGui's BeginDisabled(false) preserves an
            // already-disabled scope, so the rows stay greyed while a dump runs.
            std::string cvarKey = "gEnhancements.Workshop.DumpClass." + cls;
            std::string label = gdx::GdxDumpPrettyName(cls) + "##dumpclass_" + cls;
            UIWidgets::CVarCheckbox(label.c_str(), cvarKey.c_str(),
                                    UIWidgets::CheckboxOptions().DefaultValue(true));
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Dump selected")) {
        std::vector<std::string> selected;
        for (const auto& cls : dumpClasses) {
            std::string cvarKey = "gEnhancements.Workshop.DumpClass." + cls;
            if (CVarGetInteger(cvarKey.c_str(), 1) != 0) {
                selected.push_back(cls);
            }
        }
        gdx::GdxDumpStartBatch(sDumpEnv, selected, GdxWorkshopDumpDir(true));
    }
    ImGui::SameLine();
    if (ImGui::Button("Dump everything")) {
        gdx::GdxDumpStartBatch(sDumpEnv, dumpClasses, GdxWorkshopDumpDir(true));
    }
    ImGui::EndDisabled();

    // -- Cancel (cooperative: stops AFTER the current class finishes) --
    ImGui::SameLine();
    ImGui::BeginDisabled(!running || snap.cancelRequested);
    if (ImGui::Button("Cancel")) {
        gdx::GdxDumpRequestCancel();
    }
    ImGui::EndDisabled();
    UIWidgets::Tooltip("Stops after the current class finishes. The running class is left to "
                       "complete cleanly - no child process is killed.");

    // -- Per-class progress lines + batch summary --
    for (const auto& p : snap.classes) {
        std::string pretty = gdx::GdxDumpPrettyName(p.name);
        switch (p.phase) {
        case gdx::DumpPhase::Queued:
            ImGui::TextDisabled("%s: queued", pretty.c_str());
            break;
        case gdx::DumpPhase::Running: {
            const char spin[] = {'|', '/', '-', '\\'};
            ImGui::Text("%s: running %c", pretty.c_str(),
                        spin[static_cast<int>(ImGui::GetTime() * 4.0) & 3]);
            break;
        }
        case gdx::DumpPhase::Done:
            if (p.itemsDumped >= 0) {
                ImGui::Text("%s: done - %d item(s) in %.1fs", pretty.c_str(), p.itemsDumped,
                            p.elapsedSeconds);
            } else {
                ImGui::Text("%s: done - %.1fs", pretty.c_str(), p.elapsedSeconds);
            }
            break;
        case gdx::DumpPhase::Failed:
            ImGui::TextColored(kRed, "%s: FAILED (exit %d) %s", pretty.c_str(), p.exitCode,
                               p.lastLine.c_str());
            break;
        default:
            ImGui::TextDisabled("%s: idle", pretty.c_str());
            break;
        }
    }
    if (!snap.summary.empty()) {
        ImGui::TextDisabled("%s", snap.summary.c_str());
    }

    // Every dumper writes into the same dump/ directory, so one shortcut serves all of them.
    // This is the only in-menu way to reach that folder.
    if (ImGui::Button("Open dump folder")) {
        GdxOpenFolder(GdxWorkshopDumpDir(true));
    }
    UIWidgets::Tooltip("Open the dump/ folder in your file browser (created if absent).");
}

// 64DD durable-save sidecar status + the one-shot format authorization modal.
void GdxMenu::DrawDdSave() {
    const ImVec4 kRed = ImVec4(0.90f, 0.25f, 0.25f, 1.0f);

    ImGui::Text("Sidecar: %s", gdx_disk_sidecar_present() ? "present" : "none yet");
    ImGui::Text("Journal records: %d", gdx_disk_sidecar_record_count());
    ImGui::Text("Last flush: %s", gdx_disk_last_flush_ok() ? "ok" : "FAILED");
    if (gdx_disk_format_refused_this_boot()) {
        ImGui::TextColored(kRed, "The disk's MFS save area is uninitialized.");
        if (ImGui::Button("Initialize DD save area")) {
            ImGui::OpenPopup("##ddformat");
        }
        UIWidgets::Tooltip("Authorizes a one-time format of the 64DD MFS save area on the NEXT boot.");
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
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// DEVELOPER — the pop-out buttons for the LUS dev windows.
//    Console + Stats are auto-registered by the LUS Gui ctor; Gfx Debugger is registered in
//    main.cpp at boot. Each button toggles the LIVE window via GetGuiWindow(name)->
//    ToggleVisibility() (a bare CVarSet would not move an already-constructed window).
//
// Custom because the three share one error line: a missing window is REPORTED rather than
// ignored, and the report belongs to the group, not to any one button.
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenu::DrawDevToolButtons() {
    // These names must match the registrations in main.cpp / the libultraship Gui ctor exactly, and
    // a typo used to produce a button that looked fine and did nothing at all.
    static std::string sToolStatus;
    struct ToolButton {
        const char* label;
        const char* window;
        const char* tip;
    };
    static const ToolButton kTools[] = {
        { "Open Stats", "Stats", "Toggle the live frame-timing / renderer Stats window." },
        { "Open Console", "Console", "Toggle the developer console and command history." },
        { "Open Gfx Debugger", "Gfx Debugger", "Toggle the Fast3D display-list debugger." },
    };
    for (int i = 0; i < static_cast<int>(sizeof(kTools) / sizeof(kTools[0])); ++i) {
        if (i != 0) {
            ImGui::SameLine();
        }
        if (ImGui::Button(kTools[i].label)) {
            if (GdxToggleWindow(kTools[i].window)) {
                sToolStatus.clear();
            } else {
                sToolStatus = std::string(kTools[i].window) + " is not registered in this build.";
            }
        }
        UIWidgets::Tooltip(kTools[i].tip);
    }
    if (!sToolStatus.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", sToolStatus.c_str());
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 8b) DEVELOPER GATES — the checkbox surface for port/gdx_dev_gates.{h,c}.
//
// These rows replace ~25 environment variables that were previously invisible and undiscoverable.
// Everything here is table-driven: adding a gate to kGates in gdx_dev_gates.c makes it appear
// with no edit to this function.
//
// Three sections, matching the bucket policy:
//   Logging     (Bucket D) — persisted CVar adopted at boot; carries the honest "applies to new
//                            output; restart to capture boot" caveat, and is DISABLED when an
//                            environment variable pinned it for this run.
//   Diagnostics (Bucket A) — logging only, safe to leave on, live.
//   Behaviour   (Bucket B) — CHANGES RENDERING OR GAME BEHAVIOUR. Whole section compiled out
//                            unless GDX_DEV_TOOLS (Debug-only by default; see port/CMakeLists.txt).
//
// A toggle writes the CVar, schedules the config save, and calls gdx_dev_gates_refresh() so the
// change lands on the CURRENT frame rather than the next one (the frame loop's own refresh already
// ran before the menu drew).
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenu::DrawDevGates() {
    ImGui::SeparatorText("Developer gates");
    ImGui::TextWrapped("These replace the port's GDX_* environment variables. Changes apply "
                       "immediately and persist in gdiffuser.cfg.json. A variable exported at "
                       "launch still works and is marked below.");

    // Confirmed rather than immediate: this discards every gate the user set, across all three
    // sections at once, and there is no undo once the config is saved. Someone half-way through
    // bisecting a bug can lose the whole setup to one stray click.
    if (ImGui::Button("Reset all gates to defaults")) {
        ImGui::OpenPopup("##resetgates");
    }
    UIWidgets::Tooltip("Clears every gate back to stock. Gates pinned by an environment "
                       "variable are unaffected until the next launch.");
    if (ImGui::BeginPopupModal("##resetgates", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        int changed = 0;
        for (int id = 0; id < gdx_dev_gate_count(); ++id) {
            if ((gdx_dev_gate(id) != 0) != (gdx_dev_gate_default(id) != 0)) {
                ++changed;
            }
        }
        ImGui::TextUnformatted("Reset every developer gate to its default?");
        ImGui::Separator();
        if (changed == 0) {
            ImGui::TextDisabled("Nothing to reset - every gate is already at its default.");
        } else {
            ImGui::Text("%d gate(s) will be turned back to stock. This cannot be undone.", changed);
        }
        ImGui::Separator();
        if (ImGui::Button("Reset")) {
            for (int id = 0; id < gdx_dev_gate_count(); ++id) {
                CVarSetInteger(gdx_dev_gate_cvar_name(id), gdx_dev_gate_default(id));
            }
            GdxSaveCvars();
            gdx_dev_gates_refresh();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Presentation order, which is NOT the enum order: logging first (it is the one a bug reporter
    // needs), then read-only diagnostics, then the dangerous section last.
    static const int kBucketOrder[] = { GDX_GATE_BUCKET_BOOT, GDX_GATE_BUCKET_DIAG,
                                        GDX_GATE_BUCKET_BEHAVIOR };
    for (int bucketIndex = 0; bucketIndex < (int) (sizeof(kBucketOrder) / sizeof(kBucketOrder[0]));
         ++bucketIndex) {
        const int bucket = kBucketOrder[bucketIndex];
#ifndef GDX_DEV_TOOLS
        // Bucket B is not compiled into this build: its gates are hard-wired to 0 with no getenv and
        // no CVar read, so drawing live checkboxes would be a lie. Silently skipping the section was
        // its own problem though -- the docs and the Behavior gates are referenced elsewhere, and a
        // section that simply is not there reads as "this build is broken" rather than "this build
        // deliberately excludes it". Say so instead of vanishing.
        if (bucket == GDX_GATE_BUCKET_BEHAVIOR) {
            ImGui::SeparatorText("Behavior overrides (not in this build)");
            ImGui::TextDisabled("Compiled out of Release. These gates change what the game renders or\n"
                                "how it sounds, and exist to bisect bugs rather than to play with.\n"
                                "Configure with -DGDX_FORCE_DEV_TOOLS=ON to build them in.");
            continue;
        }
#endif
        if (bucket == GDX_GATE_BUCKET_BOOT) {
            ImGui::SeparatorText("Logging");
            ImGui::TextWrapped("Read at startup from the saved setting, so enabling one here and "
                               "restarting captures the next boot.");
        } else if (bucket == GDX_GATE_BUCKET_DIAG) {
            ImGui::SeparatorText("Diagnostics");
            ImGui::TextWrapped("Extra log output only. Safe to leave on; enable the Logging gates "
                               "above so the lines reach gdiffuser-run.log.");
        } else {
            ImGui::SeparatorText("Behavior overrides (not for normal play)");
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.30f, 1.0f),
                               "WARNING: these change what the game renders or how it sounds. "
                               "They exist to bisect bugs, not to play with. Leave them off unless "
                               "you are reproducing a specific issue.");
            ImGui::TextDisabled("Debug builds only - this section is compiled out of a Release build.");
        }

        for (int group = 0; group < GDX_GATE_GROUP_COUNT; ++group) {
            bool groupHeaderDrawn = false;
            for (int id = 0; id < gdx_dev_gate_count(); ++id) {
                if (gdx_dev_gate_bucket(id) != bucket || gdx_dev_gate_group(id) != group) {
                    continue;
                }
                // Only the Diagnostics and Behavior sections need a per-domain sub-header; the
                // Logging bucket is a single group already named by its section title.
                if (!groupHeaderDrawn && bucket != GDX_GATE_BUCKET_BOOT) {
                    groupHeaderDrawn = true;
                    ImGui::TextDisabled("%s", gdx_dev_gate_group_name(group));
                }

                const bool pinned = gdx_dev_gate_is_env_pinned(id) != 0;
                bool value = gdx_dev_gate(id) != 0;

                if (pinned) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Checkbox(gdx_dev_gate_label(id), &value)) {
                    CVarSetInteger(gdx_dev_gate_cvar_name(id), value ? 1 : 0);
                    GdxSaveCvars();
                    gdx_dev_gates_refresh(); // land the change on THIS frame
                }
                if (pinned) {
                    ImGui::EndDisabled();
                }

                // AllowWhenDisabled: a pinned row is exactly the row a user most needs explained --
                // without this flag ImGui suppresses hover on disabled items, so the one checkbox
                // that refuses to move is also the only one that will not say why.
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("%s\n\nEnvironment: %s\nSetting: %s", gdx_dev_gate_help(id),
                                      gdx_dev_gate_env_name(id), gdx_dev_gate_cvar_name(id));
                }
                GdxModifiedMarker(value != (gdx_dev_gate_default(id) != 0));

                // Order matters. A BOOT gate that is merely PRESENT in the environment but resolved
                // to off is now freely toggleable, so it gets the useful restart hint rather than a
                // "(started from ...)" label that would be stale the moment the user ticked it.
                if (pinned) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(pinned ON by %s this run)", gdx_dev_gate_env_name(id));
                } else if (bucket == GDX_GATE_BUCKET_BOOT) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(applies to new output; restart to capture boot)");
                } else if (gdx_dev_gate_from_env(id)) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(started from %s)", gdx_dev_gate_env_name(id));
                }
            }
        }
    }

    // Environment variables that deliberately stay environment variables (Bucket C): they are
    // consumed before the console exists, or they carry a value rather than a flag. Listed so the
    // Dev Tools page is a complete map of the port's switches rather than a partial one.
    ImGui::SeparatorText("Environment only (no setting)");
    ImGui::TextWrapped("Consumed before the settings system exists, or not a simple on/off:");
    ImGui::BulletText("GDX_INPUT_SCRIPT - deterministic input playback for unattended tests");
    ImGui::BulletText("FZEROX_ROM, GDX_STRICT_ARCHIVE, GDX_DUMP_SELFTEST - boot and tooling paths");
    ImGui::BulletText("GDX_CAPTURE_FRAMES / GDX_CAPTURE_MODE / GDX_CAPTURE_WINDOW - \"start:count\" captures");
    ImGui::BulletText("GDX_PCM_CAPTURE, GDX_PCM_CAPTURE_FRAMES - audio capture prefix and length");
    ImGui::BulletText("GDX_RAND_SEED1 / GDX_RAND_SEED2 - RNG determinism pins (numeric seeds)");
    ImGui::BulletText("GDX_SEED_BOOT_LOGO, GDX_AUDIO_THREAD, GDX_AI_CUSHION - decided before this menu exists");
    ImGui::BulletText("GDX_INTERP_P1 / GDX_INTERP_P2 - interpolation test overrides (see Graphics)");
}

// The port's release version, single source of truth for user-facing surfaces. Bumped by hand at
// release points — there is deliberately no build-count automation, because a version a human
// didn't choose is a build id, not a version.
static constexpr const char* kGdxVersionString = "1.0.0";

// ─────────────────────────────────────────────────────────────────────────────────────────────
// 9) ABOUT — the Kiziio logo (loaded once from gdiffuser.o2r's branding/ entry via Fast3dGui),
//    the version line, the EK-required boot notice, and credits.
// ─────────────────────────────────────────────────────────────────────────────────────────────
void GdxMenu::DrawAboutMenu() {
    // Lazy one-shot load on first open rather than at boot: the About page is visited rarely, and
    // a failed load (archive predating the branding entry) must degrade to the text title, never
    // block the menu. tried/loaded are separate so a failure doesn't retry every frame.
    static bool sLogoTried = false;
    static bool sLogoLoaded = false;
    if (!sLogoTried) {
        sLogoTried = true;
        auto gui = std::dynamic_pointer_cast<Fast::Fast3dGui>(
            Ship::Context::GetInstance()->GetWindow()->GetGui());
        if (gui != nullptr) {
            gui->LoadTextureFromRawImage("gdx-logo", "branding/gdiffuser-logo.png");
            sLogoLoaded = gui->GetTextureByName("gdx-logo") != nullptr;
        }
    }
    bool logoDrawn = false;
    if (sLogoLoaded) {
        auto gui = std::dynamic_pointer_cast<Fast::Fast3dGui>(
            Ship::Context::GetInstance()->GetWindow()->GetGui());
        ImTextureID logo = (gui != nullptr) ? gui->GetTextureByName("gdx-logo") : nullptr;
        if (logo != nullptr) {
            // Source is 1024x324; draw at a width that fits the pane, aspect preserved.
            const float w = std::min(420.0f, ImGui::GetContentRegionAvail().x);
            const float h = w * (324.0f / 1024.0f);
            ImGui::Image(logo, ImVec2(w, h));
            logoDrawn = true;
        }
    }
    if (!logoDrawn) {
        ImGui::Text("G-Diffuser");
    }
    ImGui::Text("Version %s", kGdxVersionString);
    ImGui::TextDisabled("A native PC source port of F-Zero X (N64) + Expansion Kit (64DD)");

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
    ImGui::BulletText("Logo & icon artwork - Kiziio (github.com/Kiziio1)");

    ImGui::Separator();
    ImGui::TextDisabled("https://github.com/Zorkats/G-Diffuser");

}
