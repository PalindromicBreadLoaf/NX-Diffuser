#include "sram_buffer.h"
#include "port_log.h"
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cwchar>
#else
#include <cstdlib>
#include <unistd.h> // readlink
#endif

// Host-side persistence for the game's cart-SRAM save (settings, course/death-race
// records, cup completion, character engine tuning, single-slot player/staff ghost).
// Mirrors rom_buffer.cpp's pattern: this is a host .cpp TU (part of the G-Diffuser
// exe target, not the gdiffuser_game decomp object library) so it can freely use the
// MSVC CRT. The decomp side (decomp/src/overlays/ovl_i2/save.c) only ever calls the
// three C-linkage entry points below; it never sees fopen/FILE* etc.
extern "C" {

static uint8_t s_sramBuffer[GDX_SRAM_SIZE];
static bool s_initialized = false;

#ifdef _WIN32
static bool gdx_sram_path(wchar_t* outPath, size_t outCapChars) {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        return false;
    }

    wchar_t* slash = std::wcsrchr(exePath, L'\\');
    if (slash == nullptr) {
        return false;
    }
    slash[1] = L'\0';

    const wchar_t* fileName = L"fzerox.sav";
    if (wcslen(exePath) + wcslen(fileName) >= outCapChars) {
        return false;
    }
    wcscpy_s(outPath, outCapChars, exePath);
    wcscat_s(outPath, outCapChars, fileName);
    return true;
}
#else
// POSIX: resolve the save file next to the executable via /proc/self/exe, mirroring the Windows
// GetModuleFileNameW path. Falls back to the bare relative name if the exe path is unavailable.
static bool gdx_sram_path(char* outPath, size_t outCap) {
    const char* fileName = "fzerox.sav";
    char exePath[4096];
    ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (n <= 0) {
        // No exe path: use the CWD-relative name so a save still persists somewhere sane.
        if (strlen(fileName) >= outCap) {
            return false;
        }
        strcpy(outPath, fileName);
        return true;
    }
    exePath[n] = '\0';

    char* slash = strrchr(exePath, '/');
    if (slash == nullptr) {
        return false;
    }
    slash[1] = '\0'; // keep trailing separator

    if (strlen(exePath) + strlen(fileName) >= outCap) {
        return false;
    }
    strcpy(outPath, exePath);
    strcat(outPath, fileName);
    return true;
}
#endif

void gdx_sram_init(void) {
    if (s_initialized) {
        return;
    }
    s_initialized = true;
    memset(s_sramBuffer, 0, sizeof(s_sramBuffer));

#ifdef _WIN32
    wchar_t path[MAX_PATH * 2] = {};
    if (!gdx_sram_path(path, sizeof(path) / sizeof(path[0]))) {
        gdx_port_logf("[sram] WARNING: could not resolve save file path; starting with a fresh save.\n");
        return;
    }

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) {
        gdx_port_logf("[sram] no existing fzerox.sav; starting fresh (first boot creates it on first write).\n");
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz != (long) GDX_SRAM_SIZE) {
        gdx_port_logf("[sram] fzerox.sav size mismatch (%ld bytes, expected %u); starting with a fresh save.\n", sz,
                      GDX_SRAM_SIZE);
        fclose(f);
        return;
    }

    if (fread(s_sramBuffer, 1, GDX_SRAM_SIZE, f) != GDX_SRAM_SIZE) {
        gdx_port_logf("[sram] WARNING: failed reading fzerox.sav; starting with a fresh save.\n");
        memset(s_sramBuffer, 0, sizeof(s_sramBuffer));
    } else {
        gdx_port_logf("[sram] loaded %u bytes from fzerox.sav\n", GDX_SRAM_SIZE);
    }
    fclose(f);
#else
    char path[4096] = {};
    if (!gdx_sram_path(path, sizeof(path))) {
        gdx_port_logf("[sram] WARNING: could not resolve save file path; starting with a fresh save.\n");
        return;
    }

    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        gdx_port_logf("[sram] no existing fzerox.sav; starting fresh (first boot creates it on first write).\n");
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz != (long) GDX_SRAM_SIZE) {
        gdx_port_logf("[sram] fzerox.sav size mismatch (%ld bytes, expected %u); starting with a fresh save.\n", sz,
                      GDX_SRAM_SIZE);
        fclose(f);
        return;
    }

    if (fread(s_sramBuffer, 1, GDX_SRAM_SIZE, f) != GDX_SRAM_SIZE) {
        gdx_port_logf("[sram] WARNING: failed reading fzerox.sav; starting with a fresh save.\n");
        memset(s_sramBuffer, 0, sizeof(s_sramBuffer));
    } else {
        gdx_port_logf("[sram] loaded %u bytes from fzerox.sav\n", GDX_SRAM_SIZE);
    }
    fclose(f);
