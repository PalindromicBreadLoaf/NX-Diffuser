// G-Diffuser — decomp-side port subsystems (Slice 4c).
// Compiled WITH the decomp's headers/flags (part of the gdiffuser_game target), so it uses
// the real game types. Provides port reimplementations / placeholders for symbols that lived
// in N64-platform files excluded from the host build (segment system, save, fixed-address
// data). Placeholders let the exe LINK and boot; real backing (resource system, LUS save)
// comes next.

#include "global.h"
#include "fzx_course.h"
#include "fzx_game.h"
#include "n64_rdram.h"

/* port_log.h pulls in <stdio.h> which clashes with the decomp's libc/stdint.h.
   Use gdx_ck (defined in n64_sched.c, which CAN include stdio.h) for logging. */
extern void gdx_ck(const char* s);
extern void gdx_seg_log(const char* kind, int seg, uintptr_t raw, void* resolved);
extern void gdx_addr_log(const char* kind, uintptr_t raw, void* resolved);
extern void* gdx_host_calloc(size_t count, size_t size);
extern void  gdx_host_exit(int status);
extern void  gdx_host_abort(void);
extern void* gdx_resolve_registered_host_address(unsigned int addr);
extern void* gdx_resolve_module_host_address(unsigned int addr);
extern void* gdx_ensure_asset_segment_for_symbol(unsigned int symLow32, unsigned int* outOffset);
extern int   gdx_load_venue_texture_segment(int venue);
extern s32   gGameMode;

extern unsigned char* gdx_rom_buffer;
extern size_t gdx_rom_size;
extern unk_80225800 D_80225800;
#ifdef EXPANSION_KIT
extern unk_80128C94* D_80128C90;
extern unk_80128C94* D_80128C94;
#endif

// ---- RDRAM host buffer globals ----------------------------------------------
// gdx_rdram: single 16MB contiguous buffer allocated by gdx_rdram_init().
// All N64 physical addresses resolve to gdx_rdram + phys.
// gdx_gfxpool: pointer to the GfxPool D_1000000 object (alias, not RDRAM-resident).
// gdx_rdram_bump: byte offset of the next free arena byte (carves upward).
// gdx_rdram_arena_start: first arena byte after the GfxPool reservation AND the
//   dedicated ALLOC_PEEK staging block (see gdx_rdram_staging_base below).
// gdx_rdram_staging_base: byte offset of the fixed GDX_RDRAM_STAGING_SIZE block
//   reserved for gdx_rdram_peek_raw, carved between the GfxPool and the arena.

unsigned char* gdx_rdram          = NULL;
static size_t  gdx_rdram_bump     = 0;
size_t         gdx_rdram_arena_start = 0;
static size_t  gdx_rdram_persist_top = 0;
static size_t  gdx_rdram_staging_base = 0;
GfxPool*       gdx_gfxpool        = NULL;

static size_t Gdx_RomOffset(u32 addr) {
    u32 phys = addr & 0x1FFFFFFF;
    return (phys >= 0x10000000) ? (size_t)(phys - 0x10000000) : (size_t)phys;
}

// ---- RDRAM init + bump allocator -------------------------------------------

extern void gdx_register_host_range(void* ptr, size_t size); // defined in n64_gfx_bridge.cpp
extern void gdx_register_host_wide_command_range(void* ptr, size_t size);
extern GfxPool D_1000000; // defined below; forward-declared here so gdx_rdram_init() can reference it
extern GfxPool D_8024DCE0[2];

void gdx_rdram_init(void) {
    gdx_rdram = (unsigned char*)gdx_host_calloc(1, GDX_RDRAM_SIZE);
    if (gdx_rdram == NULL) {
        gdx_ck("[rdram] FATAL: failed to allocate 8MB RDRAM buffer");
        gdx_host_exit(1);
    }

    // Staging block starts right after the GfxPool reservation, 16-byte aligned.
    // The arena then starts after the staging block, so ALLOC_FRONT/BACK commits
    // (gdx_rdram_alloc_raw) can never bump into the staging bytes that a live
    // ALLOC_PEEK (gdx_rdram_peek_raw) is using.
    gdx_rdram_staging_base = GDX_RDRAM_GFXPOOL_OFFSET +
                             ((sizeof(GfxPool) + 15u) & ~(size_t)15u);
    gdx_rdram_arena_start  = gdx_rdram_staging_base + GDX_RDRAM_STAGING_SIZE;
    gdx_rdram_bump         = gdx_rdram_arena_start;
    gdx_rdram_persist_top  = GDX_RDRAM_SIZE;

    // D_1000000 is a linker-symbol BSS global; point gdx_gfxpool at it.
    // The GfxPool stays as a host BSS allocation so all extern GfxPool D_1000000
    // declarations in the decomp source files link correctly without modification.
    gdx_gfxpool = &D_1000000;

    // Register the whole RDRAM buffer once — covers all future arena allocs.
    gdx_register_host_range(gdx_rdram, GDX_RDRAM_SIZE);
    gdx_register_host_range(&D_1000000, sizeof(D_1000000));
    gdx_register_host_range(D_8024DCE0, sizeof(D_8024DCE0));
    // The audio heap arena: every AudioHeap_Alloc* pool (including the aiBuffers the
    // audio HLE's A_LOADBUFF/A_SAVEBUFF ops address by truncated low32) carves from
    // this BSS block. It was never registered — on Windows the module-range
    // reconstruction happened to cover it, but on Linux PIE the module range does
    // not reach this BSS tail, every LOADBUFF/SAVEBUFF resolved NULL, the ops were
    // skipped, and the synthesized output stayed all-zero (the "no audio on Linux"
    // defect). Registration makes the resolution explicit on every platform.
    // Size MUST match the PORT declaration in decomp/src/audio/disk/lib/audio.h
    // ("extern u8 gAudioHeap[0x2ECA00 * 4]" — enlarged 4x on host builds).
    {
        extern unsigned char gAudioHeap[];
        gdx_register_host_range(gAudioHeap, (size_t)0x2ECA00 * 4);
    }

    {
        extern void gdx_cki(const char*, int);
        extern void gdx_ckp(const char*, void*);
        gdx_ckp("[rdram] base", (void*)gdx_rdram);
        gdx_cki("[rdram] staging_base", (int)gdx_rdram_staging_base);
        gdx_cki("[rdram] staging_size", (int)GDX_RDRAM_STAGING_SIZE);
        gdx_cki("[rdram] arena_start", (int)gdx_rdram_arena_start);
        gdx_cki("[rdram] sizeof GfxPool", (int)sizeof(GfxPool));
    }

    gdx_ck("[rdram] 8MB buffer initialized");
}

