/* LLE audio dispatch (branch lle-audio).
 *
 * This is the seam between the game's audio scheduler and the audio RSP. It routes
 * each M_AUDTASK either to the REAL audio microcode (aspMain) running on the vendored
 * cxd4 RSP interpreter -- via a per-tick memory-model bridge that marshals the Acmd
 * command list and every buffer it references into a flat physical RDRAM window the
 * RSP can DMA (Option B: flat-physical-RDRAM, realized as a bounded per-tick scratch
 * arena rather than a full heap relocation, which cannot fit under cxd4's 24-bit DMA
 * addressing alongside the 12-13MB GFX working set) -- or falls back to the existing
 * software HLE interpreter (gdx_audio_hle_run).
 *
 * Toggle is FILE-based (env vars are unreliable in the target shell): LLE is ON by
 * default (the owner-confirmed, grain-free audio path); a file `gdx-audio-lle.txt`
 * whose first byte is '0' forces the HLE fallback. Read once and cached.
 *
 * ============================================================================
 * THE MEMORY-MODEL BRIDGE (gdx_audio_lle_bridge_run)
 * ============================================================================
 * The real aspMain microcode DMAs its command list and every buffer that list
 * references (compressed sample data, ADPCM codebooks, per-note decode/resample
 * state, reverb rings, the AI output PCM, ...) straight out of RDRAM by *physical
 * offset*. On this host port those buffers live scattered across (a) the 16MB
 * gdx_rdram arena and (b) the exe's static image (gAudioCtx and its embedded
 * arrays). The Acmd `w1` fields that name them only carry the *low 32 bits* of a
 * host pointer (see n64_audio_hle.c's pointer-truncation essay), so the bridge
 * reconstructs each full host pointer with the same resolvers the HLE uses
 * (gdx_resolve_registered_host_address, then gdx_resolve_module_host_address),
 * copies the referenced buffer into a contiguous physical scratch window the RSP
 * *can* reach, rewrites the command's `w1` to that scratch offset, runs one task,
 * then copies RSP-written buffers back out to their host homes.
 *
 * ----------------------------------------------------------------------------
 * THE ENDIANNESS RULE (critical -- see per-transform notes below)
 * ----------------------------------------------------------------------------
 * cxd4 stores ALL of RDRAM/DMEM/IMEM as HOST-NATIVE LITTLE-ENDIAN 32-BIT WORDS.
 * It models N64 (big-endian) memory as "byte-swap each aligned 32-bit word of the
 * N64-big-endian content". Since our scratch IS a region of gdx_rdram (cxd4's RDRAM
 * base is pointed straight at gdx_rdram by gdx_rsp_lle_init), a host-native u32 we
 * store at gdx_rdram+P is exactly the word cxd4 reads as RDRAM word P. So to place a
 * buffer for the RSP we transform each aligned 32-bit HOST word according to how the
 * host stores that buffer's element type:
 *
 *   GDX_XFORM_CMD  (identity)  -- Acmd words. The game already built w0/w1 as
 *       host-native u32; a host-native u32 == (N64-BE u32 then word-swapped), so no
 *       transform is needed beyond rewriting address-bearing w1 fields. Copying the
 *       raw command bytes verbatim is therefore already correct.
 *
 *   GDX_XFORM_S16  (rotate-16) -- buffers the host stores as an array of NATIVE s16
 *       (decomp reads them with plain `short` / gdx_rd_s16, so predictorState /
 *       adpcmdecState / finalResampleState / filter state+coefs / reverb-ring PCM /
 *       AI output PCM are all native s16 in host memory). N64-BE of a native s16 pair
 *       [s0 s1] is bytes [s0hi s0lo s1hi s1lo]; word-swapping that yields, expressed as
 *       an op on the host word W = (s1<<16)|s0, exactly (W>>16)|(W<<16): swap the two
 *       16-bit halves. (Involution.)
 *
 *   GDX_XFORM_BYTESTREAM (bswap32) -- buffers the host stores as a RAW big-endian byte
 *       stream in N64 linear order (compressed ADPCM sample data loaded straight from
 *       cart/disk; the HLE copies these bytes 1:1 into DMEM and decodes them correctly,
 *       which proves host order == N64 linear byte order). N64-BE of a linear byte run
 *       [b0 b1 b2 b3] is itself; word-swapping is a plain 32-bit byte reversal. (Involution.)
 *
 * All three transforms are their own inverse, so COPY-BACK (RSP-written buffers) uses
 * the SAME transform as copy-in.
 *
 * BEST-GUESS / TUNING KNOBS (flagged so a future tuner can flip one byte order without
 * touching the machinery): the transforms live in gdx_copy_in/gdx_copy_out keyed by a
 * per-buffer-type GdxXform, chosen per opcode in the marshal switch. The two that rest
 * on inference rather than a hard ABI field are:
 *   (1) A_LOADBUFF compressed-vs-PCM classification (GDX_XFORM_BYTESTREAM only for the
 *       single compressed-ADPCM load, identified by the DMEM signature dmemDest+size ==
 *       DMEM_COMPRESSED_ADPCM_DATA; everything else treated as S16 PCM). If uncompressed
 *       (CODEC_S16) samples come out wrong, revisit this classifier.
 *   (2) Whether the host actually stores compressed sample data big-endian. The HLE's
 *       correctness says yes; if LLE ADPCM is noise while HLE is clean, flip A_LOADBUFF's
 *       compressed branch from BYTESTREAM to S16 (or identity) here.
 *
 * ----------------------------------------------------------------------------
 * SAFETY NET
 * ----------------------------------------------------------------------------
 * If ANY opcode/address cannot be confidently marshalled (unresolved w1 token, a
 * truly-unknown opcode, or scratch overflow) the bridge ABORTS this tick and defers to
 * gdx_audio_hle_run() -- no host buffer is mutated before a successful RSP run, so the
 * fall-through is clean. LLE-enabled therefore never crashes; worst case it silently
 * degrades to HLE for a tick. Cross-tick state stays consistent across LLE<->HLE ticks
 * because the HOST buffers are the source of truth: every input is copied host->scratch
 * before the run and every RSP output is copied scratch->host after it.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "rsp/cxd4/gdx_rsp_driver.h"
#include "rsp/aspmain_ucode.h"
#include "n64_rdram.h"

extern void gdx_audio_hle_run(const void* dataPtr, unsigned int dataSizeBytes);

/* Low-32 -> full host pointer resolvers (defined in port/n64_gfx_bridge.cpp; the same
 * ones the HLE uses). Deliberately `unsigned int` (unambiguous 32 bits in every TU). */
