// G-Diffuser -- software (HLE) implementation of the N64 audio RSP microcode ("aspMain").
//
// THE GAP THIS FILE FILLS
// ------------------------------------------------------------------------------------------
// The decomp's CPU-side audio driver (decomp/src/audio/disk/lib/synthesis.c) only ever BUILDS
// an RSP command list (the "Acmd" ABI documented in decomp/include/PR/abi.h) -- on real
// hardware the RSP's aspMain microcode (decomp/src/rsp/aspmain.s, an un-decompiled .incbin
// blob) executes that list to actually VADPCM-decode / resample / envelope-mix / interleave
// samples into the AI output buffer. On this host port nothing ever ran that list
// (port/n64_sched.c acked M_AUDTASK without executing it, same as the GFX task path acks
// unknown ucodes), so AudioSynth_Update produced command lists that nobody consumed: the
// sequencer ticked, tasks were created, osAiSetNextBuffer was wired to the real SDL player --
// but the buffers handed to it were never filled with real PCM. This file is that missing
// DSP stage.
//
// It is an ORIGINAL implementation written from the ABI's public field layouts (abi.h) and
// the well known, widely documented N64 "ABI2" audio semantics (the same command set/algorithm
// family independently reimplemented by e.g. the SM64 PC port's mixer.c and the vgmstream
// project's N64 VADPCM decoders) -- no code was copied from any GPL source.
//
// ------------------------------------------------------------------------------------------
// THE POINTER-TRUNCATION HAZARD (why this can't just dereference Acmd words as host pointers)
// ------------------------------------------------------------------------------------------
// Acmd's `Awords { u32 w0; u32 w1; }` (PR/abi.h) can only hold a 32-bit value. On real N64
// hardware the values synthesis.c packs into `w1` for LOADBUFF/SAVEBUFF/ADPCM/LOADADPCM/
// SETLOOP/RESAMPLE/FILTER are already 32-bit physical/KSEG0/KSEG1 addresses, so nothing is
// lost. On this x64 host the values passed to those macros are REAL C POINTERS -- e.g.
// disk/lib/synthesis.c: `aLoadBuffer(aList++, sampleData - sampleDataStartPad, addr, aligned)`,
// `aSaveBuffer(aList++, dmem, &reverb->leftRingBuf[startPos], size)`,
// `aLoadADPCM(aList++, nEntries, gAudioCtx.curLoadedBook)`,
// `aADPCMdec(aList++, flags, synthState->synthesisBuffers->adpcmdecState)` -- and every one of
// those macros casts the pointer through (uintptr_t)/(u32) before assigning it to a u32 field
// (see PR/abi.h aLoadBuffer/aSaveBuffer/aADPCMdec/aLoadADPCM/aSetLoop/aResample/aFilter), so the
// TOP 32 BITS OF THE HOST POINTER ARE SILENTLY DISCARDED the instant the command is packed. By
// the time this interpreter reads an Acmd back out of the task's data_ptr, all it has is the
// low 32 bits of the original pointer.
//
// This is the exact same hazard port/n64_gfx_bridge.cpp already solved for GBI display-list
// command words, via gdx_register_host_range()-backed low32 reconstruction. Every pointer that
// ever gets packed into an audio command lives in one of two already-registered places:
//   - the 16MB gdx_rdram arena (Arena_Allocate-backed sample/reverb-ringbuffer/note-state
//     buffers) -- covered by gdx_resolve_registered_host_address(), or
//   - the exe's static/BSS image (gAudioCtx itself is a plain global, so its embedded arrays --
//     e.g. SynthesisReverb fields, NoteSynthesisBuffers -- resolve through the module range)
//     -- covered by gdx_resolve_module_host_address().
// Both resolvers are already exported (extern "C") from n64_gfx_bridge.cpp for exactly this
// purpose; this file reuses them unchanged instead of reimplementing address reconstruction.
// (This file does NOT modify n64_gfx_bridge.cpp -- only calls its existing exported API.)
//
// Note on OSMesg width (the OTHER half of the uintptr32 hazard class, per the prior audio-slice
// session): that hazard is about osRecvMesg writing a pointer-width OSMesg into a narrower C
// local. It does not apply here -- this file never calls osRecvMesg/osSendMesg; the task's
// `data_ptr`/`data_size` (PR/sptask.h: `u64* data_ptr; u32 data_size;`) are already pointer-width
// fields read directly out of the OSTask by n64_sched.c, so no message-queue narrowing occurs
// on that path.
//
// ------------------------------------------------------------------------------------------
// SCOPE / CONFIDENCE NOTES (read this before debugging distorted audio)
// ------------------------------------------------------------------------------------------
// High confidence (mechanical, directly derived from PR/abi.h's macros + synthesis.c's actual
// call sites, no ambiguity): A_CLEARBUFF, A_SETBUFF, A_DMEMMOVE, A_LOADBUFF, A_SAVEBUFF,
// A_LOADADPCM (byte count + DMA into a private codebook buffer), A_SETLOOP (pending pointer),
// A_MIXER, A_INTERLEAVE, A_ENVSETUP1/A_ENVSETUP2/A_ENVMIXER (block-ramped volume envelope --
// the block-of-8-samples ramp granularity is derived directly from
// AudioSynth_ProcessEnvelope's `aiBufLen >> 3` math, not guessed), A_HILOGAIN, A_S8DEC.
//
// Medium confidence (standard, publicly documented algorithms, implemented from memory of the
// widely mirrored N64/GC "VADPCM" order-2 fixed-point predictor scheme -- codebook layout
// verified against this repo's own AdpcmBook comment "size 8*order*numPredictors"): A_ADPCM
// decode math (coefficient indexing / Q11 shift) and A_RESAMPLE (linear interpolation instead
// of the RSP's exact polyphase filter -- audibly close, not bit-exact).
//
// Deliberately stubbed as safe no-ops (left the DMEM buffer unchanged) because the real
// semantics are under-documented and these are secondary/aesthetic paths, NOT required for
// primary note audibility: A_FILTER (reverb low-pass / per-note comb filter -- reverb still
// passes through via aMix, just without the low-pass coloring) and A_UNK19 (an SDK-unknown
// opcode, only triggered for the rare bookOffset==3 note path). A_INTERL is implemented as a
// best-guess decimation (out[k] = in[2k]) for the rare "two-part" note split path.
//
// KNOWN OPEN QUESTION (documented, not fixed here -- out of this slice's scope): whether
// ROM-sourced 16-bit audio data (ADPCM codebooks, loop predictor-history) is byte-swapped
// anywhere in this port's asset/DMA pipeline before decomp C code reads it as host `s16`.
// decomp code reads these fields as ordinary native shorts everywhere (not just here), so this
// file follows that same existing assumption for consistency. If BGM comes out as harsh/
// structured static rather than recognizable (if mistuned) music, ROM audio byte order is the
// first thing to check -- it would be a pre-existing, pipeline-wide issue, not specific to this
// interpreter.
// ------------------------------------------------------------------------------------------