void* gdx_rdram_alloc_raw(size_t size, size_t align) {
    size_t base = (gdx_rdram_bump + (align - 1u)) & ~(align - 1u);
    if (base + size > gdx_rdram_persist_top) {
        gdx_ck("[rdram] FATAL: arena exhausted");
        gdx_host_abort();
    }
    gdx_rdram_bump = base + size;
    return gdx_rdram + base;
}

/* Persistent allocations bump DOWN from the top of RDRAM toward the mode
   arena. gdx_rdram_mode_reset never touches this region, so it survives
   every game-mode rewind. Used for data whose lifetime is the whole session
   (audio soundfont conversions: gAudioCtx.soundFontList keeps pointers into
   these across mode transitions). The two regions grow toward each other;
   exhaustion only when they actually meet. */
void* gdx_rdram_persist_alloc_raw(size_t size, size_t align) {
    size_t base;
    if (size > gdx_rdram_persist_top) {
        gdx_ck("[rdram] FATAL: arena exhausted (persist)");
        gdx_host_abort();
    }
    base = (gdx_rdram_persist_top - size) & ~(align - 1u);
    if (base < gdx_rdram_bump) {
        gdx_ck("[rdram] FATAL: arena exhausted (persist)");
        gdx_host_abort();
    }
    gdx_rdram_persist_top = base;
    return gdx_rdram + base;
}

/* Deep-audit H1: ALLOC_PEEK on console returns the arena cursor WITHOUT
   advancing it (transient scratch, overwritten by the next real allocation).
   The bump shim previously served peeks straight from the mode arena cursor
   (gdx_rdram_bump). That is a race: the texture loader (object.c cases
   17/18/20/21) peeks a staging buffer, DMAs MIO0-compressed data into it, then
   calls mio0Decode, which cooperatively yields every 4096 output bytes
   (torch/lib/libmio0/mio0.c). During a yield any other fiber committing via
   Arena_Allocate(ALLOC_FRONT/BACK) bumps gdx_rdram_bump forward from exactly
   where the peek sits, and the next commit can land on top of the live
   compressed source mid-decode.
   Fix: peeks are now served from a dedicated GDX_RDRAM_STAGING_SIZE block
   (gdx_rdram_staging_base..+GDX_RDRAM_STAGING_SIZE) that sits BEFORE
   gdx_rdram_arena_start and is never touched by gdx_rdram_alloc_raw or
   gdx_rdram_persist_alloc_raw. FRONT/BACK commits physically cannot reach it.

   INVARIANT: every peek is served from the SAME base offset within the
   staging block (no bump, no accumulation across calls) — this mirrors
   console PEEK semantics, where the next commit (or the next peek) may
   overwrite a previous peek's contents. This is only correct if at most one
   peek is ever "live" (allocated but not yet fully consumed) at a time.
   Verified against every compiled ALLOC_PEEK call site (decomp/src/game/object.c,
   decomp/src/overlays/ovl_i10/1459A0.c — decomp/src/sys/segment.c is excluded
   from the PORT build, see port/CMakeLists.txt): each texture load does at
   most one FRONT commit, then ONE peek that is either consumed synchronously
   (header parse, e.g. object.c's func_800AA6BC on an 8-byte peek) before the
   next peek call, or handed once to mio0Decode and not touched again until
   the following iteration's peek. The save-load peek in 1459A0.c is consumed
   synchronously via Sram_ReadWrite with no yield. No caller holds two live
   peeks concurrently, so serving every peek at the block base is safe. */
