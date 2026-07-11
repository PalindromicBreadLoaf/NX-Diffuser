// port/n64_gfx_bridge.cpp — S3: C/C++ boundary for GFX task submission.
// Called from port/n64_sched.c (C) via extern "C" linkage.
// Uses GetInterpreterWeak() (public) + Interpreter::Run() directly — NOT
// DrawAndRunGraphicsCommands, which would double-wrap StartFrame/EndFrame.
// Build placement: G-Diffuser executable target only (NOT gdiffuser_game OBJECT library).

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "ship/Context.h"
#include "fast/Fast3dWindow.h"
#include "fast/lus_gbi.h"
#include "port_log.h"
#include "rom_buffer.h"
#include "n64_rdram.h"
#include "n64_gfx_bridge.h"
#include "n64_gfx_convert.h"
#include "gdx_vi_convert.h"
extern "C" {
#include "mio0.h"
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

extern "C" int gGdxRaceActive;
// #16 investigation aid: decomp sets this to 1 (racer.c, right where it already
// logs "[countdown] draw emitted") the instant the countdown draw code runs, so
// the bridge's raw vtx/mtx trace only has to cover the interesting few frames
// instead of the whole race -- gGdxRaceActive alone stays 1 for the entire race
// and made a fixed-size trace file fill up long before the countdown appeared.
extern "C" int gGdxCountdownProbeArm = 0;
// #16 phase 3: the coarse arm above stays 1 for the rest of the process once
// the countdown first runs, so an edge-trigger on it alone catches whatever
// triangle the interpreter happens to reach first afterward -- not necessarily
// the countdown digit quad. decomp (racer.c) tags the digit quad's own vertex
// pointer here (still a raw N64-style low32 at translate time, so it can be
// compared directly against in.w1 below) right before drawing it; once this
// bridge sees a G_VTX command whose raw pointer matches, it publishes the
// RESOLVED host pointer so interpreter.cpp's GfxSpVertex can recognize that
// exact draw and tighten the render-state probe to it.
extern "C" unsigned int gGdxCountdownProbeVtxLow32 = 0;
extern "C" uintptr_t gGdxCountdownProbeResolvedVtx = 0;
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>
#include <chrono>

extern "C" unsigned long long gSegments[16];

extern "C" uint8_t D_3000000[];
extern "C" uint8_t D_3000028[];
extern "C" uint8_t D_3000050[];
extern "C" uint8_t D_3000088[];
extern "C" uint8_t D_30000C0[];
extern "C" uint8_t D_3000100[];
extern "C" uint8_t D_3000138[];
extern "C" uint8_t D_3000170[];
extern "C" uint8_t D_30001A8[];
extern "C" uint8_t D_3000270[];
extern "C" uint8_t D_30002E0[];
extern "C" uint8_t D_3000338[];
extern "C" uint8_t D_3000400[];
extern "C" uint8_t D_3000438[];
extern "C" uint8_t D_3000470[];
extern "C" uint8_t D_30004A8[];
extern "C" uint8_t D_30004E0[];
extern "C" uint8_t D_3000510[];
extern "C" uint8_t D_3000540[];
extern "C" uint8_t D_3000590[];
extern "C" uint8_t D_30005D8[];
extern "C" uint8_t D_3000688[];
extern "C" uint8_t D_30006D0[];
extern "C" uint8_t aVpFullScreen[];
extern "C" uint16_t D_A000000_235130[];
extern "C" uint16_t D_A000000_239A80[];
extern "C" uint16_t D_A000000_23EC50[];
extern "C" uint16_t D_A000000_243D90[];
extern "C" uint16_t D_A000000_24A270[];
extern "C" uint16_t D_A000000_2507F0[];
extern "C" uint16_t D_A000000_255100[];
extern "C" uint16_t D_A000000_259600[];
extern "C" uint16_t D_A000000_25F360[];
extern "C" uint16_t D_A000000_266C20[];
extern "C" uint16_t D_A000000_26D780[];
extern "C" uint8_t D_2000000[];
extern "C" uint8_t D_80225800[];
extern "C" uint8_t D_1000000[];
/* Unsuffixed venue texture bank symbols (segment 0x0A, 0x1000-byte banks).
   course.c's road material table references them directly (ROAD_1..WALLED_ROAD),
   but they are 1-byte LinkStubs — the per-venue data loads via the suffixed
   symbols (D_A000000_235130 etc.) into gSegments[0x0A]. */
extern "C" uint8_t D_A000000[];
extern "C" uint8_t D_A001000[];
extern "C" uint8_t D_A002000[];
extern "C" uint8_t D_A003000[];
extern "C" uint8_t D_A004000[];
extern "C" uint8_t D_A005000[];
extern "C" uint8_t D_A006000[];
extern "C" uint8_t D_A007000[];
extern "C" uint8_t D_A008000[];
extern "C" uint8_t gspF3DEX2_fifoTextStart[];
extern "C" uint8_t gspF3DFLX2_Rej_fifoTextStart[];
extern "C" uint8_t gspF3DLX2_Rej_fifoTextStart[];
extern "C" unsigned long long gspF3DEX2_Rej_fifoTextStart[];
extern "C" unsigned long long gspL3DEX2_fifoTextStart[];

extern "C" int gdx_lookup_asset_segment(unsigned int sym_low32,
                                         unsigned char* segment,
                                         unsigned int* rom_base,
                                         unsigned char* compressed,
                                         unsigned int* offset,
                                         unsigned int* image_size);
extern "C" int gdx_lookup_asset_segment_interior(unsigned int sym_low32,
                                                  unsigned char* segment,
                                                  unsigned int* rom_base,
                                                  unsigned char* compressed,
                                                  unsigned int* offset,
                                                  unsigned int* image_size);
extern "C" void gdx_fixup_asset_segment_image(unsigned char segment,
                                               unsigned int rom_base,
                                               unsigned char* data,
                                               unsigned int size);
extern "C" void gdx_register_asset_segment_command_ranges(unsigned char segment,
                                                            unsigned int rom_base,
                                                            unsigned char* data,
                                                            unsigned int size);
extern "C" const char* GDiffuser_LookupLoadedAssetKey(const void* buffer, size_t minSize, int requireUnmodified);
extern "C" const char* gdx_lookup_asset_segment_o2r_key(unsigned int sym_low32);

namespace {

// The decomp builds N64 display-list packets as two 32-bit words. libultraship's Fast3D
// interpreter reads packets as two uintptr_t words on 64-bit hosts. Never cast the decomp
// command stream directly to LUS Gfx*; expand it first.
struct N64Gfx {
    uint32_t w0;
    uint32_t w1;
};

static_assert(sizeof(N64Gfx) == 8, "N64 display-list packets must stay 8 bytes");

constexpr size_t kN64GfxStride = sizeof(N64Gfx);
// Host-built decomp Gfx packets can be wider than the original 8-byte N64
// packet because some union members carry host pointers on 64-bit builds.
// Read host-built lists with the host stride; RDRAM/ROM decoded lists stay 8-byte.
constexpr size_t kHostBuiltGfxStride = (sizeof(uintptr_t) > 4) ? 16 : kN64GfxStride;

static inline uint32_t Byteswap32(uint32_t x);

N64Gfx ReadRawCommand(const N64Gfx* source, size_t index, size_t stride) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(source) + (index * stride);
    N64Gfx command = {};
    std::memcpy(&command, bytes, sizeof(command));
    return command;
}

N64Gfx ReadCommand(const N64Gfx* source, size_t index, size_t stride, bool isBig) {
    N64Gfx command = ReadRawCommand(source, index, stride);
    if (isBig) {
        command.w0 = Byteswap32(command.w0);
        command.w1 = Byteswap32(command.w1);
    }
    return command;
}

static inline uint32_t Byteswap32(uint32_t x) {
    return ((x & 0xFF000000) >> 24) |
           ((x & 0x00FF0000) >> 8)  |
           ((x & 0x0000FF00) << 8)  |
           ((x & 0x000000FF) << 24);
}

static inline uint16_t Byteswap16(uint16_t x) {
    return ((x & 0xFF00) >> 8) | ((x & 0x00FF) << 8);
}

bool IsLikelyDisplayListOpcode(uint8_t op); // defined below

static inline bool IsLikelyBigEndianDisplayList(const N64Gfx* source, size_t readableLimit) {
    if (readableLimit == 0) return false;
    uint32_t w0 = source[0].w0;
    uint8_t opL = w0 >> 24;
    uint8_t opB = w0 & 0xFF;
    if (opL == 0 && opB != 0) return true;
    if ((opB >= 0xB0 || opB == 0x01 || opB == 0x04) && opL < 0x20) return true;
    /* The first word alone is ambiguous when the BE command carries nonzero
       operand bytes (e.g. gSPGeometryMode D9 FD FF FF reads as LE 0xFFFFFDD9,
       whose top byte 0xFF is also a plausible opcode — this made every EK
       disk-filled UI display list classify as LE and get rejected). Walk the
       list as big-endian: a genuine BE list produces a valid opcode chain
       that reaches G_ENDDL within the readable window. */
    {
        const size_t scan = (readableLimit < 64) ? readableLimit : 64;
        for (size_t i = 0; i < scan; i++) {
            const uint8_t op = static_cast<uint8_t>(source[i].w0 & 0xFFu);
            if (!IsLikelyDisplayListOpcode(op)) return false;
            if (op == 0xDF || op == 0xB8) return true; // G_ENDDL (EX2 / F3D)
        }
    }
    return false;
}

constexpr uint8_t kGfxSegmentCount = 16;
constexpr uint32_t kSegmentOffsetLimit = 0x01000000;
constexpr size_t kMaxUnboundedDisplayListCommands = 1 << 20;

constexpr uint8_t kOpVtx = 0x01;
constexpr uint8_t kOpBranchZ = 0x04;
// G_BRANCH_Z in the original F3D microcode (used by F-Zero X race DLs).
// In F3D: G_IMMFIRST=0xE5, so G_BRANCH_Z = 0xE5-15 = 0xD6.
// In F3DEX2: G_BRANCH_Z = 0x04 (kOpBranchZ above).
// Legacy F3D assets still omit this optimization; host-built F3DEX2 lists retain
// opcode 0x04 and evaluate it against the interpreter's transformed vertex state.
constexpr uint8_t kOpBranchZF3D = 0xD6;
constexpr uint8_t kOpEndDl = 0xDF;
constexpr uint8_t kOpDl = 0xDE;
constexpr uint8_t kOpMovemem = 0xDC;
constexpr uint8_t kOpMoveword = 0xDB;
constexpr uint8_t kOpMtx = 0xDA;
constexpr uint8_t kOpRdpHalf1 = 0xE1;
constexpr uint8_t kOpLoadTlut = 0xF0;
constexpr uint8_t kOpSetTileSize = 0xF2;
constexpr uint8_t kOpLoadBlock = 0xF3;
constexpr uint8_t kOpLoadTile = 0xF4;
constexpr uint8_t kOpSetTile = 0xF5;
constexpr uint8_t kOpSetColorImage = 0xFF;
constexpr uint8_t kOpSetDepthImage = 0xFE;
constexpr uint8_t kOpSetTextureImage = 0xFD;
constexpr uint8_t kOpSetTextureImageOtrFilepath = 0x25;

constexpr uint8_t kMovewordSegmentIndex = 0x06;
constexpr size_t kDisplayListValidationCommandLimit = 1 << 16;
constexpr size_t kTextureLoadScanCommandLimit = 2048;
constexpr size_t kMinRawTextureCopyBytes = 8;
constexpr size_t kMaxRawTextureCopyBytes = 1 << 20;
constexpr uint32_t kTextureImageFrac = 2;
constexpr uintptr_t kSetupGfxRomOffset = 0x17B1E0;
constexpr size_t kSetupGfxSize = 0x778;

// Use G_IM_FMT_* / G_IM_SIZ_* macros from lus_gbi.h (included above).
// G_IM_FMT_RGBA=0  G_IM_FMT_YUV=1  G_IM_FMT_CI=2  G_IM_FMT_IA=3  G_IM_FMT_I=4
// G_IM_SIZ_4b=0  G_IM_SIZ_8b=1  G_IM_SIZ_16b=2  G_IM_SIZ_32b=3

uint8_t Opcode(uint32_t w0) {
    return static_cast<uint8_t>(w0 >> 24);
}

uint8_t WordParam(uint32_t w0) {
    return static_cast<uint8_t>((w0 >> 16) & 0xFF);
}

bool IsLikelyDisplayListOpcode(uint8_t op) {
    if (op <= 0x09) return true;
    if ((op >= 0x20) && (op <= 0x49)) return true;
    if ((op >= 0xB0) && (op <= 0xBF)) return true;  // F3D opcodes: G_TRI1, G_ENDDL, G_TEXTURE, etc.
    if ((op >= 0xC8) && (op <= 0xCF)) return true;
    if ((op >= 0xD3) && (op <= 0xE3)) return true;
    if ((op >= 0xE4) && (op <= 0xFF)) return true;
    return false;
}

Fast::F3DGfx MakeLusGfx(uintptr_t w0, uintptr_t w1) {
    Fast::F3DGfx gfx = {};
    gfx.words.w0 = w0;
    gfx.words.w1 = w1;
    return gfx;
}

struct ResolvedAddress {
    uintptr_t full = 0;
    uint8_t segment = 0;
    uint32_t offset = 0;
    bool segmented = false;
};

struct ConversionStats {
    std::array<size_t, 256> opCounts{};
    size_t convertedLists = 0;
    size_t noopDisplayLists = 0;
    size_t fallbackDataCommands = 0;
    size_t skippedDataCommands = 0;
    size_t skippedTextures = 0;
    size_t textureCopies = 0;
    size_t textureCopyBytes = 0;
    size_t commandsOut = 0;
    size_t f3dLists = 0;
    size_t ucodeSwitches = 0;
    size_t unknownUcodeSwitches = 0;
    uint32_t firstUnknownUcodeRaw = 0;
    /* Deliberate L3DEX2 (line ucode) section skips, counted SEPARATELY from
       unknownUcodeSwitches: the old shared counter made the benign per-menu-frame
       L3DEX2 skip (raw = low32 of gspL3DEX2_fifoTextStart) indistinguishable from
       a genuinely unmatched G_LOAD_UCODE in the [gfxdiag] line (graphics wave W1). */
    size_t l3dexUcodeSkips = 0;
    uint32_t firstL3dexUcodeRaw = 0;
    uint8_t firstFallbackDataOp = 0;
    uint32_t firstFallbackDataRaw = 0;
    uint32_t firstFallbackDataW0 = 0;
    uintptr_t firstFallbackDataSource = 0;
    size_t firstFallbackDataIndex = 0;
    uint8_t firstSkippedDataOp = 0;
    uint32_t firstSkippedDataRaw = 0;
    uint32_t firstSkippedDataW0 = 0;
    uint32_t firstNoopDlRaw = 0;
    size_t missingDisplayLists = 0;
    size_t badDisplayLists = 0;
    uint32_t firstMissingDlRaw = 0;
    uint32_t firstBadDlRaw = 0;
    uintptr_t firstMissingParent = 0;
    size_t firstMissingParentIndex = 0;
    size_t firstMissingParentStride = 0;
    bool firstMissingParentBigEndian = false;
    bool firstMissingParentF3D = false;
    uint32_t firstMissingParentRawW0 = 0;
    uint32_t firstMissingParentRawW1 = 0;
    uint32_t firstMissingParentDecodedW0 = 0;
    uint32_t firstMissingParentDecodedW1 = 0;
    uintptr_t firstBadDlTarget = 0;
    size_t firstBadDlLimit = 0;
    size_t firstBadDlStride = 0;
    bool firstBadDlBigEndian = false;
    bool firstBadDlF3D = false;
    uint32_t firstBadDlFirstW0 = 0;
    uint32_t firstBadDlFirstW1 = 0;
    size_t firstBadDlFailureIndex = 0;
    uint8_t firstBadDlFailureOpcode = 0;
    uint8_t firstBadDlFailureReason = 0; // 1=zero limit, 2=invalid opcode, 3=no terminator

    /* Every distinct resolved G_SETCOLORIMAGE host address seen while converting
       this task's display list (deduped, capped). A single task frequently
       redirects CIMG to an offscreen N64 framebuffer mid-task (the SETCIMG
       "canvas" idiom in texture_utils.c's func_8007AB88/func_8007ABA4 and the
       OBJECT_FRAMEBUFFER object type) and then restores it before the task
       ends. The end-of-task mirror only ever inspected the FINAL color image,
       so any framebuffer that was only a mid-task target never got mirrored to
       CPU memory and stayed permanently invalid. Track every target here so
       gdx_gfx_run can mirror all of them, not just the last one. */
    std::array<uintptr_t, 8> colorImageTargets{};
    size_t colorImageTargetCount = 0;
};

struct HostRange {
    uintptr_t begin = 0;
    size_t size = 0;
};

struct N64AddressRange {
    uint32_t n64Begin = 0;
    uintptr_t hostBegin = 0;
    size_t size = 0;
};

struct PersistentRawTextureCopy {
    uintptr_t source = 0;
    size_t size = 0;
    std::unique_ptr<uint8_t[]> bytes;
    uint64_t dmaGenAtCopy = UINT64_MAX;
};

struct N64FramebufferInfo {
    uintptr_t address = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool valid = false;
};

std::vector<HostRange> gHostRanges;
std::vector<HostRange> gRawN64Ranges;
std::vector<HostRange> gHostN64CommandRanges;
std::vector<N64AddressRange> gN64AddressRanges;
std::vector<HostRange> gF3DAssetRanges;
std::vector<HostRange> gNativeRgba16Ranges;
std::vector<uint8_t> gSetupGfxSegment;
std::vector<PersistentRawTextureCopy> gRawTextureCopies;
std::vector<uintptr_t> gPendingTextureCacheDeletes;
std::vector<std::unique_ptr<uint8_t[]>> gPersistentAllocations; // Fixed undefined mPersistentAllocations
std::vector<N64FramebufferInfo> gN64Framebuffers;
uintptr_t gViCurrentFramebuffer = 0;
uintptr_t gViNextFramebuffer = 0;
uintptr_t gLastRenderedFramebuffer = 0;

// Phase G2 coarse asset epoch: bumped whenever an asset/ROM-backed segment image
// is (re)decoded (EnsureAssetSegmentImage). Declared here -- ahead of that
// function -- so it can invalidate converted lists built against an old image.
// The rest of the G2 converter state lives further down (needs the resolver
// helpers forward-declared below).
uint32_t gConvertEpoch = 1;

// Set by gdx_gfx_run() whenever a real GFX task renders into the current host
// frame; checked + cleared once per frame by gdx_vi_present_fallback(). When it
// is false at present time, no task produced this frame (boot-logo phase or any
// other CPU-drawn screen) and the fallback must scan out the VI framebuffer.
bool gHostFrameGfxTaskRan = false;

struct AssetSegmentLookup {
    uint8_t segment = 0;
    uint32_t romBase = 0;
    bool compressed = false;
    uint32_t offset = 0;
    uint32_t imageSize = 0;
};

struct LoadedAssetSegment {
    uint8_t segment = 0;
    uint32_t romBase = 0;
    bool compressed = false;
    std::vector<uint8_t> bytes;
};

std::vector<LoadedAssetSegment> gLoadedAssetSegments;

/* Hybrid segment-tag table: static asset segments have a KNOWN display-list
 * dialect, so DLs inside their decompressed images never need the per-DL
 * opcode-scan heuristics (which misclassify, e.g., F3DEX2 setup DLs whose
 * byte stream happens to contain 0xB8 before 0xDF).
 *   segment 0x08 — course_track_gfx: F3DEX2 (setup DLs use 0xD9 geometry
 *                  mode, 0xDF G_ENDDL, and 0x06 as G_TRI2)
 *   segment 0x0A — venue textures: data only, no display lists
 * Segments not listed keep the existing heuristic path.
 *
 * segment 0x03 (machine_custom_gfx) is INTENTIONALLY absent: it is not a
 * single dialect. ROM ground truth (baserom.us.rev0.z64, decompressed
 * segment-3 image, offset 0x6D0) shows a genuine F3DEX2 sub-list —
 * G_VTX(0x01)+3xG_TRI2(0x06) terminated by G_ENDDL=0xDF — sitting a few
 * bytes before named legacy-F3D machine-part DLs in the same segment
 * (e.g. D_3000780). A prior blanket "segment 0x03 = F3D" tag forced every
 * G_TRI2 in that F3DEX2 sub-list through the F3D-only "0x06 = G_DL"
 * remap (see the isF3DSource-gated case 0x06 below), sending vertex-index
 * pairs (raw=0x00000406/0x00080A0C/0x000A0E0C, etc.) into
 * TranslateDisplayListPointer as if they were sub-DL pointers — the
 * dominant new [gdl-bad] classes after the resolver-window fix, and the
 * likely source of the custom-machine-body garbage wedges (race
 * decorations/booster glow, machine-select preview). Falling through to
 * the per-DL heuristic below (DisplayListUsesF3D) restores correct
 * per-list detection via the real 0xB8/0xDF terminator, the same path
 * every other unlisted segment already relies on. */
enum class GdxSegmentUcode : uint8_t { Unknown = 0, F3D, F3DEX2 };

static GdxSegmentUcode GdxSegmentDialect(uint8_t segment) {
    /* REVERTED 2026-07-11 (same day): a rigorous word-level re-decode of the
     * decoration DLs (D_80172A0: 0xD7 G_TEXTURE, 0x01 G_VTX, 0x05 G_TRI1 runs,
     * 0xDF terminator) proves they are F3DEX2 -- the blanket below was CORRECT
     * and the earlier "they are F3D" identification was a dual-dialect
     * pattern-matching error. The decorations' real defect is the fixup-region
     * vertex-block swap (see sAssetFixups split, AssetBindings.c). */
    switch (segment) {
        case 0x08:
            return GdxSegmentUcode::F3DEX2;
        default:
            return GdxSegmentUcode::Unknown;
    }
}

GdxSegmentUcode GdxAssetPointerDialect(uintptr_t addr) {
    for (const LoadedAssetSegment& seg : gLoadedAssetSegments) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(seg.bytes.data());
        if ((base != 0) && (addr >= base) && (addr < base + seg.bytes.size())) {
            return GdxSegmentDialect(seg.segment);
        }
    }
    return GdxSegmentUcode::Unknown;
}

// Incremented on every DMA chunk loaded into RDRAM. RDRAM texture copies record
// the generation at copy time; a mismatch on the next frame triggers a re-upload.
// This avoids expensive per-frame memcmp against large raw texture copies.
static uint64_t gDmaGeneration = 0;
struct DmaDirtyRange {
    uintptr_t begin;
    uintptr_t end;
    uint64_t generation;
};
static std::vector<DmaDirtyRange> gDmaDirtyRanges;

void RecordHostWrite(uintptr_t begin, size_t size) {
    ++gDmaGeneration;
    if (begin == 0 || size == 0 || size > UINTPTR_MAX - begin) {
        return;
    }
    gDmaDirtyRanges.push_back({ begin, begin + size, gDmaGeneration });
    if (gDmaDirtyRanges.size() > 4096) {
        gDmaDirtyRanges.erase(gDmaDirtyRanges.begin(), gDmaDirtyRanges.begin() + 2048);
    }
}

} // namespace (temporarily close to define extern "C")

extern "C" void gdx_record_dma_load(uint32_t rdram_phys, uint32_t rom_offset, uint32_t size) {
    (void)rom_offset;
    ++gDmaGeneration;
    if (gdx_rdram != nullptr && size != 0 && rdram_phys < GDX_RDRAM_SIZE) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(gdx_rdram) + rdram_phys;
        const uintptr_t end = begin + std::min<size_t>(size, GDX_RDRAM_SIZE - rdram_phys);
        gDmaDirtyRanges.push_back({begin, end, gDmaGeneration});
        if (gDmaDirtyRanges.size() > 4096) {
            gDmaDirtyRanges.erase(gDmaDirtyRanges.begin(), gDmaDirtyRanges.begin() + 2048);
        }
    }
}

namespace {

bool HostRangeChanged(uintptr_t source, size_t size, uint64_t sinceGeneration) {
    if (sinceGeneration == gDmaGeneration) {
        return false;
    }
    if (!gDmaDirtyRanges.empty() && sinceGeneration < gDmaDirtyRanges.front().generation) {
        return true;
    }

    const uintptr_t end = source + size;
    for (auto it = gDmaDirtyRanges.rbegin(); it != gDmaDirtyRanges.rend(); ++it) {
        if (it->generation <= sinceGeneration) {
            break;
        }
        if (source < it->end && end > it->begin) {
            return true;
        }
    }
    return false;
}

bool IsN64FramebufferRange(uintptr_t source, size_t size) {
    if (size > UINTPTR_MAX - source) {
        return false;
    }
    const uintptr_t end = source + size;
    for (const N64FramebufferInfo& framebuffer : gN64Framebuffers) {
        const size_t framebufferSize =
            static_cast<size_t>(framebuffer.width) * framebuffer.height * sizeof(uint16_t);
        if (framebufferSize <= UINTPTR_MAX - framebuffer.address &&
            source >= framebuffer.address && end <= framebuffer.address + framebufferSize) {
            return true;
        }
    }
    return false;
}

bool IsNativeRgba16Range(uintptr_t source, size_t size) {
    for (const HostRange& range : gNativeRgba16Ranges) {
        if (source >= range.begin && source + size <= range.begin + range.size) {
            return true;
        }
    }
    return false;
}

void CopyRawTextureBytes(uint8_t* destination, uintptr_t source, size_t size) {
    const auto* input = reinterpret_cast<const uint8_t*>(source);
    if (!IsNativeRgba16Range(source, size)) {
        std::memcpy(destination, input, size);
        return;
    }

    size_t i = 0;
    for (; i + 1 < size; i += 2) {
        destination[i] = input[i + 1];
        destination[i + 1] = input[i];
    }
    if (i < size) {
        destination[i] = input[i];
    }
}

// ---------------------------------------------------------------------------
// Phase G3: quarantine of the pointer-GUESSING resolver branches.
//
// G1 (architecture/g1-wide-gfx) made game-emitted display lists carry real
// 64-bit host pointers. G2 (architecture/g2-binary-dl-converters) converts
// every binary N64 (8-byte) list to that same wide layout ONCE, at first
// encounter, with deterministic pointer resolution (segment table + RDRAM-
// arena physical strip only -- never a low32-window match or a high-32
// reconstruction). Together they mean the guessing machinery below --
// ResolveRegisteredHostPointer's low32-window match, the KSEG0 high-32
// reconstruction, the physical/source-window high-32 reconstructions, the
// ambiguous cross-segment fallback, and the raw-buffer/last-resort
// substitutions -- should now only ever fire for STRAGGLERS: narrow lists
// reached while GDX_G2_CONVERT=0 (the G2 kill switch) or a G2 conversion
// miss, and legacy F3D asset paths G2 does not touch.
//
// Neither G1 nor G2 has been runtime-verified yet (no game launch permitted).
// A hard delete of the guessing paths right now would remove the safety net
// before a single soak run proves they are unused. Instead, every guessing
// branch is gated behind GDX_LEGACY_RESOLVE (default ON, so soak-build
// behavior is UNCHANGED) and instrumented so the next run quantifies exactly
// what still relies on guessing: a per-branch hit counter, a capped
// "[legacy-resolve] branch=<name> hits=<n> raw=%08X op=%02X" log line for the
// first 8 hits of each branch, and a one-time "[legacy-resolve] SUMMARY" line
// the instant any branch fires for the first time this run. Zero hits across
// a full race is the green light to delete.
//
// Flip the default OFF by changing this one line once the soak is clean:
/* W3 flip (2026-07-10, campaign doc §W3): the W2 soak ran a full session
 * (boot→race→close) with ZERO legacy-resolve hits — the guessing branches
 * were already idle in the widened-pointer world. Default OFF; the
 * [legacy-resolve] SUMMARY line still names any straggler instantly, and
 * GDX_LEGACY_RESOLVE=1 restores the old machinery without a rebuild. */
constexpr bool kGdxLegacyResolveDefaultEnabled = false;
//
// POST-SOAK DELETION CHECKLIST (only after a full race shows zero hits on
// EVERY branch below with GDX_LEGACY_RESOLVE left at its default):
//   1. ResolveRegisteredHostPointer() and its call site in TryResolveAddress
//      -- the registered-host low32-window match.
//   2. The "Out-of-RDRAM KSEG0" high-32 reconstruction sub-block inside the
//      KSEG0/KSEG1 branch of TryResolveAddress (the highCandidates loop over
//      mRootBegin/mModuleBegin + gHostRanges). KEEP the deterministic
//      raw & 0x1FFFFFFF RDRAM strip directly above it -- that part mirrors
//      G2ResolvePhysical and is not a guess.
//   3. The tryPhysicalWindow / tryAllPhysicalWindows lambdas in
//      TryResolveAddress and both call sites (preferPhysical branch and the
//      post-segment-table fallback branch).
//   4. The trySourceWindow lambda in TryResolveAddress and both call sites.
//   5. The "ambiguous cross-segment fallback" SegCandidate sort-and-probe
//      block in TryResolveAddress.
//   6. The mModuleBegin high-32 reconstruction block (after the cross-segment
//      fallback, before the raw>=0x10000000 scan) in TryResolveAddress.
//   7. The raw>=0x10000000 highCandidates scan (mRootBegin/mModuleBegin/
//      gSegments[]/gHostRanges high32 substitution) in TryResolveAddress.
//   8. FallbackDataPointer() + the kFallbackIdentityMtx/kFallbackViewport/
//      kFallbackVertices static buffers, and their call sites in ProcessList.
//   9. The raw-as-direct-pointer cast fallback at the end of
//      TranslateDataPointer (the `direct = static_cast<uintptr_t>(raw)` path).
//  10. This entire quarantine block (flag, counters, RecordLegacyResolveHit)
//      once nothing references it anymore.
// Do NOT delete: segment-table lookups (both the explicit-segment and
// encodedSegment paths), ResolvePortBssAlias/ResolveVenueBankAlias/
// ResolveGeneratedAssetStub/ResolveSetupGfxStub (exact known-symbol matches,
// not guesses), the D_1000000 special case, the EK gN64AddressRanges reverse
// scan, texture/framebuffer image op handling (SETTIMG/SETCIMG/SETZIMG),
// BRANCH_Z/DMA_IO/RDPHALF_1, or the G2 converter/cache/GDX_G2_CONVERT switch.
// ---------------------------------------------------------------------------
enum class LegacyResolveBranch : uint8_t {
    kRegisteredHostLow32 = 0, // ResolveRegisteredHostPointer: low32-window match
    kKseg0High32,             // out-of-RDRAM KSEG0/KSEG1 high-32 reconstruction
    kPhysicalWindow,          // tryPhysicalWindow/tryAllPhysicalWindows
    kSourceWindow,            // trySourceWindow (referencing-DL-window guess)
    kCrossSegmentFallback,    // ambiguous cross-segment SegCandidate sort
    kModuleHigh32,            // mModuleBegin high-32 reconstruction
    kRawHigh32Scan,           // raw>=0x10000000 highCandidates scan
    kFallbackBuffer,          // FallbackDataPointer identity/viewport/vtx buffers
    kDirectCast,              // TranslateDataPointer raw-as-pointer last resort
    kCount,
};

inline const char* LegacyResolveBranchName(LegacyResolveBranch branch) {
    switch (branch) {
        case LegacyResolveBranch::kRegisteredHostLow32: return "reghost_low32";
        case LegacyResolveBranch::kKseg0High32: return "kseg0_high32";
        case LegacyResolveBranch::kPhysicalWindow: return "phys_window";
        case LegacyResolveBranch::kSourceWindow: return "src_window";
        case LegacyResolveBranch::kCrossSegmentFallback: return "cross_seg_fallback";
        case LegacyResolveBranch::kModuleHigh32: return "module_high32";
        case LegacyResolveBranch::kRawHigh32Scan: return "raw_high32_scan";
        case LegacyResolveBranch::kFallbackBuffer: return "fallback_buffer";
        case LegacyResolveBranch::kDirectCast: return "direct_cast";
        default: return "unknown";
    }
}

inline uint64_t (&LegacyResolveHitCounters())[static_cast<size_t>(LegacyResolveBranch::kCount)] {
    static uint64_t hits[static_cast<size_t>(LegacyResolveBranch::kCount)] = {};
    return hits;
}

// Set by ProcessList before each command is handled, so both member (TryResolveAddress)
// and free-function (ResolveRegisteredHostPointer, FallbackDataPointer) guessing
// branches can tag their [legacy-resolve] log lines with the opcode that triggered
// them, without threading a new parameter through every call site. 0xFF = unknown
// (a guess fired outside the per-command loop, e.g. from ResolveDisplayListSource).
uint8_t gLegacyResolveCurrentOp = 0xFFu;

// Runtime kill switch: GDX_LEGACY_RESOLVE=0 disables every guessing branch below
// (each one then behaves as if it never matched), restoring the old machinery
// instantly for the current process without a rebuild. Default ON so the soak
// build's behavior is byte-for-byte unchanged from before this phase.
inline bool LegacyResolveEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("GDX_LEGACY_RESOLVE");
        if (env != nullptr) {
            return env[0] != '0';
        }
        return kGdxLegacyResolveDefaultEnabled;
    }();
    return enabled;
}

