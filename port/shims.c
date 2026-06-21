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

// Audio interface (libultraship provides osAiSetNextBuffer but not this).
int osAiSetFrequency(void) { return 0; }

// 64DD / leo boot (Expansion Kit) — stubbed for the US base.
void LeoBootGame(void) {}

// libc: BSD byte-compare not in the MSVC CRT.
int bcmp(const void* a, const void* b, int n) { return memcmp(a, b, (size_t)n); }
void bcopy(const void* src, void* dst, int n) { memmove(dst, src, (size_t)n); }

// ---- Memory arena (port reimplementation) ----------------------------------
// N64 carved arenas out of fixed RDRAM regions; on host we just use the heap so the game's
// allocations actually have backing memory. (allocationType is ignored for now.)
void* Arena_Allocate(int allocationType, size_t size) { (void)allocationType; return malloc(size); }
void  Arena_StartInit(void)        {}
void  Arena_DefaultStartInit(void) {}
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
