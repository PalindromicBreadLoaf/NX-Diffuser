// G-Diffuser — in-window first-time setup flow (ImGui). See gdx_firstboot_gui.h.
//
// This TU is part of the G-Diffuser exe target. It runs AFTER libultraship is up (window + Gui +
// FileDropMgr exist) and BEFORE the game boots, so it may freely touch LUS state and ImGui.
//
// Frame pump: the game loop is not running yet, so this flow drives its own GUI-only frames using the
// abstract Window interface — HandleEvents / StartDraw / StartFrame / RunGuiOnly / EndDraw / EndFrame,
// mirroring Fast3dWindow::DrawAndRunGraphicsCommands but substituting RunGuiOnly() (which clears/binds
// the game framebuffer and renders no gfx task) for the game's Run(). No libultraship change needed —
// RunGuiOnly()/StartFrame()/EndFrame() are all pure-virtual on Ship::Window.

#include "gdx_firstboot_gui.h"

#include "gdx_firstboot.h"       // validators, canonical names, copy/persist helpers, native picker
#include "gdx_extract_launch.h"  // async extraction driver (start/poll/reset)
#include "gdx_gui.h"             // optional bundled large/mono fonts
#include "port_log.h"

#include "ship/Context.h"
#include "ship/window/Window.h"
#include "ship/window/MouseStateManager.h"
#include "ship/window/FileDropMgr.h"
#include "ship/window/gui/Gui.h"
#include "ship/resource/ResourceManager.h"
#include "ship/resource/archive/ArchiveManager.h"

#include <imgui.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace gdx {
namespace {

// The installed game archive name (matches gdx_extract_launch.cpp's kArchiveName). Kept as a local
// literal rather than exported: it is the SoH/Starship game-named-archive convention and is stable.
constexpr const char* kGameArchiveName = "fzerox.o2r";

enum class Phase {
    Acquire,       // Waiting for the three inputs to be provided + validated.
    Extracting,    // Async extraction is running; showing live progress.
    ExtractFailed, // Extraction failed; offering [Continue anyway] / [Retry].
    Done,          // Setup complete — the loop exits and boot continues.
};

enum class RowStatus { Missing, Ok, Invalid };

using Validator = bool (*)(const std::string&, std::string&);
using Picker = std::string (*)();

struct Row {
    const char* label;         // human-facing label
    const char* canonicalName; // on-disk name inside the data dir
    Validator validate;
    Picker pick;               // native picker (Windows); returns empty elsewhere / on cancel
    RowStatus status = RowStatus::Missing;
    std::string reason;        // populated when status == Invalid
    std::string detail;        // extra verified-identity line shown under the status (may be empty)
    std::string warning;       // visible warning shown on an OK row (unrecognized-but-accepted dump)
    std::string okHeader;      // overrides the "OK (...)" header text when non-empty (region label, or
                               // the archive-satisfied message when the original file is gone)
    // The actual filesystem path Recheck() resolved and validated this row against -- normally
    // equal to RowPath(i) (the canonical dataDir path), but for the disk row it may instead be the
    // managed-copy fallback path (see Recheck's managed-copy handling), and for the ROM/disk rows it may
    // be the accepted Japanese alternate name (SetupRomFileNameJp/SetupDiskFileNameJp). Callers that
    // need the row's real on-disk location (rather than always the canonical path) must use this,
    // not RowPath(i).
    std::string resolvedPath;
    // True when the ROM row resolved to the accepted Japanese dump: setup must SKIP archive
    // extraction (US recipes cannot process it) and complete for the experimental raw-ROM boot.
    bool jpRom = false;
};

// ── Setup screen state machine ───────────────────────────────────────────────────────────────────
class SetupScreen {
  public:
    SetupScreen(std::string dataDir, std::string exeDir)
        : mDataDir(std::move(dataDir)), mExeDir(std::move(exeDir)) {
        mRows[0] = { "F-Zero X ROM (US rev0, .z64)", SetupRomFileName(), &ValidateRomFile, &PickRomFile };
        mRows[1] = { "Expansion Kit disk (.ndd)", SetupDiskFileName(), &ValidateDiskFile, &PickDiskFile };
        mRows[2] = { "64DD IPL ROM (N64DDIPLROM.n64)", SetupIplFileName(), &ValidateIplFile, &PickIplFile };
        for (Row& r : mRows) {
            Recheck(r); // pre-check files that already exist beside the exe (resumed setup)
        }
    }