// Always-on bookkeeping: increments the branch counter and prints the capped
// diagnostic lines described above. Called only from the success path of a
// guessing branch (i.e. "this guess actually produced a resolution"), so a
// branch with hits==0 across a full run genuinely never contributed anything.
inline void RecordLegacyResolveHit(LegacyResolveBranch branch, uint32_t raw, uint8_t op) {
    static bool sAnySeen = false;
    uint64_t& hits = LegacyResolveHitCounters()[static_cast<size_t>(branch)];
    ++hits;
    if (!sAnySeen) {
        sAnySeen = true;
        gdx_port_logf("[legacy-resolve] SUMMARY: first legacy-resolve hit this run (branch=%s) -- "
                      "soak is NOT clean yet. GDX_LEGACY_RESOLVE=%d\n",
                      LegacyResolveBranchName(branch), LegacyResolveEnabled() ? 1 : 0);
    }
    if (hits <= 8) {
        gdx_port_logf("[legacy-resolve] branch=%s hits=%llu raw=%08X op=%02X\n",
                      LegacyResolveBranchName(branch), static_cast<unsigned long long>(hits), raw, op);
    }
}

alignas(8) const int32_t kFallbackIdentityMtx[16] = {
    0x00010000, 0x00000000,
    0x00000001, 0x00000000,
    0x00000000, 0x00010000,
    0x00000000, 0x00000001,
    0, 0, 0, 0, 0, 0, 0, 0,
};

alignas(8) const int16_t kFallbackViewport[8] = {
    640, 480, 0x03FF, 0,
    640, 480, 0, 0,
};

/* Zeroed, fully-readable stand-in substituted for an unreadable vertex
 * pointer (see the G_VTX crash failsafe in ProcessList). It MUST be at least
 * as large as the largest vertex load the interpreter can be asked to perform
 * from a single G_VTX command, because the failsafe only swaps the POINTER --
 * it does NOT reduce the vertex COUNT the interpreter re-reads from the command
 * word (C0(12,8) for F3DEX2). F3DEX2 encodes that count in 8 bits, so up to 255
 * vertices * sizeof(F3DVtx)(16B) can be read. A garbage/desynced command with
 * count=0xF0 (=240, 3840B) previously overran the old 64-entry (1024B) buffer
 * and faulted deep inside Fast::Interpreter::GfxSpVertex (campaign soak fix 3):
 * the fix-1 failsafe fired but handed the interpreter a buffer far smaller than
 * the count it still walked. Size for the full 8-bit range (256 entries). */
alignas(8) const uint8_t kFallbackVertices[256 * 16] = {};

uint32_t Low32(uintptr_t value) {
    return static_cast<uint32_t>(value);
}

/* PE preferred image base (map header: "Preferred load address is
   0000000140000000"). The runtime module base is ASLR-randomized every launch,
   so a logged runtime pointer/low32 cannot be resolved against G-Diffuser.map
   directly. Emitting (kPreferredImageBaseVA + moduleOffset) turns any diagnostic
   pointer into a deterministic map lookup regardless of the run's base. Kept as
   uint64_t so the arithmetic is valid on a 32-bit host build too. */
constexpr uint64_t kPreferredImageBaseVA = 0x140000000ULL;

uint32_t ReadBE32(const uint8_t* src) {
    return (static_cast<uint32_t>(src[0]) << 24) |
           (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8) |
           static_cast<uint32_t>(src[3]);
}

bool LookupAssetSegment(uint32_t raw, AssetSegmentLookup& out) {
    unsigned char segment = 0;
    unsigned int romBase = 0;
    unsigned char compressed = 0;
    unsigned int offset = 0;
    unsigned int imageSize = 0;

    if (gdx_lookup_asset_segment(raw, &segment, &romBase, &compressed, &offset, &imageSize) == 0 &&
        gdx_lookup_asset_segment_interior(raw, &segment, &romBase, &compressed, &offset, &imageSize) == 0) {
        return false;
    }

    out.segment = segment;
    out.romBase = romBase;
    out.compressed = compressed != 0;
    out.offset = offset;
    out.imageSize = imageSize;
    return true;
}

/* True when a pointer's low32 is a generated ASSET PLACEHOLDER symbol (an
 * exact or interior match in the asset-segment map). These 1-byte BSS stub
 * symbols (e.g. setup_gfx's D_3000050, referenced by gSPDisplayList/gSPVertex)
 * stand in for data that is loaded into a runtime segment; their real DL/vertex
 * bytes live in the loaded segment image, NOT at the stub's own address. Such a
 * pointer must be resolved through the segment/asset path (TryResolveAddress ->
 * ResolveGeneratedAssetStub), never used verbatim. Used by ProcessList to keep
 * a wide packet carrying a placeholder from being mistaken for a real host
 * pointer just because its high32 is set (campaign soak fix 3). */
bool IsAssetPlaceholderPointer(uint32_t low32) {
    AssetSegmentLookup scratch = {};
    return LookupAssetSegment(low32, scratch);
}

uintptr_t EnsureAssetSegmentImage(const AssetSegmentLookup& lookup) {
    for (LoadedAssetSegment& loaded : gLoadedAssetSegments) {
        if ((loaded.segment == lookup.segment) &&
            (loaded.romBase == lookup.romBase) &&
            (loaded.compressed == lookup.compressed) &&
            !loaded.bytes.empty()) {
            /* Claim the segment slot only when it is unowned, mirroring the
               fresh-load path below. Reassigning on every cache hit lets any
               stray pointer that matches another venue's texture symbol hijack
               segment 0x0A mid-race — the road then renders with a different
               venue's texture. gdx_load_venue_texture_segment remains the
               authority for slot 0x0A and assigns it unconditionally. */
            if (gSegments[lookup.segment] == 0) {
                gSegments[lookup.segment] = reinterpret_cast<uintptr_t>(loaded.bytes.data());
            }
            return reinterpret_cast<uintptr_t>(loaded.bytes.data());
        }
    }

    if ((gdx_rom_buffer == nullptr) || (lookup.romBase >= gdx_rom_size)) {
        return 0;
    }

    LoadedAssetSegment loaded = {};
    loaded.segment = lookup.segment;
    loaded.romBase = lookup.romBase;
    loaded.compressed = lookup.compressed;

    if (lookup.compressed) {
        if (gdx_rom_size < static_cast<size_t>(lookup.romBase) + MIO0_HEADER_LENGTH ||
            std::memcmp(gdx_rom_buffer + lookup.romBase, "MIO0", 4) != 0) {
            return 0;
        }

        const uint32_t decodedSize = ReadBE32(gdx_rom_buffer + lookup.romBase + 4);
        const size_t outputSize = std::max<size_t>(decodedSize, lookup.imageSize);
        if (outputSize == 0) {
            return 0;
        }

        loaded.bytes.resize(outputSize);
        std::memset(loaded.bytes.data(), 0, loaded.bytes.size());
        const int decoded = mio0_decode(gdx_rom_buffer + lookup.romBase, loaded.bytes.data(), nullptr);
        if (decoded <= 0) {
            return 0;
        }
    } else if (gdx_rom_size >= static_cast<size_t>(lookup.romBase) + MIO0_HEADER_LENGTH &&
               std::memcmp(gdx_rom_buffer + lookup.romBase, "MIO0", 4) == 0) {
        gdx_port_logf("[segload] MIO0-autodetect seg=%u romBase=%08X (binding said uncompressed)\n",
                      lookup.segment, lookup.romBase);
        // Some segments (notably the per-venue texture segments, e.g. Mute City's
        // D_A000000_235130) are MIO0-compressed in ROM but the asset bindings mark
        // them uncompressed. Copying the raw MIO0 bytes as texture data renders the
        // compressed stream directly — that is the "track stripes". Detect the MIO0
        // magic here and decompress regardless of the (wrong) compressed flag.
        // Keep loaded.compressed = lookup.compressed so the segment cache key still
        // matches future lookups (the cached bytes are already decompressed).
        const uint32_t decodedSize = ReadBE32(gdx_rom_buffer + lookup.romBase + 4);
        const size_t outputSize = std::max<size_t>(decodedSize, lookup.imageSize);
        if (outputSize == 0) {
            return 0;
        }
        loaded.bytes.resize(outputSize);
        std::memset(loaded.bytes.data(), 0, loaded.bytes.size());
        const int decoded = mio0_decode(gdx_rom_buffer + lookup.romBase, loaded.bytes.data(), nullptr);
        if (decoded <= 0) {
            return 0;
        }
    } else {
        const size_t available = gdx_rom_size - lookup.romBase;
        const size_t allocSize = lookup.imageSize != 0
            ? std::min<size_t>(lookup.imageSize, available)
            : std::min<size_t>(available, 8 * 1024 * 1024);
        if (allocSize == 0) {
            return 0;
        }

        loaded.bytes.resize(allocSize);
        std::memset(loaded.bytes.data(), 0, loaded.bytes.size());
        std::memcpy(loaded.bytes.data(), gdx_rom_buffer + lookup.romBase, allocSize);
    }

    if (!loaded.bytes.empty()) {
        gdx_fixup_asset_segment_image(lookup.segment,
                                      lookup.romBase,
                                      loaded.bytes.data(),
                                      static_cast<unsigned int>(std::min<size_t>(loaded.bytes.size(), UINT32_MAX)));
        gdx_register_asset_segment_command_ranges(
            lookup.segment,
            lookup.romBase,
            loaded.bytes.data(),
            static_cast<unsigned int>(std::min<size_t>(loaded.bytes.size(), UINT32_MAX)));
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(loaded.bytes.data());
    if (gSegments[lookup.segment] == 0) {
        gSegments[lookup.segment] = base;
    }
    gHostRanges.push_back({ base, loaded.bytes.size() });
    gRawN64Ranges.push_back({ base, loaded.bytes.size() });
    gLoadedAssetSegments.emplace_back(std::move(loaded));
    // Phase G2 invalidation chokepoint: a freshly decoded asset image may rebind
    // a segment or change the pointer targets converted lists resolved against.
    // Bump the epoch so any cached wide conversion in the asset/ROM stamp space
    // is rebuilt on next draw-time encounter.
    ++gConvertEpoch;
    return base;
}

uintptr_t EnsureAssetSegmentForSymbol(uint32_t symbolLow32, uint32_t* outOffset = nullptr) {
    AssetSegmentLookup lookup = {};
    if (!LookupAssetSegment(symbolLow32, lookup)) {
        return 0;
    }

    const uintptr_t base = EnsureAssetSegmentImage(lookup);
    if (base == 0) {
        return 0;
    }

    if (outOffset != nullptr) {
        *outOffset = lookup.offset;
    }
    return base;
}

uintptr_t FallbackDataPointer(uint8_t op, uint32_t raw = 0) {
    // Guessing branch (resolution-of-last-resort): substitutes a static
    // identity matrix / default viewport / zeroed vertex buffer when nothing
    // resolved at all. Quarantined behind GDX_LEGACY_RESOLVE like every other
    // guess in TryResolveAddress -- see the quarantine block above
    // kFallbackIdentityMtx for the post-soak deletion checklist.
    if (!LegacyResolveEnabled()) {
        return 0;
    }
    uintptr_t result = 0;
    switch (op) {
        case kOpMtx:
            result = reinterpret_cast<uintptr_t>(kFallbackIdentityMtx);
            break;
        case kOpMovemem:
            result = reinterpret_cast<uintptr_t>(kFallbackViewport);
            break;
        case kOpVtx:
            result = reinterpret_cast<uintptr_t>(kFallbackVertices);
            break;
        default:
            return 0;
    }
    RecordLegacyResolveHit(LegacyResolveBranch::kFallbackBuffer, raw, op);
    return result;
}

bool IsReadableAddress(uintptr_t address);

uintptr_t NormalizeLusDirectPointer(uintptr_t pointer) {
    /* libultraship's SegAddr() treats bit 0 as a segmented-address sentinel.
       Host allocations can legitimately have odd low32 values after N64 pointer
       reconstruction, but the actual data these commands read is aligned. Clear
       the sentinel bit when the aligned address is still readable. */
    if ((pointer & 1u) == 0) {
        return pointer;
    }

    const uintptr_t aligned = pointer & ~static_cast<uintptr_t>(1);
    return IsReadableAddress(aligned) ? aligned : pointer;
}

size_t RegisteredHostRemaining(uintptr_t full_addr) {
    for (const HostRange& range : gHostRanges) {
        if ((range.begin == 0) || (range.size == 0)) {
            continue;
        }
        if ((full_addr >= range.begin) && (full_addr < range.begin + range.size)) {
            return range.size - static_cast<size_t>(full_addr - range.begin);
        }
    }
    return 0;
}

bool IsRdramHostPointer(uintptr_t full_addr) {
    if (gdx_rdram == nullptr) {
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(gdx_rdram);
    return (full_addr >= base) && (full_addr < base + GDX_RDRAM_SIZE);
}

/* True when a pointer's low32 falls on a PORT BSS ALIAS object: a dummy host
 * definition that stands in for N64 storage whose LIVE bytes are elsewhere.
 * These are the two known aliases:
 *  - D_1000000 (GfxPool dummy defined in decomp_port.c): game code WRITES
 *    matrices through gGfxPool-> (the live, double-buffered pool the segment-1
 *    table points at) but emits DL references as &D_1000000.member (racer.c
 *    gSPMatrix modelviews, camera.c:3415 the PROJECTION matrix). Pre-wide,
 *    the truncated low32 resolved through TryResolveAddress's D_1000000
 *    host-range special case to gSegments[1]+offset. A wide packet carries the
 *    dummy's REAL host address, so the w1IsHostPointer fast path read the
 *    never-written dummy instead: zero projection/modelview matrices ==> every
 *    3D triangle collapses while the matrix-free 2D texrect pipeline survives
 *    (the "2D works / 3D fully dead" blackout, graphics wave W1/W2).
 *  - D_2000000 (segment-2 BSS base, 1-byte LinkStubs token): live storage is
 *    D_80225800 via ResolvePortBssAlias. Taken verbatim it is also misaligned
 *    ((low32 & 7) != 0), so kOpMtx zeroed it and every frame fell back to the
 *    identity matrix ([datafail] op=DA raw=AAA694AC, W2).
 * Route these back through the low32 resolver exactly like the asset
 * placeholders in IsAssetPlaceholderPointer (campaign soak fix 3). */
bool IsPortBssAliasPointer(uint32_t low32) {
    if (low32 == Low32(reinterpret_cast<uintptr_t>(D_2000000))) {
        return true;
    }
    const uintptr_t d1Base = reinterpret_cast<uintptr_t>(D_1000000);
    const uint32_t d1Low = Low32(d1Base);
    const HostRange* r = gHostRanges.data();
    const size_t n = gHostRanges.size();
    for (size_t i = 0; i < n; i++) {
        if (r[i].begin == d1Base) {
            return (low32 >= d1Low) && (static_cast<size_t>(low32 - d1Low) < r[i].size);
        }
    }
    return false;
}

/* These containment scans run for nearly every translated command/pointer,
   over hundreds of registered ranges. Iterate via data()/size() — MSVC Debug
   iterator checking on range-for made these loops a measurable per-frame
   cost once menus/gameplay started resolving EK asset pointers. */
static inline bool HostRangeListContains(const std::vector<HostRange>& list, uintptr_t full_addr) {
    const HostRange* r = list.data();
    const size_t n = list.size();
    for (size_t i = 0; i < n; i++) {
        if ((r[i].begin != 0) && (r[i].size != 0) &&
            (full_addr >= r[i].begin) && (full_addr < r[i].begin + r[i].size)) {
            return true;
        }
    }
    return false;
}

bool IsRawN64HostPointer(uintptr_t full_addr) {
    if (IsRdramHostPointer(full_addr)) {
        return true;
    }
    return HostRangeListContains(gRawN64Ranges, full_addr);
}

bool IsHostN64CommandPointer(uintptr_t full_addr) {
    return HostRangeListContains(gHostN64CommandRanges, full_addr);
}

bool IsF3DAssetPointer(uintptr_t full_addr) {
    for (const HostRange& range : gF3DAssetRanges) {
        if ((range.begin == 0) || (range.size == 0)) {
            continue;
        }
        if ((full_addr >= range.begin) && (full_addr < range.begin + range.size)) {
            return true;
        }
    }
    return false;
}

bool DisplayListUsesF3D(const N64Gfx* source, size_t limit, size_t stride, bool isBig) {
    if (source == nullptr || limit == 0) {
        return false;
    }

    const size_t scanLimit = std::min(limit, kDisplayListValidationCommandLimit);
    for (size_t i = 0; i < scanLimit; ++i) {
        const uint8_t op = Opcode(ReadCommand(source, i, stride, isBig).w0);
        if (op == 0xB8u) {
            return true; // F3D G_ENDDL
        }
        if (op == kOpEndDl) {
            return false; // F3DEX2 G_ENDDL
        }
    }
    return false;
}

// Forward-declared: defined later in this file, needed here for the
// requiredBytes readability check added to ResolveRegisteredHostPointer.
size_t ReadableByteLimit(uintptr_t address);

bool ResolveRegisteredHostPointer(uint32_t raw, ResolvedAddress& out, size_t requiredBytes = 1) {
    // GDX_LEGACY_RESOLVE quarantine (see the block above kFallbackIdentityMtx):
    // this is the "registered-host low32-window match" guess G2 explicitly does
    // NOT reproduce (G2ResolvePhysical only does the deterministic RDRAM-arena
    // subset). Disabling the flag makes this branch behave as if it never
    // matched, same as if it were deleted.
    if (!LegacyResolveEnabled()) {
        return false;
    }
    for (const HostRange& range : gHostRanges) {
        if ((range.begin == 0) || (range.size == 0)) {
            continue;
        }

        const uint32_t baseLow = Low32(range.begin);
        const uint32_t offset = raw - baseLow;
        /* requiredBytes audit (2026-07-08): previously accepted any range whose
           low32 window merely CONTAINED the start byte, with no check that
           requiredBytes fit before the range's end, and no page-level
           readability confirmation -- and it committed to the FIRST matching
           range in registration order even when that match was coincidental.
           gHostRanges can have low32 windows that overlap by chance for an
           unrelated raw value, so validate both the declared size and the real
           mapped pages, and keep scanning on failure instead of trusting the
           first hit. This is one of the "wrong-but-readable" resolution gaps
           suspected as the remaining cause of vertex/matrix garbage loads. */
        if (offset < range.size && requiredBytes <= range.size - offset) {
            const uintptr_t full = range.begin + offset;
            if (ReadableByteLimit(full) >= requiredBytes) {
                out.full = full;
                out.segmented = false;
                RecordLegacyResolveHit(LegacyResolveBranch::kRegisteredHostLow32, raw, gLegacyResolveCurrentOp);
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Phase G2: binary N64 (8-byte) -> wide 16-byte boundary converter + cache.
//
// A narrow N64-format list (EK disk asset, ROM blob, or RDRAM-decoded segment)
// is converted ONCE to the wide layout the Phase G1 fast path consumes and
// cached across frames. The per-frame path then reads a wide source (stride 16,
// resolver-free) instead of re-parsing the narrow list and guessing pointers.
//
// Wiring is LAZY: the redirect lives in EnqueueList, so every narrow list the
// draw-time walk reaches -- root or sub-DL -- is converted+cached on first
// encounter, and sub-DL recursion falls out of the existing walk (each sub-DL is
// itself an EnqueueList that hits the same hook). See n64_gfx_convert.{h,cpp}.
// ---------------------------------------------------------------------------
gdx::GfxWideCache gWideCache;
// (gConvertEpoch is defined earlier, near the framebuffer globals.)
// Runtime kill switch (GDX_G2_CONVERT=0 disables the converter and restores the
// pure narrow path). Default ON. Lets the next runtime-verification session flip
// the boundary off instantly without a rebuild if a regression appears.
bool gG2ConvertEnabled = true;
bool gG2ConvertInit = false;
// Dialect (F3D vs F3DEX2) of each converted wide buffer, keyed by its exact data
// pointer. ProcessList consults this instead of re-deriving the dialect from the
// wide stream: the converted buffer loses its source segment's dialect tag, and
// the opcode-scan fallback can misclassify F3DEX2 setup DLs whose bytes contain
// 0xB8 before 0xDF (the exact case the segment-tag table guards -- see comment
// above GdxSegmentDialect). Recorded before enqueue, so the value for a live
// buffer is always current at read time.
std::unordered_map<const void*, bool> gConvertedWideIsF3d;

bool G2ResolvePhysical(void* /*user*/, uint32_t raw, size_t required_bytes, uintptr_t* out_host) {
    // Deterministic RDRAM-arena resolution ONLY -- the exact non-guessing subset
    // of TryResolveAddress (KSEG0/KSEG1 physical + bare physical offset). It never
    // does the registered-host low32 window match or the high-32 reconstruction;
    // those are the guesses G2 removes, not reproduces. A physical RDRAM address
    // is correct by construction (the literal address the game stored), so a
    // 1-byte readability gate suffices -- it cannot be "wrong-but-readable".
    if (gdx_rdram == nullptr) {
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(gdx_rdram);
    if (raw >= 0x80000000u && raw <= 0xBFFFFFFFu) {
        const uint32_t phys = raw & 0x1FFFFFFFu;
        if (phys < static_cast<uint32_t>(GDX_RDRAM_SIZE)) {
            const uintptr_t full = base + phys;
            if (ReadableByteLimit(full) >= required_bytes) {
                *out_host = full;
                return true;
            }
        }
        return false;
    }
    if ((raw >= static_cast<uint32_t>(GDX_RDRAM_GFXPOOL_OFFSET)) &&
        (raw < static_cast<uint32_t>(GDX_RDRAM_SIZE))) {
        const uintptr_t full = base + raw;
        if (ReadableByteLimit(full) >= required_bytes) {
            *out_host = full;
            return true;
        }
    }
    return false;
}

void EnsureG2ConvertInit() {
    if (gG2ConvertInit) {
        return;
    }
    gG2ConvertInit = true;
    const char* env = std::getenv("GDX_G2_CONVERT");
    gG2ConvertEnabled = (env == nullptr) || (env[0] != '0');
    gdx::ConvertContext ctx;
    ctx.resolve_physical = &G2ResolvePhysical;
    ctx.user = nullptr;
    gWideCache.SetContext(ctx);
}

uint64_t G2StampFor(const N64Gfx* src) {
    // Two disjoint stamp namespaces so an RDRAM DMA never collides with an asset
    // epoch. RDRAM-backed lists are mutable (course loads overwrite the arena):
    // key them on the DMA generation so any write rebuilds them. Asset/ROM lists
    // are immutable once decoded (a reload takes a fresh heap address = new key):
    // key them on the coarse asset epoch, high bit set to separate the spaces.
    if (IsRdramHostPointer(reinterpret_cast<uintptr_t>(src))) {
        return gDmaGeneration;
    }
    return 0x8000000000000000ull | static_cast<uint64_t>(gConvertEpoch);
}

bool ResolveGeneratedAssetStub(uint32_t raw, ResolvedAddress& out) {
    AssetSegmentLookup lookup = {};
    if (!LookupAssetSegment(raw, lookup)) {
        return false;
    }

    /* Live-carve preference, segment 0x0A ONLY (2026-07-10, early-race floor
     * root cause): interior venue-bank pointers low32-match whichever venue's
     * suffixed stub range happens to contain them (the stubs alias in low32),
     * so the per-symbol heap image can be a DIFFERENT venue's texture bank --
     * the frames 1-43 wrong-venue floor. The slot-0x0A carve is rotated by
     * gdx_load_venue_texture_segment (the authority) and every venue shares
     * the same bank*0x1000 layout, so resolving live is venue-correct by
     * construction. Deliberately NOT generalized to other segments: routing
     * seg-4/7 placeholder textures to the rdram carve strips their o2r
     * delivery eligibility (the SETTIMG path's !IsRdramHostPointer gate) and
     * puts them on the raw-copy path with per-frame dma-generation staleness
     * refreshes -- the 2026-07-10 regression run (whole HUD + vehicles
     * garbled, unplayable FPS). The rank digits gain nothing from the live
     * carve either: [digit-carve] proved gSegments[4]+0x13DE0 is zero at race
     * time (the console's runtime fill has no port equivalent yet). */
    if (lookup.segment == 0x0Au) {
        const uintptr_t live = gSegments[0x0A];
        if (live != 0 && ReadableByteLimit(live + lookup.offset) >= 1) {
            out.full = live + lookup.offset;
            out.segment = lookup.segment;
            out.offset = lookup.offset;
            out.segmented = true;
            return true;
        }
    }

    const uintptr_t base = EnsureAssetSegmentImage(lookup);
    if (base == 0) {
        return false;
    }
    out.full = base + lookup.offset;
    out.segment = lookup.segment;
    out.offset = lookup.offset;
    out.segmented = true;
    return true;
}

bool ResolvePortBssAlias(uint32_t raw, ResolvedAddress& out) {
    /*
     * D_2000000 is the original segment-2 BSS base. LinkStubs can only provide
     * a one-byte symbol token for it, while the active host storage begins at
     * D_80225800. Host-built display lists carry the token directly, so they
     * bypass normal segmented-address resolution and need the same alias here.
     * Do not use D_80225800_2: that duplicate overlap object is never initialized
     * by Game_ThreadEntry, so it contains a zero modelview matrix.
     */
    if (raw != Low32(reinterpret_cast<uintptr_t>(D_2000000))) {
        return false;
    }

    out.full = reinterpret_cast<uintptr_t>(D_80225800);
    out.segment = 2;
    out.offset = 0;
    out.segmented = true;
    return true;
}

/* course.c's road material table (ROAD_1..WALLED_ROAD) stores the unsuffixed
 * venue bank symbols D_A000000..D_A008000 directly, so road-pass SETTIMGs carry
 * those stubs' truncated addresses. The stubs are 1-byte placeholders with no
 * binding; without this alias they false-match zero-filled memory and the road
 * samples an all-black texture. The real data is the per-venue segment image
 * gdx_load_venue_texture_segment puts in gSegments[0x0A]; bank N sits at
 * N * 0x1000 (the suffixed per-venue symbols confirm the stride). Exact-base
 * match only: the stubs are packed 1 byte apart, so interior spans would
 * collide with the next symbol. */
/* Phase G1 regression fix (2026-07-09): game-BUILT wide DLs carry the REAL
   host addresses of generated 1-byte asset stubs (port/gen/LinkStubs.c)
   whenever a compile-time table stores asset symbols — course.c's road/wall
   material table (course.c:101-112) stores the venue banks D_A000000..
   D_A008000 directly. The wide host-pointer fast path took those addresses
   verbatim (the EXE module is a registered host range), so the track floor,
   walls and tunnel sampled EXE data-section bytes: black where the stub
   neighborhood is zeroed, striped garbage otherwise, changing per BUILD with
   the module layout. The low32 resolver already knows these identities
   (ResolveVenueBankAlias / ResolveGeneratedAssetStub) but wide pointers never
   reach it. Exact full-address membership inside the module range only — a
   genuine data pointer cannot false-match because the stub's address IS the
   full address being compared. Returns 0 when `full` is not a known stub. */
bool ResolveVenueBankAlias(uint32_t raw, ResolvedAddress& out); // fwd decl (defined below)
uintptr_t ResolveWideAssetStubPointer(uintptr_t full, uintptr_t moduleBegin, uintptr_t moduleEnd) {
    if (full == 0 || moduleBegin == 0 || full < moduleBegin || full >= moduleEnd) {
        return 0;
    }
    const uint32_t low = Low32(full);
    ResolvedAddress out = {};
    if (ResolveVenueBankAlias(low, out)) {
        return out.full;
    }
    if (ResolveGeneratedAssetStub(low, out)) {
        return out.full;
    }
    return 0;
}

bool ResolveVenueBankAlias(uint32_t raw, ResolvedAddress& out) {
    if (gSegments[0x0A] == 0) {
        return false;
    }
    static const uint32_t kBankLow32[] = {
        Low32(reinterpret_cast<uintptr_t>(D_A000000)), Low32(reinterpret_cast<uintptr_t>(D_A001000)),
        Low32(reinterpret_cast<uintptr_t>(D_A002000)), Low32(reinterpret_cast<uintptr_t>(D_A003000)),
        Low32(reinterpret_cast<uintptr_t>(D_A004000)), Low32(reinterpret_cast<uintptr_t>(D_A005000)),
        Low32(reinterpret_cast<uintptr_t>(D_A006000)), Low32(reinterpret_cast<uintptr_t>(D_A007000)),
        Low32(reinterpret_cast<uintptr_t>(D_A008000)),
    };
    for (uint32_t bank = 0; bank < static_cast<uint32_t>(std::size(kBankLow32)); bank++) {
        if (raw == kBankLow32[bank]) {
            out.full = gSegments[0x0A] + bank * 0x1000u;
            out.segment = 0x0A;
            out.offset = bank * 0x1000u;
            out.segmented = true;
            return true;
        }
    }
    return false;
}

uintptr_t EnsureSetupGfxSegment() {
    uint32_t offset = 0;
    const uintptr_t base = EnsureAssetSegmentForSymbol(Low32(reinterpret_cast<uintptr_t>(D_3000000)), &offset);
    if (base != 0) {
        return base;
    }

    if (!gSetupGfxSegment.empty()) {
        return reinterpret_cast<uintptr_t>(gSetupGfxSegment.data());
    }

    if ((gdx_rom_buffer == nullptr) || (gdx_rom_size < kSetupGfxRomOffset + kSetupGfxSize)) {
        return 0;
    }

    gSetupGfxSegment.resize(kSetupGfxSize);
    for (size_t i = 0; i < kSetupGfxSize; i += sizeof(uint32_t)) {
        const uint8_t* src = gdx_rom_buffer + kSetupGfxRomOffset + i;
        const uint32_t word = ReadBE32(src);
        std::memcpy(gSetupGfxSegment.data() + i, &word, sizeof(word));
    }

    {
        const uintptr_t segBase = reinterpret_cast<uintptr_t>(gSetupGfxSegment.data());
        // Register so RegisteredHostRemaining() treats this as ROM-backed, and so
        // G_MOVEWORD can't overwrite gSegments[3] with a garbage arena-buffer address.
        gHostRanges.push_back({ segBase, gSetupGfxSegment.size() });
        gHostN64CommandRanges.push_back({ segBase, gSetupGfxSegment.size() });
        if (gSegments[3] == 0) {
            gSegments[3] = segBase;
        }
        return segBase;
    }
}

uintptr_t MakeFramebufferToken(uint32_t raw) {
#if UINTPTR_MAX > UINT32_MAX
    constexpr uintptr_t kFramebufferTokenBase = 0x0000000300000000ull;
#else
    constexpr uintptr_t kFramebufferTokenBase = 0x30000000u;
#endif
    // CIMG and ZIMG commands that reference the same N64 address must retain
    // pointer identity. Fast3D uses that identity to distinguish a depth clear
    // from a visible color fill.
    return kFramebufferTokenBase | (static_cast<uintptr_t>(raw) & 0xFFFFFFFEu);
}

bool ResolveSetupGfxStub(uint32_t raw, ResolvedAddress& out) {
    struct SetupSymbol {
        const uint8_t* symbol;
        uint32_t offset;
    };

    static const SetupSymbol kSetupSymbols[] = {
        { D_3000000, 0x000 }, { D_3000028, 0x028 }, { D_3000050, 0x050 }, { D_3000088, 0x088 },
        { D_30000C0, 0x0C0 }, { D_3000100, 0x100 }, { D_3000138, 0x138 }, { D_3000170, 0x170 },
        { D_30001A8, 0x1A8 }, { D_3000270, 0x270 }, { D_30002E0, 0x2E0 }, { D_3000338, 0x338 },
        { D_3000400, 0x400 }, { D_3000438, 0x438 }, { D_3000470, 0x470 }, { D_30004A8, 0x4A8 },
        { D_30004E0, 0x4E0 }, { D_3000510, 0x510 }, { D_3000540, 0x540 }, { D_3000590, 0x590 },
        { D_30005D8, 0x5D8 }, { D_3000688, 0x688 }, { D_30006D0, 0x6D0 },
    };

    for (const SetupSymbol& entry : kSetupSymbols) {
        if (raw == Low32(reinterpret_cast<uintptr_t>(entry.symbol))) {
            const uintptr_t base = EnsureSetupGfxSegment();
            if (base == 0) {
                return false;
            }
            out.full = base + entry.offset;
            out.segment = 3;
            out.offset = entry.offset;
            out.segmented = true;
            return true;
        }
    }
    return false;
}

void GetMainModuleRange(uintptr_t& moduleBegin, uintptr_t& moduleEnd) {
    moduleBegin = 0;
    moduleEnd = 0;

#ifdef _WIN32
    HMODULE module = GetModuleHandleA(nullptr);
    if (module == nullptr) {
        return;
    }

    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }

    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const uint8_t*>(module) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }

    moduleBegin = reinterpret_cast<uintptr_t>(module);
    moduleEnd = moduleBegin + ntHeaders->OptionalHeader.SizeOfImage;
#endif
}

bool IsReadablePageProtect(uint32_t protect) {
#ifdef _WIN32
    if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0) {
        return false;
    }

    protect &= 0xFF;
    return protect == PAGE_READONLY || protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
           protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
#else
    (void)protect;
    return true;
#endif
}

size_t ReadableCommandLimit(const void* source, size_t stride = kN64GfxStride) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(source, &mbi, sizeof(mbi)) == 0) {
        return 0;
    }

    if (mbi.State != MEM_COMMIT || !IsReadablePageProtect(mbi.Protect)) {
        return 0;
    }

    const uintptr_t begin = reinterpret_cast<uintptr_t>(source);
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (begin >= regionEnd) {
        return 0;
    }

    return static_cast<size_t>((regionEnd - begin) / stride);
#else
    (void)source;
    return 0;
#endif
}

size_t ReadableByteLimit(uintptr_t address) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) == 0) {
        return 0;
    }

    if (mbi.State != MEM_COMMIT || !IsReadablePageProtect(mbi.Protect)) {
        return 0;
    }

    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (address >= regionEnd) {
        return 0;
    }

    return static_cast<size_t>(regionEnd - address);
