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
#include <cstdio> // snprintf (Practice-tab ghost import/export status line)
#include <cstdlib> // std::system (non-Windows open-folder fallback)
#include <memory> // std::dynamic_pointer_cast (null-safe downcast to Fast::Fast3dWindow)
#include <string>

#include "gdx_ghost_io.h" // .gdg ghost import/export C API (Practice tab Export / Import buttons)
#include "gdx_gui.h"
#include "gdx_workshop.h"    // Workshop tab: texture-pack listing, override count, reload, dump dir
#include "gdx_dump_launch.h" // Workshop tab "Asset Dump" section: per-class offline gen_dump_all.py launcher
#include "disk_savefile.h"   // Workshop tab "DD Save" subsection: sidecar status + one-shot format
#include "rom_buffer.h"      // Data & Files: gdx_rom_buffer/gdx_rom_path (live ROM residency signal)
#include "gdx_firstboot.h"   // Data & Files: canonical file names + gdx::ManagedDiskPath
#include "gdx_segment_source.h" // Data & Files: R1 archive-coverage telemetry (fallback counters)

#include <vector>
#include <filesystem>

// From port/input_bridge.c: nonzero while an on-track race is live. The ghost Import writes to the
// SRAM ghost slot, which must not race the game fiber, so the Import button is disabled in-race.
extern "C" int gdx_input_in_gameplay(void);
extern "C" void gdx_game_request_reset(void);
// R8 Step 1: deletion-gate verdict (port/disk_buffer.cpp). 1 iff this boot reconstructed the EK disk
// from fzerox-disk.o2r AND proved it byte-identical to the R7 managed copy. The Data & Files panel
// offers disk deletion ONLY on a passed verdict; it never deletes anything itself.
extern "C" int gdx_disk_archive_verified(void);

// From port/n64_gfx_bridge.cpp: R6-P2 frame-interpolation telemetry for the Graphics tab's
// "subframes last tick" status line. Declared here rather than pulling in n64_gfx_bridge.h (this
// TU doesn't otherwise need the gfx bridge's internals) — signatures match the header exactly.
extern "C" int gdx_gfx_interp_last_subframes(void);
extern "C" double gdx_gfx_interp_last_t(void);
// Real-FPS visibility (owner requirement, 2026-07-23): true presented frames/sec, the live master
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
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Changed from the default (stock) value.");
    }
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
// "Data & Files" (General tab): live on-disk state for the three original setup inputs, per
// R4/R7's C-R4.3/C-R7.3 UX contract. Every line below is backed by a live, cheaply-rechecked
// signal rather than static copy -- see the per-row comments for exactly what is and is not
// determinable from this file (gdx_menu.cpp cannot touch disk_buffer.cpp, which owns the IPL/EK
// disk load state and is out of scope for this wave; we report filesystem + archive-mount facts
// instead of guessing which source disk_buffer.cpp actually read from).
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
    // No live "which source is loaded" getter is exposed outside disk_buffer.cpp (out of scope this
    // wave), so this reports the two independently-checkable facts instead of guessing: whether the
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
    // Reports presence of the original, of the R7 managed copy (gdx::ManagedDiskPath), and of the R8
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

    // ── Archive coverage (R1 telemetry: gdx_segment_source_fallback_total / FamilyStats) ───────
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

// Bump whenever the Page/Header enum ordering changes so persisted ActivePage/ActiveHeader ints
// from an older layout aren't reinterpreted against the new ordering (same numeric header/page
// value can silently point at a different tab after a reorder).
static constexpr int kMenuLayoutVersion = 2;

