// G-Diffuser — asset loader (Slice 4c / R2 integration).
// Bridges the generated AssetBindings (C) to libultraship's ResourceManager: given an o2r
// resource key "category/symbol", load it and return the raw data pointer the game expects.
// C linkage so the generated C binding table can call it.

#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"

extern "C" void* GDiffuser_LoadAsset(const char* key) {
    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return nullptr;
    }
    auto rm = ctx->GetResourceManager();
    if (rm == nullptr) {
        return nullptr;
    }
    // Ensure the resource is parsed/cached, then hand back its raw data pointer.
    if (rm->LoadResource(key) == nullptr) {
        return nullptr; // not present or no factory yet (e.g. custom F-Zero-X types — R1b/R6)
    }
    return rm->GetResourceRawPointer(key);
}