#else
    (void)address;
    return 0;
#endif
}

bool IsReadableAddress(uintptr_t address) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) == 0) {
        return false;
    }

    return mbi.State == MEM_COMMIT && IsReadablePageProtect(mbi.Protect);
#else
    (void)address;
    return false;
#endif
}

uintptr_t MakePersistentVtxCopy(uintptr_t source, size_t count) {
    if (source == 0 || count == 0) {
        return 0;
    }
    size_t requiredBytes = count * 16;

    /* Defense in depth for the machine vertex-spike bug (#3): callers are now
       expected to have validated `requiredBytes` bytes are readable at `source`
       via TranslateDataPointer(..., requiredBytes) before reaching here, but an
       under-validated resolution (e.g. only 1 byte proven readable) previously
       let this loop walk past the end of the real vertex buffer into unrelated
       host memory -- one garbage vertex is a visible "spike" (stretched
       polygon). Clamp defensively and zero the tail so a residual gap can never
       turn into an out-of-bounds host read again, and log it so the true
       source of any remaining spikes is still visible. */
    const size_t readable = ReadableByteLimit(source);
    size_t safeCount = count;
    if (readable < requiredBytes) {
        safeCount = readable / 16;
        static int sVtxClampLogs = 0;
        if (sVtxClampLogs < 40) {
            ++sVtxClampLogs;
            gdx_port_logf("[vtx-clamp] source=%p requested=%zu(%zuB) readable=%zuB clampedCount=%zu\n",
                          reinterpret_cast<void*>(source), count, requiredBytes, readable, safeCount);
        }
    }

    auto alloc = std::make_unique<uint8_t[]>(requiredBytes);
    uint8_t* out = alloc.get();
    std::memset(out, 0, requiredBytes);
    gPersistentAllocations.push_back(std::move(alloc));

    const uint8_t* in = reinterpret_cast<const uint8_t*>(source);
    for (size_t i = 0; i < safeCount; i++) {
        uint16_t* out_s = reinterpret_cast<uint16_t*>(out + i * 16);
        const uint16_t* in_s = reinterpret_cast<const uint16_t*>(in + i * 16);
        out_s[0] = Byteswap16(in_s[0]);
        out_s[1] = Byteswap16(in_s[1]);
        out_s[2] = Byteswap16(in_s[2]);
        out_s[3] = Byteswap16(in_s[3]);
        out_s[4] = Byteswap16(in_s[4]);
        out_s[5] = Byteswap16(in_s[5]);
        out[i * 16 + 12] = in[i * 16 + 12];
        out[i * 16 + 13] = in[i * 16 + 13];
        out[i * 16 + 14] = in[i * 16 + 14];
        out[i * 16 + 15] = in[i * 16 + 15];
    }
    return reinterpret_cast<uintptr_t>(out);
}

/* Big-endian static Mtx data (asset/heap sources) word-swapped for the
   interpreter, which reads matrices as host-order u32 words. Host-built
   matrices (Matrix_ToMtx's j^1 layout) must NOT pass through here. */
uintptr_t MakePersistentMtxCopy(uintptr_t source) {
    if (source == 0) {
        return 0;
    }
    auto alloc = std::make_unique<uint8_t[]>(64);
    uint8_t* out = alloc.get();
    gPersistentAllocations.push_back(std::move(alloc));

    const uint32_t* in_w = reinterpret_cast<const uint32_t*>(source);
    uint32_t* out_w = reinterpret_cast<uint32_t*>(out);
    for (size_t i = 0; i < 16; i++) {
        out_w[i] = Byteswap32(in_w[i]);
    }
    return reinterpret_cast<uintptr_t>(out);
}

uintptr_t MakePersistentRawTextureCopy(uintptr_t source, size_t requiredBytes, bool* outRefreshed) {
    if (outRefreshed != nullptr) {
        *outRefreshed = false;
    }
    if ((source == 0) || (requiredBytes == 0)) {
        return 0;
    }

    size_t readable = ReadableByteLimit(source);
    if (readable == 0) {
        readable = RegisteredHostRemaining(source);
    }
    if (readable == 0) {
        return 0;
    }

    const size_t copyBytes = std::min(requiredBytes, readable);

    for (PersistentRawTextureCopy& copy : gRawTextureCopies) {
        if (copy.source != source) {
            continue;
        }

        const bool needsResize = (copy.bytes == nullptr) || (copy.size < requiredBytes);
        bool changed = needsResize;
        if (!needsResize) {
            if (IsRdramHostPointer(source) || IsN64FramebufferRange(source, copy.size)) {
                changed = HostRangeChanged(source, copy.size, copy.dmaGenAtCopy);
            } else {
                // ROM-backed textures are stable after the segment is loaded; skip memcmp.
                const bool stableSource = RegisteredHostRemaining(source) > 0;
                if (!stableSource) {
                    changed = (std::memcmp(copy.bytes.get(), reinterpret_cast<const void*>(source), copyBytes) != 0);
                }
            }
        }

        if (changed) {
            if (outRefreshed != nullptr) {
                *outRefreshed = true;
            }
            if (!needsResize) {
                gPendingTextureCacheDeletes.push_back(reinterpret_cast<uintptr_t>(copy.bytes.get()));
                std::memset(copy.bytes.get(), 0, copy.size);
                CopyRawTextureBytes(copy.bytes.get(), source, copyBytes);
            } else {
                if (copy.bytes != nullptr) {
                    gPendingTextureCacheDeletes.push_back(reinterpret_cast<uintptr_t>(copy.bytes.get()));
                    gPersistentAllocations.push_back(std::move(copy.bytes));
                }
                auto refreshed = std::make_unique<uint8_t[]>(requiredBytes);
                std::memset(refreshed.get(), 0, requiredBytes);
                CopyRawTextureBytes(refreshed.get(), source, copyBytes);
                copy.bytes = std::move(refreshed);
                copy.size = requiredBytes;
            }
            copy.dmaGenAtCopy = gDmaGeneration;
        }
        return reinterpret_cast<uintptr_t>(copy.bytes.get());
    }

    PersistentRawTextureCopy copy = {};
    copy.source = source;
    copy.size = requiredBytes;
    copy.bytes = std::make_unique<uint8_t[]>(requiredBytes);
    std::memset(copy.bytes.get(), 0, requiredBytes);
    CopyRawTextureBytes(copy.bytes.get(), source, copyBytes);
    copy.dmaGenAtCopy = gDmaGeneration;

    const uintptr_t out = reinterpret_cast<uintptr_t>(copy.bytes.get());
    gRawTextureCopies.emplace_back(std::move(copy));
    if (outRefreshed != nullptr) {
        *outRefreshed = true;
    }
    return out;
}

class N64DisplayListAdapter {
  public:
    struct ConvertedList {
        std::vector<Fast::F3DGfx> commands;
    };

    struct QueueItem {
        const N64Gfx* source;
        size_t limit;
        ConvertedList* listPtr;
    };

    N64DisplayListAdapter(const void* root, size_t rootSizeBytes, bool isBig, ConversionStats* stats = nullptr)
        : mRootBegin(static_cast<const N64Gfx*>(root)),
          mRootByteEnd(reinterpret_cast<uintptr_t>(root) + rootSizeBytes),
          mIsBig(isBig),
          mStats(stats) {
        GetMainModuleRange(mModuleBegin, mModuleEnd);
    }

    uintptr_t EnqueueList(const N64Gfx* source, size_t explicitLimit) {
        // Phase G2 lazy boundary: if `source` is a narrow N64-format list, convert
        // it to the wide layout once (cached) and enqueue the wide buffer instead,
        // so ProcessList takes the resolver-free fast path. No-op for sources that
        // are already wide (game-emitted or previously converted).
        source = GetOrBuildConvertedWide(source, explicitLimit);

        auto cached = mLists.find(source);
        if (cached != mLists.end()) return reinterpret_cast<uintptr_t>(cached->second->commands.data());

        auto list = std::make_unique<ConvertedList>();
        ConvertedList* listPtr = list.get();
        mLists.emplace(source, std::move(list));
        if (mStats != nullptr) mStats->convertedLists++;

        const size_t limit = EffectiveLimit(source, explicitLimit);
        listPtr->commands.reserve(limit + 1);

        mWorkQueue.push_back({source, limit, listPtr});

        return reinterpret_cast<uintptr_t>(listPtr->commands.data());
    }

  private:

    const N64Gfx* mRootBegin = nullptr;
    uintptr_t mRootByteEnd = 0;
    uintptr_t mModuleBegin = 0;
    uintptr_t mModuleEnd = 0;
    bool mIsBig = false;
    ConversionStats* mStats = nullptr;
    std::unordered_map<const N64Gfx*, std::unique_ptr<ConvertedList>> mLists;
    std::vector<QueueItem> mWorkQueue;
    std::vector<Fast::F3DGfx> mNoopList{ MakeLusGfx(static_cast<uintptr_t>(kOpEndDl) << 24, 0) };

    size_t CommandStrideForSource(const N64Gfx* source) const {
        const uintptr_t ptr = reinterpret_cast<uintptr_t>(source);
        return (IsRawN64HostPointer(ptr) || IsHostN64CommandPointer(ptr)) ? kN64GfxStride : kHostBuiltGfxStride;
    }

    // Phase G2: return a wide 16-byte version of `source`, converting+caching on
    // first encounter. `ioLimit` is updated to the wide command count. Returns
    // `source` unchanged (and leaves ioLimit alone) when conversion is disabled,
    // the source is already wide, or its extent is unknown -- in which case the
    // caller falls back to the original narrow machinery.
    const N64Gfx* GetOrBuildConvertedWide(const N64Gfx* source, size_t& ioLimit) {
        EnsureG2ConvertInit();
        if (!gG2ConvertEnabled || source == nullptr) {
            return source;
        }
        const size_t stride = CommandStrideForSource(source);
        if (stride != kN64GfxStride) {
            return source;  // already wide (game-emitted, or a converted buffer)
        }

        // Bound the walk exactly as the narrow path would.
        size_t narrowLimit = (ioLimit != 0) ? ioLimit : KnownCommandLimit(source);
        const size_t knownLimit = KnownCommandLimit(source);
        if (knownLimit != 0) {
            narrowLimit = (narrowLimit != 0) ? std::min(narrowLimit, knownLimit) : knownLimit;
        }
        if (narrowLimit == 0) {
            return source;  // unknown extent -> leave to the narrow path
        }

        // Decide endianness and dialect the SAME way ProcessList would, so the
        // converted value path is byte-identical and the pointer classification
        // matches. The dialect decision is recorded so ProcessList reuses it
        // rather than re-deriving it (and possibly misclassifying) from the
        // dialect-tagless wide buffer.
        const bool isBig = CommandSourceIsBigEndian(source, stride);
        const GdxSegmentUcode dialect = GdxAssetPointerDialect(reinterpret_cast<uintptr_t>(source));
        const bool isF3d =
            (dialect == GdxSegmentUcode::F3D)      ? true
            : (dialect == GdxSegmentUcode::F3DEX2) ? false
            : (IsF3DAssetPointer(reinterpret_cast<uintptr_t>(source)) ||
               DisplayListUsesF3D(source, narrowLimit, stride, isBig));

        const std::vector<gdx::WideGfx>& wide =
            gWideCache.GetOrBuild(source, narrowLimit, isBig, isF3d, G2StampFor(source));
        if (wide.empty()) {
            return source;
        }

        const N64Gfx* wideSrc = reinterpret_cast<const N64Gfx*>(wide.data());
        /* [dl-census] (2026-07-10, graphics-audit phase 4): one line per UNIQUE
           narrow list converted by G2 across the whole session (menus included).
           Names which asset families' display lists take the binary route --
           the pause-menu/decoration suspects -- with an ASLR-stable identity
           where the source is module-resident. */
        {
            static uintptr_t sDlCensusSeen[48] = {};
            static int sDlCensusCount = 0;
            const uintptr_t srcAddr = reinterpret_cast<uintptr_t>(source);
            bool dlDup = false;
            for (int dc = 0; dc < sDlCensusCount; dc++) {
                if (sDlCensusSeen[dc] == srcAddr) {
                    dlDup = true;
                    break;
                }
            }
            if (!dlDup && sDlCensusCount < 48) {
                sDlCensusSeen[sDlCensusCount++] = srcAddr;
                uint64_t prefVA = 0;
                if (mModuleBegin != 0 && srcAddr >= mModuleBegin && srcAddr < mModuleEnd) {
                    prefVA = kPreferredImageBaseVA + (srcAddr - mModuleBegin);
                }
                const char* cls = IsRdramHostPointer(srcAddr) ? "rdram"
                                  : (prefVA != 0)             ? "module"
                                                              : "other";
                gdx_port_logf("[dl-census] narrow src=%p cls=%s prefVA=%011llX limit=%zu isBig=%d f3d=%d\n",
                              reinterpret_cast<const void*>(source), cls,
                              static_cast<unsigned long long>(prefVA), narrowLimit,
                              isBig ? 1 : 0, isF3d ? 1 : 0);
            }
        }
        // The dialect map is re-recorded here before any ProcessList read of this
        // exact buffer, so clearing it when it grows large is always safe (a stale
        // rebuilt buffer's old address is never read again). Bounds lifetime
        // growth from in-place rebuilds that relocate a cached list's storage.
        if (gConvertedWideIsF3d.size() > 8192) {
            gConvertedWideIsF3d.clear();
        }
        gConvertedWideIsF3d[wideSrc] = isF3d;
        ioLimit = wide.size();
        return wideSrc;
    }

    bool CommandSourceIsBigEndian(const N64Gfx* source, size_t stride) const {
        if ((source == nullptr) || (stride != kN64GfxStride)) {
            return false;
        }

        const uintptr_t ptr = reinterpret_cast<uintptr_t>(source);
        if (!IsRawN64HostPointer(ptr) || IsHostN64CommandPointer(ptr)) {
            return false;
        }

        return IsLikelyBigEndianDisplayList(source, ReadableCommandLimit(source, stride));
    }

    size_t RootCommandLimit(const N64Gfx* source) const {
        if ((source != nullptr) && (mRootBegin != nullptr)) {
            const uintptr_t ptr = reinterpret_cast<uintptr_t>(source);
            const uintptr_t begin = reinterpret_cast<uintptr_t>(mRootBegin);
            if ((ptr >= begin) && (ptr < mRootByteEnd)) {
                return static_cast<size_t>((mRootByteEnd - ptr) / CommandStrideForSource(source));
            }
        }
        return 0;
    }

    size_t KnownCommandLimit(const N64Gfx* source) const {
        if (source == nullptr) {
            return 0;
        }

        const size_t stride = CommandStrideForSource(source);
        const size_t readableLimit = ReadableCommandLimit(source, stride);
        if (readableLimit == 0) {
            return 0;
        }

        size_t limit = 0;
        const auto applyLimit = [&limit](size_t candidate) {
            if (candidate == 0) {
                return;
            }
            limit = (limit == 0) ? candidate : std::min(limit, candidate);
        };

        applyLimit(RootCommandLimit(source));

        const uintptr_t ptr = reinterpret_cast<uintptr_t>(source);
        const size_t registeredRemaining = RegisteredHostRemaining(ptr);
        if (registeredRemaining >= stride) {
            applyLimit(registeredRemaining / stride);
        }

        if ((mModuleBegin != 0) && (ptr >= mModuleBegin) && (ptr < mModuleEnd)) {
            applyLimit(static_cast<size_t>((mModuleEnd - ptr) / stride));
        }

        for (uint8_t segment = 0; segment < kGfxSegmentCount; segment++) {
            const uintptr_t base = gSegments[segment];
            if ((base != 0) && (ptr >= base) && (ptr < base + kSegmentOffsetLimit)) {
                applyLimit(static_cast<size_t>(((base + kSegmentOffsetLimit) - ptr) / stride));
            }
        }

        applyLimit(readableLimit);
        return limit;
    }

    bool TryResolveAddress(uint32_t raw, ResolvedAddress& out, size_t requiredBytes = 1, bool preferPhysical = false,
                           uintptr_t sourceHint = 0) const {
        if (raw == 0) {
            return false;
        }

        /*
         * Disk-resident EK overlays retain their original N64 virtual or
         * segmented pointers inside display lists. Their payloads live in
         * generated host arrays, so resolve those explicit token ranges before
         * the generic KSEG/segment heuristics. Reverse order lets the most
         * recently registered overlay win if original overlay VRAM ranges
         * overlap.
         */
        /* Raw-pointer reverse iteration: this runs per translated data pointer
           over ~600 registered EK ranges; MSVC Debug checked iterators made it
           a real per-frame cost (reverse order preserved: most recently
           registered overlay wins on overlap). */
        {
            const N64AddressRange* ranges = gN64AddressRanges.data();
            for (size_t ri = gN64AddressRanges.size(); ri > 0; ri--) {
                const N64AddressRange& r = ranges[ri - 1];
                if (raw < r.n64Begin) {
                    continue;
                }
                const size_t offset = static_cast<size_t>(raw - r.n64Begin);
                if (offset <= r.size && requiredBytes <= r.size - offset) {
                    out.full = r.hostBegin + offset;
                    out.segmented = (r.n64Begin >> 24) < kGfxSegmentCount;
                    if (out.segmented) {
                        out.segment = static_cast<uint8_t>(r.n64Begin >> 24);
                        out.offset = raw & 0x00FFFFFFu;
                    }
                    return true;
                }
            }
        }

        if (ResolvePortBssAlias(raw, out)) {
            return true;
        }

        if (ResolveVenueBankAlias(raw, out)) {
            return true;
        }

        if (ResolveGeneratedAssetStub(raw, out)) {
            return true;
        }

        if (ResolveSetupGfxStub(raw, out)) {
            return true;
        }

        const uint32_t d1000000_low = Low32(reinterpret_cast<uintptr_t>(D_1000000));
        for (const HostRange& range : gHostRanges) {
            if (range.begin == reinterpret_cast<uintptr_t>(D_1000000)) {
                /* requiredBytes audit (2026-07-08): this only proved the START of
                   the candidate offset was inside D_1000000's range, never that
                   requiredBytes actually fit before the end of the buffer -- the
                   same short-read hazard already fixed for the other candidate
                   paths in this function, but missed here. D_1000000 is the huge
                   static data blob that also backs per-player/machine structures
                   (e.g. &D_1000000.unk_21988[playerIndex], the G_MTX source
                   confirmed in the countdown-face investigation), so a matrix or
                   vertex load landing near the tail of this buffer could silently
                   walk past it into unrelated memory -- a plausible source of the
                   still-visible machine vertex spikes. */
                const size_t offset = static_cast<size_t>(raw - d1000000_low);
                if (raw >= d1000000_low && offset <= range.size && requiredBytes <= range.size - offset) {
                    out.full = static_cast<uintptr_t>(gSegments[1]) + offset;
                    out.segmented = false;
                    return true;
                }
                break;
            }
        }

        /* Explicit N64 segment addresses (top byte = segment index 1..15, e.g.
           the course/venue texture pointers 0x08xxxxxx / 0x0Axxxxxx emitted by
           F3D asset display lists) must resolve through the segment table, which
           now points at the decompressed venue image. Do this BEFORE the low-32
           host-range heuristic: that heuristic can false-match a segment address
           against an unrelated host allocation whose low 32 bits happen to cover
           it, which is what left the track textures reading raw/garbage bytes. */
        {
            const uint8_t seg = static_cast<uint8_t>(raw >> 24);
            const uint32_t segOffset = raw & 0x00FFFFFFu;
            if (seg >= 1 && seg < kGfxSegmentCount && gSegments[seg] != 0 &&
                segOffset < kSegmentOffsetLimit) {
                const uintptr_t full = gSegments[seg] + segOffset;
                /* A segment match is only authoritative when the result is
                   actually readable. K0_TO_PHYS truncates module data pointers
                   to 29 bits, so e.g. 0x7FF702142C10 & 0x1FFFFFFF = 0x02142C10
                   masquerades as a segment-2 offset far past the real segment-2
                   buffer. Accepting it blindly yields an unreadable pointer that
                   TranslateDataPointer nulls — the texture silently vanishes.
                   Fall through so the physical-window paths can reconstruct the
                   original module pointer instead. */
                if (ReadableByteLimit(full) >= requiredBytes) {
                    out.full = full;
                    out.segment = seg;
                    out.offset = segOffset;
                    out.segmented = true;
                    return true;
                }
            }
        }

        if (ResolveRegisteredHostPointer(raw, out, requiredBytes)) {
            return true;
        }

        /* KSEG0 (0x80000000–0x9FFFFFFF) and KSEG1 (0xA0000000–0xBFFFFFFF):
           strip segment bits to obtain the physical RDRAM offset.
           Ordered AFTER all asset-stub resolvers so ROM-backed stubs always win. */
        if (raw >= 0x80000000u && raw <= 0xBFFFFFFFu) {
            const uint32_t phys = raw & 0x1FFFFFFFu;
            if (phys < static_cast<uint32_t>(GDX_RDRAM_SIZE) && gdx_rdram != nullptr) {
                out.full = reinterpret_cast<uintptr_t>(gdx_rdram) + phys;
                out.segmented = false;
                return true;
            }
            /* Out-of-RDRAM KSEG0: NOT necessarily MMIO/cart — a truncated
               64-bit host pointer whose low32 happens to fall in 0x80-0x9F
               (e.g. 0x933AEF70 from heap 0x20A933AEF70) looks identical.
               Try reconstructing it against known high-32 windows before
               giving up; texture loads feed garbage into TMEM otherwise.
               GDX_LEGACY_RESOLVE quarantine: this "high-32 reconstruction" is
               a guess (see the quarantine block above kFallbackIdentityMtx) --
               the deterministic RDRAM strip above already covers the
               non-guessing case. */
            if (LegacyResolveEnabled()) {
                const uintptr_t highCandidates[] = {
                    reinterpret_cast<uintptr_t>(mRootBegin) & 0xFFFFFFFF00000000ULL,
                    mModuleBegin & 0xFFFFFFFF00000000ULL,
                };
                for (uintptr_t high : highCandidates) {
                    if (high == 0) {
                        continue;
                    }
                    const uintptr_t full = high | static_cast<uintptr_t>(raw);
                    if (ReadableByteLimit(full) >= requiredBytes) {
                        out.full = full;
                        out.segmented = false;
                        RecordLegacyResolveHit(LegacyResolveBranch::kKseg0High32, raw, gLegacyResolveCurrentOp);
                        return true;
                    }
                }
                for (const HostRange& range : gHostRanges) {
                    const uintptr_t high = range.begin & 0xFFFFFFFF00000000ULL;
                    if (high == 0) {
                        continue;
                    }
                    const uintptr_t full = high | static_cast<uintptr_t>(raw);
                    if (ReadableByteLimit(full) >= requiredBytes) {
                        out.full = full;
                        out.segmented = false;
                        RecordLegacyResolveHit(LegacyResolveBranch::kKseg0High32, raw, gLegacyResolveCurrentOp);
                        return true;
                    }
                }
            }
            return false; /* genuinely unresolvable KSEG0 (MMIO/cart range) */
        }

        /* Some PORT paths pass bare physical RDRAM offsets through display-list
           words after osVirtualToPhysical() truncates host pointers.  Mirror the
           decomp-side resolver here so sub-DL/data pointers like 0x0013C700 and
           0x0015EC00 do not become no-op display lists.  Keep a conservative
           lower bound at the graphics-pool/arena area so tiny immediates such as
           0x400 are not accidentally treated as real pointers. */
        if ((raw >= static_cast<uint32_t>(GDX_RDRAM_GFXPOOL_OFFSET)) &&
            (raw < static_cast<uint32_t>(GDX_RDRAM_SIZE)) &&
            (gdx_rdram != nullptr)) {
            out.full = reinterpret_cast<uintptr_t>(gdx_rdram) + raw;
            out.segmented = false;
            return true;
        }

        const uint8_t encodedSegment = static_cast<uint8_t>(raw >> 24);
        const uint32_t encodedOffset = raw & 0x00FFFFFE;

        /* K0_TO_PHYS()/OS_K0_TO_PHYSICAL() etc. used to store only the low 29
           bits of host-built pointers in commands such as G_MTX. On PORT those
           macros are now full passthroughs — (u32)(uintptr_t)(x) — so raw is
           the complete unmasked low32 of the real host pointer (see
           bugfix/os_convert_macro_corruption / bugfix/effects_regression_audit).
           The window mask below must match that: reconstruct by taking the
           high 32 bits from a known range and ORing in the full low32, the
           same convention gdx_resolve_module_host_address() and the
           mModuleBegin block below already use. Extract the window helper
           early so it can run either before or after the segment table
           depending on the caller. Bounds + readability checks prevent small
           immediates from matching. */
        constexpr uintptr_t kPhysicalAddressMask = 0xFFFFFFFFu;
        // GDX_LEGACY_RESOLVE quarantine: tryPhysicalWindow/tryAllPhysicalWindows
        // reconstruct a full pointer by substituting a KNOWN range's high32 onto
        // `raw`'s low32 -- a guess (see the quarantine block above
        // kFallbackIdentityMtx). The gate lives in tryAllPhysicalWindows so both
        // call sites below are covered by a single check.
        const auto tryPhysicalWindow = [&](uintptr_t begin, uintptr_t end) -> bool {
            if ((begin == 0) || (end <= begin)) {
                return false;
            }
            uintptr_t full = (begin & ~kPhysicalAddressMask) | static_cast<uintptr_t>(raw);
            /*
             * A host range can cross a 4 GB low32 window (the full passthrough
             * period). In that case the token may belong to the next window
             * even though the range begins in the previous one.
             */
            if (full < begin) {
                constexpr uintptr_t kPhysicalAddressWindow = kPhysicalAddressMask + 1u;
                if (full > UINTPTR_MAX - kPhysicalAddressWindow) {
                    return false;
                }
                full += kPhysicalAddressWindow;
            }
            if ((full < begin) || (full >= end) ||
                (requiredBytes > static_cast<size_t>(end - full)) ||
                (ReadableByteLimit(full) < requiredBytes)) {
                return false;
            }
            out.full = full;
            out.segmented = false;
            RecordLegacyResolveHit(LegacyResolveBranch::kPhysicalWindow, raw, gLegacyResolveCurrentOp);
            return true;
        };
        const auto tryAllPhysicalWindows = [&]() -> bool {
            if (!LegacyResolveEnabled()) {
                return false;
            }
            if (tryPhysicalWindow(mModuleBegin, mModuleEnd)) return true;
            for (const HostRange& range : gHostRanges) {
                if ((range.begin == 0) || (range.size == 0) ||
                    (range.size > UINTPTR_MAX - range.begin)) {
                    continue;
                }
                if (tryPhysicalWindow(range.begin, range.begin + range.size)) return true;
            }
            return false;
        };

        /* A host-built display list and the matrices/vertices it references are
         * almost always allocated together in the same 4 GB low32 window
         * (the GfxPool / task DL arena). When that arena is not a
         * registered host range, the reconstructions above miss and the pointer
         * would fall back to an identity matrix. Reconstruct the pointer from the
         * referencing DL's own window as a last resort, accepting it only when the
         * result is readable AND lies within one allocation region of the source
         * so it can never false-match unrelated memory elsewhere in the window. */
        // GDX_LEGACY_RESOLVE quarantine: trySourceWindow is explicitly a
        // "last resort" guess (see the quarantine block above
        // kFallbackIdentityMtx) -- gated the same as the other branches.
        const auto trySourceWindow = [&]() -> bool {
            // A/B escape hatch: GDX_DIAG_NO_SRCWIN=1 disables the source-window
            // matrix reconstruction so regressions can be bisected without a rebuild.
            static const bool sSrcWinDisabled = std::getenv("GDX_DIAG_NO_SRCWIN") != nullptr;
            if (!LegacyResolveEnabled() || sSrcWinDisabled || (sourceHint == 0)) return false;
            uintptr_t full = (sourceHint & ~kPhysicalAddressMask) | static_cast<uintptr_t>(raw);
            const uintptr_t lo = (full < sourceHint) ? full : sourceHint;
            const uintptr_t hi = (full < sourceHint) ? sourceHint : full;
            constexpr uintptr_t kSourceWindowSpan = 0x04000000u; // 64 MB around the DL
            if ((hi - lo) > kSourceWindowSpan) return false;
            if (ReadableByteLimit(full) < requiredBytes) return false;
            out.full = full;
            out.segmented = false;
            RecordLegacyResolveHit(LegacyResolveBranch::kSourceWindow, raw, gLegacyResolveCurrentOp);
            return true;
        };

        /* When the caller knows this raw value came from K0_TO_PHYS on a host
           pointer (e.g., a G_MTX from host-built F3DEX2 code), try the full
           low32 physical window BEFORE the segment table.  That prevents raw
           values whose top byte matches an active segment index — e.g.,
           0x0805DAA0 matching segment 8 — from being misrouted. */
        if (preferPhysical && (tryAllPhysicalWindows() || trySourceWindow())) {
            return true;
        }

        if ((encodedSegment < kGfxSegmentCount) &&
            ((gSegments[encodedSegment] != 0) || (encodedSegment == 0)) &&
            ((raw & 0x00FFFFFF) < kSegmentOffsetLimit)) {
            const uintptr_t full = gSegments[encodedSegment] + encodedOffset;
            /* Same readability gate as the explicit-segment path above: reject
               truncated module pointers masquerading as segment offsets so the
               physical-window reconstruction below gets a chance. */
            if (ReadableByteLimit(full) >= requiredBytes) {
                out.full = full;
                out.segment = encodedSegment;
                out.offset = encodedOffset;
                out.segmented = true;
                return true;
            }
        }

        if (!preferPhysical && (tryAllPhysicalWindows() || trySourceWindow())) {
            return true;
        }

        /* Ambiguous cross-segment fallback (audited 2026-07-08): this used to pick
           whichever registered segment produced the numerically SMALLEST low32
           offset for `raw`, with no readability check at all. When the raw
           value's own top-byte segment is stale/unregistered this frame (e.g. a
           race decoration or machine part referencing a segment that has not
           been (re)pointed yet), a completely unrelated segment could "win" this
           race purely by low32 coincidence -- and the winner was accepted even
           if the resulting address was unreadable or, worse, readable-but-wrong
           (garbage bytes from someone else's buffer). That is a plausible source
           of the wrong-but-readable vertex/matrix loads behind the still-visible
           spike/mesh-explosion reports even after the requiredBytes fix, which
           only guarded against SHORT reads, not WRONG-but-long-enough ones.
           Fix: gather every segment whose low32 offset is in range, sort by
           offset ascending, and accept the first candidate that actually proves
           readable for requiredBytes -- falling through to the next-closest
           segment instead of blindly trusting the closest one.
           GDX_LEGACY_RESOLVE quarantine: this whole block is the "ambiguous
           cross-segment fallback" guess (see the quarantine block above
           kFallbackIdentityMtx) -- gated as a unit. */
        if (LegacyResolveEnabled()) {
            struct SegCandidate {
                uint8_t segment;
                uint32_t offset;
                uintptr_t full;
            };
            SegCandidate candidates[kGfxSegmentCount];
            int candidateCount = 0;

            for (uint8_t segment = 0; segment < kGfxSegmentCount; segment++) {
                const uintptr_t base = gSegments[segment];
                if (base == 0) {
                    continue;
                }

                const uint32_t baseLow = static_cast<uint32_t>(base);
                const uint32_t offset = raw - baseLow;
                if (offset < kSegmentOffsetLimit) {
                    candidates[candidateCount++] = SegCandidate{ segment, offset, base + offset };
                }
            }

            std::sort(candidates, candidates + candidateCount,
                      [](const SegCandidate& a, const SegCandidate& b) { return a.offset < b.offset; });

            for (int i = 0; i < candidateCount; i++) {
                if (ReadableByteLimit(candidates[i].full) >= requiredBytes) {
                    out.full = candidates[i].full;
                    out.segment = candidates[i].segment;
                    out.offset = candidates[i].offset;
                    out.segmented = true;
                    RecordLegacyResolveHit(LegacyResolveBranch::kCrossSegmentFallback, raw, gLegacyResolveCurrentOp);
                    return true;
                }
            }
        }

        // GDX_LEGACY_RESOLVE quarantine: mModuleBegin high-32 reconstruction guess.
        if (LegacyResolveEnabled() && (mModuleBegin != 0)) {
            uintptr_t full = (mModuleBegin & 0xFFFFFFFF00000000ULL) | static_cast<uintptr_t>(raw);
            if (full < mModuleBegin) {
                full += 0x100000000ULL;
            }

            /* Vertex-spike root cause (#3), confirmed by [vtx-spike] runtime evidence:
               this reconstruction only checked that `full` falls inside the overall
               module's mapped address RANGE, never that `requiredBytes` are actually
               readable from it. A candidate landing 5 bytes from the end of a mapped
               region (e.g. right before an unmapped/guard page) would "succeed" here
               and then a 58-vertex (928-byte) MakePersistentVtxCopy walked hundreds of
               bytes into unrelated/unmapped memory -- one or more garbage vertices,
               the visible stretched-polygon "spike". Require the full payload to be
               readable, matching the other candidate paths in this function. */
            if ((full >= mModuleBegin) && (full < mModuleEnd) &&
                (ReadableByteLimit(full) >= requiredBytes)) {
                out.full = full;
                out.segmented = false;
                RecordLegacyResolveHit(LegacyResolveBranch::kModuleHigh32, raw, gLegacyResolveCurrentOp);
                return true;
            }
        }

        // GDX_LEGACY_RESOLVE quarantine: raw>=0x10000000 highCandidates guess scan.
        if (LegacyResolveEnabled() && (raw >= 0x10000000)) {
            const uintptr_t highCandidates[] = {
                reinterpret_cast<uintptr_t>(mRootBegin) & 0xFFFFFFFF00000000ULL,
                mModuleBegin & 0xFFFFFFFF00000000ULL,
                gSegments[0] & 0xFFFFFFFF00000000ULL,
                gSegments[1] & 0xFFFFFFFF00000000ULL,
                gSegments[2] & 0xFFFFFFFF00000000ULL,
                gSegments[3] & 0xFFFFFFFF00000000ULL,
                gSegments[4] & 0xFFFFFFFF00000000ULL,
                gSegments[5] & 0xFFFFFFFF00000000ULL,
                gSegments[6] & 0xFFFFFFFF00000000ULL,
                gSegments[7] & 0xFFFFFFFF00000000ULL,
                gSegments[8] & 0xFFFFFFFF00000000ULL,
                gSegments[9] & 0xFFFFFFFF00000000ULL,
                gSegments[10] & 0xFFFFFFFF00000000ULL,
                gSegments[11] & 0xFFFFFFFF00000000ULL,
                gSegments[12] & 0xFFFFFFFF00000000ULL,
                gSegments[13] & 0xFFFFFFFF00000000ULL,
                gSegments[14] & 0xFFFFFFFF00000000ULL,
                gSegments[15] & 0xFFFFFFFF00000000ULL,
            };

            for (uintptr_t high : highCandidates) {
                if (high == 0) {
                    continue;
                }

                // Same vertex-spike fix as the mModuleBegin block above: a 1-byte
                // IsReadableAddress() check let a candidate a few bytes from the end
                // of its region "succeed" for a much larger vertex/matrix payload.
                const uintptr_t full = high | static_cast<uintptr_t>(raw);
                if (ReadableByteLimit(full) >= requiredBytes) {
                    out.full = full;
                    out.segmented = false;
                    RecordLegacyResolveHit(LegacyResolveBranch::kRawHigh32Scan, raw, gLegacyResolveCurrentOp);
                    return true;
                }
            }

            /* Also try high32 from every registered host range (covers heap / fiber-stack
               allocations that share a VirtualAlloc region with gdx_rdram but whose full
               address isn't yet captured in gSegments or the module range). */
            for (const auto& range : gHostRanges) {
                if (range.begin == 0) {
                    continue;
                }
                const uintptr_t high = range.begin & 0xFFFFFFFF00000000ULL;
                if (high == 0) {
                    continue;
                }
                const uintptr_t full = high | static_cast<uintptr_t>(raw);
                if (ReadableByteLimit(full) >= requiredBytes) {
                    out.full = full;
                    out.segmented = false;
                    RecordLegacyResolveHit(LegacyResolveBranch::kRawHigh32Scan, raw, gLegacyResolveCurrentOp);
                    return true;
                }
            }
        }

        {
            static int sResolveFails = 0;
            if (sResolveFails < 200) {
                ++sResolveFails;
                const uintptr_t rootHigh = mRootBegin
                    ? (reinterpret_cast<uintptr_t>(mRootBegin) & 0xFFFFFFFF00000000ULL)
                    : 0ULL;
                const uintptr_t modHigh  = mModuleBegin & 0xFFFFFFFF00000000ULL;
                const uintptr_t rootCand = rootHigh | static_cast<uintptr_t>(raw);
                const uintptr_t modCand  = modHigh  | static_cast<uintptr_t>(raw);
                gdx_port_logf("[resolve-fail] raw=%08X "
                              "mModule=[%016llX,%016llX) "
                              "rootCand=%016llX rootReadable=%d "
                              "modCand=%016llX modInRange=%d modReadable=%d\n",
                              raw,
                              static_cast<unsigned long long>(mModuleBegin),
                              static_cast<unsigned long long>(mModuleEnd),
                              static_cast<unsigned long long>(rootCand),
                              IsReadableAddress(rootCand) ? 1 : 0,
                              static_cast<unsigned long long>(modCand),
                              (modCand >= mModuleBegin && modCand < mModuleEnd) ? 1 : 0,
                              IsReadableAddress(modCand) ? 1 : 0);
            }
        }
        return false;
    }