void* gdx_rdram_peek_raw(size_t size, size_t align) {
    size_t base;

    if (size <= GDX_RDRAM_STAGING_SIZE) {
        base = (gdx_rdram_staging_base + (align - 1u)) & ~(align - 1u);
        return gdx_rdram + base;
    }

    // Oversized peek (bigger than the dedicated staging block): fall back to
    // the old cursor behavior. Still racy against a concurrent FRONT/BACK
    // commit landing at gdx_rdram_bump, but this path should never be hit by
    // any known compiled caller (see census above) — log once per distinct
    // size so a regression or new caller is visible without spamming.
    {
        extern void gdx_cki(const char*, int);
        static size_t gdx_rdram_peek_overflow_last = (size_t)-1;
        if (size != gdx_rdram_peek_overflow_last) {
            gdx_rdram_peek_overflow_last = size;
            gdx_ck("[rdram] WARN: peek exceeds staging block");
            gdx_cki("[rdram] WARN peek size", (int)size);
        }
    }

    base = (gdx_rdram_bump + (align - 1u)) & ~(align - 1u);
    if (base + size > gdx_rdram_persist_top) {
        gdx_ck("[rdram] FATAL: arena exhausted (peek)");
        gdx_host_abort();
    }
    return gdx_rdram + base;
}

/* Deep-audit H1: console Arena_StartInit resets the arena at every game-mode
   transition; the port shim was a no-op, leaking all mode-scoped allocations.
   The first StartInit call captures the post-boot cursor as the baseline
   (protecting boot-time persistent carve-outs made before any mode starts);
   later calls rewind to it. */
static size_t gdx_rdram_mode_baseline = 0;
static int gdx_rdram_baseline_set = 0;

#ifdef PORT
/* Base-game glyph/texture decode cache (object.c: D_800E33E0[] keyed by asset,
   value = decoded buffer pointer; count D_800E3A20). func_80077D44() invalidates
   it by zeroing the count only — cheap and safe to call on every rewind. */
extern void func_80077D44(void);
#endif

void gdx_rdram_mode_reset(void) {
    if (!gdx_rdram_baseline_set) {
        gdx_rdram_baseline_set = 1;
        gdx_rdram_mode_baseline = gdx_rdram_bump;
        return;
    }
    gdx_rdram_bump = gdx_rdram_mode_baseline;

#ifdef PORT
    /* SCRAMBLED-TEXT FIX (docs/investigation/2026-07-17/SCRAMBLED_TEXT.md).
       The glyph/texture decode cache stores HOST pointers (gdx_rdram + offset)
       into this bump arena. Rewinding the bump here re-issues those exact offsets
       to the next mode's decodes, so any cache entry that survives the rewind now
       maps a glyph key to memory holding a DIFFERENT glyph's bytes. On a cache HIT
       the decode is skipped and the stale pointer is served verbatim -> crisp but
       wrong letters (CAPTAIN FALCON -> C3OP3IC, SILENCE, editor node/warning text),
       consistent per screen and random across runs.

       The decomp resets the cache only on the func_80079EC8 transition path
       (object.c:1417); the reload path func_80079F1C (game.c:883) rewinds objects
       but NOT the cache, leaving stale host pointers into a reused arena. Because
       the bump arena is rewound ONLY through here (Arena_StartInit and
       Arena_DefaultStartInit both route to gdx_rdram_mode_reset; gdx_rdram_alloc_raw
       only advances), invalidating the cache on every rewind makes the two atomic
       regardless of which decomp path triggered the transition — no cache entry can
       ever outlive the memory it points into. */
    func_80077D44();
#endif
}

// ---- Physical <-> Virtual address translation (PORT) -----------------------
// physicaltovirtual.c and virtualtophysical.c are excluded from the CMake build
// (they contain N64-platform implementations). Port implementations live here
// where both gdx_rdram and gdx_rom_buffer are visible.

#ifdef PORT
void* osPhysicalToVirtual(u32 addr) {
    u32 phys = addr & 0x1FFFFFFFu;
    if (phys >= 0x10000000u) {
        // Cart ROM region: map into the host ROM buffer.
        if (gdx_rom_buffer != NULL) {
            return gdx_rom_buffer + (phys - 0x10000000u);
        }
        return gdx_rdram; // safe non-NULL fallback
    }
    // RDRAM region.
    return gdx_rdram + phys;
}

u32 osVirtualToPhysical(void* vaddr) {
    unsigned char* c = (unsigned char*)vaddr;
    // RDRAM-resident pointer: return offset from base.
    if (gdx_rdram != NULL && c >= gdx_rdram && c < gdx_rdram + GDX_RDRAM_SIZE) {
        return (u32)(c - gdx_rdram);
    }
    /* Host/BSS pointers cannot fit in libultra's u32 physical-address return.
       Return a low32 token, then Gdx_ResolvePortAddress reconstructs it via
       registered host ranges or the EXE module range before storing gSegments[]. */
    return (u32)(uintptr_t)vaddr;
}
#endif

u32 gdx_rom_read32(u32 addr) {
    size_t off = Gdx_RomOffset(addr);
    if (gdx_rom_buffer == NULL || off + sizeof(u32) > gdx_rom_size) {
        return 0;
    }

    return ((u32)gdx_rom_buffer[off + 0] << 24) |
           ((u32)gdx_rom_buffer[off + 1] << 16) |
           ((u32)gdx_rom_buffer[off + 2] << 8)  |
           ((u32)gdx_rom_buffer[off + 3] << 0);
}