void GdxMenu::InitElement() {
    const int header = CVarGetInteger("gSettings.Menu.ActiveHeader", static_cast<int>(Header::Settings));
    const int page = CVarGetInteger("gSettings.Menu.ActivePage", static_cast<int>(Page::General));
    const int layoutVersion = CVarGetInteger("gSettings.Menu.LayoutVersion", 0);
    if (header >= static_cast<int>(Header::Settings) && header <= static_cast<int>(Header::DevTools)) {
        mActiveHeader = static_cast<Header>(header);
    }
    if (page >= static_cast<int>(Page::General) && page <= static_cast<int>(Page::GfxDebugger)) {
        mActivePage = static_cast<Page>(page);
    }
    if (HeaderForPage(mActivePage) != mActiveHeader) {
        mActivePage = FirstPageForHeader(mActiveHeader);
    }
    if (layoutVersion != kMenuLayoutVersion) {
        // Stored page/header indices were persisted under a different Page/Header ordering;
        // a matching numeric value could now name a different tab, so reset to defaults rather
        // than trust the stale mapping.
        mActiveHeader = Header::Settings;
        mActivePage = Page::General;
        CVarSetInteger("gSettings.Menu.ActiveHeader", static_cast<int>(mActiveHeader));
        CVarSetInteger("gSettings.Menu.ActivePage", static_cast<int>(mActivePage));
        CVarSetInteger("gSettings.Menu.LayoutVersion", kMenuLayoutVersion);
        CVarSave();
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
            pageButton(Page::Graphics);
            pageButton(Page::Audio);
            pageButton(Page::Controls);
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
            pageButton(Page::InputViewer);
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
        case Page::Stats: DrawStatsMenu(); break;
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
        { Page::Gameplay, "gameplay transitions autosave ghost" },
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
    if (page <= Page::Controls) return Header::Settings;      // General .. Controls
    if (page <= Page::Practice) return Header::Enhancements;  // EnhancementGraphics .. Practice
    if (page <= Page::Content) return Header::Workshop;       // Ghosts .. Content
    if (page == Page::OnlineOverview) return Header::Online;
    return Header::DevTools;                                  // DeveloperGeneral .. GfxDebugger
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
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("How opaque this menu's backdrop is over the game (35%% = most see-through).");
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
    GdxDrawDataAndFilesPanel();
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
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Render scale relative to the window size. 1.00x = native; higher is\n"
                              "sharper but costs GPU. Applies immediately.");
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
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Multi-sample anti-aliasing. Higher smooths edges at a GPU cost.\n"
                              "Off (1x) = stock. Applies immediately.");
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
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("How textures are sampled. Three-point mimics the N64 (stock);\n"
                              "Linear is smoother; None is sharp/pixelated. Applies immediately.");
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
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Syncs presentation to the display refresh to avoid tearing.\n"
                              "On = stock. Turn off if you use Frame pacing.");
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

    // Z-fighting mode — CVar gZFightingMode (enum), default 0 (= 1:1). Consumed live by the active
    // Fast3D backend when it builds the rasterizer state for DECAL z-mode polygons: it sets the
    // SlopeScaledDepthBias applied to coplanar decal surfaces (track markings, shadows, surface
    // text) so they don't z-fight against the geometry they sit on (gfx_direct3d11.cpp:724, and the
    // matching gfx_opengl/gfx_metal switches). Mode 1 scales the bias by render height to mimic the
    // N64's own decal offset; Mode 2 uses a stronger bias that stops far decals from vanishing.
    // Only visible where decal geometry coexists with its base surface, so the effect is subtle.
    {
        static const char* const kZLabels[] = { "Disabled", "N64-style (scaled)", "No vanishing decals" };
        int idx = CVarGetInteger("gZFightingMode", 0);
        if (idx < 0 || idx > 2) {
            idx = 0;
        }
        if (ImGui::Combo("Z-fighting reduction", &idx, kZLabels, 3)) {
            CVarSetInteger("gZFightingMode", idx);
            GdxSaveCvars();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Adjusts the depth bias on decal surfaces (track markings, shadows)\n"
                              "so they don't shimmer against the road. Disabled = stock.");
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
        GdxModifiedMarker(!on); // default is on
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
        GdxModifiedMarker(on); // default is off
    }

    // Draw distance — CVar gEnhancements.Graphics.DrawDistance (%, default 100 = stock). Scales each
    // course's own far-render cutoff per-venue (course.c Course_Draw); 100% is bit-exact.
    //
    // SLIDER CAPPED AT 200% ON PURPOSE (effective ceiling, honest UI). The CVar multiplies the
    // per-chunk cull threshold (sCourseFarRenderDistance * scale), but the track itself is streamed
    // as a fixed set of chunks that Course_SegmentsInit builds only out to a bounded horizon
    // (gSegmentChunks, capped at SEGMENT_CHUNK_COUNT — course.c). By ~200% the raised cull threshold
    // already clears the depth of the furthest chunk that was ever built, so pushing the scale
    // higher un-culls nothing: there is no loaded geometry beyond that point to draw. Anything past
    // ~200% therefore produced no visible change (owner-observed), so the slider stops at the real
    // maximum rather than advertising dead range. This is a content/streaming limit, not a code
    // clamp that could simply be raised.
    {
        int dd = CVarGetInteger("gEnhancements.Graphics.DrawDistance", 100);
        if (dd < 100) {
            dd = 100;
        }
        if (dd > 200) {
            dd = 200;
        }
        if (ImGui::SliderInt("Draw distance (%)", &dd, 100, 200)) {
            if (dd < 100) {
                dd = 100;
            }
            if (dd > 200) {
                dd = 200;
            }
            CVarSetInteger("gEnhancements.Graphics.DrawDistance", dd);
            GdxSaveCvars();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Extends how far each track's own geometry renders (100%% = stock,\n"
                              "scales per-venue). 200%% is the effective max: beyond it the track's\n"
                              "streamed geometry runs out, so there is nothing further to draw.");
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
        GdxModifiedMarker(on); // default is off
    }

    ImGui::Separator();
    ImGui::TextDisabled("Enhancements (parity-gated)");
    // Frame pacing — CVar gEnhancements.Graphics.FramePacing, default 0. libultraship's Fast3D
    // backend already caps the loop to ~60fps, so this is opt-in: when on, port/gdx_frame_pacer.c
    // holds the host loop to the true N64 NTSC field rate (~59.94Hz) with a wall-clock sleep+spin.
    // Recommend VSync OFF while on (a display-refresh present beats against the fixed schedule).
    {
        bool interpOn = CVarGetInteger("gEnhancements.Graphics.FrameInterpolation", 0) != 0;
        bool on = CVarGetInteger("gEnhancements.Graphics.FramePacing", 0) != 0;
        ImGui::BeginDisabled(interpOn);
        if (ImGui::Checkbox("Frame pacing (59.94 Hz)", &on)) {
            CVarSetInteger("gEnhancements.Graphics.FramePacing", on ? 1 : 0);
            GdxSaveCvars();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (interpOn) {
                ImGui::SetTooltip("Interpolation owns pacing; frame pacing is unavailable while on.");
            } else {
                ImGui::SetTooltip("Experimental. The renderer already limits the game to ~60 fps; this\n"
                                  "pins the loop to the true N64 rate (59.94 Hz). Turn VSync OFF when using it.");
            }
        }
        GdxModifiedMarker(on); // default is off
    }

    // Frame interpolation — CVar gEnhancements.Graphics.FrameInterpolation, default 0. EXPERIMENTAL.
    // Read LIVE every tick (gdx_interp::P2HostActive / port/gdx_frame_pacer.c), so this toggle takes
    // effect on the next tick like FramePacing above — no restart needed. Mutually exclusive with
    // Frame pacing (both are pacing owners); this one takes priority when on (see BeginDisabled above).
    {
        bool on = CVarGetInteger("gEnhancements.Graphics.FrameInterpolation", 0) != 0;
        if (ImGui::Checkbox("Frame Interpolation (EXPERIMENTAL)", &on)) {
            CVarSetInteger("gEnhancements.Graphics.FrameInterpolation", on ? 1 : 0);
            GdxSaveCvars();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Interpolates rendering between 60Hz logic ticks for smoother motion on\n"
                              "high-refresh displays. EXPERIMENTAL: visual artifacts are possible during\n"
                              "camera cuts/transitions until further phases mature. VSync ON is\n"
                              "recommended. Adds about half a tick of latency. Bypasses Frame pacing\n"
                              "while on. Default OFF.");
        }
        GdxModifiedMarker(on); // default is off
        if (on) {
            // P5 completeness: gEnhancements.Graphics.InterpDebugOverlay, default 0. Purely
            // diagnostic (no rendering/pacing effect) — gates the "subframes last tick" line so it
            // is opt-in rather than always-on clutter once interpolation is enabled. Indented under
            // the toggle it belongs to and only shown while Frame Interpolation is on.
            ImGui::Indent();
            bool overlayOn = CVarGetInteger("gEnhancements.Graphics.InterpDebugOverlay", 0) != 0;
            if (ImGui::Checkbox("Debug overlay", &overlayOn)) {
                CVarSetInteger("gEnhancements.Graphics.InterpDebugOverlay", overlayOn ? 1 : 0);
                GdxSaveCvars();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Show live sub-frame statistics.");
            }
            if (overlayOn) {
                const int sub = gdx_gfx_interp_last_subframes();
                const double t = gdx_gfx_interp_last_t();
                ImGui::TextDisabled("subframes last tick: %d (t=%.2f)", sub, t);
            }

            // Target-rate mode — CVar gEnhancements.Graphics.InterpTargetMode, default 0 (Match
            // Refresh Rate). Read LIVE by main.cpp's per-tick M derivation, same idiom as the master
            // toggle above. The checkbox itself is never disabled (it IS the mode switch); it gates
            // the Target FPS slider below via BeginDisabled, mirroring the FramePacing/
            // FrameInterpolation disabled-dependency idiom further up this function.
            bool matchRefresh = CVarGetInteger("gEnhancements.Graphics.InterpTargetMode", 0) == 0;
            if (ImGui::Checkbox("Match Refresh Rate", &matchRefresh)) {
                CVarSetInteger("gEnhancements.Graphics.InterpTargetMode", matchRefresh ? 0 : 1);
                GdxSaveCvars();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Targets your monitor's current refresh rate.");
            }

            // Target FPS — CVar gEnhancements.Graphics.InterpTargetFps, default 120. Only consulted
            // in Capped mode (Match Refresh Rate off); disabled here otherwise, like FramePacing is
            // disabled while FrameInterpolation owns pacing.
            {
                int targetFps = CVarGetInteger("gEnhancements.Graphics.InterpTargetFps", 120);
                if (targetFps < 60) {
                    targetFps = 60;
                }
                if (targetFps > 480) {
                    targetFps = 480;
                }
                ImGui::BeginDisabled(matchRefresh);
                if (ImGui::SliderInt("Target FPS", &targetFps, 60, 480, "%d FPS")) {
                    if (targetFps < 60) {
                        targetFps = 60;
                    }
                    if (targetFps > 480) {
                        targetFps = 480;
                    }
                    CVarSetInteger("gEnhancements.Graphics.InterpTargetFps", targetFps);
                    GdxSaveCvars();
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (matchRefresh) {
                        ImGui::SetTooltip("Match Refresh Rate is on; the target follows your monitor\n"
                                          "instead. Disable it to set a fixed target here.");
                    } else {
                        ImGui::SetTooltip("Interpolation target frame rate. Values above your refresh rate\n"
                                          "waste GPU without improving output. Each 60fps of target adds a\n"
                                          "full render pass per tick.");
                    }
                }
            }
            ImGui::Unindent();
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

    // Output backend selection — CVar gEnhancements.Audio.Backend (0=Auto, 1=WASAPI, 2=SDL).
    // Applied at startup in main.cpp's InitAudio; Auto keeps libultraship's per-platform default
    // (WASAPI on Windows, SDL on Linux). Only backends that exist on this platform are offered:
    // WASAPI is Windows-only, and on Linux SDL routes to PipeWire/PulseAudio/ALSA.
    ImGui::SeparatorText("Output Device");
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
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Which OS audio output path to use. Auto keeps the platform default\n"
                              "(WASAPI on Windows, SDL elsewhere). Applies on restart.");
        }
        ImGui::TextDisabled("Applies on restart.");
    }

    // Engine — CVar gEnhancements.Audio.LLE, default 1. LLE = accurate (cxd4 RSP), HLE = fast.
    ImGui::SeparatorText("Synthesis Engine");
    {
        int lle = CVarGetInteger("gEnhancements.Audio.LLE", 1);
        if (ImGui::RadioButton("LLE (accurate)", lle == 1)) {
            CVarSetInteger("gEnhancements.Audio.LLE", 1);
            GdxSaveCvars();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Low-level RSP emulation (cxd4). Most accurate; the default.");
        }
        if (ImGui::RadioButton("HLE (fast)", lle == 0)) {
            CVarSetInteger("gEnhancements.Audio.LLE", 0);
            GdxSaveCvars();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("High-level audio emulation. Faster, less accurate.");
        }
        GdxModifiedMarker(lle == 0); // default is LLE
    }

    // Output reconstruction filter — CVar gEnhancements.Audio.LowPassHz, default 15000. A value of
    // 0 disables the filter; any value 500..16000 is the low-pass cutoff. The enable checkbox
    // toggles between 0 (off) and the remembered/last cutoff (on).
    ImGui::SeparatorText("Reconstruction Filter");
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
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Low-pass filter on the reconstructed output, softening high-frequency\n"
                              "aliasing. On = stock. Off disables the filter entirely.");
        }
        GdxModifiedMarker(!filterOn); // default is on

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
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Cutoff frequency of the reconstruction low-pass. Lower = softer/darker.\n"
                              "Enable the filter above to adjust this.");
        }
    }

    // Master volume — CVar gEnhancements.Audio.MasterVolume (0..100 %, default 100). Applied as a
    // final-stage gain multiply on the s16 output copy in os.cpp's osAiSetNextBuffer, read live
    // there each buffer (same live-CVar pattern as the low-pass). 100 = no-op (the multiply is
    // skipped entirely), so the default is bit-exact.
    ImGui::SeparatorText("Levels");
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
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Final output gain. 100%% = stock (bit-exact, no gain applied).");
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
        GdxModifiedMarker(!reverbOn); // default is on
    }

    // Latency / buffer size — CVar gEnhancements.Audio.BufferFrames (frames, default 4096, range
    // 1024..8192). Read ONCE at InitAudio (main.cpp), so a change applies only on the next restart
    // (hence the note). A larger reservoir rides out host scheduling jitter better but adds output
    // latency; a smaller one is snappier but more underrun-prone.
    ImGui::SeparatorText("Latency");
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
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Audio buffer size. Larger rides out host jitter (fewer dropouts) but\n"
                              "adds latency; smaller is snappier but more underrun-prone. Applies on restart.");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(applies on restart)");
    }

    ImGui::SeparatorText("More");
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
        GdxModifiedMarker(on); // default is off
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
        GdxModifiedMarker(!on); // default is on
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
        GdxModifiedMarker(on); // default is off
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
        GdxModifiedMarker(on); // default is off
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
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Browse your per-course player-ghost library and export ghosts to .gdg.");
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
        GdxModifiedMarker(on); // default is off
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
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Size of the on-screen input overlay.");
    }
    float opacity = std::clamp(CVarGetFloat("gInputViewer.Opacity", 1.0f), 0.2f, 1.0f);
    if (ImGui::SliderFloat("Overlay opacity", &opacity, 0.2f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
        CVarSetFloat("gInputViewer.Opacity", opacity);
        GdxSaveCvars();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Transparency of the input overlay.");
    }
    bool dragging = CVarGetInteger("gInputViewer.EnableDragging", 1) != 0;
    if (ImGui::Checkbox("Enable dragging", &dragging)) {
        CVarSetInteger("gInputViewer.EnableDragging", dragging ? 1 : 0);
        GdxSaveCvars();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Lets you reposition the overlay by dragging it with the mouse.");
    }
    bool background = CVarGetInteger("gInputViewer.ShowBackground", 1) != 0;
    if (ImGui::Checkbox("Show background layer", &background)) {
        CVarSetInteger("gInputViewer.ShowBackground", background ? 1 : 0);
        GdxSaveCvars();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Draws the controller-body backdrop behind the buttons.");
    }
    bool dpad = CVarGetInteger("gInputViewer.ShowDpad", 0) != 0;
    if (ImGui::Checkbox("Show D-pad layers", &dpad)) {
        CVarSetInteger("gInputViewer.ShowDpad", dpad ? 1 : 0);
        GdxSaveCvars();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Includes the D-pad in the overlay (off by default; F-Zero X does not use it).");
    }
    int outlineMode = std::clamp(CVarGetInteger("gInputViewer.ButtonOutlineMode", 1), 0, 3);
    const char* outlineLabels[] = { "Always shown", "Shown while released", "Shown while pressed", "Hidden" };
    if (ImGui::Combo("Button outlines", &outlineMode, outlineLabels, IM_ARRAYSIZE(outlineLabels))) {
        CVarSetInteger("gInputViewer.ButtonOutlineMode", outlineMode);
        GdxSaveCvars();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When each button's outline is drawn relative to its pressed state.");
    }
    bool analogValues = CVarGetInteger("gInputViewer.ShowAnalogValues", 0) != 0;
    if (ImGui::Checkbox("Show analog values", &analogValues)) {
        CVarSetInteger("gInputViewer.ShowAnalogValues", analogValues ? 1 : 0);
        GdxSaveCvars();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Prints the raw analog-stick X/Y numbers next to the stick.");
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

void GdxMenu::DrawStatsMenu() {
    bool showFps = GdxWindowVisible("FPS Counter");
    if (ImGui::Checkbox("Show FPS counter overlay", &showFps)) {
        GdxToggleWindow("FPS Counter");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Toggles a small always-on-top frames-per-second overlay.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Uses the same frame metrics as Stats");
    ImGui::Separator();

    // Real presented-FPS visibility (owner requirement, 2026-07-23). When Frame Interpolation is on
    // the sim runs at 60 Hz but the renderer presents multiple sub-frames per tick, so ImGui's own
    // io.Framerate (which counts one frame per present) is the true presented rate. We show it
    // alongside the fixed 60 Hz logic rate ("144 fps (sim 60 Hz)") plus the live sub-frame count and
    // the previous tick's tween/snap breakdown, so a "cost without benefit" regression (lerped == 0)
    // is visible at a glance. These reuse the existing bridge getters — no extra per-frame cost.
    if (gdx_gfx_interp_host_active() != 0) {
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
                                   "Interpolated slots: 0 (no tween — snapping every tick)");
            } else {
                ImGui::Text("Interpolated slots: %d   Snapped: %d", lerped, snapped);
            }
        }
        ImGui::Separator();
    }

    DrawToolWindowPage("Stats", "Live frame timing and renderer statistics.");
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
        GdxModifiedMarker(on); // default is off
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
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Enable or disable this pack. Takes effect on the next Reload or boot.");
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
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open the mods/ folder in your file browser (created if absent).");
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
        GdxModifiedMarker(on); // default is off
    }
    ImGui::TextDisabled("Dumped %d texture(s) this session -> dump/", gdx_workshop_dump_count());
    if (ImGui::Button("Open dump folder")) {
        GdxOpenFolder(GdxWorkshopDumpDir(true));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open the dump/ folder in your file browser (created if absent).");
    }

    // ── Asset Dump (per-class) ────────────────────────────────────────────────────────────────────
    // R8 Step 4b / Wave 4: offline per-class dump, native-first via the bundled gdx-extract (falls
    // back to tools/gen_dump_all.py in dev checkouts without the native binary). Implementation lives
    // in port/gdx_dump_launch.{h,cpp}; this block only READS the shared snapshot — every subprocess
    // runs on a detached worker thread, one child PER CLASS so a broken class never aborts the rest.
    ImGui::SeparatorText("Asset Dump");
    {
        // Discover the backend once (pure filesystem/PATH — no subprocess), and kick the async
        // `--list-classes` probe once. Both cached across frames via function-local statics.
        static gdx::DumpEnvironment sDumpEnv = gdx::GdxDumpDiscover();
        static bool sProbeKicked = (gdx::GdxDumpBeginClassListProbe(sDumpEnv), true);
        (void)sProbeKicked;

        ImGui::TextWrapped("Decode named assets straight from the extracted archive — no gameplay "
                           "needed. Runs the bundled dump tool once per selected class; results land "
                           "in dump/ (same place as the runtime texture dump).");
        if (!sDumpEnv.available) {
            ImGui::TextDisabled("%s", sDumpEnv.reason.c_str());
        }

        gdx::DumpBatchSnapshot snap = gdx::GdxDumpSnapshot();
        const bool running = snap.running;
        const std::vector<std::string> dumpClasses = gdx::GdxDumpCurrentClasses();

        // ── Per-class checkboxes (persisted as gEnhancements.Workshop.DumpClass.<name>, default on) ──
        ImGui::BeginDisabled(!sDumpEnv.available || running);
        if (ImGui::BeginTable("##DumpClasses", 2, ImGuiTableFlags_SizingStretchProp)) {
            for (const auto& cls : dumpClasses) {
                ImGui::TableNextColumn();
                std::string cvarKey = "gEnhancements.Workshop.DumpClass." + cls;
                bool on = CVarGetInteger(cvarKey.c_str(), 1) != 0; // default: every class checked
                std::string label = gdx::GdxDumpPrettyName(cls) + "##dumpclass_" + cls;
                if (ImGui::Checkbox(label.c_str(), &on)) {
                    CVarSetInteger(cvarKey.c_str(), on ? 1 : 0);
                    GdxSaveCvars();
                }
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

        // ── Cancel (cooperative: stops AFTER the current class finishes) ──
        ImGui::SameLine();
        ImGui::BeginDisabled(!running || snap.cancelRequested);
        if (ImGui::Button("Cancel")) {
            gdx::GdxDumpRequestCancel();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Stops after the current class finishes. The running class is left to "
                              "complete cleanly — no child process is killed.");
        }

        // ── Per-class progress lines + batch summary ──
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
                    ImGui::Text("%s: done — %d item(s) in %.1fs", pretty.c_str(), p.itemsDumped,
                                p.elapsedSeconds);
                } else {
                    ImGui::Text("%s: done — %.1fs", pretty.c_str(), p.elapsedSeconds);
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
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle the live frame-timing / renderer Stats window.");
    ImGui::SameLine();
    if (ImGui::Button("Open Console")) GdxToggleWindow("Console");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle the developer console and command history.");
    ImGui::SameLine();
    if (ImGui::Button("Open Gfx Debugger")) GdxToggleWindow("Gfx Debugger");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle the Fast3D display-list debugger.");
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
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Lets popped-out tool windows leave the main window (multi-monitor docking).\n"
                              "Applies on restart.");
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