extern void* gdx_resolve_registered_host_address(unsigned int addr);
extern void* gdx_resolve_module_host_address(unsigned int addr);

/* Verbose-diagnostics gate (defined in port/n64_sched.c). */
extern int gdx_diag_verbose(void);

/* ==========================================================================
 * Toggle (UNCHANGED -- keep the HLE fallback exactly as it was)
 * ========================================================================== */
/* LUS console-variable read (C ABI, defined in libultraship). Declared here so this C TU
 * doesn't pull the C++ bridge header. */
extern int CVarGetInteger(const char* name, int defaultValue);

static int gdx_audio_lle_enabled(void) {
    /* Live from the ImGui Audio tab (F1 > Audio > Engine): gEnhancements.Audio.LLE
     * (1 = LLE, 0 = HLE). Read each tick on the audio thread; the CVar is pre-registered at
     * boot so a concurrent menu write is a benign int-value race (worst case one tick sees the
     * old value). Default 1 = LLE on -- the owner-confirmed audio path -- so behavior is
     * unchanged if the menu is never touched. */
    return CVarGetInteger("gEnhancements.Audio.LLE", 1) != 0;
}

/* ==========================================================================
 * Diagnostics
 * ========================================================================== */
static void gdx_lle_logf(const char* fmt, ...) {
    va_list ap;
    FILE* f;
    if (!gdx_diag_verbose()) {
        return;
    }
    f = fopen("gdiffuser-run.log", "a");
    if (f == NULL) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

/* ==========================================================================
 * Opcode numbers (mirror n64_audio_hle.c / PR/abi.h, EXPANSION_KIT build)
 * ========================================================================== */
enum {
    GDX_A_SPNOOP       = 0,
    GDX_A_ADPCM        = 1,
    GDX_A_CLEARBUFF    = 2,
    GDX_A_UNK3         = 3,
    GDX_A_ADDMIXER     = 4,
    GDX_A_RESAMPLE     = 5,
    GDX_A_RESAMPLE_ZOH = 6,
    GDX_A_FILTER       = 7,
    GDX_A_SETBUFF      = 8,
    GDX_A_DMEMMOVE     = 10,
    GDX_A_LOADADPCM    = 11,
    GDX_A_MIXER        = 12,
    GDX_A_INTERLEAVE   = 13,
    GDX_A_HILOGAIN     = 14, /* EXPANSION_KIT build */
    GDX_A_SETLOOP      = 15,
    GDX_A_INTERL       = 17,
    GDX_A_ENVSETUP1    = 18,
    GDX_A_ENVMIXER     = 19,
    GDX_A_LOADBUFF     = 20,
    GDX_A_SAVEBUFF     = 21,
    GDX_A_ENVSETUP2    = 22,
    GDX_A_S8DEC        = 23,
    GDX_A_UNK19        = 25,
    GDX_A_DUPLICATE    = 26
};

/* DMEM landmark used to classify A_LOADBUFF (compressed sample chunk vs PCM). The one
 * compressed-ADPCM load in disk/lib/synthesis.c is `aLoadBuffer(.., addr, aligned)` with
 * addr = DMEM_COMPRESSED_ADPCM_DATA - aligned, so it is the unique load whose DMEM window
 * ENDS exactly at DMEM_COMPRESSED_ADPCM_DATA (mod 0x10000 -- robust to the u16 wrap when
 * aligned > addr). Reverb-ring / PCM loads target other DMEM addresses. */
#define GDX_DMEM_COMPRESSED_ADPCM_DATA 0x940u

/* State-buffer DMA sizes (bytes), sized to what the real aspMain DMAs, cross-checked
 * against the decomp struct fields the ucode author matched to the ucode:
 *   adpcmdecState[16]       -> 32  (A_ADPCM / A_S8DEC persistent decode history)
 *   finalResampleState[16]  -> 32  (A_RESAMPLE persistent 4-tap window + frac + pitch)
 *   predictorState[16]      -> 32  (A_SETLOOP loop-restart history; ROM-constant, read-only)
 *   filter coef  = FILTER_SIZE                   = 8*2  = 16 (A_FILTER prime, read-only)
 *   filter state = 2*(FILTER_BUF_PART1+PART2)    = 2*32 = 64 (A_FILTER apply, in/out) */
#define GDX_STATE_ADPCM_BYTES     32u
#define GDX_STATE_RESAMPLE_BYTES  32u
#define GDX_STATE_LOOP_BYTES      32u
#define GDX_FILTER_COEF_BYTES     16u
#define GDX_FILTER_STATE_BYTES    64u
/* A_LOADADPCM codebook DMA cap (matches the HLE's private book scratch bound). */
#define GDX_ADPCM_BOOK_MAX_BYTES  4096u

/* ==========================================================================
 * Endianness transforms (all involutions; see the file header essay)
 * ========================================================================== */
typedef enum {
    GDX_XFORM_CMD = 0,      /* identity -- Acmd words */
    GDX_XFORM_S16,          /* rotate-16 -- native s16 arrays */
    GDX_XFORM_BYTESTREAM    /* bswap32 -- raw big-endian byte streams */
} GdxXform;

static uint32_t gdx_bswap32(uint32_t w) {
    return ((w & 0x000000FFu) << 24) | ((w & 0x0000FF00u) << 8) |
           ((w & 0x00FF0000u) >> 8)  | ((w & 0xFF000000u) >> 24);
}
static uint32_t gdx_rot16(uint32_t w) {
    return (w >> 16) | (w << 16);
}
static uint32_t gdx_xform_word(uint32_t w, GdxXform xf) {
    switch (xf) {
        case GDX_XFORM_S16:        return gdx_rot16(w);
        case GDX_XFORM_BYTESTREAM: return gdx_bswap32(w);
        case GDX_XFORM_CMD:
        default:                   return w;
    }
}

/* ==========================================================================
 * Scratch arena layout (all offsets are RELATIVE to sScratchPhys)
 * --------------------------------------------------------------------------
 *   [0 .. DATA_END)        aspMain data section (fixed; written once at init)
 *   [PERSIST_OFF .. PERSIST_END)  persistent state slots (never reset): in/out
 *                          decode/resample/filter state keyed by host pointer so a
 *                          given note's state always maps to the same scratch slot.
 *   [TICK_OFF .. SCRATCH_SIZE)   per-tick bump arena (reset every tick): command list,
 *                          compressed sample data, codebooks, filter coefs, reverb-ring
 *                          and AI-output DMA windows, and any input-only buffer.
 * The scratch is carved from the TOP of RDRAM (gdx_rdram_persist_alloc_raw), above the
 * ~12-13MB GFX working set, and is never reclaimed.
 * ========================================================================== */
#define GDX_LLE_SCRATCH_SIZE   0x200000u   /* 2 MB (well over any observed audio tick) */
#define GDX_LLE_DATA_OFF       0x000000u
#define GDX_LLE_PERSIST_OFF    0x000300u   /* after the 0x2E0-byte data section, 16-aligned */
#define GDX_LLE_PERSIST_END    0x040000u   /* 256 KB for persistent state slots */
#define GDX_LLE_TICK_OFF       0x040000u   /* per-tick arena starts here */

static int      sInitState  = 0;           /* 0 = not attempted, 1 = ready, -1 = failed */
static uint32_t sScratchPhys = 0;          /* physical RDRAM offset of the scratch base */
static uint32_t sDataSecPhys = 0;          /* physical offset of the aspMain data section */

/* Persistent state slot table (stable host-ptr -> scratch offset across ticks). */
#define GDX_PERSIST_MAX 512
typedef struct {
    const void* host;
    uint32_t    phys;
    uint32_t    size;   /* rounded slot size */
} GdxPersistSlot;
static GdxPersistSlot sPersist[GDX_PERSIST_MAX];
static int            sPersistCount = 0;
static uint32_t       sPersistBump  = GDX_LLE_PERSIST_OFF;

/* Per-tick mapping list: dedups repeated references within a tick and records which
 * buffers to copy back after the run. Rebuilt each tick. */
#define GDX_MAP_MAX 1024
typedef struct {
    const void* host;
    uint32_t    phys;
    uint32_t    size;      /* exact byte count to copy back */
    GdxXform    xf;
    int         isOutput;  /* copy scratch->host after the RSP run */
} GdxMap;
static GdxMap sMap[GDX_MAP_MAX];
static int    sMapCount = 0;
static uint32_t sTickBump = GDX_LLE_TICK_OFF;

/* ==========================================================================
 * Copy helpers -- move `bytes` between a host buffer and the scratch (which is itself
 * host-addressable memory inside gdx_rdram), transforming each aligned 32-bit word.
 * Both handle a ragged 1-3 byte tail without reading/writing past either endpoint.
 * ========================================================================== */
static void gdx_copy_in(uint32_t dstPhys, const void* src, uint32_t bytes, GdxXform xf) {
    const uint8_t* s = (const uint8_t*)src;
    uint8_t* d = gdx_rdram + dstPhys;
    uint32_t full = bytes & ~3u;
    uint32_t i;
    for (i = 0; i < full; i += 4u) {
        uint32_t w;
        memcpy(&w, s + i, 4);
        w = gdx_xform_word(w, xf);
        memcpy(d + i, &w, 4);
    }
    /* Ragged 1-3 byte tail: DEAD for every real buffer (all DMA sizes here are >=4-byte
     * multiples -- states 32/64/16B, LOAD/SAVE sizes are (n<<4), codebooks 32B multiples,
     * the data section 736B). A word-transform can't straddle a sub-word boundary, so for
     * a pathological unaligned size fall back to a raw byte copy: never out-of-bounds,
     * never corrupts neighbours (vs. a transformed partial word, which could zero it). */
    if (bytes & 3u) {
        memcpy(d + full, s + full, bytes & 3u);
    }
}
static void gdx_copy_out(void* dst, uint32_t srcPhys, uint32_t bytes, GdxXform xf) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = gdx_rdram + srcPhys;
    uint32_t full = bytes & ~3u;
    uint32_t i;
    for (i = 0; i < full; i += 4u) {
        uint32_t w;
        memcpy(&w, s + i, 4);
        w = gdx_xform_word(w, xf);   /* same transform: each is its own inverse */
        memcpy(d + i, &w, 4);
    }
    if (bytes & 3u) {                /* see gdx_copy_in: safe raw tail, never reached in practice */
        memcpy(d + full, s + full, bytes & 3u);
    }
}

