// G-Diffuser — asset loader (Slice 4c / R2 integration).
// Bridges the generated AssetBindings (C) to libultraship's ResourceManager: given an o2r
// resource key "category/symbol", load it and return the raw data pointer the game expects.
// C linkage so the generated C binding table can call it.

#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct LoadedAssetBuffer {
    const unsigned char* ptr = nullptr;
    size_t size = 0;
    std::string key;
};

std::vector<LoadedAssetBuffer> gLoadedAssetBuffers;

} // namespace

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

extern "C" int GDiffuser_LoadAssetBytes(const char* key, void* out, size_t outSize, size_t* copiedSize) {
    if ((key == nullptr) || (out == nullptr) || (outSize == 0)) {
        return 0;
    }

    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return 0;
    }

    auto rm = ctx->GetResourceManager();
    if (rm == nullptr) {
        return 0;
    }

    auto resource = rm->LoadResource(key);
    if (resource == nullptr) {
        return 0;
    }

    /* Contract note (2026-07-10 delivery audit): LUS resource factories
     * consume the 64-byte OTR header (ResourceLoader.cpp BufferOffset) and
     * the per-type sub-header BEFORE the resource is handed back --
     * GetRawPointer()/GetPointerSize() are already payload-only (e.g.
     * TextureFactory sets ImageData = buffer + 0x50). Never re-strip here:
     * an earlier "OTEX strip" at this spot operated on a disproven model and
     * was a latent truncation hazard for payloads with coincidental magic. */
    const unsigned char* raw = static_cast<const unsigned char*>(resource->GetRawPointer());
    const size_t rawSize = resource->GetPointerSize();
    if ((raw == nullptr) || (rawSize == 0) || (rawSize > outSize)) {
        return 0;
    }

    std::memcpy(out, raw, rawSize);
    if (rawSize < outSize) {
        std::memset(static_cast<unsigned char*>(out) + rawSize, 0, outSize - rawSize);
    }
    if (copiedSize != nullptr) {
        *copiedSize = rawSize;
    }
    return 1;
}

extern "C" void GDiffuser_RegisterLoadedAssetBuffer(const void* buffer, size_t size, const char* key) {
    if ((buffer == nullptr) || (size == 0) || (key == nullptr) || (key[0] == '\0')) {
        return;
    }

    const auto* ptr = static_cast<const unsigned char*>(buffer);
    for (LoadedAssetBuffer& entry : gLoadedAssetBuffers) {
        if (entry.ptr == ptr) {
            entry.size = size;
            entry.key = key;
            return;
        }
    }

    gLoadedAssetBuffers.push_back({ ptr, size, key });
}

extern "C" const char* GDiffuser_LookupLoadedAssetKey(const void* buffer, size_t minSize, int requireUnmodified) {
    if (buffer == nullptr) {
        return nullptr;
    }

    const auto* ptr = static_cast<const unsigned char*>(buffer);
    for (LoadedAssetBuffer& entry : gLoadedAssetBuffers) {
        if ((entry.ptr != ptr) || (entry.size < minSize)) {
            continue;
        }

        if (requireUnmodified) {
            auto ctx = Ship::Context::GetInstance();
            if (ctx == nullptr) {
                return nullptr;
            }
            auto rm = ctx->GetResourceManager();
            if (rm == nullptr) {
                return nullptr;
            }
            auto resource = rm->LoadResource(entry.key.c_str());
            if (resource == nullptr) {
                return nullptr;
            }
            void* raw = resource->GetRawPointer();
            const size_t rawSize = resource->GetPointerSize();
            if ((raw == nullptr) || (rawSize == 0) || (rawSize > entry.size)) {
                return nullptr;
            }
            if (std::memcmp(entry.ptr, raw, rawSize) != 0) {
                return nullptr;
            }
        }

        return entry.key.c_str();
    }

    return nullptr;
}
