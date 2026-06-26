// G-Diffuser — port entry point.
// Slice 4c: granular libultraship init. The ControlDeck must be constructed AFTER the Context +
// ConsoleVariables exist (its GlobalSDLDeviceSettings reads CVars via Context::GetInstance()),
// so we use CreateUninitializedInstance + step-by-step Init rather than the one-shot CreateInstance.
// After init: register factories, bind assets, then hand off to the decomp boot (bootproc).

#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"
#include "resource/ResourceFactories.h"
#include "GDiffuserControlDeck.h"
#include "fast/Fast3dWindow.h"
#include "fast/Fast3dGui.h"
#include "ship/window/gui/GuiWindow.h"
#include "port_log.h"
#include "rom_buffer.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern "C" void GDiffuser_LoadAllAssets(void); // generated asset binding loader (R2)
extern "C" void bootproc(void);                // decomp boot entry (src/sys/sys_main.c)
extern "C" void gdx_sched_init(void);          // R6: init cooperative fiber scheduler (host fiber)
extern "C" void gdx_init_rom(int argc, char** argv); // S5: load ROM into host buffer
extern "C" void gdx_vi_tick(void);             // R6: advance VI + post retrace (wakes Main thread)
extern "C" void gdx_dispatch(void);            // R6: run game threads until quiescent
extern "C" void gdx_controller_poll(void);     // PORT: host keyboard -> decomp controller globals
extern "C" void gdx_rdram_init(void);          // n64-rdram-buffer: allocate 8MB RDRAM before bootproc
extern "C" void gdx_register_host_range(void* ptr, size_t size); // n64_gfx_bridge: expose range for TryResolveAddress
extern "C" void gdx_register_main_module_range(void); // n64_gfx_bridge: resolve low32 EXE/BSS segment tokens

static void logStep(const char* s) {
    gdx_port_logf("[G-Diffuser] %s\n", s);
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

    return archives;
}

int main(int argc, char** argv) {
    logStep("CreateUninitializedInstance");
    auto ctx = Ship::Context::CreateUninitializedInstance("G-Diffuser", "gdiffuser",
                                                          "gdiffuser.cfg.json");
    if (ctx == nullptr) { logStep("FATAL: CreateUninitializedInstance"); return 1; }

    logStep("InitLogging");          ctx->InitLogging();
    logStep("InitConfiguration");    ctx->InitConfiguration();
    logStep("InitConsoleVariables"); ctx->InitConsoleVariables();

    // Order matches BattleShip's working sequence: ControlDeck -> ResourceManager -> Window.
    // Context + CVars exist now — safe to build the ControlDeck.
    logStep("construct ControlDeck"); auto controlDeck = std::make_shared<GDiffuser::ControlDeck>();
    logStep("InitControlDeck");       ctx->InitControlDeck(controlDeck);

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

    // Now resource manager + console exist — safe to build and init the window.
    logStep("construct Fast3dGui");
    auto gui = std::make_shared<Fast::Fast3dGui>(std::vector<std::shared_ptr<Ship::GuiWindow>>());
    logStep("construct Fast3dWindow(gui)");
    auto window = std::make_shared<Fast::Fast3dWindow>(gui);
    logStep("InitWindow");            ctx->InitWindow(window);
    logStep("InitAudio");             ctx->InitAudio({});
    logStep("InitEventSystem");       ctx->InitEventSystem();
    logStep("InitFileDropMgr");       ctx->InitFileDropMgr();

    logStep("RegisterResourceFactories");
    GDiffuser::RegisterResourceFactories(ctx->GetResourceManager()->GetResourceLoader());

    logStep("GDiffuser_LoadAllAssets");
    GDiffuser_LoadAllAssets();

    logStep("gdx_sched_init() — cooperative fiber scheduler");
    gdx_sched_init();

    logStep("gdx_init_rom() — load ROM asset buffer");
    gdx_init_rom(argc, argv);

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
        gdx_vi_tick();   // advance VI framebuffer + post retrace -> wakes the Main scheduler thread
        w->GetMouseStateManager()->StartFrame();
        w->GetGui()->StartDraw();
        w->StartFrame(); // must precede gdx_dispatch: Run() needs an initialized frame
        gdx_dispatch();  // run the decomp's game threads cooperatively until they block again
        w->GetGui()->EndDraw();
        w->EndFrame();
    }
    logStep("window closed; exiting");
    return 0;
}