    // Returns true if setup completed, false if the window was closed.
    bool Run(std::string& outRomPath) {
        auto ctx = Ship::Context::GetInstance();
        auto w = (ctx != nullptr) ? ctx->GetWindow() : nullptr;
        if (w == nullptr) {
            gdx_port_logf("[setup] no window; cannot run the in-window setup flow\n");
            return false;
        }

        auto fileDrop = ctx->GetFileDropMgr();
        if (fileDrop != nullptr) {
            fileDrop->RegisterDropHandler(&SetupScreen::OnFileDroppedThunk);
        }

        gdx_port_logf("[setup] entering in-window first-time setup\n");
        while (w->IsRunning()) {
            w->HandleEvents(); // drag-and-drop events dispatch synchronously here (main thread)
            Tick();
            if (mPhase == Phase::Done) {
                break;
            }
            w->GetMouseStateManager()->StartFrame();
            w->GetGui()->StartDraw(); // ImGui NewFrame + menu/registered windows
            DrawUI();                 // our setup window, drawn into the same ImGui frame
            w->StartFrame();          // size the game framebuffers
            w->RunGuiOnly();          // clear/bind the game FB, run no gfx task
            w->GetGui()->EndDraw();   // composite + ImGui::Render + present floating windows
            w->EndFrame();            // swap buffers
        }

        if (fileDrop != nullptr) {
            fileDrop->UnregisterDropHandler(&SetupScreen::OnFileDroppedThunk);
        }

        const bool completed = (mPhase == Phase::Done);
        if (completed) {
            // Resolved path, not the canonical name: an accepted Japanese ROM lives under its own
            // alternate filename (SetupRomFileNameJp) and must be handed to the boot as-is.
            outRomPath = mRows[0].resolvedPath.empty() ? RowPath(0) : mRows[0].resolvedPath;
            gdx_port_logf("[setup] completed; ROM=%s archiveMounted=%d\n", outRomPath.c_str(),
                          mArchiveMounted ? 1 : 0);
        } else {
            gdx_port_logf("[setup] window closed before completion; exiting\n");
        }
        return completed;
    }

    // Called from the (main-thread) FileDropMgr callback for a single dropped file.
    void HandleDrop(const char* path) {
        if (path == nullptr || mPhase != Phase::Acquire) {
            return; // only accept drops while acquiring inputs
        }
        std::string src(path);
        std::string why;
        // Classify by validator: try ROM, then disk, then IPL. First match wins.
        for (Row& r : mRows) {
            std::string reason;
            if (r.validate(src, reason)) {
                if (CopyInputInto(src, mDataDir, r.canonicalName)) {
                    Recheck(r);
                    mDropError.clear();
                    gdx_port_logf("[setup] accepted dropped %s -> %s\n", src.c_str(), r.canonicalName);
                } else {
                    mDropError = "Could not copy the dropped file next to the game.";
                }
                return;
            }
        }
        mDropError = "That file is not a recognized F-Zero X ROM, Expansion Kit disk, or 64DD IPL ROM.";
        gdx_port_logf("[setup] rejected dropped file (matches no expected input): %s\n", src.c_str());
    }

  private:
    // Absolute path of a row's installed copy inside the data dir.
    std::string RowPath(int i) const {
        return (fs::path(mDataDir) / mRows[i].canonicalName).string();
    }

    // Name of the installed archive that satisfies a given row's input (originals are deletable
    // once the archive covers them). Empty string for an unrecognized canonical name.
    static const char* ArchiveForCanonical(const char* canonicalName) {
        if (std::string(canonicalName) == SetupRomFileName())  return SetupGameArchiveFileName();
        if (std::string(canonicalName) == SetupDiskFileName()) return SetupDiskArchiveFileName();
        if (std::string(canonicalName) == SetupIplFileName())  return SetupIplArchiveFileName();
        return "";
    }