/* ==========================================================================
 * One-time init: wire cxd4, reserve the scratch, seed the data section.
 * Returns 1 if the bridge is usable, 0 if it must permanently defer to HLE.
 * ========================================================================== */
static int gdx_lle_init_once(void) {
    void* scratch;
    uintptr_t rel;

    if (sInitState != 0) {
        return sInitState > 0;
    }

    if (gdx_rdram == NULL) {
        /* Should never happen (RDRAM is up long before audio), but never crash. */
        sInitState = -1;
        return 0;
    }

    gdx_rsp_lle_init(gdx_rdram);

    /* Session-lifetime scratch from the TOP of RDRAM, downward. Fatal on exhaustion
     * inside the allocator, so a non-NULL return is guaranteed on success. */
    scratch = gdx_rdram_persist_alloc_raw((size_t)GDX_LLE_SCRATCH_SIZE, (size_t)16);
    rel = (uintptr_t)((unsigned char*)scratch - gdx_rdram);

    /* cxd4 masks DMA addresses to 24 bits: the ENTIRE scratch must live below 16MB. */
    if (rel + (uintptr_t)GDX_LLE_SCRATCH_SIZE > (uintptr_t)GDX_RDRAM_SIZE) {
        fprintf(stderr, "[audio-lle] bridge DISABLED: scratchPhys=%06lX + size exceeds 24-bit DMA window\n",
                (unsigned long)rel);
        sInitState = -1;
        return 0;
    }
    sScratchPhys = (uint32_t)rel;

    /* Seed the aspMain data section (raw big-endian ROM blob) at a fixed spot. The RSP
     * DMAs ucode_data from here into DMEM at boot; the driver also byte-swaps the same
     * blob into DMEM[0] as a fallback, so this must match: bswap32 per word. */
    sDataSecPhys = sScratchPhys + GDX_LLE_DATA_OFF;
    gdx_copy_in(sDataSecPhys, gdx_aspmain_data, (uint32_t)gdx_aspmain_data_len, GDX_XFORM_BYTESTREAM);

    sPersistBump  = GDX_LLE_PERSIST_OFF;
    sPersistCount = 0;

    fprintf(stderr, "[audio-lle] bridge init OK: scratchPhys=%06X size=%X dataSec=%06X (len=%X)\n",
            (unsigned)sScratchPhys, (unsigned)GDX_LLE_SCRATCH_SIZE,
            (unsigned)sDataSecPhys, (unsigned)gdx_aspmain_data_len);

    sInitState = 1;
    return 1;
}

