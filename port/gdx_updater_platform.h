#pragma once

#include <string>

namespace gdx::updater::platform {

bool NetworkInit();
void NetworkExit();

// Asks the loader to run `nroPath` instead of returning to the homebrew menu when this process
// exits.
bool QueueNextLoad(const std::string& nroPath);

} // namespace gdx::updater::platform