#include <stdint.h>
#include <string.h>

#include "port_log.h"

// ---- Cross-TU pointer resolvers (defined in port/n64_gfx_bridge.cpp; NOT modified here) ----
// Deliberately `unsigned int`, matching how these are already declared on both sides of the TU
// boundary elsewhere in the port (see port/decomp_port.c's extern decls) -- NOT uintptr_t, which
// the decomp's own stdint shim typedefs as a 32-bit type inside gdiffuser_game TUs (see engram
// discovery/uintptr32_hazard). `unsigned int` is unambiguous 32 bits on every TU in this project.
extern void* gdx_resolve_registered_host_address(unsigned int addr);
extern void* gdx_resolve_module_host_address(unsigned int addr);

static void* GdxAudioResolveAddr(uint32_t raw, const char* what) {
    void* p;
    static int sMissLogs = 0;

    if (raw == 0) {
        return NULL;
    }

    p = gdx_resolve_registered_host_address((unsigned int)raw);
    if (p != NULL) {
        return p;
    }
    p = gdx_resolve_module_host_address((unsigned int)raw);
    if (p != NULL) {
        return p;
    }

    if (sMissLogs < 16) {
        sMissLogs++;
        gdx_port_logf("[audio-hle] UNRESOLVED %s addr=%08X (op skipped)\n", what, (unsigned)raw);
    }
    return NULL;
}

// ---- Opcode numbers (mirror decomp/include/PR/abi.h; EXPANSION_KIT=1 for this build, which
// is why A_HILOGAIN=14 here rather than 24 -- see abi.h's #ifdef EXPANSION_KIT). ----
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

// ---- DMEM scratch (real N64 RSP DMEM is 4KB; decomp/src/audio/disk/lib/audio.h's DMEM_*
// offsets are all < 0x1000 and assume this size). All offsets are masked to this size before
// use, defensively -- a malformed/edge-case command can never corrupt memory outside sDmem. ----
#define GDX_DMEM_SIZE 0x1000u
#define GDX_DMEM_MASK (GDX_DMEM_SIZE - 1u)
static uint8_t sDmem[GDX_DMEM_SIZE];

