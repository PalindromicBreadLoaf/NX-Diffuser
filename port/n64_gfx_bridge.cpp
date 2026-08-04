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
#else
/* POSIX memory-probe backend (see the /proc/self/maps snapshot helpers and the
 * #else branches of GetMainModuleRange / ReadableByteLimit / ReadableCommandLimit
 * / IsReadableAddress further down). */
#include <atomic>
#include <dlfcn.h>
#include <unistd.h>
#endif

#include "ship/Context.h"
#include "fast/Fast3dWindow.h"
#include "fast/lus_gbi.h"
#include "gdx_perf.h" // GDX_PERF sub-phase seams (xlate/run/mirror); no-op when disabled
#include "gdx_dev_gates.h" // Dev Tools gate layer: every GDX_DIAG_* / behavior switch below
#include "port_log.h"
#include "rom_buffer.h"
#include "gdx_segment_source.h" // single byte-source shim (GdxSegmentSourceRead / span)
#include "n64_rdram.h"
#include "n64_gfx_bridge.h"
#include "n64_gfx_convert.h"
#include "gdx_vi_convert.h"
#include "gdx_interp.h" // matrix frame-interpolation math + per-tick snap state (default-OFF)
extern "C" {
#include "mio0.h"
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

extern "C" int gGdxRaceActive;
/* Forward decl (also declared lower in this file) so the
   mode-gated Create Machine SETTIMG census can read GET_MODE(gGameMode)==0x10. */
extern "C" int gGameMode;

// ---------------------------------------------------------------------------------------------
// Segment-reload epoch (Create Machine / mode-transition TOCTOU guard).
//
// Mode transitions reload asset segments on the GAME thread: decomp_port.c's
// gdx_load_mode_segments() DMAs/MIO0-decodes fresh bytes into the segment-4/7/9
// carves, rewrites them in place (gdx_fixup_asset_segment_image), and swaps the
// gSegments[] bases (Segment_SetAddress). The GRAPHICS thread concurrently reads
// gSegments[] and the segment buffer CONTENTS while translating display lists,
// with no synchronization. Reading a half-rewritten command word can hand the
// interpreter a torn opcode/pointer -- e.g. a byte that lands as an OTR-filepath
// opcode (0x25) paired with a bare 32-bit token where a host string pointer is
// expected, which then faults inside strlen on the LUS side.
//
// This is a seqlock generation counter. The game thread brackets every segment
// mutation with gdx_segment_epoch_begin()/gdx_segment_epoch_end(); the value is
// ODD while a mutation is in flight. The graphics thread snapshots it before a
// segment-backed resolution and, via GdxSegmentEpochStable(), rejects the result
// if the snapshot was odd (mutation in progress) or changed across the resolution
// (a mutation started/finished mid-read -- possibly torn). The graphics thread
// NEVER blocks: on an unstable snapshot it takes the same graceful hard-skip as a
// failed resolution, dropping that one texture for the single reload frame (an
// invisible cost during a mode transition). Wait-free by construction -- only
// atomic loads, no locks, no ret/spins on the render thread.
static std::atomic<uint32_t> gGdxSegmentEpoch{0};

// Game-thread mutation brackets. acq_rel keeps each increment from being
// reordered past the segment writes it fences: begin()'s odd publish stays
// BEFORE the buffer/base writes, end()'s even publish stays AFTER them, so a
// graphics-thread acquire-load that sees an even, unchanged epoch is guaranteed
// to have observed a fully-settled segment state.
extern "C" void gdx_segment_epoch_begin(void) {
    gGdxSegmentEpoch.fetch_add(1u, std::memory_order_acq_rel);
}
extern "C" void gdx_segment_epoch_end(void) {
    gGdxSegmentEpoch.fetch_add(1u, std::memory_order_acq_rel);
}

// Graphics-thread seqlock read side.
static inline uint32_t GdxSegmentEpochSnapshot() {
    return gGdxSegmentEpoch.load(std::memory_order_acquire);
}
// True iff `snap` was taken outside any mutation window AND no mutation has begun
// since: even snapshot and unchanged now. The acquire fence orders the caller's
// segment reads (which happened after taking `snap`) before this second load, so
// a mutation that raced the reads is reliably detected as a changed epoch.
static inline bool GdxSegmentEpochStable(uint32_t snap) {
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t now = gGdxSegmentEpoch.load(std::memory_order_acquire);
    return ((snap & 1u) == 0u) && (snap == now);
}
// #16 investigation aid: decomp sets this to 1 (racer.c, right where it already
// logs "[countdown] draw emitted") the instant the countdown draw code runs, so
// the bridge's raw vtx/mtx trace only has to cover the interesting few frames
// instead of the whole race -- gGdxRaceActive alone stays 1 for the entire race
// and made a fixed-size trace file fill up long before the countdown appeared.
extern "C" int gGdxCountdownProbeArm = 0;
// The coarse arm above stays 1 for the rest of the process once
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
#include <deque> // stable-address scratch-slot arena (deque never invalidates element pointers)
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
/* The two course material setup DLs (segment 8 +0x14040 / +0x14078), read by the
   GDX_DIAG_SETUPDL probe in ProcessList. */
extern "C" uint8_t D_8014040[];
extern "C" uint8_t D_8014078[];
/* course_track_gfx base (segment 8). Referenced by gdx_boot_warm_asset_segments: its MIO0 decode
   measured 133.95ms in a single hit on the boot path -- the largest asset stall in the port. */
extern "C" uint8_t D_8000000[];
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
/* The two live GfxPools (decomp: GfxPool D_8024DCE0[2], unk_gfx_segment.c:194),
   addressed as raw bytes here exactly like gdx_interp.cpp:250 does. Needed by
   IsGfxPoolHostRange() so the persistent texture-copy cache knows this
   registered host range is REWRITTEN EVERY FRAME instead of ROM-stable. */
extern "C" uint8_t D_8024DCE0[];
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
// E4 (A3 follow-up): banks 9-11, declared in decomp's fzx_segmentA.h:15-17.
// Banks 9-10 have no current reference but are registered for symmetry with
// the rest of the table (see tools/gen_link_stubs.py's EXTRA_DATA_SYMS for why
// they needed an explicit, always-linked LinkStubs.c stub -- nothing currently
// references them so a normal log-driven regen could never add them).
extern "C" uint8_t D_A009000[];
extern "C" uint8_t D_A00A000[];
#ifdef EXPANSION_KIT
// D_A00B000..D_A00BD80 are live -- gRoadTypeMenuItems/gHRoadTypeMenuItems/
// gTRoadTypeMenuItems (decomp/src/overlays/expansion_kit/A3AE0.c:533-568)
// reference them directly as the Road-Type panel's per-venue "type 1" preview
// icons -- but that overlay directory is EXCLUDED from non-EK builds
// (port/CMakeLists.txt), and their only stubs live in the EK-only
// port/gen/EkLinkStubs.c, so these symbols do not exist to link against
// outside an EXPANSION_KIT build. Every base-game venue texture yaml
// (decomp/assets/yaml/*/rev0/*_textures.yaml) confirms these are real,
// RGBA16 24x12 icons at segment 0x0A offsets 0xB000/0xB240/0xB480/0xB6C0/
// 0xB900/0xBB40/0xBD80, immediately past the 11 main 0x1000-byte texture
// banks (D_A000000..D_A00A000) -- each venue ships its own icon set matching
// its visual theme, loaded as part of the same venue segment image.
extern "C" uint8_t D_A00B000[];
extern "C" uint8_t D_A00B240[];
extern "C" uint8_t D_A00B480[];
extern "C" uint8_t D_A00B6C0[];
extern "C" uint8_t D_A00B900[];
extern "C" uint8_t D_A00BB40[];
extern "C" uint8_t D_A00BD80[];
#endif  // EXPANSION_KIT
// E3: the hand-listed 23-symbol kEkNamedAssetStubs table (and its externs) that
// used to live here was replaced by a generic registration hook -- see
// gdx_register_host_pointer_stub / ResolveHostPointerStub below, populated from
// gdx_ek_assets_fill()'s full ~773-entry sEkAssetFills[] table instead of a
// hand-maintained subset. No named externs are needed here any more.
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
extern "C" int gdx_resolve_mode_segment9(unsigned int raw, size_t requiredBytes,
                                           uintptr_t* outAddress);
// E2: true when segment `seg` (4, 7, or 9) is currently owned by a live mode
// carve (port/decomp_port.c's sGdxSeg4Resident/sGdxSeg7Resident/sGdxSeg9Active).
// Any other segment always returns false.
extern "C" int gdx_mode_owns_segment(unsigned int seg);
// True when `rom_base` matches the ROM family CURRENTLY resident/active for
// mode-owned segment `seg` (see the comment above the definition in
// decomp_port.c). Used to gate the live-carve redirect below so a stale-family
// AssetBindings.c row (e.g. a different content sharing the same segment
// number) is not served the wrong carve's bytes.
extern "C" int gdx_mode_segment_content_matches(unsigned int seg, unsigned int rom_base);
extern "C" const char* GDiffuser_LookupLoadedAssetKey(const void* buffer, size_t minSize, int requireUnmodified);
extern "C" const char* gdx_lookup_asset_segment_o2r_key(unsigned int sym_low32);
// Workshop texture packs (port/gdx_workshop.cpp): Tier-B override shim.
extern "C" int gdx_workshop_texture_packs_enabled(void);
extern "C" const char* GdxWorkshopLookupOverridePath(const char* key);

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

// [venueload] Countdown of ticks whose translation cost should be logged after a venue segment
// load. Set by gdx_load_venue_texture_segment, consumed in gdx_gfx_run. Declared up here because
// both of those live far below and on opposite sides of the file. Exists to test whether the Cup
// Select stall is the LOAD itself or the conversion-cache invalidation that follows it -- see the
// block comment at the [venueload] emit site.
static int gGdxVenueWatchTicks = 0;

// Scheduler yield counter (n64_sched.c). Each yield returns to the host fiber, which pumps a whole
// frame before re-dispatching, so one yield inside a load costs a full ~16.7ms tick of wall clock
// irrespective of the load's own work. Used to tell real decode cost apart from yield latency.
extern "C" unsigned long gdx_yield_count;

struct ConversionStats {
    std::array<size_t, 256> opCounts{};
    size_t convertedLists = 0;
    size_t noopDisplayLists = 0;
    size_t fallbackDataCommands = 0;
    size_t skippedDataCommands = 0;
    size_t skippedTextures = 0;
    /* Commands whose segment-backed pointer resolution was rejected because a
       game-thread segment reload (gGdxSegmentEpoch, top of file) raced it. Kept
       separate from skippedTextures/skippedDataCommands so a mode-transition
       reload storm is distinguishable from genuine resolution failures. */
    size_t skippedEpochRetries = 0;
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
       a genuinely unmatched G_LOAD_UCODE in the [gfxdiag] line. */
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
std::vector<HostRange> gHostWideCommandRanges;
std::vector<N64AddressRange> gN64AddressRanges;
// E3/A1/A2: host-pointer identity registration for compiled-in host arrays that
// SETTIMG can carry the address of directly (EK disk assets from
// port/gen/EkAssetBindings.c's sEkAssetFills, plus base-game arrays like the
// ending fireworks sprites / sCourseMinimapPalette registered from
// port/decomp_port.c), populated via gdx_register_host_pointer_stub. Reused
// HostRange (begin = the array's own host address, size = its byte length)
// since the match ResolveHostPointerStub performs is identical in shape to the
// other host-range tables here -- an interior delta<size match keyed on a host
// pointer, not an N64 address. Replaces the old hand-maintained
// kEkNamedAssetStubs table.
std::vector<HostRange> gHostPointerStubs;
std::vector<HostRange> gF3DAssetRanges;
std::vector<HostRange> gNativeRgba16Ranges;
std::vector<uintptr_t> gPendingNativeRgba16RangeClears;
std::vector<uint8_t> gSetupGfxSegment;
std::vector<PersistentRawTextureCopy> gRawTextureCopies;
std::vector<uintptr_t> gPendingTextureCacheDeletes;
std::vector<std::unique_ptr<uint8_t[]>> gPersistentAllocations; // Fixed undefined mPersistentAllocations
std::vector<N64FramebufferInfo> gN64Framebuffers;
uintptr_t gViCurrentFramebuffer = 0;
uintptr_t gViNextFramebuffer = 0;
uintptr_t gLastRenderedFramebuffer = 0;

/* Title->menu wipe-band diagnostic scope. Transition_SetBackgroundBuffer
 * records the just-captured/registered transition background buffer here (via the
 * extern-C gdx_diag_note_transition_capture below). The SETTIMG host-pointer path
 * then logs, once per unique source inside this span, whether the byteswap-applying
 * native-RGBA16 range covers it. Zero size = no active transition capture, so the
 * probe costs nothing during normal rendering and can never affect a non-transition
 * texture. */
uintptr_t gDiagTransitionCaptureBegin = 0;
size_t gDiagTransitionCaptureSize = 0;

// Coarse asset epoch: bumped whenever an asset/ROM-backed segment image
// is (re)decoded (EnsureAssetSegmentImage). Declared here -- ahead of that
// function -- so it can invalidate converted lists built against an old image.
// The rest of the converter state lives further down (needs the resolver
// helpers forward-declared below).
uint32_t gConvertEpoch = 1;

// Set by gdx_gfx_run() whenever a real GFX task renders into the current host
// frame; checked + cleared once per frame by gdx_vi_present_fallback(). When it
// is false at present time, no task produced this frame (boot-logo phase or any
// other CPU-drawn screen) and the fallback must scan out the VI framebuffer.
bool gHostFrameGfxTaskRan = false;

// Once a real GFX task has ever presented, a taskless host frame must HOLD the
// last GPU image (like the N64 VI re-scanning the already-rendered RDRAM
// buffer) instead of blitting the CPU-side VI framebuffer — which is empty for
// GPU-rendered screens and produced full-screen black flashes whenever the
// game briefly dropped below present rate (Cup Select's cup slide-up rendered
// 1 of every 3 presents; the other 2 flashed black).
static bool sGpuContentLive = false;
// Set true at the tail of every real-task tick (gdx_gfx_run) to mark that the
// persistent frame mirror just got fresh content. gdx_vi_present_fallback's
// hold path used to gate a CPU readback on this flag; it now composites the
// mirror with a GPU->GPU copy every hold tick regardless (see the "40fps on
// menus" comment there), so this flag is read-and-cleared purely as a diag
// signal (GdxDiagHoldTick) for whether content actually changed since the
// previous hold tick — it no longer controls what gets drawn.
static bool sGpuHoldPixelsStale = true;

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
    /* RULED OUT -- "the decoration DLs are F3D": a word-level re-decode of the
     * decoration DLs (D_80172A0: 0xD7 G_TEXTURE, 0x01 G_VTX, 0x05 G_TRI1 runs,
     * 0xDF terminator) proves they are F3DEX2 -- the blanket below is CORRECT.
     * The earlier "they are F3D" identification was a dual-dialect pattern-matching
     * error. The decorations' real defect is the fixup-region
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

// Bytes from `source` to the end of the native-RGBA16 range that contains it
// (0 if `source` is not inside any native range). Used to keep a texture copy's
// byte-swap treatment aligned to the exact registered extent: a load-size
// estimate that rounds up past the registered image (e.g. the WIPE transition's
// single wide LOADBLOCK, whose block-rounded estimate exceeds WIDTH*HEIGHT*2 by
// one row) must not be allowed to disable the swap for the whole copy — clamp
// the copy to this remaining extent so CopyRawTextureBytes still swaps it.
size_t NativeRgba16RangeRemaining(uintptr_t source) {
    size_t best = 0;
    for (const HostRange& range : gNativeRgba16Ranges) {
        if (source >= range.begin && source < range.begin + range.size) {
            const size_t remaining = (range.begin + range.size) - source;
            if (remaining > best) {
                best = remaining;
            }
        }
    }
    return best;
}

bool NativeRgba16CopyMatches(const uint8_t* copy, uintptr_t source, size_t size) {
    if (copy == nullptr || source == 0) {
        return false;
    }

    const auto* input = reinterpret_cast<const uint8_t*>(source);
    size_t i = 0;
    for (; i + 1 < size; i += 2) {
        if (copy[i] != input[i + 1] || copy[i + 1] != input[i]) {
            return false;
        }
    }
    return i >= size || copy[i] == input[i];
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
// Quarantine for the pointer-GUESSING resolver branches.
//
// Game-emitted display lists carry real 64-bit host pointers, and the narrow->wide converter
// resolves every binary N64 list ONCE with deterministic pointer resolution (segment table +
// RDRAM-arena physical strip only -- never a low32-window match or a high-32 reconstruction).
// Together those mean the guessing machinery below -- ResolveRegisteredHostPointer's low32-window
// match, the KSEG0 high-32 reconstruction, the physical/source-window high-32 reconstructions, the
// ambiguous cross-segment fallback, and the raw-buffer/last-resort substitutions -- should only
// ever fire for STRAGGLERS: narrow lists reached with GDX_G2_CONVERT=0, a conversion miss, or a
// legacy F3D asset path the converter does not touch.
//
// The branches are gated rather than deleted so the safety net survives, and instrumented so a run
// quantifies what still relies on guessing: a per-branch hit counter, a capped "[legacy-resolve]
// branch=<name> hits=<n> raw=%08X op=%02X" line for the first 8 hits of each branch, and a one-time
// "[legacy-resolve] SUMMARY" the instant any branch fires.
//
// The gate DEFAULTS OFF -- the guessing machinery is INACTIVE in a stock build, because a full
// boot->race->close session recorded zero hits on every branch. GDX_LEGACY_RESOLVE (or the
// gDevTools.Behavior.LegacyResolve CVar) restores it without a rebuild.
//
// If these are ever deleted, do NOT delete with them: segment-table lookups (explicit-segment and
// encodedSegment paths), ResolvePortBssAlias / ResolveVenueBankAlias / ResolveGeneratedAssetStub /
// ResolveSetupGfxStub (exact known-symbol matches, not guesses), the D_1000000 special case, the EK
// gN64AddressRanges reverse scan, texture/framebuffer image op handling (SETTIMG/SETCIMG/SETZIMG),
// BRANCH_Z/DMA_IO/RDPHALF_1, or the converter/cache/GDX_G2_CONVERT switch. The deterministic
// raw & 0x1FFFFFFF RDRAM strip inside the KSEG0 branch is also NOT a guess.
// ---------------------------------------------------------------------------
constexpr bool kGdxLegacyResolveDefaultEnabled = false;
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

// Runtime switch: with kGdxLegacyResolveDefaultEnabled false, GDX_LEGACY_RESOLVE —
// or Dev Tools > Behavior overrides > "Legacy address guessing" — restores the old guessing
// machinery for the current process without a rebuild. Every branch below behaves as if it never
// matched while this is off, which is the shipping default.
//
// The gate is normalized so 0 == stock (guessing OFF), because the Dev Tools layer compiles Bucket
// B gates out of a Release build by hard-wiring them to 0. NOTE: if the compile-time default is
// ever flipped back to true, this accessor short-circuits and the gate can no longer turn guessing
// OFF — revisit it together with that flip.
inline bool LegacyResolveEnabled() {
    return kGdxLegacyResolveDefaultEnabled || gdx_dev_gate(GDX_GATE_LEGACY_RESOLVE) != 0;
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
 * and faulted deep inside Fast::Interpreter::GfxSpVertex:
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
 * pointer just because its high32 is set. */
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
               authority for slot 0x0A and assigns it unconditionally.

               THREAD/SEQLOCK CONSTRAINT (deliberate non-bracket):
               EnsureAssetSegmentImage is reachable from BOTH the game thread
               (loaders) AND the graphics thread (TryResolveAddress at the
               segment-0x0A path, ResolveSetupGfxStub, and gdx_gfx_run's
               EnsureSetupGfxSegment/aVpFullScreen pre-pass). gdx_segment_epoch_
               begin/end is a SINGLE-WRITER seqlock owned by the game thread; a
               graphics-thread begin/end would race the fetch_add and corrupt the
               odd/even parity, defeating the guard for every reader. So this
               first-claim store is intentionally NOT epoch-bracketed. It is
               benign unbracketed because it only ever transitions the slot
               0 -> a valid, immutable host pointer (guarded by the ==0 test,
               never valid -> different), the store is a single aligned uintptr_t
               (atomic on the targets, no torn pointer), and the buffer it names
               was fully built before this store. A racing graphics reader either
               sees 0 (treats the segment as unresolved -> graceful hard-skip,
               epoch-independent) or the fully-published pointer -- there is no
               valid->valid' transition here that the seqlock would need to
               protect. The seqlock still guards the game thread's AUTHORITATIVE
               0x0A rewrite in gdx_load_venue_texture_segment, which IS bracketed. */
            if (gSegments[lookup.segment] == 0) {
                gSegments[lookup.segment] = reinterpret_cast<uintptr_t>(loaded.bytes.data());
            }
            return reinterpret_cast<uintptr_t>(loaded.bytes.data());
        }
    }

    // The byte SOURCE is the single shim (GdxSegmentSourceRead), never a
    // direct gdx_rom_buffer content read. The gdx_rom_buffer/gdx_rom_size uses in
    // this function are bounds arithmetic ONLY (not content reads).
    //
    // Archive-only fix (found by deleting the ROM after setup): the old gate here
    // returned 0 whenever gdx_rom_buffer was NULL -- BEFORE the shim ever ran --
    // which killed every asset segment (all venue textures, setup_gfx, viewports)
    // on an archive-only boot even though the segment blobs fully cover them. The
    // ROM is only genuinely required when NO archive blob contains this family:
    // with a containing blob, the shim serves every read and the blob span bounds
    // the sizing arithmetic below in place of gdx_rom_size.
    uint32_t blobSpan = 0;
    const bool haveBlobSpan = GdxSegmentSourceContainingSpan(lookup.romBase, &blobSpan) != 0;
    const bool romUsable = (gdx_rom_buffer != nullptr) && (lookup.romBase < gdx_rom_size);
    if (!haveBlobSpan && !romUsable) {
        return 0;
    }

    LoadedAssetSegment loaded = {};
    loaded.segment = lookup.segment;
    loaded.romBase = lookup.romBase;
    loaded.compressed = lookup.compressed;

    // Peek the family's leading bytes through the shim: MIO0_HEADER_LENGTH bytes
    // cover both the magic sniff and the BE32 decoded-size field. A short read (a
    // family with fewer than 16 ROM bytes, or the ROM absent) leaves havePeek
    // false, which the plain path below then handles identically to the old
    // `gdx_rom_size >= romBase + MIO0_HEADER_LENGTH` guard.
    uint8_t peek[MIO0_HEADER_LENGTH];
    const bool havePeek = GdxSegmentSourceRead(lookup.romBase, MIO0_HEADER_LENGTH, peek) != 0;
    const bool isMio0 = havePeek && (std::memcmp(peek, "MIO0", 4) == 0);

    // Stage a whole MIO0 stream through the shim, then decode from the STAGED copy
    // (byte-identical to the old `mio0_decode(gdx_rom_buffer + romBase, ...)`). The
    // compressed length is not carried in the header, so size the stage to the
    // family's archive-blob span (keeps the read archive-first / fully contained);
    // with no blob, bound the stage by the MIO0 header (the uncompressed-section
    // end is the last input the decoder reads) capped to the ROM image. The stage
    // is a transient per-call heap buffer: this code runs only on a cache MISS (the
    // gLoadedAssetSegments hit loop above returns first), so it is one-time-per-
    // family and inherently thread-safe -- no shared static staging buffer to race
    // between the game and graphics threads, both of which reach this function.
    // [venueload] Entry stamp for the read/decode half. Boot-preloading the archive blob made the
    // source read free and moved the measured venue-load cost NOT AT ALL, and the fixup/range half
    // separately measured under 1 ms, so by elimination the expense lives between here and the
    // fixup timer below. This stamp closes the last gap.
    const auto gdxDecodeT0 = std::chrono::steady_clock::now();
    // Sample the scheduler's yield counter across the same window. A yield returns to the host
    // fiber, which pumps a whole frame before re-dispatching, so each one costs a full ~16.7ms tick
    // no matter how little work the load is doing. If yields > 0 correlates with the slow loads,
    // the decode is not expensive and "decode earlier" would be the wrong fix.
    const unsigned long gdxYieldsBefore = gdx_yield_count;

    auto stageAndDecodeMio0 = [&]() -> bool {
        uint32_t span = 0;
        size_t stageSize;
        if (GdxSegmentSourceContainingSpan(lookup.romBase, &span) &&
            span >= MIO0_HEADER_LENGTH) {
            stageSize = span; // exact archived family span -> stays contained
        } else {
            const uint32_t decSize = ReadBE32(peek + 4);   // dest_size
            const uint32_t uncompOff = ReadBE32(peek + 12); // uncomp_offset
            const size_t bound = static_cast<size_t>(uncompOff) + static_cast<size_t>(decSize);
            // No containing blob (or a degenerate one below header size): the raw
            // ROM is the only source left. Archive-only with no usable ROM yields
            // avail=0 -> stageSize < header -> clean failure below.
            const size_t avail = romUsable ? (gdx_rom_size - lookup.romBase) : 0;
            stageSize = std::min<size_t>(bound, avail);
        }
        if (stageSize < MIO0_HEADER_LENGTH) {
            return false;
        }
        std::vector<uint8_t> stage(stageSize);
        if (!GdxSegmentSourceRead(lookup.romBase, static_cast<uint32_t>(stageSize), stage.data())) {
            return false;
        }
        const uint32_t decodedSize = ReadBE32(stage.data() + 4);
        const size_t outputSize = std::max<size_t>(decodedSize, lookup.imageSize);
        if (outputSize == 0) {
            return false;
        }
        loaded.bytes.resize(outputSize);
        std::memset(loaded.bytes.data(), 0, loaded.bytes.size());
        const int decoded = mio0_decode(stage.data(), loaded.bytes.data(), nullptr);
        return decoded > 0;
    };

    if (lookup.compressed) {
        // Declared-compressed: unchanged gate -- require the MIO0 magic, else fail
        // exactly as before (return 0).
        if (!isMio0 || !stageAndDecodeMio0()) {
            return 0;
        }
    } else if (isMio0) {
        gdx_port_logf("[segload] MIO0-autodetect seg=%u romBase=%08X (binding said uncompressed)\n",
                      lookup.segment, lookup.romBase);
        // Some segments (notably the per-venue texture segments, e.g. Mute City's
        // D_A000000_235130) are MIO0-compressed in ROM but the asset bindings mark
        // them uncompressed. Copying the raw MIO0 bytes as texture data renders the
        // compressed stream directly — that is the "track stripes". Detect the MIO0
        // magic here and decompress regardless of the (wrong) compressed flag.
        // Keep loaded.compressed = lookup.compressed so the segment cache key still
        // matches future lookups (the cached bytes are already decompressed).
        if (!stageAndDecodeMio0()) {
            return 0;
        }
    } else {
        // Sizing bound: the ROM tail when a ROM is present (pre-shim behavior,
        // byte-identical), else the containing blob's span (archive-only boot --
        // the gate above guarantees at least one of the two exists).
        const size_t available = romUsable ? (gdx_rom_size - lookup.romBase)
                                           : static_cast<size_t>(blobSpan);
        // Structural invariant watchdog: the generator guarantees every blob
        // covers its segments' declared image sizes, but nothing at runtime enforced it. A
        // short blob would under-allocate below lookup.imageSize while downstream consumers
        // trust imageSize as the logical extent -- log loudly if a regenerated table ever
        // breaks the invariant (behavior otherwise unchanged: min() below already bounds).
        if (!romUsable && lookup.imageSize != 0 && available < lookup.imageSize) {
            gdx_port_logf("[segload] WARNING: blob span 0x%zX < declared imageSize 0x%zX for "
                          "seg=%u romBase=%08X (archive-only) -- generated blob table may be "
                          "out of sync with the segment map\n",
                          available, static_cast<size_t>(lookup.imageSize),
                          lookup.segment, lookup.romBase);
        }
        const size_t allocSize = lookup.imageSize != 0
            ? std::min<size_t>(lookup.imageSize, available)
            : std::min<size_t>(available, 8 * 1024 * 1024);
        if (allocSize == 0) {
            return 0;
        }

        loaded.bytes.resize(allocSize);
        std::memset(loaded.bytes.data(), 0, loaded.bytes.size());
        // Plain (uncompressed) families read straight into the destination via the
        // shim -- no staging needed. On an impossible read (shim returns 0) fail
        // exactly as the old bounds gate did.
        if (!GdxSegmentSourceRead(lookup.romBase, static_cast<uint32_t>(allocSize),
                                  loaded.bytes.data())) {
            return 0;
        }
    }

    // [venueload] Split the post-read half of the load. Boot-preloading the archive blobs made the
    // source read free and moved the measured cost NOT AT ALL (venue loads stayed 20-26 ms), so the
    // expense is somewhere below this line. The two candidates do very different amounts of work
    // per byte and need different fixes, so time them apart rather than reason about it.
    const auto gdxFixT0 = std::chrono::steady_clock::now();
    double gdxFixupMs = 0.0;
    double gdxRangesMs = 0.0;
    if (!loaded.bytes.empty()) {
        gdx_fixup_asset_segment_image(lookup.segment,
                                      lookup.romBase,
                                      loaded.bytes.data(),
                                      static_cast<unsigned int>(std::min<size_t>(loaded.bytes.size(), UINT32_MAX)));
        const auto gdxFixT1 = std::chrono::steady_clock::now();
        gdxFixupMs = std::chrono::duration<double, std::milli>(gdxFixT1 - gdxFixT0).count();
        gdx_register_asset_segment_command_ranges(
            lookup.segment,
            lookup.romBase,
            loaded.bytes.data(),
            static_cast<unsigned int>(std::min<size_t>(loaded.bytes.size(), UINT32_MAX)));
        gdxRangesMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gdxFixT1).count();
    }
    const double gdxDecodeMs = std::chrono::duration<double, std::milli>(gdxFixT0 - gdxDecodeT0).count();
    if ((gdxDecodeMs + gdxFixupMs + gdxRangesMs) > 1.0) {
        gdx_port_logf("[venueload] seg=%u bytes=%zu decode=%.2fms yields=%lu fixup=%.2fms ranges=%.2fms "
                      "hostRanges=%zu\n",
                      (unsigned) lookup.segment, loaded.bytes.size(), gdxDecodeMs,
                      gdx_yield_count - gdxYieldsBefore, gdxFixupMs, gdxRangesMs, gHostRanges.size());
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(loaded.bytes.data());
    /* First-claim store, intentionally NOT epoch-bracketed -- same graphics/game
       dual-reachability and 0 -> valid-immutable-pointer tolerance argument as the
       cache-hit claim above (see that comment). Bracketing here would corrupt the
       game-thread-only seqlock parity when the graphics thread reaches this. */
    if (gSegments[lookup.segment] == 0) {
        gSegments[lookup.segment] = base;
    }
    gHostRanges.push_back({ base, loaded.bytes.size() });
    gRawN64Ranges.push_back({ base, loaded.bytes.size() });
    gLoadedAssetSegments.emplace_back(std::move(loaded));
    // Converter invalidation chokepoint: a freshly decoded asset image may rebind
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
 *    (the "2D works / 3D fully dead" blackout).
 *  - D_2000000 (segment-2 BSS base, 1-byte LinkStubs token): live storage is
 *    D_80225800 via ResolvePortBssAlias. Taken verbatim it is also misaligned
 *    ((low32 & 7) != 0), so kOpMtx zeroed it and every frame fell back to the
 *    identity matrix ([datafail] op=DA raw=AAA694AC).
 * Route these back through the low32 resolver exactly like the asset
 * placeholders in IsAssetPlaceholderPointer. */
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

bool IsHostWideCommandPointer(uintptr_t full_addr) {
    return HostRangeListContains(gHostWideCommandRanges, full_addr);
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

bool ResolveRdramLow32(uint32_t raw, size_t requiredBytes, uintptr_t* outHost) {
    if (gdx_rdram == nullptr || outHost == nullptr) {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(gdx_rdram);
    const uint32_t offset = raw - Low32(base);
    if (offset >= static_cast<uint32_t>(GDX_RDRAM_SIZE) ||
        requiredBytes > static_cast<size_t>(GDX_RDRAM_SIZE) - offset) {
        return false;
    }

    const uintptr_t full = base + offset;
    if (ReadableByteLimit(full) < requiredBytes) {
        return false;
    }

    *outHost = full;
    return true;
}

bool ResolveRegisteredHostPointer(uint32_t raw, ResolvedAddress& out, size_t requiredBytes = 1) {
    // GDX_LEGACY_RESOLVE quarantine (see the block above kFallbackIdentityMtx):
    // this is the "registered-host low32-window match" guess the converter explicitly does
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
        /* requiredBytes: this previously accepted any range whose
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
// Binary N64 (8-byte) -> wide 16-byte boundary converter + cache.
//
// A narrow N64-format list (EK disk asset, ROM blob, or RDRAM-decoded segment)
// is converted ONCE to the wide layout the fast path consumes and
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

/* Segment-9 fallback probe, G2ResolvePhysical variant (see GdxSeg9FallbackDiag
 * further down for the full rationale -- this is the same scope-exit idea
 * adapted to G2ResolvePhysical's `uintptr_t* out_host` signature instead of
 * TryResolveAddress's ResolvedAddress&). Diagnosis only; env-gated on
 * GDX_LOG; bounded to the first 24 misses per process. Declared here (ahead
 * of N64DisplayListAdapter) so it is visible at G2ResolvePhysical's call
 * site, which runs before that class is defined in this translation unit. */
class GdxSeg9FallbackDiagRaw {
  public:
    GdxSeg9FallbackDiagRaw(bool armed, uint32_t raw, size_t requiredBytes, const uintptr_t* outHost)
        : mArmed(armed), mRaw(raw), mRequiredBytes(requiredBytes), mOutHost(outHost) {}
    ~GdxSeg9FallbackDiagRaw() {
        if (!mArmed) {
            return;
        }
        static int sLogs = 0;
        if (sLogs >= 24) {
            return;
        }
        ++sLogs;
        if (*mOutHost != 0) {
            gdx_port_logf("[seg9diag] G2ResolvePhysical fallback served seg9 token raw=%08X req=%zu -> host=%p\n",
                          mRaw, mRequiredBytes, reinterpret_cast<void*>(*mOutHost));
        } else {
            gdx_port_logf("[seg9diag] G2ResolvePhysical seg9 token raw=%08X req=%zu UNRESOLVED (mode resolver + "
                          "all fallbacks missed)\n",
                          mRaw, mRequiredBytes);
        }
    }
    GdxSeg9FallbackDiagRaw(const GdxSeg9FallbackDiagRaw&) = delete;
    GdxSeg9FallbackDiagRaw& operator=(const GdxSeg9FallbackDiagRaw&) = delete;

  private:
    bool mArmed;
    uint32_t mRaw;
    size_t mRequiredBytes;
    const uintptr_t* mOutHost;
};

bool G2ResolvePhysical(void* /*user*/, uint32_t raw, size_t required_bytes, uintptr_t* out_host) {
    /* Resolve exact, registered N64 overlay tokens before treating KSEG values
       as RDRAM. Expansion Kit display lists use original overlay VRAM addresses
       such as 0x80137528 for light structures; interpreting those as physical
       RDRAM silently feeds zeroed memory to the renderer. These registrations
       are authoritative token-to-host mappings, not low32 reconstruction
       guesses. Match TryResolveAddress's precedence, then fall through to the
       deterministic RDRAM paths used previously. */
    {
        uintptr_t modeAddress = 0;
        if (gdx_resolve_mode_segment9(raw, required_bytes, &modeAddress) != 0 &&
            ReadableByteLimit(modeAddress) >= required_bytes) {
            *out_host = modeAddress;
            return true;
        }
    }

    // Task 3 diag (Course Edit node-info panel scatter): see GdxSeg9FallbackDiagRaw
    // above. Armed only for genuine segment-9 tokens that just missed the
    // authoritative mode resolver; its destructor fires on whichever return
    // below actually serves (or fails to serve) this token. E5: also armed by
    // GDX_DIAG_NODEINFO, a dedicated opt-in for the [nodeinfo] investigation that
    // does not require enabling the broader GDX_LOG file-log sink.
    // Live gate reads (two inline int loads, no `static` latch) so arming the probe from
    // Dev Tools applies on the next resolution instead of requiring a restart.
    const bool seg9DiagEnabled = gdx_dev_gate(GDX_GATE_LOG_FILE) || gdx_dev_gate(GDX_GATE_DIAG_NODEINFO);
    GdxSeg9FallbackDiagRaw seg9Diag(seg9DiagEnabled && (raw >> 24) == 9u, raw, required_bytes, out_host);

    {
        const N64AddressRange* ranges = gN64AddressRanges.data();
        for (size_t ri = gN64AddressRanges.size(); ri > 0; ri--) {
            const N64AddressRange& range = ranges[ri - 1];
            if (raw < range.n64Begin) {
                continue;
            }
            const size_t offset = static_cast<size_t>(raw - range.n64Begin);
            if (offset <= range.size && required_bytes <= range.size - offset) {
                const uintptr_t host = range.hostBegin + offset;
                if (ReadableByteLimit(host) >= required_bytes) {
                    *out_host = host;
                    return true;
                }
            }
        }
    }

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
    return ResolveRdramLow32(raw, required_bytes, out_host);
}

void EnsureG2ConvertInit() {
    if (gG2ConvertInit) {
        return;
    }
    gG2ConvertInit = true;
    // Read ONCE here (not per frame): the converter's cache/context are wired at this point and
    // flipping the switch afterwards would leave already-converted lists behind. Dev Tools labels
    // the control "applies on restart" for exactly this reason.
    gG2ConvertEnabled = !gdx_dev_gate(GDX_GATE_NO_G2_CONVERT);
    // GDX_DIAG_NO_G2_CONVERT: plain-getenv diagnostic override of the same switch.
    //
    // GDX_GATE_NO_G2_CONVERT is Bucket B, so it is hard-wired to 0 in a Release build and the kill
    // switch is unreachable in exactly the binary the owner tests. This override exists because the
    // converter is the prime suspect for the interpolation flicker: display lists are converted to
    // the wide layout and CACHED LAZILY on first encounter (see the block comment above
    // gWideCache), which is a first-execution-only side effect -- and measurement shows the FIRST
    // sub-frame pass renders differently from every later one, while passes 1 and 2 are
    // byte-identical to each other. Turning the converter off makes every pass take the same narrow
    // path, so if the flicker disappears the lazy conversion is the cause.
    //
    // Diagnostic, not a shipping control: it forces the slower narrow path for the whole session.
    if (const char* e = getenv("GDX_DIAG_NO_G2_CONVERT")) {
        if (e[0] != 0 && strcmp(e, "0") != 0) {
            gG2ConvertEnabled = false;
            gdx_port_logf("[g2] converter DISABLED by GDX_DIAG_NO_G2_CONVERT (narrow path)\n");
        }
    }
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

bool ResolveGeneratedAssetStub(uint32_t raw, ResolvedAddress& out, size_t requiredBytes = 1) {
    AssetSegmentLookup lookup = {};
    if (!LookupAssetSegment(raw, lookup)) {
        return false;
    }

    /* E1 (editor resolver diagnosis): the interior match above originally had no
     * requiredBytes validation at all, so when a caller over-estimated the needed
     * byte count (e.g. EstimateRawTextureCopyBytes), an interior hit that did not
     * actually have that many bytes left in its declared image was accepted
     * anyway. That let this function steal seg-9/seg-7 tokens away from the
     * correct-but-stricter callers (gdx_resolve_mode_segment9, the mode-owned
     * carve) and hand back wrong-source bytes -- seg-9 tokens landing in the
     * machine_models ROM table (the nodeinfo scatter) and seg-7 EK tokens landing
     * in the cartridge expansion_kit_textures_beta blob (blank tooltips).
     *
     * F3 update: reject ONLY when the offset itself is out of bounds
     * (lookup.offset >= lookup.imageSize). A valid offset whose requiredBytes
     * estimate overshoots the declared image size is now ACCEPTED rather than
     * hard-rejected: block-rounded copy-size estimates can legitimately overshoot
     * the real image by one row (see NativeRgba16RangeRemaining's comment above
     * and the WIPE-transition LOADBLOCK case it documents), and the downstream
     * copy path already clamps the actual copy to what is readable
     * (`required = std::min(required, readable)` in TranslateTexturePointer), so
     * an inflated requiredBytes here can never cause an over-read. The original
     * goal of this check -- rejecting matches where an inflated estimate proves
     * the token really belongs to a mode-owned/other source -- is now covered by
     * the separate gdx_mode_owns_segment gate below (E2), so clamping instead of
     * hard-rejecting here is safe. */
    if (lookup.offset >= lookup.imageSize) {
        return false;
    }

    /* A mode-owned segment's live carve (gdx_load_seg4_if_needed /
     * gdx_load_seg7_if_needed / gdx_resolve_mode_segment9) is authoritative for
     * that segment; a ROM-backed AssetBindings.c row for the SAME segment number
     * is stale context that must not win over it BY DEFAULT. The original fix
     * hard-rejected every such row with no fallback, which was correct for the
     * editor's seg-9 scatter (a stale machine_models row while Course Edit's
     * course_edit_textures was active) but wrong for races: hud_gfx and
     * machine_global_gfx are mode-owned for the ENTIRE race (not just a
     * transition frame), so EVERY compiled-symbol reference into segments 4/7
     * (HUD digits/flag/energy bar, racer display lists) was starved for the
     * whole race. Segment 0x0A's live-carve redirect below
     * shows the correct shape: redirect into the live carve instead of
     * rejecting, when the carve is actually readable there. Generalized to any
     * mode-owned segment (4/7/9), gated on gdx_mode_segment_content_matches so a
     * row from a ROM family that is NOT the one currently resident in the carve
     * (e.g. that same stale machine_models-during-Course-Edit case, or the
     * never-port-loaded seg-4 course_edit_textures_beta family) still falls
     * through to a genuine reject -- keeping the original editor fix intact.
     * Bound check is ReadableByteLimit only, matching the proven 0x0A pattern
     * exactly: no clean per-segment active-size accessor exists for segments
     * 4/7, and segment 9's sGdxSeg9ActiveSize is already a stricter gate applied
     * by gdx_resolve_mode_segment9 with first refusal in TryResolveAddress, so
     * this is only reached as a permissive fallback net after that stricter
     * check has already failed. Segment 9 tokens get that first refusal before
     * this function ever runs, but ResolveWideAssetStubPointer's wide-pointer
     * path reaches this function directly, so the check must live here too. */
    if (gdx_mode_owns_segment(lookup.segment) != 0) {
        const uintptr_t live = gSegments[lookup.segment];
        if (live != 0 && gdx_mode_segment_content_matches(lookup.segment, lookup.romBase) != 0 &&
            ReadableByteLimit(live + lookup.offset) >= requiredBytes) {
            out.full = live + lookup.offset;
            out.segment = lookup.segment;
            out.offset = lookup.offset;
            out.segmented = true;
            return true;
        }
        {
            static int sE2RejectLogs = 0;
            if (sE2RejectLogs < 16) {
                ++sE2RejectLogs;
                gdx_port_logf("[e2-reject] seg=%u off=%X req=%zu mode-owned, live fallback unavailable\n",
                              static_cast<unsigned>(lookup.segment), lookup.offset, requiredBytes);
            }
        }
        return false;
    }

    /* Live-carve preference, segment 0x0A ONLY (early-race floor
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
     * refreshes -- a regression run showed the whole HUD + vehicles
     * garbled, unplayable FPS). The rank digits gain nothing from the live
     * carve either: [digit-carve] proved gSegments[4]+0x13DE0 is zero at race
     * time (the console's runtime fill has no port equivalent yet). This
     * segment is never mode-owned (gdx_mode_owns_segment has no 0x0A case), so
     * it is handled here as its own case rather than folded into the
     * mode-owned block above. */
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
/* Wide-DL regression fix: game-BUILT wide DLs carry the REAL
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
/* E3 (replaces the old hand-listed 23-symbol kEkNamedAssetStubs table, Fix A1's
 * original "module->asset shim gap" fix), generalized by A1/A2 (fireworks /
 * sCourseMinimapPalette) beyond its original Expansion-Kit-only scope: any REAL,
 * full-size host array whose own address SETTIMG can carry directly -- not a
 * generated 1-byte LinkStubs placeholder -- can be registered here so
 * ResolveWideAssetStubPointer RECOGNIZES it instead of miscounting it as an
 * unbound stub. Originally populated only from gdx_ek_assets_fill()'s loop (EK
 * disk assets, port/gen/EkAssetBindings.c / tools/gen_ek_assets.py, covering the
 * full ~773-entry sEkAssetFills[] table instead of a hand-maintained subset);
 * now also populated once at init from port/decomp_port.c for base-game compiled
 * -in arrays with the same "unbound stub" false positive (the ending fireworks
 * sprites D_i7_8014ADA8/AE30/AEB8 and sCourseMinimapPalette). The resolved
 * pointer is unchanged either way (delta added back onto the array's own
 * address) -- this table only affects RECOGNITION, never the delivered bytes;
 * pair it with gdx_set_native_rgba16_texture_range when the array's compiled
 * bytes also need the host-endian byteswap (see the fireworks registration).
 *
 * Interior-offset support (course_edit/191080.c's node-info number strip
 * indexes aCourseEditNumberSheetTex at +0x120/+0x240 for later digit-glyph
 * bands): same delta<size unsigned-wraparound match gdx_lookup_asset_segment_
 * interior uses for the ROM-backed table (AssetBindings.c), not an exact-only
 * comparison.
 *
 * gdx_register_host_pointer_stub itself is defined further down, alongside the
 * other gdx_register_* host-range functions, OUTSIDE this anonymous namespace
 * (matching this file's own convention for extern "C" definitions -- see
 * gdx_record_dma_load's "temporarily close to define extern "C"" split above). */
bool ResolveHostPointerStub(uint32_t raw, ResolvedAddress& out) {
    for (const HostRange& entry : gHostPointerStubs) {
        const uint32_t base = Low32(entry.begin);
        const uint32_t delta = raw - base;
        if (delta < entry.size) {
            out.full = entry.begin + delta;
            out.segment = 0;
            out.offset = delta;
            out.segmented = false;
            return true;
        }
    }
    return false;
}

bool ResolveVenueBankAlias(uint32_t raw, ResolvedAddress& out); // fwd decl (defined below)
uintptr_t ResolveWideAssetStubPointer(uintptr_t full, uintptr_t moduleBegin, uintptr_t moduleEnd,
                                       size_t requiredBytes = 1) {
    if (full == 0) {
        return 0;
    }
    /* Exact-symbol matchers run FIRST, before the coarse module-range gate below.
       They compare `full`'s low32 against the KNOWN addresses of specific stub
       symbols, so a hit is unambiguous and needs no range pre-filter. This order
       is load-bearing: on Linux PIE, GetMainModuleRange under-covers the anonymous
       .bss tail where the venue-bank stubs (D_A000000..D_A008000) live, so gating
       exact matching behind the range check dropped every venue bank and the track
       floor/walls/pipes sampled the raw zero stub byte (solid black). Exact
       resolution is safe regardless of the range: the stub's own address is what
       matched. */
    const uint32_t low = Low32(full);
    ResolvedAddress out = {};
    if (ResolveVenueBankAlias(low, out)) {
        return out.full;
    }
    if (ResolveGeneratedAssetStub(low, out, requiredBytes)) {
        return out.full;
    }
    // A1/A2 generalized this beyond EXPANSION_KIT (see ResolveHostPointerStub's
    // comment): base-game arrays register here too, so this check always runs.
    if (ResolveHostPointerStub(low, out)) {
        return out.full;
    }
    /* Coarse module-range gate: preserved to guard any range-scoped resolution
       that follows the exact matchers. A `full` outside the EXE module is not a
       generated stub and must not be reinterpreted. Nothing range-scoped exists
       below yet, so a miss falls through to 0 either way. */
    if (moduleBegin == 0 || full < moduleBegin || full >= moduleEnd) {
        return 0;
    }
    return 0;
}

bool ResolveVenueBankAlias(uint32_t raw, ResolvedAddress& out) {
    if (gSegments[0x0A] == 0) {
        return false;
    }
    // {stub symbol, byte offset within the venue segment image}. The first 11
    // banks are the uniform 0x1000-byte texture banks; the EK Road-Type panel
    // icons that follow are NOT bank-aligned (576-byte RGBA16 24x12 icons, see
    // the D_A00B000.. extern declarations above), so offsets are listed
    // explicitly rather than derived by multiplying an index.
    struct BankEntry {
        uint32_t low32;
        uint32_t offset;
    };
    static const BankEntry kBanks[] = {
        { Low32(reinterpret_cast<uintptr_t>(D_A000000)), 0x0000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A001000)), 0x1000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A002000)), 0x2000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A003000)), 0x3000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A004000)), 0x4000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A005000)), 0x5000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A006000)), 0x6000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A007000)), 0x7000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A008000)), 0x8000u },
        // E4: banks 9-10 (no current reference, registered for symmetry -- see the
        // extern declarations above).
        { Low32(reinterpret_cast<uintptr_t>(D_A009000)), 0x9000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A00A000)), 0xA000u },
