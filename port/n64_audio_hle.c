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
// Note on OSMesg width (the OTHER half of the uintptr32 hazard class): that hazard is about
// osRecvMesg writing a pointer-width OSMesg into a narrower C
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
// A_LOADADPCM (byte count + DMA into a private codebook buffer), A_SETLOOP (file-scope persistent
// pointer, see sPendingLoopState -- revamp v2 task A1), A_MIXER, A_INTERLEAVE, A_ADDMIXER
// (gainless clamped unity add, per mupen64plus-rsp-hle alist_add -- revamp v2 task A6),
// A_ENVSETUP1/A_ENVSETUP2/A_ENVMIXER (block-ramped volume envelope, wet cascaded off the
// dry-scaled sample per channel -- nead alist.c#L512-562 envmix_nead semantics, revamp v2 task
// A2 -- the block-of-8-samples ramp granularity is derived directly from
// AudioSynth_ProcessEnvelope's `aiBufLen >> 3` math, not guessed), A_HILOGAIN, A_S8DEC.
//
// Medium confidence (standard, publicly documented algorithms, implemented from memory of the
// widely mirrored N64/GC "VADPCM" order-2 fixed-point predictor scheme -- codebook layout
// verified against this repo's own AdpcmBook comment "size 8*order*numPredictors"): A_ADPCM
// decode math (coefficient indexing / Q11 shift; deferred intra-frame clamping per
// mupen64plus-rsp-hle audio.c#L109-127, revamp v2 task A4); A_RESAMPLE (4-tap polyphase FIR using
// the EXACT 64-phase Q15 ROM coefficient table transcribed from the canonical RSP audio-HLE
// RESAMPLE_LUT -- bit-exact to the ucode, see RunResample's table comment -- now with a 4-sample
// persisted pre-roll history and zero-primed A_INIT plus rounded-up-to-8-samples output count,
// per mupen64plus-rsp-hle alist.c#L621-639, revamp v2 task A5); A_FILTER (two-averaged-LUT,
// cross-block-windowed FIR reimplemented from alist_filter's documented BEHAVIOR -- see
// RunFilter's header comment for exactly what was and wasn't adopted from the GPLv2 reference,
// still gated by GDX_HLE_FILTER).
//
// Deliberately a safe no-op (left the DMEM buffer unchanged) because the semantics are under-
// documented and it's a secondary/rare path, NOT required for primary note audibility:
// A_UNK19 (an SDK-unknown opcode, only triggered for the rare bookOffset==3 note path).
// A_INTERL is implemented as a best-guess decimation (out[k] = in[2k]) for the rare
// "two-part" note split path.
//
// KNOWN OPEN QUESTION: whether
// ROM-sourced 16-bit audio data (ADPCM codebooks, loop predictor-history) is byte-swapped
// anywhere in this port's asset/DMA pipeline before decomp C code reads it as host `s16`.
// decomp code reads these fields as ordinary native shorts everywhere (not just here), so this
// file follows that same existing assumption for consistency. If BGM comes out as harsh/
// structured static rather than recognizable (if mistuned) music, ROM audio byte order is the
// first thing to check -- it would be a pre-existing, pipeline-wide issue, not specific to this
// interpreter.
// ------------------------------------------------------------------------------------------

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "port_log.h"
#include "gdx_dev_gates.h" /* Dev Tools gate layer: GDX_SEQ_ADPCM / GDX_HLE_FILTER / GDX_NO_REVERB */

// ---- Cross-TU pointer resolvers (defined in port/n64_gfx_bridge.cpp; NOT modified here) ----
// Deliberately `unsigned int`, matching how these are already declared on both sides of the TU
// boundary elsewhere in the port (see port/decomp_port.c's extern decls) -- NOT uintptr_t, which
// the decomp's own stdint shim typedefs as a 32-bit type inside gdiffuser_game TUs (see engram
// discovery/uintptr32_hazard). `unsigned int` is unambiguous 32 bits on every TU in this project.
extern void* gdx_resolve_registered_host_address(unsigned int addr);
extern void* gdx_resolve_module_host_address(unsigned int addr);

/* ---- LUS console-variable read (C ABI, defined in libultraship) ----
 * Declared here so this C TU need not pull the C++ bridge header -- the exact same pattern
 * gdx_audio_lle.c uses for the LLE engine toggle. Backs the live reverb kill switch below
 * (gEnhancements.Audio.Reverb). */
extern int CVarGetInteger(const char* name, int defaultValue);

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
/* [spike] last-writer tracker: every DMEM write records the opcode
   that made it, per 16-byte block, so a spike found at interleave time can NAME its
   writer instead of us inferring one. 0xFF = never written this session. */
static uint8_t sDmemLastOp[GDX_DMEM_SIZE >> 4];
static uint8_t sDmemCurOp = 0xFF;

/* File-based audio-stage bypass toggles (stage localization by ear). Env vars proved
   unreliable in the target shell, so read flags ONCE from gdx-audio-debug.txt next
   to the exe. Space/newline-separated keywords, any subset:
     nofilter  -- skip the per-voice A_FILTER (8-tap FIR)
     flatvol   -- envmixer applies constant volume per tick (no 8-block ramp staircase)
     noreverb  -- skip the reverb wet->dry return
     nointerl  -- skip the nParts==2 "best-guess decimation" path
   Enable ONE at a time to attribute an artifact to a stage. */
static int GdxAudioDbg(void) {
    static int sFlags = -1;
    if (sFlags == -1) {
        FILE* f = fopen("gdx-audio-debug.txt", "r");
        sFlags = 0;
        if (f != NULL) {
            char buf[256];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = '\0';
            fclose(f);
            if (strstr(buf, "nofilter") != NULL) sFlags |= 1;
            if (strstr(buf, "flatvol") != NULL) sFlags |= 2;
            if (strstr(buf, "noreverb") != NULL) sFlags |= 4;
            if (strstr(buf, "nointerl") != NULL) sFlags |= 8;
        }
        gdx_port_logf("[audio] stage-bypass flags = 0x%X (gdx-audio-debug.txt)\n", (unsigned)sFlags);
    }
    return sFlags;
}

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
    sDmemLastOp[(byteOffset & GDX_DMEM_MASK) >> 4] = sDmemCurOp;
}

static uint8_t DmemGetU8(uint32_t byteOffset) {
    return sDmem[byteOffset & GDX_DMEM_MASK];
}

static void DmemSetU8(uint32_t byteOffset, uint8_t v) {
    sDmem[byteOffset & GDX_DMEM_MASK] = v;
    sDmemLastOp[(byteOffset & GDX_DMEM_MASK) >> 4] = sDmemCurOp;
}

// Pending buffer descriptor set by A_SETBUFF and consumed by decode/resample commands.
typedef struct GdxBufDesc {
    uint32_t dmemIn;
    uint32_t dmemOut;
    uint32_t count;
} GdxBufDesc;

// ---- Unlock-jingle HLE stage capture -----------------------------------------
//
// GDX_DIAG_UNLOCK already identifies the sequence-channel notes on the CPU side.
// synthesis.c registers the exact Acmd range and persistent state addresses for
// each target note before the command list is submitted. Range attribution lets
// the HLE capture only the unlock voices without changing the command list or
// guessing from sample contents.
//
// Four timeline-aligned WAVs are produced after exactly two seconds:
//   resample      target voices immediately after A_RESAMPLE
//   pre-envelope  target voices after optional gain/filter, at A_ENVMIXER input
//   envelope      target-only dry L/R contributions after envelope/pan
//   mix           complete dry L/R buses immediately before A_INTERLEAVE
//
// Decode and resampler-input samples are summarized in the diagnostic log rather
// than emitted as WAVs: those buffers run in source-sample space and contain
// per-call overlap/preamble, so concatenating them would create artificial seams
// that could be mistaken for the defect under investigation. The capture is
// dormant until a target note is registered and has no file I/O in normal play.
#define GDX_UNLOCK_STAGE_FRAMES 64000u
#define GDX_UNLOCK_STAGE_MAX_CHUNK 256u
#define GDX_UNLOCK_STAGE_MAX_TARGETS 32u
#define GDX_UNLOCK_STAGE_MAX_RANGES 128u

#define GDX_UNLOCK_STAGE_RESAMPLE_PATH "gdiffuser-unlock-hle-resample.wav"
#define GDX_UNLOCK_STAGE_PRE_ENVELOPE_PATH "gdiffuser-unlock-hle-pre-envelope.wav"
#define GDX_UNLOCK_STAGE_ENVELOPE_PATH "gdiffuser-unlock-hle-envelope.wav"
#define GDX_UNLOCK_STAGE_MIX_PATH "gdiffuser-unlock-hle-mix.wav"

typedef struct GdxUnlockStageStats {
    uint64_t samples;
    uint64_t squareSum;
    uint64_t hash;
    int64_t sampleSum;
    int16_t minSample;
    int16_t maxSample;
    int16_t previousSample;
    uint32_t maxDelta;
    uint32_t zeroSamples;
    uint32_t clippedSamples;
    int hasPreviousSample;
} GdxUnlockStageStats;

typedef struct GdxUnlockStageTarget {
    uint32_t adpcmToken;
    uint32_t resampleToken;
    uint32_t decodeCalls;
    uint32_t decodeBoundaryMaxDelta;
    int noteIndex;
    int hasDecodeLast;
    int16_t decodeLast;
} GdxUnlockStageTarget;

typedef struct GdxUnlockStageRange {
    uintptr_t start;
    uintptr_t end;
    int targetIndex;
} GdxUnlockStageRange;

typedef struct GdxUnlockStageCapture {
    unsigned int generation;
    unsigned int sampleRate;
    uint32_t frames;
    uint32_t chunks;
    uint32_t truncatedChunks;
    uint32_t resampleClipSamples;
    uint32_t preEnvelopeClipSamples;
    uint32_t envelopeClipSamples[2];
    int active;
    int complete;
    int sawHle;
    int chunkStarted;
    int currentTarget;
    uint32_t targetCount;
    uint32_t rangeCount;
    uintptr_t commandList;
    GdxUnlockStageTarget targets[GDX_UNLOCK_STAGE_MAX_TARGETS];
    GdxUnlockStageRange ranges[GDX_UNLOCK_STAGE_MAX_RANGES];
    int32_t chunkResample[GDX_UNLOCK_STAGE_MAX_CHUNK];
    int32_t chunkPreEnvelope[GDX_UNLOCK_STAGE_MAX_CHUNK];
    int32_t chunkEnvelope[2][GDX_UNLOCK_STAGE_MAX_CHUNK];
    int16_t resample[GDX_UNLOCK_STAGE_FRAMES];
    int16_t preEnvelope[GDX_UNLOCK_STAGE_FRAMES];
    int16_t envelope[GDX_UNLOCK_STAGE_FRAMES * 2u];
    int16_t mix[GDX_UNLOCK_STAGE_FRAMES * 2u];
    GdxUnlockStageStats decodeStats;
    GdxUnlockStageStats resampleInputStats;
    GdxUnlockStageStats resampleStats;
    GdxUnlockStageStats preEnvelopeStats;
    GdxUnlockStageStats envelopeStats[2];
    GdxUnlockStageStats mixStats[2];
} GdxUnlockStageCapture;

static GdxUnlockStageCapture sGdxUnlockStage;

static void GdxUnlockStageStatsReset(GdxUnlockStageStats* stats) {
    memset(stats, 0, sizeof(*stats));
    stats->hash = UINT64_C(14695981039346656037);
}

static void GdxUnlockStageStatsBreak(GdxUnlockStageStats* stats) {
    stats->hasPreviousSample = 0;
}

static void GdxUnlockStageStatsAdd(GdxUnlockStageStats* stats, int16_t sample) {
    int32_t value = sample;
    uint32_t delta;

    if (stats->samples == 0) {
        stats->minSample = sample;
        stats->maxSample = sample;
    } else {
        if (sample < stats->minSample) {
            stats->minSample = sample;
        }
        if (sample > stats->maxSample) {
            stats->maxSample = sample;
        }
    }
    stats->samples++;
    stats->sampleSum += value;
    stats->squareSum += (uint64_t)(value * value);
    stats->hash ^= (uint8_t)(uint16_t)sample;
    stats->hash *= UINT64_C(1099511628211);
    stats->hash ^= (uint8_t)((uint16_t)sample >> 8);
    stats->hash *= UINT64_C(1099511628211);
    if (sample == 0) {
        stats->zeroSamples++;
    }
    if ((sample == INT16_MIN) || (sample == INT16_MAX)) {
        stats->clippedSamples++;
    }
    if (stats->hasPreviousSample) {
        int32_t signedDelta = value - stats->previousSample;
        delta = signedDelta < 0 ? (uint32_t)-signedDelta : (uint32_t)signedDelta;
        if (delta > stats->maxDelta) {
            stats->maxDelta = delta;
        }
    }
    stats->previousSample = sample;
    stats->hasPreviousSample = 1;
}

static int16_t GdxUnlockStageClamp(int32_t value, uint32_t* clippedSamples) {
    if (value > INT16_MAX) {
        (*clippedSamples)++;
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        (*clippedSamples)++;
        return INT16_MIN;
    }
    return (int16_t)value;
}