    uintptr_t TranslateDataPointer(uint32_t raw, size_t requiredBytes = 1, bool preferPhysical = false,
                                   uintptr_t sourceHint = 0) const {
        if (raw == 0) {
            return 0;
        }

        ResolvedAddress resolved = {};
        if (TryResolveAddress(raw, resolved, requiredBytes, preferPhysical, sourceHint)) {
            return IsReadableAddress(resolved.full) ? resolved.full : 0;
        }

        // GDX_LEGACY_RESOLVE quarantine: treats the bare low32 as a literal host
        // pointer -- only "works" if the host allocation happens to sit under
        // 4 GB, which is not guaranteed on any 64-bit target. A guess like the
        // others above (see the quarantine block above kFallbackIdentityMtx).
        if (!LegacyResolveEnabled()) {
            return 0;
        }
        const uintptr_t direct = static_cast<uintptr_t>(raw);
        if (IsReadableAddress(direct)) {
            RecordLegacyResolveHit(LegacyResolveBranch::kDirectCast, raw, gLegacyResolveCurrentOp);
            return direct;
        }
        return 0;
    }

    static uint64_t TextureBytesForPixels(uint64_t pixels, uint32_t size) {
        switch (size) {
            case 0: return (pixels + 1) / 2;
            case 1: return pixels;
            case 2: return pixels * 2;
            case 3: return pixels * 4;
            default: return 0;
        }
    }

    static uint64_t LoadBlockCopyBytes(const N64Gfx& command, uint32_t size, uint32_t imageWidth) {
        /* G_LOADBLOCK's uls is a raw source texel offset (bits 23:12 of w0),
         * unlike G_LOADTILE's fixed-point texture coordinates. The bridge must copy from
         * settimg_ptr all the way through the end of this load (uls + lrs + 1 texels)
         * so libultraship can index into the buffer at the correct source offset. */
        const uint32_t uls_texels = (command.w0 >> 12) & 0xFFF;
        const uint32_t ult_rows = command.w0 & 0xFFF;
        const uint32_t lrs = (command.w1 >> 12) & 0xFFF;
        const uint64_t startTexel = static_cast<uint64_t>(ult_rows) * imageWidth + uls_texels;
        return TextureBytesForPixels(startTexel + lrs + 1, size);
    }

    static uint64_t LoadTileCopyBytes(const N64Gfx& command, uint32_t size, uint32_t imageWidth) {
        const uint32_t uls = (command.w0 >> 12) & 0xFFF;
        const uint32_t ult = command.w0 & 0xFFF;
        const uint32_t lrs = (command.w1 >> 12) & 0xFFF;
        const uint32_t lrt = command.w1 & 0xFFF;
        if ((lrs < uls) || (lrt < ult)) {
            return 0;
        }

        const uint64_t offsetX = uls >> kTextureImageFrac;
        const uint64_t offsetY = ult >> kTextureImageFrac;
        const uint64_t width = ((lrs - uls) >> kTextureImageFrac) + 1;
        const uint64_t height = ((lrt - ult) >> kTextureImageFrac) + 1;

        uint64_t bytesPerLine = TextureBytesForPixels(imageWidth, size);
        uint64_t offsetBytes = TextureBytesForPixels(offsetX, size);
        if (size == 0) {
            offsetBytes = offsetX / 2;
        }

        const uint64_t tileLineBytes = TextureBytesForPixels(width, size);
        return (offsetY * bytesPerLine) + offsetBytes + ((height - 1) * bytesPerLine) + tileLineBytes;
    }

    static uint64_t LoadTlutCopyBytes(const N64Gfx& command) {
        const uint32_t highIndex = (command.w1 >> 14) & 0x3FF;
        return (static_cast<uint64_t>(highIndex) + 1) * 2;
    }

    size_t EstimateRawTextureCopyBytes(const N64Gfx* source, size_t index, size_t limit, size_t stride, bool isBig) const {
        /* Wide-layout w1 fix (2026-07-10, rank-gadget confetti root cause):
           ReadCommand is an 8-byte reader (w1 at offset +4). A wide 16-byte
           packet stores w1 at offset +8; offset +4 is zero padding. The main
           ProcessList loop compensates (memcpy from +8) but this scan never
           did, so every w1-derived load extent (LOADBLOCK lrs, LOADTILE
           lrt/lrs, TLUT count) read as 0 on wide lists -- the estimate
           collapsed to kMinRawTextureCopyBytes (8) and LUS decoded a full
           tile out of an 8-byte copy (the HUD rank digits' confetti). w0 is
           at offset 0 in both layouts and needs no correction. */
        const bool sourceIsWide = (stride == kHostBuiltGfxStride);
        const auto readScanCommand = [&](size_t i) {
            N64Gfx command = ReadCommand(source, i, stride, isBig);
            if (sourceIsWide) {
                uintptr_t w1full = 0;
                std::memcpy(&w1full, reinterpret_cast<const uint8_t*>(source) + i * stride + 8,
                            sizeof(w1full));
                command.w1 = static_cast<uint32_t>(w1full);
            }
            return command;
        };
        const N64Gfx setImg = readScanCommand(index);
        const uint32_t size = (setImg.w0 >> 19) & 0x3;
        const uint32_t imageWidth = (setImg.w0 & 0xFFF) + 1;
        uint64_t required = kMinRawTextureCopyBytes;

        const size_t scanEnd = std::min(limit, index + 1 + kTextureLoadScanCommandLimit);
        for (size_t i = index + 1; i < scanEnd; i++) {
            const N64Gfx command = readScanCommand(i);
            switch (Opcode(command.w0)) {
                case kOpSetTextureImage:
                case kOpEndDl:
                    i = scanEnd;
                    break;
                case kOpLoadBlock:
                    required = std::max(required, LoadBlockCopyBytes(command, size, imageWidth));
                    break;
                case kOpLoadTile:
                    required = std::max(required, LoadTileCopyBytes(command, size, imageWidth));
                    break;
                case kOpLoadTlut:
                    required = std::max(required, LoadTlutCopyBytes(command));
                    break;
                default:
                    break;
            }
        }

        if ((required == 0) || (required > kMaxRawTextureCopyBytes)) {
            return 0;
        }
        return static_cast<size_t>(required);
    }

