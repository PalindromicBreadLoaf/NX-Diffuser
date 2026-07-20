// G-Diffuser — first-boot setup + portable data-directory resolution.
//
// Runs once at the very top of main(), before any libultraship path resolution, so that:
//   * a true development tree with assets/extracted/generic.o2r may boot headlessly with its ROM;
//   * every portable install uses the executable directory for data and runs the in-window setup on
//     its first launch, even when the user already placed all three canonical inputs beside the game.
//
// See docs/FIRST_BOOT_DESIGN.md for the full inventory, layout, flow, and honest scope limits
// (notably: generic.o2r generation and Windows save-path redirection are NOT solved by this slice).
#pragma once

#include <string>

namespace gdx {

enum class FirstBootStatus {
    DevLayout,      // A true development tree supplies generic.o2r and a ROM. Boot without the wizard.
    SetupComplete,  // Setup verified or freshly completed. Boot with the configured paths.
    NeedsSetup,     // No dev/completed layout. exeDir/dataDir are resolved and the working directory is
                    // set, but the ROM/EK disk/IPL are missing: the caller must run the IN-WINDOW setup
                    // flow (port/gdx_firstboot_gui.{h,cpp}) after the window/Gui/FileDropMgr exist.
    Aborted,        // Reserved: a hard, non-recoverable setup failure. The caller should exit cleanly.
};

struct FirstBootResult {
    FirstBootStatus status = FirstBootStatus::DevLayout;
    // Absolute path to the ROM the caller should load. Empty means "let the existing loader decide"
    // (its own picker/env/next-to-exe fallbacks). When non-empty, main() injects it as a synthetic
    // argv entry so rom_buffer.cpp loads it via its CLI-arg branch and never opens its own picker.
    std::string romPath;
    std::string dataDir;        // Resolved data directory (informational / logging).
    // Directory the executable lives in. The runtime O2R extractor (gdx_extract_launch) locates the
    // packaged gdx-extract child + decomp-recipes here, which — in installed/portable mode — is NOT
    // the same as dataDir (the extractor reads recipes from exeDir but writes generic.o2r to dataDir).
    std::string exeDir;
    bool chdirApplied = false;  // True when the working directory was moved to dataDir (installed mode).
};

// argv0 is argv[0] (used only for exe-directory fallback when the OS query fails).
FirstBootResult FirstBootRun(const char* argv0);

// True when either supplied path is inside a source tree that already provides
// assets/extracted/generic.o2r. Portable release folders must return false.
bool DevelopmentTreeProvidesArchive(const std::string& exeDir, const std::string& cwd);

// ── Shared setup helpers (reused by the in-window GUI setup flow) ─────────────────────────────────
// These expose the canonical file names, structural validators, copy/persist semantics, and the
// native file picker so port/gdx_firstboot_gui.cpp can drive the exact same acquisition rules the
// old blocking wizard used, but from an ImGui screen. All paths are UTF-8 std::string.

// Canonical on-disk names inside the data directory (what the stock loaders search for).
const char* SetupRomFileName();   // "baserom.us.rev0.z64"
const char* SetupDiskFileName();  // "baserom.translated.ek.ndd"
const char* SetupIplFileName();   // "N64DDIPLROM.n64"

// Structural validators. Return true if the file at `path` is a plausible input; on false, `why`
// receives a short human-readable reason (region/size/magic mismatch). A non-existent file is
// reported as invalid.
bool ValidateRomFile(const std::string& path, std::string& why);
bool ValidateDiskFile(const std::string& path, std::string& why);
bool ValidateIplFile(const std::string& path, std::string& why);

// Copy `srcPath` into `dataDir`/`dstName`, overwriting any existing file. Returns true on success.
bool CopyInputInto(const std::string& srcPath, const std::string& dataDir, const char* dstName);

// Write the completion marker (Setup.Complete=1) plus the recorded input paths into the state file
// (gdx_firstboot.cfg) in `dataDir`. Returns true on success (a failure only means setup re-runs).
bool WriteSetupComplete(const std::string& dataDir, const std::string& romPath,
                        const std::string& diskPath, const std::string& iplPath);

// True when a native "Browse…" file picker is available on this platform (Windows only). On other
// platforms the GUI relies exclusively on drag & drop (no native picker in this port).
bool NativeFilePickerAvailable();

// Native file-open dialogs (Windows). Return the selected absolute path, or empty if the user
// cancelled or the platform has no native picker. Each preselects an appropriate file filter.
std::string PickRomFile();
std::string PickDiskFile();
std::string PickIplFile();

} // namespace gdx
