#include "rom_buffer.h"
#include "port_log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <cwchar>
#else
#include <strings.h> // strcasecmp
#include <unistd.h>   // readlink
#define _stricmp strcasecmp
#endif

extern "C" {
uint8_t* gdx_rom_buffer = nullptr;
size_t   gdx_rom_size   = 0;
char     gdx_rom_path[1024] = {};

/* Japanese-region gate (CMake option GDX_ALLOW_JP_INPUTS, default OFF — see port/CMakeLists.txt
   for the reasoning). The setup wizard refuses Japanese game data at the validation layer, but the
   wizard is not the only way a ROM reaches this loader: gdx_init_rom below also honours a command
   line argument, the Win32 picker, FZEROX_ROM, and a bare file sitting next to the executable, none
   of which pass through first-boot validation at all. This flag records that at least one candidate
   was turned away for being Japanese, purely so the fatal dialog at the end can say so. */
static bool s_refusedJapaneseRom = false;

static FILE* open_file_utf8(const char* path) {
    FILE* f = nullptr;
#ifdef _MSC_VER
    if (fopen_s(&f, path, "rb") != 0) {
        return nullptr;
    }
#else
    f = fopen(path, "rb");
#endif
    return f;
}

static void load_rom_from_file(FILE* f, const char* displayPath) {
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long szl = ftell(f);
    fseek(f, 0, SEEK_SET);
    /* A 0-byte (or unreadable) ROM previously "loaded"
       successfully (malloc(0) + fread of 0 bytes), half-booting to a blank
       screen past the null-buffer guard. Reject it here. */
    if (szl <= 0) {
        fclose(f);
        return;
    }
    /* Fixed-offset consumers (segment loads, setup_gfx at
       0x17B1E0, course_track_gfx, EK asset tables) assume a complete image and
       read high ROM offsets without per-read bounds checks. A truncated or
       partial download would boot past the null-buffer guard and OOB-read the
       heap on the first venue load. F-Zero X images are exactly 16 MiB; also
       require the big-endian z64 magic word (0x80371240) so a byte-swapped or
       foreign file is refused with a clear log line instead of garbage. */
    if (szl < (long)(16u * 1024u * 1024u)) {
        gdx_port_logf("[rom] REJECTED %s: %ld bytes (need a complete 16 MiB .z64 image)\n", displayPath, szl);
        fclose(f);
        return;
    }
    size_t sz = (size_t)szl;
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (!buf) { fclose(f); return; }
    if (fread(buf, 1, sz, f) != sz) { free(buf); fclose(f); return; }
    fclose(f);
    if (buf[0] != 0x80 || buf[1] != 0x37 || buf[2] != 0x12 || buf[3] != 0x40) {
        gdx_port_logf("[rom] REJECTED %s: not a big-endian .z64 image (magic %02X%02X%02X%02X)\n", displayPath,
                      buf[0], buf[1], buf[2], buf[3]);
        free(buf);
        return;
    }
#ifndef GDX_ALLOW_JP_INPUTS
    /* Japanese-region gate — the last line of defence, and the only one every ROM source shares.
       Offset 0x3E is the country code in the standard N64 header ('J' Japan, 'E' North America);
       the magic word checked directly above already guarantees the header is in native byte order,
       so the byte can be read at face value. This deliberately catches ANY Japanese dump, not just
       the one SHA-1 the wizard knows, and it costs a single byte comparison on a buffer that is
       already resident. See port/CMakeLists.txt's GDX_ALLOW_JP_INPUTS block for why the Japanese
       raw-ROM path is unsafe to ship (audio streamed from US offsets: static and blasting). */
    if (buf[0x3E] == 'J') {
        gdx_port_logf("[rom] REJECTED %s: this is the Japanese release of F-Zero X. Japanese-region "
                      "support is not enabled in this build (configure with "
                      "-DGDX_ALLOW_JP_INPUTS=ON to enable it).\n",
                      displayPath);
        s_refusedJapaneseRom = true;
        free(buf);
        return;
    }
#endif
    gdx_rom_buffer = buf;
    gdx_rom_size   = sz;
    strncpy(gdx_rom_path, displayPath, sizeof(gdx_rom_path) - 1);
    gdx_rom_path[sizeof(gdx_rom_path) - 1] = '\0';
    gdx_port_logf("[rom] loaded %zu bytes from %s\n", sz, displayPath);
}

static void load_rom(const char* path) {
    load_rom_from_file(open_file_utf8(path), path);
}

#ifdef _WIN32
static FILE* open_file_wide(const wchar_t* path) {
    FILE* f = nullptr;
#ifdef _MSC_VER
    if (_wfopen_s(&f, path, L"rb") != 0) {
        return nullptr;
    }
#else
    f = _wfopen(path, L"rb");
#endif
    return f;
}

static void load_rom_w(const wchar_t* path) {
    FILE* f = open_file_wide(path);
    if (!f) return;

    char displayPath[MAX_PATH * 4] = {};
    WideCharToMultiByte(CP_UTF8, 0, path, -1, displayPath, sizeof(displayPath), nullptr, nullptr);
    load_rom_from_file(f, displayPath[0] ? displayPath : "<selected ROM>");
}

static void pick_rom_with_dialog(void) {
    wchar_t fileName[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter =
        L"Nintendo 64 ROMs (*.z64;*.n64;*.v64)\0*.z64;*.n64;*.v64\0"
        L"All files (*.*)\0*.*\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Select F-Zero X ROM";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
        load_rom_w(fileName);
    } else {
        gdx_port_logf("[rom] ROM picker cancelled or failed (error=%lu)\n", CommDlgExtendedError());
    }
}
#endif

static void load_rom_next_to_exe(void) {
#ifdef _WIN32
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        return;
    }

    wchar_t* slash = std::wcsrchr(exePath, L'\\');
    if (slash == nullptr) {
        return;
    }

    slash[1] = L'\0';
    const wchar_t* candidates[] = {
        L"baserom.us.rev0.z64",
        L"fzerox.z64",
        L"f-zero-x.z64",
    };

    for (const wchar_t* candidate : candidates) {
        wchar_t path[MAX_PATH] = {};
        wcscpy_s(path, exePath);
        wcscat_s(path, candidate);
        load_rom_w(path);
        if (gdx_rom_buffer) {
            return;
        }
    }
#else
    // POSIX: resolve the executable directory via /proc/self/exe and probe the same candidate
    // ROM names beside it (the native file picker is Windows-only; the CLI arg and FZEROX_ROM
    // env fallbacks in gdx_init_rom are already portable).
    char exePath[4096];
    ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (n <= 0) {
        return;
    }
    exePath[n] = '\0';
    char* slash = strrchr(exePath, '/');
    if (slash == nullptr) {
        return;
    }
    slash[1] = '\0'; // keep trailing separator

    const char* candidates[] = {
        "baserom.us.rev0.z64",
        "fzerox.z64",
        "f-zero-x.z64",
    };
    for (const char* candidate : candidates) {
        char path[4096 + 32];
        snprintf(path, sizeof(path), "%s%s", exePath, candidate);
        load_rom(path);
        if (gdx_rom_buffer) {
            return;
        }
    }
#endif
}

