// G-Diffuser — first-boot setup + per-user data directory resolution.
//
// Runs once at the very top of main(), before any libultraship path resolution, so that:
//   * a DEV layout (ROM already sitting next to the exe) boots exactly as before, headless, with
//     NO file picker (the ROM path is auto-injected so rom_buffer.cpp's CLI-arg branch wins);
//   * an INSTALLED layout resolves a per-user data directory (%APPDATA%/G-Diffuser on Windows,
//     $XDG_DATA_HOME/G-Diffuser on Linux), sets it as the working directory so config/logs/disk/IPL
//     consolidate there, and — on a fresh install — runs a native file-picker wizard to acquire the
//     ROM, the Expansion Kit .ndd, and the 64DD IPL ROM, validating and copying them in.
//
// See docs/FIRST_BOOT_DESIGN.md for the full inventory, layout, flow, and honest scope limits
// (notably: generic.o2r generation and Windows save-path redirection are NOT solved by this slice).
#pragma once

#include <string>

namespace gdx {

enum class FirstBootStatus {
    DevLayout,      // Inputs already next to the exe. No wizard, working dir untouched, boot as before.
    SetupComplete,  // Per-user setup verified or freshly completed. Boot with the configured paths.
    Aborted,        // The user cancelled a REQUIRED pick (the ROM). The caller should exit cleanly.
};

struct FirstBootResult {
    FirstBootStatus status = FirstBootStatus::DevLayout;
    // Absolute path to the ROM the caller should load. Empty means "let the existing loader decide"
    // (its own picker/env/next-to-exe fallbacks). When non-empty, main() injects it as a synthetic
    // argv entry so rom_buffer.cpp loads it via its CLI-arg branch and never opens its own picker.
    std::string romPath;
    std::string dataDir;        // Resolved data directory (informational / logging).
    bool chdirApplied = false;  // True when the working directory was moved to dataDir (installed mode).
};

// argv0 is argv[0] (used only for exe-directory fallback when the OS query fails).
FirstBootResult FirstBootRun(const char* argv0);

} // namespace gdx
