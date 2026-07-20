#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Size of the persisted SRAM image, matching the decomp's SaveContext
// (decomp/include/fzx_save.h: profileSaves + ghostSave + characterSaves +
// cupSave = 0x8000 bytes -- the same size as the real F-Zero X cart's
// battery-backed SRAM).
#define GDX_SRAM_SIZE 0x8000u

// Loads saves/fzerox.sav (on Windows relative to the executable; on POSIX relative
// to the current working directory, matching the disk-save sidecar and ghost files --
// a legacy save from the older location is migrated in automatically) into the
// in-memory SRAM image, or zero-fills it if no save file exists yet / it's the wrong
// size (first boot).
// Idempotent: safe to call more than once, later calls are no-ops. Called from
// the decomp's Sram_Init() (see decomp/src/overlays/ovl_i2/save.c) and
// defensively from gdx_sram_read/gdx_sram_write so load order never matters.
void gdx_sram_init(void);

// SRAM byte-range primitives backing the decomp's Sram_ReadWrite. offset/size
// are relative to the GDX_SRAM_SIZE-byte SaveContext image. Out-of-range
// requests are logged and ignored (read returns zeros) rather than touching
// memory outside the buffer.
void gdx_sram_read(unsigned int offset, void* dst, unsigned int size);

// Writes into the in-memory image and immediately persists the whole image to
// fzerox.sav (write-through -- no debounce needed at this size/frequency).
void gdx_sram_write(unsigned int offset, const void* src, unsigned int size);

#ifdef __cplusplus
}
#endif