u32 gdx_io_read(u32 addr) {
    (void)addr;
    return 0;
}

void gdx_io_write(u32 addr, u32 data) {
    (void)addr;
    (void)data;
}

// ---- Segment system (port reimplementation) --------------------------------
// N64 mapped 16 graphics segments to RDRAM; the game addresses assets as (segment<<24|offset).
// On host we store REAL pointers per segment (populated as assets load) and translate directly
// — no N64 KSEG0 (PHYS_TO_K0) mapping.
unsigned long long gSegments[16];

static void* Gdx_ResolvePortAddress(uintptr_t addr) {
    static int resolveLogs = 0;
    unsigned long long wideAddr = (unsigned long long)addr;
    unsigned int raw;
    unsigned int assetOffset = 0;
    void* assetBase;
    void* registered;
    void* moduleHost;

    if (addr == 0) {
        return NULL;
    }

    /* If a PORT call path already preserved a full host pointer, keep it.  The
       low32 reconstruction below is only for legacy u32 paths such as
       osVirtualToPhysical() and display-list command words. */
    if (wideAddr > 0xFFFFFFFFULL) {
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("full", addr, (void*)addr);
        }
        return (void*)addr;
    }

    raw = (unsigned int)addr;

    /* LinkStubs.c can only provide a one-byte marker for the original linker
       segment symbol. Segment 2, however, addresses the real host-compiled BSS
       object beginning at D_80225800 (not that marker). Bind the start token
       explicitly so segmented pointers such as 0x02000000 resolve to the
       matrix/context storage they reference. */
    if (raw == (unsigned int)(uintptr_t)SEGMENT_VRAM_START(unk_bss_segment)) {
        void* resolved = &D_80225800;
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("unk-bss", addr, resolved);
        }
        return resolved;
    }

    assetBase = gdx_ensure_asset_segment_for_symbol(raw, &assetOffset);
    if (assetBase != NULL) {
        void* resolved = (u8*)assetBase + assetOffset;
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("asset", addr, resolved);
        }
        return resolved;
    }

    registered = gdx_resolve_registered_host_address(raw);
    if (registered != NULL) {
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("registered", addr, registered);
        }
        return registered;
    }

    moduleHost = gdx_resolve_module_host_address(raw);
    if (moduleHost != NULL) {
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("module", addr, moduleHost);
        }
        return moduleHost;
    }

    if ((raw >= 0x80000000u) && (raw <= 0xBFFFFFFFu)) {
        unsigned int phys = raw & 0x1FFFFFFFu;
        if ((gdx_rdram != NULL) && (phys < GDX_RDRAM_SIZE)) {
            void* resolved = gdx_rdram + phys;
            if (resolveLogs < 12) {
                resolveLogs++;
                gdx_addr_log("kseg-rdram", addr, resolved);
            }
            return resolved;
        }
        return NULL;
    }

    if ((gdx_rdram != NULL) && (raw < GDX_RDRAM_SIZE)) {
        void* resolved = gdx_rdram + raw;
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("rdram", addr, resolved);
        }
        return resolved;
    }

    if ((((raw >> 24) & 0xF) < 16) && (gSegments[(raw >> 24) & 0xF] != 0)) {
        void* resolved = (void*)(gSegments[(raw >> 24) & 0xF] + (raw & 0x00FFFFFFu));
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("segmented", addr, resolved);
        }
        return resolved;
    }

    if (resolveLogs < 12) {
        resolveLogs++;
        gdx_addr_log("fallback-low32", addr, (void*)(unsigned long long)raw);
    }
    return (void*)(unsigned long long)raw;
}

void* gdx_segmented_to_host_pointer(uintptr_t segmentedAddr) {
    return Gdx_ResolvePortAddress(segmentedAddr);
}

uintptr_t Segment_SegmentedToVirtual(uintptr_t segmentedAddr) {
    return (uintptr_t)(unsigned long long)Gdx_ResolvePortAddress(segmentedAddr);
}

uintptr_t Segment_SetPhysicalAddress(s32 segment, uintptr_t addr) {
    /* Deep-audit M3: defensive bounds — gSegments has 16 slots. */
    if ((unsigned)segment >= 16u) {
        return addr;
    }
    void* resolved = Gdx_ResolvePortAddress(addr);
    gSegments[segment] = (unsigned long long)resolved;
    gdx_seg_log("SetPhys", segment, addr, resolved);
    return addr;
}

uintptr_t Segment_SetAddress(s32 segment, uintptr_t addr) {
    void* resolved;
    /* Deep-audit M3: defensive bounds — gSegments has 16 slots. */
    if ((unsigned)segment >= 16u) {
        return addr;
    }
    resolved = Gdx_ResolvePortAddress(addr);
    gSegments[segment] = (unsigned long long)resolved;
    gdx_seg_log("SetAddr", segment, addr, resolved);
    return addr;
}

uintptr_t Segment_GetAddress(s32 segment) {
    return (uintptr_t)gSegments[segment];
}

Gfx* Segment_SetTableAddresses(Gfx* gfx) {
    // Emit one gSPSegment per slot so the converted display list carries correct
    // segment bases into the LUS interpreter's mSegmentPointers[]. The adapter's
    // kOpMoveword handler ignores the truncated 32-bit w1 and reads gSegments[]
    // directly, so the full 64-bit host pointers survive the 8→16-byte conversion.
    for (int i = 0; i < 16; i++) {
        gSPSegment(gfx++, i, gSegments[i]);
    }
    return gfx;
}

