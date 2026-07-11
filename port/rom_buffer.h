#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t* gdx_rom_buffer;
extern size_t   gdx_rom_size;
/* Best-effort path of the ROM actually loaded (picker selection, cmdline arg,
 * FZEROX_ROM env var, or exe-dir fallback candidate) -- empty if no ROM has
 * been loaded yet. Used by disk_buffer.cpp to search for a 64DD disk image
 * next to the chosen ROM before falling back to the exe directory. */
extern char gdx_rom_path[1024];
void gdx_init_rom(int argc, char** argv);

#ifdef __cplusplus
}
#endif