static void GdxUnlockStagePutU16(uint8_t* dst, uint16_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void GdxUnlockStagePutU32(uint8_t* dst, uint32_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static int GdxUnlockStageWriteWav(const char* path, const int16_t* pcm, uint32_t frames,
                                  uint16_t channels, uint32_t sampleRate) {
    uint8_t header[44];
    uint8_t sampleBytes[4096];
    uint32_t dataBytes = frames * channels * (uint32_t)sizeof(int16_t);
    uint32_t sampleCount = frames * channels;
    uint32_t sampleIndex = 0;
    FILE* file;
    int wroteAll;

    header[0] = 'R';
    header[1] = 'I';
    header[2] = 'F';
    header[3] = 'F';
    GdxUnlockStagePutU32(&header[4], 36u + dataBytes);
    header[8] = 'W';
    header[9] = 'A';
    header[10] = 'V';
    header[11] = 'E';
    header[12] = 'f';
    header[13] = 'm';
    header[14] = 't';
    header[15] = ' ';
    GdxUnlockStagePutU32(&header[16], 16u);
    GdxUnlockStagePutU16(&header[20], 1u);
    GdxUnlockStagePutU16(&header[22], channels);
    GdxUnlockStagePutU32(&header[24], sampleRate);
    GdxUnlockStagePutU32(&header[28], sampleRate * channels * (uint32_t)sizeof(int16_t));
    GdxUnlockStagePutU16(&header[32], (uint16_t)(channels * sizeof(int16_t)));
    GdxUnlockStagePutU16(&header[34], (uint16_t)(sizeof(int16_t) * 8u));
    header[36] = 'd';
    header[37] = 'a';
    header[38] = 't';
    header[39] = 'a';
    GdxUnlockStagePutU32(&header[40], dataBytes);

    file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }
    wroteAll = fwrite(header, 1, sizeof(header), file) == sizeof(header);
    while (wroteAll && (sampleIndex < sampleCount)) {
        uint32_t samplesThisWrite = sampleCount - sampleIndex;
        uint32_t i;
        if (samplesThisWrite > sizeof(sampleBytes) / 2u) {
            samplesThisWrite = sizeof(sampleBytes) / 2u;
        }
        for (i = 0; i < samplesThisWrite; i++) {
            uint16_t value = (uint16_t)pcm[sampleIndex + i];
            sampleBytes[i * 2u + 0u] = (uint8_t)value;
            sampleBytes[i * 2u + 1u] = (uint8_t)(value >> 8);
        }
        wroteAll = fwrite(sampleBytes, 1, samplesThisWrite * 2u, file) == samplesThisWrite * 2u;
        sampleIndex += samplesThisWrite;
    }
    if (fclose(file) != 0) {
        wroteAll = 0;
    }
    return wroteAll;
}

static void GdxUnlockStageLogStats(const char* stage, const char* channel,
                                   const GdxUnlockStageStats* stats) {
    double mean;
    double rms;

    if (stats->samples == 0) {
        gdx_port_logf("[unlock-stage] stats stage=%s channel=%s samples=0\n", stage, channel);
        return;
    }
    mean = (double)stats->sampleSum / (double)stats->samples;
    rms = sqrt((double)stats->squareSum / (double)stats->samples);
    gdx_port_logf("[unlock-stage] stats stage=%s channel=%s samples=%llu min=%d max=%d "
                  "mean=%.3f rms=%.3f zeros=%u clipped=%u maxDelta=%u hash=%016llX\n",
                  stage, channel, (unsigned long long)stats->samples,
                  stats->minSample, stats->maxSample, mean, rms,
                  (unsigned)stats->zeroSamples, (unsigned)stats->clippedSamples,
                  (unsigned)stats->maxDelta, (unsigned long long)stats->hash);
}

static void GdxUnlockStageChunkReset(void) {
    memset(sGdxUnlockStage.chunkResample, 0, sizeof(sGdxUnlockStage.chunkResample));
    memset(sGdxUnlockStage.chunkPreEnvelope, 0, sizeof(sGdxUnlockStage.chunkPreEnvelope));
    memset(sGdxUnlockStage.chunkEnvelope, 0, sizeof(sGdxUnlockStage.chunkEnvelope));
    sGdxUnlockStage.chunkStarted = 1;
    sGdxUnlockStage.currentTarget = -1;
}

static void GdxUnlockStageReset(unsigned int generation, unsigned int sampleRate) {
    memset(&sGdxUnlockStage, 0, sizeof(sGdxUnlockStage));
    sGdxUnlockStage.generation = generation;
    sGdxUnlockStage.sampleRate = sampleRate;
    sGdxUnlockStage.active = 1;
    sGdxUnlockStage.currentTarget = -1;
    GdxUnlockStageStatsReset(&sGdxUnlockStage.decodeStats);
    GdxUnlockStageStatsReset(&sGdxUnlockStage.resampleInputStats);
    GdxUnlockStageStatsReset(&sGdxUnlockStage.resampleStats);
    GdxUnlockStageStatsReset(&sGdxUnlockStage.preEnvelopeStats);
    GdxUnlockStageStatsReset(&sGdxUnlockStage.envelopeStats[0]);
    GdxUnlockStageStatsReset(&sGdxUnlockStage.envelopeStats[1]);
    GdxUnlockStageStatsReset(&sGdxUnlockStage.mixStats[0]);
    GdxUnlockStageStatsReset(&sGdxUnlockStage.mixStats[1]);
    gdx_port_logf("[unlock-stage] armed generation=%u frames=%u sampleRate=%u executor=HLE-required\n",
                  generation, GDX_UNLOCK_STAGE_FRAMES, sampleRate);
}

void gdx_unlock_audio_stage_begin_command_list(unsigned int generation, const void* commandList,
                                               unsigned int sampleRate) {
    if ((generation == 0u) || (commandList == NULL) || (sampleRate == 0u)) {
        return;
    }
    if (generation != sGdxUnlockStage.generation) {
        GdxUnlockStageReset(generation, sampleRate);
    }
    if (sGdxUnlockStage.complete) {
        return;
    }
    sGdxUnlockStage.commandList = (uintptr_t)commandList;
    sGdxUnlockStage.rangeCount = 0;
}

static int GdxUnlockStageFindOrAddTarget(int noteIndex, uint32_t adpcmToken,
                                         uint32_t resampleToken) {
    uint32_t i;
    GdxUnlockStageTarget* target;

    for (i = 0; i < sGdxUnlockStage.targetCount; i++) {
        target = &sGdxUnlockStage.targets[i];
        if ((target->resampleToken == resampleToken) || (target->noteIndex == noteIndex)) {
            target->adpcmToken = adpcmToken;
            target->resampleToken = resampleToken;
            target->noteIndex = noteIndex;
            return (int)i;
        }
    }
    if (sGdxUnlockStage.targetCount >= GDX_UNLOCK_STAGE_MAX_TARGETS) {
        return -1;
    }

    target = &sGdxUnlockStage.targets[sGdxUnlockStage.targetCount++];
    memset(target, 0, sizeof(*target));
    target->adpcmToken = adpcmToken;
    target->resampleToken = resampleToken;
    target->noteIndex = noteIndex;
    gdx_port_logf("[unlock-stage] target generation=%u note=%d adpcm=%08X resample=%08X count=%u\n",
                  sGdxUnlockStage.generation, noteIndex, (unsigned)adpcmToken, (unsigned)resampleToken,
                  (unsigned)sGdxUnlockStage.targetCount);
    return (int)(sGdxUnlockStage.targetCount - 1u);
}

void gdx_unlock_audio_stage_register_command_range(
    unsigned int generation, int noteIndex, const void* commandStart, const void* commandEnd,
    const void* adpcmState, const void* resampleState, unsigned int sampleRate) {
    uint32_t adpcmToken;
    uint32_t resampleToken;
    int targetIndex;
    GdxUnlockStageRange* range;

    if ((generation == 0u) || (commandStart == NULL) || (commandEnd == NULL) ||
        (adpcmState == NULL) || (resampleState == NULL) || (sampleRate == 0u) ||
        ((uintptr_t)commandEnd <= (uintptr_t)commandStart)) {
        return;
    }
    if (generation != sGdxUnlockStage.generation) {
        GdxUnlockStageReset(generation, sampleRate);
    }
    if (sGdxUnlockStage.complete ||
        (sGdxUnlockStage.rangeCount >= GDX_UNLOCK_STAGE_MAX_RANGES)) {
        return;
    }
    adpcmToken = (uint32_t)(uintptr_t)adpcmState;
    resampleToken = (uint32_t)(uintptr_t)resampleState;
    targetIndex = GdxUnlockStageFindOrAddTarget(noteIndex, adpcmToken, resampleToken);
    if (targetIndex < 0) {
        return;
    }
    range = &sGdxUnlockStage.ranges[sGdxUnlockStage.rangeCount++];
    range->start = (uintptr_t)commandStart;
    range->end = (uintptr_t)commandEnd;
    range->targetIndex = targetIndex;
}

static int GdxUnlockStageFindCommandTarget(const GdxAcmd* command) {
    uintptr_t address;
    uint32_t i;

    if (!sGdxUnlockStage.active || sGdxUnlockStage.complete) {
        return -1;
    }
    address = (uintptr_t)command;
    for (i = 0; i < sGdxUnlockStage.rangeCount; i++) {
        const GdxUnlockStageRange* range = &sGdxUnlockStage.ranges[i];
        if ((address >= range->start) && (address < range->end)) {
            return range->targetIndex;
        }
    }
    return -1;
}

static void GdxUnlockStageCaptureDecode(int targetIndex, const GdxBufDesc* buf, uint32_t flags) {
    GdxUnlockStageTarget* target;
    uint32_t sampleCount;
    uint32_t i;
    int16_t first;
    int16_t last;

    if ((targetIndex < 0) || (buf == NULL)) {
        return;
    }
    target = &sGdxUnlockStage.targets[targetIndex];
    if ((flags & 1u) != 0u) {
        target->hasDecodeLast = 0;
    }
    sampleCount = buf->count / 2u;
    if (sampleCount == 0u) {
        return;
    }
    first = DmemGetS16(buf->dmemOut + 32u);
    last = DmemGetS16(buf->dmemOut + 32u + (sampleCount - 1u) * 2u);
    if (target->hasDecodeLast) {
        int32_t signedDelta = (int32_t)first - target->decodeLast;
        uint32_t delta = signedDelta < 0 ? (uint32_t)-signedDelta : (uint32_t)signedDelta;
        if (delta > target->decodeBoundaryMaxDelta) {
            target->decodeBoundaryMaxDelta = delta;
        }
    }
    target->decodeLast = last;
    target->hasDecodeLast = 1;
    target->decodeCalls++;

    GdxUnlockStageStatsBreak(&sGdxUnlockStage.decodeStats);
    for (i = 0; i < sampleCount; i++) {
        GdxUnlockStageStatsAdd(&sGdxUnlockStage.decodeStats,
                               DmemGetS16(buf->dmemOut + 32u + i * 2u));
    }
}

static void GdxUnlockStageCaptureResample(int targetIndex, const GdxBufDesc* buf,
                                          uint32_t pitch, uint32_t numOut) {
    uint32_t needIn;
    uint32_t limit;
    uint32_t i;

    if ((targetIndex < 0) || (buf == NULL) || !sGdxUnlockStage.chunkStarted) {
        return;
    }
    needIn = ((numOut * pitch) >> 15) + 8u;
    GdxUnlockStageStatsBreak(&sGdxUnlockStage.resampleInputStats);
    for (i = 0; i < needIn; i++) {
        GdxUnlockStageStatsAdd(&sGdxUnlockStage.resampleInputStats,
                               DmemGetS16(buf->dmemIn + i * 2u));
    }

    limit = numOut;
    if (limit > GDX_UNLOCK_STAGE_MAX_CHUNK) {
        limit = GDX_UNLOCK_STAGE_MAX_CHUNK;
        sGdxUnlockStage.truncatedChunks++;
    }
    for (i = 0; i < limit; i++) {
        sGdxUnlockStage.chunkResample[i] += DmemGetS16(buf->dmemOut + i * 2u);
    }
}

static void GdxUnlockStageCapturePreEnvelope(uint32_t dmemSrc, uint32_t sampleCount) {
    uint32_t limit = sampleCount;
    uint32_t i;

    if (!sGdxUnlockStage.chunkStarted || (sGdxUnlockStage.currentTarget < 0)) {
        return;
    }
    if (limit > GDX_UNLOCK_STAGE_MAX_CHUNK) {
        limit = GDX_UNLOCK_STAGE_MAX_CHUNK;
        sGdxUnlockStage.truncatedChunks++;
    }
    for (i = 0; i < limit; i++) {
        sGdxUnlockStage.chunkPreEnvelope[i] += DmemGetS16(dmemSrc + i * 2u);
    }
}

static void GdxUnlockStageCaptureEnvelopeSample(uint32_t sampleIndex, int32_t left, int32_t right) {
    if (!sGdxUnlockStage.chunkStarted || (sGdxUnlockStage.currentTarget < 0) ||
        (sampleIndex >= GDX_UNLOCK_STAGE_MAX_CHUNK)) {
        return;
    }
    sGdxUnlockStage.chunkEnvelope[0][sampleIndex] += left;
    sGdxUnlockStage.chunkEnvelope[1][sampleIndex] += right;
}

static void GdxUnlockStageFinish(void) {
    uint32_t i;
    int writeResample;
    int writePreEnvelope;
    int writeEnvelope;
    int writeMix;

    sGdxUnlockStage.complete = 1;
    writeResample = GdxUnlockStageWriteWav(GDX_UNLOCK_STAGE_RESAMPLE_PATH,
                                           sGdxUnlockStage.resample, sGdxUnlockStage.frames, 1u,
                                           sGdxUnlockStage.sampleRate);
    writePreEnvelope = GdxUnlockStageWriteWav(GDX_UNLOCK_STAGE_PRE_ENVELOPE_PATH,
                                              sGdxUnlockStage.preEnvelope, sGdxUnlockStage.frames, 1u,
                                              sGdxUnlockStage.sampleRate);
    writeEnvelope = GdxUnlockStageWriteWav(GDX_UNLOCK_STAGE_ENVELOPE_PATH,
                                           sGdxUnlockStage.envelope, sGdxUnlockStage.frames, 2u,
                                           sGdxUnlockStage.sampleRate);
    writeMix = GdxUnlockStageWriteWav(GDX_UNLOCK_STAGE_MIX_PATH,
                                      sGdxUnlockStage.mix, sGdxUnlockStage.frames, 2u,
                                      sGdxUnlockStage.sampleRate);

    GdxUnlockStageLogStats("decode", "M", &sGdxUnlockStage.decodeStats);
    GdxUnlockStageLogStats("resample-input", "M", &sGdxUnlockStage.resampleInputStats);
    GdxUnlockStageLogStats("resample", "M", &sGdxUnlockStage.resampleStats);
    GdxUnlockStageLogStats("pre-envelope", "M", &sGdxUnlockStage.preEnvelopeStats);
    GdxUnlockStageLogStats("envelope", "L", &sGdxUnlockStage.envelopeStats[0]);
    GdxUnlockStageLogStats("envelope", "R", &sGdxUnlockStage.envelopeStats[1]);
    GdxUnlockStageLogStats("mix", "L", &sGdxUnlockStage.mixStats[0]);
    GdxUnlockStageLogStats("mix", "R", &sGdxUnlockStage.mixStats[1]);
    for (i = 0; i < sGdxUnlockStage.targetCount; i++) {
        const GdxUnlockStageTarget* target = &sGdxUnlockStage.targets[i];
        gdx_port_logf("[unlock-stage] decode-boundary note=%d token=%08X calls=%u maxDelta=%u\n",
                      target->noteIndex, (unsigned)target->adpcmToken,
                      (unsigned)target->decodeCalls, (unsigned)target->decodeBoundaryMaxDelta);
    }
    gdx_port_logf("[unlock-stage] complete generation=%u executor=%s chunks=%u frames=%u "
                  "truncated=%u sumClip=resample:%u,preEnvelope:%u,envelopeL:%u,envelopeR:%u "
                  "artifacts=resample:%s,preEnvelope:%s,envelope:%s,mix:%s\n",
                  sGdxUnlockStage.generation, sGdxUnlockStage.sawHle ? "HLE" : "none",
                  (unsigned)sGdxUnlockStage.chunks, (unsigned)sGdxUnlockStage.frames,
                  (unsigned)sGdxUnlockStage.truncatedChunks,
                  (unsigned)sGdxUnlockStage.resampleClipSamples,
                  (unsigned)sGdxUnlockStage.preEnvelopeClipSamples,
                  (unsigned)sGdxUnlockStage.envelopeClipSamples[0],
                  (unsigned)sGdxUnlockStage.envelopeClipSamples[1],
                  writeResample ? "ok" : "write-failed",
                  writePreEnvelope ? "ok" : "write-failed",
                  writeEnvelope ? "ok" : "write-failed",
                  writeMix ? "ok" : "write-failed");
}

static void GdxUnlockStageAppendChunk(uint32_t dmemLeft, uint32_t dmemRight, uint32_t sampleCount) {
    uint32_t available;
    uint32_t limit;
    uint32_t i;

    if (!sGdxUnlockStage.active || sGdxUnlockStage.complete || !sGdxUnlockStage.sawHle) {
        return;
    }
    if (!sGdxUnlockStage.chunkStarted) {
        GdxUnlockStageChunkReset();
    }
    available = GDX_UNLOCK_STAGE_FRAMES - sGdxUnlockStage.frames;
    limit = sampleCount < available ? sampleCount : available;
    if (limit > GDX_UNLOCK_STAGE_MAX_CHUNK) {
        limit = GDX_UNLOCK_STAGE_MAX_CHUNK;
        sGdxUnlockStage.truncatedChunks++;
    }

    for (i = 0; i < limit; i++) {
        uint32_t frame = sGdxUnlockStage.frames + i;
        int16_t resample = GdxUnlockStageClamp(sGdxUnlockStage.chunkResample[i],
                                               &sGdxUnlockStage.resampleClipSamples);
        int16_t preEnvelope = GdxUnlockStageClamp(sGdxUnlockStage.chunkPreEnvelope[i],
                                                  &sGdxUnlockStage.preEnvelopeClipSamples);
        int16_t envelopeLeft = GdxUnlockStageClamp(sGdxUnlockStage.chunkEnvelope[0][i],
                                                   &sGdxUnlockStage.envelopeClipSamples[0]);
        int16_t envelopeRight = GdxUnlockStageClamp(sGdxUnlockStage.chunkEnvelope[1][i],
                                                    &sGdxUnlockStage.envelopeClipSamples[1]);
        int16_t mixLeft = DmemGetS16(dmemLeft + i * 2u);
        int16_t mixRight = DmemGetS16(dmemRight + i * 2u);

        sGdxUnlockStage.resample[frame] = resample;
        sGdxUnlockStage.preEnvelope[frame] = preEnvelope;
        sGdxUnlockStage.envelope[frame * 2u + 0u] = envelopeLeft;
        sGdxUnlockStage.envelope[frame * 2u + 1u] = envelopeRight;
        sGdxUnlockStage.mix[frame * 2u + 0u] = mixLeft;
        sGdxUnlockStage.mix[frame * 2u + 1u] = mixRight;
        GdxUnlockStageStatsAdd(&sGdxUnlockStage.resampleStats, resample);
        GdxUnlockStageStatsAdd(&sGdxUnlockStage.preEnvelopeStats, preEnvelope);
        GdxUnlockStageStatsAdd(&sGdxUnlockStage.envelopeStats[0], envelopeLeft);
        GdxUnlockStageStatsAdd(&sGdxUnlockStage.envelopeStats[1], envelopeRight);
        GdxUnlockStageStatsAdd(&sGdxUnlockStage.mixStats[0], mixLeft);
        GdxUnlockStageStatsAdd(&sGdxUnlockStage.mixStats[1], mixRight);
    }
    sGdxUnlockStage.frames += limit;
    sGdxUnlockStage.chunks++;
    sGdxUnlockStage.chunkStarted = 0;
    sGdxUnlockStage.currentTarget = -1;
    if (sGdxUnlockStage.frames == GDX_UNLOCK_STAGE_FRAMES) {
        GdxUnlockStageFinish();
    }
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
// ---- A_SETLOOP persistence (revamp v2 task A1). Real hardware's aspMain keeps the loop-restart
// pointer in ucode-internal state that is NEVER cleared between command lists -- it is only ever
// REPLACED by a new A_SETLOOP (mirrors mupen64plus-rsp-hle's alist_nead.c: `hle->alist_nead.loop`
// lives on the persistent `hle_t` struct, set once by SETLOOP and read by every later ADPCM call
// that carries A_LOOP, across as many command lists as the ucode task runs). This port's own
// disk/lib/synthesis.c comment (at the ADPCM dispatch) claims synthesis.c re-emits aSetLoop before
// every list that needs it -- FALSE: synthesis.c:886-890 emits aSetLoop only under
// `synthState->restart`, i.e. only when a note (re)starts its loop, not on every subsequent list
// that decodes a later wrap of the SAME already-looping note. A function-local pendingLoopState
// (reset to NULL at the top of every gdx_audio_hle_run call) therefore went NULL on every list
// that didn't happen to restart, so any loop wrap decoded in a LATER list had no loop history to
// read from -- wrong predictor state at every wrap of a sustained looped voice (engine drones,
// looped SFX), heard as periodic clicks. File-scope static below fixes this: it is set by
// GDX_A_SETLOOP and never reset elsewhere, exactly like the real ucode's persistent register.
// RunAdpcm's existing NULL-safe fallback (falls back to the note's own state[]/[1] when no loop
// has ever been set) is kept unchanged. ----
static const int16_t* sPendingLoopState = NULL;
/* [pcm-cap] plumbing: identity of the most recent A_LOADBUFF source, so the
   A_ADPCM capture can attribute its decode to a sample. */
static uintptr_t sPcmCapLastSrc = 0;
static uint32_t sPcmCapLastRaw = 0;
static int sDecCapCount = 0; /* [dec-cap] per-frame VADPCM capture budget */

/* ---- [rs-cap] probe (garbage-sounding gained notes: booster / low-health).
   Offline census proved all 53 cart-streamed SE samples are valid ADPCM at rest and
   RunAdpcm is bit-exact, so the remaining suspect is per-tick continuity through the
   resampler (these notes are pitch-swept; a broken fractional-state carry garbles
   exactly them while constant-pitch BGM survives). Every A_RESAMPLE call is recorded
   speculatively (pre/post state, full input and output windows) and dumped on two
   triggers so the resampler can be replayed offline sample-for-sample:
     T1 = A_HILOGAIN fires. Gained notes (noteSubEu->gain != 0) are exactly the
          booster/low-health set, and per synthesis.c command order the record at that
          moment IS that note's FinalResample. Cap 8.
     T2 = an A_CONTINUE resample whose pitch changed vs the previous call on the SAME
          state address (pitch-swept note). Consecutive triggers capture consecutive
          ticks of one note -- the tick-boundary continuity evidence. Cap 16. ---- */
#define GDX_RSCAP_MAXSAMPS 192
static struct {
    int valid;
    uint32_t callNo;
    uint32_t flags;
    uint32_t pitch;
    uint32_t prevPitch; /* previous call's pitch for the same state token (or ==pitch if first) */
    uint32_t token;     /* raw w1 state address -- stable per-note identity */
    uint32_t dmemIn, dmemOut, count;
    int interl;         /* an A_INTERL ran since the previous resample (nParts==2 marker) */
    uint32_t adpcmRaw;  /* upstream stage identity, see the run-3 comment below */
    uint32_t adpcmFlags, adpcmIn, adpcmOut, adpcmCnt;
    int setloop;
    int16_t preSt[5], postSt[5]; /* state[0..3] history + state[4] fracQ16 */
    int16_t in[GDX_RSCAP_MAXSAMPS];
    int16_t out[GDX_RSCAP_MAXSAMPS];
    uint32_t inN, outN;
} sRsCapLast;
static uint32_t sRsCapCallNo = 0;
static int sRsCapT1 = 0;
static int sRsCapT2 = 0;
static uint32_t sRsCapLockTok = 0; /* chain mode: state token locked by first swept race call */
static int sRsCapChain = 0;
static int sRsCapInterlPending = 0;
/* Upstream-stage identity for each chain dump (run-3 upgrade): run 2 proved the resample
   input STREAM breaks at every tick boundary while resample state chains perfectly, so the
   fault is upstream -- either the CPU-side source-window advance or the ADPCM decode state.
   Recording the last A_LOADBUFF source address and last A_ADPCM parameters that fed this
   resample discriminates: wrong/jumping chunk address = windowing bug; clean 9-byte-frame
   advance with broken PCM = decode-state bug (loop restore). */
static uint32_t sRsCapAdpcmRaw = 0;   /* LOADBUFF raw src token at the last A_ADPCM */
static uint32_t sRsCapAdpcmFlags = 0;
static uint32_t sRsCapAdpcmIn = 0, sRsCapAdpcmOut = 0, sRsCapAdpcmCnt = 0;
static int sRsCapSetloopSince = 0;    /* an A_SETLOOP ran since the previous resample */
static struct { uint32_t token; uint32_t pitch; } sRsCapPrev[8];
static int sRsCapPrevN = 0;

/* [spike] stage-bisection detector (film-grain investigation): the AI tap shows
   ~40/s isolated single-sample outliers that are pre-pan (same glitch on L and R
   scaled by the pan difference), i.e. injected somewhere in the per-voice DSP
   chain -- but the engine note's captured resample output is clean. Scanning at
   successive pipeline stages attributes the first corrupted stage directly.
   A "spike" is a sample deviating >3000 from BOTH neighbors in the same
   direction -- rare in legitimate music, characteristic of a stray sample. */
static int GdxSpikeScan(uint32_t dmem, uint32_t nSamples) {
    /* STRICT criterion (v2): the naive both-neighbors test fires on legitimate
       near-Nyquist percussion content. A true stray sample has NEIGHBORS THAT
       AGREE with each other (|prev-next| small) while the center is far from
       their mean -- interpolation would remove it. */
    uint32_t i;
    for (i = 1; i + 1 < nSamples; i++) {
        int32_t prev = DmemGetS16(dmem + (i - 1u) * 2u);
        int32_t cur = DmemGetS16(dmem + i * 2u);
        int32_t next = DmemGetS16(dmem + (i + 1u) * 2u);
        int32_t agree = prev - next;
        int32_t dev = cur - (prev + next) / 2;
        if (agree < 0) agree = -agree;
        if (dev < 0) dev = -dev;
        if (agree < 800 && dev > 3000) return (int)i;
    }
    return -1;
}
static int sSpikeLogsResample = 0;
static int sSpikeLogsFilter = 0;
static int sSpikeLogsInterleave = 0;

static void GdxRsCapDumpWindow(const char* pfx, const int16_t* buf, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        char line[160];
        int len = snprintf(line, sizeof(line), "[rs-cap] %s%03u", pfx, (unsigned)i);
        uint32_t j;
        for (j = 0; j < 16u && i < n && len > 0 && (size_t)len < sizeof(line) - 6u; j++, i++) {
            len += snprintf(line + len, sizeof(line) - (size_t)len, " %04X", (unsigned)(uint16_t)buf[i]);
        }
        gdx_port_logf("%s\n", line);
    }
}

static void GdxRsCapDump(const char* tag) {
    gdx_port_logf("[rs-cap] %s call=%u tok=%08X fl=%u pitch=%04X prev=%04X in=%04X out=%04X cnt=%04X "
                  "interl=%d pre=%04X %04X %04X %04X %04X post=%04X %04X %04X %04X %04X inN=%u outN=%u\n",
                  tag, (unsigned)sRsCapLast.callNo, (unsigned)sRsCapLast.token,
                  (unsigned)sRsCapLast.flags, (unsigned)sRsCapLast.pitch, (unsigned)sRsCapLast.prevPitch,
                  (unsigned)sRsCapLast.dmemIn, (unsigned)sRsCapLast.dmemOut, (unsigned)sRsCapLast.count,
                  sRsCapLast.interl,
                  (unsigned)(uint16_t)sRsCapLast.preSt[0], (unsigned)(uint16_t)sRsCapLast.preSt[1],
                  (unsigned)(uint16_t)sRsCapLast.preSt[2], (unsigned)(uint16_t)sRsCapLast.preSt[3],
                  (unsigned)(uint16_t)sRsCapLast.preSt[4],
                  (unsigned)(uint16_t)sRsCapLast.postSt[0], (unsigned)(uint16_t)sRsCapLast.postSt[1],
                  (unsigned)(uint16_t)sRsCapLast.postSt[2], (unsigned)(uint16_t)sRsCapLast.postSt[3],
                  (unsigned)(uint16_t)sRsCapLast.postSt[4],
                  (unsigned)sRsCapLast.inN, (unsigned)sRsCapLast.outN);
    gdx_port_logf("[rs-cap] %s-up adpcmRaw=%08X adFl=%u adIn=%04X adOut=%04X adCnt=%04X setloop=%d\n",
                  tag, (unsigned)sRsCapLast.adpcmRaw, (unsigned)sRsCapLast.adpcmFlags,
                  (unsigned)sRsCapLast.adpcmIn, (unsigned)sRsCapLast.adpcmOut,
                  (unsigned)sRsCapLast.adpcmCnt, sRsCapLast.setloop);
    GdxRsCapDumpWindow("i", sRsCapLast.in, sRsCapLast.inN);
    GdxRsCapDumpWindow("o", sRsCapLast.out, sRsCapLast.outN);
}

// ---- A_ADPCM: standard order-2 N64/GC-style VADPCM decode (16 samples per frame: 1 header
// byte + 8 data bytes of 4-bit nibbles, or 5 bytes total -- 4 data bytes of 2-bit nibbles --
// for the "small ADPCM" flags|4 variant). state is the persistent history pointer (real host
// memory -- NoteSynthesisBuffers.adpcmdecState[16] or SynthesisReverb-embedded AdpcmLoop).
// state[0..15] is the FULL last output frame, temporal order, newest at [15] -- this is NOT a
// free layout choice: it is observable through the DMEM output preamble contract (see the
// last-frame comment inside the function body). loopStatePtr is the most recent
// A_SETLOOP target (already resolved), used as the input history source instead of `state` when
// flags has A_LOOP set (matches the real ucode: SETLOOP overrides where history is READ from;
// the running state is still always WRITTEN to `state` afterwards). ----
static void RunAdpcm(const GdxBufDesc* buf, uint32_t flags, int16_t* state, const int16_t* loopState) {
    int smallAdpcm = (flags & 4) != 0;
    int isInit = (flags & 1) != 0;   /* A_INIT */
    int isLoop = (flags & 2) != 0;   /* A_LOOP */
    /* RAW (unclamped) running history -- the predictor recursion WITHIN a 16-sample frame reads
       these. int32_t, not int16_t: real hardware's recursion happens in the wide RSP accumulator
       the whole way through a half-frame and is never narrowed to 16 bits until output, so an
       unclamped int16_t local here would just add a spurious extra wraparound this file doesn't
       need -- int32_t is plenty (predicted is already reduced by the >>11 codebook shift before
       residual is added, see predAcc below, so it never approaches int32 range). */
    int32_t hist1, hist2;
    /* CLAMPED shadow of hist1/hist2, updated in lockstep every sample from the same value written
       to DMEM. This is what actually crosses a 16-sample FRAME boundary (deferred-clamping task
       A4, per mupen64plus-rsp-hle audio.c#L109-127 adpcm_compute_residuals: the intra-half-frame
       recursive term `rdot(i, book2, src)` sums the RAW predict_frame output `src`, never the
       clamped `dst` -- but the boundary history fed into the NEXT half/frame (`last_samples`,
       taken from the previous half's already-written `last_frame` buffer) IS the clamped output.
       Concretely: "clamp only the stored output sample; clamped history propagates only ACROSS
       frames" (this file's own frame unit is one 16-sample VADPCM frame, i.e. one header+data
       block -- our sequential per-sample decoder doesn't have mupen's internal 8-sample half-frame
       split, so the natural boundary here is once per 16-sample frame). Previously EVERY sample
       clamped hist1 before feeding it back (`hist1 = ClampS16(sampleOut)` unconditionally), so a
       loud transient that overshot mid-frame got its overshoot silently discarded from every later
       sample's prediction in that same frame -- audibly different (softer/duller) attack shape on
       loud percussive/transient content vs. the real ucode, which only loses the overshoot at the
       output tap, not from the running prediction. */
    int16_t clHist1, clHist2;
    int16_t lastFrame[16]; /* persistent state: the previous call's final 16 output samples */
    uint32_t numOutSamples = buf->count / 2u;
    /* Round UP to whole 16-sample frames like the real ucode (it always
       decodes complete frames; downstream consumes only `count` samples).
       Truncating left the chunk's last 1-15 samples unwritten — zeros from
       the buffer clear — producing hundreds of 4-9-sample micro-dropouts
       per second: the crackle/"static" layered over otherwise-correct music
       (measured directly in the AI output tap). */
    uint32_t numFrames = (numOutSamples + 15u) / 16u; /* SAMPLES_PER_FRAME, ceil */
    uint32_t inCursor = buf->dmemIn;
    uint32_t outCursor = buf->dmemOut;
    uint32_t f;

    if (state == NULL) {
        return;
    }

    /* THE LAST-FRAME PREAMBLE CONTRACT (root cause of the sustained-note
       "garbage/buzz": booster, low-health, engine drones). The real aspMain ADPCM op --
       and mupen64plus-rsp-hle's alist_adpcm verbatim -- does three things this function
       previously did not:
         1. keeps its persistent state as the FULL LAST 16-SAMPLE FRAME (which is why
            adpcmdecState and AdpcmLoop.predictorState are both s16[16]);
         2. WRITES THAT FRAME TO THE OUTPUT BUFFER FIRST -- output layout is
            [16 previous-tail samples][count/2 freshly decoded samples];
         3. decodes the new frames AFTER that 32-byte preamble.
       synthesis.c's window arithmetic is built on that layout: skipInitialSamples=16
       makes frameIndex start one frame PAST samplePosInt, and skipBytes (up to 32
       bytes) indexes into the preamble to serve the current frame's remaining samples
       from the PREVIOUS tick's decode. Without the preamble every continuing tick read
       ~one frame ahead with a per-tick wobbling offset and a stale tail: the [rs-cap]
       chain capture measured a full waveform discontinuity at EVERY tick boundary
       (output +2957 -> -14312) while resample state chained perfectly. Kernel math was
       already bit-exact ([pcm-cap] 8/8); only this DMEM layout contract was wrong.
       state[0..15] = the last frame, temporal order, newest at [15]. The A_LOOP path
       seeds from the ROM-captured loop predictorState (same shape, same reason). */
    {
        uint32_t i;
        if (isInit) {
            for (i = 0; i < 16u; i++) lastFrame[i] = 0;
        } else if (isLoop && loopState != NULL) {
            for (i = 0; i < 16u; i++) lastFrame[i] = loopState[i];
        } else {
            for (i = 0; i < 16u; i++) lastFrame[i] = state[i];
        }
        for (i = 0; i < 16u; i++) {
            DmemSetS16(outCursor + i * 2u, lastFrame[i]);
        }
        outCursor += 32u; /* fresh decode starts after the preamble */
    }
    hist1 = lastFrame[15]; /* newest */
    hist2 = lastFrame[14];
    /* clHist mirrors hist at this point (both sources are already clamped s16 values). */
    clHist1 = (int16_t)hist1;
    clHist2 = (int16_t)hist2;

    {
    uint32_t decoded = 0;

    for (f = 0; f < numFrames; f++) {
        uint8_t header = DmemGetU8(inCursor);
        /* VADPCM frame header: SCALE in the high nibble, PREDICTOR in the low
           nibble. These were swapped, so every frame decoded with a garbage
           predictor row and a wrong scale — the loud-static bug. Verified
           offline against the real title-BGM sample from this ROM: header
           0x30 with a 2-predictor book can only be scale=3/pred=0, and the
           corrected decode produces a clean musical waveform (rms 5006,
           smoothness 0.57, zero clipping) identical to the SDK matrix
           reference decode. */
        uint32_t shift = (header >> 4) & 0xF;
        uint32_t predIdx = header & 0xF;
        uint32_t dataBytes = smallAdpcm ? 4u : 8u;
        uint32_t frameBytes = 1u + dataBytes;
        uint32_t nibblesPerByte = smallAdpcm ? 4u : 2u; /* 2-bit vs 4-bit nibbles */
        uint32_t s;

        /* Frame-boundary carry (task A4): every NEW frame's prediction starts from the CLAMPED
           shadow left by the previous frame (or by the isInit/isLoop/state seed above, for f==0
           -- hist1/hist2 already equal clHist1/clHist2 there, so this is a no-op on the first
           iteration). Within the frame body below, hist1/hist2 float as RAW (unclamped) values;
           only clHist1/clHist2 gets clamped every sample. */
        hist1 = (int32_t)clHist1;
        hist2 = (int32_t)clHist2;

        /* [dec-cap] history entering this frame (newer, older) -- seeds the offline
           BLOCK-form VADPCM decode identically. */
        int16_t capPreNewer = clHist1;
        int16_t capPreOlder = clHist2;

        /* GDX_BLOCK_ADPCM=1: hardware-accurate BLOCK-convolution VADPCM decode (grain
           root cause A/B). The default sequential form below truncates (>>11) every
           sample and feeds the truncated value back, accumulating signal-correlated
           quantization noise the real RSP does not -- measured ~-55dB, present on
           every frame (the "film grain"). This path instead computes each 8-sample
           sub-block from its ENTRY history using the full book columns + intra-block
           residual convolution, truncating ONCE per output, matching the block form.
           Only the standard 4-bit codec; small-ADPCM keeps the sequential path. */
        {
            /* DEFAULT ON: block-convolution is the hardware-correct decode.
               The old env-var A/B silently no-op'd (the var never reached the process),
               so the grain test was never actually run — which is precisely the failure
               the developer-gate layer exists to prevent. The switch now lives in
               Dev Tools > Behavior overrides > "Legacy sequential VADPCM" (or
               GDX_SEQ_ADPCM=1 at launch), read live as a single int load. Bucket B, so
               it is a compile-time 0 in a build without GDX_DEV_TOOLS and the sequential
               path is unreachable there. The log line still fires once and PROVES which
               path the current setting selected. */
            const int blockAdpcm = !gdx_dev_gate(GDX_GATE_SEQ_ADPCM);
            {
                static int sAdpcmLogged = 0;
                if (!sAdpcmLogged) {
                    sAdpcmLogged = 1;
                    gdx_port_logf("[audio] VADPCM decode = %s\n",
                                  blockAdpcm ? "block-convolution (hardware-correct)" : "sequential (GDX_SEQ_ADPCM=1)");
                }
            }
            if (blockAdpcm && !smallAdpcm) {
                int32_t eH2 = (int32_t)clHist2; /* sub-block entry history: older */
                int32_t eH1 = (int32_t)clHist1; /* newer */
                uint32_t sub;
                for (sub = 0; sub < 2u; sub++) {
                    int32_t e[8];
                    uint32_t i, k;
                    for (i = 0; i < 8u; i++) {
                        uint32_t si = sub * 8u + i;
                        uint8_t bv = DmemGetU8(inCursor + 1u + (si / 2u));
                        int32_t nib = (si % 2u == 0u) ? ((bv >> 4) & 0xF) : (bv & 0xF);
                        if (nib & 0x8) nib -= 16;
                        e[i] = nib << shift;
                    }
                    int16_t sblk[8];
                    for (i = 0; i < 8u; i++) {
                        int64_t acc = (int64_t)BookCoef(predIdx, 0, i) * (int64_t)eH2 +
                                      (int64_t)BookCoef(predIdx, 1, i) * (int64_t)eH1;
                        for (k = 0; k < i; k++) {
                            acc += (int64_t)BookCoef(predIdx, 1, i - 1u - k) * (int64_t)e[k];
                        }
                        acc += (int64_t)e[i] << 11;
                        sblk[i] = ClampS16((int32_t)(acc >> 11));
                        DmemSetS16(outCursor + (sub * 8u + i) * 2u, sblk[i]);
                        decoded++;
                    }
                    eH2 = sblk[6];
                    eH1 = sblk[7];
                }
                clHist2 = (int16_t)eH2;
                clHist1 = (int16_t)eH1;
                goto frame_done; /* skip the sequential path */
            }
        }

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
            /* Sequential (per-sample) VADPCM: with a true two-sample history the
               order-2 IIR needs only COLUMN 0 of the codebook — row 0 weights the
               OLDER sample, row 1 the NEWER (SDK vadpcm decodeframe: total =
               c[p][0][j]*in_vec[0] + c[p][1][j]*in_vec[1] with in_vec[0] = out[-2];
               at j = 0 the intra-block residual sum is empty, so the per-sample
               form is exact). The previous code walked the matrix columns (s&7)
               while ALSO sliding history each sample — those columns already
               encode the recursion, so the feedback was double-counted, with
               swapped history pairing on top: the loud-static bug. */
            /* 64-bit accumulator: the real RSP does this multiply-accumulate in a wide
               (48-bit) vector accumulator, never a 32-bit int. Two products of two s16
               values (max magnitude 32768 each) can each reach 2^30, and their SUM can
               reach 2^31 -- one bit past INT32_MAX -- for the theoretical extreme case of
               both codebook coefficients and both history samples simultaneously at
               -32768 (loudest possible signal on both feedback taps at once). That is
               signed-integer-overflow UB in C, and in practice (2's-complement wraparound)
               it flips the sign of `predicted` for exactly that extreme case: a huge
               positive prediction becomes a huge negative one, i.e. a polarity-inverted
               spike instead of a clean saturate-to-max sample -- an audible pop/bang on
               the loudest possible frame instead of clipping gracefully. Using int64_t
               for the accumulation removes the UB and matches the RSP's wide-accumulator
               semantics exactly; the final >>11 result is always back in a small range
               (bounded by the codebook's realistic magnitude), so truncating to int32_t
               after the shift is safe. */
            {
                int64_t predAcc = (int64_t)BookCoef(predIdx, 0, 0) * (int64_t)hist2 +
                                   (int64_t)BookCoef(predIdx, 1, 0) * (int64_t)hist1;
                predicted = (int32_t)(predAcc >> 11);
            }
            sampleOut = predicted + residual;

            {
                /* Deferred clamping (task A4, mupen64plus-rsp-hle audio.c#L109-127
                   adpcm_compute_residuals): the STORED output sample is clamped, but the
                   value fed back into hist1/hist2 for the NEXT sample's prediction (still
                   within this same 16-sample frame) is the RAW, unclamped sampleOut -- only
                   clHist1/clHist2 (used at the NEXT frame's boundary, and as the eventual
                   state[]/loop-restore write-back) is clamped every sample. A loud transient
                   that overshoots mid-frame keeps its true (unclamped) magnitude feeding the
                   predictor for the rest of that frame, matching the real ucode's parity;
                   only the ClampS16'd copy actually written to DMEM/state is ever silently
                   capped. */
                int16_t clampedOut = ClampS16(sampleOut);
                DmemSetS16(outCursor + s * 2u, clampedOut);

                hist2 = hist1;
                hist1 = sampleOut; /* RAW carry, intra-frame only */

                clHist2 = clHist1;
                clHist1 = clampedOut; /* CLAMPED shadow, carries across frames */
            }

            decoded++;
        }

        /* [dec-cap] full per-frame capture (grain root cause: sequential-IIR vs
           block-convolution VADPCM). Dumps everything an offline decoder needs to
           reproduce this exact frame with the CORRECT block form and compare to the
           port's sequential output: predictor+scale, the 8 compressed data bytes,
           the pre-frame history (older/newer), the full 16-coef book for this
           predictor, and the 16 samples the port produced. Race-gated, capped.
           Only full (4-bit) ADPCM; raw source id lets offline filter the grain
           sample (65134-byte one-shot BGM). */
        {
            extern int gGdxRaceActive;
            if (gGdxRaceActive && !smallAdpcm && sDecCapCount < 12) {
                char dbuf[64], bbuf[160], obuf[160];
                int dp = 0, bp = 0, op = 0;
                uint32_t z;
                sDecCapCount++;
                for (z = 0; z < 8u; z++) {
                    dp += snprintf(dbuf + dp, sizeof(dbuf) - (size_t)dp, "%02X",
                                   DmemGetU8(inCursor + 1u + z));
                }
                for (z = 0; z < 8u; z++) {
                    bp += snprintf(bbuf + bp, sizeof(bbuf) - (size_t)bp, "%04X %04X ",
                                   (uint16_t)BookCoef(predIdx, 0, z), (uint16_t)BookCoef(predIdx, 1, z));
                }
                for (z = 0; z < 16u; z++) {
                    op += snprintf(obuf + op, sizeof(obuf) - (size_t)op, "%04X ",
                                   (uint16_t)DmemGetS16(outCursor + z * 2u));
                }
                gdx_port_logf("[dec-cap] raw=%08X pred=%u shift=%u preOlder=%04X preNewer=%04X\n",
                              sPcmCapLastRaw, (unsigned)predIdx, (unsigned)shift,
                              (uint16_t)capPreOlder, (uint16_t)capPreNewer);
                gdx_port_logf("[dec-cap]  data=%s\n", dbuf);
                gdx_port_logf("[dec-cap]  book=%s\n", bbuf);
                gdx_port_logf("[dec-cap]  out=%s\n", obuf);
            }
        }

    frame_done: /* block-convolution path (GDX_BLOCK_ADPCM) rejoins here */
        inCursor += frameBytes;
        outCursor += 32u; /* 16 samples * 2 bytes */
    }
    (void)decoded;

    /* Persist the last frame: the 16 samples ending at the chunk's TRUE boundary
       (count), not the scratch tail of the final rounded-up frame (the old 2-short
       write-back already learned that lesson on unaligned cart-streamed chunks).
       The full output stream in DMEM is [16-sample preamble][numOutSamples fresh],
       so stream index (numOutSamples + i) for i in 0..15 is exactly the window of
       the last 16 TRUE samples -- and when numOutSamples == 0 it degenerates to
       re-reading the unchanged preamble, i.e. state passes through untouched. */
    {
        uint32_t i;
        for (i = 0; i < 16u; i++) {
            state[i] = DmemGetS16(buf->dmemOut + (numOutSamples + i) * 2u);
        }
    }
    }
}