void Segment_LoadAssets(void) {
    switch (GET_MODE(gGameMode)) {
        case GAMEMODE_GP_RACE:
        case GAMEMODE_PRACTICE:
        case GAMEMODE_VS_2P:
        case GAMEMODE_VS_3P:
        case GAMEMODE_VS_4P:
        case GAMEMODE_RECORDS:
        case GAMEMODE_COURSE_EDIT:
        case GAMEMODE_TIME_ATTACK:
        case GAMEMODE_GP_END_CS:
        case GAMEMODE_DEATH_RACE:
            if (!gdx_load_venue_texture_segment(COURSE_CONTEXT()->courseData.venue)) {
                gdx_ck("[segment] venue texture segment load failed");
            }
            /* Race-diagnostics gate: set here (mode-aware) rather than inside
               the venue loader, which the course-select preview also calls. */
            {
                extern int gGdxRaceActive;
                gGdxRaceActive = 1;
            }
            break;
        default:
            break;
    }
}

/*
 * The original Segment_LoadOverlays() also prepared per-mode graphics memory:
 * its tail runs Segment_SetupSegment4/7/9/10/5 and the Segment_LoadSegment*
 * content DMAs (decomp/src/sys/segment.c, excluded from the port build).
 * Dropping that chain left segments 4 and 7 permanently unset -- every element
 * drawn through them (countdown faces, start arc, race HUD overlays via
 * hud_gfx on segment 4; machine-part graphics via machine_global_gfx on
 * segment 7; create_machine_textures in Create Machine) was missing or read a
 * garbage base. The buffers are carved in sys_gfx.c's PORT block
 * (gSegment1B8550 = segment 4, gSegment1E23F0 = segment 7); this fills them
 * from the ROM image and points the segment table at them, mirroring the
 * console per-mode switch. Loads are synchronous -- Dma_LoadAssets is a
 * memcpy from gdx_rom_buffer on the port, so the console's async split is
 * unnecessary.
 */
extern uintptr_t gSegment1B8550VramStart;
extern uintptr_t gSegment1E23F0VramStart;
extern uintptr_t gSegment22B0A0VramStart;
extern uintptr_t gSegment22B0A0VramEnd;
extern uintptr_t gGdxMachineModelsVramStart;
extern uintptr_t gGdxMachineModelsVramEnd;
extern uintptr_t gGdxCourseEditTexturesVramStart;
extern uintptr_t gGdxCourseEditTexturesVramEnd;

/* Deep-audit / Phase 4 hitch fix: gSegment1B8550 (seg4) and gSegment1E23F0 (seg7) are two FIXED
   host buffers reused across every mode transition -- only their CONTENT rotates among a small,
   fixed set of ROM assets (hud_gfx/create_machine_textures for seg4; machine_global_gfx/
   expansion_kit_textures_beta for seg7). Profiling (`[transition] GMI_A..GMI_B (Controller_Reset +
   Segment_LoadOverlays) took 131.02ms`, vs. 5-20ms for every other mode-change step) traced to
   Dma_LoadAssets's cooperative yield (decomp/src/sys/dma.c, every 32KB) round-tripping through a
   full vsync-locked host frame each time it fires (port/n64_sched.c's gdx_yield ->
   SwitchToFiber(sHostFiber) -> main.cpp's frame loop presents before control returns). A few
   hundred KB reload costs ~8 frames at ~16.6ms == the measured 131ms, even though the bytes being
   copied are frequently IDENTICAL to what is already resident (e.g. race -> race retry, or any
   race-class mode -> another race-class mode all load the exact same hud_gfx/machine_global_gfx).
   Skipping the DMA when the requested variant already matches what's resident in the buffer is
   behaviourally identical (same buffer, same bytes) and collapses same-variant transitions to the
   cheap Segment_SetAddress-only path the `default:` case already took. */
typedef enum { GDX_SEG4_CONTENT_NONE, GDX_SEG4_CONTENT_HUD_GFX, GDX_SEG4_CONTENT_CREATE_MACHINE } GdxSeg4Content;
typedef enum { GDX_SEG7_CONTENT_NONE, GDX_SEG7_CONTENT_MACHINE_GLOBAL, GDX_SEG7_CONTENT_EK_TEXTURES } GdxSeg7Content;
typedef enum {
    GDX_SEG9_CONTENT_NONE,
    GDX_SEG9_CONTENT_MACHINE_MODELS,
    GDX_SEG9_CONTENT_COURSE_EDIT
} GdxSeg9Content;
static GdxSeg4Content sGdxSeg4Resident = GDX_SEG4_CONTENT_NONE;
static GdxSeg7Content sGdxSeg7Resident = GDX_SEG7_CONTENT_NONE;
static GdxSeg9Content sGdxSeg9Resident = GDX_SEG9_CONTENT_NONE;
static GdxSeg9Content sGdxSeg9Active = GDX_SEG9_CONTENT_NONE;
static size_t sGdxSeg9ActiveSize = 0;

