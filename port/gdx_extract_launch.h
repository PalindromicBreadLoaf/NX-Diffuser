// G-Diffuser — runtime O2R asset extraction launcher.
//
// Produces (or refreshes) <dataDir>/generic.o2r from the cartridge ROM by spawning the packaged
// `gdx-extract` child process against the shipped `decomp-recipes` tree. This is the runtime
// counterpart to the build-time archive: an installed/packaged build has NO generic.o2r until this
// runs (see docs/investigation/2026-07-18/o2r-migration/W0_FIRSTBOOT_INTEGRATION.md §7).
//
// Contract references (docs/.../o2r-migration/P0_CONTRACTS.md):
//   C1  ROM SHA-1 validation before spawn (expected hash read from decomp-recipes/config.yml).
//   C5  validation-before-install (exit code, zip entry count, archive SHA-256, version entry) +
//       atomic temp->rename install with a Windows sharing-violation retry loop.
//   C6  complete-or-absent: extraction NEVER blocks boot; on any failure the temp is deleted, any
//       previous archive is preserved, and the proven raw-ROM fallback carries the session.
//   C7  state model: completion sidecar gdx_extract_state.cfg + warm-boot skip.
//   C8  integration point (main.cpp ~285) + progress UX (Win32 modeless dialog / Linux log-only).
//
// This TU is part of the G-Diffuser exe target (not the decomp game library), so it may freely use
// the host CRT, <filesystem>, and (Windows) the Win32 process + common-controls APIs. It runs before
// libultraship is constructed, so it logs through the port's own gdx_port_logf and touches no LUS state.
#pragma once

#include <string>

namespace gdx {

// Outcome of GdxExtractEnsureArchive. The caller (main.cpp) only logs this; boot proceeds regardless
// (C6). "FailedRawFallback" is deliberately the single catch-all failure value — the boot posture is
// identical for every failure mode (no valid generic.o2r produced → raw-ROM fallback).
enum class ExtractOutcome {
    UpToDate,          // A valid (golden) generic.o2r is already present; nothing to do.
    Extracted,         // The extractor ran, its output validated, and it was atomically installed.
    FailedRawFallback, // Extraction was not performed or did not validate; boot degrades to raw ROM.
};

// Ensure a valid <dataDir>/generic.o2r exists, extracting it from the cartridge ROM if needed.
//
//   dataDir  Absolute path to the writable per-user data directory (the extractor's output dir and
//            the sidecar location). MUST be passed explicitly — never rely on the inherited CWD (C2).
//   romPath  Absolute path to the validated cartridge ROM (as installed/injected by first-boot).
//   exeDir   Absolute path to the executable's directory (where gdx-extract + decomp-recipes ship).
//
// Never throws; every failure path returns FailedRawFallback with an actionable gdx_port_logf line.
// The caller MUST skip this entirely in the dev/portable layout (FirstBootStatus::DevLayout), where
// the in-tree assets/extracted probe already provides generic.o2r.
ExtractOutcome GdxExtractEnsureArchive(const char* dataDir, const char* romPath, const char* exeDir);

// Human-readable label for logging.
const char* GdxExtractOutcomeString(ExtractOutcome outcome);

// ── Async driver for the in-window setup GUI ─────────────────────────────────────────────────────
// GdxExtractEnsureArchive is blocking (normally ~2s, up to a 120s hang deadline). The ImGui setup
// screen must keep pumping frames while it runs, so this thin wrapper runs it on a background thread
// and exposes a pollable snapshot. Single-flight: only one async extraction may be in progress.

enum class ExtractPhase {
    Idle,     // No async extraction has been started (or it was reset).
    Running,  // The background worker is executing.
    Done,     // The worker finished; `outcome` is valid.
};

struct ExtractProgress {
    ExtractPhase phase = ExtractPhase::Idle;
    ExtractOutcome outcome = ExtractOutcome::FailedRawFallback; // valid only when phase == Done
    std::string stage;      // latest stage line captured from the extractor (may be empty)
    std::string lastError;  // last actionable error line (valid on a failed Done; may be empty)
};

// Start GdxExtractEnsureArchive on a background thread. `suppressNativeDialog` (pass true from the
// GUI) suppresses the Windows Win32 marquee progress dialog so the ImGui screen owns the progress UX.
// A no-op (logs a warning) if an async extraction is already Running.
void GdxExtractStartAsync(const char* dataDir, const char* romPath, const char* exeDir,
                          bool suppressNativeDialog);

// Snapshot the current async state. Safe to call every frame.
ExtractProgress GdxExtractPollStatus();

// Join the finished worker and reset back to Idle. Call once after handling a Done result (before a
// retry, or when leaving the setup flow). Safe to call when already Idle.
void GdxExtractResetAsync();

// ── ROM identity helpers for the setup GUI ───────────────────────────────────────────────────────
// The setup screen shows the user their ROM's SHA-1 against the expected US-rev0 hash so a wrong
// dump is diagnosed at acquisition time rather than at extraction time.

// Lowercase-hex SHA-1 of the file at `path` (empty string on read failure). ~50 ms for a 16 MiB ROM.
std::string GdxExtractFileSha1(const char* path);

// The expected US-rev0 ROM SHA-1 (lowercase hex), read from <exeDir>/decomp-recipes/config.yml with
// the built-in constant as fallback — same resolution the extraction gate uses (C1).
std::string GdxExtractExpectedRomSha1(const char* exeDir);

} // namespace gdx
