// G-Diffuser — port entry point.
// Slice 4c: granular libultraship init. The ControlDeck must be constructed AFTER the Context +
// ConsoleVariables exist (its GlobalSDLDeviceSettings reads CVars via Context::GetInstance()),
// so we use CreateUninitializedInstance + step-by-step Init rather than the one-shot CreateInstance.
// After init: register factories, bind assets, then hand off to the decomp boot (bootproc).

#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"
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
#include "gdx_audio_thread.h"
#include "gdx_frame_pacer.h"  // optional wall-clock 60Hz pacer (gEnhancements.Graphics.FramePacing)
#include "gdx_savestate.h"    // in-session RAM quick-save/load infrastructure (default OFF; see file)
#include <SDL2/SDL.h>  // SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER): enable gamepad auto-detection

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
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
extern "C" void gdx_init_rom(int argc, char** argv); // S5: load ROM into host buffer
extern "C" void gdx_vi_tick(void);             // R6: advance VI + post retrace (wakes Main thread)
extern "C" void gdx_dispatch(void);            // R6: run game threads until quiescent
extern "C" void gdx_vi_present_fallback(void); // VI-scanout fallback: present CPU-drawn framebuffers
extern "C" void gdx_controller_poll(void);     // PORT: host keyboard -> decomp controller globals
extern "C" void gdx_fixed_aspect_tick(void);   // input_bridge: force 4:3 rendering for the EK editors
extern "C" void gdx_rdram_init(void);          // n64-rdram-buffer: allocate 8MB RDRAM before bootproc
extern "C" void gdx_register_host_range(void* ptr, size_t size); // n64_gfx_bridge: expose range for TryResolveAddress
extern "C" void gdx_register_main_module_range(void); // n64_gfx_bridge: resolve low32 EXE/BSS segment tokens
extern "C" void gdx_game_request_reset(void); // game.c: schedule a title reset at the game-flow boundary
extern "C" void gdx_disk_save_tick(void);      // disk_savefile: debounced flush of the 64DD save sidecar