/* Carve byte-order pass (2026-07-11, served-copy family): the carves these
   loaders fill are what gSegments[4]/[7] serve at draw time, but only the
   bridge's separate heap images ever received the generated fixups. Seg-8's
   identical fix put the start arc and Nintex boards on screen; the same
   disease here left the pause-menu TLUT-setup DL (seg-4 carve, BE garbage ->
   palette mode never enabled -> striped text) and the countdown faces / arc
   screens (seg-7 machine_global DLs) broken. Texture regions are not in the
   fixup tables, so already-working texture consumers are unaffected; calls
   with images that have no fixup entries (create_machine, EK textures) are
   no-ops by construction. */
extern void gdx_fixup_asset_segment_image(unsigned char segment, unsigned int rom_base,
                                           unsigned char* data, unsigned int size);
extern void gdx_register_asset_segment_command_ranges(unsigned char segment, unsigned int rom_base,
                                                       unsigned char* data, unsigned int size);
#ifdef EXPANSION_KIT
extern unsigned char* gdx_disk_buffer;
extern unsigned int gdx_disk_size;
extern unsigned int gdx_ek_segment_image_size(unsigned char segment);
extern int gdx_ek_segment_image_fill(unsigned char segment, const unsigned char* disk,
                                     unsigned long long diskSize, unsigned char* dest,
                                     unsigned int capacity);
#endif

static void gdx_load_seg4_if_needed(GdxSeg4Content want, unsigned char* romStart, size_t size,
                                     const char* label) {
    if (sGdxSeg4Resident != want) {
        Dma_LoadAssets(romStart, osPhysicalToVirtual(gSegment1B8550VramStart), size);
        gdx_fixup_asset_segment_image(0x04u,
                                      (want == GDX_SEG4_CONTENT_HUD_GFX)
                                          ? (unsigned int) PORT_hud_gfx_ROM_START
                                          : (unsigned int) PORT_create_machine_textures_ROM_START,
                                      (unsigned char*) osPhysicalToVirtual(gSegment1B8550VramStart),
                                      (unsigned int) size);
        sGdxSeg4Resident = want;
        gdx_ck(label); // "[transition] seg4 reload: <variant>"
    } else {
        gdx_ck("[transition] seg4 reload skipped (already resident)");
    }
    Segment_SetAddress(4, gSegment1B8550VramStart);
}

static void gdx_load_seg7_if_needed(GdxSeg7Content want, unsigned char* romStart, size_t size,
                                     const char* label) {
    if (sGdxSeg7Resident != want) {
        Dma_LoadAssets(romStart, osPhysicalToVirtual(gSegment1E23F0VramStart), size);
        gdx_fixup_asset_segment_image(0x07u,
                                      (want == GDX_SEG7_CONTENT_MACHINE_GLOBAL)
                                          ? (unsigned int) PORT_machine_global_gfx_ROM_START
                                          : (unsigned int) PORT_expansion_kit_textures_beta_ROM_START,
                                      (unsigned char*) osPhysicalToVirtual(gSegment1E23F0VramStart),
                                      (unsigned int) size);
        sGdxSeg7Resident = want;
        gdx_ck(label); // "[transition] seg7 reload: <variant>"
    } else {
        gdx_ck("[transition] seg7 reload skipped (already resident)");
    }
    Segment_SetAddress(7, gSegment1E23F0VramStart);
}

/* Segment 9 is mode-owned on the original game: decoded cartridge
 * machine_models for Create Machine and the machine-settings/cutscene modes,
 * but disk-resident course_edit_textures for Course Edit. The console loader
 * that performed this switch is excluded from the port build, and treating all
 * 0x09xxxxxx tokens as globally interchangeable makes the two layouts collide.
 * Keep the ownership explicit and expose a narrow resolver hook so the graphics
 * bridge can prefer the active image before its global generated-asset ranges. */
static int gdx_activate_machine_models_segment9(void) {
    unsigned char* dest = (unsigned char*)osPhysicalToVirtual(gGdxMachineModelsVramStart);
    size_t capacity = (size_t)(gGdxMachineModelsVramEnd - gGdxMachineModelsVramStart);
    const size_t romStart = (size_t)PORT_machine_models_ROM_START;

    if (dest == NULL || capacity == 0 || gdx_rom_buffer == NULL || romStart + 8u > gdx_rom_size ||
        gdx_rom_buffer[romStart + 0] != 'M' || gdx_rom_buffer[romStart + 1] != 'I' ||
        gdx_rom_buffer[romStart + 2] != 'O' || gdx_rom_buffer[romStart + 3] != '0') {
        gdx_ck("[segment] segment 9 machine_models source/capacity invalid");
        return 0;
    }

    if (sGdxSeg9Resident != GDX_SEG9_CONTENT_MACHINE_MODELS) {
        /* Codebase-audit P3: the MIO0 header's decoded size (big-endian u32 at +4)
         * was previously trusted implicitly; a corrupt ROM could decompress past the
         * RDRAM carve. Check it against the carve capacity before decoding (the EK
         * disk path below already does the equivalent required>capacity check). */
        {
            unsigned int decodedSize = ((unsigned int)gdx_rom_buffer[romStart + 4] << 24) |
                                       ((unsigned int)gdx_rom_buffer[romStart + 5] << 16) |
                                       ((unsigned int)gdx_rom_buffer[romStart + 6] << 8) |
                                       (unsigned int)gdx_rom_buffer[romStart + 7];
            if (decodedSize > capacity) {
                gdx_ck("[segment] segment 9 machine_models MIO0 decoded size exceeds carve capacity");
                return 0;
            }
        }
        mio0Decode(gdx_rom_buffer + romStart, dest);
        gdx_fixup_asset_segment_image(0x09u, (unsigned int)PORT_machine_models_ROM_START,
                                      dest, (unsigned int)capacity);
        gdx_register_asset_segment_command_ranges(0x09u,
                                                   (unsigned int)PORT_machine_models_ROM_START,
                                                   dest, (unsigned int)capacity);
        sGdxSeg9Resident = GDX_SEG9_CONTENT_MACHINE_MODELS;
        gdx_ck("[transition] seg9 reload: machine_models");
    } else {
        gdx_ck("[transition] seg9 reload skipped (machine_models resident)");
    }

    gSegment22B0A0VramStart = gGdxMachineModelsVramStart;
    gSegment22B0A0VramEnd = gGdxMachineModelsVramEnd;
    Segment_SetAddress(9, gSegment22B0A0VramStart);
    sGdxSeg9Active = GDX_SEG9_CONTENT_MACHINE_MODELS;
    sGdxSeg9ActiveSize = capacity;
    return 1;
}