#endif
}

static void gdx_sram_flush(void) {
#ifdef _WIN32
    wchar_t path[MAX_PATH * 2] = {};
    if (!gdx_sram_path(path, sizeof(path) / sizeof(path[0]))) {
        gdx_port_logf("[sram] WARNING: could not resolve save file path; save not persisted.\n");
        return;
    }

    // Atomic write-through: stage to fzerox.sav.tmp, flush, then MoveFileEx-replace
    // the live file. A crash mid-write can no longer truncate/half-write the real
    // save -- the previous fzerox.sav stays intact until the temp is complete.
    wchar_t tempPath[MAX_PATH * 2] = {};
    if (wcslen(path) + 4 >= sizeof(tempPath) / sizeof(tempPath[0])) {
        gdx_port_logf("[sram] WARNING: save path too long for temp file; save not persisted.\n");
        return;
    }
    wcscpy_s(tempPath, sizeof(tempPath) / sizeof(tempPath[0]), path);
    wcscat_s(tempPath, sizeof(tempPath) / sizeof(tempPath[0]), L".tmp");

    FILE* f = nullptr;
    if (_wfopen_s(&f, tempPath, L"wb") != 0 || f == nullptr) {
        gdx_port_logf("[sram] WARNING: failed to open fzerox.sav.tmp for writing; save not persisted.\n");
        return;
    }
    bool ok = fwrite(s_sramBuffer, 1, GDX_SRAM_SIZE, f) == GDX_SRAM_SIZE;
    if (fflush(f) != 0) {
        ok = false;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        gdx_port_logf("[sram] WARNING: failed writing fzerox.sav.tmp; save not persisted.\n");
        _wremove(tempPath);
        return;
    }
    if (!MoveFileExW(tempPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        gdx_port_logf("[sram] WARNING: could not replace fzerox.sav; save not persisted.\n");
        _wremove(tempPath);
        return;
    }
#else
    char path[4096] = {};
    if (!gdx_sram_path(path, sizeof(path))) {
        gdx_port_logf("[sram] WARNING: could not resolve save file path; save not persisted.\n");
        return;
    }

    // Atomic write-through: stage to fzerox.sav.tmp, flush, then rename over the live file. A
    // crash mid-write can no longer truncate the real save -- rename(2) is atomic within a
    // filesystem, so the previous fzerox.sav stays intact until the temp is complete.
    char tempPath[4096 + 8] = {};
    if (strlen(path) + 4 >= sizeof(tempPath)) {
        gdx_port_logf("[sram] WARNING: save path too long for temp file; save not persisted.\n");
        return;
    }
    strcpy(tempPath, path);
    strcat(tempPath, ".tmp");

    FILE* f = fopen(tempPath, "wb");
    if (f == nullptr) {
        gdx_port_logf("[sram] WARNING: failed to open fzerox.sav.tmp for writing; save not persisted.\n");
        return;
    }
    bool ok = fwrite(s_sramBuffer, 1, GDX_SRAM_SIZE, f) == GDX_SRAM_SIZE;
    if (fflush(f) != 0) {
        ok = false;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        gdx_port_logf("[sram] WARNING: failed writing fzerox.sav.tmp; save not persisted.\n");
        remove(tempPath);
        return;
    }
    if (rename(tempPath, path) != 0) {
        gdx_port_logf("[sram] WARNING: could not replace fzerox.sav; save not persisted.\n");
        remove(tempPath);
        return;
    }
#endif
}

void gdx_sram_read(unsigned int offset, void* dst, unsigned int size) {
    gdx_sram_init(); // defensive: guarantee the image is loaded even if Sram_Init() wasn't called first
    if (dst == nullptr) {
        return;
    }
    if ((size_t) offset + (size_t) size > GDX_SRAM_SIZE) {
        gdx_port_logf("[sram] WARNING: read out of range (offset=%u size=%u); returning zeros.\n", offset, size);
        memset(dst, 0, size);
        return;
    }
    memcpy(dst, s_sramBuffer + offset, size);
}

void gdx_sram_write(unsigned int offset, const void* src, unsigned int size) {
    gdx_sram_init();
    if (src == nullptr) {
        return;
    }
    if ((size_t) offset + (size_t) size > GDX_SRAM_SIZE) {
        gdx_port_logf("[sram] WARNING: write out of range (offset=%u size=%u); ignored.\n", offset, size);
        return;
    }
    memcpy(s_sramBuffer + offset, src, size);
    gdx_sram_flush(); // write-through: no debounce needed at 32KB / event-driven call frequency
}

} // extern "C"