    uintptr_t TranslateTexturePointer(uint32_t raw, const N64Gfx* source, size_t index, size_t limit, bool isBig,
                                      size_t stride) {
        /* Resolve with the actual upcoming load size so the readability gates in
           TryResolveAddress can reject short false matches (e.g. a segment base
           plus a truncated-module-pointer offset that is only readable for a few
           bytes) instead of feeding a partial buffer to the texture copy. */
        const size_t estimatedBytes = EstimateRawTextureCopyBytes(source, index, limit, stride, isBig);
        const uintptr_t translated = TranslateDataPointer(raw, std::max<size_t>(estimatedBytes, 1));
        if (translated == 0) {
            static int sMissingTexturePointerPrints = 0;
            if (sMissingTexturePointerPrints < 200) {
                gdx_port_logf("[texdiag] unresolved G_SETTIMG pointer raw=%08X\n", raw);
                sMissingTexturePointerPrints++;
            }
            return 0;
        }

        /* [digit-carve] one-shot (2026-07-10): the rank digits
           (aPositionDigitTexs, seg4+0x13DE0) are ZERO-FILLED in ROM -- the
           console composes/loads them at runtime. This dump decides whether
           the port's carve receives that runtime content (nonzero => the
           live-carve resolver fix alone renders the gadget) or stays zero
           (=> an EK/asset fill gap remains and the digits will be invisible
           until that loader is found). */
        if (gGdxRaceActive != 0 && gSegments[4] != 0) {
            static bool sDigitCarveLogged = false;
            if (!sDigitCarveLogged) {
                sDigitCarveLogged = true;
                const uint8_t* d = reinterpret_cast<const uint8_t*>(gSegments[4]) + 0x13DE0;
                gdx_port_logf("[digit-carve] gSegments[4]+13DE0: "
                              "%02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X\n",
                              d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
                              d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
            }
        }
        // How much data can we safely read from `translated`?
        // VirtualQuery (ReadableByteLimit) is ground truth for Windows page regions.
        // For the 8MB RDRAM calloc the entire region is one VirtualAlloc block, so
        // this gives us 7+ MB remaining — enough to cap cleanly at kMaxRawTextureCopyBytes.
        // For non-RDRAM host ranges gHostRanges is the fallback.
        // If neither gives us anything, fall back to scanning the local display list.
        const size_t registeredRemaining = RegisteredHostRemaining(translated);
        size_t readable = registeredRemaining;
        if (readable == 0) readable = ReadableByteLimit(translated);

        size_t required = estimatedBytes;
        if (required == 0) {
            static int sBadTextureEstimatePrints = 0;
            if (sBadTextureEstimatePrints < 200) {
                gdx_port_logf("[texdiag] zero/oversized texture estimate raw=%08X translated=%p\n",
                              raw, reinterpret_cast<const void*>(translated));
                sBadTextureEstimatePrints++;
            }
            // Fallback to min of readable or kMaxRawTextureCopyBytes
            required = std::min(readable, kMaxRawTextureCopyBytes);
        }
        
        // Always clamp to what is actually readable to avoid page faults
        required = std::min(required, readable);

        bool textureCopyRefreshed = false;
        uintptr_t outPtr = MakePersistentRawTextureCopy(translated, required, &textureCopyRefreshed);
        if (mStats != nullptr && textureCopyRefreshed) {
            mStats->textureCopyBytes += required;
        }
        if (outPtr == 0) {
            static int sTextureCopyFailPrints = 0;
            if (sTextureCopyFailPrints < 32) {
                gdx_port_logf("[texdiag] raw texture copy failed raw=%08X host=%p required=%zu vq=%zu reg=%zu\n",
                              raw, reinterpret_cast<const void*>(translated), required,
                              ReadableByteLimit(translated), RegisteredHostRemaining(translated));
                sTextureCopyFailPrints++;
            }
        }
        (void)textureCopyRefreshed;
        return outPtr;
    }

    const N64Gfx* ResolveDisplayListSource(uint32_t raw) const {
        ResolvedAddress resolved = {};
        if (TryResolveAddress(raw, resolved)) {
            if (!IsReadableAddress(resolved.full)) {
                return nullptr;
            }
            return reinterpret_cast<const N64Gfx*>(resolved.full);
        }
        return nullptr;
    }

    bool IsResolvableDisplayList(uint32_t raw, const N64Gfx** outTarget = nullptr) const {
        const N64Gfx* target = ResolveDisplayListSource(raw);
        if (target == nullptr) {
            return false;
        }

        const size_t limit = KnownCommandLimit(target);
        if ((limit == 0) || !LooksLikeDisplayList(target, limit)) {
            return false;
        }

        if (outTarget != nullptr) {
            *outTarget = target;
        }
        return true;
    }

    uintptr_t TranslateDisplayListPointer(uint32_t raw, const N64Gfx* parentSource = nullptr, size_t parentIndex = 0,
                                          const N64Gfx* directTarget = nullptr) {
        /*
         * Resolve the exact token first. Generated asset symbols are one-byte
         * host stubs and are not necessarily 8-byte aligned; masking them first
         * can collapse several distinct symbols into an unrelated texture.
         *
         * Genuine N64 DL addresses still get the hardware-compatible alignment
         * fallback, but only when the exact candidate is absent or invalid.
         *
         * Phase G1: a wide (host-built) parent already carries the real host
         * pointer to the sub-DL in directTarget. Use it verbatim — no low32
         * reconstruction, no alignment guessing — but still validate it and run
         * it through EnqueueList so the sub-list gets converted to F3DGfx (the
         * interpreter cannot consume a raw wide-decomp Gfx list directly).
         */
        const N64Gfx* target = (directTarget != nullptr) ? directTarget : ResolveDisplayListSource(raw);
        const auto isValidTarget = [this](const N64Gfx* candidate) {
            if (candidate == nullptr) {
                return false;
            }
            const size_t candidateLimit = KnownCommandLimit(candidate);
            return (candidateLimit != 0) && LooksLikeDisplayList(candidate, candidateLimit);
        };

        if (directTarget == nullptr && !isValidTarget(target)) {
            const uint32_t alignedRaw = raw & ~static_cast<uint32_t>(7u);
            if (alignedRaw != raw) {
                const N64Gfx* alignedTarget = ResolveDisplayListSource(alignedRaw);
                if (isValidTarget(alignedTarget)) {
                    raw = alignedRaw;
                    target = alignedTarget;
                }
            }
        }

        if (target == nullptr) {
            /* Split the print budget by race phase: a single global budget is
               exhausted by menu/transition frames long before a race starts, so
               a missing race-time DL (e.g. the finish-line start arc) was never
               logged. Same anti-pattern fixed for [bigtri]; mirror it here. */
            static int sMissingDlPrintsMenu = 0;
            static int sMissingDlPrintsRace = 0;
            int& missingBudget = (gGdxRaceActive != 0) ? sMissingDlPrintsRace : sMissingDlPrintsMenu;
            const int missingCap = (gGdxRaceActive != 0) ? 400 : 60;
            if (missingBudget < missingCap) {
                ++missingBudget;
                const uintptr_t parent = reinterpret_cast<uintptr_t>(parentSource);
                const size_t parentStride = parentSource ? CommandStrideForSource(parentSource) : kN64GfxStride;
                const N64Gfx parentCmd = parentSource ? ReadRawCommand(parentSource, parentIndex, parentStride) : N64Gfx{};
                gdx_port_logf("[gdl-miss] race=%d raw=%08X parent=%p index=%zu w0=%08X "
                              "seg0=%p seg1=%p seg2=%p seg3=%p seg8=%p\n",
                              gGdxRaceActive,
                              raw,
                              reinterpret_cast<const void*>(parent),
                              parentIndex,
                              parentCmd.w0,
                              reinterpret_cast<void*>(gSegments[0]),
                              reinterpret_cast<void*>(gSegments[1]),
                              reinterpret_cast<void*>(gSegments[2]),
                              reinterpret_cast<void*>(gSegments[3]),
                              reinterpret_cast<void*>(gSegments[8]));
            }
            if (mStats != nullptr) {
                mStats->noopDisplayLists++;
                if (mStats->firstNoopDlRaw == 0) mStats->firstNoopDlRaw = raw;
                mStats->missingDisplayLists++;
                if (mStats->firstMissingDlRaw == 0) {
                    mStats->firstMissingDlRaw = raw;
                    mStats->firstMissingParent = reinterpret_cast<uintptr_t>(parentSource);
                    mStats->firstMissingParentIndex = parentIndex;
                    if (parentSource != nullptr) {
                        const size_t stride = CommandStrideForSource(parentSource);
                        const bool isBig = CommandSourceIsBigEndian(parentSource, stride);
                        const N64Gfx rawParent = ReadRawCommand(parentSource, parentIndex, stride);
                        const N64Gfx decodedParent = ReadCommand(parentSource, parentIndex, stride, isBig);
                        mStats->firstMissingParentStride = stride;
                        mStats->firstMissingParentBigEndian = isBig;
                        mStats->firstMissingParentF3D =
                            IsF3DAssetPointer(reinterpret_cast<uintptr_t>(parentSource));
                        mStats->firstMissingParentRawW0 = rawParent.w0;
                        mStats->firstMissingParentRawW1 = rawParent.w1;
                        mStats->firstMissingParentDecodedW0 = decodedParent.w0;
                        mStats->firstMissingParentDecodedW1 = decodedParent.w1;
                    }
                }
            }
            return reinterpret_cast<uintptr_t>(mNoopList.data());
        }

        const size_t limit = KnownCommandLimit(target);
        if ((limit == 0) || !LooksLikeDisplayList(target, limit)) {
            /* Race-phase-split budget (see [gdl-miss] above): keep race-time bad
               DLs visible instead of being drowned by menu frames. */
            static int sBadDlPrintsMenu = 0;
            static int sBadDlPrintsRace = 0;
            int& badBudget = (gGdxRaceActive != 0) ? sBadDlPrintsRace : sBadDlPrintsMenu;
            const int badCap = (gGdxRaceActive != 0) ? 400 : 60;
            if (badBudget < badCap) {
                ++badBudget;
                const uint32_t alignedRaw = raw & ~7u;
                const N64Gfx* alignedTarget = (alignedRaw != raw) ? ResolveDisplayListSource(alignedRaw) : nullptr;
                const size_t targetStride = CommandStrideForSource(target);
                const N64Gfx first = (limit > 0) ? ReadCommand(target, 0, targetStride, CommandSourceIsBigEndian(target, targetStride)) : N64Gfx{};
                const size_t parentStride = parentSource ? CommandStrideForSource(parentSource) : kN64GfxStride;
                const N64Gfx parentCmd = parentSource ? ReadRawCommand(parentSource, parentIndex, parentStride) : N64Gfx{};
                gdx_port_logf("[gdl-bad] race=%d raw=%08X target=%p limit=%zu first=%08X "
                              "alignedRaw=%08X aligned=%p alignedFirst=%08X parent=%p index=%zu w0=%08X\n",
                              gGdxRaceActive,
                              raw,
                              reinterpret_cast<const void*>(target),
                              limit,
                              first.w0,
                              alignedRaw,
                              reinterpret_cast<const void*>(alignedTarget),
                              alignedTarget ? ReadRawCommand(alignedTarget, 0, CommandStrideForSource(alignedTarget)).w0 : 0,
                              reinterpret_cast<const void*>(parentSource),
                              parentIndex,
                              parentCmd.w0);
            }
            if (mStats != nullptr) {
                mStats->noopDisplayLists++;
                if (mStats->firstNoopDlRaw == 0) mStats->firstNoopDlRaw = raw;
                mStats->badDisplayLists++;
                if (mStats->firstBadDlRaw == 0) {
                    const size_t stride = CommandStrideForSource(target);
                    const bool isBig = CommandSourceIsBigEndian(target, stride);
                    const size_t scanLimit = std::min(limit, kDisplayListValidationCommandLimit);
                    mStats->firstBadDlRaw = raw;
                    mStats->firstBadDlTarget = reinterpret_cast<uintptr_t>(target);
                    mStats->firstBadDlLimit = limit;
                    mStats->firstBadDlStride = stride;
                    mStats->firstBadDlBigEndian = isBig;
                    mStats->firstBadDlF3D = IsF3DAssetPointer(reinterpret_cast<uintptr_t>(target));
                    if (limit == 0) {
                        mStats->firstBadDlFailureReason = 1;
                    } else {
                        const N64Gfx first = ReadCommand(target, 0, stride, isBig);
                        mStats->firstBadDlFirstW0 = first.w0;
                        mStats->firstBadDlFirstW1 = first.w1;
                        mStats->firstBadDlFailureReason = 3;
                        mStats->firstBadDlFailureIndex = scanLimit;
                        for (size_t i = 0; i < scanLimit; ++i) {
                            const N64Gfx command = ReadCommand(target, i, stride, isBig);
                            const uint8_t op = Opcode(command.w0);
                            if (!IsLikelyDisplayListOpcode(op)) {
                                mStats->firstBadDlFailureReason = 2;
                                mStats->firstBadDlFailureIndex = i;
                                mStats->firstBadDlFailureOpcode = op;
                                break;
                            }
                            if ((op == kOpEndDl) || (op == 0xB8u)) {
                                break;
                            }
                        }
                    }
                }
            }
            return reinterpret_cast<uintptr_t>(mNoopList.data());
        }

        return EnqueueList(target, limit);
    }

    size_t EffectiveLimit(const N64Gfx* source, size_t explicitLimit) const {
        const size_t knownLimit = KnownCommandLimit(source);
        if ((explicitLimit != 0) && (knownLimit != 0)) return std::min(explicitLimit, knownLimit);
        if (explicitLimit != 0) return explicitLimit;
        if (knownLimit != 0) return knownLimit;
        return kMaxUnboundedDisplayListCommands;
    }

    bool LooksLikeDisplayList(const N64Gfx* source, size_t limit) const {
        const size_t scanLimit = std::min(limit, kDisplayListValidationCommandLimit);
        const size_t stride = CommandStrideForSource(source);
        const bool isBig = CommandSourceIsBigEndian(source, stride);
        for (size_t i = 0; i < scanLimit; i++) {
            const N64Gfx command = ReadCommand(source, i, stride, isBig);
            const uint8_t op = Opcode(command.w0);
            if (!IsLikelyDisplayListOpcode(op)) return false;
            if (op == kOpEndDl || op == 0xB8u) return true;  // 0xB8 = F3D G_ENDDL
        }
        return false;
    }

  public:
    Fast::F3DGfx* ConvertRoot() {
        if (mRootBegin == nullptr) return nullptr;
        const size_t rootLimit = RootCommandLimit(mRootBegin);
        /* CRASH FAILSAFE (campaign soak fix, 2026-07-08): a ROOT display list
           that does not validate must render NOTHING -- it must never be fed
           to the interpreter, which would execute arbitrary host memory as GBI
           commands (the first-frame [gfxfail] bad=1 crash). Sub-DLs are already
           routed to mNoopList when they fail LooksLikeDisplayList (see
           TranslateDisplayListPointer), but the ROOT is enqueued directly and
           bypassed that guard. The N64 task DL is always terminated by
           gSPEndDisplayList (Gfx_FullSync), so a well-formed root always
           satisfies LooksLikeDisplayList well within the scan limit; a
           truncated/garbage/zeroed root does not -> skip the whole frame. */
        if (rootLimit == 0 ||
            ReadableByteLimit(reinterpret_cast<uintptr_t>(mRootBegin)) <
                CommandStrideForSource(mRootBegin) ||
            !LooksLikeDisplayList(mRootBegin, rootLimit)) {
            static int sRootRejectLogs = 0;
            if (sRootRejectLogs < 16) {
                ++sRootRejectLogs;
                gdx_port_logf("[gfxfail] ROOT rejected: ptr=%p limit=%zu readable=%zuB "
                              "-- rendering nothing this frame\n",
                              reinterpret_cast<const void*>(mRootBegin), rootLimit,
                              ReadableByteLimit(reinterpret_cast<uintptr_t>(mRootBegin)));
            }
            return nullptr;
        }
        EnqueueList(mRootBegin, rootLimit);
        while (!mWorkQueue.empty()) {
            QueueItem item = mWorkQueue.back();
            mWorkQueue.pop_back();
            ProcessList(item);
        }
        return mLists[mRootBegin]->commands.data();
    }

    void ProcessList(QueueItem item) {
        if (mStats != nullptr) mStats->convertedLists++;
        item.listPtr->commands.reserve(item.limit);

        /* Segment-4 probe: hud_gfx (countdown faces, start arc) now has real
           backing, yet those elements still don't draw. Log every DL translated
           from inside the segment-4 window so the next run proves whether the
           countdown display lists are reached at all (vs. resolving elsewhere
           or never being emitted). */
        {
            static int sSeg4Probes = 0;
            const uintptr_t seg4Base = gSegments[4];
            const uintptr_t src = reinterpret_cast<uintptr_t>(item.source);
            if (seg4Base != 0 && src >= seg4Base && src < seg4Base + 0x29EA0 && sSeg4Probes < 12) {
                ++sSeg4Probes;
                gdx_port_logf("[seg4] DL translated from seg4+0x%X limit=%zu\n",
                              static_cast<unsigned>(src - seg4Base), item.limit);
            }
        }

        /* RDRAM/ROM-loaded asset pointers use 8-byte N64 command slots. Some loaded
         * GFX ranges are already fixup-swapped to host endian, while untagged ROM GFX
         * ranges remain big-endian; detect endian from the command stream itself.
         * BSS/module/GfxPool pointers hold host-built data (little-endian).
         * Use the physical location rather than a heuristic to avoid false positives. */
        const size_t stride = CommandStrideForSource(item.source);
        const bool isBig = CommandSourceIsBigEndian(item.source, stride);
        // Prefer the segment-tag table: DLs inside a tagged asset segment have a
        // known dialect and must never fall back to the opcode-scan heuristics.
        // Phase G2: for a converted wide buffer the source segment's dialect tag
        // is gone, so consult the dialect the converter recorded for it FIRST --
        // re-deriving it from the wide stream would risk the exact F3DEX2/F3D
        // misclassification the tag table exists to prevent.
        const GdxSegmentUcode sourceDialect =
            GdxAssetPointerDialect(reinterpret_cast<uintptr_t>(item.source));
        const auto convertedDialect = gConvertedWideIsF3d.find(item.source);
        const bool isF3DSource =
            (convertedDialect != gConvertedWideIsF3d.end()) ? convertedDialect->second
            : (sourceDialect == GdxSegmentUcode::F3D)    ? true
            : (sourceDialect == GdxSegmentUcode::F3DEX2) ? false
            : (IsF3DAssetPointer(reinterpret_cast<uintptr_t>(item.source)) ||
               DisplayListUsesF3D(item.source, item.limit, stride, isBig));
        if (isF3DSource && mStats != nullptr) {
            mStats->f3dLists++;
        }

        // Diagnostic: dump how the course material setup DLs (segment 8 +0x14040 /
        // +0x14078) are classified and converted; their SETTILEs program tiles 1-7
        // for every track draw, so a misconversion here breaks all course tiling.
        static const bool sDiagSetupDl = std::getenv("GDX_DIAG_SETUPDL") != nullptr;
        bool diagThisList = false;
        if (sDiagSetupDl && gSegments[8] != 0) {
            const uintptr_t src = reinterpret_cast<uintptr_t>(item.source);
            if (src == gSegments[8] + 0x14040 || src == gSegments[8] + 0x14078) {
                static int sSetupDlDumps = 0;
                if (sSetupDlDumps < 40) {
                    ++sSetupDlDumps;
                    diagThisList = (sSetupDlDumps <= 2);
                    gdx_port_logf("[setupdl] source=%p off=+%X stride=%zu big=%d f3d=%d limit=%zu race=%d\n",
                                  item.source, (unsigned)(src - gSegments[8]), stride, (int)isBig,
                                  (int)isF3DSource, item.limit, gGdxRaceActive);
                }
            }
        }

        /* Set while an unsupported microcode (e.g. L3DEX2, the line ucode Course
           Edit loads for its track lines) is active: its SP-side commands (vertex
           loads, G_LINE3D, matrix/DL/segment ops at op < 0xE0) have semantics the
           F3DEX2 interpreter doesn't implement, so running them through it
           produces garbage frames. Drop those until the display list loads a
           supported microcode again. */
        bool skipUnsupportedUcode = false;

        // Phase G1: host-built display lists carry a FULL pointer-width w1.
        // The decomp Gfx type under PORT is { u32 w0; <pad>; uintptr_t w1; } = 16
        // bytes, so the pointer-carrying word lives at byte offset 8, not 4.
        // ReadCommand() only recovers 8 bytes (w0 @0, a 32-bit w1 @4 == padding
        // on wide packets), so for wide sources we pull the real 64-bit word here
        // and fix in.w1 to its low 32 bits (all the operand parsing below still
        // expects the 32-bit token). Narrow N64/ROM/RDRAM lists are unchanged.
        const bool sourceIsWide =
            (stride == kHostBuiltGfxStride) && (kHostBuiltGfxStride > kN64GfxStride);

        for (size_t i = 0; i < item.limit; i++) {
            N64Gfx in = ReadCommand(item.source, i, stride, isBig);
            uintptr_t w1full;
            if (sourceIsWide) {
                std::memcpy(&w1full,
                            reinterpret_cast<const uint8_t*>(item.source) + i * stride + 8,
                            sizeof(w1full));
                in.w1 = static_cast<uint32_t>(w1full);
            } else {
                w1full = in.w1;
            }
            /* A wide packet whose pointer word has any high bits set is a REAL
             * host pointer the game already resolved (Phase G1) — use it verbatim
             * and skip the low32-reconstruction guesswork (the disease this phase
             * removes). A wide packet with high bits zero holds a 32-bit VALUE or
             * a segmented address, which still flows through the segment table /
             * value path below exactly as before. Narrow sources never set high
             * bits, so this is always false for them. */
            bool w1IsHostPointer =
                sourceIsWide && ((static_cast<uint64_t>(w1full) >> 32) != 0);
            /* EXCEPTION to the "high bits set => real host pointer" rule: the
             * game references runtime-loaded segmented assets (setup_gfx render-
             * mode DLs via gSPDisplayList(&D_3000050), vertex data via
             * gSPVertex(&D_3000xxx)) through 1-byte BSS PLACEHOLDER symbols. G1's
             * wide gSPDisplayList/gSPVertex pack the full host address of that
             * placeholder, so high32 is set and it LOOKS like a real host
             * pointer -- but the placeholder is a 1-byte object; the real DL/
             * vertex bytes live in the loaded segment image. Taking it verbatim
             * branches the interpreter into the 1-byte stub and reads adjacent
             * BSS as commands: odd branch targets, a G_GEOMETRYMODE word
             * (0xD9680800) misread as a vertex pointer, and a count=0xF0 garbage
             * G_VTX -- the boot/title-phase crash (campaign soak fix 3). Route
             * these back through the low32 resolver, which maps the placeholder
             * to the loaded segment via ResolveGeneratedAssetStub exactly as the
             * pre-wide build did. Only the placeholder's own low32 is affected;
             * genuine runtime host pointers (GfxPool sub-DLs, persistent vertex
             * copies) are never registered in the asset map and are unchanged. */
            if (w1IsHostPointer &&
                (IsAssetPlaceholderPointer(in.w1) || IsPortBssAliasPointer(in.w1))) {
                w1IsHostPointer = false;
            }
            const uint8_t op = Opcode(in.w0);
            // GDX_LEGACY_RESOLVE instrumentation: tag any guessing branch that
            // fires while resolving this command's pointer(s) with the opcode
            // that triggered it (see the quarantine block above
            // kFallbackIdentityMtx). Free functions (ResolveRegisteredHostPointer,
            // FallbackDataPointer) read this global instead of a threaded param.
            gLegacyResolveCurrentOp = op;
            /* Keep 0xDD (a ucode reload ends the skip), ENDDL (0xDF EX2 / 0xB8
               F3D — dropping the terminator would leave the translated list
               unterminated and the interpreter would run off its end), AND any
               RDP command (op >= 0xE0: SETCIMG/SETZIMG/SETSCISSOR/SETPRIMCOLOR/
               SETCOMBINE/FILLRECT/tile loads/...). RDP commands are executed by
               the RDP hardware, not the active RSP microcode, so on real N64
               hardware they run identically whether F3DEX2 or L3DEX2 is loaded;
               dropping them left the color image / scissor / prim color / combine
               state stale for whatever the game draws right after the L3DEX2
               section (course_edit/191080.c reloads F3DEX2 and keeps drawing
               HUD/gadget content into the same viewport), which is a second,
               independent source of the reported Course Edit flicker beyond the
               missing line geometry itself.
               Deliberately NOT counted in skippedDataCommands: that counter
               drives the per-frame [datafail] log line, and an intentional
               line-ucode skip would spam it every Course Edit frame. */
            if (skipUnsupportedUcode && op != 0xDD && op != 0xDF && op != 0xB8 && op < 0xE0) {
                continue;
            }
            if (diagThisList && i < 24) {
                gdx_port_logf("[setupdl]   #%02zu %08X %08X op=%02X\n", i, in.w0, in.w1, op);
            }
            /* TEXRECT coord probe: log first 8 TEXRECTs unconditionally to diagnose zoom. */
            if (op == 0xE4 || op == 0xE5) {
                static int sTRectProbe = 0;
                if (sTRectProbe < 8) {
                    const uint32_t lrx = (in.w0 >> 12) & 0xFFF;
                    const uint32_t lry = in.w0 & 0xFFF;
                    const uint32_t tile = (in.w1 >> 24) & 0x7;
                    const uint32_t ulx = (in.w1 >> 12) & 0xFFF;
                    const uint32_t uly = in.w1 & 0xFFF;
                    gdx_port_logf("[trect] #%d op=%02X ul=(%u,%u) lr=(%u,%u) tile=%u stride=%zu f3d=%d big=%d\n",
                                  sTRectProbe, op, ulx, uly, lrx, lry, tile, stride, (int)isF3DSource, (int)isBig);
                    sTRectProbe++;
                }
            }
            uintptr_t outW0 = static_cast<uintptr_t>(in.w0);
            uintptr_t outW1 = static_cast<uintptr_t>(in.w1);
            if (mStats != nullptr) mStats->opCounts[op]++;

            /* #16 unconditional raw-value trace: the count==4-gated [rect] probe
               never matched the countdown draw's known raw pointers (captured
               decomp-side), even though gfxdiag shows this list's vtx/mtx commands
               being processed with no miss/bad/skip counts. Dump EVERY vtx/mtx raw
               w0/w1 seen during the race so the exact command stream can be
               diffed against the decomp-side low32 values without a count filter
               that could itself be hiding the match (e.g. wrong endianness making
               the parsed count something other than 4). */
            {
                static const bool sDiagCountdownRaw = std::getenv("GDX_DIAG_COUNTDOWN") != nullptr;
                // Gate on the arm flag (set decomp-side the instant the countdown
                // draw code runs) instead of gGdxRaceActive: that flag stays 1 for
                // the whole race, so a fixed-size trace filled up long before the
                // countdown ever appeared. Keep capturing a little after arming
                // too (car doesn't stop generating HUD each frame).
                if (sDiagCountdownRaw && gGdxCountdownProbeArm != 0 && (op == kOpVtx || op == kOpMtx)) {
                    static int sRawTraceCount = 0;
                    static FILE* sRawTraceFile = nullptr;
                    if (sRawTraceCount == 0) {
                        sRawTraceFile = fopen("vtx-mtx-trace.txt", "w");
                    }
                    if (sRawTraceFile != nullptr && sRawTraceCount < 400000) {
                        ++sRawTraceCount;
                        fprintf(sRawTraceFile, "op=%02X w0=%08X w1=%08X f3d=%d big=%d src=%p i=%zu\n",
                                op, in.w0, in.w1, (int)isF3DSource, (int)isBig, item.source, i);
                        if ((sRawTraceCount % 500) == 0) {
                            fflush(sRawTraceFile);
                        }
                    }
                }
            }

            switch (op) {
                case kOpVtx:
                    // F3D uses opcode 0x01 for G_MTX (not G_VTX). Remap to kOpMtx so Fast3D
                    // doesn't try to load a 64-byte matrix struct as a vertex buffer. The
                    // parameter flags also differ: legacy F3D stores them in w0[23:16],
                    // while F3DEX2 stores its XOR-with-PUSH form in w0[7:0].
                    if (isF3DSource) {
                        const uint8_t legacy = static_cast<uint8_t>((in.w0 >> 16) & 0xFFu);
                        uint8_t parameters = 0;
                        if ((legacy & 0x01u) != 0) parameters |= 0x04u; // projection
                        if ((legacy & 0x02u) != 0) parameters |= 0x02u; // load
                        if ((legacy & 0x04u) != 0) parameters |= 0x01u; // push
                        const uint8_t encoded = parameters ^ 0x01u;
                        outW0 = (static_cast<uintptr_t>(kOpMtx) << 24) |
                                static_cast<uintptr_t>(encoded);
                    }
                    {
                        /* Vertex-spike root cause (#3): TranslateDataPointer used to be
                           called with its default requiredBytes=1, so the resolver only
                           proved the FIRST byte of the vertex buffer readable while
                           MakePersistentVtxCopy below unconditionally read numVtx*16
                           bytes. An ambiguous/near-miss resolution (segment-offset
                           guess, physical-window reconstruction) could pass that 1-byte
                           gate yet be wrong or short past byte 0 -- one garbage vertex
                           is a visible stretched-polygon "spike". Require the FULL
                           vertex payload to be readable before accepting a candidate. */
                        const uint32_t vtxCount = (!isF3DSource) ? ((in.w0 >> 12) & 0xFFu) : 0u;
                        const size_t vtxRequiredBytes =
                            (vtxCount != 0) ? (static_cast<size_t>(vtxCount) * 16u) : 1u;
                        /* Bridge asymmetry fix (2026-07-08): the G_MTX case below passes
                           preferPhysical=!isF3DSource and sourceHint=item.source, but this
                           G_VTX case never did, so trySourceWindow() -- explicitly written
                           to reconstruct "a host-built display list and the
                           matrices/vertices it references [that] are almost always
                           allocated together in the same 4GB low32 window" -- was never
                           even attempted for vertex loads. Mirror the matrix call so
                           vertex buffers get the same last-resort reconstruction. */
                        outW1 = w1IsHostPointer
                                    ? w1full
                                    : TranslateDataPointer(in.w1, vtxRequiredBytes, /*preferPhysical=*/!isF3DSource,
                                                      reinterpret_cast<uintptr_t>(item.source));
                        /* [vtx-census] (2026-07-11, interior-stub-aliasing fix
                           verification): one line per UNIQUE vertex source, NOT
                           race-gated (machine select is a primary target).
                           Logs the resolution class and CONTENT fingerprint at
                           +0x10 -- the failure family this verifies is
                           readable-but-zero/aliased data, which the failsafe
                           probes below can never see. */
                        {
                            static uint32_t sVtxCensusSeen[48] = {};
                            static int sVtxCensusCount = 0;
                            bool vcDup = false;
                            for (int vc = 0; vc < sVtxCensusCount; vc++) {
                                if (sVtxCensusSeen[vc] == in.w1) {
                                    vcDup = true;
                                    break;
                                }
                            }
                            /* Retargeted 2026-07-11: general sources burned the
                               budget before race time twice. Segment-8 raws
                               (the decoration family, e.g. 0x08017388) are the
                               open investigation — census those exclusively. */
                            if (!vcDup && sVtxCensusCount < 48 && outW1 != 0 &&
                                ((in.w1 >> 24) & 0xFFu) == 0x08u) {
                                sVtxCensusSeen[sVtxCensusCount++] = in.w1;
                                const char* vcls = IsRdramHostPointer(outW1) ? "rdram"
                                                   : (mModuleBegin != 0 && outW1 >= mModuleBegin &&
                                                      outW1 < mModuleEnd)   ? "module"
                                                                             : "heap";
                                uint8_t vfp[8] = {};
                                if (ReadableByteLimit(outW1 + 0x10) >= sizeof(vfp)) {
                                    std::memcpy(vfp, reinterpret_cast<const void*>(outW1 + 0x10), sizeof(vfp));
                                }
                                gdx_port_logf("[vtx-census] raw=%08X host=%d out=%p cls=%s n=%u "
                                              "fp8=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                                              in.w1, w1IsHostPointer ? 1 : 0,
                                              reinterpret_cast<void*>(outW1), vcls, vtxCount,
                                              vfp[0], vfp[1], vfp[2], vfp[3], vfp[4], vfp[5], vfp[6], vfp[7]);
                            }
                        }
                        // Always on (2026-07-08): was gated behind GDX_DIAG_VTX, so a
                        // user run without that env var set captured zero [vtx-spike]/
                        // [vtx-dropped] evidence even while the visible spikes/mesh
                        // explosions kept happening. The per-frame cost is one branch
                        // plus (when a race is active) a capped set of log lines, so
                        // there is no reason to require manual opt-in for this.
                        if (gGdxRaceActive != 0 && vtxCount != 0) {
                            if (outW1 != 0) {
                                const size_t readable = ReadableByteLimit(outW1);
                                static int sVtxSpikeLogs = 0;
                                if (readable < vtxRequiredBytes && sVtxSpikeLogs < 40) {
                                    ++sVtxSpikeLogs;
                                    gdx_port_logf("[vtx-spike] raw=%08X resolved=%p need=%zuB readable=%zuB count=%u src=%p\n",
                                                  in.w1, reinterpret_cast<void*>(outW1), vtxRequiredBytes, readable,
                                                  vtxCount, item.source);
                                }
                            } else {
                                /* The strict requiredBytes gate rejected every candidate.
                                   Log what the OLD requiredBytes=1 rule would have
                                   accepted, to show whether this DL is the source of a
                                   spike that has now been turned into a dropped-vertex
                                   pop instead (safer, but still worth knowing about). */
                                const uintptr_t loose = TranslateDataPointer(in.w1, 1);
                                static int sVtxDroppedLogs = 0;
                                if (sVtxDroppedLogs < 40) {
                                    ++sVtxDroppedLogs;
                                    gdx_port_logf("[vtx-dropped] raw=%08X count=%u need=%zuB "
                                                  "looseResolve=%p looseReadable=%zuB src=%p\n",
                                                  in.w1, vtxCount, vtxRequiredBytes,
                                                  reinterpret_cast<void*>(loose),
                                                  loose ? ReadableByteLimit(loose) : 0u, item.source);
                                }
                            }
                        }
                    }
                    if (outW1 != 0 && isBig && !isF3DSource) {
                        outW1 = MakePersistentVtxCopy(outW1, (outW0 >> 12) & 0xFF);
                    } else if (outW1 != 0 && isBig && isF3DSource) {
                        /* This command was just remapped to G_MTX: big-endian
                           static Mtx data needs the word-swapped copy. */
                        outW1 = MakePersistentMtxCopy(outW1);
                    }
                    if (outW1 != 0) {
                        outW1 = NormalizeLusDirectPointer(outW1);
                    }
                    /* CRASH FAILSAFE (campaign soak fix, 2026-07-08): host-built
                       little-endian display lists (the GfxPool root and its
                       sub-DLs) skip MakePersistentVtxCopy -- which is the ONLY
                       vertex path that clamps the read to the readable region --
                       so an under-validated / truncated / legacy-guessed vertex
                       pointer reached the interpreter verbatim and was
                       dereferenced in GfxSpVertex (interpreter.cpp), faulting on
                       the first real frame after the boot-logo hold. The
                       [vtx-spike] probe above only LOGGED this; it never stopped
                       the garbage pointer. Never hand the interpreter a vertex
                       pointer whose full payload is not readable: substitute the
                       zeroed, fully-readable kFallbackVertices buffer (a benign
                       degenerate load) for a real F3DEX2 vertex load, or drop a
                       remapped F3D matrix pointer to 0 (the interpreter tolerates
                       a null matrix, same as the [mtx-dropped] path). */
                    if (outW1 != 0) {
                        const size_t vtxNeedBytes =
                            isF3DSource ? 64u
                                        : static_cast<size_t>((in.w0 >> 12) & 0xFFu) * 16u;
                        if (vtxNeedBytes != 0 && ReadableByteLimit(outW1) < vtxNeedBytes) {
                            static int sVtxFailsafeLogs = 0;
                            if (sVtxFailsafeLogs < 40) {
                                ++sVtxFailsafeLogs;
                                gdx_port_logf("[vtx-failsafe] raw=%08X resolved=%p need=%zuB "
                                              "readable=%zuB f3d=%d src=%p -> %s\n",
                                              in.w1, reinterpret_cast<void*>(outW1), vtxNeedBytes,
                                              ReadableByteLimit(outW1), (int)isF3DSource, item.source,
                                              isF3DSource ? "dropped" : "fallback-vertices");
                            }
                            outW1 = isF3DSource
                                        ? 0u
                                        : reinterpret_cast<uintptr_t>(kFallbackVertices);
                        }
                    }
                    /* #16 phase 3: publish the resolved host pointer the instant this
                       command's raw low32 matches the digit quad's tagged pointer
                       (set decomp-side in racer.c right before gSPVertex(gfx++,
                       D_400AA28, 4, 0)). interpreter.cpp's GfxSpVertex compares its
                       own vertex pointer argument against this to know precisely
                       when the digit quad itself loads, instead of the coarse
                       arm flag matching whatever triangle happens to run first. */
                    if (gGdxCountdownProbeArm != 0 && gGdxCountdownProbeVtxLow32 != 0 &&
                        in.w1 == gGdxCountdownProbeVtxLow32 && outW1 != 0) {
                        gGdxCountdownProbeResolvedVtx = outW1;
                    }
                    /* Countdown-quad probe (#16): the "3,2,1,GO" HUD quad is drawn as a
                       4-vertex textured rect (gSPVertex(gfx++, D_400AA28, 4, 0)) built
                       inside a 3D billboard positioned by a G_MTX load right before it.
                       Dump ob/tc for every 4-vertex load seen while a race is active so
                       the countdown draw's raw pointer (captured decomp-side via
                       gdx_cki("[countdown] ...")) can be grepped out of this same log to
                       see whether its object-space rect is sane (small ob magnitude,
                       tc spanning the 32x32 texture) or garbage/degenerate (all-zero,
                       NaN-adjacent, or wildly out of range). */
                    {
                        static const bool sDiagCountdown = std::getenv("GDX_DIAG_COUNTDOWN") != nullptr;
                        // Gated on the arm flag, not gGdxRaceActive: the blanket race
                        // gate let ~60 unrelated 4-vertex HUD quads (portraits, other
                        // sprites) exhaust the log cap minutes before the countdown
                        // ever ran, so this probe never actually captured the quad it
                        // was written for. Arming near the real event fixes that.
                        if (sDiagCountdown && gGdxCountdownProbeArm != 0 && !isF3DSource &&
                            (((in.w0 >> 12) & 0xFFu) == 4u) && outW1 != 0) {
                            static int sRectLogs = 0;
                            if (sRectLogs < 200) {
                                ++sRectLogs;
                                // Read the raw N64 Vtx_t layout by hand (ob[3] s16, flag u16,
                                // tc[2] s16, cn[4] u8 = 16 bytes/vertex) -- same convention
                                // MakePersistentVtxCopy already uses -- rather than depending
                                // on a Vtx_t type possibly not visible/ODR-safe in this TU.
                                const uint8_t* base = reinterpret_cast<const uint8_t*>(outW1);
                                const auto ob = [&](int v, int c) {
                                    int16_t x;
                                    std::memcpy(&x, base + v * 16 + c * 2, sizeof(x));
                                    return x;
                                };
                                const auto tc = [&](int v, int c) {
                                    int16_t x;
                                    std::memcpy(&x, base + v * 16 + 8 + c * 2, sizeof(x));
                                    return x;
                                };
                                gdx_port_logf("[rect] raw=%08X resolved=%p "
                                              "v0=(%d,%d,%d tc=%d,%d) v1=(%d,%d,%d tc=%d,%d) "
                                              "v2=(%d,%d,%d tc=%d,%d) v3=(%d,%d,%d tc=%d,%d)\n",
                                              in.w1, reinterpret_cast<void*>(outW1),
                                              ob(0,0), ob(0,1), ob(0,2), tc(0,0), tc(0,1),
                                              ob(1,0), ob(1,1), ob(1,2), tc(1,0), tc(1,1),
                                              ob(2,0), ob(2,1), ob(2,2), tc(2,0), tc(2,1),
                                              ob(3,0), ob(3,1), ob(3,2), tc(3,0), tc(3,1));
                            }
                        }
                    }
                    break;

                case kOpMtx:
                    outW1 = w1IsHostPointer
                                ? w1full
                                : TranslateDataPointer(in.w1, 64, /*preferPhysical=*/!isF3DSource,
                                                 reinterpret_cast<uintptr_t>(item.source));
                    if ((outW1 & 7u) != 0) {
                        outW1 = 0;
                    }
                    {
                        // Matrix resolution probe (#3): unlike the vertex paths above,
                        // this call already requires the full 64-byte Mtx to be
                        // readable, so a dropped resolution here (rather than a
                        // garbage read) is the expected failure mode. Log it so a
                        // human tester can correlate dropped machine matrices with
                        // observed z-fighting/punch-through during races.
                        // Always on (2026-07-08): see the [vtx-spike]/[vtx-dropped] note
                        // above -- GDX_DIAG_VTX required manual setup the user never did.
                        if (gGdxRaceActive != 0 && outW1 == 0 && in.w1 != 0) {
                            static int sMtxDroppedLogs = 0;
                            if (sMtxDroppedLogs < 40) {
                                ++sMtxDroppedLogs;
                                gdx_port_logf("[mtx-dropped] raw=%08X f3d=%d big=%d src=%p\n",
                                              in.w1, (int)isF3DSource, (int)isBig, item.source);
                            }
                        }
                    }
                    if (outW1 != 0) {
                        /* Same byte-order proxy as the G_VTX paths: matrices
                           referenced from big-endian DLs are static asset Mtx
                           data and need word-swapping for the interpreter's
                           host-order reads; host-built DLs reference
                           Matrix_ToMtx output, which is already host-order. */
                        if (isBig) {
                            outW1 = MakePersistentMtxCopy(outW1);
                        }
                        outW1 = NormalizeLusDirectPointer(outW1);
                    }
                    /* CRASH FAILSAFE (campaign soak fix, 2026-07-08): the
                       w1IsHostPointer fast path above takes w1full verbatim with
                       no readability check, and the resolver path can return an
                       under-validated candidate. A matrix load dereferences 64
                       bytes in the interpreter, so drop any matrix pointer whose
                       full 64 bytes are not readable (0 is the established
                       dropped-matrix convention the interpreter tolerates). */
                    if (outW1 != 0 && ReadableByteLimit(outW1) < 64u) {
                        static int sMtxFailsafeLogs = 0;
                        if (sMtxFailsafeLogs < 40) {
                            ++sMtxFailsafeLogs;
                            gdx_port_logf("[mtx-failsafe] raw=%08X resolved=%p readable=%zuB "
                                          "src=%p -> dropped\n",
                                          in.w1, reinterpret_cast<void*>(outW1),
                                          ReadableByteLimit(outW1), item.source);
                        }
                        outW1 = 0u;
                    }
                    // #16 countdown-matrix content probe: dump the resolved 64-byte
                    // Mtx as raw hex words, armed on the same flag as the [rect]
                    // probe, to sanity-check the countdown billboard's modelview
                    // matrix for degenerate (all-zero) or wildly-out-of-range values
                    // instead of just confirming the pointer resolved.
                    {
                        static const bool sDiagCountdownMtx = std::getenv("GDX_DIAG_COUNTDOWN") != nullptr;
                        if (sDiagCountdownMtx && gGdxCountdownProbeArm != 0 && outW1 != 0) {
                            static int sMtxContentLogs = 0;
                            if (sMtxContentLogs < 200) {
                                ++sMtxContentLogs;
                                const uint32_t* w = reinterpret_cast<const uint32_t*>(outW1);
                                gdx_port_logf("[mtx-content] raw=%08X resolved=%p "
                                              "%08X %08X %08X %08X %08X %08X %08X %08X "
                                              "%08X %08X %08X %08X %08X %08X %08X %08X\n",
                                              in.w1, reinterpret_cast<void*>(outW1),
                                              w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
                                              w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
                            }
                        }
                    }
                    break;

                case kOpMovemem:
                    outW1 = w1IsHostPointer ? w1full : TranslateDataPointer(in.w1);
                    if (outW1 != 0) {
                        outW1 = NormalizeLusDirectPointer(outW1);
                    }
                    break;

                case kOpSetColorImage:
                case kOpSetDepthImage:
                    outW1 = w1IsHostPointer ? w1full : TranslateDataPointer(in.w1);
                    if (outW1 == 0) outW1 = MakeFramebufferToken(in.w1);
                    if (op == kOpSetColorImage && outW1 != 0 && mStats != nullptr) {
                        bool alreadySeen = false;
                        for (size_t si = 0; si < mStats->colorImageTargetCount; si++) {
                            if (mStats->colorImageTargets[si] == outW1) {
                                alreadySeen = true;
                                break;
                            }
                        }
                        if (!alreadySeen && mStats->colorImageTargetCount < mStats->colorImageTargets.size()) {
                            mStats->colorImageTargets[mStats->colorImageTargetCount++] = outW1;
                        }
                    }
                    break;

                case kOpSetTextureImage:
                    {
                        const uintptr_t translated =
                            w1IsHostPointer ? w1full : TranslateDataPointer(in.w1);
                        // Only emit the O2R filepath opcode for BSS-stub textures (asset-segment
                        // symbols with a 1:1 O2R resource). RDRAM-backed textures are contiguous
                        // multi-tile buffers where the game issues many G_LOADBLOCKs with
                        // increasing ULS offsets across the full region — the O2R resource only
                        // covers the first tile and causes out-of-bounds reads for later bands.
                        // Those textures must go through the raw-copy path (1MB slice of RDRAM).
                        const char* o2rKey = (!w1IsHostPointer && translated != 0 && !IsRdramHostPointer(translated))
                                                 ? gdx_lookup_asset_segment_o2r_key(in.w1)
                                                 : nullptr;
                        const char* texCensusPath = "rawcopy"; /* [tex-census] delivery classification */
                        if (o2rKey) {
                            texCensusPath = "o2r";
                            outW0 = (outW0 & 0x00FFFFFFu) | (static_cast<uintptr_t>(kOpSetTextureImageOtrFilepath) << 24);
                            outW1 = reinterpret_cast<uintptr_t>(o2rKey);
                        } else if (w1IsHostPointer) {
                            // Phase G1: real host pointer to texel data — use directly,
                            // UNLESS it is a generated asset stub (see
                            // ResolveWideAssetStubPointer): stubs must be re-routed to
                            // the decoded asset image or the sampler reads EXE data.
                            const uintptr_t stubResolved =
                                ResolveWideAssetStubPointer(w1full, mModuleBegin, mModuleEnd);
                            // Leftover census: a module-range texture pointer that BOTH
                            // stub resolvers miss is almost certainly an unbound stub
                            // (LinkStubs symbol with no AssetBindings entry / venue bank
                            // beyond the alias table) — i.e., the next visual-garbage
                            // candidate. Enumerate them so a single soak run names every
                            // remaining broken texture source.
                            if (stubResolved == 0 && w1full >= mModuleBegin && w1full < mModuleEnd) {
                                // One line per UNIQUE address: real module-resident
                                // arrays (e.g. sCourseMinimapPalette, sampled every
                                // frame) are legitimate and must not drown the budget
                                // that should be naming actual unresolved stubs.
                                static uintptr_t sStubMissSeen[24] = {};
                                static int sStubMissCount = 0;
                                bool alreadySeen = false;
                                for (int s = 0; s < sStubMissCount; s++) {
                                    if (sStubMissSeen[s] == w1full) {
                                        alreadySeen = true;
                                        break;
                                    }
                                }
                                if (!alreadySeen && sStubMissCount < 24) {
                                    sStubMissSeen[sStubMissCount++] = w1full;
                                    /* Emit the ASLR-stable identity too: the low32
                                       alone is useless against the .map because the
                                       base is randomized each run. modOff/prefVA
                                       (imageBase + moduleOffset) resolve to the exact
                                       symbol deterministically -- e.g. this run's
                                       E3BAE880 -> prefVA 0x14134E880 =
                                       sMachineWeightDigitCompTexInfos+0x420, a real
                                       module array for which verbatim is by design. */
                                    const uintptr_t modOff = w1full - mModuleBegin;
                                    gdx_port_logf("[stub-miss] SETTIMG module ptr %p (low32=%08X) "
                                                  "modOff=%08llX prefVA=%011llX unresolved -- taken verbatim\n",
                                                  reinterpret_cast<void*>(w1full), Low32(w1full),
                                                  static_cast<unsigned long long>(modOff),
                                                  static_cast<unsigned long long>(kPreferredImageBaseVA + modOff));
                                }
                            }
                            texCensusPath = (stubResolved != 0) ? "widestub" : "hostptr";
                            outW1 = NormalizeLusDirectPointer(stubResolved != 0 ? stubResolved : w1full);
                        } else {
                            outW1 = TranslateTexturePointer(in.w1, item.source, i, item.limit, isBig, stride);
                            if (outW1 != 0) {
                                outW1 = NormalizeLusDirectPointer(outW1);
                            }
                        }
                        /* [tex-census] (2026-07-10, graphics-audit phase 4): one line
                           per UNIQUE SETTIMG source across the WHOLE session (NOT
                           race-gated -- pause menu / machine select / menus are audit
                           targets). Pairs every on-screen texture with its delivery
                           path and the first bytes LUS consumes, mirroring the audio
                           [sample-census]. Fingerprint at +0x20 to skip transparent
                           glyph corners when the buffer is large enough. */
                        {
                            /* 2026-07-10 recalibration: 160 unique slots burned out in
                               the first 8s of boot (all RGBA/I) and the pause-menu CI
                               textures -- the audit's primary target -- never logged.
                               512 general slots + a RESERVED pool for CI-format (fmt=2)
                               sources, which always log regardless of the general cap. */
                            static uint32_t sTexCensusSeen[512] = {};
                            static int sTexCensusCount = 0;
                            static uint32_t sTexCensusCiSeen[64] = {};
                            static int sTexCensusCiCount = 0;
                            const uint32_t cw0 = static_cast<uint32_t>(in.w0);
                            const uint32_t cFmt = (cw0 >> 21) & 0x7;
                            bool censusDup = false;
                            for (int tc = 0; tc < sTexCensusCount; tc++) {
                                if (sTexCensusSeen[tc] == in.w1) {
                                    censusDup = true;
                                    break;
                                }
                            }
                            for (int tc = 0; !censusDup && tc < sTexCensusCiCount; tc++) {
                                if (sTexCensusCiSeen[tc] == in.w1) {
                                    censusDup = true;
                                    break;
                                }
                            }
                            const bool censusHasBudget =
                                (sTexCensusCount < 512) ||
                                (cFmt == 2u && sTexCensusCiCount < 64);
                            if (!censusDup && censusHasBudget) {
                                if (sTexCensusCount < 512) {
                                    sTexCensusSeen[sTexCensusCount++] = in.w1;
                                } else if (cFmt == 2u && sTexCensusCiCount < 64) {
                                    sTexCensusCiSeen[sTexCensusCiCount++] = in.w1;
                                }
                                const uint32_t cSiz = (cw0 >> 19) & 0x3;
                                const uint32_t cWidth = (cw0 & 0xFFF) + 1;
                                if (o2rKey) {
                                    gdx_port_logf("[tex-census] raw=%08X path=o2r fmt=%u siz=%u w=%u key=%s\n",
                                                  in.w1, cFmt, cSiz, cWidth, o2rKey);
                                } else {
                                    uint8_t cfp[8] = {};
                                    uintptr_t fpAt = outW1;
                                    if (fpAt != 0 && ReadableByteLimit(fpAt) >= 0x28) {
                                        fpAt += 0x20;
                                    }
                                    if (fpAt != 0 && ReadableByteLimit(fpAt) >= sizeof(cfp)) {
                                        std::memcpy(cfp, reinterpret_cast<const void*>(fpAt), sizeof(cfp));
                                    }
                                    gdx_port_logf("[tex-census] raw=%08X path=%s fmt=%u siz=%u w=%u out=%p "
                                                  "fp8=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                                                  in.w1, texCensusPath, cFmt, cSiz, cWidth,
                                                  reinterpret_cast<void*>(outW1),
                                                  cfp[0], cfp[1], cfp[2], cfp[3], cfp[4], cfp[5], cfp[6], cfp[7]);
                                }
                            }
                        }
                        // Resolution-audit probe. Classifies the RESOLVED SOURCE
                        // (`translated`), not the persistent-copy output — the copy is
                        // always an unregistered heap buffer, so classifying outW1 only
                        // reports which delivery path ran, never whether the data is
                        // correct. Fingerprints the bytes LUS will actually consume and
                        // flags MIO0-compressed streams reaching the sampler (stripes).
                        static const bool sDiagSettimg = std::getenv("GDX_DIAG_SETTIMG") != nullptr;
                        if (sDiagSettimg && gGdxRaceActive != 0) {
                            static int sSettimgCount = 0;
                            if (sSettimgCount < 6000) {
                                ++sSettimgCount;
                                const auto classify = [this](uintptr_t p) -> const char* {
                                    if (p == 0) {
                                        return "NULL";
                                    }
                                    if (gdx_rdram != nullptr && p >= reinterpret_cast<uintptr_t>(gdx_rdram) &&
                                        p < reinterpret_cast<uintptr_t>(gdx_rdram) + GDX_RDRAM_SIZE) {
                                        return "rdram";
                                    }
                                    for (const LoadedAssetSegment& segImg : gLoadedAssetSegments) {
                                        const uintptr_t base = reinterpret_cast<uintptr_t>(segImg.bytes.data());
                                        if (base != 0 && p >= base && p < base + segImg.bytes.size()) {
                                            return "assetseg";
                                        }
                                    }
                                    if (p >= mModuleBegin && p < mModuleEnd) {
                                        return "module";
                                    }
                                    for (const HostRange& range : gHostRanges) {
                                        if (range.begin != 0 && p >= range.begin &&
                                            p < range.begin + range.size) {
                                            return "hostrange";
                                        }
                                    }
                                    return "OTHER";
                                };
                                const uint32_t fmt = (in.w0 >> 21) & 0x7;
                                const uint32_t siz = (in.w0 >> 19) & 0x3;
                                const uint32_t width = (in.w0 & 0xFFF) + 1;
                                uint8_t head[8] = {};
                                uint32_t sum = 0;
                                int mio0 = 0;
                                const uintptr_t fpSrc = (outW1 != 0) ? outW1 : translated;
                                const size_t fpAvail = (fpSrc != 0) ? ReadableByteLimit(fpSrc) : 0;
                                if (fpAvail >= sizeof(head)) {
                                    std::memcpy(head, reinterpret_cast<const void*>(fpSrc), sizeof(head));
                                    const size_t span = std::min<size_t>(fpAvail, 256);
                                    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(fpSrc);
                                    for (size_t b = 0; b < span; b++) {
                                        sum = sum * 31u + bytes[b];
                                    }
                                    mio0 = (std::memcmp(head, "MIO0", 4) == 0) ? 1 : 0;
                                }
                                FILE* tf = fopen("settimg-trace.txt", sSettimgCount == 1 ? "w" : "a");
                                if (tf != nullptr) {
                                    fprintf(tf,
                                            "T raw=%08X src=%p scls=%s out=%p fmt=%u siz=%u w=%u "
                                            "fp=%02X%02X%02X%02X%02X%02X%02X%02X sum=%08X mio0=%d dl=%p\n",
                                            in.w1, reinterpret_cast<void*>(translated), classify(translated),
                                            reinterpret_cast<void*>(outW1), fmt, siz, width,
                                            head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7],
                                            sum, mio0, item.source);
                                    fclose(tf);
                                }
                            }
                        }
                    }
                    break;

                case kOpDl:
                    outW1 = TranslateDisplayListPointer(
                        in.w1, item.source, i,
                        w1IsHostPointer ? reinterpret_cast<const N64Gfx*>(w1full) : nullptr);
                    break;

                case kOpMoveword:
                    if (WordParam(in.w0) == kMovewordSegmentIndex) {
                        const uint8_t segIdx = static_cast<uint8_t>((in.w0 & 0xFFFF) / 4);
                        /* Phase G1 / soak-fix-2: gSPSegment(seg, base) packs the segment
                           BASE through the WIDENED gDma1p macro (F3DEX_GBI_2 -> _GFXW1_PTR),
                           so a game-emitted wide list carries the FULL host pointer in
                           w1full. The segment table is the CENTRAL base for ALL segmented
                           addressing; reading the truncated low32 (in.w1) and running it
                           through the legacy resolver here (the [legacy-resolve]
                           reghost_low32 op=DB hits in the soak log) truncated every segment
                           base and cascaded into zero-byte texture copies (gray rects),
                           garbage vertex/matrix bases, and noop'd DL roots. Take the real
                           host pointer verbatim, resolver-free, exactly like the other
                           w1IsHostPointer pointer opcodes (VTX/MTX/MOVEMEM/DL/SETTIMG). */
                        uintptr_t translated;
                        if (w1IsHostPointer) {
                            translated = w1full;
                        } else {
                            translated = TranslateDataPointer(in.w1);
                            if (translated == 0 && in.w1 == 0 && gdx_rdram != nullptr) {
                                translated = reinterpret_cast<uintptr_t>(gdx_rdram);
                            }
                        }
                        if (translated != 0 && segIdx < kGfxSegmentCount) {
                            const uintptr_t normalized = NormalizeLusDirectPointer(translated);
                            /* In-DL segment repoints were invisible in the log (only the
                               CPU-side setters print [seg] lines), which made "element
                               draws through an unset/stale segment" bugs undiagnosable.
                               Log the first few repoints of EVERY segment. */
                            if (gSegments[segIdx] != normalized) {
                                static int sSegRepointLogs[kGfxSegmentCount] = {};
                                if (sSegRepointLogs[segIdx] < 6) {
                                    ++sSegRepointLogs[segIdx];
                                    gdx_port_logf("[seg-dl] moveword seg=%X raw=%08X %p -> %p\n", segIdx, in.w1,
                                                  reinterpret_cast<void*>(gSegments[segIdx]),
                                                  reinterpret_cast<void*>(normalized));
                                }
                                /* Early-race floor probe (2026-07-09, defect A): course-select's
                                   thumbnail carousel alone burns the 6-slot sSegRepointLogs[0x0A]
                                   budget above (5 venue-cycle repoints + 1 boot-time repoint,
                                   confirmed in the 21:10 soak log) before the race is ever entered,
                                   so any repoint of the venue-bank segment (0x0A) DURING the first
                                   race frames is invisible. That segment is what
                                   ResolveVenueBankAlias (n64_gfx_bridge.cpp above) resolves
                                   course.c's road/wall material stubs through, and
                                   MakePersistentRawTextureCopy keys its cache on the resolved
                                   `source` pointer -- if gSegments[0x0A] is transiently wrong for
                                   the opening frames (e.g. still holding the course-select
                                   thumbnail's RDRAM-mirror target) that would produce a
                                   first-created cache entry from the WRONG source (garbage), and
                                   once the segment settles to the correct venue base a NEW,
                                   correctly-sourced cache entry takes over -- a "self heal" that
                                   is not staleness re-detection but a different cache key. This
                                   probe is scoped to the race window and keyed independently of
                                   the budget above so it survives past course-select; look for
                                   "[seg-dl-race] seg=A" lines and compare their timestamps against
                                   the observed garbage-to-correct transition (~13s to ~22s of race
                                   time) on the next soak run. */
                                if (segIdx == 0x0A && gGdxRaceActive != 0) {
                                    static int sSegARaceRepointLogs = 0;
                                    if (sSegARaceRepointLogs < 32) {
                                        ++sSegARaceRepointLogs;
                                        gdx_port_logf(
                                            "[seg-dl-race] seg=A raw=%08X %p -> %p (race-scoped, unbudgeted by "
                                            "course-select)\n",
                                            in.w1, reinterpret_cast<void*>(gSegments[segIdx]),
                                            reinterpret_cast<void*>(normalized));
                                    }
                                }
                            }
                            gSegments[segIdx] = normalized;
                        } else if (in.w1 != 0 && segIdx < kGfxSegmentCount) {
                            /* A segment repoint we could NOT translate: the segment keeps
                               its stale base and everything drawn through it afterwards is
                               missing or garbage. This must never be silent. */
                            static int sSegFailLogs = 0;
                            if (sSegFailLogs < 24) {
                                ++sSegFailLogs;
                                gdx_port_logf("[seg-dl] moveword FAILED seg=%X raw=%08X (stale base %p kept)\n",
                                              segIdx, in.w1, reinterpret_cast<void*>(gSegments[segIdx]));
                            }
                        }
                        outW1 = (segIdx < kGfxSegmentCount) ? gSegments[segIdx] : static_cast<uintptr_t>(in.w1);
                    }
                    break;

                case kOpBranchZF3D:
                    if (isF3DSource) {
                        // Legacy F3D uses 0xD6 for G_BRANCH_Z. Its target/condition
                        // encoding is not compatible with F3DEX2 G_DMA_IO.
                        continue;
                    }
                    // F3DEX2 uses 0xD6 for G_DMA_IO. F3DFLX loads its per-vertex
                    // reflection-alpha lookup table through this command.
                    outW1 = w1IsHostPointer ? w1full : TranslateDataPointer(in.w1);
                    if (outW1 == 0) {
                        if (mStats != nullptr) {
                            mStats->skippedDataCommands++;
                        }
                        continue;
                    }
                    outW1 = NormalizeLusDirectPointer(outW1);
                    break;

                case kOpRdpHalf1:
                    /* G_RDPHALF_1 is also used as raw RDP payload for commands like
                       G_TEXRECT.  Do not blindly treat its w1 as a G_BRANCH_Z display-list
                       pointer just because the following command byte is 0x04; in older F3D
                       GBIs 0x04 is G_VTX, and texture-rectangle payloads can otherwise be
                       corrupted into no-op display-list pointers. */
                    if ((i + 1 < item.limit) &&
                        (Opcode(ReadCommand(item.source, i + 1, stride, isBig).w0) == kOpBranchZ) &&
                        (w1IsHostPointer || IsResolvableDisplayList(in.w1))) {
                        outW1 = TranslateDisplayListPointer(
                            in.w1, item.source, i,
                            w1IsHostPointer ? reinterpret_cast<const N64Gfx*>(w1full) : nullptr);
                    }
                    break;

                case 0xDD: {
                    /*
                     * Physical G_LOAD_UCODE packets encode a data size in w0 and
                     * a truncated host symbol in w1. Convert known F-Zero X
                     * F3DEX2-family loads into a semantic variant switch that
                     * Libultraship can consume without treating the size as a
                     * UcodeHandlers enum.
                     */
                    /* The game emits this pointer through different VA->PA
                       transforms (identity, & 0x1FFFFFFF, - 0x80000000)
                       depending on the call site — and the symbols' truncated
                       host addresses move every build. Compare within the
                       512MB physical window so recognition is layout- and
                       transform-independent (an exact-low32 match here once
                       broke per-build: machine select lost every model).
                       Graphics wave W1: when the packet is WIDE and carries a
                       full host pointer (w1IsHostPointer), match the FULL
                       pointer exactly against the stub symbol first — that is
                       unambiguous. The low29 window stays as the only test for
                       narrow/truncated sources. */
                    const auto matchesUcodeText = [raw = in.w1, w1IsHostPointer,
                                                   w1full](const void* symbol) {
                        const uintptr_t symbolAddr = reinterpret_cast<uintptr_t>(symbol);
                        if (w1IsHostPointer) {
                            return w1full == symbolAddr;
                        }
                        const uint32_t symbolLow = Low32(symbolAddr);
                        return ((raw ^ symbolLow) & 0x1FFFFFFFu) == 0;
                    };

                    Fast::F3dex2Variant variant;
                    if (matchesUcodeText(gspF3DEX2_fifoTextStart)) {
                        variant = Fast::F3dex2Variant::Standard;
                    } else if (matchesUcodeText(gspF3DLX2_Rej_fifoTextStart)) {
                        variant = Fast::F3dex2Variant::Reject;
                    } else if (matchesUcodeText(gspF3DEX2_Rej_fifoTextStart)) {
                        /* EK menus load plain F3DEX2.Rej (distinct from
                           F3DLX2.Rej). Same reject-box semantics for the
                           interpreter — without this arm the load was dropped
                           and reject screening never engaged on those screens
                           (log signature: ucode_unknown=1 every menu frame). */
                        variant = Fast::F3dex2Variant::Reject;
                    } else if (matchesUcodeText(gspF3DFLX2_Rej_fifoTextStart)) {
                        variant = Fast::F3dex2Variant::FZeroFlxReject;
                    } else if (matchesUcodeText(gspL3DEX2_fifoTextStart)) {
                        /* L3DEX2 line microcode (Course Edit track lines, menu
                           track previews): its command semantics have no
                           interpreter support, and running them through F3DEX2
                           produced garbage frames (Course Edit flicker). Skip
                           its section until a supported microcode is loaded
                           again. Counted in l3dexUcodeSkips (NOT
                           unknownUcodeSwitches): this skip is deliberate and
                           benign; sharing the unknown counter masked genuine
                           match failures (graphics wave W1 ambiguity, resolved:
                           the per-menu-frame unknown=1 raw=AA96A670 was
                           low32(gspL3DEX2_fifoTextStart), i.e. this skip). */
                        skipUnsupportedUcode = true;
                        if (mStats != nullptr) {
                            mStats->l3dexUcodeSkips++;
                            if (mStats->firstL3dexUcodeRaw == 0) {
                                mStats->firstL3dexUcodeRaw = in.w1;
                            }
                        }
                        continue;
                    } else {
                        /* Genuinely unrecognized load: drop only the load
                           itself (never engage the skip — a false positive
                           here would silently eat the rest of the frame). */
                        if (mStats != nullptr) {
                            mStats->unknownUcodeSwitches++;
                            if (mStats->firstUnknownUcodeRaw == 0) {
                                mStats->firstUnknownUcodeRaw = in.w1;
                            }
                        }
                        continue;
                    }
                    skipUnsupportedUcode = false;

                    if (mStats != nullptr) {
                        mStats->ucodeSwitches++;
                    }
                    outW0 = (static_cast<uintptr_t>(0xDDu) << 24) |
                            static_cast<uintptr_t>(ucode_f3dex2);
                    outW1 = Fast::F3DEX2_VARIANT_SWITCH_MARKER |
                            static_cast<uintptr_t>(variant);
                    break;
                }

                // F3D G_ENDDL (0xB8): stop list processing, emit F3DEX2 ENDDL so Fast3D sees a clean end.
                case 0xB8:
                    outW0 = static_cast<uintptr_t>(kOpEndDl) << 24;
                    outW1 = 0;
                    item.listPtr->commands.push_back(MakeLusGfx(outW0, outW1));
                    if (mStats != nullptr) mStats->commandsOut++;
                    return;

                // F3D G_DL (0x06): sub-display-list call — same layout as F3DEX2 G_DL (0xDE).
                // GUARD: F3DEX2 uses 0x06 for G_TRI2; only remap in F3D asset DLs.
                case 0x06:
                    if (isF3DSource) {
                        outW0 = (outW0 & 0x00FFFFFFu) | (static_cast<uintptr_t>(kOpDl) << 24);
                        outW1 = TranslateDisplayListPointer(in.w1, item.source, i);
                    }
                    break;

                // F3D G_TRI1 (0xBF): w1 stores three vertex indices multiplied by 10.
                // F3DEX2 G_TRI1 stores the indices multiplied by 2 in w0[23:0].
                // GUARD: F3DEX2 uses 0xBF for G_CULLDL; only convert in F3D asset DLs.
                case 0xBF:
                    if (isF3DSource) {
                        const uint8_t v0 = static_cast<uint8_t>((in.w1 >> 16) & 0xFFu) / 10u;
                        const uint8_t v1 = static_cast<uint8_t>((in.w1 >> 8) & 0xFFu) / 10u;
                        const uint8_t v2 = static_cast<uint8_t>(in.w1 & 0xFFu) / 10u;
                        outW0 = (static_cast<uintptr_t>(0x05u) << 24) |
                                (static_cast<uintptr_t>(v0 * 2u) << 16) |
                                (static_cast<uintptr_t>(v1 * 2u) << 8) |
                                static_cast<uintptr_t>(v2 * 2u);
                        outW1 = 0;
                    }
                    break;

                /* F3D G_MOVEMEM (0x03): in F3DEX2, 0x03 = G_CULLDL — cannot pass through.
                   Legacy gDma1p stores the target in w0[23:16] and byte count in w0[15:0].
                   Translate viewport, look-at, and light slots to the F3DEX2 G_MV_LIGHT
                   index/offset layout used by Libultraship. */
                case 0x03:
                    if (isF3DSource) {
                        const uint8_t legacyIndex = static_cast<uint8_t>((in.w0 >> 16) & 0xFFu);
                        const size_t xferSize = static_cast<size_t>(in.w0 & 0xFFFFu);
                        uint8_t index = 0;
                        uint8_t offset = 0;

                        if (legacyIndex == 0x80u) {
                            index = 8u; // G_MV_VIEWPORT
                        } else if (legacyIndex == 0x84u) {
                            index = 10u; // G_MV_LIGHT / G_MVO_LOOKATX
                            offset = 0u;
                        } else if (legacyIndex == 0x82u) {
                            index = 10u; // G_MV_LIGHT / G_MVO_LOOKATY
                            offset = 24u;
                        } else if (legacyIndex >= 0x86u && legacyIndex <= 0x94u &&
                                   ((legacyIndex - 0x86u) & 1u) == 0) {
                            index = 10u; // G_MV_LIGHT
                            offset = static_cast<uint8_t>(
                                48u + ((legacyIndex - 0x86u) / 2u) * 24u);
                        } else {
                            continue;
                        }

                        uintptr_t addr = TranslateDataPointer(in.w1, std::max<size_t>(xferSize, 1u));
                        if (addr == 0) {
                            addr = FallbackDataPointer(kOpMovemem, in.w1);
                        }
                        if (addr == 0) {
                            if (mStats != nullptr) mStats->skippedDataCommands++;
                            continue;
                        }
                        addr = NormalizeLusDirectPointer(addr);

                        /* The F3DEX2 handler consumes index from low 8 bits and
                           offset in units of eight bytes from bits 15:8. */
                        outW0 = (static_cast<uintptr_t>(kOpMovemem) << 24) |
                                (static_cast<uintptr_t>(offset / 8u) << 8) |
                                static_cast<uintptr_t>(index);
                        outW1 = addr;
                    }
                    break;

                /* F3D G_POPMTX (0xBD) → F3DEX2 G_POPMTX (0xD8).
                   F3DEX2 encodes the pop count as n*sizeof(Mtx) in w1; F3D uses 0. */
                case 0xBD:
                    if (isF3DSource) {
                        outW0 = static_cast<uintptr_t>(0xD8u) << 24;
                        outW1 = 64u; /* sizeof(Mtx) — pop 1 matrix */
                    }
                    break;

                /* F3D G_MOVEWORD (0xBC) → F3DEX2 G_MOVEWORD (0xDB).
                   Same word layout (index in w0[23:16], offset in w0[15:0], data in w1).
                   Handle segment-table writes identically to case kOpMoveword above. */
                case 0xBC:
                    if (isF3DSource) {
                        outW0 = (outW0 & 0x00FFFFFFu) | (static_cast<uintptr_t>(kOpMoveword) << 24);
                        if (WordParam(in.w0) == kMovewordSegmentIndex) {
                            const uint8_t segIdx = static_cast<uint8_t>((in.w0 & 0xFFFF) / 4);
                            /* Same full-width segment base as kOpMoveword above: a converted
                               wide F3D list (Phase G2) may commit a genuine >4GB host base to
                               w1full, so honor w1IsHostPointer before falling back to the
                               narrow-token resolver. */
                            uintptr_t translated;
                            if (w1IsHostPointer) {
                                translated = w1full;
                            } else {
                                translated = TranslateDataPointer(in.w1);
                                if (translated == 0 && in.w1 == 0 && gdx_rdram != nullptr) {
                                    translated = reinterpret_cast<uintptr_t>(gdx_rdram);
                                }
                            }
                            if (translated != 0 && segIdx < kGfxSegmentCount) {
                                gSegments[segIdx] = NormalizeLusDirectPointer(translated);
                            }
                            outW1 = (segIdx < kGfxSegmentCount) ? gSegments[segIdx]
                                                                 : static_cast<uintptr_t>(in.w1);
                        }
                    }
                    break;

                /* F3D G_TEXTURE (0xBB) → F3DEX2 G_TEXTURE (0xD7).  Same word layout. */
                case 0xBB:
                    if (isF3DSource) {
                        outW0 = (outW0 & 0x00FFFFFFu) | (static_cast<uintptr_t>(0xD7u) << 24);
                    }
                    break;

                /* F3D G_SETOTHERMODE_H (0xBA) → F3DEX2 G_SETOTHERMODE_H (0xE3).
                   F3D stores shift/length directly, while F3DEX2 stores
                   (32 - shift - length)/(length - 1). Re-encode both fields. */
                case 0xBA:
                    if (isF3DSource) {
                        const uint32_t shift = (in.w0 >> 8) & 0xFFu;
                        const uint32_t length = in.w0 & 0xFFu;
                        if ((length == 0u) || (shift + length > 32u)) {
                            continue;
                        }
                        outW0 = (static_cast<uintptr_t>(0xE3u) << 24) |
                                (static_cast<uintptr_t>(32u - shift - length) << 8) |
                                static_cast<uintptr_t>(length - 1u);
                    }
                    break;

                /* F3D G_SETOTHERMODE_L (0xB9) → F3DEX2 G_SETOTHERMODE_L (0xE2).
                   Re-encode the legacy direct shift/length fields for F3DEX2. */
                case 0xB9:
                    if (isF3DSource) {
                        const uint32_t shift = (in.w0 >> 8) & 0xFFu;
                        const uint32_t length = in.w0 & 0xFFu;
                        if ((length == 0u) || (shift + length > 32u)) {
                            continue;
                        }
                        outW0 = (static_cast<uintptr_t>(0xE2u) << 24) |
                                (static_cast<uintptr_t>(32u - shift - length) << 8) |
                                static_cast<uintptr_t>(length - 1u);
                    }
                    break;

                /* F3D G_SETGEOMETRYMODE (0xB7): OR flags into geometry mode.
                   F3DEX2 G_GEOMETRYMODE (0xD9): w0[23:0]=keep-mask, w1=set-bits.
                   Keep-mask 0xFFFFFF means keep all existing bits, then OR in w1. */
                case 0xB7:
                    if (isF3DSource) {
                        outW0 = (static_cast<uintptr_t>(0xD9u) << 24) | 0x00FFFFFFu;
                        outW1 = static_cast<uintptr_t>(in.w1);
                    }
                    break;

                /* F3D G_CLEARGEOMETRYMODE (0xB6): AND-clear flags from geometry mode.
                   F3DEX2 G_GEOMETRYMODE: keep-mask=~flags (clear exactly those bits), set=0. */
                case 0xB6:
                    if (isF3DSource) {
                        outW0 = (static_cast<uintptr_t>(0xD9u) << 24) | (~in.w1 & 0x00FFFFFFu);
                        outW1 = 0;
                    }
                    break;

                /* F3D G_CULLDL (0xBE): sub-DL conditional cull.  No F3DEX2 equivalent at 0xBE.
                   NOP — culling is an optimization, not a correctness requirement. */
                case 0xBE:
                    if (isF3DSource) {
                        continue;
                    }
                    break;

                // F3D G_VTX (0x04): only in ROM asset DLs; GfxPool F3DEX2 DLs use 0x04 for G_BRANCH_Z.
                case 0x04:
                    if (isF3DSource) {
                        // F3D: w0 = [04, par=(n-1)<<4|v0, len=sizeof(Vtx)*n]; w1 = vtx addr
                        // F3DEX2: w0 = [01, n<<12 | (v0+n)*2]; w1 = vtx addr
                        const uint16_t len = static_cast<uint16_t>(in.w0 & 0xFFFFu);
                        const uint8_t n = (len >= 16u) ? static_cast<uint8_t>(len / 16u) : 1u;
                        const uint8_t par = static_cast<uint8_t>((in.w0 >> 16) & 0xFFu);
                        const uint8_t v0 = par & 0x0Fu;
                        /* Vertex-spike root cause (#3): this path is the one the
                           original author already tagged as the source of "the
                           machine-part and decoration vertex-spike geometry" below,
                           but TranslateDataPointer was still only asked to prove 1
                           byte readable while MakePersistentVtxCopy unconditionally
                           reads n*16 bytes. Require the full payload up front so an
                           ambiguous/near-miss resolution can't slip a garbage vertex
                           into a machine model. */
                        const size_t vtxRequiredBytes = static_cast<size_t>(n) * 16u;
                        /* Same trySourceWindow() asymmetry fix as the F3DEX2 G_VTX case
                           above: pass sourceHint so a legacy-F3D vertex load can still be
                           reconstructed from its own referencing DL's window. isF3DSource
                           is always true on this path, so preferPhysical stays false
                           (unchanged) -- only sourceHint is new here. */
                        outW1 = TranslateDataPointer(in.w1, vtxRequiredBytes, /*preferPhysical=*/!isF3DSource,
                                                      reinterpret_cast<uintptr_t>(item.source));
                        // Always on (2026-07-08): see the [vtx-spike]/[vtx-dropped] note
                        // in the F3DEX2 G_VTX case above -- this legacy F3D path is the
                        // one the original author tagged as the source of machine-part
                        // and decoration vertex-spike geometry, so it especially cannot
                        // be left opt-in behind an env var the user never set.
                        if (gGdxRaceActive != 0) {
                            if (outW1 == 0) {
                                const uintptr_t loose = TranslateDataPointer(in.w1, 1);
                                static int sVtxF3DDroppedLogs = 0;
                                if (sVtxF3DDroppedLogs < 40) {
                                    ++sVtxF3DDroppedLogs;
                                    gdx_port_logf("[vtx-dropped-f3d] raw=%08X n=%u need=%zuB "
                                                  "looseResolve=%p looseReadable=%zuB src=%p\n",
                                                  in.w1, n, vtxRequiredBytes,
                                                  reinterpret_cast<void*>(loose),
                                                  loose ? ReadableByteLimit(loose) : 0u, item.source);
                                }
                            }
                        }
                        if (outW1 != 0) {
                            /* F3D vertex payloads share the DL's byte order, same
                               as the EX2 path above: big-endian sources need the
                               swapped persistent copy or the interpreter reads
                               byte-swapped s16 coordinates (the machine-part and
                               decoration vertex-spike geometry). */
                            if (isBig) {
                                outW1 = MakePersistentVtxCopy(outW1, n);
                            }
                            outW1 = NormalizeLusDirectPointer(outW1);
                        } else {
                            outW1 = FallbackDataPointer(kOpVtx, in.w1);
                            if (outW1 == 0) {
                                if (mStats != nullptr) mStats->skippedDataCommands++;
                                continue;
                            }
                        }
                        outW0 = (static_cast<uintptr_t>(kOpVtx) << 24) |
                                (static_cast<uintptr_t>(n) << 12) |
                                static_cast<uintptr_t>((v0 + n) * 2u);
                    }
                    break;

                default:
                    break;
            }

            if (outW1 == 0 && (op == kOpVtx || op == kOpMtx || op == kOpMovemem || op == kOpSetTextureImage)) {
                outW1 = FallbackDataPointer(op, in.w1);
                if (outW1 != 0) {
                    if (mStats != nullptr) {
                        if (mStats->fallbackDataCommands == 0) {
                            mStats->firstFallbackDataOp = op;
                            mStats->firstFallbackDataRaw = in.w1;
                            mStats->firstFallbackDataW0 = in.w0;
                            mStats->firstFallbackDataSource = reinterpret_cast<uintptr_t>(item.source);
                            mStats->firstFallbackDataIndex = i;
                        }
                        mStats->fallbackDataCommands++;
                    }
                } else {
                    if (mStats != nullptr) {
                        if (mStats->skippedDataCommands == 0) {
                            mStats->firstSkippedDataOp = op;
                            mStats->firstSkippedDataRaw = in.w1;
                            mStats->firstSkippedDataW0 = in.w0;
                        }
                        mStats->skippedDataCommands++;
                        mStats->skippedTextures++;
                    }
                    continue;
                }
            }

            item.listPtr->commands.push_back(MakeLusGfx(outW0, outW1));
            if (mStats != nullptr) {
                mStats->commandsOut++;
                if (op == kOpSetTextureImage) mStats->textureCopies++;
            }

            /* Both terminators must stop translation. Only stopping on the EX2
               terminator let lists ending in F3D 0xB8 (notably lone-0xB8 "empty
               part" DLs in segment 3) run past their end into adjacent EX2
               sub-lists while still classified F3D, converting G_TRI2 (0x06)
               commands into bogus G_DL branches (the raw=0x00000406-family
               [gdl-bad] class). */
            if (op == kOpEndDl || op == 0xB8u) return;
        }

        item.listPtr->commands.push_back(MakeLusGfx(static_cast<uintptr_t>(kOpEndDl) << 24, 0));
    }
};

} // namespace