    // Hash-validated archive kind for a given row's canonical name (companion to ArchiveForCanonical) --
    // used so Recheck()'s "satisfied by installed archive" determination goes through
    // GdxFirstbootArchiveSatisfies (F1 fix) instead of a bare presence probe, which would accept a
    // corrupt/foreign archive as satisfying the row. Returns false via outKind left unset when the
    // canonical name is unrecognized (mirrors ArchiveForCanonical's empty-string case).
    static bool ArchiveKindForCanonical(const char* canonicalName, GdxFirstbootArchiveKind& outKind) {
        if (std::string(canonicalName) == SetupRomFileName())  { outKind = GdxFirstbootArchiveKind::Game; return true; }
        if (std::string(canonicalName) == SetupDiskFileName()) { outKind = GdxFirstbootArchiveKind::Disk; return true; }
        if (std::string(canonicalName) == SetupIplFileName())  { outKind = GdxFirstbootArchiveKind::Ipl;  return true; }
        return false;
    }

    void Recheck(Row& r) {
        std::string dst = (fs::path(mDataDir) / r.canonicalName).string();
        std::error_code ec;
        r.detail.clear();
        r.warning.clear();
        r.okHeader.clear();
        r.jpRom = false;
        if (!fs::is_regular_file(fs::path(dst), ec)) {
            // Accepted alternate names: probe them before any derived fallback so a JP test folder
            // (baserom.jp.rev0.z64 / baserom.jp.ek.ndd) or a folder holding the US prototype IPL
            // dump filename (64DD_IPL_US_MJR.n64) is detected without renaming.
            if (std::string(r.canonicalName) == SetupIplFileName()) {
                // Shared with the boot path (gdx_firstboot.cpp's FirstBootRun, gdx_extract_launch.cpp's
                // ensureIplArchive) via GdxFindIplSourceInDir so the accepted IPL alt name can never
                // drift between the wizard and boot-time extraction again.
                std::string found = GdxFindIplSourceInDir(mDataDir);
                if (!found.empty()) {
                    dst = found;
                }
            } else {
                const char* altName = nullptr;
                if (std::string(r.canonicalName) == SetupRomFileName()) {
                    altName = SetupRomFileNameJp();
                } else if (std::string(r.canonicalName) == SetupDiskFileName()) {
                    altName = SetupDiskFileNameJp();
                }
                if (altName != nullptr) {
                    std::string alt = (fs::path(mDataDir) / altName).string();
                    std::error_code altEc;
                    if (fs::is_regular_file(fs::path(alt), altEc)) {
                        dst = alt;
                    }
                }
            }
            // The disk row's canonical copy may be gone because the user deleted their
            // original .ndd after a PRIOR setup already created the managed backup under
            // <dataDir>/media. Resolve against that managed copy rather than reporting Missing.
            if (!fs::is_regular_file(fs::path(dst), ec) &&
                std::string(r.canonicalName) == SetupDiskFileName()) {
                std::string managed = ManagedDiskPath(mDataDir);
                std::error_code mgEc;
                if (fs::is_regular_file(fs::path(managed), mgEc)) {
                    dst = managed;
                }
            }
            if (!fs::is_regular_file(fs::path(dst), ec)) {
                // Acceptance chain: before reporting Missing, honor the fact that a requirement
                // met by its INSTALLED ARCHIVE is satisfied, not missing — the original file is
                // deletable and the game boots archive-only from it (rom_buffer.cpp / disk_buffer.cpp).
                // Show that truthfully in the OK/green state instead of falsely demanding the original.
                // Use the hash-validated GdxFirstbootArchiveSatisfies (F1 fix) rather than a bare
                // existence probe -- a present-but-corrupt/foreign archive must not read as satisfied
                // here any more than it does in FirstBootRun's own SetupComplete fast path.
                const char* archiveName = ArchiveForCanonical(r.canonicalName);
                std::string archivePath = (fs::path(mDataDir) / archiveName).string();
                GdxFirstbootArchiveKind archiveKind;
                const bool archiveSatisfied = archiveName[0] != '\0' &&
                    ArchiveKindForCanonical(r.canonicalName, archiveKind) &&
                    GdxFirstbootArchiveSatisfies(archiveKind, mDataDir);
                if (archiveSatisfied) {
                    r.status = RowStatus::Ok;
                    r.reason.clear();
                    r.resolvedPath = archivePath;
                    r.okHeader = "Satisfied by installed archive (original file no longer needed)";
                    r.detail = std::string("Served from ") + archiveName;
                    gdx_port_logf("[setup] row '%s': satisfied by installed archive %s "
                                  "(original file no longer needed)\n", r.canonicalName, archiveName);
                    return;
                }
                r.status = RowStatus::Missing;
                r.reason.clear();
                r.resolvedPath.clear();
                gdx_port_logf("[setup] row '%s': missing\n", r.canonicalName);
                return;
            }
        }
        std::string why;
        if (!r.validate(dst, why)) {
            r.status = RowStatus::Invalid;
            r.reason = why;
            r.resolvedPath.clear();
            gdx_port_logf("[setup] row '%s': invalid -- %s\n", r.canonicalName, why.c_str());
            return;
        }
        r.status = RowStatus::Ok;
        r.reason.clear();
        // Record the path actually resolved above (canonical dataDir path, or the managed-copy fallback
        // for the disk row) so callers needing the real on-disk location -- e.g.
        // ConfirmAndStartExtraction's WriteSetupComplete/EnsureManagedDiskCopy calls, and the "File:"
        // provenance line in DrawAcquire -- don't re-derive it and risk showing the wrong path.
        r.resolvedPath = dst;

        // SHA-1 identity recognition (region/dump labelling). Hashing happens only when a row is
        // (re)checked, never per frame; even the 64.9 MB disk is negligible in normal setup use. The
        // rulesets (ROM strict US-rev0; IPL/disk accept-with-label-or-warning) and all message text
        // live centrally in gdx_firstboot.cpp so the future JP build reuses the same tables.
        GdxInputRecognition rec = GdxRecognizeInput(r.canonicalName, dst, mExeDir);
        switch (rec.verdict) {
            case GdxInputVerdict::VerifiedKnown:
                r.detail = rec.message;
                if (!rec.okHeaderOverride.empty()) {
                    r.okHeader = rec.okHeaderOverride; // e.g. the EK disk region label in the header
                }
                gdx_port_logf("[setup] row '%s': OK -- %s\n", r.canonicalName, rec.message.c_str());
                break;
            case GdxInputVerdict::AcceptedUnknownWarn:
                r.detail = "SHA-1: " + rec.sha1;
                r.warning = rec.message; // visible warning shown on the OK row (untested dump)
                if (!rec.okHeaderOverride.empty()) {
                    r.okHeader = rec.okHeaderOverride; // e.g. the accepted-JP-ROM experimental header
                }
                r.jpRom = rec.jpRom;
                gdx_port_logf("[setup] row '%s': OK (accepted) WARNING -- %s\n", r.canonicalName,
                              rec.message.c_str());
                break;
            case GdxInputVerdict::Rejected:
                r.status = RowStatus::Invalid;
                r.reason = rec.message;
                r.detail.clear();
                r.resolvedPath.clear();
                gdx_port_logf("[setup] row '%s': rejected -- %s\n", r.canonicalName, rec.message.c_str());
                break;
        }
    }