// ---- A_S8DEC: signed 8-bit PCM -> 16-bit (sign-extend + scale). The codec itself is
// non-predictive, but the OUTPUT LAYOUT contract is the same as A_ADPCM's (see the
// last-frame preamble comment in RunAdpcm): synthesis.c uses skipInitialSamples=16 for
// CODEC_S8 too, so the ucode writes the 16-sample state preamble before the fresh
// samples and persists the last 16 output samples as state. ----
static void RunS8Dec(const GdxBufDesc* buf, uint32_t flags, int16_t* state) {
    int isInit = (flags & 1) != 0;
    uint32_t numSamples = buf->count / 2u;
    uint32_t i;
    if (state == NULL) {
        return;
    }
    for (i = 0; i < 16u; i++) {
        DmemSetS16(buf->dmemOut + i * 2u, isInit ? 0 : state[i]);
    }
    for (i = 0; i < numSamples; i++) {
        int8_t raw = (int8_t)DmemGetU8(buf->dmemIn + i);
        DmemSetS16(buf->dmemOut + 32u + i * 2u, (int16_t)((int32_t)raw * 256));
    }
    for (i = 0; i < 16u; i++) {
        state[i] = DmemGetS16(buf->dmemOut + (numSamples + i) * 2u);
    }
}

// ---- A_RESAMPLE 4-tap polyphase FIR table -------------------------------------------------
// The real RSP A_RESAMPLE is NOT linear interpolation: it convolves 4 neighbouring source
// samples per output sample against a fixed 64-phase table of 4-tap Q15 coefficient sets
// (one set per fractional sub-position between two source samples).
//
// EXACT ROM-BAKED CONSTANTS (bit-exact to the aspMain ucode's resample coefficient table).
// Source: the canonical RSP audio-HLE resample LUT reproduced verbatim across the widely
// mirrored N64 HLE lineage -- mupen64plus-rsp-hle (src/alist.c `RESAMPLE_LUT`), and its
// byte-identical copy in Project64/AziAudio (AziAudio/Mupen64plusHLE/audio.c, `RESAMPLE_LUT
// [64 * 4]`, from which these values were transcribed). Those projects extracted the values
// directly from the RSP microcode's data section, so this is the same table the real hardware
// convolves, not an approximation. (This replaces the earlier runtime-generated windowed-sinc
// table, which was audibly close but not bit-exact.)
//
// TAP / PHASE LAYOUT (verified 1:1 against this port's RunResample below and mupen's
// alist_resample): for phase index `p` = top 6 bits of the Q16 fractional accumulator
// (fracQ16 >> 10), the 4 coefficients sResampleTable[p][0..3] weight the source samples
// {x[-1], x[0], x[+1], x[+2]} respectively (i.e. tap 0 = the one-sample history `lastSample`,
// tap 1 = the current sample, taps 2/3 = the two-sample lookahead). This is identical to
// mupen's `src[0..3] * lut[0..3]` with `lut = RESAMPLE_LUT + ((accu & 0xfc00) >> 8)`.
//
// NOTE (authentic behavior, not a bug): phase 0 is {0x0c39,0x66ad,0x0d46,0xffdf}, NOT an
// identity {0,0x8000,0,0}. The real N64 resampler therefore applies a mild band-limiting
// low-pass EVEN at unity pitch (0x8000) -- integer-ratio playback is not a bit-exact
// passthrough on hardware. This is the well-known slightly "soft" character of N64 sample
// playback and is reproduced faithfully here.
#define GDX_RESAMPLE_PHASE_BITS 6
#define GDX_RESAMPLE_PHASES (1u << GDX_RESAMPLE_PHASE_BITS) /* 64 */
#define GDX_S16(x) ((int16_t)(x)) /* sign-truncate a 16-bit hex constant, MSVC /W3-clean */
static const int16_t sResampleTable[GDX_RESAMPLE_PHASES][4] = {
    { GDX_S16(0x0c39), GDX_S16(0x66ad), GDX_S16(0x0d46), GDX_S16(0xffdf) },
    { GDX_S16(0x0b39), GDX_S16(0x6696), GDX_S16(0x0e5f), GDX_S16(0xffd8) },
    { GDX_S16(0x0a44), GDX_S16(0x6669), GDX_S16(0x0f83), GDX_S16(0xffd0) },
    { GDX_S16(0x095a), GDX_S16(0x6626), GDX_S16(0x10b4), GDX_S16(0xffc8) },
    { GDX_S16(0x087d), GDX_S16(0x65cd), GDX_S16(0x11f0), GDX_S16(0xffbf) },
    { GDX_S16(0x07ab), GDX_S16(0x655e), GDX_S16(0x1338), GDX_S16(0xffb6) },
    { GDX_S16(0x06e4), GDX_S16(0x64d9), GDX_S16(0x148c), GDX_S16(0xffac) },
    { GDX_S16(0x0628), GDX_S16(0x643f), GDX_S16(0x15eb), GDX_S16(0xffa1) },
    { GDX_S16(0x0577), GDX_S16(0x638f), GDX_S16(0x1756), GDX_S16(0xff96) },
    { GDX_S16(0x04d1), GDX_S16(0x62cb), GDX_S16(0x18cb), GDX_S16(0xff8a) },
    { GDX_S16(0x0435), GDX_S16(0x61f3), GDX_S16(0x1a4c), GDX_S16(0xff7e) },
    { GDX_S16(0x03a4), GDX_S16(0x6106), GDX_S16(0x1bd7), GDX_S16(0xff71) },
    { GDX_S16(0x031c), GDX_S16(0x6007), GDX_S16(0x1d6c), GDX_S16(0xff64) },
    { GDX_S16(0x029f), GDX_S16(0x5ef5), GDX_S16(0x1f0b), GDX_S16(0xff56) },
    { GDX_S16(0x022a), GDX_S16(0x5dd0), GDX_S16(0x20b3), GDX_S16(0xff48) },
    { GDX_S16(0x01be), GDX_S16(0x5c9a), GDX_S16(0x2264), GDX_S16(0xff3a) },
    { GDX_S16(0x015b), GDX_S16(0x5b53), GDX_S16(0x241e), GDX_S16(0xff2c) },
    { GDX_S16(0x0101), GDX_S16(0x59fc), GDX_S16(0x25e0), GDX_S16(0xff1e) },
    { GDX_S16(0x00ae), GDX_S16(0x5896), GDX_S16(0x27a9), GDX_S16(0xff10) },
    { GDX_S16(0x0063), GDX_S16(0x5720), GDX_S16(0x297a), GDX_S16(0xff02) },
    { GDX_S16(0x001f), GDX_S16(0x559d), GDX_S16(0x2b50), GDX_S16(0xfef4) },
    { GDX_S16(0xffe2), GDX_S16(0x540d), GDX_S16(0x2d2c), GDX_S16(0xfee8) },
    { GDX_S16(0xffac), GDX_S16(0x5270), GDX_S16(0x2f0d), GDX_S16(0xfedb) },
    { GDX_S16(0xff7c), GDX_S16(0x50c7), GDX_S16(0x30f3), GDX_S16(0xfed0) },
    { GDX_S16(0xff53), GDX_S16(0x4f14), GDX_S16(0x32dc), GDX_S16(0xfec6) },
    { GDX_S16(0xff2e), GDX_S16(0x4d57), GDX_S16(0x34c8), GDX_S16(0xfebd) },
    { GDX_S16(0xff0f), GDX_S16(0x4b91), GDX_S16(0x36b6), GDX_S16(0xfeb6) },
    { GDX_S16(0xfef5), GDX_S16(0x49c2), GDX_S16(0x38a5), GDX_S16(0xfeb0) },
    { GDX_S16(0xfedf), GDX_S16(0x47ed), GDX_S16(0x3a95), GDX_S16(0xfeac) },
    { GDX_S16(0xfece), GDX_S16(0x4611), GDX_S16(0x3c85), GDX_S16(0xfeab) },
    { GDX_S16(0xfec0), GDX_S16(0x4430), GDX_S16(0x3e74), GDX_S16(0xfeac) },
    { GDX_S16(0xfeb6), GDX_S16(0x424a), GDX_S16(0x4060), GDX_S16(0xfeaf) },
    { GDX_S16(0xfeaf), GDX_S16(0x4060), GDX_S16(0x424a), GDX_S16(0xfeb6) },
    { GDX_S16(0xfeac), GDX_S16(0x3e74), GDX_S16(0x4430), GDX_S16(0xfec0) },
    { GDX_S16(0xfeab), GDX_S16(0x3c85), GDX_S16(0x4611), GDX_S16(0xfece) },
    { GDX_S16(0xfeac), GDX_S16(0x3a95), GDX_S16(0x47ed), GDX_S16(0xfedf) },
    { GDX_S16(0xfeb0), GDX_S16(0x38a5), GDX_S16(0x49c2), GDX_S16(0xfef5) },
    { GDX_S16(0xfeb6), GDX_S16(0x36b6), GDX_S16(0x4b91), GDX_S16(0xff0f) },
    { GDX_S16(0xfebd), GDX_S16(0x34c8), GDX_S16(0x4d57), GDX_S16(0xff2e) },
    { GDX_S16(0xfec6), GDX_S16(0x32dc), GDX_S16(0x4f14), GDX_S16(0xff53) },
    { GDX_S16(0xfed0), GDX_S16(0x30f3), GDX_S16(0x50c7), GDX_S16(0xff7c) },
    { GDX_S16(0xfedb), GDX_S16(0x2f0d), GDX_S16(0x5270), GDX_S16(0xffac) },
    { GDX_S16(0xfee8), GDX_S16(0x2d2c), GDX_S16(0x540d), GDX_S16(0xffe2) },
    { GDX_S16(0xfef4), GDX_S16(0x2b50), GDX_S16(0x559d), GDX_S16(0x001f) },
    { GDX_S16(0xff02), GDX_S16(0x297a), GDX_S16(0x5720), GDX_S16(0x0063) },
    { GDX_S16(0xff10), GDX_S16(0x27a9), GDX_S16(0x5896), GDX_S16(0x00ae) },
    { GDX_S16(0xff1e), GDX_S16(0x25e0), GDX_S16(0x59fc), GDX_S16(0x0101) },
    { GDX_S16(0xff2c), GDX_S16(0x241e), GDX_S16(0x5b53), GDX_S16(0x015b) },
    { GDX_S16(0xff3a), GDX_S16(0x2264), GDX_S16(0x5c9a), GDX_S16(0x01be) },
    { GDX_S16(0xff48), GDX_S16(0x20b3), GDX_S16(0x5dd0), GDX_S16(0x022a) },
    { GDX_S16(0xff56), GDX_S16(0x1f0b), GDX_S16(0x5ef5), GDX_S16(0x029f) },
    { GDX_S16(0xff64), GDX_S16(0x1d6c), GDX_S16(0x6007), GDX_S16(0x031c) },
    { GDX_S16(0xff71), GDX_S16(0x1bd7), GDX_S16(0x6106), GDX_S16(0x03a4) },
    { GDX_S16(0xff7e), GDX_S16(0x1a4c), GDX_S16(0x61f3), GDX_S16(0x0435) },
    { GDX_S16(0xff8a), GDX_S16(0x18cb), GDX_S16(0x62cb), GDX_S16(0x04d1) },
    { GDX_S16(0xff96), GDX_S16(0x1756), GDX_S16(0x638f), GDX_S16(0x0577) },
    { GDX_S16(0xffa1), GDX_S16(0x15eb), GDX_S16(0x643f), GDX_S16(0x0628) },
    { GDX_S16(0xffac), GDX_S16(0x148c), GDX_S16(0x64d9), GDX_S16(0x06e4) },
    { GDX_S16(0xffb6), GDX_S16(0x1338), GDX_S16(0x655e), GDX_S16(0x07ab) },
    { GDX_S16(0xffbf), GDX_S16(0x11f0), GDX_S16(0x65cd), GDX_S16(0x087d) },
    { GDX_S16(0xffc8), GDX_S16(0x10b4), GDX_S16(0x6626), GDX_S16(0x095a) },
    { GDX_S16(0xffd0), GDX_S16(0x0f83), GDX_S16(0x6669), GDX_S16(0x0a44) },
    { GDX_S16(0xffd8), GDX_S16(0x0e5f), GDX_S16(0x6696), GDX_S16(0x0b39) },
    { GDX_S16(0xffdf), GDX_S16(0x0d46), GDX_S16(0x66ad), GDX_S16(0x0c39) },
};

