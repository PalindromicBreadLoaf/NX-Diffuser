// G-Diffuser — asset loader.
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
        return nullptr; // not present, or no factory registered for this resource type
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

    /* LUS resource factories
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

extern "C" int GDiffuser_LoadArchiveFileBytes(const char* key, void* out, size_t outSize, size_t* copiedSize) {
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

    /* Raw archive-file bytes, bypassing resource-factory deserialization.
     * Staff-ghost records are stored in the o2r as Torch "GhostRecord" resources,
     * for which the port registers NO libultraship factory yet (see the TODO in
     * port/resource/ResourceFactories.cpp). GDiffuser_LoadAssetBytes therefore cannot
     * serve them -- LoadResource() returns nullptr without a factory. LoadFileProcess
     * hands back the untouched file buffer (64-byte OTR/Torch header followed by the
     * Torch-serialized payload) so the caller can parse it directly.
     *
     * Copies min(fileSize, outSize) bytes -- a partial read is intentional: a ghost
     * entry carries ~16 KB of trailing replay data, and a caller that only needs the
     * leading record header passes a small buffer instead of an oversized one.
     *
     * fileSize note: archive backends (O2rArchive/FolderArchive/OtrArchive) over-allocate
     * File::Buffer by a fixed +4096 guard region, so Buffer->size() is NOT the real entry
     * size and would silently zero-pad truncated reads instead of surfacing a short read to
     * exact-size integrity gates downstream. File::TrueSize carries the real entry byte
     * count and is preferred here; Buffer->size() is kept only as a defensive fallback for
     * File instances where TrueSize is unset (0), and as a hard clamp so the copy can never
     * run past the allocated buffer regardless of what TrueSize reports. */
    auto file = rm->LoadFileProcess(std::string(key));
    if ((file == nullptr) || (file->Buffer == nullptr)) {
        return 0;
    }

    const size_t bufferSize = file->Buffer->size();
    const size_t fileSize = (file->TrueSize != 0) ? file->TrueSize : bufferSize;
    if (fileSize == 0) {
        return 0;
    }

    size_t copy = (fileSize < outSize) ? fileSize : outSize;
    if (copy > bufferSize) {
        copy = bufferSize;
    }
    std::memcpy(out, file->Buffer->data(), copy);
    if (copiedSize != nullptr) {
        *copiedSize = copy;
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
