// G-Diffuser — resource smoke test.
// Standalone exe (links libultraship ONLY, not the game objects). Verifies what's checkable
// headlessly: init the resource manager, mount the .o2r, and register the resource factories.
//
// NOTE: actually LOADING a resource needs the global Context (set by full CreateInstance) and,
// for textures, a GPU/renderer — i.e. a real display. That end-to-end load+render check happens
// with the full game on a real desktop, not in this headless test.

#include "resource/ResourceFactories.h"
#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const std::string archive = (argc > 1) ? argv[1] : "generic.o2r";
    auto step = [](const char* s) { printf("STEP: %s\n", s); fflush(stdout); };

    step("CreateUninitializedInstance");
    auto ctx = Ship::Context::CreateUninitializedInstance("G-Diffuser SmokeTest", "gdiffuser",
                                                          "gdiffuser.smoketest.cfg.json");
    step("InitLogging");          ctx->InitLogging();
    step("InitConfiguration");    ctx->InitConfiguration();
    step("InitConsoleVariables"); ctx->InitConsoleVariables();

    step("InitResourceManager (mounts the .o2r)");
    if (!ctx->InitResourceManager(std::vector<std::string>{ archive }, {}, 1)) {
        printf("SMOKETEST: FAIL — InitResourceManager (archive: %s)\n", archive.c_str());
        return 1;
    }

    auto rm = ctx->GetResourceManager();
    auto loader = rm ? rm->GetResourceLoader() : nullptr;
    if (loader == nullptr) {
        printf("SMOKETEST: FAIL — no ResourceLoader\n");
        return 1;
    }

    step("RegisterResourceFactories");
    GDiffuser::RegisterResourceFactories(loader);

    printf("SMOKETEST: OK — archive mounted and resource factories registered\n");
    return 0;
}
