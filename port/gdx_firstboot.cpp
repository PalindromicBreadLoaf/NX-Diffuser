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

// Per-user data directory (installed mode). Empty on failure (caller falls back to exe dir).
fs::path perUserDataDir() {
#ifdef _WIN32
    // %APPDATA% = …\AppData\Roaming. Read the wide env var (unicode-safe usernames) — same env-var
    // approach rom_buffer.cpp uses for FZEROX_ROM, so no extra shell32 link dependency is introduced.
    wchar_t appdata[MAX_PATH] = {};
    size_t len = 0;
    if (_wgetenv_s(&len, appdata, MAX_PATH, L"APPDATA") == 0 && len > 1) {
        return fs::path(appdata) / L"G-Diffuser";
    }
    return {};
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
        return fs::path(xdg) / "G-Diffuser";
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        return fs::path(home) / ".local" / "share" / "G-Diffuser";
    }
    return {};
#endif
}

bool fileExists(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
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

// Yes/No prompt. Returns true for Yes.
bool askYesNo(const wchar_t* title, const wchar_t* text) {
    return MessageBoxW(nullptr, text, title, MB_YESNO | MB_ICONQUESTION) == IDYES;
}

void showError(const wchar_t* title, const wchar_t* text) {
    MessageBoxW(nullptr, text, title, MB_OK | MB_ICONERROR);
}
#endif

// Acquire one required/optional input via picker + validate + copy. Returns true if the file now
// exists (valid) in the data dir. `required` controls messaging only; the caller decides on abort.
using Validator = bool (*)(const fs::path&, std::string&);

bool acquireInput(const fs::path& dataDir, const char* dstName, Validator validate, bool required,
                  const char* humanName,
#ifdef _WIN32
                  const wchar_t* pickerTitle, const wchar_t* pickerFilter
#else
                  const wchar_t* /*pickerTitle*/, const wchar_t* /*pickerFilter*/
#endif
) {
    fs::path dst = dataDir / dstName;

    // Already present + valid? Keep it.
    if (fileExists(dst)) {
        std::string why;
        if (validate(dst, why)) {
            gdx_port_logf("[firstboot] %s already present: %s\n", humanName, dst.string().c_str());
            return true;
        }
        gdx_port_logf("[firstboot] existing %s rejected (%s); re-acquiring\n", humanName, why.c_str());
    }

#ifdef _WIN32
    for (;;) {
        gdx_port_logf("[firstboot] prompting for %s\n", humanName);
        fs::path picked = pickFile(pickerTitle, pickerFilter);
        if (picked.empty()) {
            gdx_port_logf("[firstboot] %s selection cancelled\n", humanName);
            return false; // caller decides whether this is fatal
        }
        std::string why;
        if (!validate(picked, why)) {
            std::wstring msg = L"That file is not a valid ";
            msg += std::wstring(humanName, humanName + std::string(humanName).size());
            msg += L".\n\nReason: ";
            msg += std::wstring(why.begin(), why.end());
            msg += L"\n\nTry again?";
            if (!askYesNo(L"G-Diffuser — invalid file", msg.c_str())) {
                return false;
            }
            continue;
        }
        if (!copyInto(picked, dataDir, dstName)) {
            showError(L"G-Diffuser — copy failed",
                      L"Could not copy the selected file into the data directory.");
            return false;
        }
        gdx_port_logf("[firstboot] %s installed: %s\n", humanName, dst.string().c_str());
        return true;
    }
#else
    // No native picker on this platform in this slice. Degrade gracefully: instruct the user to drop
    // the file into the data directory and re-launch. (A cross-platform picker — e.g. tinyfiledialogs —
    // is a documented future upgrade; see docs/FIRST_BOOT_DESIGN.md.)
    gdx_port_logf("[firstboot] %s not found. Place '%s' in %s and relaunch%s.\n", humanName, dstName,
                  dataDir.string().c_str(), required ? " (required)" : " (optional)");
    return false;
#endif
}

} // namespace

