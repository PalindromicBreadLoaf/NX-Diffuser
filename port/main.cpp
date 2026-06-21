// G-Diffuser — port entry point.
// Slice 4c: init libultraship, mount assets, register factories, bind assets, then hand off to
// the decomp's own boot sequence (bootproc -> idle/main/game threads). libultraship provides the
// real libultra (threads, message queues, VI, controller) the game's threads run on.
//
// STATUS: bring-up. The game's frame-loop <-> libultraship window-loop integration is not final
// (see keep-alive below) — expect the boot to progress until it needs the frame pump, then stall.
// Use the log to see how far it gets. printf goes to stdout; libultraship also logs via spdlog.

#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"
#include "resource/ResourceFactories.h"
#include "GDiffuserControlDeck.h"

#include <memory>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

extern "C" void GDiffuser_LoadAllAssets(void); // generated asset binding loader (R2)
extern "C" void bootproc(void);                // decomp boot entry (src/sys/sys_main.c)

static void logStep(const char* s) {
    std::printf("[G-Diffuser] %s\n", s);
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    logStep("CreateInstance (libultraship: window/audio/input/resources)");
    auto controlDeck = std::make_shared<GDiffuser::ControlDeck>();
    auto ctx = Ship::Context::CreateInstance("G-Diffuser", "gdiffuser", "gdiffuser.cfg.json",
                                             std::vector<std::string>{ "generic.o2r" },
                                             /* validHashes */ {},
                                             /* reservedThreadCount */ 1,
                                             /* audioSettings */ {},
                                             /* window */ nullptr,
                                             controlDeck);
    if (ctx == nullptr) {
        logStep("FATAL: Context init failed");
        return 1;
    }

    logStep("RegisterResourceFactories");
    GDiffuser::RegisterResourceFactories(ctx->GetResourceManager()->GetResourceLoader());

    logStep("GDiffuser_LoadAllAssets");
    GDiffuser_LoadAllAssets();

    logStep("bootproc() — starting the decomp game threads");
    bootproc();
    logStep("bootproc() returned; game threads running");

    // Keep the process alive so the game's libultraship-backed threads run.
    // TODO (R6): replace with the real frame loop — drive libultraship's window/frame pump here
    // and bridge it to the game's VI retrace so the main/game threads advance per frame.
    logStep("entering keep-alive (frame-loop integration pending)");
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return 0;
}
