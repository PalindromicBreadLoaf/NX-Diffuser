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
    DevLayout,      // A true development tree supplies generic.o2r, a ROM, AND a valid EK disk + IPL
                    // (G-Diffuser is Expansion-Kit-mandatory — the game crashes/degrades without the
                    // 64DD disk, so a dev tree missing either input falls through to NeedsSetup instead
                    // of bypassing the wizard). Boot without the wizard.
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
    // Absolute path to the validated EK disk / IPL ROM found on a DevLayout boot (informational —
    // disk_buffer.cpp / the leo emulation already resolve these by canonical name relative to the
    // chosen ROM/exe dir/CWD; these fields just record what FirstBootRun verified is there). Empty
    // on every other status: NeedsSetup defers acquisition to the in-window wizard, which records the
    // final installed paths itself via WriteSetupComplete.
    std::string diskPath;
    std::string iplPath;
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
// Accepted alternate names for the Japanese dumps: the wizard probes these when the canonical name
// is absent, so a JP test folder needs no renaming. A JP ROM boots RAW (experimental; no archives).
const char* SetupRomFileNameJp();  // "baserom.jp.rev0.z64"
const char* SetupDiskFileNameJp(); // "baserom.jp.ek.ndd"

// Structural validators. Return true if the file at `path` is a plausible input; on false, `why`
// receives a short human-readable reason (region/size/magic mismatch). A non-existent file is
// reported as invalid.
bool ValidateRomFile(const std::string& path, std::string& why);
bool ValidateDiskFile(const std::string& path, std::string& why);
bool ValidateIplFile(const std::string& path, std::string& why);

// Copy `srcPath` into `dataDir`/`dstName`, overwriting any existing file. Returns true on success.
bool CopyInputInto(const std::string& srcPath, const std::string& dataDir, const char* dstName);

// ── SHA-1 identity recognition (region/dump labelling for the setup rows) ──────────────────────────
// The known-good SHA-1 sets for each input live as named tables in gdx_firstboot.cpp so the future JP
// build can reuse them. GdxRecognizeInput hashes a file that has ALREADY passed its structural
// Validate*File check and classifies it:
//   * ROM  — the US-rev0 dump is VerifiedKnown; the Japan dump is ACCEPTED (AcceptedUnknownWarn with
//            `jpRom` set) for the experimental raw-ROM boot — setup then SKIPS archive extraction for
//            it. Any other hash is Rejected with the generic mismatch message.
//   * IPL  — the one known dump is labelled (VerifiedKnown); every other correctly-sized dump is
//            AcceptedUnknownWarn (accepted, but the caller must surface the warning text visibly).
//   * disk — each known dump is labelled by region (VerifiedKnown); any other correctly-sized image is
//            AcceptedUnknownWarn. The size gate stays a hard reject inside ValidateDiskFile.
enum class GdxInputVerdict {
    VerifiedKnown,        // recognized known-good dump — OK/green; `message` is a confirmation label
    AcceptedUnknownWarn,  // structurally valid but unrecognized — OK/green; `message` is a visible warning
    Rejected,             // rejected — red; `message` is the reason
};
struct GdxInputRecognition {
    GdxInputVerdict verdict = GdxInputVerdict::VerifiedKnown;
    std::string sha1;     // lowercase hex (empty on read failure — then verdict is Rejected)
    std::string message;  // display text per verdict (label / warning / reason)
    // For a recognized Expansion Kit disk, the region label ("translated Expansion Kit disk" /
    // "retail Japanese Expansion Kit disk") the OK row header should show in place of the file name.
    // Also carries the ROM row's "OK (F-Zero X (Japan) — experimental)" header for an accepted JP
    // dump. Empty for every other input and verdict.
    std::string okHeaderOverride;
    // True only for the accepted Japanese ROM: the caller must SKIP archive extraction (the US
    // recipe tree cannot extract a JP ROM) and complete setup for the raw-ROM boot instead.
    bool jpRom = false;
};
// Recognize a reviewed input by SHA-1. `canonicalName` selects the ruleset (pass SetupRomFileName() /
// SetupIplFileName() / SetupDiskFileName()); `exeDir` supplies the recipe-authoritative US ROM hash.
GdxInputRecognition GdxRecognizeInput(const std::string& canonicalName, const std::string& path,
                                      const std::string& exeDir);

// Canonical installed archive names (what a validated archive satisfies which input). Exposed so the
// in-window setup rows can truthfully report a requirement met by its archive when the original file
// is gone (R7/R8: originals are deletable once the archive covers them).
const char* SetupGameArchiveFileName();  // "fzerox.o2r"    — satisfies the ROM input
const char* SetupIplArchiveFileName();   // "n64ddipl.o2r"  — satisfies the 64DD IPL input
const char* SetupDiskArchiveFileName();  // "fzerox-disk.o2r" — satisfies the EK disk input