#ifdef EXPANSION_KIT
static int gdx_activate_course_edit_segment9(void) {
    unsigned char* dest = (unsigned char*)osPhysicalToVirtual(gGdxCourseEditTexturesVramStart);
    size_t capacity = (size_t)(gGdxCourseEditTexturesVramEnd - gGdxCourseEditTexturesVramStart);
    size_t required = (size_t)gdx_ek_segment_image_size(9u);

    if (dest == NULL || capacity == 0 || required == 0 || required > capacity ||
        !gdx_ek_segment_image_fill(9u, gdx_disk_buffer, (unsigned long long)gdx_disk_size,
                                   dest, (unsigned int)capacity)) {
        gdx_ck("[segment] segment 9 course_edit_textures fill failed");
        return 0;
    }

    if (sGdxSeg9Resident != GDX_SEG9_CONTENT_COURSE_EDIT) {
        gdx_ck("[transition] seg9 reload: course_edit_textures");
    } else {
        gdx_ck("[transition] seg9 reload: course_edit_textures refreshed");
    }
    sGdxSeg9Resident = GDX_SEG9_CONTENT_COURSE_EDIT;
    gSegment22B0A0VramStart = gGdxCourseEditTexturesVramStart;
    gSegment22B0A0VramEnd = gGdxCourseEditTexturesVramEnd;
    Segment_SetAddress(9, gSegment22B0A0VramStart);
    sGdxSeg9Active = GDX_SEG9_CONTENT_COURSE_EDIT;
    sGdxSeg9ActiveSize = required;
    return 1;
}
#endif

static void gdx_load_segment9_for_mode(void) {
    int loaded = 0;

    switch (gGameMode) {
        case GAMEMODE_CREATE_MACHINE:
        case GAMEMODE_GP_END_CS:
        case GAMEMODE_LX_MACHINE_SETTINGS:
        case GAMEMODE_LX_GP_RACE_NEXT_MACHINE_SETTINGS:
            loaded = gdx_activate_machine_models_segment9();
            break;
#ifdef EXPANSION_KIT
        case GAMEMODE_COURSE_EDIT:
            loaded = gdx_activate_course_edit_segment9();
            break;
#endif
        default:
            break;
    }

    if (!loaded) {
        /* Match the console default path: keep the last segment address/content
         * resident, but do not make it authoritative outside a mode that owns
         * segment 9. */
        sGdxSeg9Active = GDX_SEG9_CONTENT_NONE;
        sGdxSeg9ActiveSize = 0;
        Segment_SetAddress(9, gSegment22B0A0VramStart);
    }
}

int gdx_resolve_mode_segment9(unsigned int raw, size_t requiredBytes, uintptr_t* outAddress) {
    uintptr_t hostBase;
    size_t offset;

    if (outAddress == NULL || sGdxSeg9Active == GDX_SEG9_CONTENT_NONE) {
        return 0;
    }

    hostBase = (uintptr_t)osPhysicalToVirtual(gSegment22B0A0VramStart);
    if ((raw >> 24) == 9u) {
        offset = (size_t)(raw & 0x00FFFFFFu);
    } else {
        /* Course Edit stores some pointers after C's 64-bit host address has
         * passed through an N64-sized command word.  Resolve that truncation
         * only inside the exact buffer owned by the active segment-9 mode;
         * never reconstruct arbitrary process pointers from their high bits. */
        offset = (size_t)(unsigned int)(raw - (unsigned int)hostBase);
    }

    if (offset > sGdxSeg9ActiveSize || requiredBytes > sGdxSeg9ActiveSize - offset) {
        return 0;
    }

    *outAddress = hostBase + offset;
    return 1;
}

