// G-Diffuser — 64DD disk image loader (Expansion Kit).
// Host-side counterpart of port/n64_leo.c: owns the CRT file I/O the game
// target cannot include. Mirrors rom_buffer.cpp's role for the cartridge.
#include "port_log.h"
#include <cstdio>
#include <cstdlib>

extern "C" {
unsigned char* gdx_disk_buffer = nullptr;
unsigned int gdx_disk_size = 0;

// 64DD IPL/drive ROM image (user-supplied). Holds the built-in kanji/ANK font
// the EK text renderers DMA from (LeoGetKAdr/LeoGetAAdr + DDROM_FONT_START).
unsigned char* gdx_ddipl_buffer = nullptr;
unsigned int gdx_ddipl_size = 0;

void gdx_ek_assets_fill(const unsigned char* disk, unsigned long long diskSize);
void gdx_leo_on_disk_loaded(const unsigned char* disk);

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

int gdx_disk_load(void) {
    // Preference order: the fan-translated English disk (LuigiBlood/Zoinkity,
    // 64DD.org) keeps the retail disk layout, so loading it instead of the JP
    // dump delivers English EK text/font assets through the exact same offsets
    // and MFS reads — no extraction step needed.
    static const char* const kDiskImageNames[] = {
        "baserom.translated.ek.ndd",
        "baserom.jp.ek.ndd",
        "baserom.jp.disk",
    };

    if (gdx_disk_buffer != nullptr) {
        return 1;
    }

    for (const char* name : kDiskImageNames) {
        FILE* f = fopen(name, "rb");
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
        gdx_port_logf("[leo] disk image loaded: %s (%ld bytes)\n", name, sz);
        gdx_leo_on_disk_loaded(gdx_disk_buffer);
        gdx_ek_assets_fill(gdx_disk_buffer, static_cast<unsigned long long>(gdx_disk_size));
        gdx_ddipl_load();
        return 1;
    }

    static bool sWarned = false;
    if (!sWarned) {
        sWarned = true;
        gdx_port_logf("[leo] no disk image found; Expansion Kit modes stay disabled\n");
    }
    return 0;
}
}
