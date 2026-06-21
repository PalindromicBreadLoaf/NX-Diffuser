// G-Diffuser — port entry point.
// Slice 4c: granular libultraship init. The ControlDeck must be constructed AFTER the Context +
// ConsoleVariables exist (its GlobalSDLDeviceSettings reads CVars via Context::GetInstance()),
// so we use CreateUninitializedInstance + step-by-step Init rather than the one-shot CreateInstance.
// After init: register factories, bind assets, then hand off to the decomp boot (bootproc).

#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"
#include "resource/ResourceFactories.h"
#include "GDiffuserControlDeck.h"

#include <chrono>
#include <cstdio>
#include <memory>
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
    logStep("CreateUninitializedInstance");
    auto ctx = Ship::Context::CreateUninitializedInstance("G-Diffuser", "gdiffuser",
                                                          "gdiffuser.cfg.json");
    if (ctx == nullptr) { logStep("FATAL: CreateUninitializedInstance"); return 1; }

    logStep("InitLogging");          ctx->InitLogging();
    logStep("InitConfiguration");    ctx->InitConfiguration();
    logStep("InitConsoleVariables"); ctx->InitConsoleVariables();

    // Context + CVars exist now — safe to build the ControlDeck.
    logStep("construct ControlDeck"); auto controlDeck = std::make_shared<GDiffuser::ControlDeck>();

    logStep("InitResourceManager");  ctx->InitResourceManager(std::vector<std::string>{ "generic.o2r" }, {}, 1);
    logStep("InitControlDeck");      ctx->InitControlDeck(controlDeck);
    logStep("InitCrashHandler");     ctx->InitCrashHandler();
    logStep("InitConsole");          ctx->InitConsole();
    logStep("InitWindow");           ctx->InitWindow();
    logStep("InitAudio");            ctx->InitAudio({});
    logStep("InitEventSystem");      ctx->InitEventSystem();
    logStep("InitFileDropMgr");      ctx->InitFileDropMgr();

    logStep("RegisterResourceFactories");
    GDiffuser::RegisterResourceFactories(ctx->GetResourceManager()->GetResourceLoader());

    logStep("GDiffuser_LoadAllAssets");
    GDiffuser_LoadAllAssets();

    logStep("bootproc() — starting the decomp game threads");
    bootproc();
    logStep("bootproc() returned; game threads running");

    logStep("entering keep-alive (frame-loop integration pending)");
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return 0;
}