    bool AllRowsOk() const {
        for (const Row& r : mRows) {
            if (r.status != RowStatus::Ok) {
                return false;
            }
        }
        return true;
    }

    void Browse(Row& r) {
        std::string picked = r.pick();
        if (picked.empty()) {
            return; // cancelled or no native picker
        }
        std::string why;
        if (!r.validate(picked, why)) {
            r.status = RowStatus::Invalid;
            r.reason = why;
            return;
        }
        if (!CopyInputInto(picked, mDataDir, r.canonicalName)) {
            r.status = RowStatus::Invalid;
            r.reason = "could not copy the selected file next to the game";
            return;
        }
        Recheck(r);
    }

    void StartExtraction() {
        // suppressNativeDialog = true: the ImGui screen owns the progress UX.
        const std::string romForExtraction =
            mRows[0].resolvedPath.empty() ? RowPath(0) : mRows[0].resolvedPath;
        GdxExtractStartAsync(mDataDir.c_str(), romForExtraction.c_str(), mExeDir.c_str(),
                             /*suppress=*/true);
        mStage.clear();
        mError.clear();
        mLog.clear();
        mEntriesSeen = 0;
        mSubStage = 0;
        mPhase = Phase::Extracting;
    }

    void ConfirmAndStartExtraction() {
        if (!AllRowsOk()) {
            return;
        }
        // Use the disk row's Recheck-resolved actual path, not always RowPath(1): a managed-copy-only
        // re-run (original .ndd deleted after a prior setup already created the managed
        // backup) resolves and validates against <dataDir>/media, not the canonical dataDir path.
        // Passing RowPath(1) here would record a nonexistent canonical path in the sidecar and make
        // EnsureManagedDiskCopy warn spuriously about failing to (re)create a copy that already exists.
        const std::string& diskPath = mRows[1].resolvedPath.empty() ? RowPath(1) : mRows[1].resolvedPath;
        // Same resolved-path rule for the ROM row: an accepted Japanese dump typically lives under
        // its own alternate name (SetupRomFileNameJp) -- though recognition is by HASH, so a JP dump
        // under the canonical US filename is also accepted; resolvedPath is correct either way.
        const std::string& romPath = mRows[0].resolvedPath.empty() ? RowPath(0) : mRows[0].resolvedPath;
        if (!WriteSetupComplete(mDataDir, romPath, diskPath, RowPath(2))) {
            mDropError = "Could not save the setup state next to the game.";
            return;
        }
        // The disk row is validated and committed at dataDir/kDiskName (via CopyInputInto in
        // Browse()/HandleDrop()). The SEPARATE media/ backup copy that used to be created here is
        // retired (v1.0.0): it predates full .o2r support, and with fzerox-disk.o2r as the port's
        // stable copy it was a third copy of a 64MB file whose only readers all fall back to the
        // dataDir committed copy anyway. Existing media/ copies from older installs remain honored
        // read-only; only creation is gone. See the matching retirement notes in gdx_firstboot.cpp.
        // Accepted Japanese ROM: the US recipe tree cannot extract it, so skip archive extraction
        // entirely and complete setup for the experimental raw-ROM boot (rom_buffer loads the ROM
        // directly; FirstBootRun's fast path recognizes the recorded JP hash on later boots).
        if (mRows[0].jpRom) {
            gdx_port_logf("[setup] user confirmed all inputs; Japanese ROM accepted -- skipping "
                          "archive extraction (raw-ROM boot, experimental)\n");
            mPhase = Phase::Done;
            return;
        }
        gdx_port_logf("[setup] user confirmed all inputs; wrote completion marker; starting extraction\n");
        StartExtraction();
    }

