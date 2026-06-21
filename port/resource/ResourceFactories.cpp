// G-Diffuser — resource factory registration (Slice 4c / R1).
// Registers the Fast3D + generic resource factories libultraship provides, so the .o2r
// entries Torch produced (textures, vertices, display lists, matrices, lights, blobs) load
// as IResource objects. F-Zero-X-specific types (Course, EAD anim/limb, GhostRecord,
// Sequence, SoundFont) get custom factories in a later R1 step.

#include "resource/ResourceFactories.h"

#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"
#include "ship/resource/ResourceLoader.h"
#include "ship/resource/ResourceType.h"
#include "ship/resource/File.h"
#include "ship/resource/factory/BlobFactory.h"

#include "fast/resource/ResourceType.h"
#include "fast/resource/factory/TextureFactory.h"
#include "fast/resource/factory/VertexFactory.h"
#include "fast/resource/factory/DisplayListFactory.h"
#include "fast/resource/factory/MatrixFactory.h"
#include "fast/resource/factory/LightFactory.h"

#include <memory>

namespace GDiffuser {

void RegisterResourceFactories(std::shared_ptr<Ship::ResourceLoader> loader) {
    auto reg = [&](std::shared_ptr<Ship::ResourceFactory> factory, const char* name, uint32_t type,
                   uint32_t version) {
        loader->RegisterResourceFactory(std::move(factory), RESOURCE_FORMAT_BINARY, name, type, version);
    };

    // Generic
    reg(std::make_shared<Ship::ResourceFactoryBinaryBlobV0>(), "Blob",
        static_cast<uint32_t>(Ship::ResourceType::Blob), 0);

    // Fast3D (the bulk of F-Zero X assets)
    reg(std::make_shared<Fast::ResourceFactoryBinaryTextureV0>(), "Texture",
        static_cast<uint32_t>(Fast::ResourceType::Texture), 0);
    reg(std::make_shared<Fast::ResourceFactoryBinaryTextureV1>(), "Texture",
        static_cast<uint32_t>(Fast::ResourceType::Texture), 1);
    reg(std::make_shared<Fast::ResourceFactoryBinaryVertexV0>(), "Vertex",
        static_cast<uint32_t>(Fast::ResourceType::Vertex), 0);
    reg(std::make_shared<Fast::ResourceFactoryBinaryDisplayListV0>(), "DisplayList",
        static_cast<uint32_t>(Fast::ResourceType::DisplayList), 0);
    reg(std::make_shared<Fast::ResourceFactoryBinaryMatrixV0>(), "Matrix",
        static_cast<uint32_t>(Fast::ResourceType::Matrix), 0);
    reg(std::make_shared<Fast::ResourceFactoryBinaryLightV0>(), "Light",
        static_cast<uint32_t>(Fast::ResourceType::Light), 0);

    // TODO (R1b): F-Zero-X-specific factories — Course, EADAnimation, EADLimb, GhostRecord,
    // Sequence, SoundFont (mirror torch/src/factories/fzerox/).
}

} // namespace GDiffuser
