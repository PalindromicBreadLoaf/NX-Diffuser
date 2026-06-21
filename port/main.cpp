// G-Diffuser — port entry point.
// Slice 4c: real libultraship initialization. Creates the engine Context (window, audio,
// input, resource manager) and mounts the extracted asset archive. Resource-factory
// registration and the game-loop coroutine land next.

#include "ship/Context.h"

#include <memory>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    // The asset archive produced by `torch o2r` (placed next to the executable at runtime).
    const std::vector<std::string> archivePaths = { "generic.o2r" };

    auto ctx = Ship::Context::CreateInstance(
        /* name           */ "G-Diffuser",
        /* shortName       */ "gdiffuser",
        /* configFilePath  */ "gdiffuser.cfg.json",
        /* archivePaths    */ archivePaths);

    if (ctx == nullptr) {
        return 1;
    }

    // TODO (4c): register F-Zero X resource factories, then run the decomp game loop
    // wrapped in a coroutine driven by libultraship's frame loop.
    return 0;
}