extern "C" void gdx_register_host_range(void* ptr, size_t size) {
    if ((ptr == nullptr) || (size == 0)) return;
    gHostRanges.push_back({ reinterpret_cast<uintptr_t>(ptr), size });
}

extern "C" void gdx_register_host_n64_command_range(void* ptr, size_t size) {
    if ((ptr == nullptr) || (size == 0)) return;
    gHostN64CommandRanges.push_back({ reinterpret_cast<uintptr_t>(ptr), size });
}

extern "C" void gdx_register_host_raw_n64_range(void* ptr, size_t size) {
    if ((ptr == nullptr) || (size == 0)) return;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(ptr);
    const auto duplicate = std::find_if(
        gRawN64Ranges.begin(), gRawN64Ranges.end(),
        [begin, size](const HostRange& range) {
            return range.begin == begin && range.size == size;
        });
    if (duplicate == gRawN64Ranges.end()) {
        gRawN64Ranges.push_back({ begin, size });
    }
}

extern "C" void gdx_register_n64_address_range(unsigned int n64Begin, void* hostBegin, size_t size) {
    if ((n64Begin == 0) || (hostBegin == nullptr) || (size == 0)) return;
    const uintptr_t host = reinterpret_cast<uintptr_t>(hostBegin);
    const auto duplicate = std::find_if(
        gN64AddressRanges.begin(), gN64AddressRanges.end(),
        [n64Begin, host, size](const N64AddressRange& range) {
            return range.n64Begin == n64Begin && range.hostBegin == host && range.size == size;
        });
    if (duplicate == gN64AddressRanges.end()) {
        gN64AddressRanges.push_back({ n64Begin, host, size });
    }
}

