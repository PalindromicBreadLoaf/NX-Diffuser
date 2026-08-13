// Definitions for the two subsystems port/CMakeLists.txt drops on Switch

#ifdef __SWITCH__

#include "gdx_discord.h"
#include "gdx_dump_launch.h"

extern "C" void gdx_discord_tick(void) {
}

extern "C" void gdx_discord_shutdown(void) {
}

namespace gdx {

DumpEnvironment GdxDumpDiscover() {
    DumpEnvironment env;
    env.reason = "Asset dumping is PC only";
    return env;
}

std::vector<std::string> GdxDumpCurrentClasses() {
    return {};
}

std::string GdxDumpPrettyName(const std::string& rawClass) {
    return rawClass;
}

void GdxDumpBeginClassListProbe(const DumpEnvironment&) {
}

void GdxDumpStartBatch(const DumpEnvironment&, const std::vector<std::string>&, const std::string&) {
}

void GdxDumpRequestCancel() {
}

DumpBatchSnapshot GdxDumpSnapshot() {
    return DumpBatchSnapshot();
}

} // namespace gdx

#endif // __SWITCH__