// Private ADPCM codebook scratch. aLoadADPCM's DMA target is a fixed *internal* ucode location
// never exposed through the C ABI (the command carries no DMEM parameter), so this interpreter
// is free to keep it anywhere -- a side buffer, entirely private to this file, is simplest.
#define GDX_ADPCM_BOOK_MAX_BYTES 4096u
static uint8_t sAdpcmBook[GDX_ADPCM_BOOK_MAX_BYTES];
static uint32_t sAdpcmBookLen = 0;

typedef struct {
    uint32_t w0;
    uint32_t w1;
} GdxAcmd;

static int16_t ClampS16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static int16_t DmemGetS16(uint32_t byteOffset) {
    int16_t v;
    memcpy(&v, &sDmem[byteOffset & GDX_DMEM_MASK & ~1u], sizeof(v));
    return v;
}

static void DmemSetS16(uint32_t byteOffset, int16_t v) {
    memcpy(&sDmem[byteOffset & GDX_DMEM_MASK & ~1u], &v, sizeof(v));
}

static uint8_t DmemGetU8(uint32_t byteOffset) {
    return sDmem[byteOffset & GDX_DMEM_MASK];
}

static void DmemSetU8(uint32_t byteOffset, uint8_t v) {
    sDmem[byteOffset & GDX_DMEM_MASK] = v;
}

// ---- ADPCM codebook access: per-predictor block is `8 * order` shorts (verified against
// decomp/src/audio/disk/lib/audio.h's AdpcmBook comment "size (8 * order * numPredictors)").
// The real aspMain ucode's ADPCM decode is fixed-function order-2 (well documented N64 SDK
// fact), so `order` is hardcoded to 2 here regardless of what a book's header.order says. ----
static int16_t BookCoef(uint32_t predictorIndex, uint32_t tap /* 0 or 1 */, uint32_t col /* 0..7 */) {
    uint32_t idx = (predictorIndex * 16u) + (tap * 8u) + col; /* 8*order(2) = 16 shorts/predictor */
    uint32_t byteOff = idx * 2u;
    int16_t v;
    if (byteOff + 1u >= sAdpcmBookLen) {
        return 0;
    }
    memcpy(&v, &sAdpcmBook[byteOff], sizeof(v));
    return v;
}

// ---- Pending "buffer descriptor" set by A_SETBUFF, consumed by the following A_ADPCM /
// A_S8DEC / A_RESAMPLE (mirrors the real RSP's internal input/output/count registers). ----
typedef struct {
    uint32_t dmemIn;
    uint32_t dmemOut;
    uint32_t count; /* bytes */
} GdxBufDesc;

