// G-Diffuser — first-boot setup + per-user data directory resolution. See gdx_firstboot.h and
// docs/FIRST_BOOT_DESIGN.md.
//
// This TU is part of the G-Diffuser exe target (not the decomp game library), so it may freely use
// the host CRT, <filesystem>, and the Win32 common-dialog picker (Comdlg32 is already linked for
// rom_buffer.cpp's picker). It runs before libultraship is constructed, so it logs through the port's
// own gdx_port_logf and touches no LUS state.

#include "gdx_firstboot.h"
#include "port_log.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <cwchar>
#else
#include <cstdlib>    // getenv
#include <unistd.h>   // readlink
#endif

namespace fs = std::filesystem;

namespace gdx {
namespace {

// ── Structural validation constants (see docs/FIRST_BOOT_DESIGN.md §3.1) ─────────────────────────
constexpr std::uintmax_t kRomMinBytes = 16u * 1024u * 1024u;   // F-Zero X images are 16 MiB.
constexpr std::uintmax_t kDiskExactBytes = 64931840u;          // Retail/translated 64DD image size.
constexpr std::uintmax_t kIplMinBytes = 4u * 1024u * 1024u;    // 64DD IPL dumps are 4 MiB.

// Canonical on-disk names inside the data directory. These match what the existing loaders search
// for (rom_buffer.cpp / disk_buffer.cpp), so copying a user's pick to these names lets the stock
// resolvers find it with no further wiring.
constexpr const char* kRomName = "baserom.us.rev0.z64";
constexpr const char* kDiskName = "baserom.translated.ek.ndd";
constexpr const char* kIplName = "N64DDIPLROM.n64";
constexpr const char* kGameArchiveName = "fzerox.o2r";

// ROM candidate names probed for DEV-layout detection (mirrors rom_buffer.cpp's next-to-exe list).
const char* const kRomDevCandidates[] = { "baserom.us.rev0.z64", "fzerox.z64", "f-zero-x.z64" };

// ── Path helpers ─────────────────────────────────────────────────────────────────────────────────

fs::path executableDir(const char* argv0) {
    std::error_code ec;
#ifdef _WIN32
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        fs::path p(buf);
        return p.parent_path();
    }
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        fs::path p(buf);
        return p.parent_path();
    }
#endif
    if (argv0 != nullptr && argv0[0] != '\0') {
        fs::path p = fs::absolute(fs::path(argv0), ec);
        if (!ec) {
            return p.parent_path();
        }
    }
    return fs::current_path(ec);
}

// NOTE: G-Diffuser is always portable — no per-user data directory exists. The game folder is the
// data directory on every platform (product decision, 2026-07-18); AppData/XDG are never touched.

bool fileExists(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
}

bool developmentTreeProvidesArchive(const fs::path& exeDir, const fs::path& cwd) {
    for (const fs::path& base : { exeDir, cwd }) {
        fs::path probe = base;
        for (int up = 0; up < 6 && !probe.empty(); ++up, probe = probe.parent_path()) {
            if (fileExists(probe / "assets" / "extracted" / "generic.o2r")) {
                return true;
            }
            if (probe == probe.root_path()) {
                break;
            }
        }
    }
    return false;
}

std::uintmax_t fileSize(const fs::path& p) {
    std::error_code ec;
    std::uintmax_t s = fs::file_size(p, ec);
    return ec ? 0u : s;
}

// True if the file starts with the big-endian z64 magic (80 37 12 40).
bool hasZ64Magic(const fs::path& p) {
    FILE* f = nullptr;
#ifdef _MSC_VER
    if (fopen_s(&f, p.string().c_str(), "rb") != 0) {
        return false;
    }
#else
    f = fopen(p.string().c_str(), "rb");
#endif
    if (f == nullptr) {
        return false;
    }
    unsigned char m[4] = {};
    size_t got = fread(m, 1, 4, f);
    fclose(f);
    return got == 4 && m[0] == 0x80 && m[1] == 0x37 && m[2] == 0x12 && m[3] == 0x40;
}

bool validateRom(const fs::path& p, std::string& why) {
    if (fileSize(p) < kRomMinBytes) {
        why = "not a complete 16 MiB image";
        return false;
    }
    if (!hasZ64Magic(p)) {
        why = "missing big-endian .z64 magic (80 37 12 40) — is this a byte-swapped .n64/.v64?";
        return false;
    }
    return true;
}

bool validateDisk(const fs::path& p, std::string& why) {
    std::uintmax_t s = fileSize(p);
    if (s != kDiskExactBytes) {
        why = "wrong size for a 64DD disk image (expected exactly 64,931,840 bytes)";
        return false;
    }
    return true;
}

bool validateIpl(const fs::path& p, std::string& why) {
    if (fileSize(p) < kIplMinBytes) {
        why = "too small for a 64DD IPL ROM (expected >= 4 MiB)";
        return false;
    }
    return true;
}

