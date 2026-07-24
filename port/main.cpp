// G-Diffuser — port entry point.
// Slice 4c: granular libultraship init. The ControlDeck must be constructed AFTER the Context +
// ConsoleVariables exist (its GlobalSDLDeviceSettings reads CVars via Context::GetInstance()),
// so we use CreateUninitializedInstance + step-by-step Init rather than the one-shot CreateInstance.
// After init: register factories, bind assets, then hand off to the decomp boot (bootproc).

#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"
#include "ship/resource/archive/ArchiveManager.h"  // enumerate mounted archives for the version check
#include "ship/resource/archive/Archive.h"          // Archive::HasGameVersion / GetGameVersion / GetPath
#include "ship/audio/AudioPlayer.h"
#include "ship/audio/Audio.h"  // Ship::Audio + AudioBackend (startup audio-backend selection)
#include "resource/ResourceFactories.h"
// LUS's concrete N64 ControlDeck (creates the 4 controller ports + LUS::Controller instances,
// registers the standard N64 button set + default keyboard/gamepad mappings, and implements
// WriteToPad -> OSContPad). The port previously used GDiffuser::ControlDeck, a stub that extended
// the ABSTRACT Ship::ControlDeck: it created no ports and its WriteToPad did nothing, so nobody
// could read controller state. LUS::ControlDeck is `final`, so we use it directly rather than
// subclassing. The port's input read (gdx_lus_read_pad below) drives the decomp through it.
#include "libultraship/controller/controldeck/ControlDeck.h"
#include "libultraship/libultra/controller.h"  // OSContPad + MAXCONTROLLERS (for gdx_lus_read_pad)
#include "libultraship/bridge/consolevariablebridge.h"  // CVarGetInteger (audio buffer-frames CVar)
#include "fast/Fast3dWindow.h"
#include "ship/debug/Console.h"
#include "ship/window/gui/GuiWindow.h"
// In-game enhancement menu (F1) + the reusable LUS windows it surfaces. Purely additive:
// without these the port registers no menu / windows and F1 opens nothing.
#include "gdx_gui.h"                                    // Fast3D GUI with bundled menu fonts
#include "gdx_menu.h"                                   // GdxMenu (port/gdx_menu.{h,cpp})
#include "libultraship/window/gui/GfxDebuggerWindow.h"  // LUS::GfxDebuggerWindow (Developer tab)
#include "libultraship/window/gui/InputEditorWindow.h"  // LUS::InputEditorWindow (Controls tab)
#include "gdx_ghost_window.h"                            // GdxGhostWindow (Practice tab — saved-ghost browser)
#include "gdx_input_viewer.h"                            // GdxInputViewer (Settings tab + overlay)
#include "gdx_imgui_nav.h"                               // SDL-controller -> ImGui menu navigation feed
#include "port_log.h"
#include "rom_buffer.h"
#include "gdx_firstboot.h"  // first-boot setup + per-user data directory (runs before LUS init)
#include "gdx_firstboot_gui.h"  // in-window ImGui first-time setup flow (NeedsSetup path)
#include "gdx_fps_overlay.h" // optional Stats-backed FPS/frame-time counter
#include "gdx_perf.h"        // GDX_PERF=1 frame-time telemetry (spike attribution + summaries)
#include "gdx_extract_launch.h"  // runtime O2R extraction (produces generic.o2r before the mount)
#include "gdx_audio_thread.h"
#include "gdx_frame_pacer.h"  // optional wall-clock 60Hz pacer (gEnhancements.Graphics.FramePacing)
#include "n64_gfx_bridge.h"   // R6-P2 frame-interpolation host API (gdx_gfx_interp_*)
#include <SDL2/SDL.h>  // SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER): enable gamepad auto-detection

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern "C" void GDiffuser_LoadAllAssets(void); // generated asset binding loader (R2)
extern "C" void bootproc(void);                // decomp boot entry (src/sys/sys_main.c)
extern "C" void gdx_sched_init(void);          // R6: init cooperative fiber scheduler (host fiber)
extern "C" void gdx_sched_drain_deferred_wakes(void); // cross-thread mesg wakes -> run queue (host)
extern "C" void gdx_init_rom(int argc, char** argv, int archivesValidated); // S5: load ROM into host buffer (R4: archivesValidated gates no-ROM boot)
extern "C" void gdx_vi_tick(void);             // R6: advance VI + post retrace (wakes Main thread)
extern "C" void gdx_dispatch(void);            // R6: run game threads until quiescent
extern "C" void gdx_vi_present_fallback(void); // VI-scanout fallback: present CPU-drawn framebuffers
extern "C" void gdx_controller_poll(void);     // PORT: host keyboard -> decomp controller globals
extern "C" void gdx_fixed_aspect_tick(void);   // input_bridge: force 4:3 rendering for the EK editors
extern "C" int gdx_get_force_fixed_aspect(void); // libultraship interpreter.cpp: 1 while an EK editor forces 4:3
extern "C" void gdx_rdram_init(void);          // n64-rdram-buffer: allocate 8MB RDRAM before bootproc
extern "C" void gdx_register_host_range(void* ptr, size_t size); // n64_gfx_bridge: expose range for TryResolveAddress
extern "C" void gdx_register_main_module_range(void); // n64_gfx_bridge: resolve low32 EXE/BSS segment tokens
extern "C" void gdx_game_request_reset(void); // game.c: schedule a title reset at the game-flow boundary
extern "C" void gdx_disk_save_tick(void);      // disk_savefile: debounced flush of the 64DD save sidecar
extern "C" void gdx_disk_save_flush(void);     // disk_savefile: force-persist the pending sidecar journal
extern "C" void gdx_pcm_capture_init(void);    // gdx_audio_capture: arm PCM capture if GDX_PCM_CAPTURE set
extern "C" int  gdx_pcm_capture_finished(void);// gdx_audio_capture: 1 once the capture window finalized
extern "C" void gdx_pcm_capture_shutdown(void);// gdx_audio_capture: finalize an in-progress capture
extern "C" int  GdxSegmentSourcePreload(uint32_t romBase);                                  // gdx_segment_source: force-load a blob family
extern "C" int  GdxSegmentSourcePayload(uint32_t romBase, void** outPayload, uint32_t* outSize); // gdx_segment_source: resident payload getter

static void logStep(const char* s) {
    gdx_port_logf("[G-Diffuser] %s\n", s);
}

// ── R6-P2 frame-interpolation host support ───────────────────────────────────────────────────
// One 60 Hz logic-tick budget (N64 NTSC field = 1.001/60 s). The sim advances exactly one tick
// per iteration; when interpolation is on, presents are decoupled but this deadline still paces
// the sim at 60 Hz (the frame pacer is mutually excluded — see gdx_frame_pacer.c).
static constexpr double kGdxInterpTickSeconds = 1.001 / 60.0;
// VSync-off safety cap: with VSync off, presents don't block, so bound sub-frames per tick.
// With VSync on, the panel refresh naturally bounds this to ~refresh/60. This is the HARD CEILING
// for the per-tick M derivation below (SoH-style target-fps control): 8 covers the UI's 480fps
// Target FPS ceiling (480/60 = 8) and a typical 480Hz+ "Match Refresh Rate" panel alike.
static constexpr int kGdxInterpMaxSubframes = 8;

// Real-FPS visibility getters added to the gfx bridge (2026-07-23). Declared at file scope so no
// n64_gfx_bridge.h change is needed — same minimal-include idiom gdx_menu.cpp uses for the existing
// gdx_gfx_interp_last_* accessors. Used by the [interp-p2] telemetry line in the frame loop.
extern "C" double gdx_gfx_interp_presents_per_sec(void);
extern "C" int gdx_gfx_interp_last_lerped(void);
extern "C" int gdx_gfx_interp_last_snapped(void);

// Monotonic seconds clock the gfx bridge samples to derive each sub-frame's t. Shared epoch so
// the logic-deadline wait below converts a deadline back to the same time base.
static const std::chrono::steady_clock::time_point gGdxHostClockEpoch = std::chrono::steady_clock::now();
static double gdx_host_now_seconds(void) {
    using namespace std::chrono;
    return duration<double>(steady_clock::now() - gGdxHostClockEpoch).count();
}
// Hold the host thread until `deadlineSeconds` (same base as gdx_host_now_seconds). No-op if the
// presents already spent the tick budget (VSync-on case). Keeps the SIM locked to 60 Hz when the
// sub-frame loop finished early (VSync-off case) — this is interpolation's logic pacer.
static void gdx_host_pace_logic_until(double deadlineSeconds) {
    using namespace std::chrono;
    const auto target = gGdxHostClockEpoch +
                        duration_cast<steady_clock::duration>(duration<double>(deadlineSeconds));
    if (steady_clock::now() < target) {
        std::this_thread::sleep_until(target);
    }
}

// Match-Refresh robustness (R6-P2 FIELD-DEFECT FIX, 2026-07-23): some libultraship monitor-detection
// paths report 60 Hz on a high-refresh panel (observed on this hardware: Fast3dWindow::
// GetCurrentRefreshRate() -> GetMonitorHzPeriod returned 60 on a 143 Hz display), which pins Frame
// Interpolation's default "Match Refresh Rate" mode to M=1 (no interpolation at all). Cross-check the
// OS's current display frequency and use the higher of the two, so Match Refresh follows the real
// panel even when the backend under-reports. Returns 0 (unknown) on failure or a "hardware default"
// (0/1) frequency, so a genuine 60 Hz panel is unaffected (both sources agree on 60).
//
// MULTI-MONITOR FIX (2026-07-23): the original implementation took the MAX Hz over every active
// display path system-wide, not the Hz of the monitor the game window is actually on. On a mixed-
// refresh rig (e.g. a 60 Hz primary + a 144 Hz secondary) with the window on the lower-Hz panel,
// that max-over-paths value fed interpEffectiveTarget too high; with VSync on, the resulting M
// blocking presents per tick overran the 1/60s logic budget and the simulation ran in slow motion.
// We now resolve the monitor under the window rect first (posX/posY/width/height are already
// exposed by Ship::Window -- Fast3dWindow does not expose a raw HWND, so MonitorFromRect is used
// instead of MonitorFromWindow; both resolve to the same monitor for a non-minimized window) and
// match it by GDI device name, first against QueryDisplayConfig's per-path source name, then via a
// direct EnumDisplaySettingsW on that device. The old "max over all active paths" logic is kept
// ONLY as the last-resort fallback when the window's monitor cannot be resolved at all.
#ifdef _WIN32
static int gdx_display_hz_for_device(const wchar_t* deviceName) {
    DEVMODEW dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &dm) &&
        (dm.dmFields & DM_DISPLAYFREQUENCY) != 0 && dm.dmDisplayFrequency > 1) {
        return static_cast<int>(dm.dmDisplayFrequency);
    }
    return 0;
}