extern "C" void gdx_register_n64_framebuffer(void* cpuAddr, unsigned int width, unsigned int height) {
    if (cpuAddr == nullptr || width == 0 || height == 0) {
        return;
    }
    const uintptr_t address = reinterpret_cast<uintptr_t>(cpuAddr);
    auto existing = std::find_if(gN64Framebuffers.begin(), gN64Framebuffers.end(),
                                 [address](const N64FramebufferInfo& info) {
                                     return info.address == address;
                                 });
    if (existing == gN64Framebuffers.end()) {
        gN64Framebuffers.push_back({ address, width, height, false });
    } else {
        existing->width = width;
        existing->height = height;
    }

    const size_t byteCount = static_cast<size_t>(width) * height * sizeof(uint16_t);
    const auto nativeRange = std::find_if(gNativeRgba16Ranges.begin(), gNativeRgba16Ranges.end(),
                                         [address](const HostRange& range) {
                                             return range.begin == address;
                                         });
    if (nativeRange == gNativeRgba16Ranges.end()) {
        gNativeRgba16Ranges.push_back({ address, byteCount });
    }
}

/* Shared RGBA5551 -> 24bpp BMP writer for one-shot diagnostic dumps
   (transition capture, VI-fallback frame). Pixels are host-order u16, top-down. */
static void DumpRgba16Bmp(const char* filename, const uint16_t* pixels, unsigned int width,
                          unsigned int height) {
    std::FILE* f = std::fopen(filename, "wb");
    if (f == nullptr) {
        gdx_port_logf("[dump] BMP write failed: %s\n", filename);
        return;
    }
    const unsigned int rowBytes = (width * 3u + 3u) & ~3u;
    const unsigned int imageBytes = rowBytes * height;
    const unsigned int fileBytes = 14u + 40u + imageBytes;
    unsigned char header[54] = { 0 };
    header[0] = 'B'; header[1] = 'M';
    header[2] = static_cast<unsigned char>(fileBytes);
    header[3] = static_cast<unsigned char>(fileBytes >> 8);
    header[4] = static_cast<unsigned char>(fileBytes >> 16);
    header[5] = static_cast<unsigned char>(fileBytes >> 24);
    header[10] = 54;
    header[14] = 40;
    header[18] = static_cast<unsigned char>(width);
    header[19] = static_cast<unsigned char>(width >> 8);
    header[20] = static_cast<unsigned char>(width >> 16);
    header[21] = static_cast<unsigned char>(width >> 24);
    header[22] = static_cast<unsigned char>(height);
    header[23] = static_cast<unsigned char>(height >> 8);
    header[24] = static_cast<unsigned char>(height >> 16);
    header[25] = static_cast<unsigned char>(height >> 24);
    header[26] = 1;
    header[28] = 24;
    std::fwrite(header, 1, sizeof(header), f);
    std::vector<unsigned char> row(rowBytes, 0);
    for (unsigned int y = 0; y < height; y++) {
        const uint16_t* srcRow = pixels + static_cast<size_t>(height - 1 - y) * width;
        for (unsigned int x = 0; x < width; x++) {
            const uint16_t p = srcRow[x];
            const unsigned char r5 = (p >> 11) & 0x1F;
            const unsigned char g5 = (p >> 6) & 0x1F;
            const unsigned char b5 = (p >> 1) & 0x1F;
            row[x * 3 + 2] = static_cast<unsigned char>((r5 << 3) | (r5 >> 2));
            row[x * 3 + 1] = static_cast<unsigned char>((g5 << 3) | (g5 >> 2));
            row[x * 3 + 0] = static_cast<unsigned char>((b5 << 3) | (b5 >> 2));
        }
        std::fwrite(row.data(), 1, rowBytes, f);
    }
    std::fclose(f);
}

extern "C" void gdx_vi_set_next_framebuffer(void* cpuAddr) {
    gViNextFramebuffer = reinterpret_cast<uintptr_t>(cpuAddr);
}

extern "C" void gdx_vi_set_current_framebuffer(void* cpuAddr) {
    gViCurrentFramebuffer = reinterpret_cast<uintptr_t>(cpuAddr);
}

/* Shared RGBA5551 fullscreen compositor (framebuffer coherence).
 *
 * Uploads a 320x240 RGBA5551 CPU framebuffer as one host RGBA32 texture and draws
 * a single copy-mode rectangle over the whole screen. The caller MUST have already
 * established the frame (rapi StartFrame + StartDrawToFramebuffer + ClearFramebuffer).
 * Used by BOTH the VI-scanout fallback below (whole-frame present when no task ran)
 * and the boot-logo seed hook (background drawn under a task's content). Setting
 * loaded_texture directly (as GfxDpImageRectangle does for backgrounds) uploads all
 * 320x240 in one UploadTexture with no TMEM strip loop. */
static void SeedFramebufferQuad(Fast::Interpreter* interp, const uint16_t* srcPixels) {
    constexpr uint32_t kFbW = 320; // SCREEN_WIDTH  (decomp/include/macros.h)
    constexpr uint32_t kFbH = 240; // SCREEN_HEIGHT
    constexpr uint32_t kPixels = kFbW * kFbH;

    static uint8_t sConverted[kPixels * 4];
    gdx_convert_rgba5551_to_rgba8888(srcPixels, sConverted, kPixels);

    // Source-content probe: the seed/VI quad can only show what the CPU put in
    // the framebuffer. "Boot logo invisible with seeding enabled" needs this
    // one number to split loader-side (count==0: the game never blitted the
    // logo — port feature gap in the IPL/logo path) from renderer-side
    // (count>0: the quad draw itself is broken).
    {
        static int sSeedContentLogs = 0;
        if (sSeedContentLogs < 10) {
            ++sSeedContentLogs;
            size_t nonZero = 0;
            size_t nonBackground = 0; // pixels != 0x0001 (the RGBA5551 cleared-black
                                      // value): distinguishes real blitted content
                                      // (boot logo) from a bare clear — "all nonzero"
                                      // alone cannot (0x0001 counts as nonzero).
            for (size_t k = 0; k < kPixels; k++) {
                if (srcPixels[k] != 0) {
                    ++nonZero;
                }
                if (srcPixels[k] != 0 && srcPixels[k] != 0x0001) {
                    ++nonBackground;
                }
            }
            gdx_port_logf("[seed] quad source %p nonzero=%zu nonbg=%zu/%u\n",
                          reinterpret_cast<const void*>(srcPixels), nonZero, nonBackground, kPixels);
        }
    }

    interp->mRdp->viewport_or_scissor_changed = true;
    interp->mRenderingState.viewport = {};
    interp->mRenderingState.scissor = {};

    // Boot-phase frames run before the game ever sets an RDP scissor, leaving
    // it 0x0 ([gpustate] sc=0.0,0.0 0.0x0.0) — the interpreter clips every
    // rectangle against it, so the quad was drawn and fully scissored away
    // (vifallback-frame.bmp: solid black while the source FB held the logo).
    // Establish the full-screen scissor this draw needs; coords are 10.2 fixed.
    interp->GfxDpSetScissor(0 /*G_SC_NON_INTERLACE*/, 0, 0, kFbW << 2, kFbH << 2);

    constexpr int kTile = 0;
    auto& tile = interp->mRdp->texture_tile[kTile];
    tile.fmt = G_IM_FMT_RGBA;
    tile.siz = G_IM_SIZ_32b;
    tile.cms = G_TX_CLAMP;
    tile.cmt = G_TX_CLAMP;
    tile.masks = G_TX_NOMASK;
    tile.maskt = G_TX_NOMASK;
    tile.shifts = 0;
    tile.shiftt = 0;
    tile.uls = 0.0f;
    tile.ult = 0.0f;
    tile.lrs = static_cast<float>((kFbW - 1) * 4);
    tile.lrt = static_cast<float>((kFbH - 1) * 4);
    tile.tmem = 0;
    tile.tmem_index = 0;
    tile.palette = 0;
    // Nonzero to pass ImportTexture's zero-line guard; the draw derives width/height
    // from loaded_texture's line/size below.
    tile.line_size_bytes = kFbW * 2;

    Fast::LoadedTexture& loaded = interp->mRdp->loaded_texture[0];
    loaded = Fast::LoadedTexture{};
    loaded.addr = sConverted;
    loaded.orig_size_bytes = kPixels * 4;
    loaded.size_bytes = kPixels * 4;
    loaded.full_image_line_size_bytes = kFbW * 4;
    loaded.line_size_bytes = kFbW * 4;
    loaded.tex_flags = 0;
    loaded.masked = false;
    loaded.blended = false;

    interp->mRdp->first_tile_index = kTile;
    interp->mRdp->textures_changed[0] = true;
    interp->mRdp->textures_changed[1] = true;

    // The converted buffer lives at a fixed address, so evict any cache entry from a
    // previous present — otherwise ImportTexture would serve a stale upload and a
    // CPU-animated screen would freeze.
    interp->TextureCacheDelete(sConverted);

    // Copy cycle: GfxDpTextureRectangle auto-applies a TEXEL0 passthrough combine and
    // point filtering (dsdx=0x0400 = 1 texel/pixel, so 320 texels -> 320px).
    interp->GfxSpSetOtherMode(G_MDSFT_CYCLETYPE + 32, 2, static_cast<uint64_t>(G_CYC_COPY) << 32);
    interp->GfxDpTextureRectangle(0, 0, (kFbW - 1) << G_TEXTURE_IMAGE_FRAC, (kFbH - 1) << G_TEXTURE_IMAGE_FRAC,
                                  kTile, 0, 0, 0x0400, 0x0400, false);
    interp->Flush();
}

extern "C" int gGameMode; // decomp global; GET_MODE = (gGameMode & 0x1F), GAMEMODE_TITLE == 0

// Persistent framebuffer holding a copy of the most recently completed game
// frame. Written at the end of every gdx_gfx_run task AND at the end of the
// VI-scanout fallback presenter (gdx_vi_present_fallback), so a transition
// snapshot taken during/after a boot-phase VI-fallback frame still sees a
// fresh mirror instead of a stale/empty one. Read by
// gdx_read_current_framebuffer (the game's transition snapshot). Declared
// this early (rather than immediately above gdx_gfx_run) so the VI-fallback
// presenter, which runs first in file order, can also write it.
static int gFrameMirrorFb = -1;
static bool gFrameMirrorValid = false;

// Shared epilogue for gdx_gfx_run and gdx_vi_present_fallback: refresh the persistent
// GPU-side frame mirror that transition snapshots read from (gdx_read_current_framebuffer).
// Both callers reach this point after producing a complete frame through the interpreter —
// one via a real GFX task, the other via the VI-scanout fallback quad — and the mirror
// update itself was byte-for-byte identical in both, so it now lives in one place instead
// of two copies that could silently drift. GPU->GPU copy, no CPU stall.
static void GdxUpdateFrameMirror(const std::shared_ptr<Fast::Interpreter>& interp) {
    if (gFrameMirrorFb < 0) {
        gFrameMirrorFb = interp->CreateFrameBuffer(320, 240, 320, 240, 1, false);
    }
    if (gFrameMirrorFb >= 0) {
        interp->CopyFrameBuffer(gFrameMirrorFb, 0, false, nullptr);
        gFrameMirrorValid = true;
    }
}

/* PORT boot-logo seed (framebuffer coherence, campaign-soak-fix-4).
 *
 * Registered as the interpreter's after-clear hook (Interpreter::SetPortAfterClearHook)
 * ONLY when GDX_SEED_BOOT_LOGO is enabled (see gdx_gfx_run). It runs on the
 * freshly-cleared canvas, BEFORE the frame's task commands, so the CPU-written VI
 * framebuffer (the boot logo blitted by func_806F33D0 / func_80069F5C, which no RDP
 * task renders) shows as a background UNDER the task's overlay content instead of a
 * black canvas. Gated to the boot/title phase (GAMEMODE_TITLE) so it can never
 * affect gameplay/menus. This complements the already-present GPU->CPU readback for
 * transitions (gdx_read_current_framebuffer). Left opt-in because it cannot be
 * runtime-validated without a launch; enabling it in a soak validates the logo. */
/* Graphics wave W6: the hook is now registered UNCONDITIONALLY (removing any
   registration-order/env-timing question); the opt-in gate lives here, per
   call, on a cached env check. Set by gdx_gfx_run's first-frame env probe. */
static bool gSeedBootLogoEnabled = false;

static void SeedBootLogoAfterClear(Fast::Interpreter* interp) {
    if (!gSeedBootLogoEnabled) {
        return; // Opt-in via GDX_SEED_BOOT_LOGO (see gdx_gfx_run).
    }
    if ((gGameMode & 0x1F) != 0) {
        return; // Boot/title phase only.
    }
    const uintptr_t fbAddr = gViCurrentFramebuffer;
    if (fbAddr == 0) {
        return; // No framebuffer presented yet.
    }
    SeedFramebufferQuad(interp, reinterpret_cast<const uint16_t*>(fbAddr));
}

/* VI-scanout fallback (boot-logo black screen fix, host-side).
 *
 * On the N64 the VI chip scans out whatever u16 pixels sit in the framebuffer
 * RDRAM, regardless of who wrote them. F-Zero X's boot N64/64DD logo is drawn
 * that way: sys_main.c's func_806F33D0 / func_80069F5C CPU-blit pixels straight
 * into a gFrameBuffers[] FrameBuffer (see decomp/include/gfx.h: a union whose
 * `u16 array[240][320]` view is RGBA5551) with NO RDP graphics task ever
 * submitted, then the boot code osViSwapBuffer()s that buffer and holds it on
 * screen for ~3.8s (sys_gfx.c Game_ThreadEntry) while nothing is drawn.
 *
 * This port's presentation is task-driven — everything visible comes from
 * interp->Run() rendering a parsed F3D display list — so a framebuffer nothing
 * draws through that pipeline shows only the (black) GL/D3D render target. This
 * function is the missing "VI reads raw memory" fallback.
 *
 * Design (learned from a prior failed attempt — do not regress):
 *  - It runs on the HOST/render side (called from main.cpp's frame loop after
 *    gdx_dispatch()), inside the window's already-open StartFrame/EndFrame
 *    bracket — NOT injected from a game fiber, which had no valid frame context
 *    and rendered nothing.
 *  - It is cheap: a single 320x240 texture upload + one fullscreen quad, no
 *    per-call GPU sync (no ReadFramebuffer, no extra EndFrame — the host loop's
 *    EndFrame presents), so it never stalls the audio fiber.
 *  - When a real GFX task rendered this frame (gHostFrameGfxTaskRan), it is a
 *    single boolean check and immediate return — zero cost during gameplay.
 *
 * The pixels are converted RGBA5551 -> RGBA8888 on the CPU
 * (gdx_convert_rgba5551_to_rgba8888) and uploaded as one RGBA32 texture; the
 * interpreter's own texture-rectangle path (GfxDpTextureRectangle) then draws
 * the quad, reusing its tested VBO/shader/viewport machinery. Uploading the
 * full frame in one shot (rather than the old 4 KB-TMEM strip blit) is why the
 * tile's loaded_texture fields are set directly here instead of via
 * GfxDpLoadTile — the emulated TMEM only bounds LoadTile, not import.
 */
extern "C" void gdx_vi_present_fallback(void) {
    // A real GFX task already produced this host frame: nothing to do. Clear the
    // flag for the next frame. This is the hot path once gameplay is rendering.
    if (gHostFrameGfxTaskRan) {
        gHostFrameGfxTaskRan = false;
        return;
    }

    const uintptr_t fbAddr = gViCurrentFramebuffer;
    if (fbAddr == 0) {
        return; // No framebuffer has been presented yet.
    }

    // Cache the window pointer once (same rationale as gdx_gfx_run): avoid a
    // per-frame refcount touch on the Context's window shared_ptr.
    static Fast::Fast3dWindow* sFallbackWindow = nullptr;
    if (sFallbackWindow == nullptr) {
        auto ctx = Ship::Context::GetInstance();
        if (ctx == nullptr) {
            return;
        }
        auto wnd = ctx->GetWindow();
        sFallbackWindow = static_cast<Fast::Fast3dWindow*>(wnd.get());
    }
    Fast::Fast3dWindow* fw = sFallbackWindow;
    if (fw == nullptr) {
        return;
    }
    auto interp = fw->GetInterpreterWeak().lock();
    if (!interp) {
        return;
    }
    Fast::GfxRenderingAPI* rapi = interp->GetCurrentRenderingAPI();
    if (rapi == nullptr) {
        return;
    }

    // --- Frame prologue: exactly what Interpreter::Run() establishes before a
    //     task's commands. The host's w->StartFrame() only ran interp->StartFrame()
    //     (which set mRendersToFb / framebuffer params); the rapi frame + draw
    //     target + clear are done by Run(), which is not called this frame. ---
    interp->SpReset();
    rapi->UpdateFramebufferParameters(0, interp->mGfxCurrentWindowDimensions.width,
                                      interp->mGfxCurrentWindowDimensions.height, 1, false, true, true,
                                      !interp->mRendersToFb);
    rapi->StartFrame();
    rapi->StartDrawToFramebuffer(interp->mRendersToFb ? interp->mGameFb : 0,
                                 interp->mNativeDimensions.height != 0
                                     ? static_cast<float>(interp->mCurDimensions.height) / interp->mNativeDimensions.height
                                     : 1.0f);
    rapi->ClearFramebuffer(true, true);

    // Convert + upload the VI framebuffer's CPU-written RGBA5551 pixels and draw
    // them as one fullscreen copy-mode rectangle. The pointer tracked at
    // osViSwapBuffer time is a real host pointer, read directly as u16.
    SeedFramebufferQuad(interp.get(), reinterpret_cast<const uint16_t*>(fbAddr));

    // --- Frame epilogue: same as Run(). When rendering to an offscreen game FB
    //     (resolution multiplier / MSAA), publish it for the GUI compositor. ---
    interp->mGfxFrameBuffer = 0;
    if (interp->mRendersToFb) {
        rapi->StartDrawToFramebuffer(0, 1);
        rapi->ClearFramebuffer(true, true);
        if (interp->mMsaaLevel > 1) {
            if (!interp->ViewportMatchesRendererResolution()) {
                rapi->ResolveMSAAColorBuffer(interp->mGameFbMsaaResolved, interp->mGameFb);
                interp->mGfxFrameBuffer =
                    reinterpret_cast<uintptr_t>(rapi->GetFramebufferTextureId(interp->mGameFbMsaaResolved));
            } else {
                rapi->ResolveMSAAColorBuffer(0, interp->mGameFb);
            }
        } else {
            interp->mGfxFrameBuffer = reinterpret_cast<uintptr_t>(rapi->GetFramebufferTextureId(interp->mGameFb));
        }
    }
    // The host loop's w->EndFrame() presents this frame — do NOT EndFrame here.

    static int sFallbackLogs = 0;
    if (sFallbackLogs < 8) {
        ++sFallbackLogs;
        gdx_port_logf("[vifallback] presented VI framebuffer fb=%p (%ux%u, rendersToFb=%d)\n",
                      reinterpret_cast<void*>(fbAddr), 320u, 240u, static_cast<int>(interp->mRendersToFb));
    }

    // Transition snapshot mirror: identical to the update done at the tail of
    // gdx_gfx_run. Boot-phase frames are often presented entirely through this
    // VI-scanout fallback (no GFX task runs), so without this the mirror stays
    // stale/empty until the first real task, and any transition snapshot taken
    // during/after boot reads garbage.
    GdxUpdateFrameMirror(interp);

    // Boot-logo verdict probe: dump ONE composed fallback frame (read back from
    // the mirror just updated above) to vifallback-frame.bmp. The FB source is
    // proven to hold the logo texels ([seed] nonbg=5304) yet the screen shows
    // black — this dump splits "the composed frame lacks the logo" (quad draw
    // broken) from "the frame has it" (presentation-side). Dumped on the 30th
    // fallback frame so the logo blit (a few frames in) has certainly run.
    {
        static int sFallbackFrames = 0;
        static bool sFallbackDumped = false;
        ++sFallbackFrames;
        if (!sFallbackDumped && sFallbackFrames == 30 && gFrameMirrorFb >= 0) {
            sFallbackDumped = true;
            static uint16_t sDumpPixels[320 * 240];
            interp->GetCurrentRenderingAPI()->ReadFramebufferToCPU(gFrameMirrorFb, 320, 240, sDumpPixels);
            DumpRgba16Bmp("vifallback-frame.bmp", sDumpPixels, 320, 240);
            gdx_port_logf("[vifallback] frame 30 dumped to vifallback-frame.bmp\n");
        }
    }
}

extern "C" void gdx_set_native_rgba16_texture_range(void* ptr, size_t size, int enabled) {
    const uintptr_t begin = reinterpret_cast<uintptr_t>(ptr);
    gNativeRgba16Ranges.erase(
        std::remove_if(gNativeRgba16Ranges.begin(), gNativeRgba16Ranges.end(),
                       [begin](const HostRange& range) { return range.begin == begin; }),
        gNativeRgba16Ranges.end());
    if (enabled && ptr != nullptr && size != 0) {
        gNativeRgba16Ranges.push_back({begin, size});
    }
    if (ptr != nullptr && size != 0 && IsRdramHostPointer(begin)) {
        gdx_record_dma_load(static_cast<uint32_t>(begin - reinterpret_cast<uintptr_t>(gdx_rdram)), 0,
                            static_cast<uint32_t>(std::min<size_t>(size, UINT32_MAX)));
    }
}

extern "C" void gdx_register_main_module_range(void) {
    uintptr_t moduleBegin = 0;
    uintptr_t moduleEnd = 0;
    GetMainModuleRange(moduleBegin, moduleEnd);
    if ((moduleBegin == 0) || (moduleEnd <= moduleBegin)) {
        return;
    }

    for (const HostRange& range : gHostRanges) {
        if ((range.begin == moduleBegin) && (range.size == (moduleEnd - moduleBegin))) {
            return;
        }
    }

    gHostRanges.push_back({ moduleBegin, moduleEnd - moduleBegin });
    gdx_port_logf("[bridge-init] registered EXE module range: base=%p low32=%08X size=0x%zx\n",
                  reinterpret_cast<void*>(moduleBegin),
                  static_cast<unsigned>(moduleBegin & 0xFFFFFFFFu),
                  static_cast<size_t>(moduleEnd - moduleBegin));
}

extern "C" void* gdx_ensure_asset_segment_for_symbol(unsigned int symLow32, unsigned int* outOffset) {
    uint32_t offset = 0;
    const uintptr_t base = EnsureAssetSegmentForSymbol(symLow32, &offset);
    if (outOffset != nullptr) {
        *outOffset = offset;
    }
    return reinterpret_cast<void*>(base);
}

extern "C" int gdx_load_venue_texture_segment(int venue) {
    static const void* const kVenueSegmentSymbols[] = {
        D_A000000_235130, // Mute City
        D_A000000_239A80, // Port Town
        D_A000000_23EC50, // Big Blue
        D_A000000_243D90, // Sand Ocean
        D_A000000_24A270, // Devil's Forest
        D_A000000_2507F0, // White Land
        D_A000000_255100, // Sector
        D_A000000_259600, // Red Canyon
        D_A000000_25F360, // Fire Field
        D_A000000_266C20, // Silence
        D_A000000_26D780, // Ending
    };

    if (venue < 0 || static_cast<size_t>(venue) >= std::size(kVenueSegmentSymbols)) {
        gdx_port_logf("[segment] invalid venue texture segment %d\n", venue);
        return 0;
    }

    const uint32_t symbol = Low32(reinterpret_cast<uintptr_t>(kVenueSegmentSymbols[venue]));
    uint32_t offset = 0;
    const uintptr_t base = EnsureAssetSegmentForSymbol(symbol, &offset);
    if (base == 0) {
        gdx_port_logf("[segment] failed to load venue=%d symbol=%08X\n", venue, symbol);
        return 0;
    }

    // Force segment 0x0A to point at the (decompressed) venue texture image.
    // EnsureAssetSegmentImage only claims a segment slot when it is still 0, but
    // the game sets segment 0x0A via gsSPSegment before this loads, so the slot
    // was already non-zero and kept a stale/raw pointer — the track then sampled
    // raw ROM/compressed bytes (the "stripes"). This loader is the authority for
    // the venue texture segment, so assign it unconditionally.
    gSegments[0x0A] = base;

    // gGdxRaceActive is set by the caller for race modes (decomp_port.c
    // Segment_LoadAssets) — this loader also runs for the course-select
    // preview now, and menus must not flip the race-diagnostics gate.

    // Segment_LoadAssets calls this every frame; log only on change so Debug
    // builds don't pay a file write + flush per frame.
    {
        static int sLastVenue = -1;
        static uintptr_t sLastBase = 0;
        if (venue != sLastVenue || base != sLastBase) {
            sLastVenue = venue;
            sLastBase = base;
            gdx_port_logf("[segment] loaded venue=%d segment=10 base=%p symbol=%08X offset=%08X\n",
                          venue, reinterpret_cast<void*>(base), symbol, offset);
        }
    }
    return 1;
}

extern "C" void* gdx_resolve_registered_host_address(unsigned int addr) {
    for (const HostRange& range : gHostRanges) {
        if ((range.begin == 0) || (range.size == 0)) {
            continue;
        }

        const uint32_t baseLow = Low32(range.begin);
        const uint32_t offset = addr - baseLow;
        if (offset < range.size) {
            static int sRegisteredResolveLogs = 0;
            if (sRegisteredResolveLogs < 8) {
                ++sRegisteredResolveLogs;
                gdx_port_logf("[registered-resolve] raw=%08X base=%p baseLow=%08X size=0x%zx -> %p\n",
                              addr, reinterpret_cast<void*>(range.begin), baseLow, range.size,
                              reinterpret_cast<void*>(range.begin + offset));
            }
            return reinterpret_cast<void*>(range.begin + offset);
        }
    }
    return nullptr;
}

extern "C" void* gdx_resolve_module_host_address(unsigned int addr) {
    uintptr_t moduleBegin = 0;
    uintptr_t moduleEnd = 0;
    GetMainModuleRange(moduleBegin, moduleEnd);
    if ((moduleBegin == 0) || (moduleEnd <= moduleBegin)) {
        return nullptr;
    }

    uintptr_t full = (moduleBegin & 0xFFFFFFFF00000000ULL) | static_cast<uintptr_t>(addr);
    if (full < moduleBegin) {
        full += 0x100000000ULL;
    }

    /* Match the bridge's module reconstruction rule: linker/BSS segment symbols can
       point at section boundaries that are not themselves readable, while offsets
       from that base can still land on valid display-list/data bytes. */
    if ((full >= moduleBegin) && (full < moduleEnd)) {
        static int sModuleResolveLogs = 0;
        if (sModuleResolveLogs < 8) {
            ++sModuleResolveLogs;
            gdx_port_logf("[module-resolve] raw=%08X -> %p module=[%p,%p)\n",
                          addr, reinterpret_cast<void*>(full),
                          reinterpret_cast<void*>(moduleBegin), reinterpret_cast<void*>(moduleEnd));
        }
        return reinterpret_cast<void*>(full);
    }
    return nullptr;
}

// gFrameMirrorFb / gFrameMirrorValid are declared near the top of this file
// (just after the gGameMode extern) so gdx_vi_present_fallback can also
// write them; see the comment there.

