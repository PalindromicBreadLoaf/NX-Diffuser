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
/* Printf-style sibling of gdx_ck, same stdio.h-avoidance reason (defined in
   n64_sched.c). Unlike gdx_ck/gdx_cki it is NOT gated behind GDX_TRACE -- it
   always reaches stderr/OutputDebugString and additionally persists to
   gdiffuser-run.log whenever GDX_LOG (or another diagnostic env var) is set
   (see port_log.h's gdx_log_file_enabled). Used below for the segment-9
   activation diagnostic (Course Edit node-info panel scatter investigation). */
extern void gdx_dbg_logf(const char* fmt, ...);
extern void* gdx_host_calloc(size_t count, size_t size);
extern void  gdx_host_exit(int status);
extern void  gdx_host_abort(void);
extern void* gdx_resolve_registered_host_address(unsigned int addr);
extern void* gdx_resolve_module_host_address(unsigned int addr);
extern void* gdx_ensure_asset_segment_for_symbol(unsigned int symLow32, unsigned int* outOffset);
extern int   gdx_load_venue_texture_segment(int venue);
extern s32   gGameMode;
/* Declared locally (not via <stdlib.h>, which -- like <stdio.h> above -- clashes
   with the decomp's libc/stdint.h) so the [seg9diag] gate below can read GDX_LOG
   without pulling in the standard header. */
extern char* getenv(const char* name);

extern unsigned char* gdx_rom_buffer;
extern size_t gdx_rom_size;
/* R1 (C-R1.3): single archive-first byte-source shim. The seg-9 machine_models
 * and seg-5 podium loaders below stage their compressed MIO0 span through this
 * instead of reading gdx_rom_buffer directly; it returns verbatim ROM bytes, so
 * mio0Decode sees byte-identical input. See port/gdx_segment_source.{h,c}. */
extern int GdxSegmentSourceRead(unsigned int romBase, unsigned int size, void* dst);
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
// A1/A2: same "known compiled-in host array" registration E3 uses for EK disk
// assets (see its comment in n64_gfx_bridge.cpp), reused here for base-game
// arrays. gdx_register_host_pointer_stub only affects RECOGNITION (silences the
// "[stub-miss] ... taken verbatim" census for a legitimate module-resident
// array); gdx_set_native_rgba16_texture_range additionally marks a range as
// host-endian so the SETTIMG wide-pointer path byte-swaps it into a persistent
// copy before Fast3D reads it (same mechanism transition.c uses for
// sTransitionPalette/backgroundBuffer).
extern void gdx_register_host_pointer_stub(void* dest, size_t size);
extern void gdx_set_native_rgba16_texture_range(void* ptr, size_t size, int enabled);
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
    // Venue material texture banks (road/wall/pipe/cylinder). course.c's
    // TRACK_SHAPE_* material tables (D_800CF528 / D_800CF608 / D_800CF668 / ...)
    // store native pointers to these 1-byte .bss placeholder symbols
    // (port/gen/LinkStubs.c). At draw time the gfx bridge re-routes each to its
    // decoded per-venue segment image via ResolveVenueBankAlias, but only after
    // the pointer is recognized as a known host range. On Windows the module-range
    // reconstruction happened to cover this .bss tail; on Linux PIE it does not
    // reach it (the same defect proven for gAudioHeap above), so the banks read as
    // the raw zero stub byte and the track floor/walls/pipes rendered solid black.
    // Register each bank explicitly so resolution is platform-independent. The
    // symbols are consecutive in .bss only by linker accident and LinkStubs.c
    // exposes no begin/end markers, so register each at its true 1-byte size rather
    // than assuming a contiguous span. This is exactly the set course.c references
    // and that ResolveVenueBankAlias enumerates (D_A000000..D_A008000).
    {
        extern unsigned char D_A000000[];
        extern unsigned char D_A001000[];
        extern unsigned char D_A002000[];
        extern unsigned char D_A003000[];
        extern unsigned char D_A004000[];
        extern unsigned char D_A005000[];
        extern unsigned char D_A006000[];
        extern unsigned char D_A007000[];
        extern unsigned char D_A008000[];
        gdx_register_host_range(D_A000000, 1);
        gdx_register_host_range(D_A001000, 1);
        gdx_register_host_range(D_A002000, 1);
        gdx_register_host_range(D_A003000, 1);
        gdx_register_host_range(D_A004000, 1);
        gdx_register_host_range(D_A005000, 1);
        gdx_register_host_range(D_A006000, 1);
        gdx_register_host_range(D_A007000, 1);
        gdx_register_host_range(D_A008000, 1);
    }
    // E4 (A3 follow-up): banks 9-11 (D_A009000..D_A00B000, decomp's
    // fzx_segmentA.h:15-17) -- same 1-byte-LinkStubs-on-Linux-PIE registration
    // as banks 0-8 above, extending the range n64_gfx_bridge.cpp's kBankLow32[]
    // now also covers. D_A009000/D_A00A000 are unreferenced placeholders (see
    // tools/gen_link_stubs.py's EXTRA_DATA_SYMS) always linked via LinkStubs.c;
    // D_A00B000 is live (gRoadTypeMenuItems, decomp/src/overlays/expansion_kit/
    // A3AE0.c:534-544) but only exists in the EK-only port/gen/EkLinkStubs.c, so
    // it must stay behind EXPANSION_KIT like its bridge-side extern declaration.
    {
        extern unsigned char D_A009000[];
        extern unsigned char D_A00A000[];
        gdx_register_host_range(D_A009000, 1);
        gdx_register_host_range(D_A00A000, 1);
    }