void gdx_init_rom(int argc, char** argv, int archivesValidated) {
    // 1. Explicit command-line ROM path wins for scripted/dev launches.
    for (int i = 1; i < argc; i++) {
        size_t len = strlen(argv[i]);
        if (len > 4) {
            const char* ext = argv[i] + len - 4;
            if (_stricmp(ext, ".z64") == 0 || _stricmp(ext, ".n64") == 0 || _stricmp(ext, ".v64") == 0) {
                load_rom(argv[i]);
                if (gdx_rom_buffer) return;
            }
        }
    }

    // 2. For interactive/direct launches, ask first. This intentionally runs before
    // env/exe-dir fallbacks so a stale FZEROX_ROM or nearby baserom cannot silently
    // force the old ROM during renderer testing. With validated archives the picker
    // is skipped entirely: archive-only is a supported install state (the originals
    // are deletable), so a missing ROM must not cost the user a dialog every boot.
    bool pickerCancelled = false;
#ifdef _WIN32
    if (!archivesValidated) {
        gdx_port_logf("[rom] no ROM argument provided; opening picker.\n");
        pick_rom_with_dialog();
        if (gdx_rom_buffer) return;
        pickerCancelled = true;
    } else {
        gdx_port_logf("[rom] no ROM argument provided; picker skipped (validated archives "
                       "present, archive-only boot is available).\n");
    }
#endif

    // 3. Environment variable fallback for non-interactive/dev runs.
#ifdef _WIN32
    char envBuf[MAX_PATH * 4] = {};
    size_t envLen = 0;
    if (getenv_s(&envLen, envBuf, sizeof(envBuf), "FZEROX_ROM") != 0) {
        envLen = 0;
    }
    const char* env = (envLen > 1) ? envBuf : nullptr;
#else
    const char* env = getenv("FZEROX_ROM");
#endif
    if (env && env[0]) {
        load_rom(env);
        if (gdx_rom_buffer) {
            if (pickerCancelled) {
                gdx_port_logf("[rom] picker cancelled; falling back to %s (FZEROX_ROM)\n", gdx_rom_path);
            }
            return;
        }
    }

    // 4. Last convenience fallback for unattended builds from the output folder.
    load_rom_next_to_exe();
    if (gdx_rom_buffer) {
        if (pickerCancelled) {
            gdx_port_logf("[rom] picker cancelled; falling back to %s\n", gdx_rom_path);
        }
        return;
    }

    /* archivesValidated (fzerox.o2r mounted AND survived the post-mount CRC gate) means every
       family that today streams raw+MIO0/big-endian bytes from gdx_rom_buffer has an archive-first
       path (the segment_blob, audio_blob and ipl namespaces) whose shim already tolerates
       gdx_rom_buffer==NULL and returns 0 on a total miss. So a missing ROM is not an automatic
       half-boot -- it degrades to "assets served from fzerox.o2r, raw-ROM fallback disabled by
       absence" instead of undefined blank reads. GDX_STRICT_ARCHIVE (gdx_segment_source.c) is the
       telemetry that must show zero archive-miss reads before relying on this: a NULL ROM turns
       any archive miss into a silently-blank asset. */
    if (archivesValidated) {
        gdx_port_logf("[rom] no ROM image found; booting archive-only (assets served from "
                       "fzerox.o2r; raw-ROM fallback disabled by absence)\n");
        gdx_rom_buffer = nullptr;
        gdx_rom_size = 0;
        return;
    }

    /* Without a ROM AND without a validated archive the game "runs" but every DMA read is
       blank — a confusing half-boot. Require one or the other rather than continuing silently. */
    if (s_refusedJapaneseRom) {
        /* Every candidate we saw was Japanese. Say that plainly instead of the generic
           "no ROM found", which would read as "your file is missing/corrupt" when it is neither. */
        gdx_port_logf("[rom] FATAL: the only ROM(s) found are the Japanese release of F-Zero X, and "
                       "Japanese-region support is not enabled in this build. Supply the North "
                       "American (US rev0) cartridge dump.\n");
#ifdef _WIN32
        MessageBoxW(nullptr,
                    L"This build of G-Diffuser does not support the Japanese release of F-Zero X.\n\n"
                    L"Japanese-region support is planned for a later release. To play now, supply "
                    L"the North American (US rev0) F-Zero X ROM.",
                    L"G-Diffuser - Japanese ROM not supported", MB_OK | MB_ICONINFORMATION);
#endif
        exit(1);
    }
    if (pickerCancelled) {
        gdx_port_logf("[rom] FATAL: picker cancelled and no fallback ROM found "
                       "(FZEROX_ROM unset/invalid, no baserom next to the exe), and no completed "
                       "archive setup was found either. Exiting.\n");
    } else {
        gdx_port_logf("[rom] FATAL: no ROM file found and no completed archive setup was found. "
                       "Set FZEROX_ROM env var, pass ROM path as argument, select it in the "
                       "picker, or complete first-boot setup to install fzerox.o2r.\n");
    }
#ifdef _WIN32
    MessageBoxW(nullptr,
                L"G-Diffuser needs an F-Zero X (U) ROM to run.\n\n"
                L"Select the ROM in the file picker, pass its path on the command line, "
                L"set the FZEROX_ROM environment variable, or complete first-boot setup so a "
                L"validated fzerox.o2r archive is installed (which also allows booting without "
                L"the ROM present).",
                L"G-Diffuser - ROM required", MB_OK | MB_ICONERROR);
#endif
    exit(1);
}
} // extern "C"