#ifdef EXPANSION_KIT
        // Road-Type panel icons (gRoadTypeMenuItems/gHRoadTypeMenuItems/
        // gTRoadTypeMenuItems, decomp/src/overlays/expansion_kit/A3AE0.c) --
        // EK-only, real per-venue RGBA16 24x12 icons confirmed against every
        // base-game venue texture yaml (see the extern declarations above).
        { Low32(reinterpret_cast<uintptr_t>(D_A00B000)), 0xB000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A00B240)), 0xB240u },
        { Low32(reinterpret_cast<uintptr_t>(D_A00B480)), 0xB480u },
        { Low32(reinterpret_cast<uintptr_t>(D_A00B6C0)), 0xB6C0u },
        { Low32(reinterpret_cast<uintptr_t>(D_A00B900)), 0xB900u },
        { Low32(reinterpret_cast<uintptr_t>(D_A00BB40)), 0xBB40u },
        { Low32(reinterpret_cast<uintptr_t>(D_A00BD80)), 0xBD80u },
#endif
    };
    for (const BankEntry& bank : kBanks) {
        if (raw == bank.low32) {
            out.full = gSegments[0x0A] + bank.offset;
            out.segment = 0x0A;
            out.offset = bank.offset;
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

    // Archive-only fix: same shim-aware gate as EnsureAssetSegmentImage -- the ROM
    // is only required when no archive blob contains this range (the shim read
    // below fails cleanly either way; this just avoids a pointless allocation).
    uint32_t setupSpan = 0;
    if (GdxSegmentSourceContainingSpan(static_cast<uint32_t>(kSetupGfxRomOffset), &setupSpan) == 0 &&
        ((gdx_rom_buffer == nullptr) || (gdx_rom_size < kSetupGfxRomOffset + kSetupGfxSize))) {
        return 0;
    }

    gSetupGfxSegment.resize(kSetupGfxSize);
    // This hardcoded 0x17B1E0/0x778 fallback exists ONLY for a
    // missing-binding regression -- D_3000000 is present in sAssetSegmentMap, so
    // the EnsureAssetSegmentForSymbol primary path above always resolves and this
    // is dead in normal operation. Route the raw byte read through the single
    // byte-source shim anyway; the per-word BE32 host-endian swap below is preserved
    // byte-for-byte.
    std::vector<uint8_t> raw(kSetupGfxSize);
    if (!GdxSegmentSourceRead(static_cast<uint32_t>(kSetupGfxRomOffset),
                              static_cast<uint32_t>(kSetupGfxSize), raw.data())) {
        return 0;
    }
    for (size_t i = 0; i < kSetupGfxSize; i += sizeof(uint32_t)) {
        const uint32_t word = ReadBE32(raw.data() + i);
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

#if defined(_WIN32)
bool IsReadablePageProtect(uint32_t protect);

struct WindowsMemoryRegion {
    uintptr_t begin;
    uintptr_t end;
    bool readable;
};

static std::vector<WindowsMemoryRegion> sWindowsMemoryRegions;

static const WindowsMemoryRegion* FindWindowsMemoryRegion(uintptr_t address) {
    size_t low = 0;
    size_t high = sWindowsMemoryRegions.size();
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const WindowsMemoryRegion& region = sWindowsMemoryRegions[middle];
        if (address < region.begin) {
            high = middle;
        } else if (address >= region.end) {
            low = middle + 1;
        } else {
            return &region;
        }
    }
    return nullptr;
}

static bool WindowsMemoryRegionFor(uintptr_t address, WindowsMemoryRegion& out) {
    const WindowsMemoryRegion* cached = FindWindowsMemoryRegion(address);
    if (cached != nullptr) {
        out = *cached;
        return true;
    }

    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0) {
        return false;
    }

    const uintptr_t begin = reinterpret_cast<uintptr_t>(info.BaseAddress);
    if (info.RegionSize == 0 || info.RegionSize > UINTPTR_MAX - begin) {
        return false;
    }

    WindowsMemoryRegion region = {
        begin,
        begin + info.RegionSize,
        info.State == MEM_COMMIT && IsReadablePageProtect(info.Protect),
    };
    if (!region.readable) {
        out = region;
        return address >= out.begin && address < out.end;
    }
    auto insertAt = std::lower_bound(
        sWindowsMemoryRegions.begin(), sWindowsMemoryRegions.end(), region.begin,
        [](const WindowsMemoryRegion& existing, uintptr_t value) {
            return existing.begin < value;
        });
    insertAt = sWindowsMemoryRegions.insert(insertAt, region);
    out = *insertAt;
    return address >= out.begin && address < out.end;
}

static void ResetWindowsMemoryRegionCache() {
    sWindowsMemoryRegions.clear();
    if (sWindowsMemoryRegions.capacity() == 0) {
        sWindowsMemoryRegions.reserve(64);
    }
}
#else
// ---------------------------------------------------------------------------------------------
// POSIX memory-probe backend: a snapshot of /proc/self/maps with miss-triggered refresh.
//
// The Windows probes (VirtualQuery) answer "is this address readable, and how far does the
// containing region extend?" per call. On Linux the equivalent is /proc/self/maps. Re-reading and
// parsing that file on every probe (this bridge calls ReadableByteLimit thousands of times per
// frame) would be far too slow, so we snapshot it once and re-parse only on a MISS -- an address
// not found in the snapshot triggers exactly one re-parse then re-query. That handles regions
// mmap'd after boot (the RDRAM calloc, fiber stacks, late texture arenas) without a watcher
// thread. Readable regions are coalesced at parse time so a "rest of the block" answer comes out
// comparable to VirtualQuery's region-spanning result.
//
// Threading: these probes run only on the single graphics thread. The atomic generation counter
// is belt-and-suspenders for the re-parse and is otherwise unused.
// ---------------------------------------------------------------------------------------------
struct MapsRegion {
    uintptr_t begin;
    uintptr_t end;
    bool      readable;
};
static std::vector<MapsRegion> sMaps;               // sorted by begin, readable runs coalesced
static std::atomic<uint32_t>   sMapsGeneration{0};

static void ParseProcMaps() {
    std::vector<MapsRegion> parsed;
    FILE* f = std::fopen("/proc/self/maps", "r");
    if (f != nullptr) {
        char line[512];
        while (std::fgets(line, sizeof(line), f) != nullptr) {
            unsigned long long b = 0, e = 0;
            char perms[8] = {0};
            // Line shape: "begin-end perms offset dev inode pathname".
            if (std::sscanf(line, "%llx-%llx %7s", &b, &e, perms) == 3) {
                MapsRegion r;
                r.begin = static_cast<uintptr_t>(b);
                r.end = static_cast<uintptr_t>(e);
                r.readable = (perms[0] == 'r');
                parsed.push_back(r);
            }
        }
        std::fclose(f);
    }

    std::sort(parsed.begin(), parsed.end(),
              [](const MapsRegion& a, const MapsRegion& b) { return a.begin < b.begin; });

    // Coalesce touching readable regions so a limit query spans the whole run, like VirtualQuery.
    std::vector<MapsRegion> coalesced;
    for (const MapsRegion& r : parsed) {
        if (!coalesced.empty() && coalesced.back().readable && r.readable &&
            coalesced.back().end == r.begin) {
            coalesced.back().end = r.end;
        } else {
            coalesced.push_back(r);
        }
    }

    sMaps.swap(coalesced);
    sMapsGeneration.fetch_add(1, std::memory_order_relaxed);
}

// Binary-search the snapshot for the region containing `addr`.
static const MapsRegion* FindMapsRegion(uintptr_t addr) {
    size_t lo = 0;
    size_t hi = sMaps.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (addr < sMaps[mid].begin) {
            hi = mid;
        } else if (addr >= sMaps[mid].end) {
            lo = mid + 1;
        } else {
            return &sMaps[mid];
        }
    }
    return nullptr;
}

// Look up `addr`, re-parsing once on a miss (late mmap). Copies the region out by value so the
// result stays valid even though a subsequent probe may re-parse and reallocate sMaps.
static bool PosixRegionFor(uintptr_t addr, MapsRegion& out) {
    if (sMaps.empty()) {
        ParseProcMaps();
    }
    const MapsRegion* r = FindMapsRegion(addr);
    if (r == nullptr) {
        ParseProcMaps(); // one re-parse then re-query
        r = FindMapsRegion(addr);
    }
    if (r == nullptr) {
        return false;
    }
    out = *r;
    return true;
}
#endif

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
#else
    // Mirror "module base + SizeOfImage": take the contiguous run of /proc/self/maps entries whose
    // pathname is the main executable, using begin-of-first .. end-of-last. dladdr on a local
    // function gives the load base as a cross-check; the executable path comes from readlink of
    // /proc/self/exe (dli_fname is a fallback, since it can be a relative/short name).
    Dl_info info;
    std::memset(&info, 0, sizeof(info));
    const bool haveDl = dladdr(reinterpret_cast<void*>(&GetMainModuleRange), &info) != 0;

    char exePath[4096];
    ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    const char* wantPath = nullptr;
    if (n > 0) {
        exePath[n] = '\0';
        wantPath = exePath;
    } else if (haveDl && info.dli_fname != nullptr && info.dli_fname[0] != '\0') {
        wantPath = info.dli_fname;
    }
    if (wantPath == nullptr) {
        return;
    }

    FILE* f = std::fopen("/proc/self/maps", "r");
    if (f == nullptr) {
        return;
    }
    char line[4608];
    uintptr_t lo = 0, hi = 0;
    bool found = false;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        unsigned long long b = 0, e = 0;
        char perms[8] = {0};
        char path[4096] = {0};
        // begin-end perms offset dev inode <spaces> pathname
        int matched = std::sscanf(line, "%llx-%llx %7s %*x %*s %*u %4095[^\n]", &b, &e, perms, path);
        if (matched < 3) {
            continue;
        }
        char* p = path;
        while (*p == ' ') {
            ++p;
        }
        if (matched == 4 && std::strcmp(p, wantPath) == 0) {
            if (!found) {
                lo = static_cast<uintptr_t>(b);
                found = true;
            }
            hi = static_cast<uintptr_t>(e);
        }
    }
    std::fclose(f);

    if (found) {
        moduleBegin = lo;
        moduleEnd = hi;
    } else if (haveDl && info.dli_fbase != nullptr) {
        // Pathname match failed (unusual): fall back to the dladdr load base alone. Without an end
        // we cannot bound the module, so leave moduleEnd at 0 -- callers treat {base,0} the same
        // as {0,0} (an empty range), i.e. no worse than the Windows failure path.
        moduleBegin = reinterpret_cast<uintptr_t>(info.dli_fbase);
    }
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
    const uintptr_t address = reinterpret_cast<uintptr_t>(source);
    WindowsMemoryRegion region;
    if (!WindowsMemoryRegionFor(address, region) || !region.readable) {
        return 0;
    }

    if (address >= region.end) {
        return 0;
    }

    return static_cast<size_t>((region.end - address) / stride);
#else
    const uintptr_t addr = reinterpret_cast<uintptr_t>(source);
    MapsRegion r;
    if (!PosixRegionFor(addr, r) || !r.readable) {
        return 0;
    }
    if (addr >= r.end) {
        return 0;
    }
    return static_cast<size_t>((r.end - addr) / stride);
#endif
}

size_t ReadableByteLimit(uintptr_t address) {
#ifdef _WIN32
    WindowsMemoryRegion region;
    if (!WindowsMemoryRegionFor(address, region) || !region.readable) {
        return 0;
    }

    if (address >= region.end) {
        return 0;
    }

    return static_cast<size_t>(region.end - address);
#else
    MapsRegion r;
    if (!PosixRegionFor(address, r) || !r.readable) {
        return 0;
    }
    if (address >= r.end) {
        return 0;
    }
    return static_cast<size_t>(r.end - address);
#endif
}

bool IsReadableAddress(uintptr_t address) {
#ifdef _WIN32
    WindowsMemoryRegion region;
    if (!WindowsMemoryRegionFor(address, region)) {
        return false;
    }
    return region.readable;
#else
    MapsRegion r;
    if (!PosixRegionFor(address, r)) {
        return false;
    }
    return r.readable;
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
    std::memset(out, 0, 64);
    gPersistentAllocations.push_back(std::move(alloc));

    /* Defense in depth, same idiom as MakePersistentVtxCopy's
       [vtx-clamp] above: callers are expected to have validated 64 bytes readable
       at `source` before reaching here (see kOpMtx's CRASH FAILSAFE, hoisted to
       run pre-copy), but clamp internally too so ANY caller -- present or future,
       including the second MakePersistentMtxCopy call site in the kOpVtx
       F3D-remapped-to-Mtx branch -- cannot turn an under-validated resolution
       into an out-of-bounds host read. The buffer is zeroed above, so a clamped
       tail reads as benign zero words rather than garbage; a fully-unreadable
       `source` (readable == 0) therefore yields a fully-zeroed, degenerate 64-byte
       matrix rather than a null return -- a deliberate divergence from the primary
       kOpMtx call site, which drops the whole command (outW1 = 0, never reaching
       this function) on the same under-validated condition. */
    const size_t readable = ReadableByteLimit(source);
    const size_t safeWords = std::min<size_t>(16, readable / 4);
    if (readable < 64) {
        static int sMtxClampLogs = 0;
        if (sMtxClampLogs < 40) {
            ++sMtxClampLogs;
            gdx_port_logf("[mtx-clamp] source=%p requested=64B readable=%zuB clampedWords=%zu\n",
                          reinterpret_cast<void*>(source), readable, safeWords);
        }
    }

    const uint32_t* in_w = reinterpret_cast<const uint32_t*>(source);
    uint32_t* out_w = reinterpret_cast<uint32_t*>(out);
    for (size_t i = 0; i < safeWords; i++) {
        out_w[i] = Byteswap32(in_w[i]);
    }
    return reinterpret_cast<uintptr_t>(out);
}

/* True when a texture/TLUT source address lands inside the two live GfxPools
 * (D_8024DCE0[2]).
 *
 * WHY THIS EXISTS -- the "Silence moon is bright pink after racing Mute City 2"
 * defect (venue background sprite palettes):
 *
 * MakePersistentRawTextureCopy below classifies a source as IMMUTABLE when it
 * belongs to any registered host range ("ROM-backed textures are stable after
 * the segment is loaded; skip memcmp"). That is correct for every asset-segment
 * carve, the ROM buffer and the audio heap -- but the GfxPools are registered
 * too (port/decomp_port.c:116, gdx_register_host_range(D_8024DCE0, ...)), and
 * they are RAM scratch that game logic rewrites EVERY FRAME. Classifying them
 * as immutable means the very first persistent copy taken at a given pool
 * address is served for the rest of the process, no matter how the game
 * rewrites those bytes afterwards.
 *
 * The nighttime background sprites are the case where that is visible. Their
 * CI4 TLUTs are staged into the live pool each frame
 * (decomp/src/overlays/ovl_i3/background.c:1376,
 * gGfxPool->unk_2C528[i][j] = spritePaletteReplacement->palette[j]) and the
 * display list binds them from the pool through the segment-1 alias
 * (background.c:1438, gDPLoadTLUT_pal16(gfx++, 0, D_1000000.unk_2C528[idx]),
 * rerouted to gSegments[1] + offset by IsPortBssAliasPointer/TryResolveAddress).
 * Slot index is per-course and assigned from 0 upward in first-seen order
 * (background.c:1278-1306), so EVERY night course reuses slots 0..N-1 at the
 * SAME two host addresses (one per pool parity).
 *
 * Consequence: race Mute City 2 (slot 0 = night-city skyline 1's palette
 * D_F238C10) and the 32-byte copy for that address is minted holding the
 * skyline palette. Enter Silence: the moon's staged palette D_F242210 is
 * written into the same pool slot, but the copy is treated as immutable and
 * never refreshed, so the interpreter's LOADTLUT reads the skyline palette and
 * the moon's (correct) CI4 indices decode bright pink -- exactly the palette
 * the reconstruction identified. Slot 1 does the same to the Moon Base station.
 * Cold-booting straight into Silence is CORRECT because then the first copy
 * ever minted at those addresses is Silence's own palette.
 *
 * The refresh path (the `changed` branch below) is already the right mechanism:
 * it re-copies in place AND queues the buffer for TextureCacheDelete, so LUS
 * re-imports from the updated bytes on the frame that produced them. It simply
 * never ran for pool-backed sources. Excluding the pools from the immutable
 * fast path restores it. The comparison cost is trivial: a pool-backed texture
 * source is only ever one of these 16-entry TLUTs (32 bytes), never a bulk
 * texture -- the pool's other display-list-referenced members are vertices and
 * matrices, which use MakePersistentVtxCopy/MakePersistentMtxCopy instead.
 *
 * Deliberately NOT extended to D_1000000: that object is the never-written BSS
 * alias (a translate-time stand-in only, port/decomp_port.c:1324), so a memcmp
 * against it could never observe a change. */