FirstBootResult FirstBootRun(const char* argv0) {
    FirstBootResult result;
    std::error_code ec;

    const fs::path exeDir = executableDir(argv0);
    const fs::path cwd = fs::current_path(ec);
    if (ec) {
        ec.clear();
    }

    // ── Portable / dev detection ─────────────────────────────────────────────────────────────────
    // A `portable.txt` beside the exe forces portable mode (data dir = exe dir). Independently, if a
    // ROM already sits next to the exe (or in the CWD), this is the dev layout: boot exactly as before
    // with no wizard and no working-directory change. We still resolve the ROM path so main() can
    // inject it as an argv entry — that suppresses rom_buffer.cpp's own picker, which would otherwise
    // block a headless launch.
    const bool portableMarker = fileExists(exeDir / "portable.txt");

    for (const char* cand : kRomDevCandidates) {
        for (const fs::path& dir : { exeDir, cwd }) {
            fs::path p = dir / cand;
            if (fileExists(p)) {
                result.status = FirstBootStatus::DevLayout;
                result.romPath = fs::absolute(p, ec).string();
                result.dataDir = exeDir.string();
                gdx_port_logf("[firstboot] dev/portable layout: ROM found next to exe (%s); no wizard\n",
                              result.romPath.c_str());
                return result;
            }
        }
    }

    // ── Installed mode ───────────────────────────────────────────────────────────────────────────
    fs::path dataDir = portableMarker ? exeDir : perUserDataDir();
    if (dataDir.empty()) {
        dataDir = exeDir; // last-resort fallback
    }
    fs::create_directories(dataDir, ec);
    ec.clear();
    result.dataDir = dataDir.string();

    // Move the working directory into the data dir so config, logs, the disk image, and the IPL ROM
    // (all of which resolve relative to the CWD in libultraship / disk_buffer.cpp) consolidate there.
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

    // Fast path: previously completed AND the required inputs are still present + valid.
    fs::path romInData = dataDir / kRomName;
    fs::path diskInData = dataDir / kDiskName;
    if (st.complete) {
        std::string why;
        if (fileExists(romInData) && validateRom(romInData, why)) {
            result.status = FirstBootStatus::SetupComplete;
            result.romPath = fs::absolute(romInData, ec).string();
            gdx_port_logf("[firstboot] setup complete; booting with configured ROM %s\n",
                          result.romPath.c_str());
            return result;
        }
        gdx_port_logf("[firstboot] setup was marked complete but the ROM is missing/invalid; re-running setup\n");
        st.complete = false;
    }

    // ── Wizard ───────────────────────────────────────────────────────────────────────────────────
    gdx_port_logf("[firstboot] running first-time setup wizard in %s\n", dataDir.string().c_str());

    const bool romOk = acquireInput(dataDir, kRomName, &validateRom, /*required=*/true,
                                    "F-Zero X ROM (US rev0, .z64)",
                                    L"Select your F-Zero X ROM (US rev0)",
                                    L"Nintendo 64 ROMs (*.z64;*.n64;*.v64)\0*.z64;*.n64;*.v64\0All files\0*.*\0");
    if (!romOk) {
#ifdef _WIN32
        showError(L"G-Diffuser — ROM required",
                  L"G-Diffuser needs an F-Zero X ROM to run.\n\n"
                  L"Setup was cancelled, so the program will now exit. Relaunch to try again.");
#endif
        gdx_port_logf("[firstboot] ROM not acquired; aborting setup\n");
        result.status = FirstBootStatus::Aborted;
        return result;
    }

    // Expansion Kit disk + IPL ROM are strongly encouraged but not strictly required to boot base
    // F-Zero X. Offer them; skipping just leaves EK modes dark until the files are provided later.
    acquireInput(dataDir, kDiskName, &validateDisk, /*required=*/false,
                 "Expansion Kit disk (.ndd)",
                 L"Select your F-Zero X Expansion Kit disk (.ndd)",
                 L"64DD disk images (*.ndd)\0*.ndd\0All files\0*.*\0");
    acquireInput(dataDir, kIplName, &validateIpl, /*required=*/false,
                 "64DD IPL ROM (N64DDIPLROM.n64)",
                 L"Select your 64DD IPL ROM (N64DDIPLROM.n64)",
                 L"64DD IPL ROM (*.n64;*.z64)\0*.n64;*.z64\0All files\0*.*\0");

    // Persist.
    st.complete = true;
    st.romPath = fs::absolute(romInData, ec).string();
    st.diskPath = fileExists(diskInData) ? fs::absolute(diskInData, ec).string() : std::string();
    fs::path iplInData = dataDir / kIplName;
    st.iplPath = fileExists(iplInData) ? fs::absolute(iplInData, ec).string() : std::string();
    saveState(dataDir, st);

    result.status = FirstBootStatus::SetupComplete;
    result.romPath = st.romPath;
    gdx_port_logf("[firstboot] setup complete; booting with ROM %s\n", result.romPath.c_str());
    return result;
}

} // namespace gdx
