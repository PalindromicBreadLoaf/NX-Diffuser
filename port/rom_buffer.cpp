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
#endif

extern "C" {
uint8_t* gdx_rom_buffer = nullptr;
size_t   gdx_rom_size   = 0;

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
    size_t sz = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (!buf) { fclose(f); return; }
    if (fread(buf, 1, sz, f) != sz) { free(buf); fclose(f); return; }
    fclose(f);
    gdx_rom_buffer = buf;
    gdx_rom_size   = sz;
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
#endif
}

void gdx_init_rom(int argc, char** argv) {
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
    // force the old ROM during renderer testing.
#ifdef _WIN32
    gdx_port_logf("[rom] no ROM argument provided; opening picker.\n");
    pick_rom_with_dialog();
    if (gdx_rom_buffer) return;
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
    if (env && env[0]) { load_rom(env); if (gdx_rom_buffer) return; }

    // 4. Last convenience fallback for unattended builds from the output folder.
    load_rom_next_to_exe();
    if (gdx_rom_buffer) return;

    gdx_port_logf("[rom] WARNING: no ROM file found. Set FZEROX_ROM env var, pass ROM path as argument, or select it in the picker.\n");
    gdx_port_logf("[rom] Textures will be blank. Title screen will show solid colors.\n");
}
} // extern "C"