/* Persistent slot: stable scratch offset for a given host state pointer across ticks.
 * Returns 1 and sets *outPhys on success; 0 if the persistent region is exhausted. */
static int gdx_persist_slot(const void* host, uint32_t size, uint32_t* outPhys) {
    uint32_t rounded = (size + 15u) & ~15u;
    int i;
    for (i = 0; i < sPersistCount; i++) {
        if (sPersist[i].host == host) {
            if (sPersist[i].size < rounded) {
                return 0;   /* Finding 7: a grown request must never overflow a fixed slot -> HLE */
            }
            *outPhys = sPersist[i].phys;
            return 1;
        }
    }
    if (sPersistCount >= GDX_PERSIST_MAX) {
        return 0;
    }
    if (sPersistBump + rounded > GDX_LLE_PERSIST_END) {
        return 0;
    }
    sPersist[sPersistCount].host = host;
    sPersist[sPersistCount].phys = sScratchPhys + sPersistBump;
    sPersist[sPersistCount].size = rounded;
    *outPhys = sPersist[sPersistCount].phys;
    sPersistBump += rounded;
    sPersistCount++;
    return 1;
}

/* Stage one address-bearing buffer into the scratch and hand back its physical offset.
 *   host       resolved full host pointer (never NULL here)
 *   size       exact bytes the RSP will DMA
 *   xf         endianness transform for this buffer's element type
 *   isOutput   RSP writes it -> copy scratch->host after the run
 *   persistent true for cross-tick in/out state (stable slot); false for per-tick
 *   doCopyIn   copy host->scratch now (all inputs and in/out state; false for pure
 *              outputs like the AI buffer that the RSP fills from scratch)
 * Returns 1 on success (*outPhys set), 0 on any overflow (caller must bail to HLE). */