extern "C" void gdx_gfx_run(void* dl, size_t dl_size, GdxTaskUcode taskUcode) {
    // The Fast3dWindow is created once at startup and lives for the whole
    // program. Fetch it once and cache the raw pointer instead of copying the
    // Context's window shared_ptr every frame: that per-frame refcount touch
    // crashed in _Ptr_base<Window>::_Incref when the Context's window member
    // was transiently unreadable during rapid mode transitions (e.g. machine
    // select -> settings). The cache is populated at startup when state is
    // clean, so later frames never re-read that member.
    static Fast::Fast3dWindow* sCachedWindow = nullptr;
    if (sCachedWindow == nullptr) {
        auto ctx = Ship::Context::GetInstance();
        if (ctx == nullptr) { return; }
        auto wnd = ctx->GetWindow();
        sCachedWindow = static_cast<Fast::Fast3dWindow*>(wnd.get());
    }
    Fast::Fast3dWindow* fw = sCachedWindow;
    if (fw == nullptr) { return; }

    // Deep-audit M1: advance the G2 wide-conversion cache's frame counter once
    // per real GFX task and let it sweep stale entries when it has grown past
    // its watermark (see GfxWideCache::BeginFrame). gWideCache stays self-
    // contained (no logging dependency of its own, so it still builds/unit-
    // tests standalone); the bridge does the one-line log here instead.
    {
        const size_t evicted = gWideCache.BeginFrame();
        if (evicted != 0) {
            gdx_port_logf("[g2] evicted %zu stale conversions\n", evicted);
        }
    }

    // Graphics wave W6: register the boot-logo seed hook ALWAYS and gate the
    // behavior per call inside SeedBootLogoAfterClear (gSeedBootLogoEnabled).
    // The env state is logged UNCONDITIONALLY so a soak log always shows what
    // the process actually saw. GetEnvironmentVariableA is used instead of
    // std::getenv: getenv reads the CRT's startup snapshot of the environment,
    // which can miss variables in edge cases (env changed after CRT init, or a
    // launcher passing a custom environment block); the Win32 call reads the
    // live process environment directly.
    static bool sSeedHookChecked = false;
    if (!sSeedHookChecked) {
        sSeedHookChecked = true;
        char seedValue[32] = { 0 };
        bool seedPresent = false;
#ifdef _WIN32
        const DWORD seedLen =
            GetEnvironmentVariableA("GDX_SEED_BOOT_LOGO", seedValue, sizeof(seedValue));
        seedPresent = (seedLen > 0 && seedLen < sizeof(seedValue));
#else
        if (const char* seedEnv = std::getenv("GDX_SEED_BOOT_LOGO")) {
            std::snprintf(seedValue, sizeof(seedValue), "%s", seedEnv);
            seedPresent = true;
        }
#endif
        gSeedBootLogoEnabled = seedPresent && seedValue[0] != '0';
        // Shell-proof fallback: the env var route failed silently in user soak
        // runs (PowerShell `set` does not export; double-click launches carry
        // no shell env at all). A command-line switch survives every launch
        // method.
        bool seedFromArg = false;
#ifdef _WIN32
        {
            const char* cmd = GetCommandLineA();
            if (cmd != nullptr) {
                if (!gSeedBootLogoEnabled && std::strstr(cmd, "--seed-boot-logo") != nullptr) {
                    gSeedBootLogoEnabled = true;
                    seedFromArg = true;
                }
                // Forward the TEXEL1 A/B bisect switch to the interpreter's
                // getenv probe (interpreter.cpp, GDX_DIAG_TEXEL1_FROM_BASE).
                // This runs before the interpreter's first material import,
                // so the CRT env update is seen by its one-time static check.
                if (std::strstr(cmd, "--diag-texel1-base") != nullptr) {
                    _putenv("GDX_DIAG_TEXEL1_FROM_BASE=1");
                    gdx_port_logf("[seed] --diag-texel1-base: TEXEL1 forced to base tile for this run\n");
                }
                // Forward the SETTIMG race-trace probe (classifies + fingerprints
                // every resolved texture source during a race into
                // settimg-trace.txt) so it works from any launch method.
                if (std::strstr(cmd, "--diag-settimg") != nullptr) {
                    _putenv("GDX_DIAG_SETTIMG=1");
                    gdx_port_logf("[seed] --diag-settimg: SETTIMG race trace enabled\n");
                }
            }
        }
#endif
        gdx_port_logf("[seed] GDX_SEED_BOOT_LOGO=%s%s (seeding %s)\n",
                      seedPresent ? seedValue : "<unset>",
                      seedFromArg ? " arg=--seed-boot-logo" : "",
                      gSeedBootLogoEnabled ? "ENABLED" : "disabled");
        Fast::Interpreter::SetPortAfterClearHook(&SeedBootLogoAfterClear);
    }

    // All three task variants share F3DEX2 command encoding. Their semantic
    // differences are carried separately so the base opcode table stays valid.
    fw->SetRendererUCode(ucode_f3dex2);
    auto interp = fw->GetInterpreterWeak().lock();
    if (!interp) { return; }
    switch (taskUcode) {
        case GDX_TASK_UCODE_F3DLX2_REJ:
            interp->SetF3dex2Variant(Fast::F3dex2Variant::Reject);
            break;
        case GDX_TASK_UCODE_F3DFLX2_REJ:
            interp->SetF3dex2Variant(Fast::F3dex2Variant::FZeroFlxReject);
            break;
        case GDX_TASK_UCODE_F3DEX2:
        default:
            interp->SetF3dex2Variant(Fast::F3dex2Variant::Standard);
            break;
    }

    for (int i = 0; i < 16; i++) {
        interp->mSegmentPointers[i] = gSegments[i];
    }

    EnsureSetupGfxSegment();
    EnsureAssetSegmentForSymbol(Low32(reinterpret_cast<uintptr_t>(aVpFullScreen)));
    for (int i = 0; i < 16; i++) {
        interp->mSegmentPointers[i] = gSegments[i];
    }

    bool isBigEndian = IsLikelyBigEndianDisplayList(static_cast<const N64Gfx*>(dl), dl_size / sizeof(N64Gfx));

    ConversionStats stats = {};
    N64DisplayListAdapter adapter(dl, dl_size, isBigEndian, &stats);
    Fast::F3DGfx* converted = adapter.ConvertRoot();
    if (converted == nullptr) return;

    static bool sBridgeInitDiag = false;
    if (!sBridgeInitDiag) {
        sBridgeInitDiag = true;
        uintptr_t mb = 0, me = 0;
        GetMainModuleRange(mb, me);
        gdx_port_logf("[bridge-init] EXE module: base=%p end=%p size=0x%zx\n",
                      reinterpret_cast<void*>(mb), reinterpret_cast<void*>(me), static_cast<size_t>(me - mb));
        for (size_t ri = 0; ri < gHostRanges.size(); ++ri) {
            gdx_port_logf("[bridge-init] hostrange[%zu]: begin=%p low32=%08X size=0x%zx\n",
                          ri, reinterpret_cast<void*>(gHostRanges[ri].begin),
                          static_cast<unsigned>(gHostRanges[ri].begin & 0xFFFFFFFFu),
                          gHostRanges[ri].size);
        }
        gdx_port_logf("[bridge-init] DL root: ptr=%p size=%zu isBig=%d taskUcode=%d\n",
                      dl, dl_size, static_cast<int>(isBigEndian), static_cast<int>(taskUcode));
        /* Graphics wave W1: one-shot dump of every ucode stub symbol's low32 so
           any [gfxdiag] ucode_raw / ucode_l3d_raw value in this log is
           attributable to a symbol without a PDB lookup. */
        gdx_port_logf("[bridge-init] ucode stubs: F3DEX2=%08X F3DLX2_Rej=%08X "
                      "F3DEX2_Rej=%08X F3DFLX2_Rej=%08X L3DEX2=%08X\n",
                      Low32(reinterpret_cast<uintptr_t>(gspF3DEX2_fifoTextStart)),
                      Low32(reinterpret_cast<uintptr_t>(gspF3DLX2_Rej_fifoTextStart)),
                      Low32(reinterpret_cast<uintptr_t>(gspF3DEX2_Rej_fifoTextStart)),
                      Low32(reinterpret_cast<uintptr_t>(gspF3DFLX2_Rej_fifoTextStart)),
                      Low32(reinterpret_cast<uintptr_t>(gspL3DEX2_fifoTextStart)));
    }

    static uint64_t sDiagFrames = 0;
    const bool shouldLogDiagnostics =
        sDiagFrames < 8 || (sDiagFrames % 120) == 0 ||
        stats.noopDisplayLists != 0 || stats.fallbackDataCommands != 0 ||
        stats.skippedDataCommands != 0 || stats.textureCopyBytes != 0 ||
        stats.ucodeSwitches != 0 || stats.unknownUcodeSwitches != 0 ||
        stats.l3dexUcodeSkips != 0;
    if (shouldLogDiagnostics) {
        if (gdx_diag_verbose())
        gdx_port_logf("[gfxdiag] lists=%zu f3d_lists=%zu cmds=%zu noop_dl=%zu noop_raw=%08X "
                      "miss_dl=%zu miss_raw=%08X bad_dl=%zu bad_raw=%08X "
                      "fallback_data=%zu skip_data=%zu skip_tex=%zu "
                      "tex_copy_bytes=%zu vtx=%zu mtx=%zu dl=%zu teximg=%zu settile=%zu "
                      "tlut=%zu loadblk=%zu loadtile=%zu tilesize=%zu texrect=%zu fillrect=%zu "
                      "setcimg=%zu setzimg=%zu tris=%zu end=%zu "
                      "ucode_switch=%zu ucode_unknown=%zu ucode_raw=%08X "
                      "ucode_l3d_skip=%zu ucode_l3d_raw=%08X size=%zu\n",
                      stats.convertedLists, stats.f3dLists, stats.commandsOut,
                      stats.noopDisplayLists, stats.firstNoopDlRaw,
                      stats.missingDisplayLists, stats.firstMissingDlRaw,
                      stats.badDisplayLists, stats.firstBadDlRaw,
                      stats.fallbackDataCommands,
                      stats.skippedDataCommands, stats.skippedTextures, stats.textureCopyBytes, stats.opCounts[kOpVtx],
                      stats.opCounts[kOpMtx], stats.opCounts[kOpDl], stats.opCounts[kOpSetTextureImage], stats.opCounts[kOpSetTile],
                      stats.opCounts[kOpLoadTlut], stats.opCounts[kOpLoadBlock], stats.opCounts[kOpLoadTile], stats.opCounts[kOpSetTileSize],
                      stats.opCounts[0xE4] + stats.opCounts[0xE5], stats.opCounts[0xF6], stats.opCounts[kOpSetColorImage],
                      stats.opCounts[kOpSetDepthImage],
                      stats.opCounts[0x05] + stats.opCounts[0x06] + stats.opCounts[0x07] + stats.opCounts[0xBF],
                      stats.opCounts[kOpEndDl],
                      stats.ucodeSwitches, stats.unknownUcodeSwitches,
                      stats.firstUnknownUcodeRaw,
                      stats.l3dexUcodeSkips, stats.firstL3dexUcodeRaw, dl_size);
        if (stats.noopDisplayLists != 0) {
            gdx_port_logf("[gfxfail] "
                          "miss=%zu raw=%08X parent=%p pidx=%zu pstride=%zu pbig=%d pf3d=%d "
                          "praw=%08X/%08X pdecoded=%08X/%08X "
                          "bad=%zu raw=%08X target=%p limit=%zu stride=%zu big=%d f3d=%d "
                          "first=%08X/%08X reason=%u fail_idx=%zu fail_op=%02X\n",
                          stats.missingDisplayLists,
                          stats.firstMissingDlRaw,
                          reinterpret_cast<void*>(stats.firstMissingParent),
                          stats.firstMissingParentIndex,
                          stats.firstMissingParentStride,
                          static_cast<int>(stats.firstMissingParentBigEndian),
                          static_cast<int>(stats.firstMissingParentF3D),
                          stats.firstMissingParentRawW0,
                          stats.firstMissingParentRawW1,
                          stats.firstMissingParentDecodedW0,
                          stats.firstMissingParentDecodedW1,
                          stats.badDisplayLists,
                          stats.firstBadDlRaw,
                          reinterpret_cast<void*>(stats.firstBadDlTarget),
                          stats.firstBadDlLimit,
                          stats.firstBadDlStride,
                          static_cast<int>(stats.firstBadDlBigEndian),
                          static_cast<int>(stats.firstBadDlF3D),
                          stats.firstBadDlFirstW0,
                          stats.firstBadDlFirstW1,
                          static_cast<unsigned>(stats.firstBadDlFailureReason),
                          stats.firstBadDlFailureIndex,
                          static_cast<unsigned>(stats.firstBadDlFailureOpcode));
        }
        if (stats.fallbackDataCommands != 0 || stats.skippedDataCommands != 0) {
            gdx_port_logf("[datafail] fallback=%zu op=%02X raw=%08X w0=%08X source=%p idx=%zu "
                          "skipped=%zu op=%02X raw=%08X w0=%08X\n",
                          stats.fallbackDataCommands,
                          static_cast<unsigned>(stats.firstFallbackDataOp),
                          stats.firstFallbackDataRaw,
                          stats.firstFallbackDataW0,
                          reinterpret_cast<void*>(stats.firstFallbackDataSource),
                          stats.firstFallbackDataIndex,
                          stats.skippedDataCommands,
                          static_cast<unsigned>(stats.firstSkippedDataOp),
                          stats.firstSkippedDataRaw,
                          stats.firstSkippedDataW0);
        }
    }
    sDiagFrames++;

    /* Cache eviction stays BEFORE Run so an in-place content refresh takes
       effect on the frame that produced it (LUS re-imports from the updated
       copy). The retired BUFFERS are freed after Run instead — see below. */
    if (!gPendingTextureCacheDeletes.empty()) {
        std::sort(gPendingTextureCacheDeletes.begin(), gPendingTextureCacheDeletes.end());
        gPendingTextureCacheDeletes.erase(
            std::unique(gPendingTextureCacheDeletes.begin(), gPendingTextureCacheDeletes.end()),
            gPendingTextureCacheDeletes.end());
        for (uintptr_t ptr : gPendingTextureCacheDeletes) {
            interp->TextureCacheDelete(reinterpret_cast<const uint8_t*>(ptr));
        }
        gPendingTextureCacheDeletes.clear();
    }

    interp->ResetGeometryDiagnostics();
    interp->Run(reinterpret_cast<Gfx*>(converted), {});
    // A real GFX task produced this host frame — the VI-scanout fallback must
    // not also draw over it (see gdx_vi_present_fallback).
    gHostFrameGfxTaskRan = true;

    /* Retired-buffer FREE moved AFTER Run (2026-07-10): a texture copy that
       resizes during this frame's ProcessList moves its old buffer into
       gPersistentAllocations, but the converted command stream built above
       may still carry that old buffer's pointer as a texture source.
       Freeing before Run served the interpreter a dangling pointer for one
       frame per resize (MSVC debug heap 0xDD fill). Freeing after the frame
       has drawn is always safe: the next frame re-translates and re-imports
       from the live copies. */
    gPersistentAllocations.clear();

    // Transition snapshot mirror: with a DX11 flip-model swapchain the
    // backbuffer contents are undefined after present, so
    // gdx_read_current_framebuffer cannot read last frame's pixels out of
    // fb 0 on demand (observed as all-zero transition captures). Keep a
    // persistent GPU-side copy of every completed game frame instead; the
    // transition readback samples this mirror.
    GdxUpdateFrameMirror(interp);

    const Fast::GeometryDiagnostics& geometry = interp->GetGeometryDiagnostics();
    /* Print-budget split (2026-07-08): a single global sBigTriPrints<60 cap was
       shared across the whole process lifetime, including the machine-select/
       track-preview screens that run for many seconds before a race starts.
       Those screens routinely emit >60 frames with a surviving oversized
       triangle (that's the documented normal case for the Reject ucode there),
       so the entire diagnostic budget was silently exhausted before the race
       - and hence the actual course decorations/machines the user is reporting
       spikes on - ever produced a single [bigtri] line. Give race-active
       frames their own, much larger budget so a live run always captures
       this evidence regardless of what happened on earlier menu screens. */
    static int sBigTriPrintsMenu = 0;
    static int sBigTriPrintsRace = 0;
    constexpr int kBigTriMenuCap = 20;
    constexpr int kBigTriRaceCap = 4000;
    const bool bigTriRaceActive = gGdxRaceActive != 0;
    int& bigTriPrints = bigTriRaceActive ? sBigTriPrintsRace : sBigTriPrintsMenu;
    const int bigTriCap = bigTriRaceActive ? kBigTriRaceCap : kBigTriMenuCap;
    if (gdx_diag_verbose() && geometry.bigTriangles != 0 && bigTriPrints < bigTriCap) {
        ++bigTriPrints;
        gdx_port_logf(
            "[bigtri] race=%d ucode=%d count=%llu v0=(%.1f,%.1f,%.1f,%.2f) v1=(%.1f,%.1f,%.1f,%.2f) "
            "v2=(%.1f,%.1f,%.1f,%.2f) geo=%08X combine=%016llX tile=%u tex=%p "
            "vp=%.1f,%.1f %.1fx%.1f\n",
            (int)bigTriRaceActive,
            static_cast<int>(taskUcode),
            static_cast<unsigned long long>(geometry.bigTriangles),
            geometry.bigTriX[0], geometry.bigTriY[0], geometry.bigTriZ[0], geometry.bigTriW[0],
            geometry.bigTriX[1], geometry.bigTriY[1], geometry.bigTriZ[1], geometry.bigTriW[1],
            geometry.bigTriX[2], geometry.bigTriY[2], geometry.bigTriZ[2], geometry.bigTriW[2],
            geometry.bigTriGeometryMode,
            static_cast<unsigned long long>(geometry.bigTriCombine),
            static_cast<unsigned>(geometry.bigTriTile),
            geometry.bigTriTexture,
            geometry.bigTriViewportX, geometry.bigTriViewportY,
            geometry.bigTriViewportW, geometry.bigTriViewportH);
    }
    static int sLastGeometryTaskUcode = -1;
    const bool geometryUcodeChanged = sLastGeometryTaskUcode != static_cast<int>(taskUcode);
    if (geometryUcodeChanged || (sDiagFrames % 120) == 0 ||
        geometry.invalidVertices != 0 || geometry.variantSwitches != 0) {
        gdx_port_logf(
            "[geodiag] ucode=%d vtx=%llu invalid=%llu w_nonpos=%llu near=%llu far=%llu "
            "ndc_x=%.3f..%.3f ndc_y=%.3f..%.3f ndc_z=%.3f..%.3f "
            "tri_in=%llu clip=%llu cull=%llu "
            "invisible=%llu emitted=%llu dma=%llu flx_alpha_vtx=%llu gpu_draws=%llu gpu_tris=%llu\n",
            static_cast<int>(taskUcode),
            static_cast<unsigned long long>(geometry.verticesLoaded),
            static_cast<unsigned long long>(geometry.invalidVertices),
            static_cast<unsigned long long>(geometry.verticesNonPositiveW),
            static_cast<unsigned long long>(geometry.verticesOutsideNear),
            static_cast<unsigned long long>(geometry.verticesOutsideFar),
            geometry.minNdcX, geometry.maxNdcX,
            geometry.minNdcY, geometry.maxNdcY,
            geometry.minNdcZ, geometry.maxNdcZ,
            static_cast<unsigned long long>(geometry.trianglesSubmitted),
            static_cast<unsigned long long>(geometry.trianglesClipRejected),
            static_cast<unsigned long long>(geometry.trianglesCullRejected),
            static_cast<unsigned long long>(geometry.trianglesInvisible),
            static_cast<unsigned long long>(geometry.trianglesEmitted),
            static_cast<unsigned long long>(geometry.dmaIoLoads),
            static_cast<unsigned long long>(geometry.f3dflxAlphaVertices),
            static_cast<unsigned long long>(geometry.gpuDrawCalls),
            static_cast<unsigned long long>(geometry.gpuTriangles));
        gdx_port_logf(
            "[gpustate] vp=%.1f,%.1f %.1fx%.1f sc=%.1f,%.1f %.1fx%.1f "
            "other=%08X/%08X cimg=%p zimg=%p same=%d renders_fb=%d fb_active=%d\n",
            static_cast<double>(interp->mRdp->viewport.x),
            static_cast<double>(interp->mRdp->viewport.y),
            static_cast<double>(interp->mRdp->viewport.width),
            static_cast<double>(interp->mRdp->viewport.height),
            static_cast<double>(interp->mRdp->scissor.x),
            static_cast<double>(interp->mRdp->scissor.y),
            static_cast<double>(interp->mRdp->scissor.width),
            static_cast<double>(interp->mRdp->scissor.height),
            interp->mRdp->other_mode_h, interp->mRdp->other_mode_l,
            interp->mRdp->color_image_address, interp->mRdp->z_buf_address,
            interp->mRdp->color_image_address == interp->mRdp->z_buf_address,
            static_cast<int>(interp->mRendersToFb), static_cast<int>(interp->mFbActive));
        if (gdx_diag_verbose() && geometry.variantSwitches != 0) {
            gdx_port_logf(
                "[phasegeom] switches=%llu pre_flx_vtx=%llu pre_flx_tri=%llu "
                "pre_flx_emit=%llu pre_flx_draws=%llu pre_flx_gpu_tri=%llu "
                "pre_flx_rgba16=%llu/%llu forced_opaque=%llu pre_flx_depth_bypass=%llu "
                "material tex=%llu bound0=%llu missing0=%llu forced_simple=%llu "
                "shader=%016llX/%016llX uv=%.3f..%.3f,%.3f..%.3f "
                "texstate=%ux%u line=%u size=%u tile=%u tmem=%u mask=%u/%u scale=%04X/%04X "
                "fog tri=%llu bypass=%llu factor=%.3f..%.3f mul=%d off=%d "
                "other=%08X/%08X combine=%016llX texture=%p "
                "post_flx_vtx=%llu post_flx_emit=%llu post_flx_gpu_tri=%llu "
                "post_flx_rgba16=%llu/%llu forced_opaque=%llu post_flx_depth_bypass=%llu\n",
                static_cast<unsigned long long>(geometry.variantSwitches),
                static_cast<unsigned long long>(geometry.preFlxVertices),
                static_cast<unsigned long long>(geometry.preFlxTrianglesSubmitted),
                static_cast<unsigned long long>(geometry.preFlxTrianglesEmitted),
                static_cast<unsigned long long>(geometry.preFlxGpuDrawCalls),
                static_cast<unsigned long long>(geometry.preFlxGpuTriangles),
                static_cast<unsigned long long>(geometry.preFlxRgba16OpaquePixels),
                static_cast<unsigned long long>(geometry.preFlxRgba16TransparentPixels),
                static_cast<unsigned long long>(geometry.preFlxRgba16ForcedOpaquePixels),
                static_cast<unsigned long long>(geometry.preFlxDepthBypassTriangles),
                static_cast<unsigned long long>(geometry.preFlxTexturedTriangles),
                static_cast<unsigned long long>(geometry.preFlxTexture0BoundTriangles),
                static_cast<unsigned long long>(geometry.preFlxTexture0MissingTriangles),
                static_cast<unsigned long long>(geometry.preFlxForcedSimpleMaterialTriangles),
                static_cast<unsigned long long>(geometry.preFlxShaderId0),
                static_cast<unsigned long long>(geometry.preFlxShaderId1),
                geometry.preFlxMinTextureU, geometry.preFlxMaxTextureU,
                geometry.preFlxMinTextureV, geometry.preFlxMaxTextureV,
                geometry.preFlxTextureWidth, geometry.preFlxTextureHeight,
                geometry.preFlxTextureLineBytes, geometry.preFlxTextureSizeBytes,
                static_cast<unsigned int>(geometry.preFlxTextureTile),
                static_cast<unsigned int>(geometry.preFlxTextureTmem),
                static_cast<unsigned int>(geometry.preFlxTextureMaskS),
                static_cast<unsigned int>(geometry.preFlxTextureMaskT),
                static_cast<unsigned int>(geometry.preFlxTextureScaleS),
                static_cast<unsigned int>(geometry.preFlxTextureScaleT),
                static_cast<unsigned long long>(geometry.preFlxFogTriangles),
                static_cast<unsigned long long>(geometry.preFlxFogBypassTriangles),
                geometry.preFlxMinFogFactor, geometry.preFlxMaxFogFactor,
                static_cast<int>(geometry.preFlxFogMul),
                static_cast<int>(geometry.preFlxFogOffset),
                geometry.preFlxOtherModeH, geometry.preFlxOtherModeL,
                static_cast<unsigned long long>(geometry.preFlxCombineMode),
                geometry.preFlxTexture,
                static_cast<unsigned long long>(geometry.verticesLoaded - geometry.preFlxVertices),
                static_cast<unsigned long long>(geometry.trianglesEmitted -
                                                geometry.preFlxTrianglesEmitted),
                static_cast<unsigned long long>(geometry.gpuTriangles -
                                                geometry.preFlxGpuTriangles),
                static_cast<unsigned long long>(geometry.rgba16OpaquePixels -
                                                geometry.preFlxRgba16OpaquePixels),
                static_cast<unsigned long long>(geometry.rgba16TransparentPixels -
                                                geometry.preFlxRgba16TransparentPixels),
                static_cast<unsigned long long>(geometry.rgba16ForcedOpaquePixels -
                                                geometry.preFlxRgba16ForcedOpaquePixels),
                static_cast<unsigned long long>(geometry.depthBypassTriangles -
                                                geometry.preFlxDepthBypassTriangles));
        }
    }
    sLastGeometryTaskUcode = static_cast<int>(taskUcode);

    /* Mirror every distinct N64 framebuffer that was targeted as CIMG anywhere
       in this task, not just whatever CIMG happens to be set at the very end.
       A single task's display list frequently redirects CIMG to an offscreen
       framebuffer mid-task (the SETCIMG "canvas" idiom in texture_utils.c's
       func_8007AB88/func_8007ABA4, driven by the OBJECT_FRAMEBUFFER object
       type) and restores the normal display target before the task ends.
       Checking only the final color_image_address silently dropped every such
       mid-task target: it never became valid, so gdx_read_current_framebuffer
       callers (transition captures / canvas composites) never saw real data
       for it. stats.colorImageTargets was populated while converting this
       task's display list (every resolved G_SETCOLORIMAGE, deduped); fold in
       the interpreter's own final color image in case it was set by a path
       the adapter didn't see, then mirror each match against gN64Framebuffers.
       Only the buffer matching the task's FINAL color image updates
       gLastRenderedFramebuffer — that flag distinguishes "the active display
       target" (always re-read fresh) from other buffers holding frozen
       snapshots (preserved as-is by gdx_read_current_framebuffer). */
    const uintptr_t finalColorImage = reinterpret_cast<uintptr_t>(interp->mRdp->color_image_address);
    bool finalColorImageTracked = false;
    for (size_t ti = 0; ti < stats.colorImageTargetCount; ti++) {
        if (stats.colorImageTargets[ti] == finalColorImage) {
            finalColorImageTracked = true;
            break;
        }
    }
    if (!finalColorImageTracked && finalColorImage != 0 &&
        stats.colorImageTargetCount < stats.colorImageTargets.size()) {
        stats.colorImageTargets[stats.colorImageTargetCount++] = finalColorImage;
    }

    for (size_t ti = 0; ti < stats.colorImageTargetCount; ti++) {
        const uintptr_t targetAddress = stats.colorImageTargets[ti];
        auto framebuffer = std::find_if(gN64Framebuffers.begin(), gN64Framebuffers.end(),
                                        [targetAddress](const N64FramebufferInfo& info) {
                                            return info.address == targetAddress;
                                        });
        if (framebuffer == gN64Framebuffers.end()) {
            continue;
        }
        const int hostFramebuffer = interp->mRendersToFb ? interp->mGameFb : 0;
        interp->GetCurrentRenderingAPI()->ReadFramebufferToCPU(
            hostFramebuffer, framebuffer->width, framebuffer->height,
            reinterpret_cast<uint16_t*>(framebuffer->address));
        framebuffer->valid = true;
        const size_t mirroredBytes =
            static_cast<size_t>(framebuffer->width) * framebuffer->height * sizeof(uint16_t);
        RecordHostWrite(framebuffer->address, mirroredBytes);
        const bool isFinalTarget = (targetAddress == finalColorImage);
        if (isFinalTarget) {
            gLastRenderedFramebuffer = framebuffer->address;
        }

        // One-shot-per-buffer diagnostic: confirms whether/when the mirror
        // actually fires for each registered N64 framebuffer, and whether it
        // was the task's final CIMG or a mid-task canvas target.
        static bool sMirrorLogged[8] = {};
        const size_t fbIndex = static_cast<size_t>(framebuffer - gN64Framebuffers.begin());
        if (fbIndex < 8 && !sMirrorLogged[fbIndex]) {
            sMirrorLogged[fbIndex] = true;
            gdx_port_logf("[fbmirror] fired fb=%zu addr=%p bytes=%zu final=%d\n", fbIndex,
                          reinterpret_cast<void*>(framebuffer->address), mirroredBytes,
                          static_cast<int>(isFinalTarget));
        }
    }
}

/* Graphics wave W5 instrumentation: log what the transition readback actually
   hands the game (dimensions, which source path fed it, offset of the first
   nonzero pixel) and dump the FIRST capture to transition-capture.bmp
   (RGBA5551 -> 24bpp, bottom-up) next to the exe, so the next soak shows
   exactly what the game receives instead of guessing. */
static void LogAndDumpTransitionCapture(const uint16_t* pixels, unsigned int width,
                                        unsigned int height, const char* sourcePath) {
    // Budget split (2026-07-09): 8 was consumed entirely by boot-phase
    // captures (VI-fallback frames + the title/logo transitions), so the
    // menu-transition captures a soak actually cares about never got logged.
    // Raised so later, more interesting captures still show up.
    static int sCaptureCount = 0;
    static bool sDumped = false;
    if (pixels == nullptr || sCaptureCount >= 24) {
        return;
    }
    ++sCaptureCount;

    const size_t pixelCount = static_cast<size_t>(width) * height;
    long long firstNonZero = -1;
    for (size_t i = 0; i < pixelCount; i++) {
        if (pixels[i] != 0) {
            firstNonZero = static_cast<long long>(i);
            break;
        }
    }
    gdx_port_logf("[transition] capture #%d %ux%u source=%s mode=%d firstNonZeroPx=%lld%s\n",
                  sCaptureCount, width, height, sourcePath, (gGameMode & 0x1F), firstNonZero,
                  sDumped ? "" : " dump=transition-capture.bmp");

    if (sDumped) {
        return;
    }
    sDumped = true;

    std::FILE* f = std::fopen("transition-capture.bmp", "wb");
    if (f == nullptr) {
        gdx_port_logf("[transition] BMP dump failed: fopen errno path\n");
        return;
    }
    const unsigned int rowBytes = (width * 3u + 3u) & ~3u;
    const unsigned int imageBytes = rowBytes * height;
    const unsigned int fileBytes = 14u + 40u + imageBytes;
    unsigned char header[54] = { 0 };
    header[0] = 'B'; header[1] = 'M';
    header[2] = static_cast<unsigned char>(fileBytes);
    header[3] = static_cast<unsigned char>(fileBytes >> 8);
    header[4] = static_cast<unsigned char>(fileBytes >> 16);
    header[5] = static_cast<unsigned char>(fileBytes >> 24);
    header[10] = 54; // pixel data offset
    header[14] = 40; // BITMAPINFOHEADER size
    header[18] = static_cast<unsigned char>(width);
    header[19] = static_cast<unsigned char>(width >> 8);
    header[20] = static_cast<unsigned char>(width >> 16);
    header[21] = static_cast<unsigned char>(width >> 24);
    header[22] = static_cast<unsigned char>(height);
    header[23] = static_cast<unsigned char>(height >> 8);
    header[24] = static_cast<unsigned char>(height >> 16);
    header[25] = static_cast<unsigned char>(height >> 24);
    header[26] = 1;  // planes
    header[28] = 24; // bpp
    std::fwrite(header, 1, sizeof(header), f);

    std::vector<unsigned char> row(rowBytes, 0);
    for (unsigned int y = 0; y < height; y++) {
        // BMP rows are bottom-up; N64 framebuffer is top-down.
        const uint16_t* src = pixels + static_cast<size_t>(height - 1 - y) * width;
        for (unsigned int x = 0; x < width; x++) {
            // N64 RGBA5551: RRRRRGGG GGBBBBBA (big-endian u16 already decoded to
            // host order by the readback/mirror path).
            const uint16_t p = src[x];
            const unsigned char r5 = (p >> 11) & 0x1F;
            const unsigned char g5 = (p >> 6) & 0x1F;
            const unsigned char b5 = (p >> 1) & 0x1F;
            row[x * 3 + 2] = static_cast<unsigned char>((r5 << 3) | (r5 >> 2));
            row[x * 3 + 1] = static_cast<unsigned char>((g5 << 3) | (g5 >> 2));
            row[x * 3 + 0] = static_cast<unsigned char>((b5 << 3) | (b5 >> 2));
        }
        std::fwrite(row.data(), 1, rowBytes, f);
    }
    std::fclose(f);
    gdx_port_logf("[transition] first capture dumped to transition-capture.bmp\n");
}

extern "C" int gdx_read_current_framebuffer(void* rgba16Buffer, unsigned int width, unsigned int height) {
    if (rgba16Buffer == nullptr || width == 0 || height == 0) {
        return 0;
    }

    auto wnd = Ship::Context::GetInstance()->GetWindow();
    auto* fw = static_cast<Fast::Fast3dWindow*>(wnd.get());
    if (fw == nullptr) {
        return 0;
    }

    auto interp = fw->GetInterpreterWeak().lock();
    if (!interp) {
        return 0;
    }

    const uintptr_t requestedAddress = reinterpret_cast<uintptr_t>(rgba16Buffer);
    auto requested = std::find_if(gN64Framebuffers.begin(), gN64Framebuffers.end(),
                                  [requestedAddress](const N64FramebufferInfo& info) {
                                      return info.address == requestedAddress;
                                  });
    // The persistent frame mirror IS the last completed frame — exactly what a
    // transition snapshot wants — so when it exists it is authoritative and the
    // "preserve this buffer's historical contents" early-return must not fire:
    // that path previously served bytes poisoned by an earlier all-zero boot
    // capture (requested->valid was set even though the readback produced
    // nothing), which kept transitions black forever after.
    const bool mirrorAvailable = (gFrameMirrorFb >= 0 && gFrameMirrorValid);
    if (!mirrorAvailable && requested != gN64Framebuffers.end() && requested->valid &&
        requestedAddress != gLastRenderedFramebuffer) {
        LogAndDumpTransitionCapture(static_cast<const uint16_t*>(rgba16Buffer), width, height,
                                    "preserved-mirror");
        return 1;
    }

    interp->Flush();
    const auto hasContent = [](const uint16_t* px, unsigned int w, unsigned int h) {
        const size_t count = static_cast<size_t>(w) * h;
        for (size_t k = 0; k < count; k++) {
            if (px[k] != 0) {
                return true;
            }
        }
        return false;
    };
    // Prefer the persistent frame mirror (a real framebuffer with its own
    // texture, copied GPU->GPU before present). Reading fb 0 after present is
    // undefined on DX11 flip-model swapchains. If the mirror somehow has no
    // content yet (nothing presented), fall back to the direct read rather
    // than serving zeros.
    const char* sourcePath = "none";
    uint16_t* out = static_cast<uint16_t*>(rgba16Buffer);
    bool gotContent = false;
    if (mirrorAvailable) {
        interp->GetCurrentRenderingAPI()->ReadFramebufferToCPU(gFrameMirrorFb, width, height, out);
        sourcePath = "frame-mirror";
        gotContent = hasContent(out, width, height);
    }
    if (!gotContent) {
        const int direct = interp->mRendersToFb ? interp->mGameFb : 0;
        interp->GetCurrentRenderingAPI()->ReadFramebufferToCPU(direct, width, height, out);
        sourcePath = interp->mRendersToFb ? "host-fb-game" : "host-fb-0";
        gotContent = hasContent(out, width, height);
    }
    // Only publish this buffer as a valid frame source when it actually holds
    // pixels — an all-zero capture must never poison the preserved path or the
    // dirty-range tracking.
    if (gotContent && requested != gN64Framebuffers.end()) {
        requested->valid = true;
        gLastRenderedFramebuffer = requestedAddress;
        RecordHostWrite(requestedAddress,
                        static_cast<size_t>(width) * height * sizeof(uint16_t));
    }
    LogAndDumpTransitionCapture(static_cast<const uint16_t*>(rgba16Buffer), width, height, sourcePath);
    return 1;
}
