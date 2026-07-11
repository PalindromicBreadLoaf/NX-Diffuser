// G-Diffuser — 64DD disk image loader (Expansion Kit).
// Host-side counterpart of port/n64_leo.c: owns the CRT file I/O the game
// target cannot include. Mirrors rom_buffer.cpp's role for the cartridge.
#include "port_log.h"
#include "rom_buffer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

extern "C" {
unsigned char* gdx_disk_buffer = nullptr;
unsigned int gdx_disk_size = 0;

// 64DD IPL/drive ROM image (user-supplied). Holds the built-in kanji/ANK font
// the EK text renderers DMA from (LeoGetKAdr/LeoGetAAdr + DDROM_FONT_START).
unsigned char* gdx_ddipl_buffer = nullptr;
unsigned int gdx_ddipl_size = 0;

void gdx_ek_assets_fill(const unsigned char* disk, unsigned long long diskSize);
void gdx_leo_on_disk_loaded(const unsigned char* disk);

// 64DD boot-logo source texture (decomp/assets/yaml/jp/ek/boot_logo.yaml,
// D_80769DF0, 136x39 RGBA16). Zero-filled by the EK asset generator at build
// time; gdx_ek_assets_fill() below is what actually copies real pixels into
// it from the loaded disk image. sys_main.c's func_806F33D0 blits this buffer
// straight into the VI framebuffer with no RDP task (see the PORT note in
// n64_gfx_bridge.cpp gdx_vi_present_fallback). Declared here (not in a
// decomp header) so this host TU can read it for the boot-logo evidence log
// below without pulling in decomp headers.
extern unsigned short D_80769DF0[];

// Loads N64DDIPLROM.n64 and normalizes it to native big-endian byte order
// (dumps circulate as z64/BE, v64/16-bit-swapped, or n64/32-bit-LE; detect by
// the first byte of the PI header, which is 0x80 in native order).
static void gdx_ddipl_load(void) {
    if (gdx_ddipl_buffer != nullptr) {
        return;
    }
    FILE* f = fopen("N64DDIPLROM.n64", "rb");
    if (f == nullptr) {
        gdx_port_logf("[leo] no N64DDIPLROM.n64; drive-ROM font stays blank\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0x100000) { // sanity: IPL ROM dumps are 4MB
        fclose(f);
        gdx_port_logf("[leo] N64DDIPLROM.n64 too small (%ld bytes); ignored\n", sz);
        return;
    }
    unsigned char* buf = static_cast<unsigned char*>(malloc(static_cast<size_t>(sz)));
    if (buf == nullptr || fread(buf, 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
        free(buf);
        fclose(f);
        return;
    }
    fclose(f);
    if (buf[0] == 0x80) {
        // native big-endian
    } else if (buf[1] == 0x80) {
        for (long i = 0; i + 1 < sz; i += 2) { // v64: swap 16-bit pairs
            unsigned char t = buf[i]; buf[i] = buf[i + 1]; buf[i + 1] = t;
        }
    } else if (buf[3] == 0x80) {
        for (long i = 0; i + 3 < sz; i += 4) { // n64: reverse 32-bit words
            unsigned char t0 = buf[i], t1 = buf[i + 1];
            buf[i] = buf[i + 3]; buf[i + 1] = buf[i + 2];
            buf[i + 2] = t1; buf[i + 3] = t0;
        }
    } else {
        gdx_port_logf("[leo] N64DDIPLROM.n64: unrecognized byte order (first bytes %02X %02X %02X %02X); using as-is\n",
                      buf[0], buf[1], buf[2], buf[3]);
    }
    gdx_ddipl_buffer = buf;
    gdx_ddipl_size = static_cast<unsigned int>(sz);
    gdx_port_logf("[leo] 64DD IPL ROM loaded (%ld bytes)\n", sz);
}

// Best-effort check of whether the loaded cartridge ROM is the Japanese
// release, from the standard N64 ROM header country-code byte (offset 0x3E).
// Only meaningful for a big-endian (.z64) image -- confirmed via the z64
// magic word at offset 0 (0x80 0x37 0x12 0x40). gdx_init_rom() does not
// normalize v64 (byte-swapped) or n64 (word-swapped) dumps today, so for
// those we can't read the header reliably and fall back to "not confidently
// Japanese", which keeps today's default disk preference (translated-first).
static bool gdx_rom_is_japanese(void) {
    if (gdx_rom_buffer == nullptr || gdx_rom_size < 0x40) {
        return false;
    }
    if (gdx_rom_buffer[0] != 0x80 || gdx_rom_buffer[1] != 0x37) {
        return false;
    }
    return gdx_rom_buffer[0x3E] == 'J';
}

// Directory (including trailing separator) of a file path, or "" if the path
// has no directory component. Handles both '/' and '\\' since ROM paths can
// come from a Windows file picker (backslash) or a hand-typed FZEROX_ROM
// value (either).
static void gdx_dir_of(const char* path, char* outDir, size_t outSize) {
    outDir[0] = '\0';
    if (path == nullptr || path[0] == '\0' || outSize == 0) {
        return;
    }
    const char* slash = strrchr(path, '\\');
    const char* fwdSlash = strrchr(path, '/');
    if (fwdSlash != nullptr && (slash == nullptr || fwdSlash > slash)) {
        slash = fwdSlash;
    }
    if (slash == nullptr) {
        return;
    }
    size_t len = static_cast<size_t>(slash - path) + 1; // keep the separator
    if (len >= outSize) {
        len = outSize - 1;
    }
    memcpy(outDir, path, len);
    outDir[len] = '\0';
}

#ifdef _WIN32
static void gdx_exe_dir(char* outDir, size_t outSize) {
    outDir[0] = '\0';
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        return;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash == nullptr) {
        return;
    }
    slash[1] = L'\0';
    WideCharToMultiByte(CP_UTF8, 0, exePath, -1, outDir, static_cast<int>(outSize), nullptr, nullptr);
}
#endif

int gdx_disk_load(void) {
    if (gdx_disk_buffer != nullptr) {
        return 1;
    }

    // Disk name preference order: the fan-translated English disk
    // (LuigiBlood/Zoinkity, 64DD.org) keeps the retail disk layout, so
    // loading it instead of the JP dump delivers English EK text/font
    // assets through the exact same offsets and MFS reads -- no extraction
    // step needed. That's the right default and the fallback whenever the
    // loaded ROM's region can't be determined. For a confirmed Japanese
    // cartridge ROM, prefer the untranslated JP disk so on-disk text
    // matches the cartridge's language.
    const bool jpRom = gdx_rom_is_japanese();
    static const char* const kUsPreferredNames[] = {
        "baserom.translated.ek.ndd",
        "baserom.jp.ek.ndd",
        "baserom.jp.disk",
    };
    static const char* const kJpPreferredNames[] = {
        "baserom.jp.ek.ndd",
        "baserom.jp.disk",
        "baserom.translated.ek.ndd",
    };
    const char* const* diskNames = jpRom ? kJpPreferredNames : kUsPreferredNames;
    const size_t diskNameCount = 3;

    // Search location order: (1) next to the chosen ROM -- so multiple
    // installs/folders with different .ndd files never cross-pollinate --
    // then (2) the exe directory, then (3) the process's current working
    // directory (legacy behavior, kept as a last resort for scripted/dev
    // launches that rely on CWD rather than exe-relative paths).
    char romDir[1024] = {};
    gdx_dir_of(gdx_rom_path, romDir, sizeof(romDir));
    char exeDir[1024] = {};
#ifdef _WIN32
    gdx_exe_dir(exeDir, sizeof(exeDir));
#endif

    struct SearchLocation {
        const char* dir;
        const char* why;
    };
    const SearchLocation searchLocations[] = {
        { romDir, "next to chosen ROM" },
        { exeDir, "exe directory" },
        { "", "current directory" },
    };

    for (const SearchLocation& loc : searchLocations) {
        for (size_t i = 0; i < diskNameCount; i++) {
            char path[1200];
            snprintf(path, sizeof(path), "%s%s", loc.dir, diskNames[i]);

            FILE* f = fopen(path, "rb");
            if (f == nullptr) {
                continue;
            }
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz <= 0) {
                fclose(f);
                continue;
            }
            unsigned char* buf = static_cast<unsigned char*>(malloc(static_cast<size_t>(sz)));
            if (buf == nullptr) {
                fclose(f);
                return 0;
            }
            if (fread(buf, 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
                free(buf);
                fclose(f);
                continue;
            }
            fclose(f);
            gdx_disk_buffer = buf;
            gdx_disk_size = static_cast<unsigned int>(sz);
            gdx_port_logf("[leo] disk image loaded: %s (%ld bytes) -- picked from %s, region=%s\n", path, sz,
                          loc.why, jpRom ? "JP (matches cartridge ROM)" : "US/unknown (default preference)");
            gdx_leo_on_disk_loaded(gdx_disk_buffer);
            gdx_ek_assets_fill(gdx_disk_buffer, static_cast<unsigned long long>(gdx_disk_size));

            /* Host byte order for the boot-logo texels (2026-07-09): the fill
               above copies raw BIG-ENDIAN disk bytes, but func_806F33D0
               (sys_main.c) CPU-blits these u16s into a VI framebuffer that
               everything else — the game's own clear (0x0001) and the seed
               quad's RGBA5551 conversion — treats as host-order. The result
               on screen was the byte-swap signature exactly: white "64DD"
               intact (0xFFFF is swap-invariant), the N-cube's colors
               scrambled, and the cleared-black border around the blit showing
               as dim green (0x0001 -> 0x0100). Swap once here, at the only
               fill site, right after the probe; the blit is this texture's
               only consumer (no GFX-task path reads it as big-endian). */
            for (unsigned int i = 0; i < 5304; i++) {
                const unsigned short v = D_80769DF0[i];
                D_80769DF0[i] = static_cast<unsigned short>((v >> 8) | (v << 8));
            }

            gdx_ddipl_load();
            return 1;
        }
    }

    static bool sWarned = false;
    if (!sWarned) {
        sWarned = true;
        gdx_port_logf("[leo] no disk image found; Expansion Kit modes stay disabled\n");
    }
    return 0;
}
}