static void gdx_load_mode_segments(void) {
    extern void gdx_cki(const char*, int);
    size_t hudSize = (size_t)(PORT_hud_gfx_ROM_END - PORT_hud_gfx_ROM_START);
    size_t createMachineSize =
        (size_t)(PORT_create_machine_textures_ROM_END - PORT_create_machine_textures_ROM_START);
    size_t machineGlobalSize =
        (size_t)(PORT_machine_global_gfx_ROM_END - PORT_machine_global_gfx_ROM_START);
    size_t ekTexturesSize =
        (size_t)(PORT_expansion_kit_textures_beta_ROM_END - PORT_expansion_kit_textures_beta_ROM_START);
    size_t seg7EkSize = (ekTexturesSize <= machineGlobalSize) ? ekTexturesSize : machineGlobalSize;
    static int sModeSegLogs = 0;

    switch (GET_MODE(gGameMode)) {
        case GAMEMODE_GP_RACE:
        case GAMEMODE_PRACTICE:
        case GAMEMODE_VS_2P:
        case GAMEMODE_VS_3P:
        case GAMEMODE_VS_4P:
        case GAMEMODE_TIME_ATTACK:
        case GAMEMODE_GP_END_CS:
        case GAMEMODE_DEATH_RACE:
            /* Races: hud_gfx on segment 4, machine_global_gfx on segment 7. */
            gdx_load_seg4_if_needed(GDX_SEG4_CONTENT_HUD_GFX, SEGMENT_ROM_START(hud_gfx), hudSize,
                                     "[transition] seg4 reload: hud_gfx");
            gdx_load_seg7_if_needed(GDX_SEG7_CONTENT_MACHINE_GLOBAL, SEGMENT_ROM_START(machine_global_gfx),
                                     machineGlobalSize, "[transition] seg7 reload: machine_global_gfx");
            break;

        case GAMEMODE_CREATE_MACHINE:
            /* Create Machine: its texture bank replaces hud_gfx on segment 4;
               segment 7 carries the EK texture set. */
            gdx_load_seg4_if_needed(GDX_SEG4_CONTENT_CREATE_MACHINE, SEGMENT_ROM_START(create_machine_textures),
                                     createMachineSize, "[transition] seg4 reload: create_machine_textures");
            gdx_load_seg7_if_needed(GDX_SEG7_CONTENT_EK_TEXTURES, SEGMENT_ROM_START(expansion_kit_textures_beta),
                                     seg7EkSize, "[transition] seg7 reload: expansion_kit_textures_beta");
            break;

        case GAMEMODE_COURSE_EDIT:
            /* Course Edit (EK): hud_gfx on segment 4, EK textures on 7. */
            gdx_load_seg4_if_needed(GDX_SEG4_CONTENT_HUD_GFX, SEGMENT_ROM_START(hud_gfx), hudSize,
                                     "[transition] seg4 reload: hud_gfx");
            gdx_load_seg7_if_needed(GDX_SEG7_CONTENT_EK_TEXTURES, SEGMENT_ROM_START(expansion_kit_textures_beta),
                                     seg7EkSize, "[transition] seg7 reload: expansion_kit_textures_beta");
            break;

        default:
            /* Console behavior for menus/records/machine-select: segment 4
               keeps pointing at the existing buffer (contents persist from
               the previous mode). */
            Segment_SetAddress(4, gSegment1B8550VramStart);
            break;
    }

    gdx_load_segment9_for_mode();

    if (sModeSegLogs < 12) {
        sModeSegLogs++;
        gdx_cki("[segment] mode segments loaded for gameMode", (int)GET_MODE(gGameMode));
    }
}

void Segment_LoadOverlays(void) {
#ifdef EXPANSION_KIT
    if (GET_MODE(gGameMode) == GAMEMODE_COURSE_EDIT) {
        const size_t workBufferSize = 2 * sizeof(unk_80128C94);
        size_t i;

        if (D_80128C90 == NULL) {
            D_80128C90 = (unk_80128C94*)Arena_Allocate(
                ALLOC_FRONT, workBufferSize);
        }
        D_80128C94 = D_80128C90;
        if (D_80128C90 == NULL) {
            gdx_ck("[segment] FATAL: Course Edit graphics allocation failed");
            gdx_host_abort();
        }
        for (i = 0; i < workBufferSize; i++) {
            ((u8*)D_80128C90)[i] = 0;
        }
        /* This scratch allocation is in RDRAM, but its two Gfx subarrays are
         * written by recompiled PORT code and therefore use 16-byte host Gfx
         * packets rather than the original 8-byte N64 layout. */
        gdx_register_host_wide_command_range(D_80128C90, workBufferSize);
    }
#endif
    gdx_load_mode_segments();
}

// ---- Save system -------------------------------------------------------------
// Save_LoadStaffGhostRecord and Save_SaveSettingsProfiles are real now: they're
// defined in decomp/src/overlays/ovl_i2/save.c, which now compiles (save-system
// slice). Save_LoadStaffGhostRecord still returns -1 on PORT (see the #ifdef PORT
// guard in save.c) -- that piece needs the EK ROM segment table, a separate slice.

// ---- Graphics pool ---------------------------------------------------------
// D_1000000: the N64 graphics pool (segment 0x01) — a real runtime buffer (NOT an o2r asset),
// so display-list/matrix allocations have somewhere to live.
// (aVp* viewports and D_80149A0 are real assets now provided by the R2 asset bindings.)
GfxPool D_1000000;