#ifdef EXPANSION_KIT
    {
        extern unsigned char D_A00B000[];
        gdx_register_host_range(D_A00B000, 1);
    }
#endif

    // A1 (podium/ending fireworks invisible): EndingCutsceneEffects_DrawFireworks
    // (decomp/src/overlays/ending/ending_effects.c) SETTIMGs these three compiled-in
    // u16[64] sparkle textures directly -- real, full-size host arrays, not
    // LinkStubs placeholders, so ResolveWideAssetStubPointer miscounted them as
    // unbound stubs ("[stub-miss] ... taken verbatim"). "Verbatim" was also
    // pixel-wrong: their u16 literals (e.g. 0xFFFE/0xFFFF) are compiled at their
    // native (host, little-endian) byte order, but Fast3D's RGBA16 texture reader
    // wants a big-endian byte stream -- the same endianness class as the
    // sTransitionPalette/backgroundBuffer bug transition.c already works around.
    // gdx_set_native_rgba16_texture_range marks each array so the SETTIMG wide-
    // pointer path byte-swaps it into a persistent copy before Fast3D samples it
    // (this is the actual pixel fix); gdx_register_host_pointer_stub additionally
    // marks it as a recognized identity so the stub-miss census stops naming it.
    // Sizes are 64 elements * sizeof(u16) = 128 bytes each (counted from the
    // array initializers in ending_effects.c).
    {
        extern u16 D_i7_8014ADA8[];
        extern u16 D_i7_8014AE30[];
        extern u16 D_i7_8014AEB8[];
        gdx_set_native_rgba16_texture_range(D_i7_8014ADA8, 128u, 1);
        gdx_set_native_rgba16_texture_range(D_i7_8014AE30, 128u, 1);
        gdx_set_native_rgba16_texture_range(D_i7_8014AEB8, 128u, 1);
        gdx_register_host_pointer_stub(D_i7_8014ADA8, 128u);
        gdx_register_host_pointer_stub(D_i7_8014AE30, 128u);
        gdx_register_host_pointer_stub(D_i7_8014AEB8, 128u);
    }

    // A2 (sCourseMinimapPalette stub-miss at venue load): this IS the same TLUT
    // minimap.c already pre-swaps under #ifdef PORT (MINIMAP_TLUT_ENTRY), so its
    // compiled bytes are already big-endian-correct -- unlike the fireworks
    // arrays above, it must NOT go through gdx_set_native_rgba16_texture_range
    // (that would re-swap already-correct bytes back to broken). Register it as a
    // recognized identity only, purely to silence the stub-miss census entry this
    // legitimate, sampled-every-frame array otherwise produces. 4 entries *
    // sizeof(u16) = 8 bytes (CLEAR/BLACK/WHITE/GREY, minimap.c).
    {
        extern u16 sCourseMinimapPalette[];
        gdx_register_host_pointer_stub(sCourseMinimapPalette, 8u);
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

// R6-P2 ground truth: the hand-copied kGfxPoolSize constant in port/gdx_interp.cpp cannot be
// checked at parity 0 (its self-check degenerates to a tautology there — see PrevPoolBase). This
// TU compiles WITH the real decomp GfxPool type, so it is the one place that can answer "how big
// IS GfxPool, really" without guessing. gdx_interp verifies this ONCE against its constant.
size_t gdx_gfxpool_sizeof(void) {
    return sizeof(GfxPool);
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
/* libultraship export (interpreter.cpp): drops every entry in the Fast3D texture
   cache so the next upload re-decodes from CPU memory. Same extern approach as
   port/gdx_workshop.cpp's hot-reload caller. */
extern void gfx_texture_cache_clear(void);
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

    /* Belt-and-suspenders for the whole "stale GPU texture at a reused arena
       address" class (minimap outline, decoded glyphs, decoded UI): the Fast3D
       texture cache keys some formats by address alone, so a rewind that re-hands
       an address to different content would keep serving the previous upload.
       A full clear here forces every subsequent upload to re-decode. Mode
       transitions are infrequent, so the cost is negligible. This complements the
       format-specific content-hash work done in the interpreter.
       Guarded by timing rather than a per-call interpreter null-check (a C TU
       cannot inspect the C++ interpreter instance): this rewind branch runs only
       after the baseline call above, i.e. on a real game-mode transition, by which
       point the renderer is live — the same call-site-timing guarantee that
       gdx_workshop.cpp's hot-reload caller relies on. */
    gfx_texture_cache_clear();
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
    /* R4 (C-R4.1): single chokepoint for ~27 ROM_READ sites (racer.c/machine_draw.c/
     * E7CF0.c). Read 4 bytes via the byte-source shim (archive-first, byte-identical
     * raw fallback) and assemble big-endian exactly as before. On a total miss the
     * shim returns 0 and leaves tmp untouched, so we return 0 -- the same value the
     * old NULL/OOB guard returned. */
    size_t off = Gdx_RomOffset(addr);
    u8 tmp[4];
    if (!GdxSegmentSourceRead((unsigned int)off, (unsigned int)sizeof(tmp), tmp)) {
        return 0;
    }

    return ((u32)tmp[0] << 24) |
           ((u32)tmp[1] << 16) |
           ((u32)tmp[2] << 8)  |
           ((u32)tmp[3] << 0);
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
/* Segment 5 (podium_gfx) carve markers. Declared in sys_gfx.c but NEVER assigned
 * a real backing buffer in that file's PORT block (unlike seg 4/7/9), so gSegments[5]
 * stayed null on the port -- the GP-ending seg-5 DL drop (see gdx_activate_podium_segment5). */
extern uintptr_t gSegment2738A0VramStart;
extern uintptr_t gSegment2738A0VramEnd;

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
/* Segment-reload TOCTOU epoch (defined in n64_gfx_bridge.cpp). The graphics
   thread reads gSegments[] and the seg-4/7/9 carve bytes with no lock while a
   mode transition rewrites them here. Bracketing the reload with begin()/end()
   makes the shared seqlock counter ODD for the duration, so a racing
   graphics-thread resolution detects the window and skips the affected texture
   for that one frame instead of consuming torn state (the Create Machine entry
   strlen crash). See the epoch comment block in n64_gfx_bridge.cpp. */
extern void gdx_segment_epoch_begin(void);
extern void gdx_segment_epoch_end(void);
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
/* Staging buffer for the compressed seg-9 MIO0 span, filled by the byte-source
 * shim. Sized to the full ROM span [ROM_START, ROM_END) so the entire MIO0
 * stream is present before decoding. Game-thread only (mode transitions are
 * sequential), so no additional guard is needed here. */
static unsigned char sGdxSeg9Stage[PORT_machine_models_ROM_END - PORT_machine_models_ROM_START];

static int gdx_activate_machine_models_segment9(void) {
    unsigned char* dest = (unsigned char*)osPhysicalToVirtual(gGdxMachineModelsVramStart);
    size_t capacity = (size_t)(gGdxMachineModelsVramEnd - gGdxMachineModelsVramStart);
    const size_t romStart = (size_t)PORT_machine_models_ROM_START;
    const unsigned int span = (unsigned int)(PORT_machine_models_ROM_END - PORT_machine_models_ROM_START);

    /* Stage the compressed span through the shim (archive-first, raw fallback),
     * then probe the staged bytes -- byte-identical to the old direct
     * gdx_rom_buffer probe/decode. A source miss trips the same invalid path. */
    if (dest == NULL || capacity == 0 ||
        !GdxSegmentSourceRead((unsigned int)romStart, span, sGdxSeg9Stage) ||
        sGdxSeg9Stage[0] != 'M' || sGdxSeg9Stage[1] != 'I' ||
        sGdxSeg9Stage[2] != 'O' || sGdxSeg9Stage[3] != '0') {
        gdx_ck("[segment] segment 9 machine_models source/capacity invalid");
        return 0;
    }

    if (sGdxSeg9Resident != GDX_SEG9_CONTENT_MACHINE_MODELS) {
        /* Codebase-audit P3: the MIO0 header's decoded size (big-endian u32 at +4)
         * was previously trusted implicitly; a corrupt ROM could decompress past the
         * RDRAM carve. Check it against the carve capacity before decoding (the EK
         * disk path below already does the equivalent required>capacity check). */
        {
            unsigned int decodedSize = ((unsigned int)sGdxSeg9Stage[4] << 24) |
                                       ((unsigned int)sGdxSeg9Stage[5] << 16) |
                                       ((unsigned int)sGdxSeg9Stage[6] << 8) |
                                       (unsigned int)sGdxSeg9Stage[7];
            if (decodedSize > capacity) {
                gdx_ck("[segment] segment 9 machine_models MIO0 decoded size exceeds carve capacity");
                return 0;
            }
        }
        mio0Decode(sGdxSeg9Stage, dest);
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
    /* Segment-9 ceremony diagnostic: GP-ending cutscenes (GAMEMODE_GP_END_CS)
       could silently drop vehicle models when this activation failed. Every
       failure branch above already logs via gdx_ck; add the success line here so a
       GP-ending run shows that machine_models was actually bound and for which
       game mode. */
    {
        extern void gdx_cki(const char*, int);
        gdx_cki("[segment] seg9 active=machine_models size", (int)sGdxSeg9ActiveSize);
        gdx_cki("[segment] seg9 gameMode", (int)GET_MODE(gGameMode));
    }
    return 1;
}

/* [seg9diag] instrumentation gate: gdx_dbg_logf (used throughout this block)
 * always reaches stderr/OutputDebugString regardless of env vars -- unlike
 * gdx_ck/gdx_cki it has no built-in opt-out -- so without this gate every
 * [seg9diag] line below would spam stderr/OutputDebugString on a normal run
 * even with no diagnostics requested. GDX_LOG opt-in, lazily cached (same
 * sentinel pattern as port_log.h's gdx_log_file_enabled; this TU cannot
 * include port_log.h, see the top-of-file comment). */
static int gdx_seg9diag_enabled(void) {
    static int sCached = -1;
    if (sCached < 0) {
        const char* log = getenv("GDX_LOG");
        sCached = (log != NULL && log[0] != '\0' && log[0] != '0') ? 1 : 0;
    }
    return sCached;
}

#ifdef EXPANSION_KIT
/* Task 3 diag (Course Edit node-info panel still blank at runtime, [nodeinfo]
 * scatter investigation): entry/state/result tracing for the seg-9 activation
 * this game mode depends on. Uses gdx_dbg_logf (always reaches stderr/
 * OutputDebugString; persists to gdiffuser-run.log when GDX_LOG is set -- see
 * the extern declaration above), NOT gdx_ck/gdx_cki, so these lines are not
 * silently dropped when GDX_TRACE is unset. Low-noise: activation only runs
 * on a mode transition into Course Edit, not per frame. Gated on
 * gdx_seg9diag_enabled() (GDX_LOG) so a normal run stays silent.
 *
 * The fill call is pulled out of the original single OR-chained `if` into an
 * explicit `fillOk` local so its result can be logged -- the precheck
 * (dest/capacity/required) still short-circuits BEFORE the fill is attempted,
 * exactly as the original expression did; no behavioral change. */
static int gdx_activate_course_edit_segment9(void) {
    unsigned char* dest = (unsigned char*)osPhysicalToVirtual(gGdxCourseEditTexturesVramStart);
    size_t capacity = (size_t)(gGdxCourseEditTexturesVramEnd - gGdxCourseEditTexturesVramStart);
    size_t required = (size_t)gdx_ek_segment_image_size(9u);
    int precheckFailed = (dest == NULL || capacity == 0 || required == 0 || required > capacity);
    int fillOk = 0;
    const int diagEnabled = gdx_seg9diag_enabled();

    if (diagEnabled) {
        gdx_dbg_logf("[seg9diag] activate_course_edit entry gameMode=%d diskBuffer=%p diskSize=%u dest=%p "
                     "capacity=%u required=%u\n",
                     (int)gGameMode, (void*)gdx_disk_buffer, (unsigned int)gdx_disk_size, (void*)dest,
                     (unsigned int)capacity, (unsigned int)required);
    }

    if (!precheckFailed) {
        fillOk = gdx_ek_segment_image_fill(9u, gdx_disk_buffer, (unsigned long long)gdx_disk_size, dest,
                                           (unsigned int)capacity);
    }
    if (diagEnabled) {
        gdx_dbg_logf("[seg9diag] activate_course_edit fill precheckFailed=%d fillOk=%d\n", precheckFailed, fillOk);
    }

    if (precheckFailed || !fillOk) {
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
    if (diagEnabled) {
        gdx_dbg_logf("[seg9diag] activate_course_edit SUCCESS Segment_SetAddress(9, 0x%08X) size=%u\n",
                     (unsigned int)gSegment22B0A0VramStart, (unsigned int)sGdxSeg9ActiveSize);
    }
    return 1;
}
#endif

static void gdx_load_segment9_for_mode(void) {
    int loaded = 0;
    /* Task 3 diag: one shared per-dispatch budget for both the entry and exit
     * lines below (first 40 dispatches per boot). Every call here is a
     * mode-transition event, not a per-frame one, so this stays low-noise even
     * unbounded, but the cap matches the "first-N per boot" instruction. Also
     * gated on gdx_seg9diag_enabled() (GDX_LOG) so a normal run stays silent. */
    static int sSeg9DispatchLogs = 0;
    const int diagThisCall = (sSeg9DispatchLogs < 40) && gdx_seg9diag_enabled();
    if (sSeg9DispatchLogs < 40) {
        sSeg9DispatchLogs++;
    }
    if (diagThisCall) {
        gdx_dbg_logf("[seg9diag] load_segment9_for_mode entry gameMode=%d\n", (int)gGameMode);
    }

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

    if (diagThisCall) {
        gdx_dbg_logf("[seg9diag] load_segment9_for_mode exit gameMode=%d loaded=%d activeContent=%d "
                     "Segment_SetAddress(9)=0x%08X\n",
                     (int)gGameMode, loaded, (int)sGdxSeg9Active, (unsigned int)gSegment22B0A0VramStart);
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

/* E2 (editor resolver diagnosis): while a mode owns segment 4/7/9, that live
 * carve is authoritative and the ROM-backed AssetBindings.c table rows for the
 * SAME segment are stale context that must not be treated as a fallback match
 * by the generated-stub lookup in n64_gfx_bridge.cpp (ResolveGeneratedAssetStub,
 * reached from TryResolveAddress). TryResolveAddress already gives
 * gdx_resolve_mode_segment9 first refusal over those rows for segment 9 (see
 * the comment there); this extends the same intent to segments 4/7 and gives
 * the bridge an explicit way to skip a ROM-table hit for any segment currently
 * owned by a mode carve, instead of returning wrong-source bytes. */
int gdx_mode_owns_segment(unsigned int seg) {
    switch (seg) {
        case 4u:
            return sGdxSeg4Resident != GDX_SEG4_CONTENT_NONE;
        case 7u:
            return sGdxSeg7Resident != GDX_SEG7_CONTENT_NONE;
        case 9u:
            return sGdxSeg9Active != GDX_SEG9_CONTENT_NONE;
        default:
            return 0;
    }
}

/* Segment 5 is GP-ending-only ROM data: the podium body meshes (sPodiumDLs,
 * ending.c) and the ending-venue building detail display lists, all reached via
 * 0x05xxxxxx G_DL/segment tokens. The console loaders that owned it
 * (Segment_SetupSegment5 + Segment_LoadSegment5, decomp/src/sys/segment.c) are
 * excluded from the port build, and sys_gfx.c's PORT block carves buffers for
 * segments 4/7/9 but not 5 -- so gSegments[5] was left null and every G_DL into
 * segment 5 resolved to nothing (the ending "[gdl-bad] raw=05xxxxxx first=00000000
 * w0=DE000000" burst): untextured white venue buildings and no podium.
 *
 * podium_gfx is MIO0-compressed in the cartridge ROM (matches the console
 * `if (*(s32*)vram == 'MIO0') mio0Decode(...)` path). Mirror the seg-9
 * machine_models activation: validate the MIO0 header, decode into a persistent
 * RDRAM carve, apply the generated segment fixups + command-range registration
 * (podium_gfx display lists carry 0x05xxxxxx internal pointers), and point
 * gSegments[5] at the decoded image.
 *
 * Lifetime note: unlike the seg 4/7/9 carves (allocated at boot in sys_gfx.c,
 * below the mode-reset baseline, hence rewind-protected), this buffer is carved
 * lazily on the first ending -- AFTER the baseline is captured. gdx_rdram_alloc_raw
 * memory would be rewound by gdx_rdram_mode_reset on the next mode change, so use
 * gdx_rdram_persist_alloc_raw (bumps DOWN from the top of RDRAM, never rewound):
 * the decoded podium image and its in-place fixups survive every mode transition
 * for the whole session, and revisiting the ending only re-points gSegments[5]. */
extern void* gdx_rdram_persist_alloc_raw(size_t size, size_t align);

static unsigned char* sGdxSeg5PodiumBuf = NULL; /* host pointer into gdx_rdram (persist region) */
static size_t sGdxSeg5PodiumSize = 0;
static int sGdxSeg5Resident = 0;
/* Staging buffer for the compressed seg-5 MIO0 span, filled by the byte-source
 * shim (same pattern as seg-9). Sized to the full podium_gfx ROM span. */
static unsigned char sGdxSeg5Stage[PORT_podium_gfx_ROM_END - PORT_podium_gfx_ROM_START];

static int gdx_activate_podium_segment5(void) {
    const size_t romStart = (size_t)PORT_podium_gfx_ROM_START;
    const unsigned int span = (unsigned int)(PORT_podium_gfx_ROM_END - PORT_podium_gfx_ROM_START);
    unsigned int decodedSize;

    /* Stage the compressed span through the shim, then probe the staged bytes for
     * the MIO0 magic -- byte-identical to the old direct gdx_rom_buffer probe. */
    if (!GdxSegmentSourceRead((unsigned int)romStart, span, sGdxSeg5Stage) ||
        sGdxSeg5Stage[0] != 'M' || sGdxSeg5Stage[1] != 'I' ||
        sGdxSeg5Stage[2] != 'O' || sGdxSeg5Stage[3] != '0') {
        gdx_ck("[segment] segment 5 podium_gfx source invalid (not MIO0)");
        return 0;
    }

    /* Authoritative decoded size from the MIO0 header (big-endian u32 at +4), same
     * check the seg-9 machine_models path uses -- a corrupt ROM could otherwise
     * decompress past the carve. */
    decodedSize = ((unsigned int)sGdxSeg5Stage[4] << 24) |
                  ((unsigned int)sGdxSeg5Stage[5] << 16) |
                  ((unsigned int)sGdxSeg5Stage[6] << 8) | (unsigned int)sGdxSeg5Stage[7];
    if (decodedSize == 0u || decodedSize > 0x100000u) {
        gdx_ck("[segment] segment 5 podium_gfx decoded size implausible");
        return 0;
    }

    /* Carve once from the persist region; ROM data is immutable for the process
     * lifetime, so the buffer and its fixups are reused on every later ending. */
    if (sGdxSeg5PodiumBuf == NULL || sGdxSeg5PodiumSize < decodedSize) {
        void* buf = gdx_rdram_persist_alloc_raw((size_t)decodedSize, 16u);
        if (buf == NULL) {
            gdx_ck("[segment] segment 5 podium_gfx carve alloc failed");
            return 0;
        }
        sGdxSeg5PodiumBuf = (unsigned char*)buf;
        sGdxSeg5PodiumSize = (size_t)decodedSize;
        sGdxSeg5Resident = 0;
    }

    gSegment2738A0VramStart = (uintptr_t)(sGdxSeg5PodiumBuf - gdx_rdram);
    gSegment2738A0VramEnd = gSegment2738A0VramStart + sGdxSeg5PodiumSize;

    if (!sGdxSeg5Resident) {
        mio0Decode(sGdxSeg5Stage, sGdxSeg5PodiumBuf);
        /* Rewrite the podium display lists' embedded 0x05xxxxxx pointers in place
         * (idempotency matters: fixups are applied EXACTLY once, guarded by the
         * resident flag -- double-fixup would corrupt the command words). */
        gdx_fixup_asset_segment_image(0x05u, (unsigned int)PORT_podium_gfx_ROM_START, sGdxSeg5PodiumBuf,
                                      (unsigned int)sGdxSeg5PodiumSize);
        gdx_register_asset_segment_command_ranges(0x05u, (unsigned int)PORT_podium_gfx_ROM_START, sGdxSeg5PodiumBuf,
                                                   (unsigned int)sGdxSeg5PodiumSize);
        sGdxSeg5Resident = 1;
        gdx_ck("[transition] seg5 reload: podium_gfx");
    } else {
        gdx_ck("[transition] seg5 reload skipped (podium_gfx resident)");
    }

    Segment_SetAddress(5, gSegment2738A0VramStart);
    {
        extern void gdx_cki(const char*, int);
        gdx_cki("[segment] seg5 active=podium_gfx size", (int)sGdxSeg5PodiumSize);
    }
    return 1;
}

static void gdx_load_segment5_for_mode(void) {
    switch (GET_MODE(gGameMode)) {
        case GAMEMODE_GP_END_CS:
            gdx_activate_podium_segment5();
            break;
        default:
            /* Non-ending modes never own segment 5; leave gSegments[5]/the carve
             * untouched (matches the console Segment_SetupSegment5 default path). */
            break;
    }
}

static void gdx_load_mode_segments(void) {
    extern void gdx_cki(const char*, int);
    /* P3 frame-interpolation cut epoch (MATRIX_INTERPOLATION_PLAN.md Step 7, event #5): this is the
       single port-side chokepoint every mode/screen transition passes through (Segment_LoadOverlays
       calls it on each mode change). Bumping the cut epoch here snaps the first rendered tick of the
       new mode so nothing streaks across SELECT MACHINE<->race, Course Edit<->play, GRAND PRIX
       standings, race entry, or a Retry reload. Render-only; no-op unless interpolation is on. */
    extern void gdx_interp_mark_cut_src(const char* tag);
    gdx_interp_mark_cut_src("mode-change");
    size_t hudSize = (size_t)(PORT_hud_gfx_ROM_END - PORT_hud_gfx_ROM_START);
    size_t createMachineSize =
        (size_t)(PORT_create_machine_textures_ROM_END - PORT_create_machine_textures_ROM_START);
    size_t machineGlobalSize =
        (size_t)(PORT_machine_global_gfx_ROM_END - PORT_machine_global_gfx_ROM_START);
    size_t ekTexturesSize =
        (size_t)(PORT_expansion_kit_textures_beta_ROM_END - PORT_expansion_kit_textures_beta_ROM_START);
    size_t seg7EkSize = (ekTexturesSize <= machineGlobalSize) ? ekTexturesSize : machineGlobalSize;
    static int sModeSegLogs = 0;

    /* Open the segment-reload epoch window BEFORE any base swap or carve rewrite.
       Everything below -- Dma_LoadAssets/mio0Decode into the carves,
       gdx_fixup_asset_segment_image in-place rewrites, and the Segment_SetAddress
       gSegments[] base swaps for segments 4/7/9 -- mutates state the graphics
       thread reads unsynchronized. Non-nested: this is the single top-level
       mode-transition reload path (gdx_load_segment9_for_mode is called inside
       and never re-enters here). */
    gdx_segment_epoch_begin();

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
    /* Segment 5 (podium_gfx) is GP-ending-only; mutated inside the same epoch
       window as the seg 4/7/9 rewrites so a racing graphics-thread snapshot skips
       the frame instead of consuming a torn gSegments[5] base. */
    gdx_load_segment5_for_mode();

    /* Close the epoch window: publishes the settled segment state (even counter)
       so the next graphics-thread snapshot resolves normally. Paired 1:1 with the
       begin() above on every control-flow path (no early returns in between). */
    gdx_segment_epoch_end();

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