    void MountArchive() {
        auto ctx = Ship::Context::GetInstance();
        auto rm = (ctx != nullptr) ? ctx->GetResourceManager() : nullptr;
        auto am = (rm != nullptr) ? rm->GetArchiveManager() : nullptr;
        if (am == nullptr) {
            gdx_port_logf("[setup] WARNING: archive manager unavailable; cannot hot-mount %s\n",
                          kGameArchiveName);
            return;
        }
        const std::string archive = (fs::path(mDataDir) / kGameArchiveName).string();
        std::error_code ec;
        if (!fs::is_regular_file(fs::path(archive), ec)) {
            gdx_port_logf("[setup] WARNING: %s missing after a successful extraction; raw-ROM fallback\n",
                          archive.c_str());
            return;
        }
        // Hot-mount pattern (see port/gdx_workshop.cpp GdxWorkshopReload): AddArchive rebuilds the
        // virtual file system internally. No game threads run during setup, so no quiesce is needed.
        // The main.cpp C4 mount-time version gate does NOT re-run on this hot-mount, but it is
        // SUBSUMED: extraction only installs fzerox.o2r when its SHA-256 equals this build's golden
        // reference, which is strictly stronger than the gate's ROM-CRC version-entry check (a correct
        // golden archive necessarily carries the expected US-rev0 version stamp).
        if (am->AddArchive(archive) != nullptr) {
            mArchiveMounted = true;
            gdx_port_logf("[setup] hot-mounted %s\n", archive.c_str());
        } else {
            gdx_port_logf("[setup] WARNING: AddArchive(%s) failed; raw-ROM fallback\n", archive.c_str());
        }
    }