// ---- A_RESAMPLE: 4-tap polyphase FIR resampler (see table comment above). pitch is Q15
// (UNITY_PITCH=0x8000 == 1.0x, per PR/abi.h).
//
// State layout (16 shorts, private convention, revised for task A5): state[0..3] = the 4 source
// samples immediately BEFORE this call's dmemIn[0] (persisted across calls, oldest at [0], most
// recent -- i.e. tap -1 -- at [3]); state[4] = fractional position (Q16, low 16 bits only);
// state[15] = pitch, stashed by the caller (see the A_RESAMPLE case in the main dispatch loop).
// Widened from a single history sample (state[1]) to a full 4-sample window per
// mupen64plus-rsp-hle alist.c#L621-639 (alist_resample_reset/alist_resample_load/
// alist_resample_save): the reference keeps its virtual read cursor `ipos` starting 4 WHOLE
// samples before dmemi and restores/saves that entire 4-sample window every call, not just one
// tap -- this file's own taps 0/+1/+2 still read straight out of the current call's buffer
// (synthesis.c's trailing SAMPLES_PER_FRAME padding makes that safe), but the persisted state is
// now shaped the same way as the reference's for parity and headroom against future tap changes.
//
// A_INIT now ZERO-PRIMES the history window (previously this file duplicated dmemIn[0] into the
// tap -1 slot as a "closest available" guess). The reference's alist_resample_reset explicitly
// memsets the 4 pre-roll samples to 0 and resets pitch_accu to 0 on init -- it does NOT seed from
// the incoming buffer. A freshly triggered note therefore ramps in from silence on its first few
// (sub-sample-window) output ticks rather than smearing its first real sample backward in time;
// this is what real hardware actually does. ----
static int16_t GdxResampleSampleAt(const GdxBufDesc* buf, const int16_t hist[4], uint32_t virtIdx) {
    /* virtIdx 0..3 -> persisted pre-roll history; virtIdx >= 4 -> this call's own DMEM buffer,
       with virtIdx==4 aliasing dmemIn[0] (i.e. dmemIn is logically "to the right of" the 4
       history slots, matching the reference's `ipos = (dmemi>>1) - 4` starting offset). */
    if (virtIdx < 4u) {
        return hist[virtIdx];
    }
    return DmemGetS16(buf->dmemIn + (virtIdx - 4u) * 2u);
}