// ---- A_ADPCM: standard order-2 N64/GC-style VADPCM decode (16 samples per frame: 1 header
// byte + 8 data bytes of 4-bit nibbles, or 5 bytes total -- 4 data bytes of 2-bit nibbles --
// for the "small ADPCM" flags|4 variant). state is the persistent history pointer (real host
// memory -- NoteSynthesisBuffers.adpcmdecState[16] or SynthesisReverb-embedded AdpcmLoop, never
// otherwise read/written by decomp C, so this file is free to choose its own internal layout:
// state[0]/state[1] hold the last two decoded PCM samples). loopStatePtr is the most recent
// A_SETLOOP target (already resolved), used as the input history source instead of `state` when
// flags has A_LOOP set (matches the real ucode: SETLOOP overrides where history is READ from;
// the running state is still always WRITTEN to `state` afterwards). ----
static void RunAdpcm(const GdxBufDesc* buf, uint32_t flags, int16_t* state, const int16_t* loopState) {
    int smallAdpcm = (flags & 4) != 0;
    int isInit = (flags & 1) != 0;   /* A_INIT */
    int isLoop = (flags & 2) != 0;   /* A_LOOP */
    int16_t hist1, hist2;
    uint32_t numOutSamples = buf->count / 2u;
    uint32_t numFrames = numOutSamples / 16u; /* SAMPLES_PER_FRAME */
    uint32_t inCursor = buf->dmemIn;
    uint32_t outCursor = buf->dmemOut;
    uint32_t f;

    if (state == NULL) {
        return;
    }

    if (isInit) {
        hist1 = 0;
        hist2 = 0;
    } else if (isLoop && loopState != NULL) {
        hist1 = loopState[0];
        hist2 = loopState[1];
    } else {
        hist1 = state[0];
        hist2 = state[1];
    }

    for (f = 0; f < numFrames; f++) {
        uint8_t header = DmemGetU8(inCursor);
        uint32_t predIdx = (header >> 4) & 0xF;
        uint32_t shift = header & 0xF;
        uint32_t dataBytes = smallAdpcm ? 4u : 8u;
        uint32_t frameBytes = 1u + dataBytes;
        uint32_t nibblesPerByte = smallAdpcm ? 4u : 2u; /* 2-bit vs 4-bit nibbles */
        uint32_t s;

        for (s = 0; s < 16u; s++) {
            int32_t nibble;
            int32_t residual;
            int32_t predicted;
            int32_t sampleOut;
            uint8_t byteVal = DmemGetU8(inCursor + 1u + (s / nibblesPerByte));

            if (smallAdpcm) {
                uint32_t shiftInByte = (3u - (s % 4u)) * 2u;
                nibble = (int32_t)((byteVal >> shiftInByte) & 0x3u);
                if (nibble & 0x2) nibble -= 4; /* sign-extend 2-bit */
            } else {
                uint32_t shiftInByte = (s % 2u == 0u) ? 4u : 0u;
                nibble = (int32_t)((byteVal >> shiftInByte) & 0xFu);
                if (nibble & 0x8) nibble -= 16; /* sign-extend 4-bit */
            }

            residual = nibble << shift;
            predicted = ((int32_t)BookCoef(predIdx, 0, s & 7u) * (int32_t)hist1 +
                         (int32_t)BookCoef(predIdx, 1, s & 7u) * (int32_t)hist2) >> 11;
            sampleOut = predicted + residual;

            hist2 = hist1;
            hist1 = ClampS16(sampleOut);

            DmemSetS16(outCursor + s * 2u, hist1);
        }

        inCursor += frameBytes;
        outCursor += 32u; /* 16 samples * 2 bytes */
    }

    /* Always write back the running history for the next call -- matches real hardware, which
       uses `state` as both the (conditional) input source and the unconditional output. */
    state[0] = hist1;
    state[1] = hist2;
}

// ---- A_S8DEC: signed 8-bit PCM -> 16-bit (sign-extend + scale). Stateless in practice; no
// persistent history is meaningfully needed for a non-predictive codec. ----
static void RunS8Dec(const GdxBufDesc* buf) {
    uint32_t numSamples = buf->count / 2u;
    uint32_t i;
    for (i = 0; i < numSamples; i++) {
        int8_t raw = (int8_t)DmemGetU8(buf->dmemIn + i);
        DmemSetS16(buf->dmemOut + i * 2u, (int16_t)((int32_t)raw * 256));
    }
}

// ---- A_RESAMPLE: linear-interpolation resampler. pitch is Q15 (UNITY_PITCH=0x8000 == 1.0x,
// per PR/abi.h). state (16 shorts, private layout): state[0]=fractional position (Q16, low 16
// bits only), state[1]=last consumed source sample (continuity across calls). Not bit-exact to
// the RSP's real polyphase filter, but produces audibly correct pitch/duration. ----
static void RunResample(const GdxBufDesc* buf, uint32_t flags, int16_t* state) {
    int isInit = (flags & 1) != 0; /* A_INIT */
    uint32_t pitchQ15;
    uint32_t fracQ16;
    int16_t lastSample;
    uint32_t numOutSamples = buf->count / 2u;
    uint32_t srcIdx = 0;
    uint32_t n;

    if (state == NULL) {
        return;
    }

    /* pitch is threaded in via the caller, which stashes it into state[15] before calling --
       see the A_RESAMPLE case in the main dispatch loop below. */
    pitchQ15 = (uint32_t)(uint16_t)state[15];

    if (isInit) {
        fracQ16 = 0;
        lastSample = DmemGetS16(buf->dmemIn);
    } else {
        fracQ16 = ((uint32_t)(uint16_t)state[0]);
        lastSample = state[1];
    }

    for (n = 0; n < numOutSamples; n++) {
        int16_t s1 = DmemGetS16(buf->dmemIn + srcIdx * 2u);
        int32_t out = (int32_t)lastSample +
                      (((int32_t)s1 - (int32_t)lastSample) * (int32_t)fracQ16) / 65536;
        DmemSetS16(buf->dmemOut + n * 2u, ClampS16(out));

        fracQ16 += (pitchQ15 << 1); /* Q15 -> Q16 */
        while (fracQ16 >= 0x10000u) {
            fracQ16 -= 0x10000u;
            lastSample = s1;
            srcIdx++;
            s1 = DmemGetS16(buf->dmemIn + srcIdx * 2u);
        }
    }

    state[0] = (int16_t)(uint16_t)fracQ16;
    state[1] = lastSample;
}