static void logStep(const char* s) {
    gdx_port_logf("[G-Diffuser] %s\n", s);
}

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
    for (const char* name : { "f3d.o2r", "generic.o2r" }) {
        for (const auto& root : roots) {
            const auto candidate = root / name;
            if (std::filesystem::exists(candidate, ec) && std::filesystem::is_regular_file(candidate, ec)) {
                archives.push_back(std::filesystem::absolute(candidate, ec).string());
                break;
            }
            ec.clear();
        }
    }

    if (archives.empty()) {
        archives.push_back("f3d.o2r");
        archives.push_back("generic.o2r");
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

    logStep("CreateUninitializedInstance");
    auto ctx = Ship::Context::CreateUninitializedInstance("G-Diffuser", "gdiffuser",
                                                          "gdiffuser.cfg.json");
    if (ctx == nullptr) { logStep("FATAL: CreateUninitializedInstance"); return 1; }

    logStep("InitLogging");          ctx->InitLogging();
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
    logStep("InitResourceManager");   ctx->InitResourceManager(archivePaths, {}, 1);

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

#ifndef _WIN32
        // Frame-pacing default is platform-specific. On Windows the DXGI backend's own sleep+spin
        // limiter reliably holds EndFrame() to 60fps, so the port pacer stays OFF (stock behavior).
        // On Linux the Fast3D SDL2 limiter (SyncFramerateWithTime) sleeps with a *relative*
        // nanosleep() and no EINTR retry, so a signal can cut the sleep short every frame and the
        // loop free-runs at the panel refresh (e.g. 144Hz on the ROG Ally -> ~2.4x game speed).
        // Default the robust wall-clock pacer (clock_nanosleep TIMER_ABSTIME, EINTR-safe) ON here.
        //
        // This runs BEFORE the GdxMenu ctor registers the CVar: RegisterInteger only sets a value
        // when the CVar does not yet exist, so a value already loaded from the user's config (their
        // explicit toggle) still wins, while a fresh config gets the pacer enabled by default.
        CVarRegisterInteger("gEnhancements.Graphics.FramePacing", 1);
        // One-time migration: configs written before this default flipped have FramePacing:0
        // persisted (the old cross-platform default), which the register above cannot override —
        // so those Linux users would still free-run at the panel refresh (144Hz). Flip it ON once;
        // the marker preserves any later deliberate OFF. Same pattern as gControlNav.
        if (CVarGetInteger("gdx.Migrations.LinuxFramePacingOn", 0) == 0) {
            CVarSetInteger("gdx.Migrations.LinuxFramePacingOn", 1);
            CVarSetInteger("gEnhancements.Graphics.FramePacing", 1);
            CVarSave();
        }
#endif

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
        gdx_init_rom(static_cast<int>(romArgv.size()), romArgv.data());
    }

    // Block the game from starting without a ROM — a null buffer causes silent DMA
    // zero-fills that corrupt game state and produce non-obvious crashes downstream.
    {
        if (gdx_rom_buffer == nullptr) {
            gdx_port_logf("[G-Diffuser] FATAL: no ROM loaded — cannot start game.\n");
            gdx_port_logf("[G-Diffuser] Place baserom.us.rev0.z64 next to the exe, or pass it as an argument.\n");
#ifdef _WIN32
            MessageBoxA(nullptr,
                "No F-Zero X ROM was loaded.\n\n"
                "Place 'baserom.us.rev0.z64' next to G-Diffuser.exe,\n"
                "or pass the ROM path as a command-line argument.",
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

    logStep("bootproc() — starting the decomp game threads");
    bootproc();
    logStep("bootproc() returned; game threads running");

    // Frame loop: pump SDL events + libultraship window each tick.
    // HandleEvents() MUST be called every frame to drain the SDL event queue;
    // without it, click/close events pile up and the window manager crashes.
    logStep("entering frame loop");
    auto w = ctx->GetWindow();
    while (w != nullptr && w->IsRunning()) {
        w->HandleEvents();
        gdx_controller_poll();
        // Publish the editor fixed-aspect state before this frame's gfx task runs (gdx_dispatch
        // below): Course Edit / Create Machine render through the stock 4:3 pillarbox path. No-op
        // CVar-wise except on game-mode transitions. See gdx_fixed_aspect_tick in input_bridge.c.
        gdx_fixed_aspect_tick();
        gdx_vi_tick();   // advance VI framebuffer + post retrace -> wakes the Main scheduler thread
        // Phase 3: wake the dedicated audio thread once per rendered frame (it also self-pumps
        // every 5ms independently, so a lost/late notify here is not a correctness issue --
        // see gdx_audio_thread.cpp). No-op when the kill switch reverts to the fiber path.
        gdx_audio_thread_notify_frame();
        w->GetMouseStateManager()->StartFrame();
        // Feed ImGui menu navigation from the SDL controller BEFORE StartDraw() runs ImGui's
        // NewFrame, so a raw DualSense (which ImGui's Win32/XInput backend cannot see) can open and
        // drive the menu on every platform. No-op unless gControlNav is enabled. See gdx_imgui_nav.
        gdx_imgui_nav_tick();
        w->GetGui()->StartDraw();
        w->StartFrame(); // must precede gdx_dispatch: Run() needs an initialized frame
        // Cross-thread message wakes recorded by the dedicated audio thread (sendmesg.c PORT
        // path) become runnable here, on the host thread, right before the threads dispatch.
        // See the guard block in n64_sched.c.
        gdx_sched_drain_deferred_wakes();
        gdx_dispatch();  // run the decomp's game threads cooperatively until they block again
        // In-session save-state boundary hook. gdx_dispatch() has just drained the run queue,
        // so every decomp game fiber is parked at its retrace/message wait -- the one point where
        // an RDRAM snapshot/restore is atomic w.r.t. the game threads (the audio thread is
        // serialized inside via gdx_audio_ctx_lock). Strict no-op unless
        // gEnhancements.Gameplay.SaveStates is enabled. See port/gdx_savestate.{h,c}.
        gdx_savestate_tick();
        // Durable 64DD disk-save flush. A game disk write (Course Edit save, MFS
        // format) marked the sidecar dirty via gdx_disk_save_mark_dirty; this
        // debounced tick persists it atomically once the write burst has drained.
        // No-op when nothing is pending. See port/disk_savefile.{h,cpp}.
        gdx_disk_save_tick();
        // VI-scanout fallback: if no GFX task rendered this frame (boot logo phase
        // or any CPU-drawn screen), present the current VI framebuffer's pixels.
        // Cheap no-op when a real frame was produced.
        gdx_vi_present_fallback();
        w->GetGui()->EndDraw();
        w->EndFrame();
        // Port-level wall-clock pacer, gated on gEnhancements.Graphics.FramePacing. Default is
        // platform-specific: OFF on Windows (the DXGI backend already limits EndFrame() to ~60fps)
        // and ON on Linux (the SDL2 limiter is signal-fragile and lets the loop free-run at the
        // panel refresh -- see gdx_frame_pacer.{h,c}). When on, holds the loop to the N64 NTSC
        // field rate (~59.94Hz).
        gdx_frame_pacer_tick();
    }
    logStep("window closed; exiting");
    gdx_audio_thread_stop();
    gdx_savestate_shutdown(); // free the in-RAM slot if one was ever allocated (no-op otherwise)
    return 0;
}