static int gdx_stage(const void* host, uint32_t size, GdxXform xf,
                     int isOutput, int persistent, int doCopyIn, uint32_t* outPhys) {
    uint32_t phys;
    uint32_t rounded;
    int i;

    /* Within-tick dedup: the same host buffer always maps to one scratch slot, so
     * e.g. a reverb ring loaded then saved in one tick shares its DMA window (the
     * save sees the load's data), and repeated state references copy in only once. */
    for (i = 0; i < sMapCount; i++) {
        if (sMap[i].host == host) {
            if (isOutput) {
                sMap[i].isOutput = 1;
            }
            *outPhys = sMap[i].phys;
            return 1;
        }
    }

    rounded = (size + 15u) & ~15u;
    if (persistent) {
        if (!gdx_persist_slot(host, size, &phys)) {
            return 0;
        }
    } else {
        if (sTickBump + rounded > GDX_LLE_SCRATCH_SIZE) {
            return 0;
        }
        phys = sScratchPhys + sTickBump;
        sTickBump += rounded;
    }

    if (sMapCount >= GDX_MAP_MAX) {
        return 0;
    }

    if (doCopyIn) {
        gdx_copy_in(phys, host, size, xf);
    }

    sMap[sMapCount].host     = host;
    sMap[sMapCount].phys     = phys;
    sMap[sMapCount].size     = size;
    sMap[sMapCount].xf       = xf;
    sMap[sMapCount].isOutput = isOutput;
    sMapCount++;

    *outPhys = phys;
    return 1;
}

/* Resolve an Acmd w1 low-32 token to a full host pointer (registered range first, then
 * the exe module image). Returns NULL if neither resolver knows it (caller bails). */
static void* gdx_lle_resolve(uint32_t raw) {
    void* p;
    if (raw == 0) {
        return NULL;
    }
    p = gdx_resolve_registered_host_address((unsigned int)raw);
    if (p != NULL) {
        return p;
    }
    return gdx_resolve_module_host_address((unsigned int)raw);
}

/* ==========================================================================
 * The bridge.
 * ========================================================================== */