    // Per-frame state machine (logic only; rendering is in DrawUI).
    void Tick() {
        switch (mPhase) {
            case Phase::Acquire:
                // Detection and validation are automatic, but installation is not. The user must see
                // the reviewed paths and explicitly confirm them in DrawAcquire().
                break;
            case Phase::Extracting: {
                ExtractProgress p = GdxExtractPollStatus();
                mStage = p.stage;
                // Copy the ring-buffer snapshot + real-progress counters into GUI-owned state BEFORE any
                // GdxExtractResetAsync() below, which clears the launcher's own copy. On a failed Done
                // this is what lets ExtractFailed keep showing the log the user needs to read.
                mLog.assign(p.log.begin(), p.log.end());
                mEntriesSeen = p.entriesSeen;
                mSubStage = p.subStage;
                if (p.phase == ExtractPhase::Done) {
                    if (p.outcome == ExtractOutcome::FailedRawFallback) {
                        mError = p.lastError.empty() ? std::string(GdxExtractOutcomeString(p.outcome))
                                                     : p.lastError;
                        GdxExtractResetAsync();
                        mPhase = Phase::ExtractFailed;
                    } else {
                        GdxExtractResetAsync();
                        MountArchive();
                        mPhase = Phase::Done;
                    }
                }
                break;
            }
            case Phase::ExtractFailed:
            case Phase::Done:
                break;
        }
    }

    // ── Rendering ────────────────────────────────────────────────────────────────────────────────
    void DrawUI() {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        // Re-center every frame so a window resize/minimize keeps the panel centered.
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(640.0f, 0.0f), ImGuiCond_Always);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoDocking;
        if (!ImGui::Begin("G-Diffuser - First-Time Setup", nullptr, flags)) {
            ImGui::End();
            return;
        }

        ImFont* large = GdxGuiFontLarge();
        if (large != nullptr) {
            ImGui::PushFont(large);
        }
        ImGui::TextUnformatted("Welcome to G-Diffuser");
        if (large != nullptr) {
            ImGui::PopFont();
        }
        ImGui::TextWrapped("Review the three original files below. Files already next to the game are "
                           "detected automatically, but nothing is installed until you confirm. Nothing "
                           "is uploaded.");
        ImGui::Separator();

        switch (mPhase) {
            case Phase::Acquire:
            case Phase::Done:
                DrawAcquire();
                break;
            case Phase::Extracting:
                DrawExtracting();
                break;
            case Phase::ExtractFailed:
                DrawExtractFailed();
                break;
        }

        ImGui::End();
    }