extern "C" size_t gdx_gfxpool_sizeof(void);
bool IsGfxPoolHostRange(uintptr_t source) {
    const size_t poolSize = gdx_gfxpool_sizeof();
    if (poolSize == 0) {
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(&D_8024DCE0[0]);
    const size_t span = poolSize * 2; // GfxPool D_8024DCE0[2]
    return (source >= base) && (source < base + span);
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
            if (IsNativeRgba16Range(source, copyBytes)) {
                /* Transition captures live in the back arena.  That address is
                   deliberately reused for the next transition, so treating a
                   registered host range as immutable leaves the persistent
                   texture copy containing the previous screen.  Compare in
                   the byte order stored by CopyRawTextureBytes instead. */
                changed = !NativeRgba16CopyMatches(copy.bytes.get(), source, copyBytes);
            } else if (IsRdramHostPointer(source) || IsN64FramebufferRange(source, copy.size)) {
                changed = HostRangeChanged(source, copy.size, copy.dmaGenAtCopy);
            } else {
                // ROM-backed textures are stable after the segment is loaded; skip memcmp.
                // The GfxPools are registered host ranges too but are per-frame RAM scratch,
                // so they must stay on the compare path -- see IsGfxPoolHostRange above.
                const bool stableSource = RegisteredHostRemaining(source) > 0 && !IsGfxPoolHostRange(source);
                if (!stableSource) {
                    changed = (std::memcmp(copy.bytes.get(), reinterpret_cast<const void*>(source), copyBytes) != 0);
                }
            }
        }

        /* Verification aid for the background-sprite TLUT fix (default OFF).
           GDX_DIAG_POOL_TEX=1 logs the first 8 bytes of every pool-backed
           texture/TLUT source and whether this frame's compare saw a change.
           Entering a night course, the first line for a given source must show
           chg=1 -- that is the previously-missing refresh actually happening. */
        static const bool sDiagPoolTex = std::getenv("GDX_DIAG_POOL_TEX") != nullptr;
        if (sDiagPoolTex && copyBytes >= 8 && IsGfxPoolHostRange(source)) {
            static int sPoolTexLogs = 0;
            if (sPoolTexLogs < 200) {
                ++sPoolTexLogs;
                const uint8_t* s = reinterpret_cast<const uint8_t*>(source);
                gdx_port_logf("[pool-tex] src=%p bytes=%zu chg=%d %02X%02X %02X%02X %02X%02X %02X%02X\n",
                              reinterpret_cast<void*>(source), copyBytes, changed ? 1 : 0,
                              s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
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

// =============================================================================================
// P0 — Matrix-interpolation retention + scratch-slot indirection (DEBUG-ONLY, default-OFF)
// =============================================================================================
// Ships nothing user-visible. Env-gated by GDX_INTERP_P0 (read once); every hook is a strict
// no-op with zero allocation on the normal path.
//
// RETENTION INVARIANT: the resolved command buffer (the adapter's per-list `commands` vectors) and
// every input the interpreter dereferences stay valid and re-executable within one tick. Those
// inputs are: `converted` (owned by the local N64DisplayListAdapter, valid until gdx_gfx_run
// returns); interp->mSegmentPointers[0..15] (a gSegments snapshot taken just before ConvertRoot);
// the bytes those commands point at -- GfxPool matrices in segment-1 RDRAM, persistent copies in
// gPersistentAllocations (freed only AFTER Run, so a replay must happen BEFORE that free), vertex
// staging, and texture sources; and the latched ucode/F3dex2 variant.
//
// SCRATCH-SLOT TRANSPARENCY: at G_MTX translation, pool-span matrices are copied into a stable
// per-tick scratch slot and the resolved command's pointer is rewritten to it, recording
// (origPtr, scratchPtr). At t=1 the scratch IS a byte copy of the pool matrix, so interpreter
// output is identical (checked by memcmp -- GdxP0TransparencyViolations()). Non-pool matrices
// (static/persistent, isBig -> MakePersistentMtxCopy) resolve outside the pool span and pass
// through untouched.
//
// EVIDENCE (no GPU readback): an FNV-1a over the resolved command words is logged before each
// pass. Byte-identical hashes across passes are what prove the retained buffer is stable and that
// the interpreter does not mutate it in place; any per-command operand mutation is counted and
// reported rather than hidden. A differing cmdhash would mean P2 replay must snapshot/restore the
// buffer per pass.
//
// A genuine second interp->Run() replays the SAME retained buffer at t=1. That is safe on the
// shipping DX11 backend because Run() clears+draws+MSAA-resolves but does NOT present (present is
// interp->EndFrame()/SwapBuffers, host-called once). See the block in gdx_gfx_run.

struct GdxP0Mtx {
    uint64_t w[8];
}; // 64 bytes; alignof 8 satisfies the interpreter's (ptr & 7) == 0 matrix-alignment check.
static_assert(sizeof(GdxP0Mtx) == 64, "N64 Mtx is 64 bytes");

// P0 activation. Everything downstream is a no-op when this is false. Bucket B (it changes what is
// rendered), so in a build without GDX_DEV_TOOLS the gate is a compile-time 0 and the whole P0 path
// is dead. The value is sampled once per gfx task (see mP0Enabled in the caller), not per command,
// so a mid-task flip cannot desync the two-pass scratch bookkeeping.
static bool GdxInterpP0Enabled() {
    return gdx_dev_gate(GDX_GATE_INTERP_P0) != 0;
}

// ===== Host-driven decoupled-loop configuration (set by port/main.cpp per iteration) =====
// The host configures each tick's sub-frame schedule BEFORE gdx_dispatch; gdx_gfx_run (which runs
// inside dispatch) consumes it. Single-threaded (graphics/main thread only) — no locking. All of
// this is inert unless gdx_interp::P2HostActive() and the host set active=1 for the tick.
namespace {
struct GdxInterpHostCfg {
    bool active = false;       // host enabled the decoupled present loop for this tick
    double tickStart = 0.0;    // now-fn timestamp at the top of this host iteration
    double tickDuration = 0.0; // one 60 Hz logic-tick budget in now-fn units (~1.001/60 s)
    int maxSubframes = 4;      // VSync-off cap: presents don't block, so bound the loop
};
GdxInterpHostCfg gGdxInterpHostCfg;
GdxInterpNowFn gGdxInterpNowFn = nullptr;
bool gGdxInterpPresentedLastTick = false;
int gGdxInterpLastSubframes = 0;
double gGdxInterpLastT = 1.0;
// Real-FPS visibility: last-tick lerp/snap counts surfaced by the Stats page
// and the [interp-p2] line, plus a rolling presented-frames-per-second meter. The meter counts
// EVERY sub-frame present in the decoupled loop over a ~0.5 s window (wall clock via the host
// now-fn) so the menu can show true presents/sec ("144 fps (sim 60 Hz)") rather than logic ticks.
/* [interp-idem] see the sub-frame loop: cumulative count of ticks whose replays bound DIFFERENT
   textures than pass 0, i.e. ticks where re-executing the display list was NOT idempotent. */
extern "C" void gdx_gfx_texbind_hash_reset(void);
extern "C" unsigned long long gdx_gfx_texbind_hash(void);
static unsigned long long sGdxIdemPass0Hash = 0;
static bool sGdxIdemTickCounted = false;
/* [interp-shot] >=0 while a sub-frame render should be captured; the pass index becomes the file
   suffix. Set by the sub-frame loop, consumed by gdx_gfx_post_run_capture below. */
int gGdxShotArmedPass = -1;
/* [interp-idem] Interpreter RDP state as it stood before this tick's FIRST replay. Restored before
   every later replay so all M sub-frames start from identical state. See the sub-frame loop. */
static Fast::RDP sGdxRdpSnapshot{};
size_t gGdxIdemDivergentTicks = 0;
size_t gGdxIdemMultiPassTicks = 0;

size_t gGdxInterpLastLerped = 0;
/* [interp-pair] These ACCUMULATE across ticks. The telemetry line prints one tick in every 120,
   so a per-tick snapshot made a low-rate mispairing statistically invisible -- the first version of
   this probe reported 2 hits in 34 samples, which proved nothing either way. Totals plus a
   window max are what make the reading decisive. */
float gGdxInterpPairMaxDelta = 0.0f;   // max delta since the last read (reader resets)
size_t gGdxInterpPairSuspect = 0;      // cumulative suspicious pairings since boot
size_t gGdxInterpPairLerped = 0;       // cumulative paired slots -- the denominator
size_t gGdxInterpLastSnapped = 0;
// Sub-frames the swapchain limiter refused this tick. These used to be counted as presented, so
// every rate reading was an upper bound and a heavy tick looked healthy in the log while dropping
// frames on screen. A non-zero value here means the tick did not fit its budget.
int gGdxInterpLastDropped = 0;
double gGdxInterpPresentsPerSec = 0.0;
int gGdxInterpPresentWindowCount = 0;
double gGdxInterpPresentWindowStart = -1.0;
// Snapshot read by the adapter ctor: keeps the per-tick "is P2 host mode on" decision consistent
// with the branch main.cpp already committed to for this tick (main.cpp set gGdxInterpHostCfg.active
// via gdx_gfx_interp_tick_config before dispatch), rather than re-reading the CVar mid-tick.
inline bool GdxP2HostConfigured() { return gGdxInterpHostCfg.active; }

// Tick-boundary latch for the referenced-offset set.
//
// gdx_gfx_run -- and therefore GdxInterpBeginTick -- executes ONCE PER GFX TASK, and the game
// submits 2-6 tasks per 60 Hz tick (measured directly: the [interp-geo] census emits 4, 5 and 6
// line groups under a single tick id). Rolling the referenced-offset set inside gdx_gfx_run
// therefore answered "was this offset referenced last tick?" against the PREVIOUS TASK's set:
//
//   task 1  clear -> note {A,B} -> commit          prev={A,B}
//   task 2  clear -> note {C,D} vs prev={A,B}      not present -> SNAP
//   task 3  clear -> note {E}   vs prev={C,D}      not present -> SNAP
//   next tick task 1: note {A,B} vs prev={E}       not present -> SNAP
//
// Slots that should lerp snapped instead, and when every slot in a task snapped the `degenerate`
// check in the sub-frame loop forced t=1 on all M passes -- interpolation rendered M identical
// frames while still paying for them. The host sets this flag at the real tick boundary
// (gdx_gfx_interp_tick_config, once per iteration before dispatch) and the tick's FIRST task
// consumes it. gdx_interp.h's claim that BeginTick runs "EXACTLY ONCE per rendered tick" is what
// this restores; it was not true as written.
bool gGdxInterpNewTick = false;
int gGdxInterpTasksThisTick = 0;
int gGdxInterpLastTasks = 0;
// The cut epoch latched for the WHOLE tick. CutPendingForThisTick() is a consume-once "changed
// since the last call" edge, so calling it per task meant the tick's first task ate the cut and
// every later task saw false -- half a frame snapped and the other half lerped straight across the
// cut, which is precisely the whole-frame semantic the cut exists to provide. Latched once at the
// tick boundary and read by every task in that tick.
bool gGdxInterpCutThisTick = false;
} // namespace

// P3: the in-race pause flag (decomp/src/game/game.c). The bridge reads it directly — the
// same way it already reads gSegments/D_800DCCFC/D_8024DCE0 — so pause handling stays on the
// interpolation branch: a paused tick forces a single crisp t=1 present here while the host's
// logic-deadline pacer still holds 60 Hz. Routing pause through main.cpp's default path instead
// would hand pacing to gdx_frame_pacer_tick(), which is a strict no-op while FrameInterpolation is
// on (mutual exclusion), and would free-run the present on a VSync-off panel. Read-only.
extern "C" { extern signed char gGamePaused; }

// Implemented in libultraship/src/fast/Fast3dWindow.cpp. Overrides the DXGI software rate limiter's
// verdict while the interpolation sub-frame loop is driving presents; the swapchain's waitable
// object provides the actual pacing. See the block comment at the definition.
extern "C" void gdx_fast3d_set_subframe_present(int on);

// P4: detect a pending transition background-capture THIS tick, so the capture
// reads a CANONICAL t=1 frame and never a tween. The decomp's `Transition sTransition`
// (ovl_i2/transition.h; external linkage) has TRANSITION_FLAG_SET_BACKGROUND_BUFFER set in
// Transition_Update (sys_gfx.c frame-pump step func_800690FC, sys_gfx.c:204) and cleared only inside
// Transition_SetBackgroundBuffer (transition.c:802) — which reads our frame mirror via
// gdx_read_current_framebuffer at sys_gfx.c:219, LATER in the SAME tick than this gdx_gfx_run.
//
// ORDERING PROOF (sys_gfx.c:202-219, transition.c:518-535, n64_sched.c:915, main.cpp:873-883): the
// whole game frame runs inside one gdx_vi_tick. In program order:
//   func_800690FC (Transition_Update: SETS the flag)  ->  func_80069698 (Transition_Draw: builds DL)
//   -> Gfx_FullSync (posts GFX_TASK_SET) -> the game fiber blocks on osRecvMesg(&D_800DCAC8),
//      which is where the port SYNCHRONOUSLY runs osSpTaskStartGo -> gdx_gfx_run for THIS tick
//      (GdxInterpBeginTick reads the flag HERE — still set) -> the sub-frame loop presents and
//      GdxUpdateFrameMirror refreshes the mirror -> DP-done wakes the fiber
//   -> Transition_SetBackgroundBuffer (CLEARS the flag; reads the just-refreshed mirror).
// So flag-set happens-before BeginTick, and BeginTick's mirror refresh happens-before the capture.
// OR-ing this into mForceCutSnap makes every scratch slot snap to cur (t=1); the P2 sub-frame loop
// then goes degenerate (GdxP1Lerped()==0) and renders every pass at t=1 (pass COUNT stays at M for
// a constant present cadence — see the degenerate block) — so the mirror the capture samples is
// the un-interpolated tick, as the capture requires. Read-only, direct-global idiom
// (same as gGamePaused / D_8024DCE0 / gSegments): no new decomp shim. Flag bit == (1<<0);
// `flags` is a u16 at struct offset 0x12 (activeType s32, queuedType s32, state s32, timer s16,
// argument s16, appearType u16, flags u16 — natural alignment, verified against transition.h:34-45).
extern "C" { extern unsigned char sTransition[]; }
static inline bool GdxTransitionCapturePendingThisTick() {
    const unsigned short flags =
        *reinterpret_cast<const unsigned short*>(&sTransition[0x12]);
    return (flags & 0x1u /* TRANSITION_FLAG_SET_BACKGROUND_BUFFER */) != 0;
}

// P4: determinism-gate canary globals. The game's two LCG RNG states
// (decomp/src/sys/math.c:185-188, external linkage) are advanced ONLY by game logic (physics, AI,
// effects, Math_Rand1/2) and NEVER by the render path — interpolation reads GfxPools and writes only
// scratch (prime directive). So the per-tick RNG fingerprint is identical with interpolation ON and
// OFF given identical input, and the FIRST tick whose fingerprint differs localizes any leak of a
// sub-frame value back into logic. Read-only. See GdxInterpDeterminismTick below.
extern "C" {
extern int gRandSeed1;
extern unsigned int gRandMask1;
extern int gRandSeed2;
extern unsigned int gRandMask2;
}

// GfxPool span from the segment-1 base. sys_gfx.c's Gfx_InitBuffer does
// Segment_SetPhysicalAddress(1, gGfxPool) every frame, so gSegments[1] holds the CURRENT pool's
// host base; a pool matrix resolves to gSegments[1] + member-offset.
//
// The span MUST be the real host sizeof(GfxPool), NOT the N64
// struct-comment size 0x36730. On the 64-bit host sizeof(Gfx) doubles (pointer-width w1), inflating
// gfxBuffer[13313] by 0x1A008, so the real pool is 0x50738 and — critically — the game's modelview
// matrices live in the UPPER region (past 0x36730). With the old 0x36730 bound, GdxP0MtxInPoolSpan
// rejected essentially every real modelview matrix, so NOTHING was rerouted (measured: [interp-p1]
// lerped/snapped all 0) and interpolation had nothing to tween. gdx_gfxpool_sizeof() (port/
// decomp_port.c, compiled with the real GfxPool type) is ground truth for the host size.
extern "C" size_t gdx_gfxpool_sizeof(void);
static inline bool GdxP0MtxInPoolSpan(uintptr_t p) {
    static const size_t kGdxP0GfxPoolSpanBytes = gdx_gfxpool_sizeof();
    const uintptr_t base = static_cast<uintptr_t>(gSegments[1]);
    return base != 0 && p >= base && p < base + kGdxP0GfxPoolSpanBytes;
}

// Effects-vertex span test, same ground-truth discipline as the pool span above and for the same
// reason: offsetof from decomp_port.c (the TU with the real GfxPool type), never the N64
// struct-comment constant 0x2A308 — the host offset is 0x1A008 higher because sizeof(Gfx) doubles,
// and the stale constant would aim this test into courseVtxBuffer.
extern "C" size_t gdx_gfxpool_effects_vtx_offset(void);
extern "C" size_t gdx_gfxpool_effects_vtx_bytes(void);
static inline bool GdxEffectsVtxInSpan(uintptr_t p, size_t bytes) {
    static const size_t kOffset = gdx_gfxpool_effects_vtx_offset();
    static const size_t kBytes = gdx_gfxpool_effects_vtx_bytes();
    const uintptr_t base = static_cast<uintptr_t>(gSegments[1]);
    if (base == 0) {
        return false;
    }
    const uintptr_t lo = base + kOffset;
    return p >= lo && (p + bytes) <= (lo + kBytes);
}

// Course-select carousel viewports (course_view.c: Vp D_i5_80118FF0[2][6], first index is the
// D_800DCCFC parity). Referenced directly — the D_xk3_80138930 extern in gdx_ek_disk_overrides.c
// is the precedent for naming an overlay symbol from port code; overlays are statically linked.
// Declared as raw s16 lanes rather than the decomp Vp union so this TU stays free of the decomp
// include tree; sizeof(Vp)==16 and the layout is vscale[4] then vtrans[4], asserted below.
extern "C" int16_t D_i5_80118FF0[2][6][8];
static_assert(sizeof(D_i5_80118FF0) == 2 * 6 * 16, "carousel viewport array shape");

// FNV-1a accumulation over 64-bit command words.
static inline void GdxP0FnvAccum(uint64_t& h, uint64_t word) {
    h ^= word;
    h *= 0x100000001B3ull;
}

/* Segment-9 fallback probe (Course Edit node-info panel scatter, diagnosis only).
 *
 * TryResolveAddress and G2ResolvePhysical both try gdx_resolve_mode_segment9()
 * first for every raw token -- the single authoritative source for the
 * currently-active segment 9 content (machine_models vs the Course Edit disk
 * image, see gdx_load_segment9_for_mode in decomp_port.c). When that misses,
 * execution falls through the function's remaining ~10 generic resolver
 * branches (EK address-range table, port/venue alias tables, D_1000000,
 * explicit segment table, RDRAM low32, registered host pointers, KSEG0/1...),
 * any of which could serve a genuinely seg9-tagged token (top byte == 9) by
 * accident. Three nearby seg-9 addresses resolving to three different host
 * buffers -- the reported [nodeinfo] scatter -- is exactly the signature of
 * that: different tokens missing the authoritative path and landing in
 * different fallback branches.
 *
 * Rather than adding a log line before every one of those returns (an
 * invasive touch to a hot-path function with many branches, several shared by
 * every texture/vertex/matrix pointer in the game, not just seg9), this is a
 * scope-exit probe: declared once, right after the seg9-mode miss, it reads
 * whatever `out` ends up holding when TryResolveAddress returns by ANY of its
 * exit paths (guaranteed by C++ destructor semantics) and logs the final
 * host address/segment/offset -- direct evidence of the scatter without
 * touching the resolver branches themselves. No behavioral change: purely
 * observes state that already exists when the function returns.
 *
 * Env-gated on GDX_LOG (see port_log.h); bounded to the first 24 misses per
 * process so a long soak/replay session cannot flood the log. */
class GdxSeg9FallbackDiag {
  public:
    GdxSeg9FallbackDiag(bool armed, uint32_t raw, size_t requiredBytes, const ResolvedAddress* out)
        : mArmed(armed), mRaw(raw), mRequiredBytes(requiredBytes), mOut(out) {}
    ~GdxSeg9FallbackDiag() {
        if (!mArmed) {
            return;
        }
        static int sLogs = 0;
        if (sLogs >= 24) {
            return;
        }
        ++sLogs;
        if (mOut->full != 0) {
            gdx_port_logf("[seg9diag] fallback served seg9 token raw=%08X req=%zu -> full=%p segment=%u "
                          "offset=%08X segmented=%d\n",
                          mRaw, mRequiredBytes, reinterpret_cast<void*>(mOut->full),
                          static_cast<unsigned>(mOut->segment), mOut->offset, static_cast<int>(mOut->segmented));
        } else {
            gdx_port_logf("[seg9diag] seg9 token raw=%08X req=%zu UNRESOLVED (mode resolver + all fallbacks "
                          "missed)\n",
                          mRaw, mRequiredBytes);
        }
    }
    GdxSeg9FallbackDiag(const GdxSeg9FallbackDiag&) = delete;
    GdxSeg9FallbackDiag& operator=(const GdxSeg9FallbackDiag&) = delete;

  private:
    bool mArmed;
    uint32_t mRaw;
    size_t mRequiredBytes;
    const ResolvedAddress* mOut;
};

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
        // Lazy conversion boundary: if `source` is a narrow N64-format list, convert
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

    // --- P0/P1 scratch-slot indirection + evidence API (no-ops unless mInterpEnabled) ---

    bool GdxP0Enabled() const { return mP0Enabled; }
    bool GdxP1Enabled() const { return mP1Enabled; }
    // True when the host-driven sub-frame present loop owns this tick (reuses P1 lerp).
    bool GdxP2HostActive() const { return mP2Host; }
    // t used for the presented (2nd) replay pass in P1 (0.5 for "mid"/"half").
    float GdxInterpPresentT() const { return gdx_interp::P1().presentT; }

    // Snap-event counters (per tick), surfaced by the [interp-p1] evidence line.
    size_t GdxP1Lerped() const { return mP1Lerped; }
    size_t GdxP1SnappedAbsent() const { return mP1SnappedAbsent; }
    size_t GdxP1SnappedTeleport() const { return mP1SnappedTeleport; }
    size_t GdxP1SnappedCut() const { return mP1SnappedCut; } // P3: whole-frame cut/pause snaps
    float GdxP1PairMaxDelta() const { return mP1PairMaxDelta; }
    size_t GdxP1PairSuspect() const { return mP1PairSuspect; }
    size_t GdxP1PoolBaseMisses() const { return mP1PoolBaseMisses; }
    // True iff this tick's whole-frame snap was armed by a pending transition capture.
    bool GdxCaptureSnapThisTick() const { return mCaptureSnapThisTick; }

    // Begin dual-pool tracking for this tick: latch the current/previous GfxPool bases from
    // gSegments[1] and reset the referenced-offset set.
    // Called once per gdx_gfx_run, before ConvertRoot drains the reroutes. No-op unless P1.
    void GdxInterpBeginTick() {
        mCurPoolBase = 0;
        mPrevPoolBase = 0;
        mP1Lerped = 0;
        mP1SnappedAbsent = 0;
        mP1SnappedTeleport = 0;
        mP1SnappedCut = 0;
        mP1PairMaxDelta = 0.0f;
        mP1PairSuspect = 0;
        mP1PoolBaseMisses = 0;
        mForceCutSnap = false;
        mCaptureSnapThisTick = false;

        // ---- PER-TICK work, performed by the tick's FIRST task ----
        // Runs before the mP1Enabled bail-out so the cut epoch is consumed exactly once per tick
        // even on a tick where lerping is inactive -- otherwise an epoch bump landing on a
        // non-interp tick would be double-counted against the next interp tick. Roll the
        // referenced-offset set here too: commit BEFORE clearing promotes the whole previous tick's
        // accumulated set to "previous", then starts this tick empty. Committing at the start of
        // the next tick rather than after the final task means nothing has to identify which task
        // IS the final one; every task in the tick accumulates into one set and tests against a
        // complete previous tick. See gGdxInterpNewTick above for what per-task rolling did.
        ++gGdxInterpTasksThisTick;
        if (gGdxInterpNewTick) {
            gGdxInterpNewTick = false;
            gdx_interp::CommitTick();
            gdx_interp::BeginTick();
            gGdxInterpCutThisTick = gdx_interp::CutPendingForThisTick();
        }

        if (!mP1Enabled) {
            return;
        }
        mCurPoolBase = static_cast<uintptr_t>(gSegments[1]);
        mPrevPoolBase = gdx_interp::PrevPoolBase(mCurPoolBase); // 0 if pool layout mismatch
        // P3 + P4: consume the cut epoch EXACTLY once per tick,
        // OR-in the pause flag, and OR-in a pending transition background-capture. Any of the three
        // makes the previous keyframe meaningless for the WHOLE frame this tick, so force every slot
        // to snap (t=1): the whole-frame cut semantic (a cut invalidates all prev keyframes),
        // the pause freeze (nothing new to tween; show the newest pose crisp), and the transition
        // capture (the mirror this tick feeds Transition_SetBackgroundBuffer must be un-interpolated,
        // see GdxTransitionCapturePendingThisTick's ordering proof).
        mCaptureSnapThisTick = GdxTransitionCapturePendingThisTick();
        mForceCutSnap = gGdxInterpCutThisTick || (gGamePaused != 0) || mCaptureSnapThisTick;
    }

    // Copy a fully-resolved 64-byte pool matrix into a stable per-tick scratch slot; record
    // (prevPoolPtr, curPoolPtr, scratchPtr, snap); return the scratch address so the caller
    // rewrites the resolved command's w1. The deque never invalidates element addresses on
    // push_back, so a scratch pointer baked into an earlier command stays valid as more matrices
    // are rerouted. In P0 this records (0, cur, scratch, false) and refill is an identity copy;
    // in P1 it derives the sibling-pool prev pointer and computes the per-slot snap decision.
    uintptr_t GdxP0RerouteMtx(uintptr_t origPtr, bool isProj) {
        if (!mInterpEnabled || origPtr == 0 || ReadableByteLimit(origPtr) < 64u) {
            return origPtr; // defensive: leave the pointer untouched, bit-exact stock path
        }
        mP0Scratch.emplace_back();
        GdxP0Mtx* slot = &mP0Scratch.back();
        std::memcpy(slot, reinterpret_cast<const void*>(origPtr), 64);

        uintptr_t prevPtr = 0;
        bool snap = false;
        if (mP1Enabled) {
            // A slot whose offset was NOT referenced last tick has no usable prev
            // keyframe (spawn/despawn) -> snap. Note it either way so it's in this tick's set.
            const uint32_t offset = static_cast<uint32_t>(origPtr - mCurPoolBase);
            const bool prevPresent = gdx_interp::NoteReferencedOffset(offset);
            if (mForceCutSnap) {
                // P3: a cut/teleport epoch bump or an active pause forces every slot this tick to
                // snap to cur (t=1), regardless of a usable prev keyframe. Still noted above so the
                // referenced-set stays correct for the NEXT tick's spawn/despawn decision.
                snap = true;
                ++mP1SnappedCut;
            } else if (mPrevPoolBase != 0 && mCurPoolBase != 0 && prevPresent) {
                prevPtr = mPrevPoolBase + offset;
                if (ReadableByteLimit(prevPtr) < 64u) {
                    prevPtr = 0;
                    snap = true; // sibling not readable -> snap to cur
                    ++mP1SnappedAbsent;
                } else if (gdx_interp::TranslationTeleport(reinterpret_cast<const void*>(prevPtr),
                                                           reinterpret_cast<const void*>(origPtr))) {
                    snap = true; // teleport/cut heuristic (belt-and-suspenders)
                    ++mP1SnappedTeleport;
                } else {
                    ++mP1Lerped;
                    // [interp-pair] Pairing-quality sample. See gdx_interp.h TranslationDelta for
                    // why byte-offset identity can silently pair two different objects, and why the
                    // 2000-unit teleport guard above cannot notice when it does.
                    const float delta = gdx_interp::TranslationDelta(
                        reinterpret_cast<const void*>(prevPtr), reinterpret_cast<const void*>(origPtr));
                    if (delta > mP1PairMaxDelta) {
                        mP1PairMaxDelta = delta;
                    }
                    // Suspicious, not wrong: real per-tick motion is "a few tens of units"
                    // (gdx_interp.cpp:201-204), so a paired slot moving further than this is either
                    // something genuinely fast or a mispairing. The count is what shows a fat tail
                    // appearing exactly when the camera sweeps.
                    if (delta > 200.0f) {
                        ++mP1PairSuspect;
                    }
                }
            } else {
                snap = true; // absent prev keyframe or pool-base mismatch -> snap to cur
                if (mPrevPoolBase == 0 || mCurPoolBase == 0) {
                    ++mP1PoolBaseMisses;
                } else {
                    ++mP1SnappedAbsent;
                }
            }
        }

        mP0Records.push_back({ origPtr, prevPtr, slot, snap, isProj });
        return reinterpret_cast<uintptr_t>(slot);
    }

    size_t GdxP0ScratchSlots() const { return mP0Records.size(); }

    // Refill every scratch slot for the next replay pass. P0 (and any snapped P1 slot) copies the
    // current-pool matrix verbatim (== lerp at t=1). A live P1 slot writes lerp(prev, cur, t) in
    // float space (SoH interpolate_mtxf). At t=1 both paths are byte-identical to the pool matrix.
    void GdxP0RefillScratch(float t) {
        for (const GdxP0Record& r : mP0Records) {
            if (ReadableByteLimit(r.orig) < 64u) {
                continue;
            }
            if (mP1Enabled && r.prev != 0 && !r.snap && t < 1.0f &&
                ReadableByteLimit(r.prev) >= 64u) {
                gdx_interp::LerpMtx(reinterpret_cast<const void*>(r.prev),
                                    reinterpret_cast<const void*>(r.orig), t, r.scratch);
            } else {
                std::memcpy(r.scratch, reinterpret_cast<const void*>(r.orig), 64);
            }
        }
    }

    // --- Carousel viewport interpolation (Tier 2) -------------------------------------------------
    //
    // The course-select carousel slides by rewriting viewport vtrans[0] per tick (course_select.c
    // Object_LerpPosXToClampedTargetMaxStep, up to 192 vtrans units = 48 screen px per tick) — no
    // matrix carries this motion, so the pool-matrix lerp cannot smooth it. The viewports live in
    // D_i5_80118FF0[2][6], parity-indexed by the SAME D_800DCCFC toggle as the GfxPool, so the
    // previous tick's keyframe already exists at the sibling parity row: no snapshotting needed.
    //
    // Deliberately NOT reusing the matrix machinery's parts, per the differences that would corrupt
    // it: the array is overlay BSS (outside the pool span), the prev keyframe is orig +/- 6*16
    // bytes (not prevPoolBase + offset), and NoteReferencedOffset's set is keyed on pool offsets a
    // viewport pointer would collide with. The slot index i is the identity here, and it is
    // perfectly stable — far stronger than the byte-offset identity the matrices live with.
    struct alignas(8) GdxVpSlot {
        int16_t v[8]; // vscale[4], vtrans[4] — layout of the libultra Vp_t, host-native
    };
    struct GdxVpRecord {
        uintptr_t orig;    // &D_i5_80118FF0[parity][i], the game's live viewport for this tick
        uintptr_t prev;    // sibling parity row, same slot (previous tick's values)
        GdxVpSlot* scratch;
        bool snap;
    };
    std::deque<GdxVpSlot> mVpScratch; // deque: element addresses must survive later push_backs
    std::vector<GdxVpRecord> mVpRecords;
    size_t mVpLerped = 0;
    size_t mVpSnapped = 0;

    uintptr_t GdxVpReroute(uintptr_t origPtr) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(&D_i5_80118FF0[0][0][0]);
        if (!mInterpEnabled || origPtr < base || origPtr >= base + sizeof(D_i5_80118FF0) ||
            ((origPtr - base) % sizeof(GdxVpSlot)) != 0) {
            return origPtr;
        }
        const size_t flat = (origPtr - base) / sizeof(GdxVpSlot); // 0..11
        const size_t slot = flat % 6;
        const size_t parity = flat / 6;
        const uintptr_t prevPtr = base + ((parity ^ 1u) * 6 + slot) * sizeof(GdxVpSlot);

        mVpScratch.emplace_back();
        GdxVpSlot* scratch = &mVpScratch.back();
        std::memcpy(scratch, reinterpret_cast<const void*>(origPtr), sizeof(GdxVpSlot));

        // Snap on the whole-frame cut, and on any vtrans[0] delta beyond the game's own per-tick
        // clamp: legit carousel motion never exceeds 192 units, so anything larger is the
        // GAMEMODE_FLX_GP_RACE_NEXT_COURSE instant warp or a tick where the writer was skipped and
        // the sibling row is two ticks stale. One test covers both.
        bool snap = mForceCutSnap || !mP1Enabled;
        if (!snap) {
            const int16_t* cur = reinterpret_cast<const int16_t*>(origPtr);
            const int16_t* prv = reinterpret_cast<const int16_t*>(prevPtr);
            const int delta = static_cast<int>(cur[4]) - static_cast<int>(prv[4]); // vtrans[0]
            if (delta > 192 || delta < -192) {
                snap = true;
            }
        }
        if (snap) {
            ++mVpSnapped;
        } else {
            ++mVpLerped;
        }
        mVpRecords.push_back({ origPtr, prevPtr, scratch, snap });
        return reinterpret_cast<uintptr_t>(scratch);
    }

    void GdxVpRefillScratch(float t) {
        for (const GdxVpRecord& r : mVpRecords) {
            // t>=1 must be a byte-exact copy (same Correction-1 transparency contract as the
            // matrices); the lerp rounds to nearest so the carousel cannot bias a sub-pixel toward
            // the stale keyframe on every pass.
            if (r.snap || t >= 1.0f) {
                std::memcpy(r.scratch, reinterpret_cast<const void*>(r.orig), sizeof(GdxVpSlot));
                continue;
            }
            const int16_t* cur = reinterpret_cast<const int16_t*>(r.orig);
            const int16_t* prv = reinterpret_cast<const int16_t*>(r.prev);
            for (int i = 0; i < 8; ++i) {
                const float v = static_cast<float>(prv[i]) +
                                (static_cast<float>(cur[i]) - static_cast<float>(prv[i])) * t;
                r.scratch->v[i] = static_cast<int16_t>(std::lround(v));
            }
        }
    }

    // --- Effects vertex interpolation (Tier 3: booster flames / side-attack quads) ----------------
    //
    // racer.c computes the flame/effect vertex positions on the CPU per tick from racer->modelMatrix
    // and bumps them into gGfxPool->effectsVtxBuffer — no gSPMatrix anywhere in that path, so at
    // sub-frame t the machine body renders at the lerped pose while its flame stays baked at the
    // tick-end pose: the owner's "afterimage". The pool is double-buffered, so the previous tick's
    // vertices sit at prevPoolBase + the same offset, and the pool-quiescence proof (see the loop's
    // parity latch) covers vertex bytes exactly as it covers matrices.
    //
    // Identity is the batch's pool byte offset — a MUCH higher-churn keyspace than matrices: a
    // booster batch flips between 4 and 5 vertices with boost state, and any spawn/despawn shifts
    // every downstream offset. The per-batch snap guard is load-bearing here, not belt-and-braces:
    // any single vertex moving more than the teleport threshold snaps its WHOLE batch, so a
    // mispaired quad cannot shear. Only ob[0..2] is lerped; flag/tc/cn are copied from cur verbatim.
    // Position lerp also smooths the gGameFrameCount&3 flame size pulse (it feeds ob[] via the
    // billboard half-extents) — a 15 Hz stair-step becomes a ramp, which is the desired look.
    struct GdxVtxRecord {
        uintptr_t orig;
        uint8_t* scratch; // contiguous batch buffer (count*16 bytes)
        uint32_t count;
        bool snap;
        // Anchor: the owning racer's interpolated motion, captured as the model matrix's
        // translation at both keyframes. The batch is shifted by the anchor's tween each pass; no
        // previous-tick vertex bytes are consulted at all.
        float aPrev[3];
        float aCur[3];
    };
    std::deque<std::vector<uint8_t>> mVtxScratch; // deque of per-batch buffers: stable addresses
    std::vector<GdxVtxRecord> mVtxRecords;
    size_t mVtxLerped = 0;
    size_t mVtxSnapped = 0;

    uintptr_t GdxVtxReroute(uintptr_t origPtr, uint32_t count) {
        const size_t bytes = static_cast<size_t>(count) * 16u;
        if (!mInterpEnabled || count == 0 || ReadableByteLimit(origPtr) < bytes) {
            return origPtr;
        }
        mVtxScratch.emplace_back(bytes);
        uint8_t* scratch = mVtxScratch.back().data();
        std::memcpy(scratch, reinterpret_cast<const void*>(origPtr), bytes);

        // ANCHORED interpolation, third design for this tier. The first (offset pairing + distance
        // threshold) never snapped once while the owner saw teleports; the second (tc-lane identity)
        // died to a subtler fact: stale bump-buffer bytes are STABLE, so the side-attack quads'
        // unwritten tc lanes matched the sibling pool's identical ancient garbage and kept lerping
        // into a red vehicle-shaped smear. Byte identity cannot name an effect, and in a bunched
        // pack no distance threshold separates "same flame, one tick of motion" from "the next
        // machine's flame" -- both live within tens of units.
        //
        // What CAN name an effect is its owner. Every effect in racer.c is computed from its
        // racer's modelMatrix, and that same matrix (gGfxPool->unk_20308[id], world-space, built at
        // racer.c:6070) is already rerouted with both keyframes in hand. So: find the nearest
        // non-projection matrix record to the batch centroid, and each pass shift the whole batch
        // by that anchor's interpolated translation delta. The flame rides its machine. Previous
        // vertex bytes are never read, so there is nothing to mispair: the residual cost is only
        // that per-tick shape animation (the &3 size pulse) renders at the current keyframe -- a
        // 15 Hz stair, which is stock Issue-G behaviour.
        //
        // 3-vertex batches (debris shards, flying sparks) stay permanently snapped: single-tick
        // particles, and several of them genuinely have no meaningful owner after a crash.
        bool snap = true;
        float aPrev[3] = { 0.0f, 0.0f, 0.0f };
        float aCur[3] = { 0.0f, 0.0f, 0.0f };
        if (count != 3 && !mForceCutSnap && mP1Enabled && mPrevPoolBase != 0 && mCurPoolBase != 0) {
            float cx = 0.0f, cy = 0.0f, cz = 0.0f;
            const int16_t* cur = reinterpret_cast<const int16_t*>(origPtr);
            for (uint32_t v = 0; v < count; ++v) {
                const size_t lane = v * 8;
                cx += cur[lane + 0];
                cy += cur[lane + 1];
                cz += cur[lane + 2];
            }
            const float inv = 1.0f / static_cast<float>(count);
            cx *= inv; cy *= inv; cz *= inv;

            // 60 world units: generous for "this machine's own effect" (boosters sit within ~20
            // units of the model origin), tight enough that a neighbouring machine must be
            // physically overlapping to steal an anchor -- and if machines overlap, their motions
            // are near-identical anyway, so a stolen anchor degrades to a correct answer.
            float bestD2 = 60.0f * 60.0f;
            const GdxP0Record* best = nullptr;
            for (const GdxP0Record& m : mP0Records) {
                if (m.proj || m.snap || m.prev == 0) {
                    continue;
                }
                float mf[4][4];
                gdx_interp::MtxToF(reinterpret_cast<const void*>(m.orig), mf);
                const float dx = mf[3][0] - cx;
                const float dy = mf[3][1] - cy;
                const float dz = mf[3][2] - cz;
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bestD2) {
                    bestD2 = d2;
                    best = &m;
                }
            }
            if (best != nullptr) {
                float cf[4][4];
                float pf[4][4];
                gdx_interp::MtxToF(reinterpret_cast<const void*>(best->orig), cf);
                gdx_interp::MtxToF(reinterpret_cast<const void*>(best->prev), pf);
                for (int i = 0; i < 3; ++i) {
                    aCur[i] = cf[3][i];
                    aPrev[i] = pf[3][i];
                }
                snap = false;
            }
        }
        if (snap) {
            ++mVtxSnapped;
        } else {
            ++mVtxLerped;
        }
        mVtxRecords.push_back({ origPtr, scratch, count, snap,
                                { aPrev[0], aPrev[1], aPrev[2] },
                                { aCur[0], aCur[1], aCur[2] } });
        return reinterpret_cast<uintptr_t>(scratch);
    }

    void GdxVtxRefillScratch(float t) {
        for (const GdxVtxRecord& r : mVtxRecords) {
            const size_t bytes = static_cast<size_t>(r.count) * 16u;
            if (ReadableByteLimit(r.orig) < bytes) {
                continue;
            }
            // Whole-batch copy keeps flag/tc/cn and the current shape; the anchor shift then moves
            // the batch back along its owner's motion. At t=1 the shift is exactly zero, so the
            // copy alone is byte-exact (transparency contract).
            std::memcpy(r.scratch, reinterpret_cast<const void*>(r.orig), bytes);
            if (r.snap || t >= 1.0f) {
                continue;
            }
            float shift[3];
            bool any = false;
            for (int i = 0; i < 3; ++i) {
                // lerp(prev,cur,t) - cur == (prev-cur)*(1-t)
                shift[i] = (r.aPrev[i] - r.aCur[i]) * (1.0f - t);
                if (shift[i] != 0.0f) {
                    any = true;
                }
            }
            if (!any) {
                continue;
            }
            int16_t* out = reinterpret_cast<int16_t*>(r.scratch);
            for (uint32_t v = 0; v < r.count; ++v) {
                const size_t lane = v * 8;
                for (int ax = 0; ax < 3; ++ax) {
                    const float f = static_cast<float>(out[lane + ax]) + shift[ax];
                    out[lane + ax] = static_cast<int16_t>(std::lround(f));
                }
            }
        }
    }

    size_t GdxVpLerped() const { return mVpLerped; }
    size_t GdxVpSnapped() const { return mVpSnapped; }
    size_t GdxVtxLerped() const { return mVtxLerped; }
    size_t GdxVtxSnapped() const { return mVtxSnapped; }

    // memcmp every scratch vs its origin; at t=1 they must match (Correction-1 transparency proof).
    size_t GdxP0TransparencyViolations() const {
        size_t bad = 0;
        for (const GdxP0Record& r : mP0Records) {
            if (ReadableByteLimit(r.orig) < 64u) {
                continue;
            }
            if (std::memcmp(r.scratch, reinterpret_cast<const void*>(r.orig), 64) != 0) {
                ++bad;
            }
        }
        return bad;
    }

    // FNV-1a over every resolved command word in every converted list. mLists is not mutated
    // between passes, so its (unordered) iteration order is identical across calls in one tick,
    // making the hash directly comparable pass-to-pass.
    uint64_t GdxP0HashCommands() const {
        uint64_t h = 0xCBF29CE484222325ull;
        for (const auto& kv : mLists) {
            const ConvertedList* l = kv.second.get();
            for (const Fast::F3DGfx& g : l->commands) {
                GdxP0FnvAccum(h, static_cast<uint64_t>(g.words.w0));
                GdxP0FnvAccum(h, static_cast<uint64_t>(g.words.w1));
            }
        }
        return h;
    }

    // Snapshot all command words (same iteration order as the hash) for mutation counting.
    void GdxP0SnapshotCommands(std::vector<uint64_t>& out) const {
        out.clear();
        for (const auto& kv : mLists) {
            const ConvertedList* l = kv.second.get();
            for (const Fast::F3DGfx& g : l->commands) {
                out.push_back(static_cast<uint64_t>(g.words.w0));
                out.push_back(static_cast<uint64_t>(g.words.w1));
            }
        }
    }

    // Count operands that changed vs a prior snapshot (detects in-place interpreter mutation).
    size_t GdxP0CountMutations(const std::vector<uint64_t>& snap) const {
        size_t idx = 0, muts = 0;
        for (const auto& kv : mLists) {
            const ConvertedList* l = kv.second.get();
            for (const Fast::F3DGfx& g : l->commands) {
                if (idx + 1 < snap.size()) {
                    if (snap[idx] != static_cast<uint64_t>(g.words.w0)) {
                        ++muts;
                    }
                    if (snap[idx + 1] != static_cast<uint64_t>(g.words.w1)) {
                        ++muts;
                    }
                }
                idx += 2;
            }
        }
        return muts;
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

    // P0/P1 scratch-slot indirection state (only populated when mInterpEnabled). The arena is
    // per-adapter, i.e. per gdx_gfx_run/per tick — rebuilt every tick, matching the
    // "stable per-tick scratch slot arena". deque<> guarantees element-address stability.
    // The host-driven decoupled loop reuses ALL of the P1 dual-pool lerp machinery, so
    // P2 activation is simply OR-ed into the P1 enable. Declared first so mP1Enabled can read it
    // (members initialise in declaration order). mP2Host mirrors main.cpp's per-tick decision.
    const bool mP2Host = GdxP2HostConfigured();
    const bool mP0Enabled = GdxInterpP0Enabled();
    const bool mP1Enabled = gdx_interp::P1().enabled || mP2Host;
    const bool mInterpEnabled = mP0Enabled || mP1Enabled;
    // Latched once per adapter (i.e. once per gdx_gfx_run) rather than read at the matrix command
    // that consults it: CameraInterpActive() hashes a CVar name, and the condition below is
    // evaluated for EVERY G_MTX in the display list. A handful of lookups per tick is free; a
    // lookup per command is not. The Bucket B dev gate is OR-ed in so a dev build can still force
    // camera interpolation on for A/B without touching the shipped CVar.
    const bool mInterpCamera =
        gdx_interp::CameraInterpActive() || gdx_dev_gate(GDX_GATE_INTERP_CAMERA) != 0;
    struct GdxP0Record {
        uintptr_t orig;    // resolved CURRENT-pool matrix host pointer (curPoolPtr)
        uintptr_t prev;    // sibling(PREVIOUS)-pool matrix host pointer (0 in P0 / snapped slot)
        GdxP0Mtx* scratch; // stable slot the command now points at
        bool snap;         // P1: force t=1 (absent prev keyframe or teleport) for this slot
        bool proj;         // G_MTX_PROJECTION load: excluded from effect-anchor search (its row 3
                           // is a view-space term, not a world position)
    };
    std::deque<GdxP0Mtx> mP0Scratch;
    std::vector<GdxP0Record> mP0Records;
    // Dual-pool bases (latched per tick by GdxInterpBeginTick) + snap-event counters.
    uintptr_t mCurPoolBase = 0;
    uintptr_t mPrevPoolBase = 0;
    size_t mP1Lerped = 0;
    size_t mP1SnappedAbsent = 0;
    size_t mP1SnappedTeleport = 0;
    size_t mP1SnappedCut = 0;      // P3: slots snapped by a cut-epoch bump or an active pause
    // [interp-pair] pairing-quality, reset per tick with the counters above
    float mP1PairMaxDelta = 0.0f;  // largest prev->cur translation delta among PAIRED slots
    size_t mP1PairSuspect = 0;     // paired slots that moved further than a tick plausibly can
    size_t mP1PoolBaseMisses = 0;
    bool mForceCutSnap = false;    // P3: this whole tick snaps (cut/teleport epoch changed, or paused)
    bool mCaptureSnapThisTick = false; // P4: the whole-frame snap this tick is a transition capture

    size_t CommandStrideForSource(const N64Gfx* source) const {
        const uintptr_t ptr = reinterpret_cast<uintptr_t>(source);
        /* Some game-built lists live inside the emulated RDRAM arena but use the
         * PORT Gfx ABI (16-byte packets with a pointer-width w1). They must win
         * over the general "RDRAM is raw N64" classification. */
        if (IsHostWideCommandPointer(ptr)) {
            return kHostBuiltGfxStride;
        }
        return (IsRawN64HostPointer(ptr) || IsHostN64CommandPointer(ptr)) ? kN64GfxStride : kHostBuiltGfxStride;
    }

    // Return a wide 16-byte version of `source`, converting+caching on
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
        /* [dl-census]: one line per UNIQUE
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
            // [dl-census] high-frequency per-draw diagnostic: silent unless GDX_DIAG_VERBOSE=1.
            if (gdx_diag_verbose() && !dlDup && sDlCensusCount < 48) {
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

        /* Segment 9 is not a single global namespace in Expansion Kit builds:
           Create Machine/machine-settings use the cartridge machine_models
           image, while Course Edit uses a different disk-resident image at the
           same 0x09xxxxxx addresses. The port's mode loader owns that switch;
           consult it before the generated EK address ranges so stale/overlapping
           registrations cannot select the other mode's bytes. */
        {
            uintptr_t modeAddress = 0;
            if (gdx_resolve_mode_segment9(raw, requiredBytes, &modeAddress) != 0 &&
                ReadableByteLimit(modeAddress) >= requiredBytes) {
                out.full = modeAddress;
                out.segment = 9u;
                out.offset = raw & 0x00FFFFFFu;
                out.segmented = true;
                return true;
            }
        }

        // Task 3 diag (Course Edit node-info panel scatter): see GdxSeg9FallbackDiag
        // above N64DisplayListAdapter. Armed only for genuine segment-9 tokens that
        // just missed the authoritative mode resolver; its destructor fires on
        // whichever return below actually serves (or fails to serve) this token. E5:
        // also armed by GDX_DIAG_NODEINFO, a dedicated opt-in for the [nodeinfo]
        // investigation that does not require enabling the broader GDX_LOG sink.
        // Live gate reads — see the matching comment on the raw-resolver twin above.
        const bool seg9DiagEnabled =
            gdx_dev_gate(GDX_GATE_LOG_FILE) || gdx_dev_gate(GDX_GATE_DIAG_NODEINFO);
        GdxSeg9FallbackDiag seg9Diag(seg9DiagEnabled && (raw >> 24) == 9u, raw, requiredBytes, &out);

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

        // E1: thread requiredBytes through so a caller-side over-estimate cannot
        // accept an interior match that overruns the row's declared image size.
        if (ResolveGeneratedAssetStub(raw, out, requiredBytes)) {
            return true;
        }

        if (ResolveSetupGfxStub(raw, out)) {
            return true;
        }

        const uint32_t d1000000_low = Low32(reinterpret_cast<uintptr_t>(D_1000000));
        for (const HostRange& range : gHostRanges) {
            if (range.begin == reinterpret_cast<uintptr_t>(D_1000000)) {
                /* requiredBytes: this only proved the START of
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

        /* Host-built PORT commands still have N64-sized pointer words, so a
           pointer to the emulated RDRAM arena may arrive as its low 32 bits.
           Unlike the quarantined registered-range reconstruction below, this
           is one exact, session-owned 8 MiB allocation and unsigned subtraction
           also handles a low32 wrap. Keep it after explicit segment tokens so
           0x01xxxxxx..0x0Fxxxxxx retain their N64 meaning. */
        {
            uintptr_t rdramAddress = 0;
            if (ResolveRdramLow32(raw, requiredBytes, &rdramAddress)) {
                out.full = rdramAddress;
                out.segmented = false;
                return true;
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
           the complete unmasked low32 of the real host pointer.
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
            if (!LegacyResolveEnabled() || gdx_dev_gate(GDX_GATE_NO_SRCWIN) || (sourceHint == 0))
                return false;
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

        /* Ambiguous cross-segment fallback: this used to pick
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
        /* Wide-layout w1 fix (rank-gadget confetti root cause):
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

        /* [digit-carve] one-shot: the rank digits
           (aPositionDigitTexs, seg4+0x13DE0) are ZERO-FILLED in ROM -- the
           console composes/loads them at runtime. This dump decides whether
           the port's carve receives that runtime content (nonzero => the
           live-carve resolver fix alone renders the gadget) or stays zero
           (=> an EK/asset fill gap remains and the digits will be invisible
           until that loader is found). */
        /* Mode-gated, not gGdxRaceActive-gated: Course Edit sets that latch too, and it spent the
           one-shot there in one session. 0x01 is GAMEMODE_GP_RACE (GET_MODE == gGameMode & 0x1F). */
        if ((gGameMode & 0x1F) == 0x01 && gSegments[4] != 0) {
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
         * A wide (host-built) parent already carries the real host
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
            // Always-on error family, but bounded in every phase so it cannot flood boot
            // menus: race keeps the large evidence budget, non-race is capped small (40).
            const int missingCap = (gGdxRaceActive != 0) ? 400 : 40;
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
            // Always-on error family, bounded in every phase (see [gdl-miss] above):
            // race keeps the large evidence budget, non-race is capped small (40).
            const int badCap = (gGdxRaceActive != 0) ? 400 : 40;
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

    // --- Segment-reload seqlock guards -------------------------------------
    // A mode transition reloads asset segments 4/7/9 on the GAME thread
    // (decomp_port.c gdx_load_mode_segments) -- rewriting the carve bytes AND
    // swapping the gSegments[] bases -- while this GRAPHICS thread resolves
    // segment-backed pointers here unsynchronized. Each guard snapshots the
    // epoch before a resolution and re-checks it after: a resolution that raced
    // a reload (torn base or torn bytes) is reported unstable and the caller
    // substitutes an opcode-specific fallback instead of trusting it. Wide
    // packets carrying a real host pointer never read gSegments[], so they
    // bypass the guard. Wait-free: exactly two acquire atomic loads per guarded
    // command, no locks, no allocation.

    // Rate-limited skip notice shared by every guarded site.
    void NoteEpochSkip() {
        if (mStats != nullptr) mStats->skippedEpochRetries++;
        static int sEpochSkipLogs = 0;
        if (sEpochSkipLogs < 40) {
            ++sEpochSkipLogs;
            gdx_port_logf("[seg-epoch] segment reload raced a translate; command "
                          "dropped/fell back this frame (n=%d)\n", sEpochSkipLogs);
        }
    }

    // Guarded TranslateDataPointer. Writes the resolved pointer to `out` and
    // returns true if stable; on a raced reload it counts+logs the skip and
    // returns false (caller applies its fallback). `out` is written in every
    // case, but the caller must overwrite it on a false return.
    bool ResolveGuarded(uint32_t raw, bool hostPtr, uintptr_t hostFull, uintptr_t& out,
                        size_t requiredBytes = 1, bool preferPhysical = false,
                        uintptr_t sourceHint = 0) {
        if (hostPtr) {
            out = hostFull;
            return true;
        }
        const uint32_t epoch = GdxSegmentEpochSnapshot();
        out = TranslateDataPointer(raw, requiredBytes, preferPhysical, sourceHint);
        if (GdxSegmentEpochStable(epoch)) return true;
        NoteEpochSkip();
        return false;
    }

    // Guarded TranslateDisplayListPointer. On a raced reload returns the noop DL
    // (mNoopList) -- the same fallback TranslateDisplayListPointer itself uses
    // for an unresolved target -- rather than a possibly-torn sub-list pointer.
    uintptr_t ResolveDisplayListGuarded(uint32_t raw, const N64Gfx* parentSource,
                                        size_t parentIndex, bool hostPtr, uintptr_t hostFull) {
        // Host-pointer packets never read gSegments[], so skip the epoch snapshot
        // entirely for them (mirrors the hostPtr fast path in ResolveGuarded above).
        if (hostPtr) {
            return TranslateDisplayListPointer(raw, parentSource, parentIndex,
                                                reinterpret_cast<const N64Gfx*>(hostFull));
        }
        const uint32_t epoch = GdxSegmentEpochSnapshot();
        // Pre-check: TranslateDisplayListPointer unconditionally bumps
        // mStats->missingDisplayLists/noopDisplayLists/badDisplayLists and emits
        // [gdl-miss]/[gdl-bad] log lines. If the snapshot is already unstable,
        // skip the call entirely so a raced mode-transition frame doesn't pollute
        // those counters/log budgets with a result that would be discarded anyway.
        if (!GdxSegmentEpochStable(epoch)) {
            NoteEpochSkip();
            return reinterpret_cast<uintptr_t>(mNoopList.data());
        }
        const uintptr_t resolved = TranslateDisplayListPointer(raw, parentSource, parentIndex, nullptr);
        // The race can also begin during the call itself; that residual sliver
        // still pollutes at most one call's counters -- acceptable, unlike the
        // always-hit case the pre-check above eliminates.
        if (GdxSegmentEpochStable(epoch)) return resolved;
        NoteEpochSkip();
        return reinterpret_cast<uintptr_t>(mNoopList.data());
    }

  public:
    Fast::F3DGfx* ConvertRoot() {
        if (mRootBegin == nullptr) return nullptr;
        const size_t rootLimit = RootCommandLimit(mRootBegin);
        /* CRASH FAILSAFE: a ROOT display list
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
        // For a converted wide buffer the source segment's dialect tag
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
        bool diagThisList = false;
        if (gdx_dev_gate(GDX_GATE_DIAG_SETUPDL)) {
            /* Both DLs are BSS placeholders that ResolveGeneratedAssetStub redirects into the
               bridge's own segment-8 image, so item.source is never gSegments[8] + offset --
               anchor on the generated asset rows instead. The image bytes outlive the vector that
               owns them (only ever appended to), so the resolved addresses are cacheable. */
            static uintptr_t sSetupDlA = 0;
            static uintptr_t sSetupDlB = 0;
            if (sSetupDlA == 0 || sSetupDlB == 0) {
                uint32_t offA = 0;
                uint32_t offB = 0;
                const uintptr_t baseA =
                    EnsureAssetSegmentForSymbol(Low32(reinterpret_cast<uintptr_t>(D_8014040)), &offA);
                const uintptr_t baseB =
                    EnsureAssetSegmentForSymbol(Low32(reinterpret_cast<uintptr_t>(D_8014078)), &offB);
                if (baseA != 0) sSetupDlA = baseA + offA;
                if (baseB != 0) sSetupDlB = baseB + offB;
            }
            const uintptr_t src = reinterpret_cast<uintptr_t>(item.source);
            if (src != 0 && (src == sSetupDlA || src == sSetupDlB)) {
                static int sSetupDlDumps = 0;
                if (sSetupDlDumps < 40) {
                    ++sSetupDlDumps;
                    diagThisList = (sSetupDlDumps <= 2);
                    gdx_port_logf("[setupdl] source=%p sym=%s stride=%zu big=%d f3d=%d limit=%zu race=%d\n",
                                  item.source, (src == sSetupDlA) ? "D_8014040" : "D_8014078", stride,
                                  (int)isBig, (int)isF3DSource, item.limit, gGdxRaceActive);
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
        /* True while an L3DEX2 (line microcode) section is being converted: its non-line
           commands are F3DEX2-compatible and run as Standard, and each G_LINE3D (0x08) is
           rewritten into OTR_G_LINE3D_GDX for the interpreter's screen-space quad expansion.
           Cleared by the next recognized microcode load. */
        bool l3dexLineSection = false;

        // Host-built display lists carry a FULL pointer-width w1.
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
             * host pointer the game already resolved — use it verbatim and skip the
             * low32-reconstruction guesswork. A wide packet with high bits zero holds a 32-bit VALUE or
             * a segmented address, which still flows through the segment table /
             * value path below exactly as before. Narrow sources never set high
             * bits, so this is always false for them. */
            bool w1IsHostPointer =
                sourceIsWide && ((static_cast<uint64_t>(w1full) >> 32) != 0);
            /* EXCEPTION to the "high bits set => real host pointer" rule: the
             * game references runtime-loaded segmented assets (setup_gfx render-
             * mode DLs via gSPDisplayList(&D_3000050), vertex data via
             * gSPVertex(&D_3000xxx)) through 1-byte BSS PLACEHOLDER symbols. The
             * wide gSPDisplayList/gSPVertex pack the full host address of that
             * placeholder, so high32 is set and it LOOKS like a real host
             * pointer -- but the placeholder is a 1-byte object; the real DL/
             * vertex bytes live in the loaded segment image. Taking it verbatim
             * branches the interpreter into the 1-byte stub and reads adjacent
             * BSS as commands: odd branch targets, a G_GEOMETRYMODE word
             * (0xD9680800) misread as a vertex pointer, and a count=0xF0 garbage
             * G_VTX -- the boot/title-phase crash. Route
             * these back through the low32 resolver, which maps the placeholder
             * to the loaded segment via ResolveGeneratedAssetStub exactly as the
             * pre-wide build did. Only the placeholder's own low32 is affected;
             * genuine runtime host pointers (GfxPool sub-DLs, persistent vertex
             * copies) are never registered in the asset map and are unchanged. */
            if (w1IsHostPointer &&
                (IsAssetPlaceholderPointer(in.w1) || IsPortBssAliasPointer(in.w1))) {
                w1IsHostPointer = false;
            }
            /* Second EXCEPTION: high32 == 0xFFFFFFFF is a SIGN-EXTENDED 32-bit value,
             * not a host pointer -- Windows user space never has an all-ones top half
             * (that's kernel range). Course-edit entry crash (symbolized:
             * GfxDpLoadBlock memcpy from 0xFFFFFFFFF8694130): decomp code widened a
             * bit31-set token through a signed cast into the wide list, the verbatim
             * rule accepted it, and LUS copied from kernel space -- the 80%-of-entries
             * AV. Strip to the low32 and route through the segment/low32 resolver
             * exactly like a narrow-source command: if the token's owning range is
             * registered the texture resolves correctly (pre-wide behavior); if not,
             * the existing unresolved fallbacks draw nothing instead of crashing. */
            if (w1IsHostPointer && (static_cast<uint64_t>(w1full) >> 32) == 0xFFFFFFFFull) {
                static int sSignExtLogs = 0;
                if (sSignExtLogs < 8) {
                    sSignExtLogs++;
                    gdx_port_logf("[signext] wide w1=%016llX op=%02X routed to low32 resolver\n",
                                  static_cast<unsigned long long>(w1full),
                                  static_cast<unsigned>(Opcode(in.w0)));
                }
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
            /* TEXRECT coord probe (fade dash-band investigation).
             *
             * Env-gated on GDX_DIAG_TRECT=1 (zero cost otherwise -- was previously
             * unconditional). The old version used a process-lifetime `static int`
             * capped at 8, so "only 8 strips reach the bridge" was an artifact of the
             * cap outliving every transition instance, not evidence about actual
             * TEXRECT delivery -- a fade emits ~74 strip texrects across its whole
             * run, and the cap silenced all but the first 8 ever seen in the process.
             *
             * Fix: reset the counter whenever a NEW transition instance starts,
             * detected the same way transition.c's sGdxDbgSig probe does --
             * activeTransitionType changing -- but read straight from the decomp
             * `sTransition` struct via the extern already used by
             * GdxTransitionCapturePendingThisTick above (activeTransitionType is the
             * struct's first s32, offset 0x0; see the offset note there). Logging is
             * then capped PER TRANSITION instead of for the process: the first 16
             * ops print individually, every 16th after that prints a running count,
             * and a summary line (with the last op's coords) fires the moment the
             * NEXT transition instance begins, giving a definitive total for the
             * instance that just ended -- e.g. "74/74 reached the bridge" vs a
             * lower count if commands are being dropped upstream. */
            if (op == 0xE4 || op == 0xE5) {
                if (gdx_dev_gate(GDX_GATE_DIAG_TRECT)) {
                    static int32_t sTRectLastType = 0; // TRANSITION_TYPE_NONE
                    static int sTRectIndex = 0;        // running index within the current instance
                    static int sTRectTotal = 0;        // total TEXRECTs seen this instance
                    static uint32_t sTRectLastUlx = 0, sTRectLastUly = 0, sTRectLastLrx = 0, sTRectLastLry = 0;
                    static uint32_t sTRectLastTile = 0;
                    const int32_t curType = *reinterpret_cast<const int32_t*>(&sTransition[0]);
                    if (curType != sTRectLastType) {
                        if (sTRectLastType != 0 && sTRectTotal != 0) {
                            gdx_port_logf("[trect] transition type=%d ENDED: %d texrects reached the bridge "
                                          "(last ul=(%u,%u) lr=(%u,%u) tile=%u)\n",
                                          sTRectLastType, sTRectTotal, sTRectLastUlx, sTRectLastUly, sTRectLastLrx,
                                          sTRectLastLry, sTRectLastTile);
                        }
                        sTRectLastType = curType;
                        sTRectIndex = 0;
                        sTRectTotal = 0;
                    }
                    const uint32_t lrx = (in.w0 >> 12) & 0xFFF;
                    const uint32_t lry = in.w0 & 0xFFF;
                    const uint32_t tile = (in.w1 >> 24) & 0x7;
                    const uint32_t ulx = (in.w1 >> 12) & 0xFFF;
                    const uint32_t uly = in.w1 & 0xFFF;
                    sTRectLastUlx = ulx;
                    sTRectLastUly = uly;
                    sTRectLastLrx = lrx;
                    sTRectLastLry = lry;
                    sTRectLastTile = tile;
                    ++sTRectTotal;
                    if (sTRectIndex < 16 || (sTRectIndex % 16) == 0) {
                        gdx_port_logf("[trect] type=%d #%d op=%02X ul=(%u,%u) lr=(%u,%u) tile=%u stride=%zu "
                                      "f3d=%d big=%d\n",
                                      curType, sTRectIndex, op, ulx, uly, lrx, lry, tile, stride, (int)isF3DSource,
                                      (int)isBig);
                    }
                    ++sTRectIndex;
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
                const bool sDiagCountdownRaw = gdx_dev_gate(GDX_GATE_DIAG_COUNTDOWN) != 0;
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
                        /* Vertex-spike root cause: TranslateDataPointer used to be
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
                        /* Bridge asymmetry fix: the G_MTX case below passes
                           preferPhysical=!isF3DSource and sourceHint=item.source, but this
                           G_VTX case never did, so trySourceWindow() -- explicitly written
                           to reconstruct "a host-built display list and the
                           matrices/vertices it references [that] are almost always
                           allocated together in the same 4GB low32 window" -- was never
                           even attempted for vertex loads. Mirror the matrix call so
                           vertex buffers get the same last-resort reconstruction. */
                        if (!ResolveGuarded(in.w1, w1IsHostPointer, w1full, outW1, vtxRequiredBytes,
                                            /*preferPhysical=*/!isF3DSource,
                                            reinterpret_cast<uintptr_t>(item.source))) {
                            /* Segment reload raced this vertex resolution: NEVER skip a
                               vertex load silently -- later triangles index the vertex
                               buffer and would render garbage. Substitute the same
                               fallback the readability failsafe below uses (kFallbackVertices,
                               or 0 for an F3D command remapped to G_MTX). This also
                               neutralizes the possibly-torn resolved pointer before any
                               downstream deref (census / MakePersistentVtxCopy). */
                            outW1 = isF3DSource ? 0u
                                                : reinterpret_cast<uintptr_t>(kFallbackVertices);
                        }
                        /* [vtx-census]: one line per UNIQUE vertex source, NOT
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
                            /* Retargeted: general sources burned the
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
                        // Always on: was gated behind GDX_DIAG_VTX, so a
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
                    /* CRASH FAILSAFE: host-built
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
                    // Tier 3: effects vertex lerp (booster flames / side-attack quads). Placed
                    // AFTER the readability failsafe so a kFallbackVertices substitute is never
                    // rerouted. The count cap at 20 matches racer.c's largest effect batch
                    // (count*5); course track vertices can legally spill into the effects span in
                    // the EK layout (course.c:4514's inexplicable bound), and their batches are the
                    // ones this cap excludes rather than mispairing static geometry.
                    {
                        // [vtx-interp] Wider census: the in-pool miss log above caught NOTHING in a
                        // full race, which contradicts course geometry existing. Log the first few
                        // vertex operands unconditionally to see what actually flows through here.
                        // Gated on race-active: the first census burned all its shots on title-
                        // screen asset geometry (heap-decoded course gfx) before a single racer
                        // effect could draw.
                        // Census, final form. Three earlier gatings established: the first 8 vertex
                        // operands of a session are heap-decoded ASSET geometry (n=30 course
                        // chunks), in-race operands are the same, and gGdxRaceActive never rises in
                        // a scripted race. The scripted race also cannot draw the effects this tier
                        // targets at all -- racer.c's flame block is gated on racer->unk_2B3 and
                        // scaled by boostTimer, i.e. BOOST flames, and the script never boosts.
                        // So: log the first few POOL-INTERIOR, effect-sized batches whenever they
                        // finally occur (a human boosting / side-attacking), which is the activation
                        // proof for this tier.
                        static int sGdxVtxAnyLogs = 0;
                        const uint32_t gdxVtxSeenN = (in.w0 >> 12) & 0xFFu;
                        if (sGdxVtxAnyLogs < 8 && mInterpEnabled && gdxVtxSeenN != 0 &&
                            gdxVtxSeenN <= 20u && GdxP0MtxInPoolSpan(outW1)) {
                            ++sGdxVtxAnyLogs;
                            gdx_port_logf("[vtx-interp] pool batch: op=%p poolOff=0x%zX n=%u "
                                          "(effects span 0x%zX..0x%zX)\n",
                                          reinterpret_cast<void*>(outW1),
                                          static_cast<size_t>(outW1 - static_cast<uintptr_t>(gSegments[1])),
                                          gdxVtxSeenN, gdx_gfxpool_effects_vtx_offset(),
                                          gdx_gfxpool_effects_vtx_offset() + gdx_gfxpool_effects_vtx_bytes());
                        }
                    }
                    if (mInterpEnabled && outW1 != 0 && !isBig && !isF3DSource) {
                        const uint32_t gdxVtxN = (in.w0 >> 12) & 0xFFu;
                        if (gdxVtxN != 0 && gdxVtxN <= 20u &&
                            GdxEffectsVtxInSpan(outW1, static_cast<size_t>(gdxVtxN) * 16u)) {
                            outW1 = GdxVtxReroute(outW1, gdxVtxN);
                        } else if (GdxP0MtxInPoolSpan(outW1)) {
                            // [vtx-interp] Attribution for a silent no-op: a full race measured ZERO
                            // effects-vertex reroutes while engine flames were visibly drawing, so
                            // either the operands are not where the span test looks or the count
                            // gate excludes them. Log the first few POOL-INTERIOR vertex operands
                            // that fail the effects test, with the numbers needed to say which.
                            static int sGdxVtxMissLogs = 0;
                            if (sGdxVtxMissLogs < 6) {
                                ++sGdxVtxMissLogs;
                                gdx_port_logf("[vtx-interp] in-pool miss: op=%p poolOff=0x%zX n=%u "
                                              "(effects span 0x%zX..0x%zX)\n",
                                              reinterpret_cast<void*>(outW1),
                                              static_cast<size_t>(outW1 - static_cast<uintptr_t>(gSegments[1])),
                                              gdxVtxN, gdx_gfxpool_effects_vtx_offset(),
                                              gdx_gfxpool_effects_vtx_offset() + gdx_gfxpool_effects_vtx_bytes());
                            }
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
                        const bool sDiagCountdown = gdx_dev_gate(GDX_GATE_DIAG_COUNTDOWN) != 0;
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
                    if (!ResolveGuarded(in.w1, w1IsHostPointer, w1full, outW1, 64,
                                        /*preferPhysical=*/!isF3DSource,
                                        reinterpret_cast<uintptr_t>(item.source))) {
                        /* Raced reload: drop to 0. This never reaches the interpreter as
                           a null matrix pointer -- the shared post-switch check further
                           down (outW1 == 0 for kOpVtx/kOpMtx/kOpMovemem/kOpSetTextureImage)
                           always intercepts it first: under GDX_LEGACY_RESOLVE it
                           substitutes FallbackDataPointer's result (kFallbackIdentityMtx),
                           otherwise the whole command is dropped via `continue` before the
                           interpreter ever sees it -- same as a genuine resolution failure. */
                        outW1 = 0;
                    }
                    if ((outW1 & 7u) != 0) {
                        outW1 = 0;
                    }
                    {
                        // Matrix resolution probe: unlike the vertex paths above,
                        // this call already requires the full 64-byte Mtx to be
                        // readable, so a dropped resolution here (rather than a
                        // garbage read) is the expected failure mode. Log it so a
                        // human tester can correlate dropped machine matrices with
                        // observed z-fighting/punch-through during races.
                        // Always on: see the [vtx-spike]/[vtx-dropped] note
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
                    /* CRASH FAILSAFE (hoisted pre-copy): the w1IsHostPointer fast
                       path above takes w1full verbatim with no readability
                       check, and the resolver path can return an
                       under-validated candidate (reachable now that
                       ResolveGeneratedAssetStub's E1 bounds check is
                       offset-only -- a near-end-of-image interior match can
                       return a valid-start/short-tail pointer). A matrix load
                       dereferences 64 bytes, so drop any matrix pointer whose
                       full 64 bytes are not readable (0 is the dropped-matrix
                       sentinel the shared post-switch failsafe further down
                       consumes -- see the kOpMtx resolve-failure comment above).
                       MUST run BEFORE MakePersistentMtxCopy below: that helper
                       always returns a fresh, fully-readable 64-byte
                       allocation, so validating outW1 AFTER the copy (the
                       original placement) checked the wrong pointer and never
                       protected the copy's own raw 64-byte read. */
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
                    if (outW1 != 0) {
                        /* Same byte-order proxy as the G_VTX paths: matrices
                           referenced from big-endian DLs are static asset Mtx
                           data and need word-swapping for the interpreter's
                           host-order reads; host-built DLs reference
                           Matrix_ToMtx output, which is already host-order.
                           MakePersistentMtxCopy also clamps internally now
                           (mirrors MakePersistentVtxCopy's [vtx-clamp] idiom),
                           so this is defense in depth, not the only guard. */
                        if (isBig) {
                            outW1 = MakePersistentMtxCopy(outW1);
                        }
                        outW1 = NormalizeLusDirectPointer(outW1);
                    }
                    // #16 countdown-matrix content probe: dump the resolved 64-byte
                    // Mtx as raw hex words, armed on the same flag as the [rect]
                    // probe, to sanity-check the countdown billboard's modelview
                    // matrix for degenerate (all-zero) or wildly-out-of-range values
                    // instead of just confirming the pointer resolved.
                    {
                        const bool sDiagCountdownMtx = gdx_dev_gate(GDX_GATE_DIAG_COUNTDOWN) != 0;
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
                    // P0/P1 (debug-only): scratch-slot indirection for pool matrices. When
                    // GDX_INTERP_P0 or GDX_INTERP_P1 is set and this fully-resolved 64-byte matrix
                    // lives inside the current GfxPool segment-1 span (host-built modelview loads),
                    // copy it into a stable scratch slot and rewrite the command to point there,
                    // recording (prev,cur,scratch,snap). At t=1 the scratch is a byte copy of the
                    // pool matrix (Correction-1 transparency); in P1 the per-sub-frame refill writes
                    // lerp(prev,cur,t). Static/persistent asset matrices took the isBig ->
                    // MakePersistentMtxCopy path above and resolve OUTSIDE the pool span, so they
                    // pass through untouched here (HUD/2D ortho draws never load segment-1
                    // pool matrices, so they are structurally excluded from interpolation).
                    // G_MTX_PROJECTION (0x04) loads are included whenever mInterpCamera is set,
                    // which now ships ON. race.c:250 loads gfxPool->unk_20208[] -- camera.c's
                    // combined projection*view camera -- with that flag, and course.c emits no
                    // gSPMatrix at all, so excluding projection froze BOTH the camera and the whole
                    // track at 60 Hz while racer model matrices kept interpolating: objects smoothed
                    // against a static world, which is what tore the CPU-baked booster flames off
                    // the machines. See gdx_interp.h CameraInterpActive for why lerping a combined
                    // projection*view is exact rather than an approximation.
                    // F3DEX2's `p^G_MTX_PUSH` params encoding never touches bit 0x04, so the raw
                    // low byte can be tested without un-XOR'ing first.
                    // [mtx-census] GDX_DIAG_MTX=1. Phase C research established by READING that the
                    // booster flames draw under an identity matrix at segment 2 offset 0, loaded by
                    // a G_MTX inside the aSetupBoosterDL ROM asset -- and that this load is ignored
                    // here because it resolves OUTSIDE the pool span. Neither fact was confirmed at
                    // runtime. This reports every distinct G_MTX operand that misses the reroute,
                    // with the top row of the matrix it points at, so "is it identity" and "does the
                    // bridge see it at all" are answered by measurement before any decomp change.
                    {
                        static const bool sDiagMtx = std::getenv("GDX_DIAG_MTX") != nullptr;
                        if (sDiagMtx && mInterpEnabled && outW1 != 0 && !GdxP0MtxInPoolSpan(outW1) &&
                            ReadableByteLimit(outW1) >= 64u) {
                            // Report each distinct operand once. A flat array rather than a set:
                            // the cap is 64 and this runs inside the display-list walk.
                            static uintptr_t sSeen[64];
                            static size_t sSeenCount = 0;
                            bool known = false;
                            for (size_t i = 0; i < sSeenCount; ++i) {
                                if (sSeen[i] == outW1) {
                                    known = true;
                                    break;
                                }
                            }
                            if (!known && sSeenCount < 64u) {
                                sSeen[sSeenCount++] = outW1;
                                const int32_t* w = reinterpret_cast<const int32_t*>(outW1);
                                gdx_port_logf("[mtx-census] non-pool G_MTX raw=%08X resolved=%p big=%d "
                                              "params=%02X row0=%08X %08X %08X %08X\n",
                                              in.w1, reinterpret_cast<void*>(outW1), isBig ? 1 : 0,
                                              (unsigned) (in.w0 & 0xFFu), w[0], w[1], w[2], w[3]);
                            }
                        }
                    }
                    if (mInterpEnabled && outW1 != 0 && !isBig &&
                        (((in.w0 & 0x04u) == 0u) || mInterpCamera) &&
                        GdxP0MtxInPoolSpan(outW1)) {
                        outW1 = GdxP0RerouteMtx(outW1, (in.w0 & 0x04u) != 0u);
                    }
                    break;

                case kOpMovemem:
                    if (!ResolveGuarded(in.w1, w1IsHostPointer, w1full, outW1)) {
                        continue;  // raced reload: skip the command this frame
                    }
                    if (outW1 != 0) {
                        outW1 = NormalizeLusDirectPointer(outW1);
                    }
                    // Tier 2: carousel viewport lerp. Index byte 8 == G_MV_VIEWPORT; GdxVpReroute
                    // itself rejects any pointer outside D_i5_80118FF0, so every other viewport in
                    // the game passes through untouched.
                    if (mInterpEnabled && outW1 != 0 && (in.w0 & 0xFFu) == 8u) {
                        outW1 = GdxVpReroute(outW1);
                    }
                    break;

                case kOpSetColorImage:
                case kOpSetDepthImage:
                    if (!ResolveGuarded(in.w1, w1IsHostPointer, w1full, outW1)) {
                        continue;  // raced reload: skip the command this frame
                    }
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
                        /* TOCTOU guard (Create Machine entry, thread_id=3 AV in
                           strlen): snapshot the segment-reload epoch BEFORE touching
                           any segment-backed state. A mode transition on the game
                           thread reloads segments 4/7/9 (decomp_port.c
                           gdx_load_mode_segments) -- rewriting the carve bytes AND
                           swapping the gSegments[] bases -- while this thread reads
                           them unsynchronized. */
                        const uint32_t settimgEpoch = GdxSegmentEpochSnapshot();
                        const uintptr_t translated =
                            w1IsHostPointer ? w1full : TranslateDataPointer(in.w1);
                        /* If a reload raced a segment-backed resolution (odd or
                           changed epoch), the translated pointer and the bytes it
                           addresses may be torn. Host-pointer textures never read
                           gSegments[], so they are exempt (no behavior change).
                           Drop the texture for this one frame -- the previous
                           binding persists and the skip is invisible during a mode
                           transition -- rather than sampling / stringifying
                           half-written state. Wait-free: only atomic loads ran. */
                        const bool segmentReloadRaced =
                            !w1IsHostPointer && !GdxSegmentEpochStable(settimgEpoch);
                        if (segmentReloadRaced) {
                            if (mStats != nullptr) mStats->skippedTextures++;
                            continue;
                        }
                        /* A resolution is USABLE -- for the O2R/pack OTR-filepath
                           emit and for every diagnostic that dereferences the
                           resolved bytes as memory or a string -- only if it
                           produced a non-null pointer. A failed/partial resolution
                           (the [resolve-fail] path returns 0) hard-skips the entire
                           census / pack-override / logging branch below; the normal
                           unresolved-fallback emit (FallbackDataPointer, further
                           down) is unchanged. */
                        const bool resolutionUsable = (translated != 0);
                        // Only emit the O2R filepath opcode for BSS-stub textures (asset-segment
                        // symbols with a 1:1 O2R resource). RDRAM-backed textures are contiguous
                        // multi-tile buffers where the game issues many G_LOADBLOCKs with
                        // increasing ULS offsets across the full region — the O2R resource only
                        // covers the first tile and causes out-of-bounds reads for later bands.
                        // Those textures must go through the raw-copy path (1MB slice of RDRAM).
                        const char* o2rKey = (!w1IsHostPointer && resolutionUsable && !IsRdramHostPointer(translated))
                                                 ? gdx_lookup_asset_segment_o2r_key(in.w1)
                                                 : nullptr;
                        const char* texCensusPath = "rawcopy"; /* [tex-census] delivery classification */
                        /* Tier-B texture-pack override. `translated` is the unified host/RDRAM
                           source pointer for BOTH delivery paths (host-pointer wide packets AND
                           raw-copy RDRAM), so a single lookup here covers common assets (fonts,
                           portraits, title art) regardless of which branch would otherwise take them.
                           When packs are enabled and this buffer is a registered common asset that a
                           mounted pack replaces, rewrite the load to the OTR-filepath opcode -- the
                           exact same mechanism as the Tier-A o2rKey emit. The override existence check
                           is cached per key per pack epoch, so a hit/miss costs at most one registry
                           scan; with the CVar off this is byte-identical to before (no lookup runs). */
                        const char* packPath = nullptr;
                        /* Same multi-tile exclusion as the Tier-A o2rKey emit above
                           (!IsRdramHostPointer): an RDRAM-backed buffer is a contiguous
                           multi-tile atlas sampled at many ULS offsets, and a single-OTEX
                           override only covers the first band — replacing it garbled every
                           later band (the title-screen text corruption with a font pack
                           active). Atlas replacement needs per-tile keying; until then those
                           buffers keep the raw-copy path. */
                        if (!o2rKey && resolutionUsable && !IsRdramHostPointer(translated) &&
                            gdx_workshop_texture_packs_enabled()) {
                            const char* assetKey = GDiffuser_LookupLoadedAssetKey(
                                reinterpret_cast<const void*>(translated), 0, 0);
                            if (assetKey != nullptr) {
                                packPath = GdxWorkshopLookupOverridePath(assetKey);
                            }
                        }
                        if (o2rKey) {
                            texCensusPath = "o2r";
                            outW0 = (outW0 & 0x00FFFFFFu) | (static_cast<uintptr_t>(kOpSetTextureImageOtrFilepath) << 24);
                            outW1 = reinterpret_cast<uintptr_t>(o2rKey);
                        } else if (packPath) {
                            texCensusPath = "pack-o2r";
                            outW0 = (outW0 & 0x00FFFFFFu) |
                                    (static_cast<uintptr_t>(kOpSetTextureImageOtrFilepath) << 24);
                            outW1 = reinterpret_cast<uintptr_t>(packPath);
                        } else if (w1IsHostPointer) {
                            // Real host pointer to texel data — use directly,
                            // UNLESS it is a generated asset stub (see
                            // ResolveWideAssetStubPointer): stubs must be re-routed to
                            // the decoded asset image or the sampler reads EXE data.
                            // The upcoming load-size estimate is computed here (rather
                            // than left at the resolver's default requiredBytes=1) so
                            // E1's bounds check in ResolveGeneratedAssetStub validates
                            // against the actual copy size instead of silently
                            // accepting a 1-byte-wide match; reused below for the
                            // native-RGBA16 copy so it is computed only once.
                            const size_t estimatedBytes =
                                EstimateRawTextureCopyBytes(item.source, i, item.limit, stride, isBig);
                            const uintptr_t stubResolved = ResolveWideAssetStubPointer(
                                w1full, mModuleBegin, mModuleEnd, std::max<size_t>(estimatedBytes, 1));
                            const uintptr_t hostTextureSource =
                                (stubResolved != 0) ? stubResolved : w1full;
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
                            /* [transition-cap] probe (guarded): when this
                               SETTIMG source is the recorded transition capture
                               buffer, report whether the byteswap-applying native
                               RGBA16 range covers it. native=1 => the host-endian
                               (little) capture IS swapped for the big-endian
                               RGBA5551 reader (correct, no band); native=0 => it is
                               read raw => red/blue swap + bit-shift = the title->menu
                               noise band. One line per unique source; the gate is
                               zero-size outside a transition so gameplay pays nothing
                               and no non-transition texture is ever touched. */
                            if (gDiagTransitionCaptureSize != 0 &&
                                w1full >= gDiagTransitionCaptureBegin &&
                                w1full < gDiagTransitionCaptureBegin + gDiagTransitionCaptureSize) {
                                static uintptr_t sTransCapSeen[8] = {};
                                static int sTransCapSeenCount = 0;
                                bool capSeen = false;
                                for (int s = 0; s < sTransCapSeenCount; s++) {
                                    if (sTransCapSeen[s] == w1full) {
                                        capSeen = true;
                                        break;
                                    }
                                }
                                if (!capSeen && sTransCapSeenCount < 8) {
                                    sTransCapSeen[sTransCapSeenCount++] = w1full;
                                    const bool nativeApplied = IsNativeRgba16Range(w1full, 2);
                                    gdx_port_logf("[transition-cap] SETTIMG src=%p native=%d "
                                                  "(1=byteswapped/correct, 0=raw/red-blue-swap band)\n",
                                                  reinterpret_cast<void*>(w1full), nativeApplied ? 1 : 0);
                                }
                            }
                            /* CPU framebuffer readback and transition capture buffers contain
                               host-order RGBA5551 words.  Most host pointers refer to generated
                               texture bytes that already use the N64 byte order and therefore
                               must remain direct, but ranges registered through
                               gdx_set_native_rgba16_texture_range need their two bytes swapped
                               before Fast3D's N64 texture reader consumes them.

                               The narrow/raw path already applies this policy in
                               MakePersistentRawTextureCopy.  Wide Gfx packets previously skipped
                               that path entirely, despite the [transition-cap] diagnostic
                               reporting native=1, so phased strips sampled little-endian words
                               as a big-endian stream and produced the multicolour noise bands.
                               Copy only the exact upcoming load extent and preserve the direct
                               path for every non-native host texture. */
                            if (IsNativeRgba16Range(hostTextureSource, 2)) {
                                // estimatedBytes is already computed above (hoisted so the
                                // ResolveWideAssetStubPointer call gets the real requiredBytes).
                                size_t readable = RegisteredHostRemaining(hostTextureSource);
                                if (readable == 0) {
                                    readable = ReadableByteLimit(hostTextureSource);
                                }
                                /* Keep the copy within the registered native-RGBA16
                                   extent. The load-size estimate can round up past the
                                   registered image (the WIPE transition issues one wide
                                   LOADBLOCK whose block-rounded estimate is one row larger
                                   than WIDTH*HEIGHT*2). If the copy size spills past the
                                   native range, CopyRawTextureBytes' all-or-nothing
                                   IsNativeRgba16Range check fails and the ENTIRE copy is
                                   memcpy'd WITHOUT the byte swap, so Fast3D samples the
                                   host-order words as big-endian and the un-revealed wipe
                                   region renders as rainbow noise. Clamping to the native
                                   extent guarantees the swap is applied; the estimate's
                                   extra tail bytes lie beyond the last real load anyway. */
                                const size_t nativeRemaining = NativeRgba16RangeRemaining(hostTextureSource);
                                if (nativeRemaining != 0 && nativeRemaining < readable) {
                                    readable = nativeRemaining;
                                }
                                size_t required = estimatedBytes;
                                if (required == 0) {
                                    required = std::min(readable, kMaxRawTextureCopyBytes);
                                }
                                required = std::min(required, readable);

                                bool textureCopyRefreshed = false;
                                outW1 = MakePersistentRawTextureCopy(hostTextureSource, required,
                                                                     &textureCopyRefreshed);
                                if (mStats != nullptr && textureCopyRefreshed) {
                                    mStats->textureCopyBytes += required;
                                }
                                texCensusPath = "host-native-rgba16";
                            } else {
                                texCensusPath = (stubResolved != 0) ? "widestub" : "hostptr";
                                outW1 = hostTextureSource;
                            }
                            if (outW1 != 0) {
                                outW1 = NormalizeLusDirectPointer(outW1);
                            }
                        } else {
                            outW1 = TranslateTexturePointer(in.w1, item.source, i, item.limit, isBig, stride);
                            if (outW1 != 0) {
                                outW1 = NormalizeLusDirectPointer(outW1);
                            }
                        }
                        /* Residual TOCTOU window (Create Machine entry, thread_id=3 AV
                           in strlen): the early epoch check above passed, but the
                           pointer resolution, the O2R/pack lookups, and (for RDRAM/
                           native sources) MakePersistentRawTextureCopy just READ
                           segment-backed state. Re-check the SAME snapshot now -- after
                           all resolution and content copies, before the resolved bytes
                           are consumed by the census/diagnostics below or handed to the
                           interpreter. This brackets the whole content read as a seqlock:
                           if a reload began anywhere in that window, drop the texture for
                           this frame (previous binding persists) rather than emitting a
                           torn copy. Wait-free: one more acquire load. */
                        if (!w1IsHostPointer && !GdxSegmentEpochStable(settimgEpoch)) {
                            if (mStats != nullptr) mStats->skippedTextures++;
                            NoteEpochSkip();
                            continue;
                        }
                        /* [tex-census]: one line
                           per UNIQUE SETTIMG source across the WHOLE session (NOT
                           race-gated -- pause menu / machine select / menus are the
                           interesting cases). Pairs every on-screen texture with its delivery
                           path and the first bytes LUS consumes, mirroring the audio
                           [sample-census]. Fingerprint at +0x20 to skip transparent
                           glyph corners when the buffer is large enough. */
                        {
                            /* Recalibrated: 160 unique slots burned out in
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
                            // [tex-census]/[ci-dump] high-frequency per-draw census:
                            // silent unless GDX_DIAG_VERBOSE=1.
                            if (gdx_diag_verbose() && !censusDup && censusHasBudget) {
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
                                    /* [ci-dump] (options/pause CI-cell investigation, scope item A):
                                       for CI-format sources, the discriminator between "delivered
                                       wrong" and "interpreted wrong" is the raw tile bytes at the
                                       address LUS will consume -- 32 bytes from offset 0 (CI text
                                       tiles are small; the +0x20 fingerprint skip would overshoot).
                                       Compared offline against the same tile decoded from ROM. */
                                    if (cFmt == 2u && outW1 != 0 && ReadableByteLimit(outW1) >= 32) {
                                        const uint8_t* cd = reinterpret_cast<const uint8_t*>(outW1);
                                        gdx_port_logf("[ci-dump] raw=%08X b0=%02X%02X%02X%02X%02X%02X%02X%02X"
                                                      "%02X%02X%02X%02X%02X%02X%02X%02X "
                                                      "b16=%02X%02X%02X%02X%02X%02X%02X%02X"
                                                      "%02X%02X%02X%02X%02X%02X%02X%02X\n",
                                                      in.w1,
                                                      cd[0], cd[1], cd[2], cd[3], cd[4], cd[5], cd[6], cd[7],
                                                      cd[8], cd[9], cd[10], cd[11], cd[12], cd[13], cd[14], cd[15],
                                                      cd[16], cd[17], cd[18], cd[19], cd[20], cd[21], cd[22], cd[23],
                                                      cd[24], cd[25], cd[26], cd[27], cd[28], cd[29], cd[30], cd[31]);
                                    }
                                }
                            }
                        }
                        /* Create Machine gibberish probe:
                           mode-gated one-shot census. The session-wide [tex-census] budget
                           burns out during boot before Create Machine is reached, so this
                           block logs EVERY unique SETTIMG source while GET_MODE(gGameMode)
                           == 0x10 (GAMEMODE_CREATE_MACHINE), with the raw source, the
                           resolved host pointer LUS consumes, fmt/siz/width, and the first
                           16 bytes at that pointer. Compare those bytes against the known-good
                           disk decode (0x00C8A270 bg strip, 0x00C8CE60 OK button) to prove
                           whether the interpreter receives correct BE data or wrong bytes.
                           Bounded to 512 unique sources: the old 64 saturated on the first frame,
                           so parts swapped in later were never censused. Remove once #1 is
                           root-caused. */
                        if ((gGameMode & 0x1F) == 0x10) {
                            static uint32_t sCmSeen[512] = {};
                            static int sCmCount = 0;
                            bool cmDup = false;
                            for (int s = 0; s < sCmCount; s++) {
                                if (sCmSeen[s] == in.w1) { cmDup = true; break; }
                            }
                            if (!cmDup && sCmCount < 512) {
                                sCmSeen[sCmCount++] = in.w1;
                                const uint32_t cw0 = static_cast<uint32_t>(in.w0);
                                const uint32_t cFmt = (cw0 >> 21) & 0x7;
                                const uint32_t cSiz = (cw0 >> 19) & 0x3;
                                const uint32_t cWidth = (cw0 & 0xFFF) + 1;
                                uint8_t cb[16] = {};
                                if (outW1 != 0 && ReadableByteLimit(outW1) >= sizeof(cb)) {
                                    std::memcpy(cb, reinterpret_cast<const void*>(outW1), sizeof(cb));
                                }
                                gdx_port_logf("[GDX-DBG cm] raw=%08X path=%s fmt=%u siz=%u w=%u out=%p "
                                              "b0..15=%02X%02X%02X%02X%02X%02X%02X%02X"
                                              "%02X%02X%02X%02X%02X%02X%02X%02X\n",
                                              in.w1, texCensusPath, cFmt, cSiz, cWidth,
                                              reinterpret_cast<void*>(outW1),
                                              cb[0], cb[1], cb[2], cb[3], cb[4], cb[5], cb[6], cb[7],
                                              cb[8], cb[9], cb[10], cb[11], cb[12], cb[13], cb[14], cb[15]);
                            }
                        }
                        /* Create Machine untextured 3D preview probe:
                           segment-3 (machine_custom_gfx) SETTIMG census, gated on
                           GAMEMODE_CREATE_MACHINE. Separates the host-pointer
                           path bypassing the placeholder redirect (isPlaceholder=0, zero
                           bytes) from a healthy resolve. Bounded to 512 unique sources: the old 32
                           filled in 2 ms, well before the machine was built. Remove once
                           root-caused. */
                        if ((gGameMode & 0x1F) == 0x10) {
                            static uint32_t sCmTexSeen[512] = {};
                            static int sCmTexCount = 0;
                            bool cmDup = false;
                            for (int s = 0; s < sCmTexCount; s++) {
                                if (sCmTexSeen[s] == in.w1) { cmDup = true; break; }
                            }
                            if (!cmDup && sCmTexCount < 512) {
                                sCmTexSeen[sCmTexCount++] = in.w1;
                                uint8_t cmb[8] = {};
                                if (outW1 != 0 && ReadableByteLimit(outW1) >= sizeof(cmb)) {
                                    std::memcpy(cmb, reinterpret_cast<const void*>(outW1), sizeof(cmb));
                                }
                                gdx_port_logf("[cm-seg3] raw=%08X path=%s out=%p isPlaceholder=%d "
                                              "b0..7=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                                              in.w1, texCensusPath, reinterpret_cast<void*>(outW1),
                                              IsAssetPlaceholderPointer(in.w1) ? 1 : 0,
                                              cmb[0], cmb[1], cmb[2], cmb[3], cmb[4], cmb[5], cmb[6], cmb[7]);
                            }
                        }
                        // Resolution probe. Classifies the RESOLVED SOURCE
                        // (`translated`), not the persistent-copy output — the copy is
                        // always an unregistered heap buffer, so classifying outW1 only
                        // reports which delivery path ran, never whether the data is
                        // correct. Fingerprints the bytes LUS will actually consume and
                        // flags MIO0-compressed streams reaching the sampler (stripes).
                        if (gdx_dev_gate(GDX_GATE_DIAG_SETTIMG) && gGdxRaceActive != 0) {
                            static int sSettimgCount = 0;
                            // Per-course budget. gGdxRaceActive is a latch that never clears, so a
                            // process-lifetime cap could only ever describe a GP's first course;
                            // GET_MODE moves on every race/next-course transition.
                            static int sSettimgMode = -1;
                            const int settimgMode = gGameMode & 0x1F;
                            if (settimgMode != sSettimgMode) {
                                sSettimgMode = settimgMode;
                                sSettimgCount = 0;
                            }
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
                                // One file per process, opened once and held: the old fixed name
                                // truncated the previous session's trace on every launch.
                                static FILE* sSettimgFile = nullptr;
                                if (sSettimgFile == nullptr) {
#ifdef _WIN32
                                    const unsigned long tracePid =
                                        static_cast<unsigned long>(GetCurrentProcessId());
#else
                                    const unsigned long tracePid = static_cast<unsigned long>(getpid());
#endif
                                    char tracePath[64];
                                    std::snprintf(tracePath, sizeof(tracePath), "settimg-trace-%lu.txt",
                                                  tracePid);
                                    sSettimgFile = fopen(tracePath, "w");
                                }
                                if (sSettimgFile != nullptr) {
                                    fprintf(sSettimgFile,
                                            "T raw=%08X src=%p scls=%s out=%p fmt=%u siz=%u w=%u "
                                            "fp=%02X%02X%02X%02X%02X%02X%02X%02X sum=%08X mio0=%d dl=%p\n",
                                            in.w1, reinterpret_cast<void*>(translated), classify(translated),
                                            reinterpret_cast<void*>(outW1), fmt, siz, width,
                                            head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7],
                                            sum, mio0, item.source);
                                    // Flushed per line so a crash keeps the trace, as the old
                                    // open/close-per-line pattern did.
                                    fflush(sSettimgFile);
                                }
                            }
                        }
                    }
                    // Course-Edit node-info probe. Env-gated on GDX_DIAG_NODEINFO and, unlike the
                    // GDX_DIAG_SETTIMG trace above, NOT race-gated, so it is live in the editor. It
                    // fires only when this SETTIMG's raw N64 source lands in one of two windows.
                    //
                    // Window 1 -- the seg-9 node-info textures: NumberSheet (0x09000C48), NumberTex
                    // (0x09001788), InfoBackground (0x09002F88), InfoFontSheet (0x09003408). RULED
                    // OUT for this defect: 0x7031F0 resolves to an ovl_i3 verbatim HUD array, not an
                    // editor asset; all four symbols are present and correctly sized/addressed in
                    // port/gen/EkAssetBindings.c; and they are RGBA16/I4, not CI8, so the two-half
                    // CI8 cache cannot apply.
                    //
                    // Window 2 -- the blank info panels. Those do NOT use the 64DD kanji glyph path or
                    // Font_*; they go through func_xk1_8002924C (expansion_kit/A6340.c:60), which
                    // blits 8x8 cells out of three I4 128x48 sheets at seg-7 0x07009080 / 0x07009C80 /
                    // 0x0700A880 (0xC00 each, contiguous through 0x0700B480). Those sheets hold real,
                    // legible glyph data on the translated disk (1625/1401/1566 nonzero bytes over the
                    // full 3072-byte buffers), so the asset is not the defect. Note the deliberate
                    // FULL-buffer count: all three open with a zero run for the blank space glyph at
                    // cell 0, so a fixed-prefix checksum reads them as empty and lies.
                    //
                    // The live question is resolution. seg-7 is ALSO populated from cartridge ROM
                    // (expansion_kit_textures_beta), and only the fact that TryResolveAddress scans
                    // gN64AddressRanges ahead of the segment table keeps the disk copy winning. A
                    // requiredBytes over-estimate would drop the range match and fall through to that
                    // cart data, which is structureless noise -- and the budget here is exact, 3072 of
                    // 3072, with no slack to absorb one.
                    //
                    // Reading the output: no lines at all means the draw never runs (an editor-state
                    // gate); lines with out=NULL or a short avail mean the resolve is failing and the
                    // cart fallback is what reaches the screen. Zero cost unless the env is set.
                    {
                        const bool inNodeInfoWindow = in.w1 >= 0x09000C48u && in.w1 < 0x09003808u;
                        const bool inSetupFontWindow = in.w1 >= 0x07009080u && in.w1 < 0x0700B480u;

                        if (gdx_dev_gate(GDX_GATE_DIAG_NODEINFO) && !w1IsHostPointer &&
                            (inNodeInfoWindow || inSetupFontWindow)) {
                            static int sNodeInfoLogs = 0;
                            if (sNodeInfoLogs < 400) {
                                ++sNodeInfoLogs;
                                // outW1 is the final resolved host pointer LUS samples this
                                // command; out=NULL / avail<8 means the seg-9 source did not
                                // resolve to live pixels.
                                const size_t avail = (outW1 != 0) ? ReadableByteLimit(outW1) : 0;
                                uint8_t nib[8] = {};
                                if (avail >= sizeof(nib)) {
                                    std::memcpy(nib, reinterpret_cast<const void*>(outW1), sizeof(nib));
                                }
                                // Label by the window that matched. The seg-9 ladder alone reported
                                // every seg-7 hit as NumberSheet, since 0x0700xxxx < 0x09001788.
                                const char* asset =
                                    inSetupFontWindow
                                        ? ((in.w1 < 0x07009C80u)   ? "SetupFontSheet0"
                                           : (in.w1 < 0x0700A880u) ? "SetupFontSheet1"
                                                                   : "SetupFontSheet2")
                                    : (in.w1 < 0x09001788u)   ? "NumberSheet"
                                    : (in.w1 < 0x09002F88u)   ? "NumberTex"
                                    : (in.w1 < 0x09003408u)   ? "InfoBackground"
                                                              : "InfoFontSheet";
                                gdx_port_logf("[nodeinfo] %s raw=%08X out=%p avail=%zu "
                                              "b0..7=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                                              asset, in.w1, reinterpret_cast<void*>(outW1), avail,
                                              nib[0], nib[1], nib[2], nib[3], nib[4], nib[5], nib[6], nib[7]);
                            }
                        }
                    }
                    break;

                case kOpDl:
                    outW1 = ResolveDisplayListGuarded(in.w1, item.source, i, w1IsHostPointer, w1full);
                    break;

                case kOpMoveword:
                    if (WordParam(in.w0) == kMovewordSegmentIndex) {
                        const uint8_t segIdx = static_cast<uint8_t>((in.w0 & 0xFFFF) / 4);
                        /* gSPSegment(seg, base) packs the segment
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
                        /* Segment-table WRITE guard: a mode transition on the game
                           thread reloads segments 4/7/9 and swaps gSegments[] bases.
                           Snapshot the epoch before resolving; if a reload raced it,
                           do NOT publish a base derived from a torn gSegments[]/segment
                           image -- skip the WRITE, retain the stale base for this one
                           frame (the DL re-emits the segment set next frame), and emit
                           the stale base. This is a write race, worse than the reads. */
                        const uint32_t mwEpoch =
                            w1IsHostPointer ? 0u : GdxSegmentEpochSnapshot();
                        uintptr_t translated;
                        if (w1IsHostPointer) {
                            translated = w1full;
                        } else {
                            translated = TranslateDataPointer(in.w1);
                            if (translated == 0 && in.w1 == 0 && gdx_rdram != nullptr) {
                                translated = reinterpret_cast<uintptr_t>(gdx_rdram);
                            }
                        }
                        if (!w1IsHostPointer && !GdxSegmentEpochStable(mwEpoch)) {
                            NoteEpochSkip();
                            outW1 = (segIdx < kGfxSegmentCount)
                                        ? gSegments[segIdx]
                                        : static_cast<uintptr_t>(in.w1);
                            break;
                        }
                        if (translated != 0 && segIdx < kGfxSegmentCount) {
                            const uintptr_t normalized = NormalizeLusDirectPointer(translated);
                            /* In-DL segment repoints were invisible in the log (only the
                               CPU-side setters print [seg] lines), which made "element
                               draws through an unset/stale segment" bugs undiagnosable.
                               Log the first few repoints of EVERY segment. */
                            if (gSegments[segIdx] != normalized) {
                                static int sSegRepointLogs[kGfxSegmentCount] = {};
                                // [seg-dl] successful repoint is informational per-draw:
                                // silent unless GDX_DIAG_VERBOSE=1 (the FAILED variant below
                                // is an error family and stays always-on).
                                if (gdx_diag_verbose() && sSegRepointLogs[segIdx] < 6) {
                                    ++sSegRepointLogs[segIdx];
                                    gdx_port_logf("[seg-dl] moveword seg=%X raw=%08X %p -> %p\n", segIdx, in.w1,
                                                  reinterpret_cast<void*>(gSegments[segIdx]),
                                                  reinterpret_cast<void*>(normalized));
                                }
                                /* Early-race floor probe: course-select's
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
                                    // [seg-dl-race] informational probe: silent unless GDX_DIAG_VERBOSE=1.
                                    if (gdx_diag_verbose() && sSegARaceRepointLogs < 32) {
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
                    if (!ResolveGuarded(in.w1, w1IsHostPointer, w1full, outW1)) {
                        continue;  // raced reload: skip the command this frame
                    }
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
                        outW1 = ResolveDisplayListGuarded(in.w1, item.source, i, w1IsHostPointer, w1full);
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
                       When the packet is WIDE and carries a
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

                    /* Any recognized microcode load ends a pending L3DEX2 line section;
                       the L3DEX2 arm below re-arms it. */
                    l3dexLineSection = false;

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
                           track previews). Fast3D has no line primitive, but the
                           section's non-line commands (G_VTX, PipeSync, prim
                           color, render mode) are F3DEX2-compatible: run the
                           section as Standard and rewrite each G_LINE3D (opcode
                           0xB5 under F3DEX_GBI_2, NOT the classic F3D 0x08)
                           into OTR_G_LINE3D_GDX (see case 0xB5 below), which the
                           interpreter expands into a screen-space quad. This
                           replaces the old wholesale section skip, which avoided
                           garbage frames but never rendered the course spline or
                           control-point connector lines at all. The counter now
                           counts translated sections rather than skips. */
                        variant = Fast::F3dex2Variant::Standard;
                        l3dexLineSection = true;
                        if (mStats != nullptr) {
                            mStats->l3dexUcodeSkips++;
                            if (mStats->firstL3dexUcodeRaw == 0) {
                                mStats->firstL3dexUcodeRaw = in.w1;
                            }
                        }
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

                // L3DEX2 G_LINE3D: meaningful only inside a Course Edit line section.
                // Under F3DEX_GBI_2 (this build: port CMake defines F3DEX_GBI_2=1),
                // G_LINE3D == 0x08 -- gbi.h line 121, INSIDE the "#ifdef F3DEX_GBI_2"
                // block that opens at line 90. The (G_IMMFIRST-10) == 0xB5 definition at
                // line 148 belongs to the LEGACY F3D branch after the "#else" at line 122
                // -- that #else's trailing block comment merely NAMES the condition being
                // closed (Nintendo style), it does not open the GBI_2 block; a previous
                // fix misread it and relabeled this case 0xB5, which let real 0x08 line
                // commands pass through verbatim to the interpreter (owner crash log:
                // "Unhandled OP code: 0x8, for loaded ucode: 4") and crash once 4+
                // control points made lines draw.
                // gSPLineW3D packs into w0 alone: w0[31:24]=0x08, w0[23:16]=v0*2,
                // w0[15:8]=v1*2, w0[7:0]=wd, w1=0. Rewrite to the port's custom line
                // command preserving the operand byte layout; the interpreter expands
                // it into a screen-space quad (vertex indices arrive *2, handler /2).
                // Outside a line section this opcode is dropped rather than misexecuted
                // (0x08 is G_RESERVED3 in plain F3DEX2, never legitimately emitted).
                case 0x08:
                    if (!l3dexLineSection) {
                        continue;
                    }
                    outW0 = (static_cast<uintptr_t>(0x41u) << 24) | (in.w0 & 0x00FFFFFFu);
                    outW1 = 0;
                    break;

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
                        outW1 = ResolveDisplayListGuarded(in.w1, item.source, i, /*hostPtr=*/false, 0);
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

                        uintptr_t addr;
                        if (!ResolveGuarded(in.w1, /*hostPtr=*/false, 0, addr,
                                            std::max<size_t>(xferSize, 1u))) {
                            continue;  // raced reload: skip the command this frame
                        }
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
                               wide F3D list may commit a genuine >4GB host base to
                               w1full, so honor w1IsHostPointer before falling back to the
                               narrow-token resolver. */
                            /* Same segment-table WRITE race policy as kOpMoveword:
                               snapshot before resolving and skip the write on a raced
                               reload, retaining the stale base for this frame instead
                               of publishing a torn one. */
                            const uint32_t mwEpoch =
                                w1IsHostPointer ? 0u : GdxSegmentEpochSnapshot();
                            uintptr_t translated;
                            if (w1IsHostPointer) {
                                translated = w1full;
                            } else {
                                translated = TranslateDataPointer(in.w1);
                                if (translated == 0 && in.w1 == 0 && gdx_rdram != nullptr) {
                                    translated = reinterpret_cast<uintptr_t>(gdx_rdram);
                                }
                            }
                            if (!w1IsHostPointer && !GdxSegmentEpochStable(mwEpoch)) {
                                NoteEpochSkip();
                                outW1 = (segIdx < kGfxSegmentCount)
                                            ? gSegments[segIdx]
                                            : static_cast<uintptr_t>(in.w1);
                                break;
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
                        if (!ResolveGuarded(in.w1, /*hostPtr=*/false, 0, outW1, vtxRequiredBytes,
                                            /*preferPhysical=*/!isF3DSource,
                                            reinterpret_cast<uintptr_t>(item.source))) {
                            /* Raced reload: substitute the fallback vertices -- never
                               skip a vertex load silently (later triangles index the
                               buffer). Neutralizes the torn pointer before the copy. */
                            outW1 = reinterpret_cast<uintptr_t>(kFallbackVertices);
                        }
                        // Always on: see the [vtx-spike]/[vtx-dropped] note
                        // in the F3DEX2 G_VTX case above -- this legacy F3D path is the
                        // one identified as the source of machine-part
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

            /* OTR-filepath strlen backstop (Create Machine entry, thread_id=3 AV
               in strlen). The LUS OTR-filepath handlers (interpreter.cpp
               gfx_set_timg_otr_filepath_handler_custom et al.) treat w1 as a
               `const char*` and hand it to LoadResourceProcess -> strlen. LUS
               already rejects w1 < 0x10000 and > 0x0000FFFFFFFFFFFF, but a BARE
               32-bit token zero-extended into the mid range (e.g. 0x25820F60 --
               note the 0x25 top byte is itself the SETTIMG-OTR opcode of a torn
               command word) sails through that filter straight into strlen.
               Such a value reaches here two ways: (a) a segment buffer read torn
               mid-reload whose opcode byte happens to land on an OTR-filepath
               opcode, routed through `default:` with outW1 = the raw low32 token;
               (b) any future emit path that sets an OTR opcode without a real host
               string pointer. A legitimate O2R/pack emit always stores a heap or
               module string pointer, which on this 64-bit target has non-zero
               high bits and is readable -- so this guard is byte-identical for
               every real filepath command and only drops the poisoned ones. */
            {
                const uint8_t outOp = static_cast<uint8_t>((outW0 >> 24) & 0xFFu);
                /* Literal opcode bytes (fast/lus_gbi.h): 0x24 OTR_G_VTX_OTR_FILEPATH,
                   0x25 OTR_G_SETTIMG_OTR_FILEPATH, 0x27 OTR_G_DL_OTR_FILEPATH,
                   0x28 OTR_G_PUSHCD, 0x29 OTR_G_MTX_OTR_FILEPATH. Byte values are part
                   of the LUS OTR command format and stable across header revisions. */
                const bool outIsOtrFilepath = (outOp == 0x24u) || (outOp == 0x25u) ||
                                              (outOp == 0x27u) || (outOp == 0x28u) ||
                                              (outOp == 0x29u);
                if (outIsOtrFilepath &&
                    ((static_cast<uint64_t>(outW1) >> 32) == 0 || !IsReadableAddress(outW1))) {
                    if (mStats != nullptr) mStats->skippedDataCommands++;
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

extern "C" void gdx_register_host_wide_command_range(void* ptr, size_t size) {
    if ((ptr == nullptr) || (size == 0)) return;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(ptr);
    const auto duplicate = std::find_if(
        gHostWideCommandRanges.begin(), gHostWideCommandRanges.end(),
        [begin, size](const HostRange& range) {
            return range.begin == begin && range.size == size;
        });
    if (duplicate == gHostWideCommandRanges.end()) {
        gHostWideCommandRanges.push_back({ begin, size });
    }
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

// E3/A1/A2: registers a known-good compiled-in host array's own address/size so
// ResolveHostPointerStub (above, inside the anonymous namespace) can recognize
// wide SETTIMG pointers that carry the array's address directly instead of
// miscounting them as unbound stubs. Called once per entry from
// gdx_ek_assets_fill()'s fill loop (port/gen/EkAssetBindings.c, EXPANSION_KIT
// only) and once at RDRAM init from port/decomp_port.c for base-game arrays
// (gdx_rdram_init: fireworks sprites, sCourseMinimapPalette) -- not EK-gated,
// since the latter callers exist in every build.
extern "C" void gdx_register_host_pointer_stub(void* dest, size_t size) {
    if ((dest == nullptr) || (size == 0)) return;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(dest);
    const auto duplicate = std::find_if(
        gHostPointerStubs.begin(), gHostPointerStubs.end(),
        [begin, size](const HostRange& range) {
            return range.begin == begin && range.size == size;
        });
    if (duplicate == gHostPointerStubs.end()) {
        gHostPointerStubs.push_back({ begin, size });
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
        /* A newly registered exact token can change pointer widening for an
           already-seen binary list. Force the G2 cache to rebuild on next use. */
        ++gConvertEpoch;
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
        // [seed] quad content probe: high-frequency diagnostic, silent unless GDX_DIAG_VERBOSE=1.
        if (gdx_diag_verbose() && sSeedContentLogs < 10) {
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
    // (a prior one-shot capture was solid black while the source FB held the logo).
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
// Bounded, env-gated per-frame capture facility.
// GDX_CAPTURE_FRAMES=<startFrame>:<count> dumps <count> consecutive presented
// frames (numbered from process start, counted at every GdxUpdateFrameMirror
// call, i.e. once per presented host frame regardless of real-task vs VI-fallback
// path) to gdxcap_NNNNN.bmp in the working directory. Zero cost when the env var
// is unset (single cached bool check). Kept in-tree as a debugging tool.
static void GdxCaptureFrameIfRequested(const std::shared_ptr<Fast::Interpreter>& interp) {
    static int sCapState = -1;   // -1 = unparsed, 0 = disabled, 1 = enabled
    static int sCapStart = 0;    // frames (or matching-mode frames) to skip
    static int sCapCount = 0;    // frames to dump
    static int sCapMode = -1;    // -1 = any mode; else GET_MODE gate (0x1F mask)
    static long sCapFrame = 0;   // global presented-frame counter (for filenames)
    static int sCapSkipped = 0;  // mode-matching frames skipped so far
    static int sCapDumped = 0;   // frames dumped so far
    if (sCapState == -1) {
        sCapState = 0; // parse exactly once
        const char* env = std::getenv("GDX_CAPTURE_FRAMES");
        if (env != nullptr && *env != '\0') {
            int start = 0, count = 0;
            if (std::sscanf(env, "%d:%d", &start, &count) == 2 && count > 0) {
                sCapStart = start;
                sCapCount = count;
                sCapState = 1;
                const char* menv = std::getenv("GDX_CAPTURE_MODE");
                if (menv != nullptr && *menv != '\0') {
                    sCapMode = static_cast<int>(std::strtol(menv, nullptr, 0)) & 0x1F;
                }
                gdx_port_logf("[gdxcap] enabled: start=%d count=%d modeGate=%d\n", start, count, sCapMode);
            }
        }
    }
    const long frame = sCapFrame++;
    if (sCapState != 1 || sCapDumped >= sCapCount) {
        return;
    }
    if (sCapMode >= 0) {
        // Mode-gated: only count/dump frames while GET_MODE == modeGate.
        if ((gGameMode & 0x1F) != sCapMode) {
            return;
        }
        if (sCapSkipped < sCapStart) {
            ++sCapSkipped;
            return;
        }
    } else {
        // Frame-number-gated: dump the window [start, start+count).
        if (frame < sCapStart) {
            return;
        }
    }
    if (gFrameMirrorFb < 0) {
        return;
    }
    static uint16_t sCapPixels[320 * 240];
    interp->GetCurrentRenderingAPI()->ReadFramebufferToCPU(gFrameMirrorFb, 320, 240, sCapPixels);
    char name[64];
    std::snprintf(name, sizeof(name), "gdxcap_%05ld.bmp", frame);
    DumpRgba16Bmp(name, sCapPixels, 320, 240);
    ++sCapDumped;
    gdx_port_logf("[gdxcap] dumped frame %ld -> %s (gameMode=0x%X)\n", frame, name, (gGameMode & 0x1F));
}

// GDX_INPUT_SCRIPT (dev-only) SHOT hook: one-shot named framebuffer dump requested by
// gdx_input_script.c. Arms a label; the next GdxUpdateFrameMirror call (i.e. the next presented
// frame) reuses the exact same read-back + BMP encode path as GdxCaptureFrameIfRequested above
// (ReadFramebufferToCPU + DumpRgba16Bmp) to write "autotest/<label>.bmp". A plain global instead
// of a queue: SHOT is a single-script, one-in-flight-at-a-time dev feature, and
// gdx_input_script_override() only ever issues the next SHOT after the current poll's pad state
// has been consumed, so two requests can never race.
static bool gPendingNamedDumpArmed = false;
static char gPendingNamedDumpLabel[128];

extern "C" void gdx_request_frame_dump(const char* label) {
    if (label == nullptr || label[0] == '\0') {
        return;
    }
    std::snprintf(gPendingNamedDumpLabel, sizeof(gPendingNamedDumpLabel), "%s", label);
    gPendingNamedDumpArmed = true;
}

static void GdxDumpNamedFrameIfRequested(const std::shared_ptr<Fast::Interpreter>& interp) {
    if (!gPendingNamedDumpArmed || gFrameMirrorFb < 0) {
        return;
    }
    gPendingNamedDumpArmed = false;

#ifdef _WIN32
    CreateDirectoryA("autotest", nullptr); // ERROR_ALREADY_EXISTS is fine (see disk_savefile.cpp)
#endif

    static uint16_t sNamedDumpPixels[320 * 240];
    interp->GetCurrentRenderingAPI()->ReadFramebufferToCPU(gFrameMirrorFb, 320, 240, sNamedDumpPixels);
    char path[192];
    std::snprintf(path, sizeof(path), "autotest/%s.bmp", gPendingNamedDumpLabel);
    DumpRgba16Bmp(path, sNamedDumpPixels, 320, 240);
    gdx_port_logf("[autotest] SHOT dumped -> %s\n", path);
}

/* [interp-shot] Capture one sub-frame's rendered image. Called from Fast3dWindow (libultraship)
   immediately after Interpreter::Run and BEFORE gui->EndDraw / EndFrame -- the only point where the
   image exists and nothing has been blitted or presented. Capturing after the present compares
   FLIP_DISCARD back buffers whose contents are undefined, which is how a previous attempt produced
   a meaningless "44% of pixels differ" between two passes fed identical matrices.

   Source depends on where the game actually drew: mRendersToFb selects an intermediate game
   framebuffer (widescreen pillarbox, MSAA, or a viewport/resolution mismatch), otherwise the draw
   went straight to framebuffer 0. Reading the wrong one yields a stale or blank image. */
extern "C" void gdx_gfx_post_run_capture(void) {
    if (gGdxShotArmedPass < 0 || gFrameMirrorFb < 0) {
        return;
    }
    const int pass = gGdxShotArmedPass;
    gGdxShotArmedPass = -1; // one shot per arming, regardless of what happens below

    auto wnd = Ship::Context::GetInstance() != nullptr ? Ship::Context::GetInstance()->GetWindow() : nullptr;
    if (wnd == nullptr) {
        return;
    }
    auto* fw = dynamic_cast<Fast::Fast3dWindow*>(wnd.get());
    if (fw == nullptr) {
        return;
    }
    auto interp = fw->GetInterpreterWeak().lock();
    if (interp == nullptr) {
        return;
    }
    const int src = interp->mRendersToFb ? interp->mGameFb : 0;
    interp->CopyFrameBuffer(gFrameMirrorFb, src, false, nullptr);
    static uint16_t sShotPixels[320 * 240];
    interp->GetCurrentRenderingAPI()->ReadFramebufferToCPU(gFrameMirrorFb, 320, 240, sShotPixels);
#ifdef _WIN32
    CreateDirectoryA("autotest", nullptr);
#endif
    char shotPath[192];
    std::snprintf(shotPath, sizeof(shotPath), "autotest/interp_pass%d.bmp", pass);
    DumpRgba16Bmp(shotPath, sShotPixels, 320, 240);
    gdx_port_logf("[interp-shot] pass=%d src=%s(%d) -> %s\n", pass,
                  interp->mRendersToFb ? "gameFb" : "fb0", src, shotPath);
}

static void GdxUpdateFrameMirror(const std::shared_ptr<Fast::Interpreter>& interp) {
    if (gFrameMirrorFb < 0) {
        gFrameMirrorFb = interp->CreateFrameBuffer(320, 240, 320, 240, 1, false);
    }
    if (gFrameMirrorFb >= 0) {
        interp->CopyFrameBuffer(gFrameMirrorFb, 0, false, nullptr);
        gFrameMirrorValid = true;
    }
    GdxCaptureFrameIfRequested(interp);
    GdxDumpNamedFrameIfRequested(interp);
}

/* PORT boot-logo seed (framebuffer coherence).
 *
 * Registered as the interpreter's after-clear hook (Interpreter::SetPortAfterClearHook)
 * ONLY when GDX_SEED_BOOT_LOGO is enabled (see gdx_gfx_run). It runs on the
 * freshly-cleared canvas, BEFORE the frame's task commands, so the CPU-written VI
 * framebuffer (the boot logo blitted by func_806F33D0 / func_80069F5C, which no RDP
 * task renders) shows as a background UNDER the task's overlay content instead of a
 * black canvas. Gated to the boot/title phase (GAMEMODE_TITLE) so it can never
 * affect gameplay/menus. This complements the already-present GPU->CPU readback for
 * transitions (gdx_read_current_framebuffer). Left opt-in because it cannot be
 * runtime-validated without a launch. */
/* The hook is registered UNCONDITIONALLY (which removes any registration-order /
   env-timing question); the opt-in gate lives here, per call, on a cached env check.
   Set by gdx_gfx_run's first-frame env probe. */
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

/* Present-path telemetry (Course-Edit whole-window + ImGui strobe).
 *
 * Env-gated on GDX_PRESENT_PATH_TRACE. In the default (single-present) frame
 * loop gdx_vi_present_fallback runs once per host frame and can classify which
 * path actually produced the present: a real gfx task (task-render), the
 * taskless full-res hold re-composite (hold-recomposite), or a taskless
 * VI-scanout draw (vifb-*). Identical consecutive frames are coalesced into
 * run-length lines so a 60 fps soak yields a compact alternation trace instead
 * of 60 lines/second. A pure strobe (task-render x1 / hold-recomposite x1 /
 * task-render x1 / ...) is the exact signature of the defect, and it is measurable
 * at boot/title wherever rendersToFb is taskless, without entering the editor.
 * Zero cost unless the env var is set.
 *
 * All callers pass a string LITERAL, so the pointer identity comparison below
 * is a valid "same path" test within this translation unit. */
static void GdxPresentPathTrace(const char* path) {
    if (!gdx_dev_gate(GDX_GATE_PRESENT_PATH_TRACE)) {
        return;
    }
    static const char* sLastPath = nullptr;
    static unsigned sRunLen = 0;
    static unsigned long long sTotalLines = 0;
    if (path == sLastPath) {
        ++sRunLen;
        return;
    }
    if (sLastPath != nullptr && sTotalLines < 4000) {
        ++sTotalLines;
        gdx_port_logf("[present-path] %s x%u\n", sLastPath, sRunLen);
    }
    sLastPath = path;
    sRunLen = 1;
}

/* Hold-recomposite readback probe (40fps-on-menus regression, interp ON).
 *
 * Env-gated on GDX_DIAG_HOLD=1. Confirms in one soak that the taskless hold path
 * (gdx_vi_present_fallback's holdGpuFrame branch below) no longer pays a
 * synchronous CPU readback: it now blits the persistent frame mirror straight
 * into the current draw target on the GPU (Interpreter::CopyFrameBuffer, the
 * same primitive GdxUpdateFrameMirror already uses in the other direction),
 * instead of ReadFramebufferToCPU (D3D11 Map + a full-frame CPU box-filter
 * downscale) into a CPU buffer that SeedFramebufferQuad then re-uploads as a
 * texture. `contentChangedSincePrevHold` is true when a real GFX task rendered
 * since the previous hold tick (i.e. the OLD code would have paid a readback
 * here) -- logged so the previously-every-other-frame cadence is visible
 * alongside proof that every single hold tick, changed or not, is now GPU-only. */
static void GdxDiagHoldTick(bool contentChangedSincePrevHold) {
    if (!gdx_dev_gate(GDX_GATE_DIAG_HOLD)) {
        return;
    }
    static unsigned long long sHoldTicks = 0;
    static unsigned long long sChangedTicks = 0;
    ++sHoldTicks;
    if (contentChangedSincePrevHold) {
        ++sChangedTicks;
    }
    if (sHoldTicks <= 16 || (sHoldTicks % 60) == 0) {
        gdx_port_logf("[hold-diag] tick=%llu changedSincePrevHold=%d(total=%llu) mode=gpu-copy readback=0\n",
                      sHoldTicks, contentChangedSincePrevHold ? 1 : 0, sChangedTicks);
    }
}

/* Present-target invariant: leave framebuffer 0 bound on EVERY exit from
 * gdx_vi_present_fallback.
 *
 * The host composites ImGui immediately after that function returns
 * (main.cpp:1273 -> 1274 on the default single-present path, 1307 -> 1308 on the
 * interpolation path), and neither ImGui backend selects a render target of its
 * own for the main viewport: imgui_impl_opengl3.cpp issues no glBindFramebuffer
 * anywhere, and imgui_impl_dx11.cpp's only OMSetRenderTargets lives in
 * ImGui_ImplDX11_RenderWindow, which serves secondary platform viewports. The
 * game image and the whole enhancement menu therefore land on whatever target
 * happens to be bound — the composite has always INHERITED its render target
 * rather than asserted one.
 *
 * Why that inheritance is fragile here and not upstream: this port's frame loop
 * is inverted. The entire game frame, interp->Run() included, executes inside
 * gdx_vi_tick() at main.cpp:1232 — posting the VI retrace message dispatches the
 * game fiber synchronously, so the gfx task is submitted and run right there —
 * which means Run() finishes BEFORE the host opens the frame with
 * gui->StartDraw() / w->StartFrame() at main.cpp:1252-1253. Upstream, Run() is
 * the last thing to touch the rendering API before the ImGui composite. Here
 * Interpreter::StartFrame (interpreter.cpp:7243) runs after it, and whenever
 * mRendersToFb is true it unconditionally re-runs
 * UpdateFramebufferParameters(mGameFb, ...) (interpreter.cpp:7288-7298).
 *
 * That is exactly where the two backends diverge.
 * GfxRenderingAPIDX11::UpdateFramebufferParameters never touches the output
 * merger, so the framebuffer-0 binding Run() left behind (interpreter.cpp:7482
 * on the mRendersToFb path; on the !mRendersToFb path its prologue at
 * interpreter.cpp:7431 targeted fb 0 directly) survives StartFrame and the
 * inherited composite happens to be correct.
 * GfxRenderingAPIOGL::UpdateFramebufferParameters glBindFramebuffer()s the
 * framebuffer it is about to reconfigure and never restores the previous one,
 * so on GL that same StartFrame leaves mGameFb's FBO bound. The Expansion Kit
 * editors force mRendersToFb (whole-frame fixed-aspect pillarbox), so the game
 * image and the entire menu were composited into the offscreen texture while the
 * window presented nothing but the bare black clear from Run()'s prologue:
 * Create Machine invisible, Course Edit strobing, both correct on D3D11.
 *
 * So state the invariant instead of assuming it. This is a re-bind, not a frame
 * setup, and the two omissions are load-bearing:
 *  - noiseScale is 0.0f deliberately. Both backends skip the dither-noise
 *    uniform update when it is zero (the `if (noise_scale != 0.0f)` guard in
 *    GfxRenderingAPIOGL::StartDrawToFramebuffer and in
 *    GfxRenderingAPIDX11::StartDrawToFramebuffer), so whatever a real frame
 *    latched is preserved rather than overwritten with 1/0.
 *  - It does NOT clear. On a !mRendersToFb frame Run() rendered the game
 *    straight into framebuffer 0, so a clear here would erase the very frame
 *    about to be presented.
 * On D3D11 the call is consequently inert: it rebinds an already-bound RTV,
 * assigns mRenderTargetHeight the value it already holds, skips the noise
 * update, and re-uploads a byte-identical PerFrameCB. D3D11 behaviour is
 * unchanged.
 *
 * The hold re-composite and VI-scanout branches below select their own draw
 * targets after this runs and are unaffected. The exits this exists for are the
 * task-render fast path (by far the common one), the no-VI-framebuffer return,
 * and every acquisition failure in between.
 *
 * The rendering API pointer is cached the same way gdx_gfx_run caches its window
 * pointer, for the same reason: Interpreter::Init assigns mRapi once for the
 * process (interpreter.cpp:7156), while re-reading the Context's window
 * shared_ptr every frame is the per-frame refcount touch that crashed in
 * _Ptr_base<Window>::_Incref during rapid mode transitions. Any step that yields
 * null leaves the cache empty and returns; the next frame retries.
 */
static void GdxBindWindowFramebuffer() {
    static Fast::GfxRenderingAPI* sPresentRapi = nullptr;
    if (sPresentRapi == nullptr) {
        auto ctx = Ship::Context::GetInstance();
        if (ctx == nullptr) {
            return;
        }
        auto wnd = ctx->GetWindow();
        Fast::Fast3dWindow* fw = static_cast<Fast::Fast3dWindow*>(wnd.get());
        if (fw == nullptr) {
            return;
        }
        auto interp = fw->GetInterpreterWeak().lock();
        if (!interp) {
            return;
        }
        sPresentRapi = interp->GetCurrentRenderingAPI();
        if (sPresentRapi == nullptr) {
            return;
        }
    }
    // noiseScale 0 = "leave the noise uniform alone"; no clear, see above.
    sPresentRapi->StartDrawToFramebuffer(0, 0.0f);
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
    // Framebuffer 0 is the draw target the host's ImGui composite (main.cpp:1274)
    // requires, and every exit below must satisfy it -- including the ones that
    // return before any of this function's own work happens. See
    // GdxBindWindowFramebuffer above for the full chain.
    GdxBindWindowFramebuffer();

    // A real GFX task already produced this host frame: nothing to do. Clear the
    // flag for the next frame. This is the hot path once gameplay is rendering.
    if (gHostFrameGfxTaskRan) {
        gHostFrameGfxTaskRan = false;
        GdxPresentPathTrace("task-render");
        return;
    }

    const uintptr_t fbAddr = gViCurrentFramebuffer;
    if (fbAddr == 0) {
        // Traced: this was the one exit with no telemetry at all, which made it
        // invisible in a present-path soak (every other exit reports a path).
        GdxPresentPathTrace("no-vi-fb");
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

    // Full-resolution hold path: when the game renders to an offscreen framebuffer
    // (menus, pillarboxed modes such as the Expansion Kit editors) and this tick submits
    // NO gfx task, the last frame's texture is STILL in mGameFb and interp->mGfxFrameBuffer
    // still points at it (Interpreter::StartFrame does not reset it), so Fast3dGui::DrawGame
    // can re-composite the retained frame.
    //
    // ROOT CAUSE (Course-Edit whole-window + ImGui strobe): the previous
    // implementation early-returned here doing NOTHING, on the assumption that "a taskless
    // present needs no work — returning re-presents the previous frame". That assumption is
    // false in this port's split frame loop. main.cpp unconditionally runs, AFTER this
    // function returns, gui->EndDraw() (which builds a FRESH ImGui frame — the held game
    // image via DrawGame plus the enhancement menu — and renders it) and interp->EndFrame()
    // (SwapBuffers). That composite targets the window backbuffer (framebuffer 0). On the
    // DXGI flip-model swap chain the backbuffer must be re-acquired, re-bound and cleared
    // every presented frame — exactly what Interpreter::Run()/RunGuiOnly() do in their
    // prologue (UpdateFramebufferParameters(0) -> StartFrame -> StartDrawToFramebuffer(0) ->
    // ClearFramebuffer). The early-return skipped ALL of it, so a hold frame's ImGui composite
    // landed on an unprepared/stale backbuffer. Task frames prepared the backbuffer; hold
    // frames did not, so as the two frame kinds alternated the ENTIRE window — enhancement
    // menu included — strobed. ImGui is composited by the host into the same backbuffer as the
    // game (Fast3dGui::DrawGame draws the game FB as an ImGui image inside the "Main Game"
    // window), which is why the menu flickered in lockstep with the game content. The bug is
    // invisible outside pillarboxed taskless modes because a
    // non-mRendersToFb taskless frame falls through to the VI-scanout path below, which already
    // runs the full fb-0 prologue.
    //
    // Fix: run the same fb-0 prologue a real frame runs, but do NOT touch mGameFb (it holds the
    // frame we are re-presenting) or mGfxFrameBuffer (it still references that frame's texture).
    // gui->EndDraw() then composites the retained frame into a freshly prepared, cleared
    // backbuffer — pixel-identical pipeline to a task frame, so the strobe disappears.
    if (sGpuContentLive && interp->mRendersToFb && interp->mGfxFrameBuffer != 0) {
        interp->SpReset();
        rapi->UpdateFramebufferParameters(0, interp->mGfxCurrentWindowDimensions.width,
                                          interp->mGfxCurrentWindowDimensions.height, 1, false, true, true,
                                          !interp->mRendersToFb);
        rapi->StartFrame();
        // noiseScale=1, not mCurDimensions.height/mNativeDimensions.height: that
        // ratio (used by Interpreter::StartFrame's callers when the draw target is
        // mGameFb) feeds N64->internal coordinate scaling for the internal-resolution
        // game surface -- StartDrawToFramebuffer's second parameter is actually the
        // dither-noise uniform (noise_scale in the backends), unrelated to that
        // ratio. This call's target is unconditionally framebuffer 0 (the window
        // backbuffer, already at window resolution) -- exactly the same shape as
        // Interpreter::Run()/RunGuiOnly()'s OWN unconditional-fb-0 calls (their
        // MSAA-resolve epilogue and failsafe-reset paths), which also pass a literal
        // 1. Verified against interpreter.cpp's StartFrame/Run/RunGuiOnly.
        rapi->StartDrawToFramebuffer(0, 1);
        rapi->ClearFramebuffer(true, true);
        // mGfxFrameBuffer intentionally left unchanged: mGameFb still holds last frame (no gfx
        // task cleared it this tick), so DrawGame re-composites it. The host loop's EndFrame
        // presents. Mirror update is deliberately skipped (as before): the held content is
        // already the mirror, so re-capturing it would only add a needless blit.
        GdxPresentPathTrace("hold-recomposite");
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

    // Source selection. Once GPU content is live, a taskless present re-presents
    // the last GPU frame — the CPU VI framebuffer holds no pixels for
    // GPU-rendered screens and blitting it flashed the whole screen black.
    // Before the first GFX task (boot logo and other genuinely CPU-drawn
    // phases) the VI framebuffer is the real image source, exactly as before.
    //
    // 40fps-on-menus fix (interp ON): 2D menus alternate task/no-task host
    // ticks close to 1:1, so a real GFX task refreshes the persistent frame
    // mirror on almost every OTHER tick — sGpuHoldPixelsStale (set at the tail
    // of every task tick, see gdx_gfx_run) therefore also flips true almost
    // every other tick. That is legitimate staleness (the mirror really did
    // just get new content), so no amount of smarter invalidation shrinks the
    // number of hold ticks that need fresh pixels — the ONLY way to fix a
    // near-1:1 alternation is to make refreshing a hold tick cheap. The old
    // code paid for that refresh with rapi->ReadFramebufferToCPU: a D3D11
    // Map(D3D11_MAP_READ) that blocks on the GPU plus an O(w*h) CPU box-filter
    // downscale (gfx_direct3d11.cpp, added for Issue C's fade dash-band fix) —
    // a synchronous stall on the hot path, which is what halved menu fps.
    //
    // Fix: composite the mirror with a GPU->GPU copy instead of a CPU
    // roundtrip. gFrameMirrorFb is a real entry in the interpreter's
    // mFrameBuffers map (created via CreateFrameBuffer(..., resize=true)), so
    // Interpreter::StartFrame — which the host's per-frame w->StartFrame() call
    // always runs, hold tick or not — keeps it resized in lockstep with every
    // other framebuffer (mGameFb, fb 0) whenever the window/native dimensions
    // change (interpreter.cpp:StartFrame, the mFrameBuffers resize loop). That
    // means gFrameMirrorFb and the current draw target are always the same
    // pixel size, so Interpreter::CopyFrameBuffer's DX11 backend takes its
    // fast same-size CopyResource path (no scaling shader, no readback) — the
    // exact primitive GdxUpdateFrameMirror already uses in the other direction
    // every task tick. This also drops the previous round-trip's quality loss
    // (320x240 box-filter downsample immediately followed by a 1:1-texel
    // upload and a nearest-filter re-stretch back to full resolution).
    //
    // Invariants preserved: a hold tick still requires sGpuContentLive (so it
    // can never fire before the first real task has rendered — the "1 of every
    // 3 presents flashed black" fix above is untouched) and the vifb-scanout
    // (non-GPU-content-live) path below is byte-for-byte unchanged. Branch A
    // (the mRendersToFb re-composite above) is untouched.
    const bool holdGpuFrame = sGpuContentLive && gFrameMirrorValid && gFrameMirrorFb >= 0;
    GdxPresentPathTrace(holdGpuFrame ? "vifb-held-gpu-mirror" : "vifb-vi-scanout");
    if (holdGpuFrame) {
        // Diag only: true iff a real task refreshed the mirror since the previous
        // hold tick — i.e. the case that used to force a CPU readback here. The
        // GPU copy below runs unconditionally either way; sGpuHoldPixelsStale no
        // longer gates anything, it is read-and-cleared purely for this probe.
        const bool contentChangedSincePrevHold = sGpuHoldPixelsStale;
        sGpuHoldPixelsStale = false;
        interp->CopyFrameBuffer(interp->mRendersToFb ? interp->mGameFb : 0, gFrameMirrorFb, false, nullptr);
        GdxDiagHoldTick(contentChangedSincePrevHold);
    } else {
        // Convert + upload the VI framebuffer's CPU-written RGBA5551 pixels and draw
        // them as one fullscreen copy-mode rectangle. The pointer tracked at
        // osViSwapBuffer time is a real host pointer, read directly as u16.
        SeedFramebufferQuad(interp.get(), reinterpret_cast<const uint16_t*>(fbAddr));
    }

    // --- Frame epilogue: same as Run(). When rendering to an offscreen game FB
    //     (resolution multiplier / MSAA), publish it for the GUI compositor. ---
    interp->mGfxFrameBuffer = 0;
    if (interp->mRendersToFb) {
        // Mirror Interpreter::Run()'s MSAA-resolve decision. This taskless path runs only when NO
        // gfx task executed this frame, so Interpreter::Run() (and its mid-frame fixed-aspect
        // re-latch) did NOT run; the caches here are exactly the ones interp->StartFrame() latched
        // and sized mGameFbMsaaResolved from. That makes the resolved target guaranteed-allocated
        // whenever this branch selects it (no divergence to guard against, unlike Run()'s epilogue).
        const bool widescreenPillarbox = !interp->mWidescreenEnabledCache || interp->mForceFixedAspectCache;
        // noiseScale=1 is correct here, not the mCurDimensions/mNativeDimensions ratio:
        // that ratio is only meaningful for a draw target that can be mGameFb (it feeds
        // N64->internal coordinate scaling elsewhere), while StartDrawToFramebuffer's
        // second parameter is the dither-noise uniform (noise_scale in the backends).
        // This target is unconditionally framebuffer 0, matching interpreter.cpp's OWN
        // MSAA-resolve epilogue (Run()/RunGuiOnly(), same "if (mRendersToFb) {
        // StartDrawToFramebuffer(0, 1); ..." shape this block mirrors).
        rapi->StartDrawToFramebuffer(0, 1);
        rapi->ClearFramebuffer(true, true);
        if (interp->mMsaaLevel > 1) {
            if (interp->ViewportMatchesRendererResolution() && !widescreenPillarbox) {
                // Normal path: resolve straight to the window; mGfxFrameBuffer stays 0.
                rapi->ResolveMSAAColorBuffer(0, interp->mGameFb);
            } else {
                // Viewport differs OR a whole-frame pillarbox is required: resolve into the offscreen
                // target and publish it so Fast3dGui::DrawGame composites the centred 4:3 sub-region.
                // This is what keeps the fallback from leaving mGfxFrameBuffer == 0 when a pillarbox
                // is required (which would stretch 4:3 content across the whole window).
                rapi->ResolveMSAAColorBuffer(interp->mGameFbMsaaResolved, interp->mGameFb);
                interp->mGfxFrameBuffer =
                    reinterpret_cast<uintptr_t>(rapi->GetFramebufferTextureId(interp->mGameFbMsaaResolved));
            }
        } else {
            interp->mGfxFrameBuffer = reinterpret_cast<uintptr_t>(rapi->GetFramebufferTextureId(interp->mGameFb));
        }
    }
    // The host loop's w->EndFrame() presents this frame — do NOT EndFrame here.

    static int sFallbackLogs = 0;
    if (sFallbackLogs < 8) {
        ++sFallbackLogs;
        gdx_port_logf("[vifallback] presented %s fb=%p (%ux%u, rendersToFb=%d)\n",
                      holdGpuFrame ? "held GPU frame (mirror)" : "VI framebuffer",
                      reinterpret_cast<void*>(fbAddr), 320u, 240u, static_cast<int>(interp->mRendersToFb));
    }

    // Transition snapshot mirror: identical to the update done at the tail of
    // gdx_gfx_run. Boot-phase frames are often presented entirely through this
    // VI-scanout fallback (no GFX task runs), so without this the mirror stays
    // stale/empty until the first real task, and any transition snapshot taken
    // during/after boot reads garbage. Skipped on hold frames: the composed
    // content IS the mirror, and re-capturing it would only add a needless blit.
    if (!holdGpuFrame) {
        GdxUpdateFrameMirror(interp);
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

extern "C" void gdx_defer_native_rgba16_texture_range_clear(void* ptr) {
    const uintptr_t begin = reinterpret_cast<uintptr_t>(ptr);
    if (begin != 0) {
        gPendingNativeRgba16RangeClears.push_back(begin);
    }
}

/* Minimap staleness fix: sCourseMinimapTex (minimap.c) is a CI8 texture that is
 * Arena_Allocate'd per race under EXPANSION_KIT. Fast3D's texture cache keys CI8
 * by address with no content hash, and the deterministic arena rewind hands the
 * same address to the next race, so a cache HIT serves the previous race's decoded
 * outline. Evicting the exact buffer address after it has been re-rasterized forces
 * the next upload to re-decode the current course. Scoped to this one producer on
 * purpose -- a general CI content-hash caused a race regression (see the CI-hash
 * note in libultraship interpreter.cpp). Mirrors the TextureCacheDelete usage in
 * SeedFramebufferQuad and is safe before the interpreter is up (early-out on null),
 * so callers can invoke it unconditionally. */
extern "C" void gdx_invalidate_texture_address(const void* addr) {
    if (addr == nullptr) {
        return;
    }
    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return;
    }
    auto wnd = ctx->GetWindow();
    auto* fw = static_cast<Fast::Fast3dWindow*>(wnd.get());
    if (fw == nullptr) {
        return;
    }
    auto interp = fw->GetInterpreterWeak().lock();
    if (!interp) {
        return;
    }
    interp->TextureCacheDelete(reinterpret_cast<const uint8_t*>(addr));
}

/* Transition-capture probe hook (see gDiagTransitionCaptureBegin). Called by
 * Transition_SetBackgroundBuffer immediately after it registers the captured
 * background buffer as a native-RGBA16 range, so the SETTIMG probe can report
 * whether that same buffer is byteswapped when the wipe/phased-strips draw reads
 * it. Pure diagnostic bookkeeping: it records an address span, never alters
 * rendering. Pass size 0 to clear the scope. */
extern "C" void gdx_diag_note_transition_capture(void* ptr, size_t size) {
    gDiagTransitionCaptureBegin = reinterpret_cast<uintptr_t>(ptr);
    gDiagTransitionCaptureSize = (ptr != nullptr) ? size : 0;
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

// Venue texture-bank symbols, indexed by venue id. File scope because two callers must agree on
// this list exactly: the runtime binder below, and gdx_boot_warm_asset_segments, which decodes the
// same banks at boot so the runtime call is a cache hit. A second copy of this table would be a
// place for them to silently diverge.
static const void* const kGdxVenueSegmentSymbols[] = {
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

// Decode the runtime-expensive asset segments at BOOT, so first use during play is a cache hit.
//
// WHY: the asset stalls were attributed by measurement, killing four theories in sequence.
// Preloading the COMPRESSED blobs changed nothing (11/11 warmed in 1.6ms; Cup Select loads
// unmoved). Fixups and command-range registration measured 0.00-0.01ms. The ++gConvertEpoch
// cache invalidation measured flat (xlate 0.12-0.16ms for 8 ticks after each load). And the MIO0
// decode itself -- the last suspect standing, and the one this function was first written to
// avoid -- turned out to cost 2.2ms for ALL TWELVE segments when run here on the host thread.
// The runtime figures (133.95ms for course_track_gfx in one hit, 6-26ms per venue bank at first
// Cup Select entry, each overrunning the 16.68ms tick that carried it) were dominated by
// gdx_yield ROUND-TRIPS: the load window measured yields=2..9, and per-yield cost varies with
// where in the host frame the yield lands (seg 8: 9 yields ~= 134ms). Which call inside
// EnsureAssetSegmentImage yields on the game fiber remains unattributed -- moving the work here
// makes the question moot, since the host context cannot yield at all.
// Decoded images live in gLoadedAssetSegments, which NEVER evicts, so warming here removes those
// stalls for the whole session.
//
// WHY THE SNAPSHOT/RESTORE: EnsureAssetSegmentImage claims gSegments[seg] when the slot is still 0.
// Left alone, this loop would boot the game with segment 0x0A bound to whichever venue decoded
// last -- a binding the game never asked for. Restoring the snapshot returns every slot to its
// pre-warm state; the claim is re-applied harmlessly by the first runtime cache hit (the hit path
// has the same ==0 claim), and the venue binder above re-asserts 0x0A unconditionally anyway.
// The gConvertEpoch bumps this loop causes are left alone: nothing is converted yet at boot, so
// there is nothing to invalidate.
//
// CALLED: from main.cpp, after gdx_rdram_init and the blob preloads, BEFORE bootproc -- host
// thread only, no fibers running, so none of the seqlock/threading constraints in this file apply
// yet. gdx_yield inside the load path is a no-op in host context (__osRunningThread == NULL).
extern "C" void gdx_boot_warm_asset_segments(void) {
    const void* const kWarmSymbols[] = {
        D_8000000, // course_track_gfx (seg 8): the 133.95ms single hit
        kGdxVenueSegmentSymbols[0], kGdxVenueSegmentSymbols[1], kGdxVenueSegmentSymbols[2],
        kGdxVenueSegmentSymbols[3], kGdxVenueSegmentSymbols[4], kGdxVenueSegmentSymbols[5],
        kGdxVenueSegmentSymbols[6], kGdxVenueSegmentSymbols[7], kGdxVenueSegmentSymbols[8],
        kGdxVenueSegmentSymbols[9], kGdxVenueSegmentSymbols[10],
    };

    unsigned long long savedSegments[16];
    static_assert(sizeof(savedSegments) == sizeof(gSegments), "gSegments snapshot size");
    std::memcpy(savedSegments, gSegments, sizeof(savedSegments));

    const auto warmT0 = std::chrono::steady_clock::now();
    int warmed = 0;
    for (const void* sym : kWarmSymbols) {
        uint32_t offset = 0;
        if (EnsureAssetSegmentForSymbol(Low32(reinterpret_cast<uintptr_t>(sym)), &offset) != 0) {
            ++warmed;
        }
    }

    std::memcpy(gSegments, savedSegments, sizeof(savedSegments));

    gdx_port_logf("[boot-warm] decoded %d/%zu asset segments in %.1fms (segments table restored)\n",
                  warmed, std::size(kWarmSymbols),
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - warmT0).count());
}

extern "C" int gdx_load_venue_texture_segment(int venue) {
    if (venue < 0 || static_cast<size_t>(venue) >= std::size(kGdxVenueSegmentSymbols)) {
        gdx_port_logf("[segment] invalid venue texture segment %d\n", venue);
        return 0;
    }

    const uint32_t symbol = Low32(reinterpret_cast<uintptr_t>(kGdxVenueSegmentSymbols[venue]));
    uint32_t offset = 0;
    // [venueload] Attribute the Cup Select stall. This path is invisible to the existing
    // [transition] timers, which only bracket mode-change ticks -- and Cup Select's course preview
    // loading is NOT a mode change. It runs one course per game tick from
    // course_model.c:35-39, so the ~350ms the owner sees is really ~6 consecutive ticks.
    //
    // Two candidate causes, and they need opposite fixes, which is why this measures rather than
    // assumes: either the LOAD itself is expensive (archive read + MIO0 decode + fixups), in which
    // case preloading the blobs at boot removes it; or the load is cheap and the cost is the
    // ++gConvertEpoch below invalidating every cached display-list conversion, in which case
    // preloading changes NOTHING and the fix is scoped epoch invalidation. The epoch counter is
    // logged alongside so the following ticks' xlate can be correlated.
    const double gdxVenueT0 = (gGdxInterpNowFn != nullptr) ? gGdxInterpNowFn() : 0.0;
    const uint32_t gdxEpochBefore = gConvertEpoch;
    const uintptr_t base = EnsureAssetSegmentForSymbol(symbol, &offset);
    if (base == 0) {
        gdx_port_logf("[segment] failed to load venue=%d symbol=%08X\n", venue, symbol);
        return 0;
    }
    if (gGdxInterpNowFn != nullptr) {
        gdx_port_logf("[venueload] venue=%d symbol=%08X load=%.2fms epoch %u->%u\n", venue, symbol,
                      (gGdxInterpNowFn() - gdxVenueT0) * 1000.0, (unsigned) gdxEpochBefore,
                      (unsigned) gConvertEpoch);
        gGdxVenueWatchTicks = 8; // log the next 8 ticks' translation cost (see gdx_gfx_run)
    }

    // Force segment 0x0A to point at the (decompressed) venue texture image.
    // EnsureAssetSegmentImage only claims a segment slot when it is still 0, but
    // the game sets segment 0x0A via gsSPSegment before this loads, so the slot
    // was already non-zero and kept a stale/raw pointer — the track then sampled
    // raw ROM/compressed bytes (the "stripes"). This loader is the authority for
    // the venue texture segment, so it still re-asserts unconditionally whenever
    // the value actually changes (e.g. the game's own DL rewrote it via moveword).
    // Bracket the write with the seqlock epoch: an unbracketed store here was
    // invisible to the graphics-thread's GdxSegmentEpochStable() guard, so a
    // racing translate could observe a torn/stale base with no skip counted.
    // gdx_load_venue_texture_segment is game-thread-only (called from
    // decomp_port.c Segment_LoadAssets), which is a requirement for calling
    // gdx_segment_epoch_begin/end -- never call these from the graphics thread.
    if (gSegments[0x0A] != base) {
        gdx_segment_epoch_begin();
        gSegments[0x0A] = base;
        gdx_segment_epoch_end();
    }

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

/* Writable extent of the registered host range containing `host`, or 0 when the pointer is not
   inside any registered range. Exposed to decomp TUs because Dma_RomCopy resolves a 32-bit N64
   pointer to a host address and then writes `size` bytes into it, and nothing in that path
   otherwise knows how large the destination actually is. */
extern "C" size_t gdx_registered_host_capacity(const void* host) {
    return RegisteredHostRemaining(reinterpret_cast<uintptr_t>(host));
}

/* Report a DMA whose destination is too small for the requested transfer. Rate-limited: a load
   loop that trips this once normally trips it on every block, and Dma_LoadAssets issues one call
   per 1 KB. */
extern "C" void gdx_dma_report_short_dest(const void* dst, unsigned int size, size_t capacity,
                                          unsigned int romOffset) {
    static int sReports = 0;
    if (sReports >= 16) {
        return;
    }
    ++sReports;
    gdx_port_logf("[dma] REFUSED copy: dst=%p needs %u bytes, only %zu writable (romOffset=%08X) "
                  "-- this would have written past the end of the destination buffer\n",
                  dst, size, capacity, romOffset);
}

extern "C" void* gdx_resolve_registered_host_address(unsigned int addr) {
    /* Two candidates per lookup: the raw value, then the value with bit 31 restored.
       Decomp code routinely converts pointers with the KSEG0->physical idiom
       (osVirtualToPhysical / K0_TO_PHYS strips bit 31) before storing them in
       32-bit fields (audio acmd lists, DMA descriptors). On Windows the module
       and heap sit below 2 GB so low32 never has bit 31 set and the strip is a
       no-op; on Linux PIE/mmap the low32 of a host pointer frequently has bit 31
       set, and the stripped form matched nothing — the audio HLE's
       LOADBUFF/SAVEBUFF ops resolved NULL and were skipped, producing the
       all-zero (silent) sample output. The exact-match pass always runs first,
       so this cannot shadow a legitimate raw match. */
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t candidate = (pass == 0) ? addr : (addr | 0x80000000u);
        if (pass == 1 && candidate == addr) {
            break; /* bit 31 already set: nothing new to try */
        }
        for (const HostRange& range : gHostRanges) {
            if ((range.begin == 0) || (range.size == 0)) {
                continue;
            }

            const uint32_t baseLow = Low32(range.begin);
            const uint32_t offset = candidate - baseLow;
            if (offset < range.size) {
                static int sRegisteredResolveLogs = 0;
                if (sRegisteredResolveLogs < 8) {
                    ++sRegisteredResolveLogs;
                    gdx_port_logf("[registered-resolve] raw=%08X cand=%08X base=%p baseLow=%08X size=0x%zx -> %p\n",
                                  addr, candidate, reinterpret_cast<void*>(range.begin), baseLow, range.size,
                                  reinterpret_cast<void*>(range.begin + offset));
                }
                return reinterpret_cast<void*>(range.begin + offset);
            }
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

    /* Same two-candidate rule as gdx_resolve_registered_host_address above: the raw
       low32, then low32 with bit 31 restored (KSEG0->physical stripping — see the
       comment there). On Linux PIE the module's BSS low32 range regularly crosses
       0x80000000, so stripped pointers reconstructed below moduleBegin and the +4GB
       correction overshot moduleEnd -> NULL (silent audio: skipped LOADBUFF/SAVEBUFF). */
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t candidate = (pass == 0) ? addr : (addr | 0x80000000u);
        if (pass == 1 && candidate == addr) {
            break;
        }

        uintptr_t full = (moduleBegin & 0xFFFFFFFF00000000ULL) | static_cast<uintptr_t>(candidate);
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
                gdx_port_logf("[module-resolve] raw=%08X cand=%08X -> %p module=[%p,%p)\n",
                              addr, candidate, reinterpret_cast<void*>(full),
                              reinterpret_cast<void*>(moduleBegin), reinterpret_cast<void*>(moduleEnd));
            }
            return reinterpret_cast<void*>(full);
        }
    }
    return nullptr;
}

// gFrameMirrorFb / gFrameMirrorValid are declared near the top of this file
// (just after the gGameMode extern) so gdx_vi_present_fallback can also
// write them; see the comment there.

// ===== Host API (see port/n64_gfx_bridge.h) — thin accessors over the module state above. =====
extern "C" void gdx_gfx_interp_set_now_fn(GdxInterpNowFn fn) {
    gGdxInterpNowFn = fn;
}

extern "C" int gdx_gfx_interp_host_active(void) {
    return gdx_interp::P2HostActive() ? 1 : 0;
}

// port/gdx_frame_pacer.c used to read the RAW FrameInterpolation CVar to
// decide "am I mutually excluded this tick", but main.cpp's per-tick interpOn additionally forces
// the classic single-present path off (interpOn = host_active && !interpEditorActive) while an EK
// editor (Course Edit / Create Machine) is active. With BOTH FrameInterpolation and FramePacing on,
// an editor tick took the classic branch (which DOES call gdx_frame_pacer_tick()) but the raw CVar
// was still 1, so the pacer self-unarmed and neither pacing mechanism ran for that tick (free-run).
// This accessor exposes the per-tick truth main.cpp already committed via
// gdx_gfx_interp_tick_config (gGdxInterpHostCfg.active) instead of the always-on raw CVar, so the
// pacer's mutual-exclusion check reflects what THIS tick actually did, not the menu toggle.
extern "C" int gdx_gfx_interp_tick_active(void) {
    return gGdxInterpHostCfg.active ? 1 : 0;
}

extern "C" void gdx_gfx_interp_tick_config(int active, double tickStart, double tickDuration,
                                           int maxSubframes) {
    gGdxInterpHostCfg.active = (active != 0);
    gGdxInterpHostCfg.tickStart = tickStart;
    gGdxInterpHostCfg.tickDuration = tickDuration;
    gGdxInterpHostCfg.maxSubframes = (maxSubframes > 0) ? maxSubframes : 1;
    // Reset per tick; gdx_gfx_run sets it back to true only if it actually presents (a gfx task
    // ran AND p2Host held). On a taskless tick it stays false and the host presents once.
    gGdxInterpPresentedLastTick = false;
    // THE tick boundary. This is the only place in the bridge that runs exactly once per 60 Hz
    // logic tick (the host calls it per iteration, before dispatch), so it is where the
    // referenced-offset set is armed to roll. gdx_gfx_run cannot do it: it runs per GFX task.
    gGdxInterpNewTick = true;
    gGdxInterpLastTasks = gGdxInterpTasksThisTick;
    gGdxInterpTasksThisTick = 0;
}

// Tasks (gdx_gfx_run calls) the previous tick submitted. Surfaced so the [interp-p2] line can show
// the number the per-task/per-tick distinction turns on, instead of it being folklore.
extern "C" int gdx_gfx_interp_last_tasks(void) {
    return gGdxInterpLastTasks;
}

extern "C" int gdx_gfx_interp_presented_last_tick(void) {
    return gGdxInterpPresentedLastTick ? 1 : 0;
}

extern "C" int gdx_gfx_interp_last_subframes(void) {
    return gGdxInterpLastSubframes;
}

extern "C" double gdx_gfx_interp_last_t(void) {
    return gGdxInterpLastT;
}

// Real-FPS visibility. Declared locally (extern "C") by gdx_menu.cpp's Stats
// page and by the FPS overlay — same minimal-include idiom as gdx_gfx_interp_last_subframes above,
// so no n64_gfx_bridge.h change is needed. presents_per_sec is a rolling ~0.5 s meter of true
// sub-frame presents; last_lerped/last_snapped are the previous tick's per-slot tween/snap counts.
extern "C" double gdx_gfx_interp_presents_per_sec(void) {
    return gGdxInterpPresentsPerSec;
}

/* Tick-budget reserve exchange between the host loop and the sub-frame burst.
 *
 * The burst does NOT own the whole 60 Hz tick: only ~0.8 ms of the game frame runs before the gfx
 * task is submitted, and the game continues after osSpTaskStartGo, so several more milliseconds of
 * game work happen AFTER the burst returns -- inside the same tick. A budget guard that ignores
 * that tail hands the loop a 15.9 ms slot that is really ~9.8 ms, and consequently never fires.
 *
 * The reserve must be measured as WORK, never as elapsed wall-clock: deriving it from the interval
 * between bursts silently includes the logic pacer's sleep, which makes the estimate self-
 * reinforcing (healthy sim -> pacer sleeps -> reserve grows -> passes dropped -> more sleep). That
 * version converged on one pass per tick and pinned presents at 59.9/s. Only the host loop can
 * bracket real work, so it owns the arithmetic and publishes the answer here. */
static double gGdxInterpLastBurstSec = 0.0;
static double gGdxInterpNonBurstReserve = 0.008; // seeded near the observed tail; host refines it

extern "C" double gdx_gfx_interp_last_burst_seconds(void) {
    return gGdxInterpLastBurstSec;
}

extern "C" void gdx_gfx_interp_set_nonburst_reserve(double seconds) {
    // Clamp rather than trust: a mode change or breakpoint can make one tick arbitrarily long, and
    // a poisoned reserve would suppress every burst until it decayed back out.
    if (seconds > 0.0 && seconds < 0.100) {
        gGdxInterpNonBurstReserve = seconds;
    }
}

static double gdx_gfx_interp_nonburst_reserve(void) {
    return gGdxInterpNonBurstReserve;
}
/* [interp-pair] Pairing-quality readout. Largest prev->cur translation delta among slots that
   actually PAIRED this tick, and how many of those exceeded a plausible per-tick motion. See
   gdx_interp.h TranslationDelta: byte-offset slot identity can pair two different objects when the
   pool layout shifts, and the 2000-unit teleport guard is far too coarse to notice. Latched into
   file globals at the same site as the other P1 counters, since the adapter is tick-scoped. */
extern "C" float gdx_gfx_interp_pair_max_delta(void) {
    // Read-and-reset: each printed value is the worst pairing seen since the previous line, not an
    // all-time high that would saturate on the first bad tick and never move again.
    const float v = gGdxInterpPairMaxDelta;
    gGdxInterpPairMaxDelta = 0.0f;
    return v;
}

extern "C" int gdx_gfx_interp_pair_suspect(void) {
    return static_cast<int>(gGdxInterpPairSuspect);
}

extern "C" int gdx_gfx_interp_idem_divergent(void) {
    return static_cast<int>(gGdxIdemDivergentTicks);
}

extern "C" int gdx_gfx_interp_idem_multipass(void) {
    return static_cast<int>(gGdxIdemMultiPassTicks);
}

extern "C" int gdx_gfx_interp_pair_lerped_total(void) {
    return static_cast<int>(gGdxInterpPairLerped);
}

extern "C" int gdx_gfx_interp_last_lerped(void) {
    return static_cast<int>(gGdxInterpLastLerped);
}
extern "C" int gdx_gfx_interp_last_snapped(void) {
    return static_cast<int>(gGdxInterpLastSnapped);
}
extern "C" int gdx_gfx_interp_last_dropped(void) {
    return gGdxInterpLastDropped;
}

// Tier 2/3 coverage counters: viewport and effects-vertex batches classified this tick. Read by
// main.cpp's [interp-p2] line. The lerped/snapped split is the acceptance instrument for the
// effects-vertex work in particular: its byte-offset identity churns far harder than the matrices'
// (batch sizes flip 4<->5 with boost state, spawns shift every downstream offset), and a snapped
// batch renders exactly like the pre-Tier-3 build — so a high snap ratio means "shipped but inert",
// which must be visible in one glance at the log, not discovered by eyeballing flames.
static size_t gGdxInterpLastVpLerped = 0;
static size_t gGdxInterpLastVpSnapped = 0;
static size_t gGdxInterpLastVtxLerped = 0;
static size_t gGdxInterpLastVtxSnapped = 0;
extern "C" int gdx_gfx_interp_last_vp_lerped(void) {
    return static_cast<int>(gGdxInterpLastVpLerped);
}
extern "C" int gdx_gfx_interp_last_vp_snapped(void) {
    return static_cast<int>(gGdxInterpLastVpSnapped);
}
extern "C" int gdx_gfx_interp_last_vtx_lerped(void) {
    return static_cast<int>(gGdxInterpLastVtxLerped);
}
extern "C" int gdx_gfx_interp_last_vtx_snapped(void) {
    return static_cast<int>(gGdxInterpLastVtxSnapped);
}

// P4 determinism gate: per-tick logic-state fingerprint. Called ONCE per rendered tick from
// gdx_gfx_run, on BOTH the interpolation-ON and interpolation-OFF paths (gdx_gfx_run is reached
// identically either way, and the tick counter advances only on ticks that produce a gfx task --
// the same ticks on both paths, so the sequences stay index-aligned). It reads ONLY game-logic RNG
// state that the render path never touches (interpolation reads GfxPools and writes only scratch
// -- the prime directive), so with identical input the fingerprint sequence is byte-identical ON
// vs OFF, and the FIRST divergent tick localizes any leak of a sub-frame value back into logic.
// No-op unless GDX_INTERP_DETERMINISM is set (parsed once).
//
// To use it: record a ghost with FrameInterpolation OFF, replay it twice with
// GDX_INTERP_DETERMINISM=1 (once interp OFF, once ON), and diff the [interp-determinism] lines.
// Byte-identical tick-for-tick means interpolation is provably render-only. The RNG fingerprint is
// only the canary -- the real gate is the ghost byte-streams and finishing times themselves; this
// makes a divergence cheap to localize.
static void GdxInterpDeterminismTick() {
    if (!gdx_dev_gate(GDX_GATE_INTERP_DETERMINISM)) {
        return;
    }
    static uint64_t sTick = 0;
    const uint32_t words[4] = {
        static_cast<uint32_t>(gRandSeed1), gRandMask1,
        static_cast<uint32_t>(gRandSeed2), gRandMask2,
    };
    uint64_t h = 0xCBF29CE484222325ull; // FNV-1a
    for (uint32_t w : words) {
        h ^= w;
        h *= 0x100000001B3ull;
    }
    gdx_port_logf("[interp-determinism] tick=%llu rng=%08X/%08X/%08X/%08X hash=%016llX\n",
                  static_cast<unsigned long long>(sTick++),
                  static_cast<unsigned>(words[0]), static_cast<unsigned>(words[1]),
                  static_cast<unsigned>(words[2]), static_cast<unsigned>(words[3]),
                  static_cast<unsigned long long>(h));
}

extern "C" void gdx_gfx_run(void* dl, size_t dl_size, GdxTaskUcode taskUcode) {
    // Time the WHOLE bridge call, so the perf summary's "logic" figure stops absorbing work that
    // is not game logic. logic is derived as gametick - (xlate + run + mirror), and only ConvertRoot
    // was ever timed, so every other thing this function does was being reported as decomp time.
    //
    // Scope guard rather than a begin/end pair: this function has several early returns (no window,
    // no interpreter, bad display list), and a leaked open timer would silently corrupt every
    // subsequent sample rather than fail loudly.
    // The POST half is opened after the sub-frame burst, but this function has several exits after
    // that point, so the guard closes it on whichever one is taken rather than requiring every
    // return site to remember.
    bool gdxPostTimerOpen = false;
    struct GdxGfxRunTimer {
        bool* postOpen;
        explicit GdxGfxRunTimer(bool* p) : postOpen(p) {
            gdx_perf_sub_begin(GDX_PERF_SUB_GFXRUN);
        }
        ~GdxGfxRunTimer() {
            if (*postOpen) {
                gdx_perf_sub_end(GDX_PERF_SUB_POST);
            }
            gdx_perf_sub_end(GDX_PERF_SUB_GFXRUN);
        }
    } gdxGfxRunTimer(&gdxPostTimerOpen);
    gdx_perf_sub_begin(GDX_PERF_SUB_SETUP);

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

    // Advance the wide-conversion cache's frame counter once
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

    // Register the boot-logo seed hook ALWAYS and gate the
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
                    // The _putenv stays until interpreter.cpp is migrated off getenv (its probe
                    // still samples the CRT environment); gdx_dev_gate_force arms the same probe
                    // on the bridge side, which has already sampled the environment by now.
                    _putenv("GDX_DIAG_SETTIMG=1");
                    gdx_dev_gate_force(GDX_GATE_DIAG_SETTIMG, 1);
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
    // The task ucode is this display list's ENTRY state, not a property of the whole frame, so it
    // is kept in a local and re-armed before every sub-frame replay (see the pass loop below).
    // A mid-list G_LOAD_UCODE variant-switch marker mutates mF3dex2Variant during the walk
    // (interpreter.cpp:7159) and SpReset does not restore it. Set once per task, pass 0 therefore
    // walks the list from the task variant while every replay walks it from whatever the previous
    // pass ended on -- and under Reject/FZeroFlxReject that arms the 2x-viewport reject box
    // (interpreter.cpp:3007) over triangles pass 0 rendered normally. Measured: ~39 extra clip
    // rejections per task with bit-identical vertex and matrix hashes, converging after the first
    // replay because the list's end state is a fixed point. Visible as the floor and clouds
    // dropping out on replays while pass 0 stays correct.
    Fast::F3dex2Variant gdxTaskVariant = Fast::F3dex2Variant::Standard;
    switch (taskUcode) {
        case GDX_TASK_UCODE_F3DLX2_REJ:
            gdxTaskVariant = Fast::F3dex2Variant::Reject;
            break;
        case GDX_TASK_UCODE_F3DFLX2_REJ:
            gdxTaskVariant = Fast::F3dex2Variant::FZeroFlxReject;
            break;
        case GDX_TASK_UCODE_F3DEX2:
        default:
            gdxTaskVariant = Fast::F3dex2Variant::Standard;
            break;
    }
    interp->SetF3dex2Variant(gdxTaskVariant);

    for (int i = 0; i < 16; i++) {
        interp->mSegmentPointers[i] = gSegments[i];
    }

    EnsureSetupGfxSegment();
    EnsureAssetSegmentForSymbol(Low32(reinterpret_cast<uintptr_t>(aVpFullScreen)));
    for (int i = 0; i < 16; i++) {
        interp->mSegmentPointers[i] = gSegments[i];
    }

    bool isBigEndian = IsLikelyBigEndianDisplayList(static_cast<const N64Gfx*>(dl), dl_size / sizeof(N64Gfx));

    // Sub-phase: per-command DL translation (the adapter's ConvertRoot walk). See gdx_perf.h.
#ifdef _WIN32
    ResetWindowsMemoryRegionCache();
#endif
    gdx_perf_sub_end(GDX_PERF_SUB_SETUP);
    const double gdxXlateT0 = (gGdxVenueWatchTicks > 0 && gGdxInterpNowFn != nullptr) ? gGdxInterpNowFn() : 0.0;
    gdx_perf_sub_begin(GDX_PERF_SUB_XLATE);
    ConversionStats stats = {};
    N64DisplayListAdapter adapter(dl, dl_size, isBigEndian, &stats);
    // Latch the current/previous GfxPool bases and reset the referenced-offset set BEFORE
    // ConvertRoot drains the G_MTX reroutes (which populate this tick's lerp list). No-op unless P1.
    adapter.GdxInterpBeginTick();
    Fast::F3DGfx* converted = adapter.ConvertRoot();
    // The referenced-offset set is NOT promoted here. This function runs once per GFX TASK and the
    // game submits several per tick, so the promotion happens at the real tick boundary inside
    // GdxInterpBeginTick (see gGdxInterpNewTick) where a complete tick's set is available.
    // Emit this tick's determinism fingerprint (no-op unless GDX_INTERP_DETERMINISM set).
    // Placed on the common path so it runs identically whether interpolation is ON or OFF this tick.
    GdxInterpDeterminismTick();
    gdx_perf_sub_end(GDX_PERF_SUB_XLATE);
    // [venueload] The discriminating measurement. If translation stays flat across these ticks the
    // cost is the load itself and a boot-time preload fixes it. If it spikes, the ++gConvertEpoch
    // in the load path invalidated every cached conversion and we are paying full re-translation
    // for several consecutive ticks -- which no amount of preloading would avoid.
    if (gGdxVenueWatchTicks > 0 && gGdxInterpNowFn != nullptr) {
        --gGdxVenueWatchTicks;
        gdx_port_logf("[venueload] post tick=%d xlate=%.2fms lists=%zu cmds_out=%zu epoch=%u\n",
                      8 - gGdxVenueWatchTicks, (gGdxInterpNowFn() - gdxXlateT0) * 1000.0,
                      stats.convertedLists, stats.commandsOut, (unsigned) gConvertEpoch);
    }
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
        /* One-shot dump of every ucode stub symbol's low32 so
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
        stats.l3dexUcodeSkips != 0 || stats.skippedEpochRetries != 0;
    if (shouldLogDiagnostics) {
        if (gdx_diag_verbose()) {
            const unsigned int romFallbackTotal = gdx_segment_source_fallback_total();
            gdx_port_logf("[gfxdiag] lists=%zu f3d_lists=%zu cmds=%zu noop_dl=%zu noop_raw=%08X "
                          "miss_dl=%zu miss_raw=%08X bad_dl=%zu bad_raw=%08X "
                          "fallback_data=%zu skip_data=%zu skip_tex=%zu skip_epoch=%zu "
                          "tex_copy_bytes=%zu vtx=%zu mtx=%zu dl=%zu teximg=%zu settile=%zu "
                          "tlut=%zu loadblk=%zu loadtile=%zu tilesize=%zu texrect=%zu fillrect=%zu "
                          "setcimg=%zu setzimg=%zu tris=%zu end=%zu "
                          "ucode_switch=%zu ucode_unknown=%zu ucode_raw=%08X "
                          "ucode_l3d_skip=%zu ucode_l3d_raw=%08X size=%zu romfb=%u\n",
                          stats.convertedLists, stats.f3dLists, stats.commandsOut,
                          stats.noopDisplayLists, stats.firstNoopDlRaw,
                          stats.missingDisplayLists, stats.firstMissingDlRaw,
                          stats.badDisplayLists, stats.firstBadDlRaw,
                          stats.fallbackDataCommands,
                          stats.skippedDataCommands, stats.skippedTextures, stats.skippedEpochRetries,
                          stats.textureCopyBytes, stats.opCounts[kOpVtx],
                          stats.opCounts[kOpMtx], stats.opCounts[kOpDl], stats.opCounts[kOpSetTextureImage], stats.opCounts[kOpSetTile],
                          stats.opCounts[kOpLoadTlut], stats.opCounts[kOpLoadBlock], stats.opCounts[kOpLoadTile], stats.opCounts[kOpSetTileSize],
                          stats.opCounts[0xE4] + stats.opCounts[0xE5], stats.opCounts[0xF6], stats.opCounts[kOpSetColorImage],
                          stats.opCounts[kOpSetDepthImage],
                          stats.opCounts[0x05] + stats.opCounts[0x06] + stats.opCounts[0x07] + stats.opCounts[0xBF],
                          stats.opCounts[kOpEndDl],
                          stats.ucodeSwitches, stats.unknownUcodeSwitches,
                          stats.firstUnknownUcodeRaw,
                          stats.l3dexUcodeSkips, stats.firstL3dexUcodeRaw, dl_size, romFallbackTotal);
            /* When any raw-ROM fallback has happened, emit one
               extra line listing the nonzero families, rate-limited to the periodic
               gfxdiag cadence so a per-frame stats trigger cannot spam it. */
            if (romFallbackTotal != 0 && (sDiagFrames < 8 || (sDiagFrames % 120) == 0)) {
                char famLine[512];
                size_t famOff = 0;
                const char* famKey = nullptr;
                unsigned int famFb = 0;
                for (unsigned int fi = 0; GdxSegmentSourceFamilyStats(fi, &famKey, &famFb); ++fi) {
                    if (famFb != 0 && famKey != nullptr && famOff + 1 < sizeof(famLine)) {
                        int n = std::snprintf(famLine + famOff, sizeof(famLine) - famOff,
                                              " %s=%u", famKey, famFb);
                        if (n > 0) {
                            famOff += static_cast<size_t>(n);
                        }
                        if (famOff >= sizeof(famLine)) {
                            famOff = sizeof(famLine) - 1;
                            break;
                        }
                    }
                }
                famLine[famOff] = '\0';
                gdx_port_logf("[gfxdiag] romfb families:%s\n", famLine);
            }
        }
        // [gfxfail]/[datafail] per-frame aggregate diagnostics: silent unless
        // GDX_DIAG_VERBOSE=1. The per-occurrence, bounded [gdl-miss]/[gdl-bad] lines
        // (and the bounded [gfxfail] ROOT-rejected error above) stay always-on.
        if (gdx_diag_verbose() && stats.noopDisplayLists != 0) {
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
        if (gdx_diag_verbose() && (stats.fallbackDataCommands != 0 || stats.skippedDataCommands != 0)) {
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

    // ===== P0/P1 retention + M=2 replay (DEBUG-ONLY, GDX_INTERP_P0 / GDX_INTERP_P1) =====
    // Pools are quiescent across this whole window: D_800DCCFC ^= 1 toggles only in the NEXT
    // tick's Gfx_InitBuffer (decomp/src/sys/sys_gfx.c:115-125), so both GfxPools — and every byte
    // this buffer dereferences — are stable while we replay.
    // This block sits BEFORE the post-Run buffer frees below (gPersistentAllocations.clear, native
    // RGBA16 range retirement), so pass 1's Run() dereferences the same live inputs as pass 0.
    //
    // P0 (evidence-only, both passes t=1) and P1 (lerp: pass 1 renders the tween) are mutually
    // exclusive: P1 breaks P0's t=1 transparency/hash invariants, so when GDX_INTERP_P1 is set the
    // P0 evidence path is suppressed and P1 owns the replay. PASS ORDERING (verified against this
    // block + interpreter.cpp): each interp->Run() clears+redraws mGameFb WITHOUT presenting
    // (present is interp->EndFrame()/SwapBuffers, host-called ONCE), so the LAST Run's output is
    // what EndFrame presents. Pass 0 renders at t=1 first; pass 1 renders the presented frame at
    // presentT (0.5 for "mid") second — so the host presents the interpolated midpoint.
    // ===== P2 branch: host-driven main-loop render/logic decoupling =====
    // When the host owns pacing this tick (gEnhancements.Graphics.FrameInterpolation / GDX_INTERP_P2,
    // committed by main.cpp via gdx_gfx_interp_tick_config before dispatch), the retained buffer is
    // replayed AND PRESENTED M times right here — the only place it is alive (this sits before the
    // post-Run frees; the pool stays quiescent until the next tick's Gfx_InitBuffer). Each
    // sub-frame is a COMPLETE present via fw->DrawAndRunGraphicsCommands (StartDraw -> StartFrame ->
    // Run -> EndDraw -> EndFrame), so composite / ImGui / MSAA-resolve are all correct per sub-frame.
    // The host does NOT open its own present bracket on interp ticks (would nest the ImGui frame);
    // it hands ownership here and only paces the LOGIC deadline (the frame pacer is mutually excluded).
    // The env-gated P0/P1 in-bridge diagnostics keep their own single-present M=2 path in the else.
    const bool p2Host = adapter.GdxP2HostActive() && gGdxInterpHostCfg.active;
    if (p2Host) {
        // Game logic already ran exactly once (ConvertRoot above); no sub-frame re-enters it.
        // Sub-frames read the two GfxPools and write only scratch — render-only (prime directive).
        // DETERMINISTIC sub-frame schedule (SoH interpolate_frame).
        // main.cpp derives `count` (delivered as maxSubframes) from the target rate via a rational
        // remainder accumulator (running remainder, NO clock reads), so it is stable per tick and
        // averages target/60 sub-frames per tick across ticks. Here we simply present exactly `count`
        // evenly-spaced sub-frames at t = (k+1)/count. The OLD wall-clock accumulator (t = (now -
        // tickStart)/tickDur sampled inside the loop) coupled t to game-logic wall time, so t clustered
        // unpredictably and the present count oscillated with VSync jitter into an unstable
        // framerate. Even spacing with the LAST pass at t = count/count = 1.0 presents the newest pose
        // (byte-identical to stock, minimizing latency) with uniform tweens leading up to it.
        const int count = (gGdxInterpHostCfg.maxSubframes > 0) ? gGdxInterpHostCfg.maxSubframes : 1;
        const size_t lerpSlots = adapter.GdxP0ScratchSlots();
        // Discontinuity safety: if this tick referenced no pool
        // matrices, or every referenced slot snapped (empty lerp list, or PrevPoolBase mismatch so all
        // prev keyframes are unusable), there is nothing to tween — every pass renders at t=1
        // (content identical to the disabled path for this tick).
        // The old guard also DROPPED to a
        // single present on these ticks, so the presented rate FLAPPED between the target and 60
        // whenever a menu/transition/cut tick had zero lerpable slots (telemetry: 120 -> 91 -> 60 ->
        // 120 across one menu visit). A flapping rate is far more jarring than the redundant
        // re-presents are costly, so keep presenting `count` passes — all at t=1 — for a CONSTANT
        // present cadence. Content per pass is byte-identical to the old single pass, so the
        // transition-capture contract (GdxTransitionCapturePendingThisTick block above: the mirror
        // must sample the un-interpolated tick) is preserved exactly.
        const bool degenerate = (lerpSlots == 0) || (adapter.GdxP1Lerped() == 0);
        // Mutable: the budget pre-sizer below may shrink this BEFORE the loop, so the t denominator
        // shrinks with it and the last presented pose stays t=1. See the pre-sizer comment.
        int passes = count;
        const int gdxPlannedPasses = count;

        // [interp-pace] probe: when each pass was ATTEMPTED and whether the limiter took it. Kept
        // after the pacing experiment it was built to test (see the note in the loop) because it
        // settled that question in a single run where frame-rate averages had argued in circles for
        // hours. `window` is how much of the tick remained when the present loop was entered --
        // measured at ~15.86ms of a 16.68ms tick, i.e. the loop starts almost at the tick boundary,
        // NOT after a long game-logic phase as the perf phase breakdown had suggested.
        const double gdxLoopStart = (gGdxInterpNowFn != nullptr) ? gGdxInterpNowFn() : 0.0;
        double gdxPaceWindow = 0.0;
        if (gGdxInterpNowFn != nullptr && gGdxInterpHostCfg.tickDuration > 0.0) {
            gdxPaceWindow = (gGdxInterpHostCfg.tickStart + gGdxInterpHostCfg.tickDuration) - gdxLoopStart;
            if (gdxPaceWindow <= 0.0 || gdxPaceWindow > gGdxInterpHostCfg.tickDuration) {
                gdxPaceWindow = 0.0;
            }
        }
        double gdxAttemptAt[8] = {};
        bool gdxAttemptOk[8] = {};

        // Take ownership of pacing for the sub-frame burst; the swapchain's waitable object does the
        // real pacing. See the block comment on gdx_fast3d_set_subframe_present in
        // libultraship/src/fast/Fast3dWindow.cpp for why the software limiter cannot pace a burst.
        // GDX_INTERP_LIMITER=1 restores the old behaviour for A/B without a rebuild.
        static const bool sHonourLimiter = [] {
            const char* e = getenv("GDX_INTERP_LIMITER");
            return e != nullptr && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
        }();
        gdx_fast3d_set_subframe_present(sHonourLimiter ? 0 : 1);

        // Budget guard for the loop below. Declared out here, not in the loop body, so its
        // thread-safe-static guard variable is not re-checked on every pass.
        //
        // WHY THIS EXISTS. Bypassing the software limiter above removed the only mechanism in the
        // system that could shed load: IsFrameReady used to return false and skip a present cheaply
        // when the schedule was behind. Nothing replaced it, so the tick had to pay for all M
        // presents no matter how long they took. That is not merely a frame-rate problem, because
        // gdx_vi_tick advances the simulation exactly ONCE per host-loop iteration and there is no
        // catch-up anywhere: every millisecond this loop overruns is a millisecond the 60 Hz sim
        // never gets back. Measured consequence before this guard, at a 144 Hz target: sim rate
        // median 57 Hz, mean 52 Hz, worst 8.6 Hz -- the game visibly in slow motion, the
        // machine-select model spinning slow and the 3-2-1-GO countdown stretching in real seconds.
        //
        // THE TRADE, stated plainly: when the tick budget is gone, drop the remaining sub-frames.
        // A frame-rate dip is a smoothness cost; a slow game clock is a correctness fault. Smoothness
        // is the thing that yields.
        //
        // THE GUARD MUST PREDICT, NOT REACT. A first attempt asked "is the tick budget already
        // spent?" before each pass, and it NEVER FIRED ONCE across 76 measured samples while ticks
        // were overrunning by 7ms -- because the overrun happens DURING the pass that follows the
        // check, not before it. With three ~5ms passes in a 16.68ms tick, the check at pass 2 sees
        // ~14ms elapsed, waves it through, and the pass ends at ~21ms. Reacting to an overrun that
        // has already happened is worthless; the pass has to be refused before it starts.
        //
        // So the test is "will this pass FIT?", using a measured cost rather than a guessed one:
        // an exponential moving average of how long a pass has actually been taking. An EMA rather
        // than the last sample because a single expensive pass (a shader compile, a texture upload)
        // must not collapse the burst for the next tick, and rather than a fixed constant because
        // per-pass cost varies by an order of magnitude between a menu and a 30-machine race.
        //
        // GDX_NO_INTERP_BUDGET=1 disables the guard for A/B without a rebuild. A suppressed guard
        // must reproduce the slow sim to earn the claim that it is what fixed it.
        static const bool sNoBudgetGuard = [] {
            const char* e = getenv("GDX_NO_INTERP_BUDGET");
            return e != nullptr && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
        }();
        // PASS 0 AND THE REPLAYS ARE NOT THE SAME PRICE, and averaging them together is what kept
        // this guard over-conservative. Pass 0 pays for the tick's real rendering work -- texture
        // uploads, shader binds, cold caches -- while passes 1..M-1 re-execute the same command
        // buffer with everything already resident. Measured: ticks running 1.83 passes averaged
        // 2.98 ms/pass, while ticks running 1.02 passes averaged 6.20 ms/pass. Cost per pass
        // appeared to DOUBLE as passes were removed, which is only possible if the first one carries
        // most of the cost.
        //
        // The guard only ever decides about passes k >= 1, so it must price a REPLAY, not a blended
        // average that is dominated by a pass it has already committed to. Blending made a replay
        // look several times more expensive than it is and refused passes the tick could easily
        // afford -- visible as 3.6 ms of idle sitting unused every tick.
        //
        // THE REPLAY SEED MUST BE OPTIMISTIC, and this is not a tuning preference -- a pessimistic
        // seed deadlocks. The replay estimate is only ever updated by a replay that actually ran, so
        // seeding it high makes the guard refuse every replay, which means no samples arrive, which
        // means the seed never moves. Seeded at 7 ms it pinned the burst to ~1.1 passes while 3.9 ms
        // of every tick sat idle, and no amount of running longer would have recovered it.
        //
        // Seeded low the loop self-corrects in the safe direction: it attempts a replay, measures
        // what it really cost, and the average climbs within a few ticks if replays are expensive.
        // The downside is bounded -- at worst a handful of early ticks overrun slightly -- whereas
        // the pessimistic failure is permanent and silent.
        //
        // Pass 0 keeps the realistic seed: it is never refused, so its estimate cannot deadlock, and
        // it is tracked separately only because blending it into the replay price is what made
        // replays look several times more expensive than they are.
        static double gdxPass0CostEma = 0.007;
        static double gdxReplayCostEma = 0.0005;
        // THE BURST DOES NOT OWN THE WHOLE TICK, and assuming it did is why the first two versions of
        // this guard never fired. Only ~0.8ms of the game frame runs BEFORE the gfx task is
        // submitted; the game continues after osSpTaskStartGo, so roughly 6ms of game work still has
        // to happen AFTER this loop returns, inside the same 16.68ms tick. Measured: window reads
        // ~15.9ms of budget remaining while the tick actually ends up 19.5ms long.
        //
        // So reserve that tail. Rather than hardcode it -- it differs by scene, and a wrong constant
        // silently mis-sizes every burst -- measure it: the interval between consecutive entries to
        // this loop is one whole tick, and this loop's own duration is known, so the difference is
        // everything else the tick does. An EMA of that is the reservation.
        // The reserve must measure WORK, not elapsed time, and only the host loop can tell the
        // difference. A first version derived it here, as (interval between loop entries) minus
        // (this loop's duration) -- which silently included the logic pacer's SLEEP. That built a
        // feedback loop that ate itself: a healthy sim means the pacer sleeps, sleep inflates the
        // reserve, a bigger reserve drops more passes, fewer passes means more sleep. It converged
        // on exactly one pass per tick, pinning presents at 59.9/s -- a perfect 60 Hz sim with
        // interpolation contributing nothing, which is not the trade anyone asked for.
        //
        // So main.cpp publishes it instead: it brackets the tick's real work (tick start to the
        // moment before gdx_host_pace_logic_until) and subtracts the burst, so idle time never
        // enters. See gdx_gfx_interp_set_nonburst_reserve there.
        // Reserve only the TAIL, not all non-burst work. The published figure covers everything in
        // the tick that is not the burst, which includes the ~0.8 ms of game frame that ran BEFORE
        // this loop was entered. That part is already spent and already reflected in the clock the
        // guard compares against, so counting it again shrinks the budget twice and costs a whole
        // pass: measured 3.6 ms of idle per tick sitting unused while the guard refused a 3.0 ms
        // pass. Subtract the pre-burst offset so the reservation describes only what still has to
        // happen after this loop returns.
        const double gdxPreBurst = (gGdxInterpHostCfg.tickStart > 0.0 && gdxLoopStart > gGdxInterpHostCfg.tickStart)
                                       ? (gdxLoopStart - gGdxInterpHostCfg.tickStart)
                                       : 0.0;
        double gdxReserve = gdx_gfx_interp_nonburst_reserve() - gdxPreBurst;
        if (gdxReserve < 0.0) {
            gdxReserve = 0.0;
        }
        // Never reserve so much that pass 0 is all the tick can ever afford -- at that point the
        // guard would be permanently disabling interpolation rather than protecting it. Half the
        // tick is the floor; beyond that the honest answer is that the machine cannot run this
        // target, which the frame rate itself will report.
        if (gGdxInterpHostCfg.tickDuration > 0.0 && gdxReserve > gGdxInterpHostCfg.tickDuration * 0.5) {
            gdxReserve = gGdxInterpHostCfg.tickDuration * 0.5;
        }
        const double gdxBudgetEnd =
            (gGdxInterpHostCfg.tickDuration > 0.0)
                ? (gGdxInterpHostCfg.tickStart + gGdxInterpHostCfg.tickDuration - gdxReserve)
                : 0.0;
        int budgetDropped = 0;

        // PRE-SIZE THE BURST instead of only clipping it mid-loop. The mid-loop guard decides after
        // the t values are already fixed at (k+1)/passes, so clipping the third pass of a 3-pass
        // tick presents t=1/3 and t=2/3 and NEVER PRESENTS t=1 -- the motion stream skips the
        // tick's newest pose and takes a double-width step into the next tick (0.33, 0.67, then
        // 1.33): a judder spike riding exactly on the ticks that were already struggling. Owner-
        // visible as the "caps at 120" feel: the cadence plateau is expected quantization (all-2s
        // x 60 Hz = 120), but the skipped terminal pose made it jarring rather than merely lower.
        //
        // Predicting the affordable pass count BEFORE the loop lets the t denominator shrink with
        // it: a tick sized to 2 passes presents t=1/2 and t=1 -- evenly spaced, terminal pose
        // correct, no skip. The mid-loop guard stays as the backstop for mispredictions (a pass
        // that runs far over its estimate can still push the budget past the wall).
        //
        // Affordability arithmetic: pass 0 always runs (this loop owns presentation; see the
        // guard's pass-0 rule), so the question is how many REPLAYS fit after it.
        if (!sNoBudgetGuard && gdxBudgetEnd > 0.0 && gdxLoopStart > 0.0 && passes > 1 &&
            gdxReplayCostEma > 0.0) {
            const double room = gdxBudgetEnd - gdxLoopStart - gdxPass0CostEma;
            int affordable = 1;
            if (room > 0.0) {
                const double replays = room / gdxReplayCostEma;
                // Cap the intermediate before the int conversion: with the optimistic replay seed
                // (0.0005s) `replays` can be in the tens of thousands, which is UB territory for a
                // float->int cast only above INT_MAX, but clamping first costs nothing and reads
                // as intent rather than luck.
                affordable = 1 + static_cast<int>(std::min(replays, 64.0));
            }
            if (affordable < passes) {
                budgetDropped += passes - affordable;
                passes = affordable;
                // DECAY ON SHRINK — the same one-sided-estimator lesson as the in-loop guard, which
                // this pre-sizer initially failed to inherit and promptly re-proved: a spike
                // inflated gdxReplayCostEma, the pre-sizer clamped every burst to 1 pass, no replay
                // ever ran again to supply a cheaper sample, and presents pinned at 60/s for an
                // entire session (passes=1 planned=3 on every pace line). Any estimator that gates
                // the only activity that can update it MUST decay when it says no.
                gdxReplayCostEma *= 0.94;
            }
        }

        int presented = 0;
        float lastT = 1.0f;
        // Pool quiescence: latch the GfxPool double-buffer parity before the loop.
        // The toggle (D_800DCCFC ^= 1) only ever runs in the NEXT tick's Gfx_InitBuffer (inside
        // gdx_dispatch), which this loop never re-enters, so the parity CANNOT change while
        // we replay both quiescent pools. We re-check after the loop and log once if the invariant is
        // ever violated — a falsifiable guard rather than a mid-present abort (aborting after frames
        // are already on screen would be worse than a diagnostic on a can't-happen path).
        const int poolParityAtStart = gdx_interp::PoolParity();
        // Issue G (in-race flicker under interpolation) — DOCUMENTED LIMITATION, not a bug here.
        // Every sub-frame below replays the SAME retained command buffer, substituting only
        // interpolated pool MATRICES (scratch slots). The command stream's combiner/alpha/vertex
        // data is whatever game logic emitted for THIS tick, keyed on gGameFrameCount (F-Zero X's
        // flicker-blend transparency: e.g. the low-energy body-color gradient blend and the
        // pursuit check-marker prim-alpha pulse alternate every 60 Hz tick — see decomp racer.c
        // gGameFrameCount & 7 / & 0xF modulation). That phase is
        // therefore FROZEN across all M sub-frames and only advances at the next 60 Hz tick.
        // Consequence: motion is smooth at the target rate, but the 60 Hz alternation is displayed
        // for M refreshes per phase, and because the rational accumulator makes M oscillate (2,3,2,3
        // at 144 Hz = target/60 = 2.4) consecutive phases get UNEQUAL screen time, so the intended
        // even blend reads as strobing. Option (c) "render each sub-frame's matching phase" is
        // infeasible: the other phase lives only in the adjacent tick's command buffer, which was
        // never retained, and re-deriving it would require re-running (or reaching back into) game
        // logic — a violation of the render-only prime directive. Option (a) "detect and blend
        // flicker-blend DL pairs" cannot distinguish game-logic-keyed alternation from legitimate
        // per-frame animation at the DL level, so it would blur correct content. The honest, safe
        // resolution is to leave interpolation matrix-only (SoH-class ports carry the identical
        // artifact) and surface it to the user via the FrameInterpolation menu tooltip.
        // Sub-frames the DXGI/GL limiter refused. DrawAndRunGraphicsCommands returns false without
        // rendering when the frame is dropped (Fast3dWindow.cpp: the IsFrameReady guard); counting
        // those as delivered made presents/s an upper bound and produced readings ABOVE the target
        // (a 153.5 sample against a 144 Hz target), which is what hid the real menu cost.
        // Seeded with the pre-sizer's drops so presented+dropped always sums to the PLANNED count:
        // a pass shed before the loop and a pass refused inside it are the same fact to telemetry.
        int dropped = budgetDropped;
        if (passes > 1) {
            ++gGdxIdemMultiPassTicks; // denominator for the divergence ratio
        }
        for (int k = 0; k < passes; ++k) {
            // TRIED AND REVERTED (2026-08-02): holding each pass until loopStart + window*k/passes.
            // The [interp-pace] probe proved the wait worked -- gaps came out at exactly
            // window/passes, 7.93ms for two passes -- and presents were STILL refused, frequently
            // every pass in a tick including pass 0. A refused FIRST present cannot be caused by
            // intra-tick spacing, so burst-calling was not the fault. It also regressed throughput
            // (median 63 presents/s, never above 128) and could not have reached 144 by
            // construction: window/passes yields 7.93ms gaps inside the tick and 8.75ms across the
            // tick boundary, averaging ~120 fps. Do not re-attempt without first explaining why
            // whole ticks are refused.
            // Budget guard. See the block comment on sNoBudgetGuard above the loop for why dropping
            // sub-frames is the correct response and a slow simulation is not.
            //
            // Pass 0 is never dropped: this loop owns presentation for the whole tick
            // (gGdxInterpPresentedLastTick tells the host not to present again), so skipping every
            // pass would leave the previous frame on screen and turn a rate dip into a freeze. One
            // present per tick is the floor, which is exactly the non-interpolated behaviour.
            //
            // Checked BEFORE the work rather than after, because the point is to not start a pass
            // that cannot finish inside the tick. The remaining passes are counted as dropped so the
            // telemetry stays honest -- presents/s must not be flattered by frames we chose to skip.
            if (k > 0 && !sNoBudgetGuard && gdxBudgetEnd > 0.0 && gGdxInterpNowFn != nullptr) {
                const double nowSec = gGdxInterpNowFn();
                if (nowSec + gdxReplayCostEma > gdxBudgetEnd) {
                    budgetDropped = passes - k;
                    dropped += budgetDropped;
                    // DECAY ON REFUSAL, or this estimator can never recover. It is one-sided: the
                    // only thing that updates it is a replay that was allowed to run, so any upward
                    // excursion is self-locking. Replays are genuinely expensive during boot and
                    // load, the average climbs, and from then on every replay is refused -- which
                    // means no cheap sample can ever arrive to bring it back down. Observed exactly
                    // that: presents pinned at 59.8/s with 3 ms of every tick idle, while the
                    // accumulator was still asking for 2-3 passes.
                    //
                    // Shrinking the estimate each time it causes a refusal makes the guard probe
                    // again after a few ticks. If replays really are expensive the next sample puts
                    // the average straight back up; if the scene got cheaper it settles at the new
                    // cost. The sim stays protected either way, because a probe can overrun by at
                    // most one replay.
                    gdxReplayCostEma *= 0.94;
                    break;
                }
            }
            const double gdxPassStart = (gGdxInterpNowFn != nullptr) ? gGdxInterpNowFn() : 0.0;
            if (k < 8) {
                gdxAttemptAt[k] = gdxPassStart;
            }
            // Deterministic even spacing: t = (k+1)/passes. Last pass = 1.0 (newest pose, exact).
            // GDX_INTERP_FORCE_T1: diagnostic only. Pins every sub-frame to t=1 so all M passes get
            // IDENTICAL matrices, while still replaying and presenting M times. This is the only way
            // to read idem_div as a statement about STATE: with varying t the passes render different
            // poses, so different triangles cull and clip and the bind sequence legitimately differs
            // -- divergence then means nothing. With t pinned, the input to every pass is identical,
            // so ANY divergence is state leaking across replays. Motion is frozen to 60 Hz while
            // this is set, which is expected; it is a measurement mode, not a play mode.
            static const bool sForceT1 = [] {
                const char* e = getenv("GDX_INTERP_FORCE_T1");
                const bool on = e != nullptr && e[0] != '\0' && strcmp(e, "0") != 0;
                // Announced unconditionally, once, because an A/B whose arm state is not in the log
                // is not an experiment. A FORCE_T1 play-test was run to falsify the booster
                // afterimage mechanism and the log carried NO evidence the pin was live -- lerped=
                // stays nonzero either way (it counts classification in ConvertRoot, not refills),
                // and t_last reads 1.000 either way. The result was uninterpretable and the run
                // wasted. Every A/B toggle must write its state to the log at arm time.
                gdx_port_logf("[interp] GDX_INTERP_FORCE_T1=%s (sub-frame t %s)\n", on ? "1" : "unset/0",
                              on ? "PINNED to 1.0 -- measurement mode, 60Hz motion expected" : "normal");
                return on;
            }();
            float t = (degenerate || sForceT1) ? 1.0f
                                               : (static_cast<float>(k + 1) / static_cast<float>(passes));
            if (t > 1.0f) {
                t = 1.0f;
            }
            adapter.GdxP0RefillScratch(t); // lerp(prev,cur,t) into every non-snapped scratch slot
            adapter.GdxVpRefillScratch(t);  // Tier 2: carousel viewports, same t
            adapter.GdxVtxRefillScratch(t); // Tier 3: effects vertices, same t
            // Re-arm the task's entry ucode variant. Replaying a display list must start from the
            // same RSP state pass 0 started from; a mid-list variant switch leaks forward otherwise.
            // See gdxTaskVariant above for the measurement this fixes.
            interp->SetF3dex2Variant(gdxTaskVariant);
            interp->ResetGeometryDiagnostics();
            // Full present of the SAME retained command buffer at this sub-frame's t. VSync ON: each
            // present blocks on the panel refresh, so the presents self-pace to the display and the
            // host logic-deadline wait (main.cpp) becomes a near-no-op. VSync OFF: presents don't
            // block; main.cpp's logic-deadline wait paces the SIM to 60 Hz. Logic stays 60 Hz either
            // way; the rational accumulator keeps the long-run present rate at the target.
            // Bracketed per PASS, not around the whole loop: a single bracket could not separate
            // pass 0 from pass 2, nor CPU work from the vsync wait. gdx_perf_sub_end accumulates
            // (subMs[id] += ...), so the per-tick total is unchanged and the phase mean now
            // reports per-sub-frame cost.
            // [interp-idem] MAKE THE REPLAY IDEMPOTENT. Measured before this existed: 182 of 3928
            // multi-pass ticks (4.6%) bound DIFFERENT textures on a replay than on pass 0, which is
            // the in-race floor flicker -- it appears the instant M > 1 and is clean at M == 1, and
            // uneven phase timing is ruled out (a true 120 Hz panel gives M == 2 exactly, every
            // phase on screen for 16.67ms, and it still strobed).
            //
            // Geometry cannot differ between replays: the track carries no matrix of its own and
            // the camera is not rerouted in a Release build. So the difference was STATE. Interpreter
            // RDP state survives Run(): loaded_texture[512], the emulated tmem[4096], the palette
            // staging, the tile descriptors -- and tmem_generation, which the header describes as
            // "bumped on every TMEM write so the texture cache can key on content". Pass 1 therefore
            // began from pass 0's END state and generated DIFFERENT cache keys for identical
            // content, selecting different textures for the same draws.
            //
            // Snapshot before the first pass, restore before every later one, so each sub-frame is
            // a pure function of (command buffer, matrices) exactly as it must be. The GPU-side
            // texture cache is deliberately NOT restored: it is content-keyed, so re-executing the
            // same loads hits it rather than re-uploading. ~21 KB memcpy once or twice per tick.
            // REVERTED 2026-08-02. The snapshot/restore below is left here, disabled, as a record of
            // a fix that was wrong. Restoring *mRdp before each replay produced a VISIBLE
            // REGRESSION -- boost/heal plates rendered black and the HUD position digit vanished --
            // because RDP is not self-contained: loaded_texture[] carries raw_tex_metadata with
            // live resource handles, and the emulated tmem/generation pair is what the GPU-side
            // texture cache keys on. Rewinding that half of the pair while the cache itself (which
            // is deliberately NOT restored, and which evicts and frees GPU textures during a pass)
            // moves forward leaves the two describing different worlds, and the replay binds
            // textures that no longer exist.
            //
            // The idea was sound -- every sub-frame should be a pure function of (command buffer,
            // matrices) -- but this is the wrong seam to enforce it at, and it was shipped on a
            // hypothesis that had never been measured. It made things worse for three builds.
#if 0
            if (interp != nullptr && interp->mRdp != nullptr) {
                if (k == 0) {
                    sGdxRdpSnapshot = *interp->mRdp;
                } else {
                    *interp->mRdp = sGdxRdpSnapshot;
                }
            }
#endif
            // Is replaying one tick's display list IDEMPOTENT? Kept after the fix as a regression
            // guard: idem_div must stay at 0 now. Any future change that reintroduces cross-replay
            // state will show up here instead of as a bug report about flicker.
            // flicker appears the instant M > 1 and is clean at M == 1, and phase timing is ruled
            // out (a true 120 Hz panel gives M == 2 exactly, every phase on screen for 16.67ms --
            // identical to interpolation off -- and it still strobes). Geometry cannot differ
            // between replays: the track carries no matrix of its own and the camera is not
            // rerouted in a Release build. So if the picture differs, the difference is STATE.
            // mRdp->loaded_texture survives Run(), and StoreLoadedTexture is path-dependent
            // (interpreter.cpp:4421 erases overlapping entries), so replay 2 begins from replay 1's
            // end-state. Hash what each replay actually binds and compare against pass 0.
            // [interp-shot] Arm the capture for THIS pass. gdx_gfx_post_run_capture (below) runs
            // inside DrawAndRunGraphicsCommands right after Interpreter::Run, which is the only
            // point where the sub-frame's image exists and nothing has presented yet.
            {
                static const long sDumpTick = [] {
                    const char* e = getenv("GDX_INTERP_DUMP_TICK");
                    return (e != nullptr && e[0] != 0) ? strtol(e, nullptr, 10) : -1L;
                }();
                gGdxShotArmedPass = (sDumpTick >= 0 && gGdxIdemMultiPassTicks == (size_t) sDumpTick)
                                        ? k
                                        : -1;
            }
            gdx_gfx_texbind_hash_reset();
            // [interp-geo] Decoupled from the screenshot tick. Pinning the census to one fixed tick
            // made it a lottery: whether a pass renders at all depends on the swapchain limiter, and
            // the first attempt landed on a tick where 2 of 3 passes were refused, so there was no
            // pass-to-pass comparison to make. GDX_INTERP_GEO=<n> logs the census for EVERY pass of
            // every nth multi-pass tick, so a single run yields many comparable pairs and the ones
            // where two or more passes actually rendered can be picked out afterwards.
            static const long sGeoEvery = [] {
                const char* e = getenv("GDX_INTERP_GEO");
                return (e != nullptr && e[0] != 0) ? strtol(e, nullptr, 10) : 0L;
            }();
            const bool gdxGeoDiag =
                gGdxShotArmedPass >= 0 ||
                (sGeoEvery > 0 && (gGdxIdemMultiPassTicks % (size_t) sGeoEvery) == 0);
            // [interp-geo] RSP state this pass INHERITS. Interpreter::Run calls SpReset first, but
            // SpReset resets a specific list (extra_geometry_mode, matrix stack SIZE, branch_z
            // target, viewport z scale/trans, lights, dmem) and geometry_mode is NOT in it --
            // interpreter.cpp:7187-7210. geometry_mode carries G_CULL_BACK/FRONT, G_LIGHTING,
            // G_FOG and G_TEXTURE_GEN, and is read at :3105 for the cull test. So pass 0 inherits it
            // from the PREVIOUS tick's last list while pass 1 inherits it from pass 0's end. If
            // those differ, the two passes cull differently -- and the measured cull counts do
            // differ (1483 vs 1477) while vertices loaded are identical.
            const uint32_t gdxInheritedGeoMode =
                (interp != nullptr && interp->mRsp != nullptr) ? interp->mRsp->geometry_mode : 0u;
            // [interp-geo] The converted command buffer is supposed to be immutable across the M
            // replays -- the same bytes handed to Run() every pass. Hashing it either side of the
            // run tests that directly: if cmd_in differs between pass 0 and pass 1, the interpreter
            // rewrote operands in place during pass 0 and later passes are walking a DIFFERENT
            // display list, which would explain one-shot divergence without any RSP state leak.
            const uint64_t gdxCmdIn = gdxGeoDiag ? adapter.GdxP0HashCommands() : 0ull;
            gdx_perf_sub_begin(GDX_PERF_SUB_RUN);
            const bool delivered = fw->DrawAndRunGraphicsCommands(reinterpret_cast<Gfx*>(converted), {});
            gdx_perf_sub_end(GDX_PERF_SUB_RUN);
            const uint64_t gdxCmdOut = gdxGeoDiag ? adapter.GdxP0HashCommands() : 0ull;

            // [interp-geo] Per-pass geometry census on the dump tick. Owner-confirmed symptom: on
            // replay passes the FLOOR AND CLOUDS ARE NOT DRAWN AT ALL -- not shaded differently,
            // absent. ResetGeometryDiagnostics() already runs per pass, so these counters say WHERE
            // the draws are lost: fewer vertices loaded means the walk stopped reaching them; more
            // cull/clip/invisible means they were reached and thrown away; identical counts with
            // different pixels would put the loss in render state rather than geometry.
            if (gdxGeoDiag && interp != nullptr) {
                const auto& g = interp->GetGeometryDiagnostics();
                // forcet1 is recorded because it is otherwise invisible after the fact: t_last in the
                // [interp-p2] line is the LAST sub-frame's t, which is ~1.0 under ordinary
                // interpolation too, so it cannot distinguish a pinned run from a normal one. Every
                // conclusion about state leakage depends on knowing t was pinned.
                gdx_port_logf("[interp-geo] tick=%lu pass=%d/%d forcet1=%d drawn=%d in_geomode=%08X out_geomode=%08X "
                              "vtx=%llu invalid=%llu tris_sub=%llu clip=%llu "
                              "cull=%llu invis=%llu emitted=%llu "
                              "vhash=%016llX mphash=%016llX cmd_in=%016llX cmd_out=%016llX\n",
                              (unsigned long) gGdxIdemMultiPassTicks, k, passes, sForceT1 ? 1 : 0,
                              delivered ? 1 : 0, gdxInheritedGeoMode,
                              (interp->mRsp != nullptr) ? interp->mRsp->geometry_mode : 0u,
                              (unsigned long long) g.verticesLoaded,
                              (unsigned long long) g.invalidVertices,
                              (unsigned long long) g.trianglesSubmitted,
                              (unsigned long long) g.trianglesClipRejected,
                              (unsigned long long) g.trianglesCullRejected,
                              (unsigned long long) g.trianglesInvisible,
                              (unsigned long long) g.trianglesEmitted,
                              (unsigned long long) g.vertexHash,
                              (unsigned long long) g.mpFirstHash,
                              (unsigned long long) gdxCmdIn,
                              (unsigned long long) gdxCmdOut);
            }

            if (delivered) {
                ++presented;
            } else {
                ++dropped;
            }
            if (k < 8) {
                gdxAttemptOk[k] = delivered;
            }
            // Feed the budget guard's cost estimate. Only DELIVERED passes count: a pass the limiter
            // refused returns without rendering, so timing it would drag the average toward zero and
            // the guard would then wave through passes that cannot possibly fit.
            //
            // 1/8 smoothing -- fast enough to follow a scene change within a few ticks, slow enough
            // that one shader-compile stall does not suppress the next tick's whole burst.
            if (delivered && gdxPassStart > 0.0 && gGdxInterpNowFn != nullptr) {
                const double cost = gGdxInterpNowFn() - gdxPassStart;
                if (cost > 0.0 && cost < 0.5) { // ignore absurd samples (breakpoint, alt-tab, resize)
                    double& ema = (k == 0) ? gdxPass0CostEma : gdxReplayCostEma;
                    ema += (cost - ema) * 0.125;
                }
            }
            lastT = t;
        }
        // Hand pacing back before leaving the burst, so the host's own taskless-VI present and
        // every non-interpolated path keep honouring the limiter exactly as they do today.
        gdx_fast3d_set_subframe_present(0);

        // Publish this burst's duration so the host can subtract it from the tick's total WORK and
        // hand back the reserve (see the block comment on gdxReserve above).
        if (gGdxInterpNowFn != nullptr && gdxLoopStart > 0.0) {
            gGdxInterpLastBurstSec = gGdxInterpNowFn() - gdxLoopStart;
        }
        gdx_perf_sub_begin(GDX_PERF_SUB_POST);
        gdxPostTimerOpen = true;

        // [interp-pace] One line per 120 multi-pass ticks. gN is the wall-clock gap in ms between
        // pass N-1's attempt and pass N's; an 'X' suffix marks a pass the limiter refused. Reading:
        // even gaps with dropped=0 confirms burst-calling was the fault; even gaps with drops still
        // present kills the theory outright and says the limiter is rejecting for another reason;
        // near-zero gaps mean the wait above is not taking effect at all.
        // Gate on the PLANNED count: a tick the pre-sizer shrank from 3 to 1 is precisely the tick
        // this line exists to expose, and gating on the sized count would hide it.
        if (gdxPlannedPasses > 1 && gGdxInterpNowFn != nullptr) {
            static size_t sPaceLogTick = 0;
            if ((++sPaceLogTick % 120u) == 0u) {
                // Gaps are only meaningful between passes that were ACTUALLY ATTEMPTED. When the
                // budget guard breaks out, the remaining gdxAttemptAt[] slots keep their zero
                // initialiser, and differencing those against a real timestamp printed nonsense
                // like g1=-23547.66 -- a seconds-since-epoch value wearing a milliseconds label.
                // -1.0 is this line's established "not applicable" marker; reuse it rather than
                // emit a number that looks like a measurement.
                const auto gap = [&](int i) {
                    if (passes <= i || gdxAttemptAt[i] <= 0.0 || gdxAttemptAt[i - 1] <= 0.0) {
                        return -1.0;
                    }
                    return (gdxAttemptAt[i] - gdxAttemptAt[i - 1]) * 1000.0;
                };
                const double g1 = gap(1);
                const double g2 = gap(2);
                const double g3 = gap(3);
                // budget= is the subset of `dropped` this tick that the budget guard skipped because
                // the tick was already spent, as distinct from presents the limiter refused. The two
                // mean opposite things: limiter drops say presentation is ahead of schedule, budget
                // drops say the tick could not afford the burst and the sim was about to lose time.
                gdx_port_logf("[interp-pace] passes=%d planned=%d window=%.2fms g1=%.2f%s g2=%.2f%s g3=%.2f%s "
                              "presented=%d dropped=%d budget=%d\n",
                              passes, gdxPlannedPasses, gdxPaceWindow * 1000.0,
                              g1, (passes > 1 && !gdxAttemptOk[1]) ? "X" : "",
                              g2, (passes > 2 && !gdxAttemptOk[2]) ? "X" : "",
                              g3, (passes > 3 && !gdxAttemptOk[3]) ? "X" : "",
                              presented, dropped, budgetDropped);
            }
        }

        gGdxInterpPresentedLastTick = true; // host must NOT present again for this tick
        gGdxInterpLastSubframes = presented;
        gGdxInterpLastDropped = dropped;
        gGdxInterpLastT = static_cast<double>(lastT);
        gGdxInterpLastLerped = adapter.GdxP1Lerped();
        gGdxInterpLastVpLerped = adapter.GdxVpLerped();
        gGdxInterpLastVpSnapped = adapter.GdxVpSnapped();
        gGdxInterpLastVtxLerped = adapter.GdxVtxLerped();
        gGdxInterpLastVtxSnapped = adapter.GdxVtxSnapped();
        if (adapter.GdxP1PairMaxDelta() > gGdxInterpPairMaxDelta) {
            gGdxInterpPairMaxDelta = adapter.GdxP1PairMaxDelta();
        }
        gGdxInterpPairSuspect += adapter.GdxP1PairSuspect();
        gGdxInterpPairLerped += adapter.GdxP1Lerped();
        gGdxInterpLastSnapped = adapter.GdxP1SnappedAbsent() + adapter.GdxP1SnappedTeleport() +
                                adapter.GdxP1SnappedCut() + adapter.GdxP1PoolBaseMisses();
        // Rolling presents-per-second meter (real presented FPS, not logic ticks).
        if (gGdxInterpNowFn != nullptr) {
            const double nowSec = gGdxInterpNowFn();
            if (gGdxInterpPresentWindowStart < 0.0) {
                gGdxInterpPresentWindowStart = nowSec;
                gGdxInterpPresentWindowCount = 0;
            }
            gGdxInterpPresentWindowCount += presented;
            const double elapsed = nowSec - gGdxInterpPresentWindowStart;
            if (elapsed >= 0.5) {
                gGdxInterpPresentsPerSec = gGdxInterpPresentWindowCount / elapsed;
                gGdxInterpPresentWindowStart = nowSec;
                gGdxInterpPresentWindowCount = 0;
            }
        }

        // Verify the pools stayed quiescent across the replay window. This can only
        // fire if the structural guarantee (no gdx_dispatch re-entry in the loop) is ever broken.
        if (gdx_interp::PoolParity() != poolParityAtStart) {
            static bool sPoolQuiescenceViolationLogged = false;
            if (!sPoolQuiescenceViolationLogged) {
                sPoolQuiescenceViolationLogged = true;
                gdx_port_logf("[interp-p4] FINDING pool parity changed across sub-frame loop "
                              "(%d -> %d): the GfxPool double-buffer toggled mid-replay — "
                              "quiescence invariant broken; sub-frames may have read torn pools\n",
                              poolParityAtStart, gdx_interp::PoolParity());
            }
        }

        // P4 evidence: on a transition-capture tick the whole frame was forced to snap, so
        // every one of the loop's `count` passes rendered the canonical t=1 content (constant-cadence
        // fix: the pass COUNT stays at M, but all passes are byte-identical un-interpolated frames)
        // and the frame mirror the game samples in Transition_SetBackgroundBuffer (later this same
        // tick) is un-interpolated. Capture snaps are rare (once per screen transition), so this
        // line is never a per-tick spam source.
        if (adapter.GdxCaptureSnapThisTick()) {
            gdx_port_logf("[interp-p4] transition-capture tick: forced canonical t=1 (subframes=%d) "
                          "so the captured background is un-interpolated\n", presented);
        }
    } else {
    const bool p1Active = adapter.GdxP1Enabled();
    const bool p0Active = adapter.GdxP0Enabled() && !p1Active; // P0 evidence only when P1 is off
    const bool interpActive = p0Active || p1Active;
    std::vector<uint64_t> p0Snapshot;
    uint64_t p0Hash0 = 0, p0Hash1 = 0;
    const size_t interpSlots = interpActive ? adapter.GdxP0ScratchSlots() : 0;
    const float pass1T = p1Active ? adapter.GdxInterpPresentT() : 1.0f;
    if (interpActive) {
        // Pass 0: refill scratch at t=1 (== current-pool matrix, byte-identical to stock). In P0
        // this also hashes/snapshots the retained command stream for cross-pass mutation counting.
        adapter.GdxP0RefillScratch(1.0f);
        adapter.GdxVpRefillScratch(1.0f);
        adapter.GdxVtxRefillScratch(1.0f);
        if (p0Active) {
            p0Hash0 = adapter.GdxP0HashCommands();
            adapter.GdxP0SnapshotCommands(p0Snapshot);
            gdx_port_logf("[interp-p0] pass=0 cmdhash=%016llX scratch_slots=%u\n",
                          static_cast<unsigned long long>(p0Hash0), static_cast<unsigned>(interpSlots));
        }
    }

    // Sub-phase: the interpreter Run (F3D command execution -> GPU submission). See gdx_perf.h.
    gdx_perf_sub_begin(GDX_PERF_SUB_RUN);
    interp->Run(reinterpret_cast<Gfx*>(converted), {}); // pass 0 (real render, t=1)
    gdx_perf_sub_end(GDX_PERF_SUB_RUN);

    if (interpActive) {
        // Pass 1 preamble: refill scratch for the presented pass — t=1 in P0 (no-op replay), the
        // configured presentT in P1 (writes lerp(prev,cur,t) into every non-snapped scratch slot).
        adapter.GdxP0RefillScratch(pass1T);
        adapter.GdxVpRefillScratch(pass1T);
        adapter.GdxVtxRefillScratch(pass1T);
        if (p0Active) {
            // An identical hash proves the retained buffer is stable and interp->Run() did NOT
            // mutate it in place; any changed operand is counted as a P0 FINDING (not hidden).
            p0Hash1 = adapter.GdxP0HashCommands();
            const size_t p0Muts = adapter.GdxP0CountMutations(p0Snapshot);
            const size_t p0Viol = adapter.GdxP0TransparencyViolations();
            gdx_port_logf("[interp-p0] pass=1 cmdhash=%016llX scratch_slots=%u\n",
                          static_cast<unsigned long long>(p0Hash1), static_cast<unsigned>(interpSlots));
            if (p0Hash1 != p0Hash0 || p0Muts != 0) {
                gdx_port_logf("[interp-p0] FINDING interpreter mutates retained buffer in place: "
                              "%u operand(s) changed across pass 0 (h0=%016llX h1=%016llX) -> P2 replay "
                              "must snapshot/restore the buffer per pass\n",
                              static_cast<unsigned>(p0Muts),
                              static_cast<unsigned long long>(p0Hash0),
                              static_cast<unsigned long long>(p0Hash1));
            }
            static bool sP0TransparencyLogged = false;
            if (!sP0TransparencyLogged) {
                sP0TransparencyLogged = true;
                gdx_port_logf("[interp-p0] scratch indirection: %u pool matrices rerouted, "
                              "t=1 transparent (violations=%u)\n",
                              static_cast<unsigned>(interpSlots), static_cast<unsigned>(p0Viol));
            }
        }
        if (p1Active) {
            // Per-tick lerp evidence. lerped = slots tweened; snapped_absent = no prev keyframe
            // (spawn/despawn via referenced-set, or unreadable/mismatched pool) ; snapped_teleport
            // = translation-magnitude cut heuristic. t is the presented pass's fraction.
            // Rate-limited: the tick counter advances every tick, but the line is
            // emitted only for the first 8 ticks and then every 120th (~1/2 s at 60 Hz) — mirroring
            // this file's shouldLogDiagnostics cadence — PLUS on any tick where the teleport-snap
            // heuristic fired (a notable cut event), so the log is never spammed at
            // 60 lines/s while still surfacing the steady-state counts and every discontinuity.
            static size_t sInterpP1Tick = 0;
            const size_t tick = sInterpP1Tick++;
            const size_t teleports = adapter.GdxP1SnappedTeleport();
            const size_t cutSnaps = adapter.GdxP1SnappedCut();
            const bool captureSnap = adapter.GdxCaptureSnapThisTick();
            // P3/P4: also surface on any tick a whole-frame cut/pause/capture snap fired,
            // alongside the first-8 / every-120th / teleport cadence.
            if (tick < 8 || (tick % 120) == 0 || teleports != 0 || cutSnaps != 0 || captureSnap) {
                gdx_port_logf("[interp-p1] tick=%zu lerped=%u snapped_absent=%u snapped_teleport=%u "
                              "snapped_cut=%u capture=%d t=%.3f\n",
                              tick,
                              static_cast<unsigned>(adapter.GdxP1Lerped()),
                              static_cast<unsigned>(adapter.GdxP1SnappedAbsent() +
                                                    adapter.GdxP1PoolBaseMisses()),
                              static_cast<unsigned>(teleports),
                              static_cast<unsigned>(cutSnaps),
                              captureSnap ? 1 : 0,
                              static_cast<double>(pass1T));
            }
        }

        // Genuine M=2: re-execute the SAME retained buffer a second time. Safe on the shipping DX11
        // backend: interp->Run() begins with mRapi->StartFrame() + StartDrawToFramebuffer +
        // ClearFramebuffer(true,true) and ends after the MSAA resolve WITHOUT presenting — present
        // is interp->EndFrame() -> mRapi->EndFrame()/SwapBuffers (interpreter.cpp:6884), called ONCE
        // by the host loop. So the second Run clears and redraws mGameFb (no draw-list accumulation,
        // no double present) and the host presents only this final pass — the interpolated midpoint
        // in P1. Presenting BOTH passes for real smoothness requires P2's loop decoupling
        // (main.cpp wrapping each sub-frame in its own StartFrame/EndFrame present bracket).
        // P1 "mid" therefore only demonstrates correct lerp math by presenting the midpoint
        // frame (motion appears half-a-tick behind, uniform).
        // ResetGeometryDiagnostics keeps the downstream [geodiag] counters reflecting a single pass.
        interp->ResetGeometryDiagnostics();
        interp->Run(reinterpret_cast<Gfx*>(converted), {}); // pass 1 (presented: t=1 in P0, presentT in P1)
    }
    } // end else (non-P2 path: default single Run + env-gated P0/P1 in-bridge M=2)

    // A real GFX task produced this host frame — the VI-scanout fallback must
    // not also draw over it (see gdx_vi_present_fallback).
    gHostFrameGfxTaskRan = true;
    // From now on, taskless presents hold this frame (via the mirror) instead
    // of scanning out the CPU VI framebuffer. sGpuHoldPixelsStale marks that
    // the mirror just got fresh content from this task (diag-only signal for
    // GdxDiagHoldTick — see gdx_vi_present_fallback's holdGpuFrame branch).
    sGpuContentLive = true;
    sGpuHoldPixelsStale = true;

    /* Transition_Draw releases its back-arena capture after emitting the last
       textured frame, but conversion and sampling happen here later in the
       same task.  Retire native-RGBA16 ownership only after Run has consumed
       that frame.  Otherwise clearing in Transition_Draw breaks the final
       strip/wipe; never clearing lets the reused arena address misclassify
       ordinary menu textures on subsequent frames. */
    if (!gPendingNativeRgba16RangeClears.empty()) {
        std::sort(gPendingNativeRgba16RangeClears.begin(), gPendingNativeRgba16RangeClears.end());
        gPendingNativeRgba16RangeClears.erase(
            std::unique(gPendingNativeRgba16RangeClears.begin(), gPendingNativeRgba16RangeClears.end()),
            gPendingNativeRgba16RangeClears.end());
        for (uintptr_t begin : gPendingNativeRgba16RangeClears) {
            gNativeRgba16Ranges.erase(
                std::remove_if(gNativeRgba16Ranges.begin(), gNativeRgba16Ranges.end(),
                               [begin](const HostRange& range) { return range.begin == begin; }),
                gNativeRgba16Ranges.end());
            if (gDiagTransitionCaptureBegin == begin) {
                gDiagTransitionCaptureBegin = 0;
                gDiagTransitionCaptureSize = 0;
            }
        }
        gPendingNativeRgba16RangeClears.clear();
    }

    /* Retired-buffer FREE happens AFTER Run: a texture copy that
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
    // Sub-phase: frame-mirror refresh. See gdx_perf.h.
    gdx_perf_sub_begin(GDX_PERF_SUB_MIRROR);
    GdxUpdateFrameMirror(interp);
    gdx_perf_sub_end(GDX_PERF_SUB_MIRROR);

    const Fast::GeometryDiagnostics& geometry = interp->GetGeometryDiagnostics();
    /* Print-budget split: a single global sBigTriPrints<60 cap was
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
    // [geodiag]/[gpustate]/[phasegeom] are per-frame diagnostics: silent unless GDX_DIAG_VERBOSE=1.
    if (gdx_diag_verbose() &&
        (geometryUcodeChanged || (sDiagFrames % 120) == 0 ||
         geometry.invalidVertices != 0 || geometry.variantSwitches != 0)) {
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

    gdx_perf_sub_begin(GDX_PERF_SUB_FBMIRROR);
    for (size_t ti = 0; ti < stats.colorImageTargetCount; ti++) {
        const uintptr_t targetAddress = stats.colorImageTargets[ti];
        auto framebuffer = std::find_if(gN64Framebuffers.begin(), gN64Framebuffers.end(),
                                        [targetAddress](const N64FramebufferInfo& info) {
                                            return info.address == targetAddress;
                                        });
        if (framebuffer == gN64Framebuffers.end()) {
            continue;
        }
        // LAZY, not eager. This used to call ReadFramebufferToCPU here on EVERY task -- a full
        // GPU->CPU readback, which stalls the pipeline until the GPU drains and then drags the
        // pixels back across the bus. Measured at 7.16 ms of a 16.68 ms tick: the entire bridge
        // overhead of the frame, and single-handedly the reason frame interpolation could not
        // afford the sub-frames its target rate asked for (fbmirror=7.16 against bridge=7.18).
        //
        // It is redundant with its only consumer. gdx_read_current_framebuffer already performs
        // this readback on demand, preferring gFrameMirrorFb -- a GPU->GPU copy maintained by
        // GdxUpdateFrameMirror, which costs 0.00 ms because it never touches the CPU -- and falling
        // back to a direct read; it also sets valid and gLastRenderedFramebuffer itself once the
        // pixels are proven non-empty. Transition captures happen once per screen change, so paying
        // a full readback on all sixty ticks per second to have one ready was the wrong trade by
        // roughly three orders of magnitude.
        //
        // What still happens every task is the bookkeeping: the buffer is registered as the render
        // target so the on-demand path knows where to read from. Only the copy is deferred.
        //
        // GDX_EAGER_FBMIRROR=1 restores the per-task readback for A/B without a rebuild. If a
        // transition capture ever comes back empty, set it -- and if that fixes it, the on-demand
        // path is missing a case rather than this deferral being wrong.
        // CAPTURE TICKS STILL READ BACK EAGERLY. Deferring unconditionally regressed the Cup Select
        // transition: the readback also published framebuffer->valid, and a transition capture on
        // the FIRST visit to a screen consumes that flag before anything has set it
        // (gdx_read_current_framebuffer only sets it once it has proven non-empty pixels, which on a
        // first visit has not happened yet). The symptom was the historical "Cup Select squeeze",
        // first entry only -- see the gFrameMirrorFb dimension note in that investigation.
        //
        // GdxTransitionCapturePendingThisTick is the right gate rather than a heuristic: the capture
        // runs LATER IN THE SAME TICK than this loop (sys_gfx.c calls gdx_read_current_framebuffer
        // after the task), which is the ordering that function was written to establish. So the
        // expensive path costs one readback per screen transition instead of one per tick -- the
        // frequency it was always worth paying at.
        const bool gdxCaptureThisTick = GdxTransitionCapturePendingThisTick();
        static const bool sEagerFbMirror = [] {
            const char* e = getenv("GDX_EAGER_FBMIRROR");
            return e != nullptr && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
        }();
        const size_t mirroredBytes =
            static_cast<size_t>(framebuffer->width) * framebuffer->height * sizeof(uint16_t);
        if (sEagerFbMirror || gdxCaptureThisTick) {
            const int hostFramebuffer = interp->mRendersToFb ? interp->mGameFb : 0;
            interp->GetCurrentRenderingAPI()->ReadFramebufferToCPU(
                hostFramebuffer, framebuffer->width, framebuffer->height,
                reinterpret_cast<uint16_t*>(framebuffer->address));
            framebuffer->valid = true;
            RecordHostWrite(framebuffer->address, mirroredBytes);
        }
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

    // Close POST here, at the last STATEMENT, rather than leaving it to the scope guard. The guard
    // is declared before `adapter`, so by C++ destruction order the adapter is torn down FIRST and
    // its cost would land inside POST. Ending here excludes destructors, which makes the summary's
    // (bridge - post) residual read as exactly the teardown cost of this function's locals -- the
    // N64DisplayListAdapter holds the whole converted display list (an unordered_map of vectors,
    // plus the per-matrix scratch records), and it is built and freed once per 60 Hz tick.
    // A residual near zero exonerates teardown; a large one names it.
    gdx_perf_sub_end(GDX_PERF_SUB_FBMIRROR);
    gdx_perf_sub_end(GDX_PERF_SUB_POST);
    gdxPostTimerOpen = false;
}

/* Instrumentation: log what the transition readback actually
   hands the game (dimensions, which source path fed it, offset of the first
   nonzero pixel) and dump the FIRST capture to transition-capture.bmp
   (RGBA5551 -> 24bpp, bottom-up) next to the exe, so the next soak shows
   exactly what the game receives instead of guessing. */
static void LogAndDumpTransitionCapture(const uint16_t* pixels, unsigned int width,
                                        unsigned int height, const char* sourcePath) {
    // Budget split: 8 was consumed entirely by boot-phase
    // captures (VI-fallback frames + the title/logo transitions), so the
    // menu-transition captures a soak actually cares about never got logged.
    // Raised so later, more interesting captures still show up.
    // The one-shot BMP dump is instrumentation. Gate it (and the
    // expensive per-pixel row conversion + file write it needs) behind GDX_DIAG_TRANSITION_DUMP
    // so a normal play session never writes transition-capture.bmp to disk. The cheap log line
    // stays (it only reaches a file when the opt-in log sink is enabled — see port_log.h), and
    // its "dump=" suffix now only advertises the dump when the dump is actually going to happen.
    const bool sDumpEnabled = gdx_dev_gate(GDX_GATE_TRANSITION_DUMP) != 0;
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
    const bool willDump = sDumpEnabled && !sDumped;
    gdx_port_logf("[transition] capture #%d %ux%u source=%s mode=%d firstNonZeroPx=%lld%s\n",
                  sCaptureCount, width, height, sourcePath, (gGameMode & 0x1F), firstNonZero,
                  willDump ? " dump=transition-capture.bmp" : "");

    if (!sDumpEnabled || sDumped) {
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

/* Fade-transition garbled horizontal-stripe band probe.
 *
 * The stock [transition] line only reports firstNonZeroPx, which is nearly useless:
 * `firstNonZero` is initialised to -1 and set to the first index whose pixel != 0, so
 * firstNonZeroPx=0 means "pixel 0 is non-zero" (the capture HAS content), NOT "no
 * non-zero pixels" (that would print -1). It cannot tell a valid title image from a
 * stride-scrambled or wrong-aspect one.
 *
 * This env-gated probe (GDX_DIAG_CAPTURE_PROBE) emits conviction-grade layout evidence
 * on the already-downscaled 320x240 capture so an attract-mode run pinpoints
 * the failure mode WITHOUT guessing:
 *  - source render dims + aspect: the mirror is a resizable FB tracking mCurDimensions,
 *    read back with a nearest-neighbour downscale to 320x240. At a widescreen source
 *    (aspect > ~1.4) that downscale squeezes the full frame into 320 columns -> a
 *    horizontally-compressed (but still recognisable) title image, the leading
 *    "the band IS the title screen" hypothesis. A ~1.333 aspect rules that out.
 *  - shear signature: for the first rows, the column of the first non-zero pixel. A
 *    genuine stride mismatch (rows read at the wrong pitch) makes that column DRIFT by
 *    a roughly constant delta per row -> the diagonal "horizontal dashes" look. A stable
 *    or content-driven column rules stride shear out.
 *  - a rolling checksum: identical across two consecutive captures => the mirror is
 *    STALE (frozen wrong frame), a different failure than a live-but-mislaid capture.
 * Zero cost unless the env var is set (single cached bool). Diagnostic only. */
static void GdxDiagCaptureContentProbe(const uint16_t* px, unsigned int width, unsigned int height, int srcW,
                                       int srcH, const char* sourcePath) {
    if (!gdx_dev_gate(GDX_GATE_CAPTURE_PROBE) || px == nullptr || width == 0 || height == 0) {
        return;
    }
    static int sCount = 0;
    if (sCount >= 48) {
        return;
    }
    ++sCount;

    const size_t pixelCount = static_cast<size_t>(width) * height;
    size_t nonZero = 0;
    uint16_t vMin = 0xFFFF, vMax = 0;
    uint64_t fnv = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < pixelCount; i++) {
        const uint16_t p = px[i];
        fnv = (fnv ^ p) * 0x100000001b3ull;
        if (p != 0) {
            ++nonZero;
            if (p < vMin) vMin = p;
            if (p > vMax) vMax = p;
        }
    }
    if (nonZero == 0) {
        vMin = 0;
    }

    static uint64_t sPrevFnv = 0;
    const bool stale = (sCount > 1 && fnv == sPrevFnv);
    sPrevFnv = fnv;

    // First-non-zero column for the first rows: a constant per-row drift == stride shear.
    char shear[128];
    int off = 0;
    off += std::snprintf(shear + off, sizeof(shear) - off, "firstNZcol[");
    const unsigned probeRows = height < 8 ? height : 8;
    for (unsigned y = 0; y < probeRows; y++) {
        int col = -1;
        const uint16_t* row = px + static_cast<size_t>(y) * width;
        for (unsigned x = 0; x < width; x++) {
            if (row[x] != 0) {
                col = static_cast<int>(x);
                break;
            }
        }
        // snprintf returns the WOULD-HAVE-WRITTEN length; clamp the accumulator so a future
        // larger probeRows/column width can never push `off` past the buffer and feed a
        // wrapped unsigned size into the next call (judge hardening finding).
        off += std::snprintf(shear + off, sizeof(shear) - off, "%s%d", y ? "," : "", col);
        if (off >= static_cast<int>(sizeof(shear)) - 1) {
            off = static_cast<int>(sizeof(shear)) - 1;
            break;
        }
    }
    std::snprintf(shear + off, sizeof(shear) - off, "]");

    const double srcAspect = (srcH > 0) ? static_cast<double>(srcW) / srcH : 0.0;
    const bool wideSrc = srcAspect > 1.4;
    gdx_port_logf("[capture-probe] #%d %ux%u src=%dx%d aspect=%.3f%s nonzero=%.1f%% range=[%u..%u] "
                  "fnv=%016llx%s %s src_path=%s\n",
                  sCount, width, height, srcW, srcH, srcAspect,
                  wideSrc ? "(WIDE:downscale-squeezes-horizontally)" : "",
                  100.0 * static_cast<double>(nonZero) / static_cast<double>(pixelCount), vMin, vMax,
                  static_cast<unsigned long long>(fnv), stale ? " STALE(==prev)" : "", shear, sourcePath);
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
    // Issue C conviction probe: report the source render dims/aspect and a layout
    // fingerprint of the downscaled capture (env-gated, GDX_DIAG_CAPTURE_PROBE). The
    // captured mirror is read from mCurDimensions -> 320x240 nearest-neighbour, so a
    // widescreen source is the prime suspect for the "title-screen band"; the probe's
    // aspect + shear + staleness stats convict the exact failure on the next soak.
    GdxDiagCaptureContentProbe(out, width, height, static_cast<int>(interp->mCurDimensions.width),
                               static_cast<int>(interp->mCurDimensions.height), sourcePath);
    return 1;
}