// ---- Main interpreter entry point. Called from port/n64_sched.c's osSpTaskStartGo when an
// M_AUDTASK is submitted (the same place M_GFXTASK is routed to gdx_gfx_run). `dataPtr`/
// `dataSizeBytes` come directly from the OSTask's `t.data_ptr`/`t.data_size` -- already real,
// full-width host values (PR/sptask.h: `u64* data_ptr;`), NOT subject to the Acmd-word
// truncation hazard described above (that hazard is only inside the command payload itself). --
void gdx_audio_hle_run(const void* dataPtr, unsigned int dataSizeBytes) {
    const GdxAcmd* cmds = (const GdxAcmd*)dataPtr;
    uint32_t count = dataSizeBytes / (uint32_t)sizeof(GdxAcmd);
    uint32_t i;

    /* Pending A_SETBUFF descriptor, consumed by the following A_ADPCM/A_S8DEC/A_RESAMPLE. */
    GdxBufDesc pendingBuf = { 0, 0, 0 };

    /* Pending A_SETLOOP target (resolved host pointer), consumed by the following A_ADPCM when
       its flags include A_LOOP. Reset every call: synthesis.c always emits aSetLoop immediately
       before the aADPCMdec that needs it, within the same command list. */
    const int16_t* pendingLoopState = NULL;

    /* Pending A_ENVSETUP1/A_ENVSETUP2 state, consumed by the following A_ENVMIXER (mirrors the
       real RSP's internal envelope-mixer registers). */
    int32_t envRampReverb = 0, envRampLeft = 0, envRampRight = 0;
    int32_t envCurVolLeft = 0, envCurVolRight = 0, envReverbVol2 = 0;

    static int sUnhandledLogs = 0;

    /* One-shot diagnostic (engram slice/audio-synthesis follow-up): log a full opcode
       histogram for the first 3 tasks. This discriminates the two remaining hypotheses
       for "audio buffers are all zeros with zero interpreter errors":
         (a) AudioSynth_SingleAudioUpdate's note list is empty every task (zero
             A_ADPCM/A_MIXER/A_ENVMIXER ever emitted) -- in that case zeros ARE the
             mathematically correct output (see file header), and the bug is upstream
             in the sequencer/note-allocation path, not in this interpreter; vs.
         (b) notes ARE being synthesized (nonzero counts) but still produce silence --
             in that case the bug is in THIS interpreter's ADPCM/resample/envelope math
             or in the address resolution feeding it. */
    static int sDiagTasksLogged = 0;

    if (cmds == NULL || count == 0) {
        return;
    }

    if (sDiagTasksLogged < 3) {
        uint32_t histogram[32] = { 0 };
        uint32_t k;
        for (k = 0; k < count; k++) {
            uint32_t opk = (cmds[k].w0 >> 24) & 0xFFu;
            if (opk < 32u) {
                histogram[opk]++;
            }
        }
        gdx_port_logf(
            "[audio-hle-diag] task#%d cmdCount=%u ADPCM=%u S8DEC=%u RESAMPLE=%u MIXER=%u "
            "ENVMIXER=%u CLEARBUFF=%u SAVEBUFF=%u LOADBUFF=%u INTERLEAVE=%u\n",
            sDiagTasksLogged, (unsigned)count, (unsigned)histogram[GDX_A_ADPCM],
            (unsigned)histogram[GDX_A_S8DEC], (unsigned)histogram[GDX_A_RESAMPLE],
            (unsigned)histogram[GDX_A_MIXER], (unsigned)histogram[GDX_A_ENVMIXER],
            (unsigned)histogram[GDX_A_CLEARBUFF], (unsigned)histogram[GDX_A_SAVEBUFF],
            (unsigned)histogram[GDX_A_LOADBUFF], (unsigned)histogram[GDX_A_INTERLEAVE]);
        sDiagTasksLogged++;
    }

    for (i = 0; i < count; i++) {
        uint32_t w0 = cmds[i].w0;
        uint32_t w1 = cmds[i].w1;
        uint32_t op = (w0 >> 24) & 0xFFu;

        switch (op) {
            case GDX_A_SPNOOP:
                break;

            case GDX_A_CLEARBUFF: {
                uint32_t dmem = w0 & 0xFFFFFFu; /* aClearBuffer packs dmem into the low 24 bits */
                uint32_t size = w1;
                uint32_t k;
                for (k = 0; k < size; k++) {
                    DmemSetU8(dmem + k, 0);
                }
                break;
            }

            case GDX_A_SETBUFF: {
                uint32_t flags = (w0 >> 16) & 0xFFu; /* unused by any consumer here */
                uint32_t dmemIn = w0 & 0xFFFFu;
                uint32_t dmemOut = (w1 >> 16) & 0xFFFFu;
                uint32_t cnt = w1 & 0xFFFFu;
                (void)flags;
                pendingBuf.dmemIn = dmemIn;
                pendingBuf.dmemOut = dmemOut;
                pendingBuf.count = cnt;
                break;
            }

            case GDX_A_DMEMMOVE: {
                uint32_t dmemIn = w0 & 0xFFFFFFu;
                uint32_t dmemOut = (w1 >> 16) & 0xFFFFu;
                uint32_t cnt = w1 & 0xFFFFu;
                uint32_t k;
                /* Byte-by-byte to safely handle overlap in either direction without relying on
                   memmove's overlap semantics against a masked/wrapping index space. */
                if (dmemOut <= dmemIn) {
                    for (k = 0; k < cnt; k++) DmemSetU8(dmemOut + k, DmemGetU8(dmemIn + k));
                } else {
                    for (k = cnt; k > 0; k--) DmemSetU8(dmemOut + k - 1u, DmemGetU8(dmemIn + k - 1u));
                }
                break;
            }

            case GDX_A_LOADBUFF: {
                uint32_t size = ((w0 >> 16) & 0xFFu) << 4;
                uint32_t dmemDest = w0 & 0xFFFFu;
                void* src = GdxAudioResolveAddr(w1, "LOADBUFF");
                if (src != NULL) {
                    uint32_t k;
                    const uint8_t* s = (const uint8_t*)src;
                    for (k = 0; k < size; k++) DmemSetU8(dmemDest + k, s[k]);
                }
                break;
            }

            case GDX_A_SAVEBUFF: {
                uint32_t size = ((w0 >> 16) & 0xFFu) << 4;
                uint32_t dmemSrc = w0 & 0xFFFFu;
                void* dst = GdxAudioResolveAddr(w1, "SAVEBUFF");
                if (dst != NULL) {
                    uint32_t k;
                    uint8_t* d = (uint8_t*)dst;
                    for (k = 0; k < size; k++) d[k] = DmemGetU8(dmemSrc + k);
                }
                break;
            }

            case GDX_A_LOADADPCM: {
                uint32_t byteCount = w0 & 0xFFFFFFu;
                void* src = GdxAudioResolveAddr(w1, "LOADADPCM");
                if (byteCount > GDX_ADPCM_BOOK_MAX_BYTES) {
                    byteCount = GDX_ADPCM_BOOK_MAX_BYTES;
                }
                if (src != NULL) {
                    memcpy(sAdpcmBook, src, byteCount);
                    sAdpcmBookLen = byteCount;
                }
                break;
            }

            case GDX_A_SETLOOP: {
                pendingLoopState = (const int16_t*)GdxAudioResolveAddr(w1, "SETLOOP");
                break;
            }

            case GDX_A_ADPCM: {
                uint32_t flags = (w0 >> 16) & 0xFFu;
                int16_t* state = (int16_t*)GdxAudioResolveAddr(w1, "ADPCM-state");
                RunAdpcm(&pendingBuf, flags, state, pendingLoopState);
                break;
            }

            case GDX_A_S8DEC: {
                /* State pointer resolved for parity with real hardware but unused: S8 is a
                   non-predictive codec (see RunS8Dec). */
                (void)GdxAudioResolveAddr(w1, "S8DEC-state");
                RunS8Dec(&pendingBuf);
                break;
            }

            case GDX_A_RESAMPLE: {
                uint32_t flags = (w0 >> 16) & 0xFFu;
                uint32_t pitch = w0 & 0xFFFFu;
                int16_t* state = (int16_t*)GdxAudioResolveAddr(w1, "RESAMPLE-state");
                if (state != NULL) {
                    /* Stash pitch in the otherwise-unused last slot of the 16-short state
                       buffer for RunResample to pick up (keeps RunResample's signature simple;
                       this slot is never read by decomp C code, only by this interpreter). */
                    state[15] = (int16_t)(uint16_t)pitch;
                    RunResample(&pendingBuf, flags, state);
                }
                break;
            }

            case GDX_A_MIXER: {
                uint32_t count8 = (w0 >> 16) & 0xFFu; /* count>>4, i.e. groups of 8 samples */
                int32_t gain = (int16_t)(w0 & 0xFFFFu); /* Q15 signed gain */
                uint32_t dmemIn = (w1 >> 16) & 0xFFFFu;
                uint32_t dmemOut = w1 & 0xFFFFu;
                uint32_t numSamples = count8 * 8u;
                uint32_t k;
                for (k = 0; k < numSamples; k++) {
                    int32_t in = DmemGetS16(dmemIn + k * 2u);
                    int32_t out = DmemGetS16(dmemOut + k * 2u);
                    out = out + ((in * gain) >> 15);
                    DmemSetS16(dmemOut + k * 2u, ClampS16(out));
                }
                break;
            }

            case GDX_A_ADDMIXER: {
                /* Not exercised by disk/lib/synthesis.c today; implemented defensively for
                   robustness against future callers. Treats the low 16 bits of w0 as a Q15
                   gain, mirroring A_MIXER. */
                uint32_t count8 = (w0 >> 16) & 0xFFu;
                int32_t gain = (int16_t)(w0 & 0xFFFFu);
                uint32_t dmemIn = (w1 >> 16) & 0xFFFFu;
                uint32_t dmemOut = w1 & 0xFFFFu;
                uint32_t numSamples = count8 * 8u;
                uint32_t k;
                for (k = 0; k < numSamples; k++) {
                    int32_t in = DmemGetS16(dmemIn + k * 2u);
                    int32_t out = DmemGetS16(dmemOut + k * 2u);
                    out = out + ((in * gain) >> 15);
                    DmemSetS16(dmemOut + k * 2u, ClampS16(out));
                }
                break;
            }

            case GDX_A_INTERLEAVE: {
                uint32_t dmemOut = w0 & 0xFFFFu;
                uint32_t dmemL = (w1 >> 16) & 0xFFFFu;
                uint32_t dmemR = w1 & 0xFFFFu;
                uint32_t byteCount = ((w0 >> 16) & 0xFFu) << 4; /* per-channel bytes, c>>4 packed */
                uint32_t numSamples = byteCount / 2u;
                uint32_t k;
                for (k = 0; k < numSamples; k++) {
                    DmemSetS16(dmemOut + k * 4u + 0u, DmemGetS16(dmemL + k * 2u));
                    DmemSetS16(dmemOut + k * 4u + 2u, DmemGetS16(dmemR + k * 2u));
                }
                break;
            }

            case GDX_A_INTERL: {
                /* Rare "two-part" note-splitting path (nParts==2). Best-guess decimation:
                   extract every other sample. See file header scope notes. */
                uint32_t numSamples = w0 & 0xFFFFu;
                uint32_t dmemIn = (w1 >> 16) & 0xFFFFu;
                uint32_t dmemOut = w1 & 0xFFFFu;
                uint32_t k;
                for (k = 0; k < numSamples; k++) {
                    DmemSetS16(dmemOut + k * 2u, DmemGetS16(dmemIn + k * 4u));
                }
                break;
            }

            case GDX_A_ENVSETUP1: {
                int32_t a = (int32_t)((w0 >> 16) & 0xFFu);
                int32_t b = (int32_t)(int16_t)(w0 & 0xFFFFu);
                int32_t c = (int32_t)(int16_t)((w1 >> 16) & 0xFFFFu);
                int32_t d = (int32_t)(int16_t)(w1 & 0xFFFFu);
                envReverbVol2 = a;
                envRampReverb = b;
                envRampLeft = c;
                envRampRight = d;
                break;
            }

            case GDX_A_ENVSETUP2: {
                envCurVolLeft = (int32_t)(uint16_t)((w1 >> 16) & 0xFFFFu);
                envCurVolRight = (int32_t)(uint16_t)(w1 & 0xFFFFu);
                break;
            }

            case GDX_A_ENVMIXER: {
                /* EK macro: w0 = bits(opcode) | (dmemi>>4)<<16(8) | count<<8(8) | swapLR<<4(1) |
                   x0<<3 | x1<<2 | x2<<1 | x3<<0. `count` is the RAW sample count for this call
                   (e.g. aiBufLen for the chunk -- NOT pre-shifted), unlike dmemi which IS
                   pre-shifted by 4. w1 = m (the dmemDests word packed via AUDIO_MK_CMD -- NOT a
                   pointer, despite the union field being named `addr` in PR/abi.h). */
                uint32_t dmemSrc = ((w0 >> 16) & 0xFFu) << 4;
                uint32_t sampleCount = (w0 >> 8) & 0xFFu;
                uint32_t swapLR = (w0 >> 4) & 1u;
                uint32_t dmemDests = w1;
                uint32_t dryLeftDmem = ((dmemDests >> 24) & 0xFFu) << 4;
                uint32_t dryRightDmem = ((dmemDests >> 16) & 0xFFu) << 4;
                uint32_t wetLeftDmem = ((dmemDests >> 8) & 0xFFu) << 4;
                uint32_t wetRightDmem = (dmemDests & 0xFFu) << 4;
                /* Ramps advance once per block of 8 samples (see AudioSynth_ProcessEnvelope's
                   `aiBufLen >> 3` ramp-step math -- `sampleCount` here is always a multiple of 8). */
                uint32_t numBlocks = sampleCount >> 3;
                int32_t curVolL = envCurVolLeft, curVolR = envCurVolRight, curReverb = envReverbVol2;
                uint32_t sIdx = 0;
                uint32_t blk;

                if (swapLR) {
                    uint32_t tmp = dryLeftDmem; dryLeftDmem = dryRightDmem; dryRightDmem = tmp;
                }

                for (blk = 0; blk < numBlocks; blk++) {
                    uint32_t n;
                    for (n = 0; n < 8u && sIdx < sampleCount; n++, sIdx++) {
                        int32_t s = DmemGetS16(dmemSrc + sIdx * 2u);
                        int32_t dl = (s * curVolL) >> 16;
                        int32_t dr = (s * curVolR) >> 16;
                        /* Mono voice: the same wet send feeds both wet channels equally (the
                           exotic x0..x3 headset-pan/stereo-strong effects that would
                           differentiate them are deliberately not implemented -- see file
                           header scope notes). */
                        int32_t wet = (s * curReverb) >> 8;

                        DmemSetS16(dryLeftDmem + sIdx * 2u,
                                   ClampS16(DmemGetS16(dryLeftDmem + sIdx * 2u) + dl));
                        DmemSetS16(dryRightDmem + sIdx * 2u,
                                   ClampS16(DmemGetS16(dryRightDmem + sIdx * 2u) + dr));
                        DmemSetS16(wetLeftDmem + sIdx * 2u,
                                   ClampS16(DmemGetS16(wetLeftDmem + sIdx * 2u) + wet));
                        DmemSetS16(wetRightDmem + sIdx * 2u,
                                   ClampS16(DmemGetS16(wetRightDmem + sIdx * 2u) + wet));
                    }
                    curVolL += envRampLeft;
                    curVolR += envRampRight;
                    curReverb += envRampReverb;
                }
                break;
            }

            case GDX_A_HILOGAIN: {
                int32_t gain = (int32_t)((w0 >> 16) & 0xFFu); /* Q4, 0x10 == 1.0x */
                uint32_t size = w0 & 0xFFFFu;
                uint32_t dmemIn = (w1 >> 16) & 0xFFFFu;
                uint32_t dmemOut = w1 & 0xFFFFu;
                uint32_t numSamples = size / 2u;
                uint32_t k;
                for (k = 0; k < numSamples; k++) {
                    int32_t s = DmemGetS16(dmemIn + k * 2u);
                    DmemSetS16(dmemOut + k * 2u, ClampS16((s * gain) >> 4));
                }
                break;
            }

            case GDX_A_FILTER: {
                /* Stubbed as a safe passthrough -- see file header scope notes. Still resolves
                   the address for parity/diagnostics, but does not touch DMEM. */
                (void)GdxAudioResolveAddr(w1, "FILTER");
                break;
            }

            case GDX_A_UNK19:
                /* SDK-unknown opcode; only used for the rare bookOffset==3 note path. Safe
                   no-op (src==dst in the one call site, so leaving DMEM untouched cannot drop
                   samples that another op still needs). */
                break;

            case GDX_A_UNK3:
            case GDX_A_RESAMPLE_ZOH:
            case GDX_A_DUPLICATE:
                /* Not used by decomp/src/audio/disk/lib/synthesis.c; no-op. */
                break;

            default:
                if (sUnhandledLogs < 16) {
                    sUnhandledLogs++;
                    gdx_port_logf("[audio-hle] unhandled opcode=%u w0=%08X w1=%08X (skipped)\n",
                                  (unsigned)op, (unsigned)w0, (unsigned)w1);
                }
                break;
        }
    }
}