static void gdx_audio_lle_bridge_run(const void* dataPtr, unsigned int dataSizeBytes) {
    const uint32_t* cmds;       /* host command list, {w0,w1} pairs of host-native u32 */
    uint32_t count;
    uint32_t cmdBytes;
    uint32_t cmdListPhys;
    uint32_t* scratchCmds;      /* the scratch copy we rewrite w1 fields in */
    uint32_t ostask[16];        /* 64-byte OSTask, host-native little-endian */
    uint32_t i;
    static int sBailLogs = 0;

    /* --- BAIL macro: never mutate host state before a successful run, so a mid-marshal
     * abort simply hands the untouched tick to the HLE. --- */
    #define GDX_LLE_BAIL(reasonfmt, ...)                                              \
        do {                                                                          \
            if (sBailLogs < 32) {                                                     \
                sBailLogs++;                                                          \
                gdx_lle_logf("[audio-lle] bail -> HLE: " reasonfmt "\n", __VA_ARGS__);\
            }                                                                         \
            gdx_audio_hle_run(dataPtr, dataSizeBytes);                                \
            return;                                                                   \
        } while (0)

    if (!gdx_lle_init_once()) {
        gdx_audio_hle_run(dataPtr, dataSizeBytes);
        return;
    }

    cmds  = (const uint32_t*)dataPtr;
    count = dataSizeBytes / 8u;                 /* Acmd is 8 bytes: {u32 w0; u32 w1;} */
    if (cmds == NULL || count == 0u) {
        gdx_audio_hle_run(dataPtr, dataSizeBytes);
        return;
    }

    /* --- Reset the per-tick arena and mapping list. Persistent slots survive. --- */
    sTickBump = GDX_LLE_TICK_OFF;
    sMapCount = 0;

    /* Persistent-state region recycling (Finding 3): slots are keyed by host pointer and
     * never individually evicted, so over a long session with audio-heap churn (bank/scene
     * changes reallocate note state to fresh pointers) the 512-slot table would fill and then
     * bail every state-bearing tick to HLE for the rest of the run. Recycle the whole region
     * BETWEEN ticks when nearly full -- never mid-marshal, which would invalidate slots already
     * placed this tick. Safe because host buffers are authoritative: each tick copies state
     * host->scratch before the run, so live slots are re-established next tick with correct
     * data (a dead reverb tail may glitch for one tick at most). One tick uses <~32 slots. */
    if (sPersistCount > GDX_PERSIST_MAX - 32) {
        sPersistCount = 0;
        sPersistBump  = GDX_LLE_PERSIST_OFF;
    }

    /* --- Copy the command list into scratch verbatim (host-native u32 == cxd4 word,
     * GDX_XFORM_CMD is identity, so a raw byte copy is correct). We rewrite the
     * address-bearing w1 fields IN THIS COPY, leaving the game's original list pristine
     * (so the HLE fallback and the game itself are unaffected). --- */
    cmdBytes = count * 8u;
    {
        uint32_t rounded = (cmdBytes + 15u) & ~15u;
        if (sTickBump + rounded > GDX_LLE_SCRATCH_SIZE) {
            GDX_LLE_BAIL("cmd list %u bytes overflows scratch", (unsigned)cmdBytes);
        }
        cmdListPhys = sScratchPhys + sTickBump;
        sTickBump += rounded;
        memcpy(gdx_rdram + cmdListPhys, cmds, cmdBytes);
        scratchCmds = (uint32_t*)(void*)(gdx_rdram + cmdListPhys);
    }

    /* --- Marshal pass: for each address-bearing command, resolve w1, stage the buffer,
     * and rewrite the scratch copy's w1 to the scratch physical offset. --- */
    for (i = 0; i < count; i++) {
        uint32_t w0 = cmds[i * 2u + 0u];
        uint32_t w1 = cmds[i * 2u + 1u];
        uint32_t op = (w0 >> 24) & 0xFFu;

        switch (op) {

            /* ---- Address-bearing opcodes (w1 = an RDRAM address) ---------------- */

            case GDX_A_LOADBUFF: {
                /* Load `size` bytes of sample data from RDRAM into DMEM. INPUT only.
                 * size = (w0>>16 & 0xFF) << 4, dmemDest = w0 & 0xFFFF (see HLE). The
                 * compressed-ADPCM chunk load is the unique one whose DMEM window ends
                 * at DMEM_COMPRESSED_ADPCM_DATA -> treat as a raw BE byte stream; every
                 * other load (reverb ring / uncompressed PCM) is native s16.
                 * [TUNING KNOB #1/#2: this classifier + the BYTESTREAM choice.] */
                uint32_t size     = ((w0 >> 16) & 0xFFu) << 4;
                uint32_t dmemDest = w0 & 0xFFFFu;
                GdxXform xf = (((dmemDest + size) & 0xFFFFu) == GDX_DMEM_COMPRESSED_ADPCM_DATA)
                              ? GDX_XFORM_BYTESTREAM : GDX_XFORM_S16;
                void* host = gdx_lle_resolve(w1);
                uint32_t phys;
                if (host == NULL) {
                    GDX_LLE_BAIL("LOADBUFF unresolved w1=%08X", (unsigned)w1);
                }
                if (size == 0u) {
                    break; /* nothing to DMA; leave w1 as-is (harmless, size 0) */
                }
                if (!gdx_stage(host, size, xf, /*out*/0, /*persist*/0, /*copyIn*/1, &phys)) {
                    GDX_LLE_BAIL("LOADBUFF scratch overflow (size=%X)", (unsigned)size);
                }
                scratchCmds[i * 2u + 1u] = phys;
                break;
            }

            case GDX_A_SAVEBUFF: {
                /* Store `size` bytes of RSP-produced PCM from DMEM to RDRAM. OUTPUT.
                 * Destinations are reverb rings and the AI output buffer -- all native
                 * s16. No copy-in (the RSP fills the DMA window); copy scratch->host
                 * after the run. If the same host ptr was already loaded this tick, the
                 * dedup in gdx_stage reuses that window (load->process->save round-trip). */
                uint32_t size    = ((w0 >> 16) & 0xFFu) << 4;
                uint32_t dmemSrc = w0 & 0xFFFFu;
                void* host = gdx_lle_resolve(w1);
                uint32_t phys;
                (void)dmemSrc;
                if (host == NULL) {
                    GDX_LLE_BAIL("SAVEBUFF unresolved w1=%08X", (unsigned)w1);
                }
                if (size == 0u) {
                    break;
                }
                if (!gdx_stage(host, size, GDX_XFORM_S16, /*out*/1, /*persist*/0, /*copyIn*/0, &phys)) {
                    GDX_LLE_BAIL("SAVEBUFF scratch overflow (size=%X)", (unsigned)size);
                }
                scratchCmds[i * 2u + 1u] = phys;
                break;
            }

            case GDX_A_LOADADPCM: {
                /* Load an ADPCM codebook (host-native s16 array, written by gdx_rd_s16).
                 * byteCount = w0 & 0xFFFFFF (capped like the HLE's private book). INPUT. */
                uint32_t byteCount = w0 & 0xFFFFFFu;
                void* host = gdx_lle_resolve(w1);
                uint32_t phys;
                if (host == NULL) {
                    GDX_LLE_BAIL("LOADADPCM unresolved w1=%08X", (unsigned)w1);
                }
                if (byteCount > GDX_ADPCM_BOOK_MAX_BYTES) {
                    byteCount = GDX_ADPCM_BOOK_MAX_BYTES;
                }
                if (byteCount == 0u) {
                    break;
                }
                if (!gdx_stage(host, byteCount, GDX_XFORM_S16, /*out*/0, /*persist*/0, /*copyIn*/1, &phys)) {
                    GDX_LLE_BAIL("LOADADPCM scratch overflow (bytes=%X)", (unsigned)byteCount);
                }
                scratchCmds[i * 2u + 1u] = phys;
                break;
            }

            case GDX_A_SETLOOP: {
                /* Load the loop-restart predictor history (predictorState[16], host s16).
                 * INPUT only and ROM-constant, so a per-tick slot is fine (nothing to
                 * retain across ticks -- the RSP never writes it back). */
                void* host = gdx_lle_resolve(w1);
                uint32_t phys;
                if (host == NULL) {
                    GDX_LLE_BAIL("SETLOOP unresolved w1=%08X", (unsigned)w1);
                }
                if (!gdx_stage(host, GDX_STATE_LOOP_BYTES, GDX_XFORM_S16,
                               /*out*/0, /*persist*/0, /*copyIn*/1, &phys)) {
                    GDX_LLE_BAIL("SETLOOP scratch overflow%s", "");
                }
                scratchCmds[i * 2u + 1u] = phys;
                break;
            }

            case GDX_A_ADPCM:
            case GDX_A_S8DEC: {
                /* Persistent per-note decode state (adpcmdecState[16], host s16). IN/OUT:
                 * the RSP reads last tick's tail and writes this tick's tail back. Stable
                 * scratch slot keyed by the state pointer + copy-in/out each tick keeps
                 * decode continuity correct even across LLE<->HLE ticks. */
                void* host = gdx_lle_resolve(w1);
                uint32_t phys;
                if (host == NULL) {
                    GDX_LLE_BAIL("ADPCM/S8DEC unresolved w1=%08X", (unsigned)w1);
                }
                if (!gdx_stage(host, GDX_STATE_ADPCM_BYTES, GDX_XFORM_S16,
                               /*out*/1, /*persist*/1, /*copyIn*/1, &phys)) {
                    GDX_LLE_BAIL("ADPCM/S8DEC scratch overflow%s", "");
                }
                scratchCmds[i * 2u + 1u] = phys;
                break;
            }

            case GDX_A_RESAMPLE: {
                /* Persistent per-note resample state (finalResampleState[16], host s16).
                 * IN/OUT: 4-sample history + fractional accumulator carried tick to tick. */
                void* host = gdx_lle_resolve(w1);
                uint32_t phys;
                if (host == NULL) {
                    GDX_LLE_BAIL("RESAMPLE unresolved w1=%08X", (unsigned)w1);
                }
                if (!gdx_stage(host, GDX_STATE_RESAMPLE_BYTES, GDX_XFORM_S16,
                               /*out*/1, /*persist*/1, /*copyIn*/1, &phys)) {
                    GDX_LLE_BAIL("RESAMPLE scratch overflow%s", "");
                }
                scratchCmds[i * 2u + 1u] = phys;
                break;
            }

            case GDX_A_FILTER: {
                /* Two-step reverb low-pass op (see HLE's RunFilter header):
                 *   prime (f==2): w1 = coefficient table (FILTER_SIZE=16 host s16). INPUT.
                 *   apply (else):  w1 = filter delay-line state (64 bytes host s16). IN/OUT,
                 *                  persistent (history carried across command lists). */
                uint32_t f = (w0 >> 16) & 0xFFu;
                void* host = gdx_lle_resolve(w1);
                uint32_t phys;
                if (host == NULL) {
                    GDX_LLE_BAIL("FILTER(f=%u) unresolved w1=%08X", (unsigned)f, (unsigned)w1);
                }
                if (f == 2u) {
                    if (!gdx_stage(host, GDX_FILTER_COEF_BYTES, GDX_XFORM_S16,
                                   /*out*/0, /*persist*/0, /*copyIn*/1, &phys)) {
                        GDX_LLE_BAIL("FILTER coef scratch overflow%s", "");
                    }
                } else {
                    if (!gdx_stage(host, GDX_FILTER_STATE_BYTES, GDX_XFORM_S16,
                                   /*out*/1, /*persist*/1, /*copyIn*/1, &phys)) {
                        GDX_LLE_BAIL("FILTER state scratch overflow%s", "");
                    }
                }
                scratchCmds[i * 2u + 1u] = phys;
                break;
            }

            /* ---- Non-address opcodes: w1 is packed DMEM offsets / immediates, NEVER a
             * pointer (verified against n64_audio_hle.c -- none of these call the address
             * resolvers). Nothing to marshal; the ucode runs them straight from the list.
             * NOTE: A_ENVSETUP2's w1 is TWO immediate volume words here (EXPANSION_KIT
             * ABI), NOT a state address -- the HLE decodes it as `(w1>>16)&0xFFFF` /
             * `w1&0xFFFF`, so despite the general N64 lore it is not address-bearing in
             * this build. A_ENVMIXER's w1 is a packed DMEM-dest word (union field named
             * `addr` but not a pointer). A_HILOGAIN's w1 high half is a DMEM offset. ---- */
            case GDX_A_SPNOOP:
            case GDX_A_CLEARBUFF:
            case GDX_A_SETBUFF:
            case GDX_A_DMEMMOVE:
            case GDX_A_MIXER:
            case GDX_A_ADDMIXER:
            case GDX_A_INTERLEAVE:
            case GDX_A_INTERL:
            case GDX_A_ENVSETUP1:
            case GDX_A_ENVSETUP2:
            case GDX_A_ENVMIXER:
            case GDX_A_HILOGAIN:
            /* SDK-unknown / unused-by-this-decomp opcodes: carry no RDRAM address (all
             * DMEM-internal or no-ops in synthesis.c). The real ucode implements them;
             * we pass them through untouched. */
            case GDX_A_UNK3:
            case GDX_A_RESAMPLE_ZOH:
            case GDX_A_UNK19:
            case GDX_A_DUPLICATE:
                break;

            /* ---- Genuinely unknown opcode: we cannot prove w1 isn't an address we'd
             * fail to marshal, so degrade this tick to the HLE rather than risk feeding
             * the RSP a bad DMA source. ---- */
            default:
                GDX_LLE_BAIL("unknown opcode=%u w0=%08X w1=%08X", (unsigned)op,
                             (unsigned)w0, (unsigned)w1);
        }
    }

    /* --- Build the 64-byte OSTask (host-native little-endian). aspMain's boot reads
     * exactly these four fields (verified in the ucode text disasm: lw at +0x18/+0x1C
     * off the task header, and +0x30/+0x34); everything else is zero.
     *   +0x00 type            = 2 (M_AUDTASK)
     *   +0x18 ucode_data      = scratch offset of the data section
     *   +0x1C ucode_data_size = gdx_aspmain_data_len (0x2E0)
     *   +0x30 data_ptr        = scratch offset of the rewritten command list
     *   +0x34 data_size       = dataSizeBytes --- */
    memset(ostask, 0, sizeof(ostask));
    ostask[0x00u / 4u] = 2u;
    ostask[0x18u / 4u] = sDataSecPhys;
    ostask[0x1Cu / 4u] = (uint32_t)gdx_aspmain_data_len;
    ostask[0x30u / 4u] = cmdListPhys;
    ostask[0x34u / 4u] = dataSizeBytes;

    /* --- Run one aspMain task to the task-complete BREAK. --- */
    gdx_rsp_lle_run_task(gdx_aspmain_text, (unsigned)gdx_aspmain_text_len,
                         gdx_aspmain_data, (unsigned)gdx_aspmain_data_len,
                         ostask);

    /* Watchdog: if the ucode never reached BREAK (mis-marshalled/runaway task), the
     * driver longjmp'd out after the instruction cap. Discard the (partial) scratch
     * output -- no host buffer has been mutated yet, copy-out is below -- and fall back
     * to the HLE for this tick so a marshalling bug degrades gracefully instead of
     * spinning the audio thread. */
    if (!gdx_rsp_lle_completed()) {
        if (gdx_diag_verbose()) {
            gdx_lle_logf("[audio-lle] tick WATCHDOG-TIMEOUT: cmds=%u bytes=%u -> HLE fallback\n",
                         (unsigned)count, (unsigned)dataSizeBytes);
        }
        gdx_audio_hle_run(dataPtr, dataSizeBytes);
        return;
    }

    /* --- Copy back every RSP-written buffer (reverb rings + AI output PCM + in/out
     * state) to its host home, applying the inverse (== same) transform. The AI PCM
     * especially must land back in host memory so the existing AI pipeline plays it. --- */
    for (i = 0; i < (uint32_t)sMapCount; i++) {
        if (sMap[i].isOutput) {
            gdx_copy_out((void*)sMap[i].host, sMap[i].phys, sMap[i].size, sMap[i].xf);
        }
    }

    if (gdx_diag_verbose()) {
        gdx_lle_logf("[audio-lle] tick OK: cmds=%u bytes=%u maps=%d tickUsed=%X persistSlots=%d status=%08X\n",
                     (unsigned)count, (unsigned)dataSizeBytes, sMapCount,
                     (unsigned)(sTickBump - GDX_LLE_TICK_OFF), sPersistCount,
                     (unsigned)gdx_rsp_lle_status());
    }

    #undef GDX_LLE_BAIL
}

/* ==========================================================================
 * Entry point (UNCHANGED toggle + HLE fallback contract). When LLE is enabled the
 * bridge takes over; the bridge itself defers to gdx_audio_hle_run on any trouble.
 * ========================================================================== */
void gdx_audio_lle_run(const void* dataPtr, unsigned int dataSizeBytes) {
    if (!gdx_audio_lle_enabled()) {
        gdx_audio_hle_run(dataPtr, dataSizeBytes);
        return;
    }
    gdx_audio_lle_bridge_run(dataPtr, dataSizeBytes);
}