    void DrawAcquire() {
        for (int i = 0; i < 3; ++i) {
            Row& r = mRows[i];
            ImGui::PushID(i);
            ImGui::SeparatorText(r.label);

            switch (r.status) {
                case RowStatus::Ok:
                    // okHeader overrides the file name in the header: the EK disk region label
                    // ("translated Expansion Kit disk"), or the archive-satisfied message when the
                    // original file has been deleted and its installed archive covers the requirement.
                    ImGui::TextColored(ImVec4(0.35f, 0.80f, 0.35f, 1.0f), "OK  (%s)",
                                       r.okHeader.empty() ? r.canonicalName : r.okHeader.c_str());
                    if (!r.warning.empty()) {
                        // Accepted but unrecognized dump: visible amber warning, row still passes.
                        ImGui::TextColored(ImVec4(0.90f, 0.70f, 0.30f, 1.0f), "%s", r.warning.c_str());
                    }
                    break;
                case RowStatus::Invalid:
                    ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.35f, 1.0f), "Invalid");
                    ImGui::TextWrapped("Reason: %s", r.reason.c_str());
                    break;
                case RowStatus::Missing:
                default:
                    ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.35f, 1.0f), "Missing");
                    break;
            }
            // Provenance: show the path Recheck actually resolved and validated (the managed media/
            // copy, or the installed archive, when the canonical original is gone) rather than always
            // the canonical dataDir path -- which previously mislabeled a managed/archive hit as the
            // deleted root file (Defect 1 provenance fix). Falls back to the canonical path pre-check.
            const std::string shownPath = r.resolvedPath.empty() ? RowPath(i) : r.resolvedPath;
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("File: %s", shownPath.c_str());
            ImGui::PopTextWrapPos();
            if (!r.detail.empty()) {
                ImGui::TextDisabled("%s", r.detail.c_str());
            }

            if (NativeFilePickerAvailable()) {
                if (ImGui::Button(r.status == RowStatus::Ok ? "Replace..." : "Browse...")) {
                    Browse(r);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("...or drag & drop the file onto this window");
            } else {
                ImGui::TextDisabled("Drag & drop the file onto this window");
            }
            ImGui::PopID();
        }

        ImGui::Separator();
        if (!mDropError.empty()) {
            ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.35f, 1.0f), "%s", mDropError.c_str());
        }
        if (AllRowsOk()) {
            ImGui::TextColored(ImVec4(0.35f, 0.80f, 0.35f, 1.0f), "All three files are verified.");
            ImGui::TextWrapped("Confirm to build fzerox.o2r from your ROM and continue to the game.");
            // The deletable-files statement. Named per-file so the reader never has to guess which
            // originals are safe to remove. The media/ managed copy is TRANSITIONAL (deletable once
            // the disk archive verifies — the green line in Data & Files), and saves live ONLY in
            // saves/*.gdd.
            // JP exception: an accepted Japanese ROM boots RAW -- no fzerox.o2r is
            // ever built for it, so the ROM file is the ONLY copy of the game data. The generic
            // "all deletable" paragraph told JP users to delete a file the app privately requires;
            // gate it and state the JP truth instead.
            if (mRows[0].jpRom) {
                ImGui::TextWrapped(
                    "Japanese ROM install (experimental): KEEP the ROM file -- it stays required "
                    "(no game-data archive is built for the Japanese version). The N64DD IPL ROM "
                    "and the Expansion Kit disk (.ndd) become deletable once their archives verify "
                    "(the green lines in Data & Files). Your saves live only in saves/*.gdd -- "
                    "back up that folder to preserve your progress.");
            } else {
                ImGui::TextWrapped(
                    "Once this finishes, none of your three original files are needed anymore: the "
                    "F-Zero X ROM (.z64), the N64DD IPL ROM, and the Expansion Kit disk (.ndd) are all "
                    "deletable. The disk is kept as a temporary managed copy in this folder's media/ "
                    "subfolder; after your next boot verifies the disk archive (the green line in "
                    "Data & Files), that copy is deletable too. Your saves live only in saves/*.gdd -- "
                    "back up that folder to preserve your progress.");
            }
            ImGui::Spacing();
            if (ImGui::Button("Build game data and continue", ImVec2(-1.0f, 0.0f))) {
                ConfirmAndStartExtraction();
            }
        } else {
            ImGui::TextDisabled("Waiting for all three files.");
        }
    }

    void DrawExtracting() {
        // Stage label (matches gdx_extract_launch.h's ExtractProgress::subStage contract: 0 = cart,
        // 1 = validating the cart archive, 2 = the independent IPL font-block archive).
        const char* stageLabel = "Extracting game assets...";
        if (mSubStage == 1) {
            stageLabel = "Validating extracted archive...";
        } else if (mSubStage == 2) {
            stageLabel = "Extracting IPL font data...";
        }
        ImGui::TextUnformatted(stageLabel);
        ImGui::TextWrapped("This happens once and usually takes only a few seconds.");
        ImGui::Spacing();

        // Real progress when a total is known (GDX_O2R_EXPECTED_ENTRY_COUNT for the cart stage, the
        // frozen 2-entry IPL archive for the ipl stage) -- counted from the extractor's own per-asset
        // "Processing" stdout lines (see gdx_extract_launch.cpp's looksLikeEntryProgressLine). The
        // validating sub-stage has no comparable per-entry signal (it is a few hash/zip checks after the
        // extractor child has already exited), so it always falls back to the indeterminate bar below.
        const int total = (mSubStage == 2) ? GdxExtractExpectedIplEntryCount() : GdxExtractExpectedCartEntryCount();
        if (mSubStage != 1 && total > 0) {
            int done = mEntriesSeen;
            if (done > total) {
                done = total; // clamp: the "Processing" heuristic can occasionally over-count
            }
            std::string overlay = std::to_string(done) + " / " + std::to_string(total);
            ImGui::ProgressBar(static_cast<float>(done) / static_cast<float>(total), ImVec2(-1.0f, 0.0f),
                               overlay.c_str());
        } else {
            // No reliable count for this sub-stage (placeholder golden header, or mid-validation) --
            // indeterminate animated bar (ImGui renders a moving indicator for a negative fraction).
            ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1.0f, 0.0f),
                               mStage.empty() ? "working..." : mStage.c_str());
        }

        ImGui::Spacing();
        DrawLogView();
    }

    // Bordered, auto-scrolling scrollback of the extractor's stdout (ring-buffer snapshot from
    // GdxExtractPollStatus). Shared by the live Extracting view and the ExtractFailed view so a failure
    // never loses the diagnostic context the single-line summary used to discard.
    void DrawLogView() {
        ImFont* mono = GdxGuiFontMono();
        if (mono != nullptr) {
            ImGui::PushFont(mono);
        }
        const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
        ImGui::BeginChild("ExtractLog", ImVec2(-1.0f, rowHeight * 12.0f), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (const std::string& l : mLog) {
            ImGui::TextUnformatted(l.c_str());
        }
        // Auto-scroll to the bottom only while already pinned there, so a user who scrolls up to read
        // earlier lines is not yanked back down on the next incoming line.
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        if (mono != nullptr) {
            ImGui::PopFont();
        }
    }

    void DrawExtractFailed() {
        ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.35f, 1.0f), "Asset extraction did not complete.");
        ImGui::TextWrapped("%s", mError.empty()
                                       ? "Verify that gdx-extract and decomp-recipes are next to the game."
                                       : mError.c_str());
        ImGui::TextWrapped("You can continue with the raw ROM (assets are read directly, which is slower "
                           "and less compatible), or retry the extraction.");
        ImGui::Spacing();
        // Keep the log visible on failure -- this is the actual diagnostic detail the single reduced
        // line used to lose; the user (or an issue report) needs it to see what went wrong.
        DrawLogView();
        ImGui::Separator();
        if (ImGui::Button("Continue anyway (raw assets)")) {
            // Boot with the raw-ROM fallback: no archive mounted, ROM path already installed.
            gdx_port_logf("[setup] user chose to continue with the raw-ROM fallback\n");
            mPhase = Phase::Done;
        }
        ImGui::SameLine();
        if (ImGui::Button("Retry")) {
            gdx_port_logf("[setup] user requested extraction retry\n");
            StartExtraction();
        }
    }

    // ── File-drop callback plumbing ────────────────────────────────────────────────────────────────
    // FileDroppedFunc is a plain C function pointer (no user data), so forward through a file-scope
    // pointer to the active screen. The callback fires on the main thread inside HandleEvents().
    static bool OnFileDroppedThunk(char* path);

    std::string mDataDir;
    std::string mExeDir;
    Row mRows[3] = {};
    Phase mPhase = Phase::Acquire;
    std::string mStage;      // latest extraction stage line
    std::string mError;      // extraction failure reason (surfaced in DrawExtractFailed)
    std::string mDropError;  // transient "unrecognized drop" message
    bool mArchiveMounted = false;
    std::vector<std::string> mLog; // ring-buffer snapshot of extractor stdout (GUI-owned copy -- see
                                    // Tick()'s comment on why this survives GdxExtractResetAsync())
    int mEntriesSeen = 0;          // real per-entry progress counter for the current sub-stage
    int mSubStage = 0;             // 0 = cart, 1 = validating cart, 2 = ipl (ExtractProgress::subStage)
};

SetupScreen* gActiveScreen = nullptr;

bool SetupScreen::OnFileDroppedThunk(char* path) {
    if (gActiveScreen != nullptr) {
        gActiveScreen->HandleDrop(path);
    }
    return true; // consume the event (prevents the "Unsupported file dropped" overlay)
}

} // namespace

bool GdxFirstBootSetupRun(const std::string& dataDir, const std::string& exeDir, std::string& outRomPath) {
    SetupScreen screen(dataDir, exeDir);
    gActiveScreen = &screen;
    bool completed = screen.Run(outRomPath);
    gActiveScreen = nullptr;
    return completed;
}

} // namespace gdx
