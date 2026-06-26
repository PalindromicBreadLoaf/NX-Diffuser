#pragma once
/* Do NOT include <stddef.h> or <stdint.h> here.
   C callers must include global.h first (provides size_t/uint32_t via decomp libc).
   C++ callers must include a standard header before this one (e.g. ship/Context.h). */
#ifdef __cplusplus
extern "C" {
#endif

/* 16MB host RDRAM buffer — maps N64 physical addr `phys` to `gdx_rdram + phys`.
   F-Zero X requires the Expansion Pak; overlay texture offsets reach ~12-13MB physical. */
#define GDX_RDRAM_SIZE           ((size_t)0x1000000u)  /* 16 MB (base 8MB + Expansion Pak) */
#define GDX_RDRAM_GFXPOOL_OFFSET ((size_t)0x100000u)   /* D_1000000 — segment 0x01 */

/* Allocated once at startup by gdx_rdram_init(), before bootproc(). */
extern unsigned char* gdx_rdram;

/* Translate a physical RDRAM offset to a host pointer. */
static inline void* gdx_rdram_ptr(unsigned int phys) {
    return gdx_rdram + phys;
}

/* Allocate 8MB buffer, zero it, place GfxPool pointer, register host range. Fatal on failure. */
void gdx_rdram_init(void);

/* Bump-allocate from the RDRAM arena. Fatal if exhausted. */
void* gdx_rdram_alloc_raw(size_t size, size_t align);

#ifdef __cplusplus
}
#endif