// windowPosX/Y/Width/Height: the game window's current screen rect (Ship::Window::GetPosX/GetPosY/
// GetWidth/GetHeight). outPath receives a short string naming which resolution path produced the
// returned Hz, for the [interp-diag] log.
static int gdx_os_display_refresh_hz(int32_t windowPosX, int32_t windowPosY, uint32_t windowWidth,
                                     uint32_t windowHeight, const char** outPath) {
    *outPath = "unresolved";

    // Resolve the monitor the window rect actually sits on, then its GDI device name.
    wchar_t deviceName[CCHDEVICENAME] = {};
    RECT windowRect = { windowPosX, windowPosY, windowPosX + static_cast<LONG>(windowWidth),
                        windowPosY + static_cast<LONG>(windowHeight) };
    HMONITOR hMonitor = MonitorFromRect(&windowRect, MONITOR_DEFAULTTONEAREST);
    if (hMonitor != nullptr) {
        MONITORINFOEXW info;
        ZeroMemory(&info, sizeof(info));
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(hMonitor, &info)) {
            wcsncpy(deviceName, info.szDevice, CCHDEVICENAME - 1);
        }
    }

    // Primary source: QueryDisplayConfig, matched to the window's monitor by source device name so
    // we get the EXACT vertical-sync rate as a rational (numerator/denominator) for THAT panel —
    // EnumDisplaySettings (dmDisplayFrequency) and the DXGI backend's GetMonitorHzPeriod can round
    // down to 60 on some high-refresh panels (observed on a 143 Hz monitor).
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) == ERROR_SUCCESS &&
        pathCount > 0) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(),
                               nullptr) == ERROR_SUCCESS) {
            if (deviceName[0] != L'\0') {
                for (UINT32 i = 0; i < pathCount; ++i) {
                    DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
                    sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                    sourceName.header.size = sizeof(sourceName);
                    sourceName.header.adapterId = paths[i].sourceInfo.adapterId;
                    sourceName.header.id = paths[i].sourceInfo.id;
                    if (DisplayConfigGetDeviceInfo(&sourceName.header) == ERROR_SUCCESS &&
                        _wcsicmp(sourceName.viewGdiDeviceName, deviceName) == 0) {
                        const DISPLAYCONFIG_RATIONAL& r = paths[i].targetInfo.refreshRate;
                        if (r.Denominator != 0) {
                            const double hz =
                                static_cast<double>(r.Numerator) / static_cast<double>(r.Denominator);
                            const int rounded = static_cast<int>(hz + 0.5);
                            if (rounded > 1) {
                                *outPath = "querydisplayconfig:window-monitor";
                                return rounded;
                            }
                        }
                    }
                }
            }
            // Window's monitor didn't resolve or didn't match a QueryDisplayConfig source: last-
            // resort fallback identical to the pre-fix behavior (max over all active paths).
            double best = 0.0;
            for (UINT32 i = 0; i < pathCount; ++i) {
                const DISPLAYCONFIG_RATIONAL& r = paths[i].targetInfo.refreshRate;
                if (r.Denominator != 0) {
                    const double hz = static_cast<double>(r.Numerator) / static_cast<double>(r.Denominator);
                    if (hz > best) {
                        best = hz;
                    }
                }
            }
            const int rounded = static_cast<int>(best + 0.5);
            if (rounded > 1) {
                *outPath = "querydisplayconfig:max-over-paths-fallback";
                return rounded;
            }
        }
    }

    // Secondary path: EnumDisplaySettingsW directly on the window's monitor device name.
    if (deviceName[0] != L'\0') {
        const int hz = gdx_display_hz_for_device(deviceName);
        if (hz > 0) {
            *outPath = "enumdisplaysettings:window-monitor";
            return hz;
        }
    }

    // Last-resort fallback: the primary display's current mode (window monitor unresolved).
    DEVMODEW dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) &&
        (dm.dmFields & DM_DISPLAYFREQUENCY) != 0 && dm.dmDisplayFrequency > 1) {
        *outPath = "enumdisplaysettings:primary-fallback";
        return static_cast<int>(dm.dmDisplayFrequency);
    }
    return 0;
}
#else
static int gdx_os_display_refresh_hz(int32_t, int32_t, uint32_t, uint32_t, const char** outPath) {
    *outPath = "unavailable";
    return 0;
}
#endif

// LUS ControlDeck's connected-port bitmask. ControlDeck::Init() stores this pointer and sets bit
// 0; the Input Editor reads it for its per-port connection display. It must outlive the deck, so
// it lives at file scope.
static uint8_t sGdxControllerBits = 0;

// ── PORT input read bridge: LUS ControlDeck -> decomp controller globals ─────────────────────────
// Called every frame from input_bridge.c (C) via gdx_controller_poll(). input_bridge.c cannot
// touch the C++ ControlDeck API directly, so this extern "C" shim reads port `port`'s resolved
// controller state — the Input Editor's mappings applied to the connected keyboard / SDL gamepad /
// mouse devices — and returns it as a standard N64 button bitmask + analog stick (-80..80).
//
// The heavy lifting is ControlDeck::WriteToPad(), which dispatches (virtually) to
// LUS::ControlDeck::WriteToOSContPad(): it calls SDL_PumpEvents() to refresh live gamepad state,
// honors AllGameInputBlocked() (leaving the pad zeroed while the ImGui overlay owns input), and
// reads every mapped device for every port into our OSContPad buffer. Keyboard / mouse / device
// add-remove SDL events were already delivered to the ControlDeck earlier this frame by
// Fast3dWindow::HandleEvents() (main frame loop), so this is the read half of an already-pumped
// pipeline — no separate per-frame controller pump is needed.
//
// Returns 1 on success, 0 if the ControlDeck is not available yet (caller degrades to zero input).
extern "C" int gdx_lus_read_pad(int port, unsigned short* outButtons, signed char* outStickX,
                                signed char* outStickY) {
    if (outButtons != nullptr) {
        *outButtons = 0;
    }
    if (outStickX != nullptr) {
        *outStickX = 0;
    }
    if (outStickY != nullptr) {
        *outStickY = 0;
    }

    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return 0;
    }
    auto controlDeck = ctx->GetControlDeck();
    if (controlDeck == nullptr || port < 0 || port >= MAXCONTROLLERS) {
        return 0;
    }

    // WriteToOSContPad OR-accumulates into pad->button and only overwrites a stick axis when the
    // incoming value is 0 — it never clears the buffer. So we MUST zero it every frame or buttons
    // would latch on. One entry per port (WriteToPad writes all MAXCONTROLLERS ports).
    OSContPad pads[MAXCONTROLLERS];
    std::memset(pads, 0, sizeof(pads));
    controlDeck->WriteToPad(pads);

    if (outButtons != nullptr) {
        *outButtons = pads[port].button;
    }
    if (outStickX != nullptr) {
        *outStickX = pads[port].stick_x;
    }
    if (outStickY != nullptr) {
        *outStickY = pads[port].stick_y;
    }
    return 1;
}

static void addArchiveCandidateRoots(std::vector<std::filesystem::path>& roots, std::filesystem::path start) {
    std::error_code ec;
    if (start.empty()) {
        return;
    }

    start = std::filesystem::absolute(start, ec);
    if (ec) {
        ec.clear();
    }
    if (std::filesystem::is_regular_file(start, ec)) {
        start = start.parent_path();
        ec.clear();
    }

    for (std::filesystem::path dir = start; !dir.empty(); dir = dir.parent_path()) {
        roots.push_back(dir);
        roots.push_back(dir / "assets" / "extracted");

        if (dir == dir.root_path()) {
            break;
        }
    }
}

