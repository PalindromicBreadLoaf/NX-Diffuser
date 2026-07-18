// G-Diffuser — port shims (Slice 4c).
// Minimal definitions for the libultra/N64 symbols that libultraship does NOT provide and
// that the decomp references. These are PLACEHOLDERS to achieve a clean link; each gets a
// real implementation (resource-backed ROM access, save system, audio microcode) later.
//
// C linkage: the linker resolves these by name (C has no signature mangling), so simplified
// prototypes are sufficient to satisfy the decomp's references.

#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include "port_log.h"

#define GDX_LEO_TEST_UNIT_MR 0x01
#define GDX_LEO_ERROR_MEDIUM_NOT_PRESENT 42

// ---- libultra function stubs ------------------------------------------------
// Controller Pak (save) — to be backed by libultraship's save/storage system.
int osPfsInitPak(void)       { return -1; }
int osPfsAllocateFile(void)  { return -1; }
int osPfsReadWriteFile(void) { return -1; }
int osPfsFindFile(void)      { return -1; }

// PI / EPI (ROM DMA) — to be replaced by resource-archive reads.
int osEPiReadIo(void)        { return -1; }
int osEPiWriteIo(void)       { return -1; }
int osEPiLinkHandle(void)    { return  0; }
int osDriveRomInit(void)     { return -1; }

// Threading / low-level: osStopThread + __osSetHWIntrRoutine are now provided by the decomp's
// real libultra/os scheduler (stopthread.c, sethwinterrupt.c) — R6 Starship-style.

// libultra debug error hook. Some libultra paths reference this as a function; keep a real
// no-op function shim instead of satisfying the linker with a data symbol.
void __osError(short code, short numArgs, ...) {
    (void)code;
    (void)numArgs;
}

// Audio interface (libultraship provides osAiSetNextBuffer but not this).
int osAiSetFrequency(void) { return 0; }

#ifndef EXPANSION_KIT
// 64DD / leo boot — stubbed for the US-only build. With EXPANSION_KIT enabled
// port/n64_leo.c provides the disk-image-backed implementations of both.
void LeoBootGame(void) {}

// Base-game 64DD probe: the PC port has no inserted disk. Report that state
// explicitly so title-screen logic does not mistake a zeroed status word for
// a fatal drive condition and cover the frame with its black error overlay.
int LeoTestUnitReady(unsigned char* status) {
    if (status != NULL) {
        *status = GDX_LEO_TEST_UNIT_MR;
    }
    return GDX_LEO_ERROR_MEDIUM_NOT_PRESENT;
}
#endif

// libc: BSD byte-compare not in the MSVC CRT. glibc provides both natively (with size_t
// signatures that would conflict), so these shims are Windows-only.
#ifdef _WIN32
int bcmp(const void* a, const void* b, int n) { return memcmp(a, b, (size_t)n); }
void bcopy(const void* src, void* dst, int n) { memmove(dst, src, (size_t)n); }
#endif

// Host CRT wrappers for decomp-side code. The gdiffuser_game object target must not include
// MSVC system headers, so it calls these wrappers instead of relying on implicit CRT prototypes.
void* gdx_host_calloc(size_t count, size_t size) { return calloc(count, size); }
void gdx_host_free(void* ptr) { free(ptr); }
void  gdx_host_exit(int status) { exit(status); }
void  gdx_host_abort(void) { abort(); }

// ---- Memory arena (port reimplementation) ----------------------------------
// Arena_Allocate now carves from the 8MB RDRAM bump allocator (gdx_rdram_alloc_raw).
// The whole RDRAM buffer is registered once at startup in gdx_rdram_init() —
// no per-allocation gdx_register_host_range call needed here.
void* gdx_rdram_alloc_raw(size_t size, size_t align); // defined in decomp_port.c
void* gdx_rdram_peek_raw(size_t size, size_t align);  // deep-audit H1: non-committing peek
void  gdx_rdram_mode_reset(void);                     // deep-audit H1: per-mode arena rewind

/* Deep-audit H1: honor console arena semantics. ALLOC_PEEK is transient
   scratch (cursor not advanced; the next committed allocation may overwrite
   it — exactly how the decomp's texture loader stages mio0 input). FRONT and
   BACK both commit from the single bump region (the console's front/back
   split is an optimization, not a semantic the callers depend on here). */
void* Arena_Allocate(int allocationType, size_t size) {
    if (allocationType == 1 /* ALLOC_PEEK, sys.h */) {
        return gdx_rdram_peek_raw(size, 16u);
    }
    return gdx_rdram_alloc_raw(size, 16u);
}
void  Arena_StartInit(void)        { gdx_rdram_mode_reset(); }
void  Arena_DefaultStartInit(void) { gdx_rdram_mode_reset(); }
void  Arena_EndInit(void)          {}

// ---- N64 ROM-segment / audio-microcode symbols ------------------------------
// Placeholders so global audio data links; real values come from the resource system (4c).
unsigned char audio_bank_ROM_START[1];
unsigned char audio_table_ROM_START[1];
unsigned char audio_seq_ROM_START[1];
unsigned char audio_context_VRAM[1];
unsigned char audio_context_VRAM_END[1];
unsigned long long aspMainTextStart[1];
unsigned long long aspMainDataStart[1];
unsigned long long aspMainDataEnd[1];
