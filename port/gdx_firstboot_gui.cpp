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
            outRomPath = RowPath(0);
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

    void Recheck(Row& r) {
        const std::string dst = (fs::path(mDataDir) / r.canonicalName).string();
        std::error_code ec;
        r.detail.clear();
        if (!fs::is_regular_file(fs::path(dst), ec)) {
            r.status = RowStatus::Missing;
            r.reason.clear();
            return;
        }
        std::string why;
        if (r.validate(dst, why)) {
            r.status = RowStatus::Ok;
            r.reason.clear();
        } else {
            r.status = RowStatus::Invalid;
            r.reason = why;
            return;
        }
        // Show the cryptographic identity of every reviewed input. Hashing happens only when a row
        // is (re)checked, never per frame; even the 64.9 MB disk is therefore negligible in normal
        // setup use. The cartridge additionally has a recipe-backed expected SHA-1 and is rejected
        // here instead of failing later during extraction.
        const std::string got = GdxExtractFileSha1(dst.c_str());
        if (got.empty()) {
            r.status = RowStatus::Invalid;
            r.reason = "could not read the file to calculate its SHA-1";
            return;
        }
        r.detail = "SHA-1: " + got;

        if (std::string(r.canonicalName) == SetupRomFileName()) {
            const std::string expected = GdxExtractExpectedRomSha1(mExeDir.c_str());
            if (got != expected) {
                r.status = RowStatus::Invalid;
                r.reason = "SHA-1 mismatch — this dump is not the US rev0 cartridge";
                r.detail = "yours:    " + got + "\nexpected: " + expected;
            } else {
                r.detail = "SHA-1 verified: " + got;
            }
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
        GdxExtractStartAsync(mDataDir.c_str(), RowPath(0).c_str(), mExeDir.c_str(), /*suppress=*/true);
        mStage.clear();
        mError.clear();
        mPhase = Phase::Extracting;
    }

    void ConfirmAndStartExtraction() {
        if (!AllRowsOk()) {
            return;
        }
        if (!WriteSetupComplete(mDataDir, RowPath(0), RowPath(1), RowPath(2))) {
            mDropError = "Could not save the setup state next to the game.";
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
        if (!ImGui::Begin("G-Diffuser — First-Time Setup", nullptr, flags)) {
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
                    ImGui::TextColored(ImVec4(0.35f, 0.80f, 0.35f, 1.0f), "OK  (%s)", r.canonicalName);
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
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("File: %s", RowPath(i).c_str());
            ImGui::PopTextWrapPos();
            if (!r.detail.empty()) {
                ImGui::TextDisabled("%s", r.detail.c_str());
            }

            if (NativeFilePickerAvailable()) {
                if (ImGui::Button(r.status == RowStatus::Ok ? "Replace…" : "Browse…")) {
                    Browse(r);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("…or drag & drop the file onto this window");
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
            ImGui::Spacing();
            if (ImGui::Button("Build game data and continue", ImVec2(-1.0f, 0.0f))) {
                ConfirmAndStartExtraction();
            }
        } else {
            ImGui::TextDisabled("Waiting for all three files.");
        }
    }

    void DrawExtracting() {
        ImGui::TextUnformatted("Building the asset archive from your ROM…");
        ImGui::TextWrapped("This happens once and usually takes only a few seconds.");
        ImGui::Spacing();
        // Indeterminate animated bar (ImGui renders a moving indicator for a negative fraction).
        ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1.0f, 0.0f),
                           mStage.empty() ? "working…" : mStage.c_str());
    }

    void DrawExtractFailed() {
        ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.35f, 1.0f), "Asset extraction did not complete.");
        ImGui::TextWrapped("%s", mError.empty()
                                       ? "Verify that gdx-extract and decomp-recipes are next to the game."
                                       : mError.c_str());
        ImGui::TextWrapped("You can continue with the raw ROM (assets are read directly, which is slower "
                           "and less compatible), or retry the extraction.");
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
