#pragma once
// G-Diffuser — resource factory registration (Slice 4c / R1).
#include <memory>

namespace Ship {
class ResourceLoader;
}

namespace GDiffuser {
// Registers all resource factories the port needs with the given libultraship ResourceLoader.
// Pass Context's resource loader after the ResourceManager is initialized, before loading any
// game resources. (Takes the loader explicitly so it works whether or not the global Context
// singleton is set, e.g. under CreateUninitializedInstance.)
void RegisterResourceFactories(std::shared_ptr<Ship::ResourceLoader> loader);
} // namespace GDiffuser