// Copy src -> dstDir/dstName, overwriting. Returns true on success.
bool copyInto(const fs::path& src, const fs::path& dstDir, const char* dstName) {
    std::error_code ec;
    fs::path dst = dstDir / dstName;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        gdx_port_logf("[firstboot] ERROR copying %s -> %s: %s\n", src.string().c_str(),
                      dst.string().c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

// ── Simple key=value state file (independent of libultraship Config load order) ──────────────────

fs::path stateFilePath(const fs::path& dataDir) {
    return dataDir / "gdx_firstboot.cfg";
}

struct SetupState {
    bool complete = false;
    std::string romPath;
    std::string diskPath;
    std::string iplPath;
};

SetupState loadState(const fs::path& dataDir) {
    SetupState st;
    FILE* f = nullptr;
    std::string path = stateFilePath(dataDir).string();
#ifdef _MSC_VER
    if (fopen_s(&f, path.c_str(), "rb") != 0) {
        f = nullptr;
    }
#else
    f = fopen(path.c_str(), "rb");
#endif
    if (f == nullptr) {
        return st;
    }
    char line[4096];
    while (fgets(line, sizeof(line), f) != nullptr) {
        std::string s(line);
        // Strip trailing newline/CR.
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
            s.pop_back();
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = s.substr(0, eq);
        std::string val = s.substr(eq + 1);
        if (key == "Setup.Complete") {
            st.complete = (val == "1");
        } else if (key == "Game.RomPath") {
            st.romPath = val;
        } else if (key == "Game.DiskPath") {
            st.diskPath = val;
        } else if (key == "Game.DdIplPath") {
            st.iplPath = val;
        }
    }
    fclose(f);
    return st;
}

bool saveState(const fs::path& dataDir, const SetupState& st) {
    FILE* f = nullptr;
    std::string path = stateFilePath(dataDir).string();
#ifdef _MSC_VER
    if (fopen_s(&f, path.c_str(), "wb") != 0) {
        f = nullptr;
    }
#else
    f = fopen(path.c_str(), "wb");
#endif
    if (f == nullptr) {
        gdx_port_logf("[firstboot] WARNING: could not write %s; setup will re-run next launch\n",
                      path.c_str());
        return false;
    }
    fprintf(f, "# G-Diffuser first-boot state. Auto-generated; safe to delete to re-run setup.\n");
    fprintf(f, "Setup.Complete=%d\n", st.complete ? 1 : 0);
    fprintf(f, "Game.RomPath=%s\n", st.romPath.c_str());
    fprintf(f, "Game.DiskPath=%s\n", st.diskPath.c_str());
    fprintf(f, "Game.DdIplPath=%s\n", st.iplPath.c_str());
    fclose(f);
    return true;
}

// ── Native pickers / dialogs (Win32; graceful no-op elsewhere) ───────────────────────────────────

#ifdef _WIN32
// Opens a file picker. Returns the selected path, or empty if cancelled.
fs::path pickFile(const wchar_t* title, const wchar_t* filter) {
    wchar_t fileName[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        return fs::path(fileName);
    }
    return {};
}
#endif

} // namespace

FirstBootResult FirstBootRun(const char* argv0) {
    FirstBootResult result;
    std::error_code ec;

    const fs::path exeDir = executableDir(argv0);
    const fs::path cwd = fs::current_path(ec);
    if (ec) {
        ec.clear();
    }
    // Record the executable directory on every return path (dev, warm, wizard, abort). The runtime
    // O2R extractor reads its packaged gdx-extract child + decomp-recipes from here.
    result.exeDir = exeDir.string();

    // A ROM beside the executable is normal for a portable release and must not bypass first-time
    // setup. Preserve the headless shortcut only for a real source tree that already provides the
    // development generic.o2r archive.
    if (developmentTreeProvidesArchive(exeDir, cwd)) {
        for (const char* cand : kRomDevCandidates) {
            for (const fs::path& dir : { exeDir, cwd }) {
                fs::path p = dir / cand;
                if (fileExists(p)) {
                    result.status = FirstBootStatus::DevLayout;
                    result.romPath = fs::absolute(p, ec).string();
                    result.dataDir = exeDir.string();
                    gdx_port_logf("[firstboot] development tree: ROM=%s; setup not required\n",
                                  result.romPath.c_str());
                    return result;
                }
            }
        }
    }

    // ── Wizard mode: always portable ─────────────────────────────────────────────────────────────
    // G-Diffuser never writes to a per-user directory (AppData / XDG data): the game folder is the
    // data directory, period. Everything the port creates — the extracted fzerox.o2r, saves/,
    // ghosts/, config, and explicitly requested diagnostics — lives beside the executable, so the
    // whole installation is one folder that can be moved, backed up, or deleted as a unit.
    fs::path dataDir = exeDir;
    fs::create_directories(dataDir, ec);
    ec.clear();
    result.dataDir = dataDir.string();

    // Move the working directory into the data dir so config, the disk image, and the IPL ROM
    // (which resolve relative to the CWD in libultraship / disk_buffer.cpp) consolidate there.
    if (fs::current_path(dataDir, ec); !ec) {
        result.chdirApplied = true;
        gdx_port_logf("[firstboot] data directory: %s (working directory set)\n",
                      dataDir.string().c_str());
    } else {
        ec.clear();
        gdx_port_logf("[firstboot] WARNING: could not set working directory to %s\n",
                      dataDir.string().c_str());
    }

    SetupState st = loadState(dataDir);

    // Fast path: previously completed AND the required inputs and game archive are still present.
    // Requiring fzerox.o2r keeps an interrupted or failed extraction inside the setup flow on the
    // next launch instead of silently converting the completion marker into permanent raw fallback.
    fs::path romInData = dataDir / kRomName;
    fs::path diskInData = dataDir / kDiskName;
    fs::path archiveInData = dataDir / kGameArchiveName;
    if (st.complete) {
        std::string why;
        fs::path iplCheck = dataDir / kIplName;
        if (fileExists(romInData) && validateRom(romInData, why) && fileExists(diskInData) &&
            fileExists(iplCheck) && fileExists(archiveInData)) {
            result.status = FirstBootStatus::SetupComplete;
            result.romPath = fs::absolute(romInData, ec).string();
            gdx_port_logf("[firstboot] setup complete; booting with configured ROM %s\n",
                          result.romPath.c_str());
            return result;
        }
        gdx_port_logf(
            "[firstboot] setup was marked complete but a required input or fzerox.o2r is missing; re-running setup\n");
        st.complete = false;
    }

    // ── Needs setup ────────────────────────────────────────────────────────────────────────────────
    // The dev fast-path and the completed fast-path both missed: the required inputs are absent or
    // invalid. The old blocking Win32-dialog wizard is gone — acquisition now happens IN-WINDOW after
    // libultraship + the Gui + the FileDropMgr exist. Resolve nothing further here; return NeedsSetup
    // so main() proceeds through Context/window init without a ROM, then runs the ImGui setup flow
    // (port/gdx_firstboot_gui.{h,cpp}), which reuses the exported validators/copy/state helpers below.
    // result.romPath stays empty (the GUI fills the caller's ROM path once the user installs one).
    result.status = FirstBootStatus::NeedsSetup;
    gdx_port_logf("[firstboot] required inputs missing; deferring to the in-window setup flow (%s)\n",
                  dataDir.string().c_str());
    return result;
}

bool DevelopmentTreeProvidesArchive(const std::string& exeDir, const std::string& cwd) {
    return developmentTreeProvidesArchive(fs::path(exeDir), fs::path(cwd));
}

// ── Exported setup helpers (shared with the in-window GUI setup flow) ─────────────────────────────

const char* SetupRomFileName() {
    return kRomName;
}

const char* SetupDiskFileName() {
    return kDiskName;
}

const char* SetupIplFileName() {
    return kIplName;
}

bool ValidateRomFile(const std::string& path, std::string& why) {
    return validateRom(fs::path(path), why);
}

bool ValidateDiskFile(const std::string& path, std::string& why) {
    return validateDisk(fs::path(path), why);
}

bool ValidateIplFile(const std::string& path, std::string& why) {
    return validateIpl(fs::path(path), why);
}

bool CopyInputInto(const std::string& srcPath, const std::string& dataDir, const char* dstName) {
    return copyInto(fs::path(srcPath), fs::path(dataDir), dstName);
}

bool WriteSetupComplete(const std::string& dataDir, const std::string& romPath,
                        const std::string& diskPath, const std::string& iplPath) {
    SetupState st;
    st.complete = true;
    st.romPath = romPath;
    st.diskPath = diskPath;
    st.iplPath = iplPath;
    return saveState(fs::path(dataDir), st);
}

bool NativeFilePickerAvailable() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

#ifdef _WIN32
std::string PickRomFile() {
    fs::path p = pickFile(L"Select your F-Zero X ROM (US rev0, .z64)",
                          L"Nintendo 64 ROMs (*.z64;*.n64;*.v64)\0*.z64;*.n64;*.v64\0All files\0*.*\0");
    return p.string();
}

std::string PickDiskFile() {
    fs::path p = pickFile(L"Select your F-Zero X Expansion Kit disk (.ndd)",
                          L"64DD disk images (*.ndd)\0*.ndd\0All files\0*.*\0");
    return p.string();
}

std::string PickIplFile() {
    fs::path p = pickFile(L"Select your 64DD IPL ROM (N64DDIPLROM.n64)",
                          L"64DD IPL ROM (*.n64;*.z64)\0*.n64;*.z64\0All files\0*.*\0");
    return p.string();
}
#else
std::string PickRomFile() {
    return {};
}
std::string PickDiskFile() {
    return {};
}
std::string PickIplFile() {
    return {};
}
#endif

} // namespace gdx
