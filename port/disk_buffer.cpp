// G-Diffuser — 64DD disk image loader (Expansion Kit).
// Host-side counterpart of port/n64_leo.c: owns the CRT file I/O the game
// target cannot include. Mirrors rom_buffer.cpp's role for the cartridge.
#include "port_log.h"
#include <cstdio>
#include <cstdlib>

extern "C" {
unsigned char* gdx_disk_buffer = nullptr;
unsigned int gdx_disk_size = 0;

void gdx_ek_assets_fill(const unsigned char* disk, unsigned long long diskSize);
void gdx_leo_on_disk_loaded(const unsigned char* disk);

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