// ── Managed disk copy (R7: disk internalization) ───────────────────────────────────────────────
// A byte-identical copy of the validated Expansion Kit disk, held under <dataDir>/media/<canonical
// disk name> so the user's original .ndd becomes deletable after setup, exactly like the ROM/IPL
// (which the port only ever reads back from their installed copies). Kept in a subdirectory SEPARATE
// from dataDir's root so a genuine second copy exists even when the root-level file (the wizard's
// CopyInputInto destination, or the DevLayout candidate itself) is the user's only other copy.
// port/disk_savefile.cpp keys its .gdd journal off the disk's LEAF NAME ONLY, never a path (see
// gdx_disk_load in disk_buffer.cpp, which passes the bare canonical name), so storing the managed
// copy under this same leaf name preserves the existing save key untouched regardless of directory.

// Absolute path of the managed disk copy inside `dataDir` (<dataDir>/media/<disk name>). Pure path
// computation -- does not touch the filesystem.
std::string ManagedDiskPath(const std::string& dataDir);

// Ensure a valid managed copy of the disk exists under `dataDir`/media, copying it from
// `validatedDiskPath` if needed. `validatedDiskPath` MUST already have passed ValidateDiskFile.
// Idempotent: a managed copy already present and correctly sized is left untouched -- never
// re-copied, never overwritten. The copy is atomic-ish (temp name in the same directory, size
// verified, then renamed into place) and, on success, its SHA-256 is best-effort recorded in the
// extraction sidecar (gdx_extract_state.cfg) via GdxExtractRecordManagedDisk. Returns true iff a
// valid managed copy exists at `outManagedPath` when this returns (pre-existing or freshly created);
// on false, `outManagedPath` is still set to the computed managed path but the file may be
// missing/incomplete -- callers must not treat it as usable.
bool EnsureManagedDiskCopy(const std::string& dataDir, const std::string& validatedDiskPath,
                           std::string& outManagedPath);

// Write the completion marker (Setup.Complete=1) plus the recorded input paths into the state file
// (gdx_firstboot.cfg) in `dataDir`. Returns true on success (a failure only means setup re-runs).
bool WriteSetupComplete(const std::string& dataDir, const std::string& romPath,
                        const std::string& diskPath, const std::string& iplPath);

// ── Missing-input diagnostic (R4 C-R4.1 UX helper) ─────────────────────────────────────────────
// Conservative, read-only summary of which of the three canonical inputs (ROM / 64DD IPL ROM /
// Expansion Kit disk) and which game archive are missing or invalid under `dataDir`. Built entirely
// from the same file-existence + structural-validator helpers this header already exports (plus the
// R7 managed-copy path), so it never opens a new probe surface and never throws. Returns an empty
// string when everything needed to boot is present and valid.
//
// Intended consumer: main.cpp's no-ROM boot error path (C-R4.1 -- "archives validated ⇒ boot
// proceeds without FZEROX_ROM; missing/invalid archives + missing ROM ⇒ actionable wizard error").
// This helper is NOT wired into main.cpp by this change -- another agent owns that integration this
// wave; it only produces the user-presentable text the future error path can display.
std::string GdxFirstBootDescribeMissing(const std::string& dataDir);

// ── Exported archive-satisfied checks (shared with the in-window wizard's Recheck) ────────────────
// The wizard's Recheck() (gdx_firstboot_gui.cpp) needs to know whether a requirement is satisfied by
// its INSTALLED ARCHIVE using the exact same hash-validated acceptance chain FirstBootRun's
// SetupComplete fast path uses -- not a bare existence probe, which would accept a corrupt/foreign
// archive as satisfying. The underlying gameArchiveSatisfies / iplArchiveSatisfies / diskArchiveSatisfies
// checks stay file-local statics in gdx_firstboot.cpp; this is a thin exported wrapper around them.
enum class GdxFirstbootArchiveKind { Game, Ipl, Disk };

// True when the canonical archive for `kind` is installed under `dataDir` and, when this build
// recorded a golden hash for it, hashes to the recorded value -- the same acceptance chain
// FirstBootRun's SetupComplete fast path uses. A failing hash quarantines the archive exactly like the
// fast path does (renamed <name>.bad).
//
// Caching: a PASSING result latches gdx_extract_launch's per-boot archive-validation latch (see
// GdxExtractMarkArchiveValidated in gdx_extract_launch.h), so a later same-boot re-check of the same
// kind -- either another call to this wrapper, or GdxExtractEnsureArchive's own warm-boot check --
// skips the redundant re-hash of a multi-MB/multi-ten-MB file. This wrapper does NOT itself cache
// short of that latch: Recheck() in gdx_firstboot_gui.cpp is event-driven (constructor + file-drop +
// Browse click), never called per-frame, so a per-(kind, mtime+size) cache is not required for that
// caller. A future per-frame caller MUST add its own memoization keyed on (kind, mtime, size) before
// calling this in a hot loop -- the underlying hash is not free.
bool GdxFirstbootArchiveSatisfies(GdxFirstbootArchiveKind kind, const std::string& dataDir);

// True when a native "Browse…" file picker is available on this platform (Windows only). On other
// platforms the GUI relies exclusively on drag & drop (no native picker in this port).
bool NativeFilePickerAvailable();

// Native file-open dialogs (Windows). Return the selected absolute path, or empty if the user
// cancelled or the platform has no native picker. Each preselects an appropriate file filter.
std::string PickRomFile();
std::string PickDiskFile();
std::string PickIplFile();

} // namespace gdx
