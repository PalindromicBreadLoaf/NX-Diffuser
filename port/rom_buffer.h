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
/* R4 (C-R4.1): archivesValidated is the caller-computed "no-ROM boot is safe" predicate --
 * non-zero iff the fzerox.o2r/generic.o2r game archive is mounted AND survived the post-mount
 * CRC gate (main.cpp's InitResourceManager version check). When resolution fails:
 *   - archivesValidated != 0  -> archive-only boot: log and return with gdx_rom_buffer/gdx_rom_size
 *     left at NULL/0 (every consumer already tolerates this -- see rom_buffer.cpp).
 *   - archivesValidated == 0  -> unchanged actionable hard-fail (FATAL log + MessageBoxW + exit(1)).
 */
void gdx_init_rom(int argc, char** argv, int archivesValidated);

#ifdef __cplusplus
}
#endif