// Workshop W0: is a mod pack disabled by the user? gEnhancements.Workshop.DisabledPacks is a
// comma-joined list of pack basenames (e.g. "20-portraits.o2r,legacy.o2r") persisted by the
// Workshop menu. Matching is case-insensitive on the basename. Mounting a pack is always safe (a
// pack with no matching keys is inert); this list lets the user keep a pack on disk but out of the
// load set without deleting it.
static bool workshopPackDisabled(const std::string& basename) {
    const char* raw = CVarGetString("gEnhancements.Workshop.DisabledPacks", "");
    if (raw == nullptr || raw[0] == '\0') {
        return false;
    }
    auto lower = [](std::string s) {
        for (char& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };
    const std::string target = lower(basename);
    std::string list = raw;
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        const size_t end = (comma == std::string::npos) ? list.size() : comma;
        std::string token = list.substr(start, end - start);
        // Trim surrounding whitespace.
        size_t b = token.find_first_not_of(" \t");
        size_t e = token.find_last_not_of(" \t");
        if (b != std::string::npos) {
            token = token.substr(b, e - b + 1);
            if (lower(token) == target) {
                return true;
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return false;
}

static std::vector<std::string> findArchivePaths(const char* argv0) {
    std::vector<std::filesystem::path> roots;
    std::error_code ec;
    addArchiveCandidateRoots(roots, std::filesystem::current_path(ec));
    if (argv0 != nullptr) {
        addArchiveCandidateRoots(roots, argv0);
    }

    std::vector<std::string> archives;
    // The game archive is fzerox.o2r (runtime-extracted); generic.o2r is accepted as a fallback
    // only when no fzerox.o2r exists — that is Torch's default output name, still used by the
    // in-tree dev archive (assets/extracted/generic.o2r). Never mount both: they carry the same
    // resource keys and double-mounting would just shadow one with the other.
    // R3 (C-R3.3): n64ddipl.o2r carries the 64DD IPL font block as its own dedicated archive so the
    // IPL ROM file becomes deletable after setup. Its absence is tolerated — gdx_ddipl_load falls back
    // to the raw N64DDIPLROM.n64 (retained until R4) — and it is unversioned, which the HasGameVersion
    // mount gate already skips, so it mounts cleanly alongside the game archives.
    // R8 Step 1 (analogous): fzerox-disk.o2r carries the whole 64DD EK disk image so the raw .ndd and
    // the R7 managed copy become deletable once a boot reconstructs from it. Also unversioned and
    // tolerated-absent — gdx_disk_load falls back to the managed copy / raw .ndd.
    for (const auto& nameGroup : { std::vector<const char*>{ "gdiffuser.o2r", "f3d.o2r" },
                                   std::vector<const char*>{ "fzerox.o2r", "generic.o2r" },
                                   std::vector<const char*>{ "n64ddipl.o2r" },
                                   std::vector<const char*>{ "fzerox-disk.o2r" } }) {
        bool found = false;
        for (const char* name : nameGroup) {
            for (const auto& root : roots) {
                const auto candidate = root / name;
                if (std::filesystem::exists(candidate, ec) && std::filesystem::is_regular_file(candidate, ec)) {
                    archives.push_back(std::filesystem::absolute(candidate, ec).string());
                    found = true;
                    break;
                }
                ec.clear();
            }
            if (found) {
                break;
            }
        }
    }

    if (archives.empty()) {
        archives.push_back("gdiffuser.o2r");
        archives.push_back("fzerox.o2r");
    }

    // Workshop W0: append mods/*.o2r after the base archives. ArchiveManager registers files
    // last-wins, so a later archive overrides a same-path resource in an earlier one -- load order
    // is priority order. Scanning in case-insensitive lexicographic order gives users deterministic
    // control via a numeric filename prefix ("10-hifonts.o2r"). The first candidate root that has a
    // mods/ directory wins, matching the base-archive resolution above. Mounting is never gated by a
    // CVar (an unmatched pack is inert); the Tier-B texture-pack shim is the actual behavior switch.
    for (const auto& root : roots) {
        const auto modsDir = root / "mods";
        if (!std::filesystem::is_directory(modsDir, ec)) {
            ec.clear();
            continue;
        }
        std::vector<std::pair<std::string, std::string>> mods; // (sortKeyLower, absolutePath)
        for (const auto& entry : std::filesystem::directory_iterator(modsDir, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            std::string ext = entry.path().extension().string();
            std::string extLower = ext;
            for (char& c : extLower) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (extLower != ".o2r") {
                continue;
            }
            const std::string basename = entry.path().filename().string();
            std::string keyLower = basename;
            for (char& c : keyLower) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (workshopPackDisabled(basename)) {
                gdx_port_logf("[workshop] pack: %s (disabled)\n", basename.c_str());
                continue;
            }
            mods.emplace_back(keyLower, std::filesystem::absolute(entry.path(), ec).string());
        }
        std::sort(mods.begin(), mods.end());
        for (const auto& [keyLower, path] : mods) {
            archives.push_back(path);
            gdx_port_logf("[workshop] pack: %s\n",
                          std::filesystem::path(path).filename().string().c_str());
        }
        break; // first root with a mods/ dir wins, like the base archives
    }

    return archives;
}

int main(int argc, char** argv) {
    // First-boot setup + per-user data directory. MUST run before any libultraship path resolution
    // (below) so config, logs, the disk image, and the IPL ROM consolidate into the resolved data
    // directory. In the dev/portable layout (ROM next to the exe) this shows no wizard and leaves the
    // working directory untouched — it only resolves the ROM path so the ROM picker stays suppressed
    // for a headless launch. See port/gdx_firstboot.{h,cpp} + docs/FIRST_BOOT_DESIGN.md.
    gdx::FirstBootResult firstBoot = gdx::FirstBootRun((argc > 0) ? argv[0] : nullptr);
    if (firstBoot.status == gdx::FirstBootStatus::Aborted) {
        return 1;
    }

    // ── Runtime O2R asset extraction (contracts C1/C5/C6/C7/C8) ──────────────────────────────────
    // Produce (or refresh) <dataDir>/generic.o2r from the cartridge ROM using the packaged gdx-extract
    // child + decomp-recipes, BEFORE findArchivePaths (below) builds the mount list — the data dir is
    // a candidate root, so a freshly installed archive is picked up this same boot. This runs after
    // the ROM/disk/IPL are validated + consolidated into the data dir (first-boot) and while no window
    // exists yet, so progress UX is a modeless Win32 dialog on Windows and log-only on Linux.
    //
    // The ROM-next-to-exe layout is ALSO the normal end-user layout (SoH/BattleShip convention:
    // unzip, drop the ROM beside the exe, launch — the archive is generated beside the exe, which
    // is DevLayout's dataDir). Extraction is skipped only on a true development tree, detected by
    // the in-tree assets/extracted probe already providing generic.o2r — there, re-extraction would
    // be wasteful and could clobber a developer's working archive. Extraction NEVER blocks boot: on
    // any failure it logs an actionable line and the proven raw-ROM fallback carries the session (C6).
    //
    // SKIPPED for the NeedsSetup path: there is no ROM yet (the user has not provided one), so
    // extraction cannot run here. It is instead driven from the in-window setup flow below, after the
    // window exists, once the ROM/disk/IPL have been acquired. (firstBoot.romPath is empty here.)
    if (firstBoot.status != gdx::FirstBootStatus::NeedsSetup) {
        std::error_code cwdEc;
        const bool devTreeArchive = gdx::DevelopmentTreeProvidesArchive(
            firstBoot.exeDir, std::filesystem::current_path(cwdEc).string());
        if (devTreeArchive) {
            gdx_port_logf("[G-Diffuser] asset extraction: skipped (dev tree provides "
                          "assets/extracted/generic.o2r)\n");
        } else {
            gdx::ExtractOutcome extractOutcome = gdx::GdxExtractEnsureArchive(
                firstBoot.dataDir.c_str(), firstBoot.romPath.c_str(), firstBoot.exeDir.c_str());
            gdx_port_logf("[G-Diffuser] asset extraction: %s\n",
                          gdx::GdxExtractOutcomeString(extractOutcome));
        }
    }

    logStep("CreateUninitializedInstance");
    auto ctx = Ship::Context::CreateUninitializedInstance("G-Diffuser", "gdiffuser",
                                                          "gdiffuser.cfg.json");
    if (ctx == nullptr) { logStep("FATAL: CreateUninitializedInstance"); return 1; }

    logStep("InitLogging");
    if (gdx_log_file_enabled()) {
        ctx->InitLogging();
    } else {
        ctx->InitLogging(spdlog::level::off, spdlog::level::off, false);
    }
    logStep("InitConfiguration");    ctx->InitConfiguration();
    logStep("InitConsoleVariables"); ctx->InitConsoleVariables();

    // Order matches BattleShip's working sequence: ControlDeck -> ResourceManager -> Window.
    // Context + CVars exist now — safe to build the ControlDeck. LUS::ControlDeck's ctor creates
    // the 4 ports (each with a concrete LUS::Controller) and the standard N64 default mappings.
    // Bring up SDL's game-controller subsystem BEFORE the ControlDeck (its constructor scans for
    // connected gamepads via RefreshConnectedSDLGamepads). The DirectX 11 backend never calls
    // SDL_Init(SDL_INIT_VIDEO) (that is the SDL2 window path, unused here) and audio only inits
    // SDL_INIT_AUDIO -- so without this, SDL_NumJoysticks()==0 and NO controller (DualSense
    // included) is ever detected. SDL_INIT_GAMECONTROLLER implies JOYSTICK + EVENTS, and enables
    // both the boot scan and the hot-plug path (SDLAddRemoveDeviceEventHandler).
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        gdx_port_logf("[input] SDL_InitSubSystem(GAMECONTROLLER) FAILED: %s\n", SDL_GetError());
    } else {
        gdx_port_logf("[input] SDL gamecontroller subsystem up; %d joystick(s) present at boot\n",
                      SDL_NumJoysticks());
    }
    logStep("construct ControlDeck"); auto controlDeck = std::make_shared<LUS::ControlDeck>();
    logStep("InitControlDeck");       ctx->InitControlDeck(controlDeck);

    // Context::InitControlDeck() does NOT call ControlDeck::Init(), so do it explicitly here.
    // Init() wires the connected-port bitmask and, per port, either reloads the user's saved
    // mappings from gdiffuser.cfg.json or (on a fresh config) applies the default keyboard / mouse
    // / SDL-gamepad mappings. Without this, the controllers have no mappings and read all-zero.
    // Config + CVars are already initialized above, so this is safe now.
    logStep("ControlDeck::Init");     controlDeck->Init(&sGdxControllerBits);

    // ResourceManager MUST init before the window is constructed/inited.
    // Resolve archives from the working directory, executable directory, or extracted asset tree so
    // direct exe launches and command-line runs both see the same O2R data.
    auto archivePaths = findArchivePaths((argc > 0) ? argv[0] : nullptr);
    for (const auto& archivePath : archivePaths) {
        gdx_port_logf("[G-Diffuser] archive: %s\n", archivePath.c_str());
    }
    // Archive version gate.
    //
    // libultraship's built-in gate (the validHashes set passed to InitResourceManager) is
    // REJECT-ONLY and, critically, does NOT special-case archives that declare no version:
    // ArchiveManager::AddArchiveUnlocked compares every archive's GetGameVersion() (which
    // defaults to the sentinel 0xFFFFFFFF when the archive carries no "version" file) against
    // the set, and a non-empty set that lacks that value rejects — i.e. never mounts — the
    // archive outright (libultraship/src/ship/resource/archive/ArchiveManager.cpp:295-299).
    //
    // The port's CURRENTLY SHIPPED archives (f3d.o2r via tools/gen_f3d_o2r.py, texture packs
    // via tools/gen_texture_pack.py) do NOT stamp a "version" file or a numeric
    // manifest.json.game_version. Passing any non-empty validHashes today would therefore
    // reject all of them and break boot. So we keep the built-in gate DISABLED (empty set)
    // and instead run a post-mount check below. Two policies (contract C4):
    //   * generic.o2r is ENFORCING. Torch stamps its version = the US-rev0 ROM CRC (0x78D90EB3);
    //     the bridge's SETTIMG OTR rewrite path is not partial-resilient, so a mismatched
    //     generic.o2r (wrong region/rev, stale recipe) would render blank textures with no raw
    //     recovery. On mismatch we UNMOUNT it (RemoveArchive) so the proven-complete raw-ROM
    //     fallback carries the session — never a silently-wrong archive (C6, complete-or-absent).
    //   * every OTHER archive stays WARN-ONLY against kGdxExpectedArchiveVersion: unversioned
    //     archives (f3d.o2r, texture packs) pass through untouched, so the owner's boot is never
    //     broken, and a versioned foreign pack that mismatches is merely reported.
    static constexpr uint32_t kGdxExpectedArchiveVersion = 1u;        // schema v1 = first versioned O2R
    static constexpr uint32_t kGdxExpectedGenericRomCrc = 0x78D90EB3u; // C4: US-rev0 ROM CRC stamp
    logStep("InitResourceManager");   ctx->InitResourceManager(archivePaths, {}, 1);

    // R4 (C-R4.1): "archives validated" predicate for the no-ROM boot gate below. True iff the
    // fzerox.o2r/generic.o2r game archive is mounted AND survives this CRC gate (i.e. is NOT
    // unmounted by the version check in the block below). Captured here, at the point the gate's
    // outcome is fully known, and threaded into gdx_init_rom so a missing ROM can be tolerated
    // when the archive that actually serves the game's assets is present and correct.
    bool archivesValidated = false;
    {
        auto rm = ctx->GetResourceManager();
        auto am = (rm != nullptr) ? rm->GetArchiveManager() : nullptr;
        auto archives = (am != nullptr) ? am->GetArchives() : nullptr;
        // Collect generic.o2r paths to unmount AFTER the scan: RemoveArchive rebuilds the VFS and
        // mutates the internal archive list, so we must not remove while iterating the snapshot.
        std::vector<std::string> toUnmount;
        if (archives != nullptr) {
            for (const auto& archive : *archives) {
                if (archive == nullptr || !archive->HasGameVersion()) {
                    continue; // unversioned archive (f3d.o2r, texture packs) — nothing to validate
                }
                const std::string path = archive->GetPath();
                std::string basename = std::filesystem::path(path).filename().string();
                for (char& c : basename) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                const uint32_t got = archive->GetGameVersion();

                if (basename == "fzerox.o2r" || basename == "generic.o2r") {
                    // ENFORCING: the version's ROM-CRC must match the expected US-rev0 CRC.
                    // Covers both the installed name (fzerox.o2r) and Torch's default output name
                    // (generic.o2r, still used by the in-tree dev archive).
                    if (got != kGdxExpectedGenericRomCrc) {
                        gdx_port_logf(
                            "[G-Diffuser] ERROR: game archive \"%s\" version ROM-CRC is 0x%08X but this "
                            "build expects 0x%08X (US rev0). Unmounting it and booting from the raw ROM; "
                            "delete it (or the gdx_extract_state.cfg sidecar) to force a fresh "
                            "extraction.\n",
                            path.c_str(), got, kGdxExpectedGenericRomCrc);
#ifdef _WIN32
                        char msg[512];
                        snprintf(msg, sizeof(msg),
                                 "The extracted asset archive does not match your ROM:\n\n%s\n\nversion "
                                 "ROM-CRC 0x%08X, expected 0x%08X (US rev0).\n\nG-Diffuser will boot "
                                 "from the raw ROM. Delete this file to force a fresh extraction.",
                                 path.c_str(), got, kGdxExpectedGenericRomCrc);
                        MessageBoxA(nullptr, msg, "G-Diffuser — incompatible generic.o2r",
                                    MB_OK | MB_ICONWARNING);
#endif
                        toUnmount.push_back(path);
                    } else {
                        // R4 (C-R4.1): this is the "fzerox.o2r/generic.o2r mounted AND survived
                        // the CRC gate" condition -- the exact predicate the no-ROM boot gate
                        // needs. Both the installed name and Torch's generic dev-archive name
                        // are accepted by this same branch, so both satisfy the predicate.
                        archivesValidated = true;
                    }
                } else if (got != kGdxExpectedArchiveVersion) {
                    // WARN-ONLY for every other versioned archive.
                    gdx_port_logf(
                        "[G-Diffuser] ERROR: archive \"%s\" reports version %u but this build "
                        "expects %u. The archive is stale or incompatible. Regenerate it with "
                        "tools/gen_f3d_o2r.py (f3d.o2r) / tools/gen_texture_pack.py, then relaunch.\n",
                        path.c_str(), got, kGdxExpectedArchiveVersion);
#ifdef _WIN32
                    char msg[512];
                    snprintf(msg, sizeof(msg),
                             "Asset archive is stale or incompatible:\n\n%s\n\nreports version %u "
                             "but this build expects %u.\n\nRegenerate it with tools/gen_f3d_o2r.py "
                             "(f3d.o2r) / tools/gen_texture_pack.py, then relaunch.",
                             path.c_str(), got, kGdxExpectedArchiveVersion);
                    MessageBoxA(nullptr, msg, "G-Diffuser — incompatible asset archive",
                                MB_OK | MB_ICONWARNING);
#endif
                    // Warn-and-continue: libultraship already mounted the archive (empty gate),
                    // so we let it run rather than hard-aborting a possibly-still-usable build.
                }
            }
        }
        // Enforce C4 for generic.o2r: remove the mismatched archive from the VFS before any resource
        // is read (GDiffuser_LoadAllAssets runs later), leaving a clean archive-absent state.
        if (am != nullptr) {
            for (const auto& path : toUnmount) {
                am->RemoveArchive(path);
                gdx_port_logf("[G-Diffuser] unmounted incompatible archive: %s\n", path.c_str());
            }
        }
    }

    // Console must exist BEFORE the Gui is built: the Gui adds a ConsoleWindow whose Init()
    // (called by AddGuiWindow) registers commands via Context::GetConsole().
    logStep("InitCrashHandler");      ctx->InitCrashHandler();
    logStep("InitConsole");           ctx->InitConsole();
    ctx->GetConsole()->AddCommand(
        "reset",
        { [](std::shared_ptr<Ship::Console>, std::vector<std::string>, std::string* output) {
             gdx_game_request_reset();
             if (output != nullptr) {
                 *output = "Game reset requested.";
             }
             return 0;
         },
          "Reset the game to the title screen.", {} });

    // Now resource manager + console exist — safe to build and init the window.
    logStep("construct GdxFast3dGui");
    auto gui = std::make_shared<GdxFast3dGui>(std::vector<std::shared_ptr<Ship::GuiWindow>>());
    logStep("construct Fast3dWindow(gui)");
    auto window = std::make_shared<Fast::Fast3dWindow>(gui);
    logStep("InitWindow");            ctx->InitWindow(window);

    // ── In-game enhancement menu (F1) ────────────────────────────────────────────────────────
    // The Window + Gui exist now (InitWindow inited the Gui). Register the reusable LUS dev/input
    // windows that the Gui ctor does NOT auto-add, then install the port's full-screen menu. This is what
    // makes F1 actually open a menu: libultraship already wires the F1 / Esc / Gamepad-Back toggle
    // (Gui.cpp:206-213), but the port previously passed an empty window vector (above) and set no
    // menu bar. Purely additive — nothing below runs until the user presses F1.
    // See docs/IMGUI_MENU_SCOPE.md + port/gdx_menu.{h,cpp}.
    logStep("register enhancement menu + dev/input windows");
    {
        auto pgui = ctx->GetWindow()->GetGui();

        // Gfx Debugger: LUS::GfxDebuggerWindow. Registered name "Gfx Debugger", visibility CVar
        // "gGfxDebuggerEnabled" (GfxDebuggerWindow.h:23-24; libultraship/cmake/cvars.cmake:12).
        // Not auto-registered by the Gui ctor, so add it explicitly (AddGuiWindow calls its
        // Init()). Toggled from the Developer menu. CVar names are string literals because the
        // libultraship CVAR_* macros are not in scope for the port target (cvars.cmake is
        // include()d only inside libultraship/src).
        pgui->AddGuiWindow(std::make_shared<LUS::GfxDebuggerWindow>("gGfxDebuggerEnabled", "Gfx Debugger"));

        // Input Editor: LUS::InputEditorWindow. Registered name "Input Editor", visibility CVar
        // "gControllerConfigurationEnabled" (InputEditorWindow.h:25; cvars.cmake:9). Also not
        // auto-registered. Toggled from the Controls menu ("Controller Configuration...").
        pgui->AddGuiWindow(
            std::make_shared<LUS::InputEditorWindow>("gControllerConfigurationEnabled", "Input Editor"));

        // Ghost Browser: GdxGhostWindow (port/gdx_ghost_window.{h,cpp}). Registered name
        // "Ghost Browser", visibility CVar "gEnhancements.Practice.GhostBrowserOpen" (default 0 =
        // hidden, applied by the GuiWindow two-arg ctor). Toggled from the Practice menu. Browses the
        // per-course PC player-ghost library and exports the selected entry to .gdg.
        pgui->AddGuiWindow(
            std::make_shared<GdxGhostWindow>("gEnhancements.Practice.GhostBrowserOpen", "Ghost Browser"));

        // Input Viewer: a native N64 overlay fed from the exact mapped state seen by the game.
        pgui->AddGuiWindow(std::make_shared<GdxInputViewer>());

        // Minimal performance overlay: uses ImGui's live Framerate/DeltaTime, exactly like Stats.
        pgui->AddGuiWindow(std::make_shared<GdxFpsOverlay>());

        // Frame-pacing default is now OFF on ALL platforms. Owner device evidence (ROG Ally X,
        // Linux): with the pacer OFF the game holds a clean 60 FPS because the backend's vsync caps
        // correctly; the port pacer misbehaves when ON. Windows already ran with the pacer OFF (the
        // DXGI backend's own sleep+spin limiter holds EndFrame() to 60fps), so this only changes the
        // Linux default. The setting stays fully functional — a user who hits the high-refresh
        // free-run symptom (panel refresh with broken vsync) can re-enable the Frame Pacer from the
        // Settings menu, and that choice persists.
        //
        // This runs BEFORE the GdxMenu ctor registers the CVar: RegisterInteger only sets a value
        // when the CVar does not yet exist, so a value already loaded from the user's config (their
        // explicit toggle) still wins, while a fresh config gets the pacer disabled by default.
        CVarRegisterInteger("gEnhancements.Graphics.FramePacing", 0);

        // R6-P2: frame-interpolation master toggle, DEFAULT OFF. When 1, the host loop below
        // decouples render from logic — the sim still advances exactly one tick per iteration
        // (never re-cadenced), but the retained gfx task is replayed + presented as M wall-clock
        // sub-frames per tick (smooth motion on >60 Hz panels). Registered here at its stock-
        // reproducing default like FramePacing so a persisted user toggle still wins; the P5 menu
        // ctor also registers it. Interpolation and FramePacing are mutually-exclusive pacing
        // owners — the pacer no-ops itself while this is on (see gdx_frame_pacer.c).
        CVarRegisterInteger("gEnhancements.Graphics.FrameInterpolation", 0);
        // One-time migration to the new OFF default. A previous build defaulted the pacer ON for
        // Linux and force-enabled it via gdx.Migrations.LinuxFramePacingOn, persisting FramePacing:1
        // into existing configs — which the RegisterInteger above cannot override. Flip it back OFF
        // exactly once for every config that has not yet seen THIS migration (a second, distinct
        // marker key so it fires once more even for configs the old migration already touched).
        // We cannot distinguish a value the user deliberately set ON since then from the old
        // auto-migrated ON, so this is a deliberate one-time reset to the new default (preferred
        // over added complexity); the marker prevents a second reset, so a later manual re-enable in
        // the menu persists normally.
        if (CVarGetInteger("gdx.Migrations.FramePacingDefaultOff", 0) == 0) {
            CVarSetInteger("gdx.Migrations.FramePacingDefaultOff", 1);
            CVarSetInteger("gEnhancements.Graphics.FramePacing", 0);
            CVarSave();
            gdx_port_logf("[G-Diffuser] Frame pacer reset to OFF (new default). "
                          "Re-enable it in Settings if the game runs too fast.\n");
        }

        // Install the full-screen menu (Gui::SetMenu calls Init()). The GdxMenu ctor pins
        // visibility to "gOpenMenuBar" for configuration compatibility and registers the port's
        // gEnhancements.* CVars at their 1:1 defaults.
        pgui->SetMenu(std::make_shared<GdxMenu>());
    }

    logStep("InitAudio");
    {
        // Phase 3 (port/gdx_audio_thread.cpp): 4096 frames (~128ms) matches SoH's proven
        // reservoir size (engram design/audio-pipeline-hm-ports) -- large enough for the
        // dedicated audio thread's catch-up loop to ride out normal host scheduling jitter
        // without libultraship/.../os.cpp's old osAiGetLength under-report cushion (removed
        // for this path; see that file). Only matters once the dedicated thread is active —
        // the legacy fiber path (GDX_AUDIO_THREAD=0) never reads DesiredBuffered.
        //
        // Buffer size is now the CVar gEnhancements.Audio.BufferFrames (ImGui Audio tab), default
        // 4096 so a fresh config reproduces today's value exactly. It is read ONCE here at
        // InitAudio, so changing it in the menu is correctly labelled "(applies on restart)". The
        // CVar is registered at its 1:1 default by the GdxMenuBar ctor, which runs just above (menu
        // bar installed before InitAudio), so registration precedes this read; a persisted user
        // value loaded from gdiffuser.cfg.json at boot is honored regardless. The menu clamps to a
        // sensible 1024..8192 range; guard the low end here too in case the config is hand-edited.
        Ship::AudioSettings audioSettings{};
        int bufferFrames = CVarGetInteger("gEnhancements.Audio.BufferFrames", 4096);
        if (bufferFrames < 1024) {
            bufferFrames = 1024;
        } else if (bufferFrames > 8192) {
            bufferFrames = 8192;
        }
        audioSettings.DesiredBuffered = bufferFrames;
        ctx->InitAudio(audioSettings);

        // SoH-style audio backend selection. CVar gEnhancements.Audio.Backend:
        //   0 = Auto (default) -> keep libultraship's per-platform default (WASAPI on Windows,
        //                         SDL on Linux) exactly as ctx->InitAudio just chose it. No
        //                         behavior change on a stock config.
        //   1 = WASAPI, 2 = SDL -> override the active backend at startup, but only if that
        //                          backend is actually available on this platform (the Audio
        //                          subsystem builds the available list per-OS), so selecting
        //                          WASAPI on Linux is a no-op rather than a failure. The menu
        //                          combo (Audio tab) writes this CVar; it applies on restart.
        CVarRegisterInteger("gEnhancements.Audio.Backend", 0);
        int backendSel = CVarGetInteger("gEnhancements.Audio.Backend", 0);
        if (backendSel != 0) {
            auto audio = ctx->GetAudio();
            if (audio != nullptr) {
                Ship::AudioBackend want = Ship::AudioBackend::SDL;
                if (backendSel == 1) {
                    want = Ship::AudioBackend::WASAPI;
                } else if (backendSel == 2) {
                    want = Ship::AudioBackend::SDL;
                }
                auto avail = audio->GetAvailableAudioBackends();
                bool available =
                    avail != nullptr && std::find(avail->begin(), avail->end(), want) != avail->end();
                if (available && audio->GetCurrentAudioBackend() != want) {
                    audio->SetCurrentAudioBackend(want);
                }
            }
        }
    }
    logStep("InitEventSystem");       ctx->InitEventSystem();
    logStep("InitFileDropMgr");       ctx->InitFileDropMgr();

    // ── In-window first-time setup (NeedsSetup path) ─────────────────────────────────────────────
    // FirstBootRun deferred acquisition to here when the ROM/EK disk/IPL were missing. The window,
    // Gui, and FileDropMgr now exist, but the game has NOT booted (no RegisterResourceFactories /
    // LoadAllAssets / bootproc yet — all of which require the ROM). This runs the ImGui setup screen,
    // which acquires + validates + copies the three inputs, runs the O2R extraction with live progress,
    // hot-mounts the produced fzerox.o2r, and returns the installed ROM path. On completion, boot falls
    // through to RegisterResourceFactories → LoadAllAssets → … with firstBoot.romPath now set. If the
    // user closes the window during setup, exit cleanly (partial files persist; next launch resumes).
    if (firstBoot.status == gdx::FirstBootStatus::NeedsSetup) {
        std::string setupRomPath;
        if (!gdx::GdxFirstBootSetupRun(firstBoot.dataDir, firstBoot.exeDir, setupRomPath)) {
            // The dedicated audio thread has not started yet (gdx_audio_thread_start is below), so a
            // plain return is a clean exit here.
            gdx_port_logf("[G-Diffuser] first-time setup was closed before completion; exiting.\n");
            return 0;
        }
        firstBoot.romPath = setupRomPath;
        firstBoot.status = gdx::FirstBootStatus::SetupComplete;
    }

    // Phase 3: resolve the GDX_AUDIO_THREAD kill switch and, if enabled (default), start the
    // dedicated audio thread now. Safe this early -- it internally waits for
    // gAudioContextInitialized (set once decomp's Audio_Init runs, well after bootproc() below)
    // before producing anything.
    logStep("gdx_audio_thread_start()");
    gdx_audio_thread_start(argc, argv);

    logStep("RegisterResourceFactories");
    GDiffuser::RegisterResourceFactories(ctx->GetResourceManager()->GetResourceLoader());

    logStep("GDiffuser_LoadAllAssets");
    GDiffuser_LoadAllAssets();

    logStep("gdx_sched_init() — cooperative fiber scheduler");
    gdx_sched_init();

    logStep("gdx_init_rom() — load ROM asset buffer");
    {
        // Inject the first-boot-resolved ROM path as a trailing synthetic argv entry. rom_buffer.cpp
        // step 1 scans args in order and returns on the first that loads, so a real command-line ROM
        // (lower index) still wins; the injected path is only reached as the fallback. Loading via the
        // CLI-arg branch returns BEFORE rom_buffer's interactive picker, keeping the launch headless
        // and automatic once setup is complete. firstBoot.romPath outlives this call (declared above).
        std::vector<char*> romArgv(argv, argv + argc);
        if (!firstBoot.romPath.empty()) {
            romArgv.push_back(const_cast<char*>(firstBoot.romPath.c_str()));
        }
        gdx_init_rom(static_cast<int>(romArgv.size()), romArgv.data(), archivesValidated ? 1 : 0);
    }

    // R4 (C-R4.1): with archivesValidated, gdx_init_rom's own resolution failure path already
    // returns success with gdx_rom_buffer == nullptr / gdx_rom_size == 0 (archive-only boot) --
    // it does not exit(1). So this second null check is now the ACTUAL no-ROM boot gate:
    //   - gdx_rom_buffer == nullptr && archivesValidated: expected archive-only state, proceed.
    //   - gdx_rom_buffer == nullptr && !archivesValidated: gdx_init_rom's own FATAL/exit(1) path
    //     should already have terminated the process before we get here -- this branch is kept
    //     purely as defense-in-depth (same actionable error the old unconditional check used).
    if (gdx_rom_buffer == nullptr) {
        if (archivesValidated) {
            gdx_port_logf("[G-Diffuser] no ROM loaded; continuing archive-only boot (fzerox.o2r "
                          "validated). Raw-ROM fallback reads will be logged/zero-filled -- the R4 "
                          "soak (C-R4.2, GDX_STRICT_ARCHIVE telemetry) must show none for this to "
                          "be a supported end-user configuration.\n");
        } else {
            gdx_port_logf("[G-Diffuser] FATAL: no ROM loaded and no validated archive — cannot start game.\n");
            gdx_port_logf("[G-Diffuser] Place baserom.us.rev0.z64 next to the exe, pass it as an argument, "
                          "or complete first-boot setup so a validated fzerox.o2r archive is installed.\n");
#ifdef _WIN32
            MessageBoxA(nullptr,
                "No F-Zero X ROM was loaded, and no validated asset archive was found either.\n\n"
                "Place 'baserom.us.rev0.z64' next to G-Diffuser.exe,\n"
                "pass the ROM path as a command-line argument,\n"
                "or complete first-boot setup so a validated fzerox.o2r archive is installed.",
                "G-Diffuser — ROM required", MB_OK | MB_ICONERROR);
#endif
            return 1;
        }
    }

    logStep("gdx_rdram_init() — allocate 8MB RDRAM host buffer");
    gdx_rdram_init();

    // Register ROM buffer so TryResolveAddress can resolve low32(rom_ptr) addresses
    // (cockpit overlay textures compiled into EXE BSS at N64 addresses, accessed via
    // osVirtualToPhysical which truncates to low32 for non-RDRAM pointers).
    if (gdx_rom_buffer != nullptr) {
        gdx_register_host_range(gdx_rom_buffer, gdx_rom_size);
        gdx_port_logf("[rom] registered ROM buffer: base=%p low32=%08X size=0x%zx\n",
                      static_cast<void*>(gdx_rom_buffer),
                      static_cast<unsigned>(reinterpret_cast<uintptr_t>(gdx_rom_buffer) & 0xFFFFFFFFu),
                      gdx_rom_size);
    }
    gdx_register_main_module_range();

    // Audio delivery (C-R2.2): preload the three audio blob families once, here, in the same
    // boot window gdx_rom_buffer is registered (AFTER gdx_rdram_init, BEFORE bootproc), so the
    // payloads are resident before the first audio DMA rather than loading lazily on an audio
    // tick. Each resident payload is then registered as a host range so truncated-low32 tokens
    // of blob-served audio buffers resolve through the marshaller EXACTLY like gAudioHeap /
    // gdx_rom_buffer do (shared LLE/HLE two-tier resolver). Absence degrades silently: if the
    // live archive lacks these entries (owner's fzerox.o2r predates them), preload returns 0 and
    // the DMA sink falls back to raw ROM -- zero behavior change from today.
    //
    // Bases are the PORT_audio_{bank,seq,table}_ROM_START constants from
    // decomp/include/port_segment_addrs.h, declared here by known value (this file deliberately
    // avoids the decomp include tree; the shared blob table is keyed on these exact bases).
    {
        static const uint32_t kAudioBlobBases[3] = {
            0x00524D60u, // PORT_audio_bank_ROM_START  (audio_blob/audio_bank)
            0x00527AF0u, // PORT_audio_seq_ROM_START   (audio_blob/audio_seq)
            0x00528730u, // PORT_audio_table_ROM_START (audio_blob/audio_table)
        };
        for (int i = 0; i < 3; ++i) {
            const uint32_t base = kAudioBlobBases[i];
            if (GdxSegmentSourcePreload(base)) {
                void* payload = nullptr;
                uint32_t payloadSize = 0;
                if (GdxSegmentSourcePayload(base, &payload, &payloadSize) && payload != nullptr) {
                    gdx_register_host_range(payload, payloadSize);
                    gdx_port_logf("[audio-blob] preloaded+registered base=%08X payload=%p low32=%08X size=0x%X\n",
                                  base, payload,
                                  static_cast<unsigned>(reinterpret_cast<uintptr_t>(payload) & 0xFFFFFFFFu),
                                  payloadSize);
                }
            } else {
                gdx_port_logf("[audio-blob] base=%08X not resident (archive lacks entry) — raw-ROM fallback\n",
                              base);
            }
        }
    }

    // PCM-parity capture (C-R2.3): arm before the audio thread starts producing ticks, so the
    // capture window and its deterministic-RNG gate are live on the first audio tick. No-op unless
    // GDX_PCM_CAPTURE is set — zero behavior change for normal play.
    gdx_pcm_capture_init();

    logStep("bootproc() — starting the decomp game threads");
    bootproc();
    logStep("bootproc() returned; game threads running");

    // Frame loop: pump SDL events + libultraship window each tick.
    // HandleEvents() MUST be called every frame to drain the SDL event queue;
    // without it, click/close events pile up and the window manager crashes.
    // R6-P2: give the gfx bridge a monotonic clock so its sub-frame loop can derive t. Registered
    // once, before the loop; inert unless FrameInterpolation is on.
    gdx_gfx_interp_set_now_fn(&gdx_host_now_seconds);

    logStep("entering frame loop");
    auto w = ctx->GetWindow();
    while (w != nullptr && w->IsRunning()) {
        gdx::PerfFrameBegin();
        gdx::PerfPhaseBegin(gdx::PerfEvents);
        w->HandleEvents();
        gdx::PerfPhaseEnd(gdx::PerfEvents);
        gdx::PerfPhaseBegin(gdx::PerfInput);
        gdx_controller_poll();
        // Publish the editor fixed-aspect state before this frame's gfx task runs (the gametick
        // below): Course Edit / Create Machine render through the stock 4:3 pillarbox path. No-op
        // CVar-wise except on game-mode transitions. See gdx_fixed_aspect_tick in input_bridge.c.
        gdx_fixed_aspect_tick();
        gdx::PerfPhaseEnd(gdx::PerfInput);

        // R6-P2: decide render/logic decoupling for THIS tick and configure the bridge's sub-frame
        // schedule BEFORE gdx_vi_tick — the game's gfx-task submission (gdx_gfx_run) can execute
        // inside gdx_vi_tick's synchronous fiber dispatch (see the comment just below), so the
        // schedule must be live before any gfx work runs. Inert unless FrameInterpolation / GDX_INTERP_P2.
        // tickStart anchors the wall-clock accumulator (plan Step 2a); tickDuration is the fixed 60 Hz
        // logic budget — the SIM cadence never changes, only the number of presented sub-frames does.
        // P3 (plan edge #3): gate interpolation OFF while an EK editor (Course Edit / Create
        // Machine) forces the stock 4:3 pillarbox path. gdx_fixed_aspect_tick() above just
        // published that state; gdx_get_force_fixed_aspect() reads the same interpreter global the
        // pillarbox path uses. Editors are low-motion and their fresh mRendersToFb/aspect recompute
        // must not run per sub-frame, so we drop to the single-present default path (M=1). The
        // decomp_port.c mode-load cut shim snaps the first tick after leaving the editor.
        const bool interpEditorActive = (gdx_get_force_fixed_aspect() != 0);
        const bool interpOn = (gdx_gfx_interp_host_active() != 0) && !interpEditorActive;
        const double interpTickStart = gdx_host_now_seconds();
        // SoH-style Frame Interpolation FPS control (gdx_menu.cpp UI, ~:1290-1380): derive this
        // tick's M (max sub-frames) from the two live CVars instead of the old fixed constant.
        //   gEnhancements.Graphics.InterpTargetMode = 0 -> Match Refresh Rate: target = the
        //                                             display's current refresh rate, queried via
        //                                             Ship::Window::GetCurrentRefreshRate() (`w`
        //                                             below is already ctx->GetWindow() from above).
        //                                             1 -> Capped: target = InterpTargetFps.
        // M = clamp(ceil(target/60), 1, kGdxInterpMaxSubframes) -- one wall-clock present per full
        // 60 Hz slice of the target, e.g. 60Hz->1 (present-per-tick, still correct), 144Hz->3,
        // 480fps target->8 (the hard ceiling). Only computed while interpOn; the default path
        // ignores maxSubframes entirely, so this is inert with interpolation off. Cheap: a couple of
        // live CVar reads plus (Match Refresh only) one window query, same "read live every tick"
        // idiom FrameInterpolation's own master toggle already uses above.
        // R6-P2 FIELD-DEFECT FIX (2026-07-23): derive this tick's sub-frame COUNT via a deterministic
        // rational remainder accumulator (SoH interpolate_frame), not the old ceil(target/60). The
        // target-frames that fall inside one 60 Hz logic tick is target * tickSeconds — a FRACTION
        // (e.g. 143 Hz -> 143 * 1.001/60 = 2.386). ceil() rounded that up to a fixed 3 every tick,
        // which on a VSync-on 143 Hz panel cannot fit in the tick budget (3 blocking presents ~= 21 ms
        // > 16.68 ms) and made the loop overrun/oscillate — the owner's "unstable framerate". The
        // accumulator instead carries the fractional remainder across ticks so the per-tick count
        // alternates (2,2,3,...) and averages exactly target/60, keeping the long-run present rate at
        // the target while logic stays 60 Hz. No clock reads: purely a running remainder. The M cap
        // (kGdxInterpMaxSubframes) is preserved; the tick_config API shape is unchanged (main derives
        // the count from the target and hands it over as maxSubframes; the bridge presents that many
        // evenly-spaced sub-frames).
        int interpMaxSubframes = kGdxInterpMaxSubframes;
        static double sInterpFrameAccum = 0.0;
        // Cache for the OS panel-Hz resolution (MonitorFromRect + GetMonitorInfoW + a
        // QueryDisplayConfig device-matching loop, see gdx_os_display_refresh_hz above) -- that
        // path is comparatively expensive to run every logic tick while Match Refresh is enabled.
        // Re-resolved at most once per >=1000ms (using this tick's own wall-clock time source,
        // interpTickStart) or immediately whenever Match Refresh (re-)becomes active, so a monitor
        // change/hotplug is still picked up quickly without paying the resolution cost every tick.
        static double sInterpOsHzLastResolve = -1000.0;
        static int sInterpOsHzCached = 0;
        static const char* sInterpOsHzPathCached = "unresolved";
        static bool sInterpMatchRefreshActive = false;
        if (interpOn) {
            const bool interpMatchRefresh = CVarGetInteger("gEnhancements.Graphics.InterpTargetMode", 0) == 0;
            int interpEffectiveTarget;
            if (interpMatchRefresh) {
                const uint32_t refreshRate = w->GetCurrentRefreshRate();
                int detected = (refreshRate > 0) ? static_cast<int>(refreshRate) : 0;
                // Cross-check the OS panel rate for the monitor the window is actually ON (see
                // gdx_os_display_refresh_hz): libultraship may under-report on some high-refresh
                // panels. Use the higher of the two so a mis-detected 60 Hz on a real 143 Hz panel
                // still drives interpolation, while a window sitting on a genuinely lower-Hz
                // monitor doesn't get pulled up to a higher-Hz secondary display's rate.
                if (!sInterpMatchRefreshActive || (interpTickStart - sInterpOsHzLastResolve) >= 1.0) {
                    sInterpOsHzCached = gdx_os_display_refresh_hz(w->GetPosX(), w->GetPosY(), w->GetWidth(),
                                                                   w->GetHeight(), &sInterpOsHzPathCached);
                    sInterpOsHzLastResolve = interpTickStart;
                }
                sInterpMatchRefreshActive = true;
                const int osHz = sInterpOsHzCached;
                const char* osHzPath = sInterpOsHzPathCached;
                if (osHz > detected) {
                    detected = osHz;
                }
                // Fallback to 120 if neither source can report a refresh rate. Floor a genuine
                // sub-60 report (50 Hz PAL sets, VM/RDP virtual displays, eco panel modes) at 60:
                // the sim ticks at 60 Hz and always presents at least once per tick, so a sub-60
                // target cannot be honored anyway — flooring keeps framesPerTick >= 1 by
                // construction instead of leaning on the count<1 guard below.
                interpEffectiveTarget = (detected > 0) ? detected : 120;
                if (interpEffectiveTarget < 60) {
                    interpEffectiveTarget = 60;
                }
                static bool sLoggedRefreshDiag = false;
                if (!sLoggedRefreshDiag) {
                    sLoggedRefreshDiag = true;
                    gdx_port_logf(
                        "[interp-diag] MatchRefresh: lus_refresh=%u os_refresh=%d (path=%s) -> target=%d\n",
                        static_cast<unsigned>(refreshRate), osHz, osHzPath, interpEffectiveTarget);
                }
            } else {
                sInterpMatchRefreshActive = false; // re-resolve immediately if Match Refresh re-enables later
                // Root-fix (judgment-day round 2): clamp the CVar read itself to the UI's enforced
                // range [60, 480] (gdx_menu.cpp Target FPS slider, ~:1437-1442). A manually edited
                // config value below 60 used to reach the `count < 1` debit guard below, whose
                // debit-then-floor arithmetic cannot actually reduce presentation below one present
                // per tick for a value that low (truncation toward zero on the negative
                // accumulator). Clamping at the source removes that broken-in-practice path.
                interpEffectiveTarget = CVarGetInteger("gEnhancements.Graphics.InterpTargetFps", 120);
                if (interpEffectiveTarget < 60) {
                    interpEffectiveTarget = 60;
                }
                if (interpEffectiveTarget > 480) {
                    interpEffectiveTarget = 480;
                }
            }
            // target-frames per logic tick (fractional); accumulate the remainder deterministically.
            const double framesPerTick = static_cast<double>(interpEffectiveTarget) * kGdxInterpTickSeconds;
            sInterpFrameAccum += framesPerTick;
            int count = static_cast<int>(sInterpFrameAccum); // floor
            sInterpFrameAccum -= static_cast<double>(count);  // carry the fractional remainder
            if (count < 1) {
                count = 1; // always present at least once (also covers sub-60 targets)
                // This forced present wasn't "earned" by the accumulator (remainder was already <1
                // before forcing), so debit it the same way an earned present would have been
                // subtracted, to avoid a sub-60 target's long-run present rate creeping above
                // target/60. Floored at -1.0 (at most one tick's worth of debt) so the debt can't
                // grow unbounded if this guard is ever hit on consecutive ticks. Both
                // interpEffectiveTarget sources are now floor-bounded at >=60 (Capped mode's CVar
                // read is clamped to the UI's [60, 480] range above; Match Refresh floors its
                // detected rate at 60 and falls back to 120 when nothing is detected), so
                // framesPerTick >= 1 and this branch should not fire; kept as defense-in-depth
                // for any future target source that skips those clamps.
                sInterpFrameAccum -= 1.0;
                if (sInterpFrameAccum < -1.0) {
                    sInterpFrameAccum = -1.0;
                }
            }
            if (count > kGdxInterpMaxSubframes) {
                count = kGdxInterpMaxSubframes; // hard M cap
                sInterpFrameAccum = 0.0;        // don't let a clamped burst bank unbounded remainder
            }
            interpMaxSubframes = count;

            // R6-P2 FIELD-DEFECT FIX (2026-07-23, THE throughput fix): raise the window's frame-limiter
            // target to the interpolation target while interp is on. The DXGI/SDL backend software
            // limiter (gfx_dxgi.cpp / gfx_sdl2.cpp, mTargetFps, default 60) throttles EVERY present —
            // including each decoupled sub-frame present — to 60 fps. Measured: with the sub-frame loop
            // presenting 2-3 frames/tick the limiter pinned total presents at 60/s and dragged the SIM
            // to ~25 Hz during races (60 / avg_M), so interpolation delivered ZERO extra presented
            // frames AND ran the game in slow-motion. Setting the limiter target to interpEffectiveTarget
            // lets it pace sub-frames at the target rate instead (VSync-on: the panel refresh binds;
            // VSync-off: the limiter paces to target and the logic-deadline wait below still holds the
            // SIM at 60 Hz). Applied only on a CHANGE (SetTargetFps recomputes the limiter phase),
            // using the LIVE backend value as the source of truth so on/off and target-change
            // transitions all settle correctly. This is exactly how SoH drives its interpolation.
            Fast::Fast3dWindow* interpWin = static_cast<Fast::Fast3dWindow*>(w.get());
            if (interpWin->GetTargetFps() != interpEffectiveTarget) {
                interpWin->SetTargetFps(interpEffectiveTarget);
            }
        } else {
            sInterpFrameAccum = 0.0; // reset so a later re-enable starts from a clean remainder
            sInterpMatchRefreshActive = false; // force an immediate OS-Hz re-resolve on next enable
            // Restore the stock 60 fps limiter target when interpolation is off (mirror of the raise
            // above); live-value guarded so we only touch the backend on a real change.
            Fast::Fast3dWindow* interpWin = static_cast<Fast::Fast3dWindow*>(w.get());
            if (interpWin->GetTargetFps() != 60) {
                interpWin->SetTargetFps(60);
            }
        }
        gdx_gfx_interp_tick_config(interpOn ? 1 : 0, interpTickStart, kGdxInterpTickSeconds,
                                   interpMaxSubframes);

        // "gametick" is where the game frame ACTUALLY runs: gdx_vi_tick posts the VI retrace
        // message, which wakes the Main scheduler thread and the cooperative scheduler dispatches
        // the game fiber right here (osSendMesg -> osStartThread -> __osDispatchThread, because
        // __osRunningThread is NULL in host context). The whole game frame -- game logic AND the
        // synchronous gfx-task submission (gdx_gfx_run: DL translation, interpreter Run, frame
        // mirror) -- executes inside this call, not inside gdx_dispatch() below (which then finds
        // the run queue empty). Sub-phase timers in gdx_gfx_run attribute the breakdown; see
        // gdx_perf.h. This is why the old telemetry booked all game work under "input"/dispatch=0.
        gdx::PerfPhaseBegin(gdx::PerfGameTick);
        gdx_vi_tick();   // advance VI framebuffer + post retrace -> runs the Main game fiber here
        gdx::PerfPhaseEnd(gdx::PerfGameTick);
        gdx::PerfPhaseBegin(gdx::PerfInput);
        // Phase 3: wake the dedicated audio thread once per rendered frame (it also self-pumps
        // every 5ms independently, so a lost/late notify here is not a correctness issue --
        // see gdx_audio_thread.cpp). No-op when the kill switch reverts to the fiber path.
        gdx_audio_thread_notify_frame();
        w->GetMouseStateManager()->StartFrame();
        // Feed ImGui menu navigation from the SDL controller BEFORE StartDraw() runs ImGui's
        // NewFrame, so a raw DualSense (which ImGui's Win32/XInput backend cannot see) can open and
        // drive the menu on every platform. No-op unless gControlNav is enabled. See gdx_imgui_nav.
        gdx_imgui_nav_tick();
        gdx::PerfPhaseEnd(gdx::PerfInput);
        if (!interpOn) {
            // ===== DEFAULT PATH — one tick, one Run, one present. Byte-identical to pre-P2. =====
            // When FrameInterpolation is OFF, gdx_gfx_interp_host_active() returned 0, the bridge's
            // adapter leaves mInterpEnabled false (no scratch reroute), gdx_gfx_run takes its single
            // interp->Run path, and this exact statement sequence presents once and paces via the
            // frame pacer. Nothing on this branch touches the interpolation machinery.
            gdx::PerfPhaseBegin(gdx::PerfGuiStart);
            w->GetGui()->StartDraw();
            w->StartFrame(); // must precede gdx_dispatch: Run() needs an initialized frame
            // Cross-thread message wakes recorded by the dedicated audio thread (sendmesg.c PORT
            // path) become runnable here, on the host thread, right before the threads dispatch.
            // See the guard block in n64_sched.c.
            gdx_sched_drain_deferred_wakes();
            gdx::PerfPhaseEnd(gdx::PerfGuiStart);
            gdx::PerfPhaseBegin(gdx::PerfDispatch);
            gdx_dispatch();  // run the decomp's game threads cooperatively until they block again
            gdx::PerfPhaseEnd(gdx::PerfDispatch);
            gdx::PerfPhaseBegin(gdx::PerfTicks);
            // Durable 64DD disk-save flush. A game disk write (Course Edit save, MFS
            // format) marked the sidecar dirty via gdx_disk_save_mark_dirty; this
            // debounced tick persists it atomically once the write burst has drained.
            // No-op when nothing is pending. See port/disk_savefile.{h,cpp}.
            gdx_disk_save_tick();
            gdx::PerfPhaseEnd(gdx::PerfTicks);
            gdx::PerfPhaseBegin(gdx::PerfPresent);
            // VI-scanout fallback: if no GFX task rendered this frame (boot logo phase
            // or any CPU-drawn screen), present the current VI framebuffer's pixels.
            // Cheap no-op when a real frame was produced.
            gdx_vi_present_fallback();
            w->GetGui()->EndDraw();
            w->EndFrame();
            gdx::PerfPhaseEnd(gdx::PerfPresent);
            // Port-level wall-clock pacer, gated on gEnhancements.Graphics.FramePacing. Default is
            // platform-specific: OFF on Windows (the DXGI backend already limits EndFrame() to ~60fps)
            // and ON on Linux (the SDL2 limiter is signal-fragile and lets the loop free-run at the
            // panel refresh -- see gdx_frame_pacer.{h,c}). When on, holds the loop to the N64 NTSC
            // field rate (~59.94Hz).
            gdx::PerfPhaseBegin(gdx::PerfPacer);
            gdx_frame_pacer_tick();
            gdx::PerfPhaseEnd(gdx::PerfPacer);
        } else {
            // ===== FRAME-INTERPOLATION PATH (R6-P2, default-OFF) =====
            // The bridge owns the present this tick: gdx_gfx_run replays + presents M wall-clock
            // sub-frames of the retained gfx task via fw->DrawAndRunGraphicsCommands (each a full
            // StartDraw..EndFrame bracket). We must NOT open our own ImGui StartDraw here — it would
            // nest the per-sub-frame ImGui frames. So this branch runs the scheduler + dispatch, then
            // only presents itself as a FALLBACK for a taskless tick (edge #6, e.g. boot logo).
            gdx::PerfPhaseBegin(gdx::PerfGuiStart);
            gdx_sched_drain_deferred_wakes();
            gdx::PerfPhaseEnd(gdx::PerfGuiStart);
            gdx::PerfPhaseBegin(gdx::PerfDispatch);
            gdx_dispatch();  // game runs once; gdx_gfx_run presents the interpolated sub-frames
            gdx::PerfPhaseEnd(gdx::PerfDispatch);
            gdx::PerfPhaseBegin(gdx::PerfTicks);
            gdx_disk_save_tick();
            gdx::PerfPhaseEnd(gdx::PerfTicks);
            gdx::PerfPhaseBegin(gdx::PerfPresent);
            if (gdx_gfx_interp_presented_last_tick() == 0) {
                // No gfx task this tick -> interpolation no-ops cleanly (edge #6). Present once via
                // the normal single-frame bracket (the CPU-drawn VI framebuffer / hold pixels).
                w->GetGui()->StartDraw();
                w->StartFrame();
                gdx_vi_present_fallback();
                w->GetGui()->EndDraw();
                w->EndFrame();
            }
            gdx::PerfPhaseEnd(gdx::PerfPresent);
            // Pacer mutual exclusion: the frame pacer is NOT called here (it also no-ops itself when
            // FrameInterpolation is on — see gdx_frame_pacer.c). Presents ran VSync-paced inside the
            // sub-frame loop; this holds the host to the 60 Hz LOGIC deadline so the SIM cadence stays
            // locked at 60 Hz even when presents finished early (VSync-off case). Interpolation and
            // FramePacing are mutually-exclusive pacing owners (plan §4).
            gdx::PerfPhaseBegin(gdx::PerfPacer);
            // R6-P2 FIELD-DEFECT FIX (2026-07-23): pace the SIM against a RUNNING absolute schedule
            // (advance one tick each iteration), not a fresh interpTickStart+tick each tick. With the
            // rational accumulator, some ticks present 2 sub-frames (~14 ms VSync) and some 3 (~21 ms);
            // a per-tick deadline re-anchored to "now" would pad the short ticks with idle time,
            // injecting a stutter every 2-3 ticks and dragging the average below 60 Hz. A running
            // schedule lets a short tick recover the time a long tick overran (and vice-versa), so
            // under VSync-on the presents self-pace and this wait is a near-no-op; re-anchor on a big
            // stall (menu/alt-tab/breakpoint) so we never replay a burst of missed ticks.
            static double sNextLogicDeadline = 0.0;
            if (sNextLogicDeadline <= 0.0 ||
                interpTickStart > sNextLogicDeadline + 4.0 * kGdxInterpTickSeconds) {
                sNextLogicDeadline = interpTickStart + kGdxInterpTickSeconds;
            } else {
                sNextLogicDeadline += kGdxInterpTickSeconds;
            }
            gdx_host_pace_logic_until(sNextLogicDeadline);
            gdx::PerfPhaseEnd(gdx::PerfPacer);

            // Telemetry (spec item 6): rate-limited [interp-p2] line + a one-time activation line.
            // Cadence mirrors the bridge's diagnostics: first 8 ticks then every 120th (~1/2 s).
            {
                static bool sInterpP2Announced = false;
                if (!sInterpP2Announced) {
                    sInterpP2Announced = true;
                    // interpMaxSubframes is THIS tick's target-fps-derived M (see the derivation
                    // above tick_config); kGdxInterpMaxSubframes is the hard ceiling it was clamped
                    // against, independent of the live InterpTargetMode/InterpTargetFps CVars.
                    gdx_port_logf("[interp-p2] decoupled loop ACTIVE: sim locked at 60 Hz, presenting "
                                  "%d evenly-spaced sub-frames this tick (rational accumulator averages "
                                  "target/60; hard cap %d). Window fps-limiter raised to target; frame "
                                  "pacer mutually excluded.\n",
                                  interpMaxSubframes, kGdxInterpMaxSubframes);
                }
                static size_t sInterpP2Tick = 0;
                static size_t sInterpP2SubAccum = 0;
                const size_t tick = sInterpP2Tick++;
                const int sub = gdx_gfx_interp_last_subframes();
                sInterpP2SubAccum += (sub > 0) ? static_cast<size_t>(sub) : 0;
                if (tick < 8 || (tick % 120) == 0) {
                    const double avg = (tick + 1 > 0)
                                           ? static_cast<double>(sInterpP2SubAccum) / static_cast<double>(tick + 1)
                                           : 0.0;
                    // lerped/snapped: the per-slot tween evidence the P2 path previously never logged
                    // (it only lived on the env-P1 single-present branch) — surfaced here so the
                    // owner's actual FrameInterpolation path is diagnosable from the log. presents/s
                    // is the rolling real-FPS meter. In steady state expect lerped >> snapped.
                    gdx_port_logf("[interp-p2] ticks=%zu subframes=%d avg_m=%.2f t_last=%.3f "
                                  "lerped=%d snapped=%d presents/s=%.1f\n",
                                  tick + 1, sub, avg, gdx_gfx_interp_last_t(),
                                  gdx_gfx_interp_last_lerped(), gdx_gfx_interp_last_snapped(),
                                  gdx_gfx_interp_presents_per_sec());
                }
            }
        }
        gdx::PerfFrameEnd();

        // Auto-exit after a bounded PCM capture (C-R2.3): once the capture window has finalized
        // (GDX_PCM_CAPTURE_FRAMES reached), request a clean shutdown through the SAME path the
        // window-close event uses (Window::Close() -> IsRunning() goes false), so the loop drains
        // and the normal teardown below runs. Gated on GDX_PCM_CAPTURE so there is zero behavior
        // change when capture is not configured. Checked once via a static.
        {
            static const bool sCaptureMode = (std::getenv("GDX_PCM_CAPTURE") != nullptr);
            if (sCaptureMode && gdx_pcm_capture_finished()) {
                logStep("PCM capture finalized; requesting window close (auto-exit)");
                w->Close();
            }
        }
    }
    logStep("window closed; exiting");
    // Force-persist any pending 64DD save journal before exit. The per-frame
    // gdx_disk_save_tick() only flushes after the write burst has been idle for
    // kDebounceFrames (~0.5s); a save landing inside that final window when the
    // window closes would otherwise be dropped. gdx_disk_save_flush() writes the
    // current journal unconditionally (no-op when nothing is active/pending).
    gdx_disk_save_flush();
    // Stop/join the audio thread BEFORE finalizing PCM capture: gdx_pcm_capture_shutdown() closes
    // the file and folds the SHA, so it must not race a still-running audio thread calling feed().
    gdx_audio_thread_stop();
    // Finalize any still-open PCM capture (unbounded run, or window closed mid-window) so its
    // <prefix>.pcm.sha256 is always emitted. No-op unless a capture was armed and not yet done.
    gdx_pcm_capture_shutdown();
    return 0;
}