static void RunResample(const GdxBufDesc* buf, uint32_t flags, int16_t* state) {
    int isInit = (flags & 1) != 0; /* A_INIT */
    uint32_t pitchQ15;
    uint32_t fracQ16;
    int16_t hist[4];
    /* Byte count rounded UP to a whole 16-byte (8-sample) granule, mirroring
       mupen64plus-rsp-hle's RESAMPLE handler: `(hle->alist_nead.count + 0xf) & ~0xf` before the
       byte->sample halving -- the same "always finish the whole processing granule, never leave
       a ragged tail" convention already applied to A_ADPCM's frame rounding in this file (see
       RunAdpcm's numFrames comment). A non-16-byte-aligned SETBUFF count previously produced
       exactly `count/2` samples and left anything past that byte-truncated; this can now write a
       few extra (harmless -- always real, in-window FIR output, never garbage) trailing samples
       into DMEM past the caller's nominal count, matching what the real ucode always does. */
    uint32_t roundedByteCount = (buf->count + 0xFu) & ~0xFu;
    uint32_t numOutSamples = roundedByteCount / 2u;
    uint32_t virtIdx = 4u; /* logical cursor; 4..(4+n) walk this call's own buffer, see above */
    uint32_t n;

    if (state == NULL) {
        return;
    }

    /* pitch is threaded in via the caller, which stashes it into state[15] before calling --
       see the A_RESAMPLE case in the main dispatch loop below. */
    pitchQ15 = (uint32_t)(uint16_t)state[15];

    if (isInit) {
        hist[0] = hist[1] = hist[2] = hist[3] = 0; /* zero-prime, see file comment above */
        fracQ16 = 0;
    } else {
        hist[0] = state[0];
        hist[1] = state[1];
        hist[2] = state[2];
        hist[3] = state[3];
        fracQ16 = ((uint32_t)(uint16_t)state[4]);
    }

    for (n = 0; n < numOutSamples; n++) {
        int16_t xm1 = GdxResampleSampleAt(buf, hist, virtIdx - 1u);
        int16_t x0 = GdxResampleSampleAt(buf, hist, virtIdx);
        int16_t x1 = GdxResampleSampleAt(buf, hist, virtIdx + 1u);
        int16_t x2 = GdxResampleSampleAt(buf, hist, virtIdx + 2u);
        uint32_t phase = (fracQ16 >> (16u - GDX_RESAMPLE_PHASE_BITS)) & (GDX_RESAMPLE_PHASES - 1u);
        const int16_t* c = sResampleTable[phase];
        int32_t out = ((int32_t)xm1 * c[0] + (int32_t)x0 * c[1] +
                       (int32_t)x1 * c[2] + (int32_t)x2 * c[3]) >> 15;
        DmemSetS16(buf->dmemOut + n * 2u, ClampS16(out));

        fracQ16 += (pitchQ15 << 1); /* Q15 -> Q16 */
        while (fracQ16 >= 0x10000u) {
            fracQ16 -= 0x10000u;
            virtIdx++;
        }
    }

    /* Persist the 4-sample window ending at this call's final cursor position (virtIdx-1) --
       becomes the next call's pre-roll history, exactly mirroring alist_resample_save saving the
       ucode's own 4-sample working window back to DRAM at end of call. */
    state[0] = GdxResampleSampleAt(buf, hist, virtIdx - 4u);
    state[1] = GdxResampleSampleAt(buf, hist, virtIdx - 3u);
    state[2] = GdxResampleSampleAt(buf, hist, virtIdx - 2u);
    state[3] = GdxResampleSampleAt(buf, hist, virtIdx - 1u);
    state[4] = (int16_t)(uint16_t)fracQ16;
}

// ---- A_FILTER (task A3, reimplemented per alist_filter BEHAVIOR -- mupen64plus-rsp-hle
// alist.c#L794-907 -- SEMANTICS only, no code copied; mupen is GPLv2). A_FILTER is a TWO-STEP
// protocol, exactly like A_LOADADPCM+A_ADPCM or A_SETLOOP+A_ADPCM (see AudioSynth_FilterReverb /
// LoadFilterSize+LoadFilterBuffer in disk/lib/synthesis.c, always emitted as an immediate pair):
//   1) aFilter(f=2, countOrBuf=<byte size of the buffer the NEXT call will filter>, addr=
//      <coefficient table pointer>) -- "prime": load 8 Q15 coefficients, remember the size. Our
//      existing pendingFilterCoef plumbing (main dispatch loop below) is UNCHANGED by this task.
//   2) aFilter(f=<A_INIT/A_CONTINUE>, countOrBuf=<DMEM buffer address>, addr=<state pointer>)
//      -- "apply": filter that DMEM buffer IN PLACE for the primed size, carrying/continuing
//      history via state.
//
// STRUCTURE (documented behavior only -- the reference's literal per-lane index arithmetic is its
// own reverse-engineered expression of the RSP's vector-lane shuffles and is NOT reproduced here):
//   * The 8 primed Q15 coefficients are applied DIRECTLY as a fixed 8-tap symmetric FIR -- there is
//     NO per-call averaging of two LUTs. (An earlier revision averaged a "carried" LUT with the
//     fresh coefficients, lut[x]=(carried[x]+coef[x])>>1; that HALVED gLowPassFilterData's identity
//     row {0,0,0,32767,0,0,0,0} to -6 dB on the very first apply and made the reverb-filter gain
//     depend on how many consecutive A_CONTINUE calls had run -- a fabrication that broke the
//     documented "cutoff 0 == unity passthrough" case AudioHeap_LoadFilter relies on. Removed: the
//     primed coef IS the FIR directly. The two-LUT averaging was this port's own invention, not a
//     reference behavior -- the real nead FILTER's "second table" is the delay-line state, not a
//     second coefficient set.)
//   * The filter runs per 8-sample BLOCK; each block's 8 outputs come from a 16-sample WINDOW =
//     [previous block's 8-sample tail] ++ [this block's 8 samples], the tap window sliding one
//     sample at a time (out[k] = sum_t coef[t]*win[k+t], k=0..7). A straightforward transversal FIR
//     with continuous history, not a transcription of the reference's lane pattern.
//   * State carry: the previous call's final 8 INPUT samples become the next call's "previous tail",
//     so the FIR is continuous across command lists. A_INIT zeroes the tail history.
//
// BYTE ORDER (RULED OUT: the coefficient table is NOT big-endian):
// the coefficient table is NEVER byte-swapped here and MUST NOT be. It originates as the
// compile-time host-order s16 array gLowPassFilterData[] (disk/lib/filter_data.c), is copied
// element-by-element into reverb->filterLeft/Right by AudioHeap_LoadLowPassFilter (heap.c: a plain
// host-order `filter[i]=ptr[i]`), and reaches this file as a host pointer -- so the prime memcpy
// below (host->host) is already correct, exactly like BookCoef reading the ADPCM codebook that
// gdx_audio_convert_font wrote host-order (the endianness swap lives in the load pipeline's
// gdx_rd_s16, never in this interpreter). Corollary: the "riff silence when gated ON" is NOT this
// op zeroing output -- valid host-order coefs yield attenuated-but-nonzero WET output; that riff
// defect is the silence-prefill/wet path (port-layer F3), not RunFilter.
//
// state buffer layout (private convention; the real filterLeftState/filterRightState buffers are
// 32 shorts each and never read by decomp C, so -- like adpcmdecState -- this file chooses its own
// layout): state[0..7] = previous call's final 8-sample INPUT tail (oldest at [0], newest at [7]);
// the remaining 24 shorts are unused. ----
static void RunFilter(uint32_t dmemBuf, uint32_t sizeBytes, uint32_t flags, int16_t* state,
                      const int16_t* coef) {
    uint32_t numSamples = sizeBytes / 2u;
    uint32_t numBlocks = numSamples / 8u; /* real ucode always operates on whole 8-sample blocks */
    int isInit = (flags & 1) != 0; /* A_INIT */
    int16_t tail[8];
    uint32_t blk;
    int t;

    /* Gate lifted: default ON. An A/B run confirmed the
       corrected direct-FIR path is healthy (boot riff plays, tunnel boom
       gone with GDX_HLE_FILTER=1). This low-pass is the only thing bleeding
       high-frequency energy out of the engine-echo reverb feedback loop
       (SynthesisReverb routes to SFX channels only); running with it OFF
       lets that loop slowly diverge -- the progressive race static. The
       pre-revamp build ran this FIR unconditionally. GDX_HLE_FILTER=0
       remains as the kill switch. */
    /* Live gate read: the switch is normalized to "disable the filter" (0 = stock = filter on),
       which is what lets Bucket B compile out of Release without silencing the FIR. */
    if (gdx_dev_gate(GDX_GATE_NO_HLE_FILTER)) {
        return;
    }

    if (state == NULL || coef == NULL || numSamples == 0u) {
        /* No coefficients/state resolved yet (e.g. the priming call was skipped due to an
           unresolved address) -- leave DMEM untouched rather than filter with garbage. */
        return;
    }

    if (isInit) {
        for (t = 0; t < 8; t++) tail[t] = 0;
    } else {
        for (t = 0; t < 8; t++) tail[t] = state[t];
    }

    for (blk = 0; blk < numBlocks; blk++) {
        uint32_t base = dmemBuf + blk * 16u;
        int16_t cur[8];
        int16_t win[16]; /* win[0..7] = previous tail, win[8..15] = this block's input */
        int16_t outBlk[8];
        int k;

        /* Read the block's INPUT before any write-back -- output overwrites DMEM in place, but the
           tail carried to the next block must be this block's INPUT samples, not its output. */
        for (t = 0; t < 8; t++) {
            cur[t] = DmemGetS16(base + (uint32_t)t * 2u);
        }
        for (t = 0; t < 8; t++) { win[t] = tail[t]; win[8 + t] = cur[t]; }

        for (k = 0; k < 8; k++) {
            int32_t acc = 0;
            for (t = 0; t < 8; t++) {
                acc += (int32_t)coef[t] * (int32_t)win[k + t]; /* primed coefs applied directly */
            }
            /* Round-to-nearest before the Q15 reduction (half-LSB `+0x4000` bias) rather than a
               plain truncating shift -- a standard fixed-point rounding convention. */
            outBlk[k] = ClampS16((acc + 0x4000) >> 15);
        }

        for (k = 0; k < 8; k++) {
            DmemSetS16(base + (uint32_t)k * 2u, outBlk[k]);
        }

        for (t = 0; t < 8; t++) tail[t] = cur[t]; /* this block's input becomes the next block's tail */
    }

    for (t = 0; t < 8; t++) state[t] = tail[t];
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

    /* Pending A_FILTER "prime" (f==2) payload, consumed by the following A_FILTER apply call
       (f==A_INIT/A_CONTINUE). Reset every call, same rationale as pendingLoopState above:
       AudioSynth_FilterReverb / LoadFilterSize+LoadFilterBuffer always emit the prime+apply
       pair back-to-back with nothing unrelated interleaved between them. */
    int16_t pendingFilterCoef[8] = { 0 };
    uint32_t pendingFilterSizeBytes = 0;
    int pendingFilterHaveCoef = 0;

    /* Pending A_ENVSETUP1/A_ENVSETUP2 state, consumed by the following A_ENVMIXER (mirrors the
       real RSP's internal envelope-mixer registers). */
    int32_t envRampReverb = 0, envRampLeft = 0, envRampRight = 0;
    int32_t envCurVolLeft = 0, envCurVolRight = 0, envReverbVol2 = 0;

    static int sUnhandledLogs = 0;

    if (cmds == NULL || count == 0) {
        return;
    }

    if (sGdxUnlockStage.active &&
        (sGdxUnlockStage.commandList != (uintptr_t)dataPtr)) {
        sGdxUnlockStage.rangeCount = 0;
    }

    if (sGdxUnlockStage.active && !sGdxUnlockStage.complete && !sGdxUnlockStage.sawHle) {
        sGdxUnlockStage.sawHle = 1;
        gdx_port_logf("[unlock-stage] begin generation=%u executor=HLE targets=%u\n",
                      sGdxUnlockStage.generation, (unsigned)sGdxUnlockStage.targetCount);
    }

    for (i = 0; i < count; i++) {
        uint32_t w0 = cmds[i].w0;
        uint32_t w1 = cmds[i].w1;
        uint32_t op = (w0 >> 24) & 0xFFu;
        sDmemCurOp = (uint8_t)op; /* [spike] last-writer attribution */

        switch (op) {
            case GDX_A_SPNOOP:
                break;

            case GDX_A_CLEARBUFF: {
                uint32_t dmem = w0 & 0xFFFFFFu; /* aClearBuffer packs dmem into the low 24 bits */
                uint32_t size = w1;
                uint32_t k;
                if (sGdxUnlockStage.active && !sGdxUnlockStage.complete &&
                    (dmem == 0x940u) && (size >= 0x340u)) {
                    GdxUnlockStageChunkReset();
                }
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
                    /* [pcm-cap] plumbing: remember the last compressed-sample
                       source so the ADPCM capture below can name its input. */
                    sPcmCapLastSrc = (uintptr_t)src;
                    sPcmCapLastRaw = w1;
                    /* [spike] mix-stage bisection: scan the LOADED region -- a spike
                       here means the corruption arrived FROM RDRAM (reverb ring
                       content, resample state area, etc.), i.e. it was created on a
                       PREVIOUS tick's save side, not by this tick's mixing.
                       Gate LOWERED to >=0x20: the reverb ring wrap
                       splits a tick into pieces as small as 0x10, and the old >=0x100
                       gate left every wrap tick's small piece UNSCANNED -- the one
                       proven route for unscanned wet content to reach the dry buses.
                       0x20 (16 samples) is the floor for a meaningful strict-spike
                       scan; compressed ADPCM chunk loads land at dmemDest < 0x580
                       (staging grows down from 0x940), so gate on the wet range too
                       to avoid false positives from compressed bytes. */
                    if (size >= 0x20u && dmemDest >= 0xC80u) {
                        static int sSpikeLogsLoadbuff = 0;
                        if (sSpikeLogsLoadbuff < 16) {
                            int si = GdxSpikeScan(dmemDest, size / 2u);
                            if (si >= 0) {
                                sSpikeLogsLoadbuff++;
                                gdx_port_logf("[spike] loadbuff at=%d dmem=%04X size=%04X raw=%08X\n",
                                              si, dmemDest, size, w1);
                            }
                        }
                    }
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
                sPendingLoopState = (const int16_t*)GdxAudioResolveAddr(w1, "SETLOOP");
                sRsCapSetloopSince = 1; /* [rs-cap] loop-wrap marker for the next resample */
                break;
            }

            case GDX_A_ADPCM: {
                uint32_t flags = (w0 >> 16) & 0xFFu;
                int16_t* state = (int16_t*)GdxAudioResolveAddr(w1, "ADPCM-state");
                int stageTarget = GdxUnlockStageFindCommandTarget(&cmds[i]);
                /* [pcm-cap] carry-in snapshot BEFORE the decode overwrites it:
                   offline verification of A_CONTINUE frames needs the pre-call
                   history. Under the last-frame state layout the newest two
                   samples live at the tail: [15]=newer, [14]=older. */
                int16_t preSt0 = (state != NULL) ? state[15] : 0;
                int16_t preSt1 = (state != NULL) ? state[14] : 0;
                /* [rs-cap] upstream identity for the next resample's chain dump */
                sRsCapAdpcmRaw = sPcmCapLastRaw;
                sRsCapAdpcmFlags = flags;
                sRsCapAdpcmIn = pendingBuf.dmemIn;
                sRsCapAdpcmOut = pendingBuf.dmemOut;
                sRsCapAdpcmCnt = pendingBuf.count;
                RunAdpcm(&pendingBuf, flags, state, sPendingLoopState);
                GdxUnlockStageCaptureDecode(stageTarget, &pendingBuf, flags);
                /* [pcm-cap] (audio decode ground-truth capture):
                   one block per UNIQUE sample source. Logs the input identity
                   (resolved src + raw), the book head, the first compressed
                   bytes, and the first 8 DECODED s16 samples from DMEM. The
                   offline check decodes the same ROM bytes with the same book
                   and diffs: equal => kernels innocent, defect is per-note
                   setup/gain; different => the guilty op is caught here. */
                {
                    static uintptr_t sPcmCapSeen[24];
                    static int sPcmCapCount = 0;
                    int pc, pcDup = 0;
                    for (pc = 0; pc < sPcmCapCount; pc++) {
                        if (sPcmCapSeen[pc] == sPcmCapLastSrc) { pcDup = 1; break; }
                    }
                    {
                        extern int gGdxRaceActive;
                        if (!gGdxRaceActive) {
                            /* Retargeted: boot/menu samples burned
                               the budget before any garbage race SFX played.
                               Race sounds are the open investigation. */
                            break;
                        }
                    }
                    if (!pcDup && sPcmCapCount < 24 && sPcmCapLastSrc != 0) {
                        sPcmCapSeen[sPcmCapCount++] = sPcmCapLastSrc;
                        gdx_port_logf("[pcm-cap] raw=%08X src=%p flags=%02X in=%04X out=%04X n=%u "
                                      "st=%04X %04X\n",
                                      sPcmCapLastRaw, (void*)sPcmCapLastSrc, (unsigned)flags,
                                      pendingBuf.dmemIn, pendingBuf.dmemOut, pendingBuf.count,
                                      (uint16_t)preSt0, (uint16_t)preSt1);
                        /* Full 32-short book: the offline order-2 decode needs
                           all 16 coefficients of BOTH predictors (the first
                           probe iteration logged only 4 and the verdict
                           stalled). Two lines of 16. */
                        {
                            /* sAdpcmBook holds raw BYTES of host-order s16
                               coefficients (see BookCoef's memcpy access) --
                               read pairs, not single bytes. 32 shorts = both
                               predictors of an order-2 book. */
                            const int16_t* bk = (const int16_t*)(const void*)sAdpcmBook;
                            int bl;
                            for (bl = 0; bl < 2; bl++) {
                                gdx_port_logf("[pcm-cap]  bk%d=%04X %04X %04X %04X %04X %04X %04X %04X "
                                              "%04X %04X %04X %04X %04X %04X %04X %04X\n", bl,
                                              (uint16_t)bk[bl * 16 + 0], (uint16_t)bk[bl * 16 + 1],
                                              (uint16_t)bk[bl * 16 + 2], (uint16_t)bk[bl * 16 + 3],
                                              (uint16_t)bk[bl * 16 + 4], (uint16_t)bk[bl * 16 + 5],
                                              (uint16_t)bk[bl * 16 + 6], (uint16_t)bk[bl * 16 + 7],
                                              (uint16_t)bk[bl * 16 + 8], (uint16_t)bk[bl * 16 + 9],
                                              (uint16_t)bk[bl * 16 + 10], (uint16_t)bk[bl * 16 + 11],
                                              (uint16_t)bk[bl * 16 + 12], (uint16_t)bk[bl * 16 + 13],
                                              (uint16_t)bk[bl * 16 + 14], (uint16_t)bk[bl * 16 + 15]);
                            }
                        }
                        gdx_port_logf("[pcm-cap]  book=%04X %04X %04X %04X "
                                      "cmp8=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                                      (uint16_t)sAdpcmBook[0], (uint16_t)sAdpcmBook[1],
                                      (uint16_t)sAdpcmBook[2], (uint16_t)sAdpcmBook[3],
                                      DmemGetU8(pendingBuf.dmemIn + 0u), DmemGetU8(pendingBuf.dmemIn + 1u),
                                      DmemGetU8(pendingBuf.dmemIn + 2u), DmemGetU8(pendingBuf.dmemIn + 3u),
                                      DmemGetU8(pendingBuf.dmemIn + 4u), DmemGetU8(pendingBuf.dmemIn + 5u),
                                      DmemGetU8(pendingBuf.dmemIn + 6u), DmemGetU8(pendingBuf.dmemIn + 7u));
                        /* +32: fresh decode output starts after the 16-sample last-frame
                           preamble (see RunAdpcm's layout contract comment). */
                        gdx_port_logf("[pcm-cap]  pcm8=%04X %04X %04X %04X %04X %04X %04X %04X\n",
                                      (uint16_t)DmemGetS16(pendingBuf.dmemOut + 32u),
                                      (uint16_t)DmemGetS16(pendingBuf.dmemOut + 34u),
                                      (uint16_t)DmemGetS16(pendingBuf.dmemOut + 36u),
                                      (uint16_t)DmemGetS16(pendingBuf.dmemOut + 38u),
                                      (uint16_t)DmemGetS16(pendingBuf.dmemOut + 40u),
                                      (uint16_t)DmemGetS16(pendingBuf.dmemOut + 42u),
                                      (uint16_t)DmemGetS16(pendingBuf.dmemOut + 44u),
                                      (uint16_t)DmemGetS16(pendingBuf.dmemOut + 46u));
                    }
                }
                break;
            }

            case GDX_A_S8DEC: {
                uint32_t flags = (w0 >> 16) & 0xFFu;
                int16_t* state = (int16_t*)GdxAudioResolveAddr(w1, "S8DEC-state");
                RunS8Dec(&pendingBuf, flags, state);
                break;
            }

            case GDX_A_RESAMPLE: {
                uint32_t flags = (w0 >> 16) & 0xFFu;
                uint32_t pitch = w0 & 0xFFFFu;
                int16_t* state = (int16_t*)GdxAudioResolveAddr(w1, "RESAMPLE-state");
                int stageTarget = GdxUnlockStageFindCommandTarget(&cmds[i]);
                sGdxUnlockStage.currentTarget = stageTarget;
                if (state != NULL) {
                    /* [rs-cap] speculative record: dumped only if a trigger fires (see the
                       probe comment at the sRsCapLast definition). Input window is snapshotted
                       BEFORE the run because the two-part path's output overlaps its input
                       (DMEM_TEMP vs DMEM_TEMP+0x20) and would clobber it. */
                    uint32_t numOut = ((pendingBuf.count + 0xFu) & ~0xFu) / 2u;
                    uint32_t needIn = ((numOut * pitch) >> 15) + 8u;
                    uint32_t prevPitch = 0xFFFFFFFFu;
                    int swept = 0;
                    uint32_t k;
                    sRsCapCallNo++;
                    sRsCapLast.valid = 1;
                    sRsCapLast.callNo = sRsCapCallNo;
                    sRsCapLast.flags = flags;
                    sRsCapLast.pitch = pitch;
                    sRsCapLast.token = w1;
                    sRsCapLast.dmemIn = pendingBuf.dmemIn;
                    sRsCapLast.dmemOut = pendingBuf.dmemOut;
                    sRsCapLast.count = pendingBuf.count;
                    sRsCapLast.interl = sRsCapInterlPending;
                    sRsCapInterlPending = 0;
                    sRsCapLast.adpcmRaw = sRsCapAdpcmRaw;
                    sRsCapLast.adpcmFlags = sRsCapAdpcmFlags;
                    sRsCapLast.adpcmIn = sRsCapAdpcmIn;
                    sRsCapLast.adpcmOut = sRsCapAdpcmOut;
                    sRsCapLast.adpcmCnt = sRsCapAdpcmCnt;
                    sRsCapLast.setloop = sRsCapSetloopSince;
                    sRsCapSetloopSince = 0;
                    sRsCapLast.inN = needIn > GDX_RSCAP_MAXSAMPS ? GDX_RSCAP_MAXSAMPS : needIn;
                    sRsCapLast.outN = numOut > GDX_RSCAP_MAXSAMPS ? GDX_RSCAP_MAXSAMPS : numOut;
                    for (k = 0; k < sRsCapLast.inN; k++) {
                        sRsCapLast.in[k] = DmemGetS16(pendingBuf.dmemIn + k * 2u);
                    }
                    for (k = 0; k < 5u; k++) {
                        sRsCapLast.preSt[k] = state[k];
                    }
                    for (k = 0; k < (uint32_t)sRsCapPrevN; k++) {
                        if (sRsCapPrev[k].token == w1) {
                            prevPitch = sRsCapPrev[k].pitch;
                            sRsCapPrev[k].pitch = pitch;
                            break;
                        }
                    }
                    if (k == (uint32_t)sRsCapPrevN) {
                        if (sRsCapPrevN < 8) {
                            sRsCapPrev[sRsCapPrevN].token = w1;
                            sRsCapPrev[sRsCapPrevN].pitch = pitch;
                            sRsCapPrevN++;
                        } else {
                            sRsCapPrev[sRsCapCallNo & 7u].token = w1;
                            sRsCapPrev[sRsCapCallNo & 7u].pitch = pitch;
                        }
                    }
                    sRsCapLast.prevPitch = (prevPitch == 0xFFFFFFFFu) ? pitch : prevPitch;
                    swept = ((flags & 1u) == 0u) && (prevPitch != 0xFFFFFFFFu) && (prevPitch != pitch);
                    /* Chain mode (run-2 upgrade): swept T2 dumps from run 1 could not be
                       continuity-checked because the same note resamples several times per
                       frame and only the swept calls were dumped -- no adjacent pairs. Now the
                       first swept call DURING A RACE locks its state token and EVERY subsequent
                       call on that token is dumped ("C" tag), giving directly adjacent
                       pre/post-state pairs across tick boundaries. */
                    {
                        extern int gGdxRaceActive;
                        if (sRsCapLockTok == 0u && swept && gGdxRaceActive) {
                            sRsCapLockTok = w1;
                        }
                    }

                    /* Stash pitch in the otherwise-unused last slot of the 16-short state
                       buffer for RunResample to pick up (keeps RunResample's signature simple;
                       this slot is never read by decomp C code, only by this interpreter). */
                    state[15] = (int16_t)(uint16_t)pitch;
                    RunResample(&pendingBuf, flags, state);
                    GdxUnlockStageCaptureResample(stageTarget, &pendingBuf, pitch, numOut);

                    for (k = 0; k < sRsCapLast.outN; k++) {
                        sRsCapLast.out[k] = DmemGetS16(pendingBuf.dmemOut + k * 2u);
                    }
                    for (k = 0; k < 5u; k++) {
                        sRsCapLast.postSt[k] = state[k];
                    }
                    if (sRsCapLockTok != 0u && w1 == sRsCapLockTok && sRsCapChain < 24) {
                        sRsCapChain++;
                        GdxRsCapDump("C");
                    } else if (swept && sRsCapT2 < 4) {
                        sRsCapT2++;
                        GdxRsCapDump("T2");
                    }
                    if (sSpikeLogsResample < 16) {
                        int si = GdxSpikeScan(pendingBuf.dmemOut, numOut);
                        if (si >= 0) {
                            sSpikeLogsResample++;
                            gdx_port_logf("[spike] post-resample call=%u tok=%08X fl=%u pitch=%04X at=%d "
                                          "in=%04X out=%04X cnt=%04X interl=%d adCnt=%04X\n",
                                          (unsigned)sRsCapCallNo, (unsigned)w1, (unsigned)flags,
                                          (unsigned)pitch, si, pendingBuf.dmemIn, pendingBuf.dmemOut,
                                          pendingBuf.count, sRsCapLast.interl, (unsigned)sRsCapAdpcmCnt);
                            GdxRsCapDump("SPK");
                        }
                    }
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
                /* Reverb kill switch. Skip ONLY the reverb wet->dry return (dmemOut==LEFT_CH
                   0x940); the decay mix 0xC80->0xC80 and all note mixes are untouched. Reverb is
                   turned OFF by ANY of three independent sources:
                     (1) gEnhancements.Audio.Reverb CVar (ImGui Audio tab, F1 > Audio > Reverb):
                         reverb OFF when the CVar is 0. Default 1 = ON, so behavior is unchanged if
                         the menu is never touched (bit-exact default). Read LIVE each call (benign
                         int race with the main-thread menu write -- same live-CVar pattern as
                         gdx_audio_lle.c's engine toggle and os.cpp's low-pass), so a menu toggle
                         applies without a restart. NOTE: this is the HLE reverb only. Under the
                         default LLE engine the reverb is the ucode's own and this A_MIXER path is
                         not taken, so the toggle has no audible effect there -- it is still wired
                         correctly for the HLE fallback path.
                     (2) The GDX_NO_REVERB dev gate (Dev Tools > Behavior overrides > "Disable
                         reverb wet return", or GDX_NO_REVERB=1 at launch): the RELIABLE A/B dev
                         fallback, since the decomp-side toggle in synthesis.c used to silently
                         no-op when that TU's getenv returned NULL. Now both sides read the same
                         gate cache, so they can no longer disagree. Bucket B: a compile-time 0
                         in a build without GDX_DEV_TOOLS.
                     (3) The GdxAudioDbg()&4 debug bit, exactly as before. */
                {
                    int reverbOff; /* declared at block top (project C style; see gdx_audio_lle.c) */
                    /* Both reads are live (one int load + one CVar read). */
                    reverbOff = gdx_dev_gate(GDX_GATE_NO_REVERB) ||
                                !CVarGetInteger("gEnhancements.Audio.Reverb", 1);
                    if ((reverbOff || (GdxAudioDbg() & 4)) && dmemOut == 0x940u) {
                        break;
                    }
                }
                for (k = 0; k < numSamples; k++) {
                    int32_t in = DmemGetS16(dmemIn + k * 2u);
                    int32_t out = DmemGetS16(dmemOut + k * 2u);
                    out = out + ((in * gain) >> 15);
                    DmemSetS16(dmemOut + k * 2u, ClampS16(out));
                }
                /* [spike] bisection: scan the dry bus
                   immediately after the reverb wet->dry return (dmemOut==LEFT_CH 0x940,
                   the only A_MIXER writing a dry bus; the decay mix is 0xC80->0xC80).
                   The two surviving candidates split HERE:
                     - spike present now  => reverb TRANSPORTED a pre-existing spike
                       (RANK 2: unscanned wrap-tick wet content) -- it entered before
                       any note ran. Also scan the mixer's INPUT to prove it arrived
                       already-bad vs was created by this op's clamp.
                     - clean now, spiked at interleave => injected DURING note mixing
                       (RANK 5 op-19 path: envmixer scan-window vs interleave-read
                       mismatch). Names which branch to chase without a second run. */
                if (dmemOut == 0x940u) {
                    static int sSpikeLogsRvbRet = 0;
                    if (sSpikeLogsRvbRet < 12) {
                        int so = GdxSpikeScan(dmemOut, numSamples);
                        int siN = GdxSpikeScan(dmemIn, numSamples);
                        if (so >= 0 || siN >= 0) {
                            sSpikeLogsRvbRet++;
                            gdx_port_logf("[spike] reverb-return dryOut=%d wetIn=%d "
                                          "(out=%04X in=%04X n=%u)\n",
                                          so, siN, dmemOut, dmemIn, numSamples);
                        }
                    }
                }
                break;
            }

            case GDX_A_ADDMIXER: {
                /* mupen64plus-rsp-hle's alist_add (alist.c#L595-609) is a
                   GAINLESS clamped unity add -- `*dst = clamp_s16(*dst + *src)`, no multiply at
                   all. This op is not called anywhere in decomp/src/audio/disk/lib/synthesis.c
                   (checked: no aAddMixer call sites exist in this decomp), so there is no real
                   call site to inspect for what abi.h's aAddMixer `a4` parameter would carry --
                   but abi.h's own macro (`aAddMixer(pkt, count, dmemi, dmemo, a4)`) packs a4 into
                   the low 16 bits of w0 as a generic, undocumented ucode field, not something this
                   port has any evidence is a gain. Previously this treated those low 16 bits as a
                   signed Q15 gain (mirroring A_MIXER) -- per the spec's directive ("if the
                   reference is gainless and a4 is not a gain, drop ours"), dropped: this now
                   matches the reference's gainless unity add exactly. count>>4 packing (bits
                   16..23 of w0) is unchanged, matching abi.h's `count >> 4` field. */
                uint32_t count8 = (w0 >> 16) & 0xFFu;
                uint32_t dmemIn = (w1 >> 16) & 0xFFFFu;
                uint32_t dmemOut = w1 & 0xFFFFu;
                uint32_t numSamples = count8 * 8u;
                uint32_t k;
                for (k = 0; k < numSamples; k++) {
                    int32_t in = DmemGetS16(dmemIn + k * 2u);
                    int32_t out = DmemGetS16(dmemOut + k * 2u);
                    DmemSetS16(dmemOut + k * 2u, ClampS16(out + in));
                }
                break;
            }

            case GDX_A_INTERLEAVE: {
                uint32_t dmemOut = w0 & 0xFFFFu;
                uint32_t dmemL = (w1 >> 16) & 0xFFFFu;
                uint32_t dmemR = w1 & 0xFFFFu;
                uint32_t byteCount = ((w0 >> 16) & 0xFFu) << 4; /* per-channel bytes, c>>4 packed */
                uint32_t numSamples = byteCount / 2u;
                GdxUnlockStageAppendChunk(dmemL, dmemR, numSamples);
                /* [spike] stage scan: the interleave inputs are the fully mixed dry L/R
                   buses -- a spike here but not post-resample means the mixing stages
                   (envmixer/mixer/reverb return) injected it. */
                if (sSpikeLogsInterleave < 16) {
                    int sl = GdxSpikeScan(dmemL, numSamples);
                    int sr = GdxSpikeScan(dmemR, numSamples);
                    if (sl >= 0 || sr >= 0) {
                        uint8_t wl = (sl >= 0) ? sDmemLastOp[((dmemL + (uint32_t)sl * 2u) & GDX_DMEM_MASK) >> 4]
                                               : 0xFF;
                        uint8_t wr = (sr >= 0) ? sDmemLastOp[((dmemR + (uint32_t)sr * 2u) & GDX_DMEM_MASK) >> 4]
                                               : 0xFF;
                        sSpikeLogsInterleave++;
                        gdx_port_logf("[spike] pre-interleave L=%d R=%d dmemL=%04X dmemR=%04X n=%u "
                                      "lastOpL=%u lastOpR=%u\n",
                                      sl, sr, dmemL, dmemR, numSamples, wl, wr);
                    }
                }
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
                sRsCapInterlPending = 1; /* [rs-cap] nParts==2 marker for the next resample */
                if (GdxAudioDbg() & 8) { /* nointerl bypass: straight copy, no decimation */
                    for (k = 0; k < numSamples; k++) {
                        DmemSetS16(dmemOut + k * 2u, DmemGetS16(dmemIn + k * 2u));
                    }
                    break;
                }
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
                /* Task A2's env2 base/step scale resolution: mupen64plus-rsp-hle's nead
                   ENVSETUP1 handler (alist_nead.c ENVSETUP1/ENVSETUP1_MK, ~L175-189) computes
                   `env_values[2] = (w1 >> 8) & 0xff00` -- given this file's w0 layout
                   (`a` in bits[23:16], per the driver contract comment above the ENVMIXER case),
                   that expression reduces to exactly `a << 8`. env_values[2] is later used by
                   envmix_nead in the SAME `(x * env_values[N]) >> 16` formula as env_values[0]/[1]
                   (the dry L/R volumes from ENVSETUP2, which are already full Q16 words --
                   targetVol<<4 per this port's own driver contract). So env_values[2]=a<<8 lives
                   in that SAME Q16 numeric space, NOT a separate Q8 space -- confirming `a` (an
                   8-bit reverb-volume base, 0..254) and the ramp step share ONE scale once `a` is
                   left-shifted by 8. env_steps[2] in the reference is `w1` truncated to its low 16
                   bits, i.e. exactly our `rampReverb` (b) here, ADDED DIRECTLY every block with no
                   further scaling -- which only lines up with the a<<8 base because this port's
                   own driver-contract comment already derives rampReverb as
                   `(Δ(reverb&0x7F)<<9)/blocks`, i.e. pre-scaled into the SAME reverb<<9 == a<<8
                   units (a = reverb*2, so a<<8 == reverb<<9). Storing `a<<8` here (instead of the
                   previous bare `a`) makes envReverbVol2 directly summable with envRampReverb and
                   directly usable in the same >>16 formula as curVolL/curVolR below -- resolving
                   the ambiguity the spec flagged without introducing a second scale factor. */
                envReverbVol2 = a << 8;
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

                sGdxUnlockStage.currentTarget = GdxUnlockStageFindCommandTarget(&cmds[i]);

                /* flatvol bypass: zero the per-block ramp so the whole tick uses one
                   constant volume -- tests whether the 8-block volume staircase is the
                   grain (grain A/B). */
                if (GdxAudioDbg() & 2) {
                    envRampLeft = envRampRight = envRampReverb = 0;
                }

                if (swapLR) {
                    uint32_t tmp = dryLeftDmem; dryLeftDmem = dryRightDmem; dryRightDmem = tmp;
                }

                GdxUnlockStageCapturePreEnvelope(dmemSrc, sampleCount);

                for (blk = 0; blk < numBlocks; blk++) {
                    uint32_t n;
                    for (n = 0; n < 8u && sIdx < sampleCount; n++, sIdx++) {
                        int32_t s = DmemGetS16(dmemSrc + sIdx * 2u);
                        /* Volumes here are Q16, NOT Q12: synthesis.c's
                           AudioSynth_ProcessEnvelope does `targetVol <<= 4`
                           before packing ENVSETUP1/2 (playback.c's Q12 value
                           times 16, full volume = 0xFFF0). >>16 is correct; a
                           >>12 experiment overdrove every normal-volume voice
                           16x into rail-to-rail clipping (41% clipped samples
                           in the AI tap) while quiet voices stayed clean —
                           heard as music buried under loud static. */
                        int32_t dl = (s * curVolL) >> 16;
                        int32_t dr = (s * curVolR) >> 16;
                        /* Task A2, nead semantics (mupen64plus-rsp-hle alist.c#L512-562
                           envmix_nead): wet CASCADES the DRY-SCALED sample, not the raw input --
                           `l2 = (l * env_values[2]) >> 16` where `l` is ITSELF already
                           `(in*env_values[0])>>16` (our `dl`). Previously this computed
                           `wet = (s*curReverb)>>8` directly off the raw input `s`, sharing one
                           mono value across both wet channels -- wrong-family model per the
                           emulator-comparison research. The reference cascades PER CHANNEL: wetL
                           off dl, wetR off dr (each channel's own dry-scaled sample), both through
                           envReverbVol2 which is now already in the a<<8 (Q16-shared) space set up
                           in GDX_A_ENVSETUP1 above, so the formula is the same >>16 shift used for
                           dl/dr -- no separate Q8 scale. (xors/headset-pan flags remain out of
                           scope, per file header.) */
                        int32_t wetL = (dl * curReverb) >> 16;
                        int32_t wetR = (dr * curReverb) >> 16;

                        GdxUnlockStageCaptureEnvelopeSample(sIdx, dl, dr);

                        DmemSetS16(dryLeftDmem + sIdx * 2u,
                                   ClampS16(DmemGetS16(dryLeftDmem + sIdx * 2u) + dl));
                        DmemSetS16(dryRightDmem + sIdx * 2u,
                                   ClampS16(DmemGetS16(dryRightDmem + sIdx * 2u) + dr));
                        DmemSetS16(wetLeftDmem + sIdx * 2u,
                                   ClampS16(DmemGetS16(wetLeftDmem + sIdx * 2u) + wetL));
                        DmemSetS16(wetRightDmem + sIdx * 2u,
                                   ClampS16(DmemGetS16(wetRightDmem + sIdx * 2u) + wetR));
                    }
                    curVolL += envRampLeft;
                    curVolR += envRampRight;
                    curReverb += envRampReverb;
                }
                /* [spike] mix-stage bisection: post-resample is now
                   clean but pre-interleave still spikes -- scan this op's own INPUT and
                   all four output buses so one run says whether the corruption arrives
                   WITH the source (upstream, e.g. DMEM_TEMP after filter/comb), is
                   injected by this op, or was already sitting on a bus (reverb return /
                   earlier voice accumulation). */
                {
                    static int sSpikeLogsEnvmix = 0;
                    if (sSpikeLogsEnvmix < 12) {
                        int si = GdxSpikeScan(dmemSrc, sampleCount);
                        int sdl = GdxSpikeScan(dryLeftDmem, sampleCount);
                        int sdr = GdxSpikeScan(dryRightDmem, sampleCount);
                        int swl = GdxSpikeScan(wetLeftDmem, sampleCount);
                        int swr = GdxSpikeScan(wetRightDmem, sampleCount);
                        if (si >= 0 || sdl >= 0 || sdr >= 0 || swl >= 0 || swr >= 0) {
                            sSpikeLogsEnvmix++;
                            /* NOTE-PATH BISECTION: when the envmixer's SOURCE (DMEM_TEMP)
                               carries the spike, name the op that last wrote that sample
                               via the last-writer tracker: 5=A_RESAMPLE, 7=A_FILTER,
                               14=A_HILOGAIN. Resample-clean-but-src-spiked was the whole
                               puzzle; this says whether filter or hilogain injects it. */
                            uint8_t srcWriter = (si >= 0)
                                ? sDmemLastOp[((dmemSrc + (uint32_t)si * 2u) & GDX_DMEM_MASK) >> 4]
                                : 0xFF;
                            gdx_port_logf("[spike] envmixer src=%d dryL=%d dryR=%d wetL=%d wetR=%d "
                                          "(dmemSrc=%04X n=%u) srcWriter=%u\n",
                                          si, sdl, sdr, swl, swr, dmemSrc, sampleCount, srcWriter);
                        }
                    }
                }
                sGdxUnlockStage.currentTarget = -1;
                break;
            }

            case GDX_A_HILOGAIN: {
                /* IN-PLACE op (missing booster/low-health root
                   cause): the real ucode scales the buffer at w1>>16 IN PLACE;
                   w1's low 16 bits are unused padding -- synthesis.c:1069
                   emits AudioSynth_HiLoGain(..., DMEM_TEMP, /out=/ 0, ...).
                   This case previously honored that 0 as a destination:
                   every gain-carrying note (boost, low-energy warning --
                   noteSubEu->gain != 0) wrote its scaled audio over DMEM 0
                   (clobbering scratch state consumed later in the tick) while
                   the real note path at DMEM_TEMP flowed on UNSCALED --
                   sounds missing plus collateral garbage, once per tick per
                   gained note. */
                int32_t gain = (int32_t)((w0 >> 16) & 0xFFu); /* Q4, 0x10 == 1.0x */
                uint32_t size = w0 & 0xFFFFu;
                uint32_t dmem = (w1 >> 16) & 0xFFFFu;
                uint32_t numSamples = size / 2u;
                uint32_t k;
                /* [rs-cap] T1: a HILOGAIN means this is a gained note (the booster/low-health
                   set) -- dump its FinalResample record, snapshotted just above in the list. */
                if (sRsCapT1 < 8 && sRsCapLast.valid) {
                    sRsCapT1++;
                    gdx_port_logf("[rs-cap] T1-hilogain gain=%02X size=%04X dmem=%04X\n",
                                  (unsigned)gain, (unsigned)size, (unsigned)dmem);
                    GdxRsCapDump("T1");
                }
                for (k = 0; k < numSamples; k++) {
                    int32_t s = DmemGetS16(dmem + k * 2u);
                    DmemSetS16(dmem + k * 2u, ClampS16((s * gain) >> 4));
                }
                break;
            }

            case GDX_A_FILTER: {
                /* Two-step protocol -- see RunFilter's header comment. f (bits 16..23 of w0)
                   distinguishes "prime" (f==2: load coefficients, remember the upcoming
                   buffer size) from "apply" (any other f: A_INIT/A_CONTINUE, actually filter
                   DMEM in place). countOrBuf (bits 0..15 of w0) is a coefficient-table BYTE
                   SIZE on the prime call, but a DMEM ADDRESS on the apply call -- it is never
                   itself resolved as a host pointer either way (unlike w1). */
                uint32_t f = (w0 >> 16) & 0xFFu;
                uint32_t countOrBuf = w0 & 0xFFFFu;
                if (f == 2u) {
                    void* coefSrc = GdxAudioResolveAddr(w1, "FILTER-coef");
                    if (coefSrc != NULL) {
                        memcpy(pendingFilterCoef, coefSrc, sizeof(pendingFilterCoef));
                        pendingFilterHaveCoef = 1;
                    }
                    pendingFilterSizeBytes = countOrBuf;
                } else if (GdxAudioDbg() & 1) {
                    /* nofilter bypass: leave the note buffer unfiltered (grain A/B). */
                } else {
                    int16_t* state = (int16_t*)GdxAudioResolveAddr(w1, "FILTER-state");
                    RunFilter(countOrBuf, pendingFilterSizeBytes, f, state,
                              pendingFilterHaveCoef ? pendingFilterCoef : NULL);
                    /* [spike] stage scan: filter runs in place on the note buffer. */
                    if (sSpikeLogsFilter < 16) {
                        int si = GdxSpikeScan(countOrBuf, pendingFilterSizeBytes / 2u);
                        if (si >= 0) {
                            sSpikeLogsFilter++;
                            gdx_port_logf("[spike] post-filter at=%d dmem=%04X n=%u fl=%u\n",
                                          si, countOrBuf, pendingFilterSizeBytes / 2u, f);
                        }
                    }
                }
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
    sGdxUnlockStage.rangeCount = 0;
    sGdxUnlockStage.commandList = 0;
}
