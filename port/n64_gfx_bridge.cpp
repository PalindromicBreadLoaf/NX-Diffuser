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
extern "C" {
#include "mio0.h"
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

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
extern "C" uint8_t D_80225800_2[];

extern "C" int gdx_lookup_asset_segment(unsigned int sym_low32,
                                         unsigned char* segment,
                                         unsigned int* rom_base,
                                         unsigned char* compressed,
                                         unsigned int* offset,
                                         unsigned int* image_size);
extern "C" void gdx_fixup_asset_segment_image(unsigned char segment,
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

static inline bool IsLikelyBigEndianDisplayList(const N64Gfx* source, size_t readableLimit) {
    if (readableLimit == 0) return false;
    uint32_t w0 = source[0].w0;
    uint8_t opL = w0 >> 24;
    uint8_t opB = w0 & 0xFF;
    if (opL == 0 && opB != 0) return true;
    if ((opB >= 0xB0 || opB == 0x01 || opB == 0x04) && opL < 0x20) return true;
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
// We NOP this on PC: the z-condition can't be evaluated without RSP vertex state,
// and the sub-DL at w1 is alternate near-clip geometry the interpreter doesn't need.
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

void LogTexturePipelineCommand(const N64Gfx* source, size_t index, size_t limit, size_t stride, uint32_t w0, uint32_t w1,
                               bool isBig) {
    static const bool sTraceEnabled = std::getenv("GDX_GFX_TRACE") != nullptr;
    static int sPipeDiagPrints = 0;
    if (!sTraceEnabled || sPipeDiagPrints >= 10000) {
        return;
    }

    const uint8_t op = Opcode(w0);
    switch (op) {
        case kOpSetTextureImage: {
            const uint32_t fmt = (w0 >> 21) & 0x7;
            const uint32_t siz = (w0 >> 19) & 0x3;
            const uint32_t width = (w0 & 0xFFF) + 1;
            gdx_port_logf("[pipedg] %04zu SETTIMG fmt=%u siz=%u width=%u raw=%08X\n",
                          index, fmt, siz, width, w1);
            sPipeDiagPrints++;
            break;
        }
        case kOpSetTile: {
            const uint32_t fmt = (w0 >> 21) & 0x7;
            const uint32_t siz = (w0 >> 19) & 0x3;
            const uint32_t line = (w0 >> 9) & 0x1FF;
            const uint32_t tmem = w0 & 0x1FF;
            const uint32_t tile = (w1 >> 24) & 0x7;
            const uint32_t palette = (w1 >> 20) & 0xF;
            const uint32_t cmt = (w1 >> 18) & 0x3;
            const uint32_t maskt = (w1 >> 14) & 0xF;
            const uint32_t shiftt = (w1 >> 10) & 0xF;
            const uint32_t cms = (w1 >> 8) & 0x3;
            const uint32_t masks = (w1 >> 4) & 0xF;
            const uint32_t shifts = w1 & 0xF;
            gdx_port_logf("[pipedg] %04zu SETTILE tile=%u fmt=%u siz=%u line=%u tmem=%u pal=%u cms=%u masks=%u shifts=%u cmt=%u maskt=%u shiftt=%u\n",
                          index, tile, fmt, siz, line, tmem, palette, cms, masks, shifts, cmt, maskt, shiftt);
            sPipeDiagPrints++;
            break;
        }
        case kOpLoadBlock: {
            const uint32_t tile = (w1 >> 24) & 0x7;
            const uint32_t uls = (w0 >> 12) & 0xFFF;
            const uint32_t ult = w0 & 0xFFF;
            const uint32_t lrs = (w1 >> 12) & 0xFFF;
            const uint32_t dxt = w1 & 0xFFF;
            gdx_port_logf("[pipedg] %04zu LOADBLOCK tile=%u uls=%u ult=%u lrs=%u dxt=%u\n",
                          index, tile, uls, ult, lrs, dxt);
            sPipeDiagPrints++;
            break;
        }
        case kOpLoadTile: {
            const uint32_t tile = (w1 >> 24) & 0x7;
            const uint32_t uls = (w0 >> 12) & 0xFFF;
            const uint32_t ult = w0 & 0xFFF;
            const uint32_t lrs = (w1 >> 12) & 0xFFF;
            const uint32_t lrt = w1 & 0xFFF;
            gdx_port_logf("[pipedg] %04zu LOADTILE tile=%u uls=%u ult=%u lrs=%u lrt=%u\n",
                          index, tile, uls, ult, lrs, lrt);
            sPipeDiagPrints++;
            break;
        }
        case kOpSetTileSize: {
            const uint32_t tile = (w1 >> 24) & 0x7;
            const uint32_t uls = (w0 >> 12) & 0xFFF;
            const uint32_t ult = w0 & 0xFFF;
            const uint32_t lrs = (w1 >> 12) & 0xFFF;
            const uint32_t lrt = w1 & 0xFFF;
            gdx_port_logf("[pipedg] %04zu TILESIZE tile=%u uls=%u ult=%u lrs=%u lrt=%u\n",
                          index, tile, uls, ult, lrs, lrt);
            sPipeDiagPrints++;
            break;
        }
        case 0xE4:
        case 0xE5: {
            const uint32_t lrx = (w0 >> 12) & 0xFFF;
            const uint32_t lry = w0 & 0xFFF;
            const uint32_t tile = (w1 >> 24) & 0x7;
            const uint32_t ulx = (w1 >> 12) & 0xFFF;
            const uint32_t uly = w1 & 0xFFF;
            uint32_t st0 = 0, st1 = 0, dd0 = 0, dd1 = 0;
            if (index + 2 < limit) {
                N64Gfx st = ReadCommand(source, index + 1, stride, isBig);
                N64Gfx dd = ReadCommand(source, index + 2, stride, isBig);
                st0 = st.w0;
                st1 = st.w1;
                dd0 = dd.w0;
                dd1 = dd.w1;
            }
            gdx_port_logf("[pipedg] %04zu TEXRECT op=%02X tile=%u ul=(%u,%u) lr=(%u,%u) st=(%04X,%04X) dd=(%04X,%04X) rawNext=%08X/%08X rawNext2=%08X/%08X\n",
                          index, op, tile, ulx, uly, lrx, lry,
                          (st1 >> 16) & 0xFFFF, st1 & 0xFFFF,
                          (dd1 >> 16) & 0xFFFF, dd1 & 0xFFFF,
                          st0, st1, dd0, dd1);
            sPipeDiagPrints++;
            break;
        }
        default:
            break;
    }
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
};

struct HostRange {
    uintptr_t begin = 0;
    size_t size = 0;
};

struct PersistentRawTextureCopy {
    uintptr_t source = 0;
    size_t size = 0;
    std::unique_ptr<uint8_t[]> bytes;
    uint64_t dmaGenAtCopy = UINT64_MAX;
};

std::vector<HostRange> gHostRanges;
std::vector<HostRange> gRawN64Ranges;
std::vector<HostRange> gHostN64CommandRanges;
std::vector<HostRange> gF3DAssetRanges;
std::vector<HostRange> gNativeRgba16Ranges;
std::vector<uint8_t> gSetupGfxSegment;
std::vector<PersistentRawTextureCopy> gRawTextureCopies;
std::vector<uintptr_t> gPendingTextureCacheDeletes;
std::vector<std::unique_ptr<uint8_t[]>> gPersistentAllocations; // Fixed undefined mPersistentAllocations

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

bool RdramRangeChanged(uintptr_t source, size_t size, uint64_t sinceGeneration) {
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

alignas(8) const uint8_t kFallbackVertices[64 * 16] = {};

uint32_t Low32(uintptr_t value) {
    return static_cast<uint32_t>(value);
}

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

    if (gdx_lookup_asset_segment(raw, &segment, &romBase, &compressed, &offset, &imageSize) == 0) {
        return false;
    }

    out.segment = segment;
    out.romBase = romBase;
    out.compressed = compressed != 0;
    out.offset = offset;
    out.imageSize = imageSize;
    return true;
}

uintptr_t EnsureAssetSegmentImage(const AssetSegmentLookup& lookup) {
    for (LoadedAssetSegment& loaded : gLoadedAssetSegments) {
        if ((loaded.segment == lookup.segment) &&
            (loaded.romBase == lookup.romBase) &&
            (loaded.compressed == lookup.compressed) &&
            !loaded.bytes.empty()) {
            gSegments[lookup.segment] = reinterpret_cast<uintptr_t>(loaded.bytes.data());
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
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(loaded.bytes.data());
    if (gSegments[lookup.segment] == 0) {
        gSegments[lookup.segment] = base;
    }
    gHostRanges.push_back({ base, loaded.bytes.size() });
    gRawN64Ranges.push_back({ base, loaded.bytes.size() });
    gLoadedAssetSegments.emplace_back(std::move(loaded));
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

uintptr_t FallbackDataPointer(uint8_t op) {
    switch (op) {
        case kOpMtx:
            return reinterpret_cast<uintptr_t>(kFallbackIdentityMtx);
        case kOpMovemem:
            return reinterpret_cast<uintptr_t>(kFallbackViewport);
        case kOpVtx:
            return reinterpret_cast<uintptr_t>(kFallbackVertices);
        default:
            return 0;
    }
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

bool IsRawN64HostPointer(uintptr_t full_addr) {
    if (IsRdramHostPointer(full_addr)) {
        return true;
    }

    for (const HostRange& range : gRawN64Ranges) {
        if ((range.begin == 0) || (range.size == 0)) {
            continue;
        }
        if ((full_addr >= range.begin) && (full_addr < range.begin + range.size)) {
            return true;
        }
    }
    return false;
}

bool IsHostN64CommandPointer(uintptr_t full_addr) {
    for (const HostRange& range : gHostN64CommandRanges) {
        if ((range.begin == 0) || (range.size == 0)) {
            continue;
        }
        if ((full_addr >= range.begin) && (full_addr < range.begin + range.size)) {
            return true;
        }
    }
    return false;
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

bool ResolveRegisteredHostPointer(uint32_t raw, ResolvedAddress& out) {
    for (const HostRange& range : gHostRanges) {
        if ((range.begin == 0) || (range.size == 0)) {
            continue;
        }

        const uint32_t baseLow = Low32(range.begin);
        const uint32_t offset = raw - baseLow;
        if (offset < range.size) {
            out.full = range.begin + offset;
            out.segmented = false;
            return true;
        }
    }
    return false;
}

bool ResolveGeneratedAssetStub(uint32_t raw, ResolvedAddress& out) {
    uint32_t offset = 0;
    const uintptr_t base = EnsureAssetSegmentForSymbol(raw, &offset);
    if (base == 0) {
        return false;
    }

    out.full = base + offset;

    AssetSegmentLookup lookup = {};
    if (LookupAssetSegment(raw, lookup)) {
        out.segment = lookup.segment;
        out.offset = lookup.offset;
        out.segmented = true;
    } else {
        out.offset = offset;
        out.segmented = false;
    }
    return true;
}

bool ResolvePortBssAlias(uint32_t raw, ResolvedAddress& out) {
    /*
     * D_2000000 is the original segment-2 BSS base. LinkStubs can only provide
     * a one-byte symbol token for it, while the actual host storage begins at
     * D_80225800_2. Host-built display lists carry the token directly, so they
     * bypass normal segmented-address resolution and need the same alias here.
     */
    if (raw != Low32(reinterpret_cast<uintptr_t>(D_2000000))) {
        return false;
    }

    out.full = reinterpret_cast<uintptr_t>(D_80225800_2);
    out.segment = 2;
    out.offset = 0;
    out.segmented = true;
    return true;
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

uintptr_t MakeFramebufferToken(uint8_t op, uint32_t raw) {
    const uintptr_t tag = (op == kOpSetDepthImage) ? 0x20000000u : 0x10000000u;
    return (tag | (static_cast<uintptr_t>(raw) & 0x0FFFFFFEu));
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
    auto alloc = std::make_unique<uint8_t[]>(requiredBytes);
    uint8_t* out = alloc.get();
    gPersistentAllocations.push_back(std::move(alloc));
    
    const uint8_t* in = reinterpret_cast<const uint8_t*>(source);
    for (size_t i = 0; i < count; i++) {
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
            if (IsRdramHostPointer(source)) {
                changed = RdramRangeChanged(source, copy.size, copy.dmaGenAtCopy);
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

    bool TryResolveAddress(uint32_t raw, ResolvedAddress& out, size_t requiredBytes = 1, bool preferPhysical = false) const {
        if (raw == 0) {
            return false;
        }

        if (ResolvePortBssAlias(raw, out)) {
            return true;
        }

        if (ResolveGeneratedAssetStub(raw, out)) {
            return true;
        }

        if (ResolveSetupGfxStub(raw, out)) {
            return true;
        }

        if (ResolveRegisteredHostPointer(raw, out)) {
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
            return false; /* out-of-RDRAM KSEG0 (MMIO/cart range) — no match */
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

        /* K0_TO_PHYS() stores only the low 29 bits of host-built pointers in
           commands such as G_MTX.  Extract the window helper early so it can
           run either before or after the segment table depending on the caller.
           Bounds + readability checks prevent small immediates from matching. */
        constexpr uintptr_t kPhysicalAddressMask = 0x1FFFFFFFu;
        const auto tryPhysicalWindow = [&](uintptr_t begin, uintptr_t end) -> bool {
            if ((begin == 0) || (end <= begin)) {
                return false;
            }
            uintptr_t full = (begin & ~kPhysicalAddressMask) | static_cast<uintptr_t>(raw);
            /*
             * A host range can cross a 512 MB K0 physical-token window.
             * In that case the token may belong to the next window even
             * though the range begins in the previous one.
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
            return true;
        };
        const auto tryAllPhysicalWindows = [&]() -> bool {
            if (raw > 0x1FFFFFFFu) return false;
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

        /* When the caller knows this raw value came from K0_TO_PHYS on a host
           pointer (e.g., a G_MTX from host-built F3DEX2 code), try the 512MB
           physical window BEFORE the segment table.  That prevents raw values
           whose top byte matches an active segment index — e.g., 0x0805DAA0
           matching segment 8 — from being misrouted. */
        if (preferPhysical && tryAllPhysicalWindows()) {
            return true;
        }

        if ((encodedSegment < kGfxSegmentCount) && 
            ((gSegments[encodedSegment] != 0) || (encodedSegment == 0)) &&
            ((raw & 0x00FFFFFF) < kSegmentOffsetLimit)) {
            out.full = gSegments[encodedSegment] + encodedOffset;
            out.segment = encodedSegment;
            out.offset = encodedOffset;
            out.segmented = true;
            return true;
        }

        if (!preferPhysical && tryAllPhysicalWindows()) {
            return true;
        }

        bool found = false;
        ResolvedAddress best = {};
        uint32_t bestOffset = UINT32_MAX;

        for (uint8_t segment = 0; segment < kGfxSegmentCount; segment++) {
            const uintptr_t base = gSegments[segment];
            if (base == 0) {
                continue;
            }

            const uint32_t baseLow = static_cast<uint32_t>(base);
            const uint32_t offset = raw - baseLow;
            if ((offset < kSegmentOffsetLimit) && (offset < bestOffset)) {
                best.full = base + offset;
                best.segment = segment;
                best.offset = offset;
                best.segmented = true;
                bestOffset = offset;
                found = true;
            }
        }

        if (found) {
            out = best;
            return true;
        }

        if (mModuleBegin != 0) {
            uintptr_t full = (mModuleBegin & 0xFFFFFFFF00000000ULL) | static_cast<uintptr_t>(raw);
            if (full < mModuleBegin) {
                full += 0x100000000ULL;
            }

            if ((full >= mModuleBegin) && (full < mModuleEnd)) {
                out.full = full;
                out.segmented = false;
                return true;
            }
        }

        if (raw >= 0x10000000) {
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

                const uintptr_t full = high | static_cast<uintptr_t>(raw);
                if (IsReadableAddress(full)) {
                    out.full = full;
                    out.segmented = false;
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
                if (IsReadableAddress(full)) {
                    out.full = full;
                    out.segmented = false;
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

    uintptr_t TranslateDataPointer(uint32_t raw, size_t requiredBytes = 1, bool preferPhysical = false) const {
        if (raw == 0) {
            return 0;
        }

        ResolvedAddress resolved = {};
        if (TryResolveAddress(raw, resolved, requiredBytes, preferPhysical)) {
            return IsReadableAddress(resolved.full) ? resolved.full : 0;
        }

        const uintptr_t direct = static_cast<uintptr_t>(raw);
        return IsReadableAddress(direct) ? direct : 0;
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
        const N64Gfx setImg = ReadCommand(source, index, stride, isBig);
        const uint32_t size = (setImg.w0 >> 19) & 0x3;
        const uint32_t imageWidth = (setImg.w0 & 0xFFF) + 1;
        uint64_t required = kMinRawTextureCopyBytes;

        const size_t scanEnd = std::min(limit, index + 1 + kTextureLoadScanCommandLimit);
        for (size_t i = index + 1; i < scanEnd; i++) {
            const N64Gfx command = ReadCommand(source, i, stride, isBig);
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
        const uintptr_t translated = TranslateDataPointer(raw);
        if (translated == 0) {
            static int sMissingTexturePointerPrints = 0;
            if (sMissingTexturePointerPrints < 200) {
                gdx_port_logf("[texdiag] unresolved G_SETTIMG pointer raw=%08X\n", raw);
                sMissingTexturePointerPrints++;
            }
            return 0;
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

        size_t required = EstimateRawTextureCopyBytes(source, index, limit, stride, isBig);
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

        const N64Gfx setImg = ReadCommand(source, index, stride, isBig);
        const uint32_t extractedFmt = (setImg.w0 >> 21) & 0x7;
        const uint32_t extractedSize = (setImg.w0 >> 19) & 0x3;

        static int sTextureCopyPrints = 0;
        if (sTextureCopyPrints < 16) {
            const uint32_t width = (setImg.w0 & 0xFFF) + 1;
            gdx_port_logf("[texdiag] setimg raw=%08X host=%p fmt=%u siz=%u width=%u copy=%zu readable=%zu\n",
                          raw, reinterpret_cast<const void*>(translated), extractedFmt, extractedSize, width, required, readable);
            sTextureCopyPrints++;
        }

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

    uintptr_t TranslateDisplayListPointer(uint32_t raw, const N64Gfx* parentSource = nullptr, size_t parentIndex = 0) {
        /*
         * Resolve the exact token first. Generated asset symbols are one-byte
         * host stubs and are not necessarily 8-byte aligned; masking them first
         * can collapse several distinct symbols into an unrelated texture.
         *
         * Genuine N64 DL addresses still get the hardware-compatible alignment
         * fallback, but only when the exact candidate is absent or invalid.
         */
        const N64Gfx* target = ResolveDisplayListSource(raw);
        const auto isValidTarget = [this](const N64Gfx* candidate) {
            if (candidate == nullptr) {
                return false;
            }
            const size_t candidateLimit = KnownCommandLimit(candidate);
            return (candidateLimit != 0) && LooksLikeDisplayList(candidate, candidateLimit);
        };

        if (!isValidTarget(target)) {
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
            static int sMissingDlPrints = 0;
            if (sMissingDlPrints < 200) {
                ++sMissingDlPrints;
                const uintptr_t parent = reinterpret_cast<uintptr_t>(parentSource);
                const size_t parentStride = parentSource ? CommandStrideForSource(parentSource) : kN64GfxStride;
                const N64Gfx parentCmd = parentSource ? ReadRawCommand(parentSource, parentIndex, parentStride) : N64Gfx{};
                gdx_port_logf("[gdl-miss] raw=%08X parent=%p index=%zu w0=%08X "
                              "seg0=%p seg1=%p seg2=%p seg3=%p seg8=%p\n",
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
            static int sBadDlPrints = 0;
            if (sBadDlPrints < 200) {
                ++sBadDlPrints;
                const uint32_t alignedRaw = raw & ~7u;
                const N64Gfx* alignedTarget = (alignedRaw != raw) ? ResolveDisplayListSource(alignedRaw) : nullptr;
                const size_t targetStride = CommandStrideForSource(target);
                const N64Gfx first = (limit > 0) ? ReadCommand(target, 0, targetStride, CommandSourceIsBigEndian(target, targetStride)) : N64Gfx{};
                const size_t parentStride = parentSource ? CommandStrideForSource(parentSource) : kN64GfxStride;
                const N64Gfx parentCmd = parentSource ? ReadRawCommand(parentSource, parentIndex, parentStride) : N64Gfx{};
                gdx_port_logf("[gdl-bad] raw=%08X target=%p limit=%zu first=%08X "
                              "alignedRaw=%08X aligned=%p alignedFirst=%08X parent=%p index=%zu w0=%08X\n",
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
        EnqueueList(mRootBegin, RootCommandLimit(mRootBegin));
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

        /* RDRAM/ROM-loaded asset pointers use 8-byte N64 command slots. Some loaded
         * GFX ranges are already fixup-swapped to host endian, while untagged ROM GFX
         * ranges remain big-endian; detect endian from the command stream itself.
         * BSS/module/GfxPool pointers hold host-built data (little-endian).
         * Use the physical location rather than a heuristic to avoid false positives. */
        const size_t stride = CommandStrideForSource(item.source);
        const bool isBig = CommandSourceIsBigEndian(item.source, stride);
        const bool isF3DSource =
            IsF3DAssetPointer(reinterpret_cast<uintptr_t>(item.source)) ||
            DisplayListUsesF3D(item.source, item.limit, stride, isBig);
        if (isF3DSource && mStats != nullptr) {
            mStats->f3dLists++;
        }

        for (size_t i = 0; i < item.limit; i++) {
            N64Gfx in = ReadCommand(item.source, i, stride, isBig);
            const uint8_t op = Opcode(in.w0);
            LogTexturePipelineCommand(item.source, i, item.limit, stride, in.w0, in.w1, isBig);

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

            switch (op) {
                case kOpVtx:
                    // F3D uses opcode 0x01 for G_MTX (not G_VTX). Remap to kOpMtx so Fast3D
                    // doesn't try to load a 64-byte matrix struct as a vertex buffer.
                    if (isF3DSource) {
                        outW0 = (outW0 & 0x00FFFFFFu) | (static_cast<uintptr_t>(kOpMtx) << 24);
                    }
                    outW1 = TranslateDataPointer(in.w1);
                    if (outW1 != 0 && isBig && !isF3DSource) {
                        outW1 = MakePersistentVtxCopy(outW1, (outW0 >> 12) & 0xFF);
                    }
                    if (outW1 != 0) {
                        outW1 = NormalizeLusDirectPointer(outW1);
                    }
                    break;

                case kOpMtx:
                    outW1 = TranslateDataPointer(in.w1, 64, /*preferPhysical=*/!isF3DSource);
                    if ((outW1 & 7u) != 0) {
                        outW1 = 0;
                    }
                    if (outW1 != 0) {
                        outW1 = NormalizeLusDirectPointer(outW1);
                    }
                    break;

                case kOpMovemem:
                    outW1 = TranslateDataPointer(in.w1);
                    if (outW1 != 0) {
                        outW1 = NormalizeLusDirectPointer(outW1);
                    }
                    break;

                case kOpSetColorImage:
                case kOpSetDepthImage:
                    outW1 = TranslateDataPointer(in.w1);
                    if (outW1 == 0) outW1 = MakeFramebufferToken(op, in.w1);
                    break;

                case kOpSetTextureImage:
                    {
                        const uintptr_t translated = TranslateDataPointer(in.w1);
                        // Only emit the O2R filepath opcode for BSS-stub textures (asset-segment
                        // symbols with a 1:1 O2R resource). RDRAM-backed textures are contiguous
                        // multi-tile buffers where the game issues many G_LOADBLOCKs with
                        // increasing ULS offsets across the full region — the O2R resource only
                        // covers the first tile and causes out-of-bounds reads for later bands.
                        // Those textures must go through the raw-copy path (1MB slice of RDRAM).
                        const char* o2rKey = (translated != 0 && !IsRdramHostPointer(translated))
                                                 ? gdx_lookup_asset_segment_o2r_key(in.w1)
                                                 : nullptr;
                        if (o2rKey) {
                            outW0 = (outW0 & 0x00FFFFFFu) | (static_cast<uintptr_t>(kOpSetTextureImageOtrFilepath) << 24);
                            outW1 = reinterpret_cast<uintptr_t>(o2rKey);
                        } else {
                            outW1 = TranslateTexturePointer(in.w1, item.source, i, item.limit, isBig, stride);
                            if (outW1 != 0) {
                                outW1 = NormalizeLusDirectPointer(outW1);
                            }
                        }
                    }
                    break;

                case kOpDl:
                    outW1 = TranslateDisplayListPointer(in.w1, item.source, i);
                    break;

                case kOpMoveword:
                    if (WordParam(in.w0) == kMovewordSegmentIndex) {
                        const uint8_t segIdx = static_cast<uint8_t>((in.w0 & 0xFFFF) / 4);
                        uintptr_t translated = TranslateDataPointer(in.w1);
                        if (translated == 0 && in.w1 == 0 && gdx_rdram != nullptr) {
                            translated = reinterpret_cast<uintptr_t>(gdx_rdram);
                        }
                        if (translated != 0 && segIdx < kGfxSegmentCount) {
                            gSegments[segIdx] = NormalizeLusDirectPointer(translated);
                        }
                        outW1 = (segIdx < kGfxSegmentCount) ? gSegments[segIdx] : static_cast<uintptr_t>(in.w1);
                    }
                    break;

                case kOpBranchZF3D:
                    // F3D G_BRANCH_Z (0xD6): conditional branch on vertex z-depth.
                    // Target DL is in w1. PC can't evaluate the RSP z-condition, so
                    // NOP the whole command — execution continues from the normal list.
                    continue;

                case kOpRdpHalf1:
                    /* G_RDPHALF_1 is also used as raw RDP payload for commands like
                       G_TEXRECT.  Do not blindly treat its w1 as a G_BRANCH_Z display-list
                       pointer just because the following command byte is 0x04; in older F3D
                       GBIs 0x04 is G_VTX, and texture-rectangle payloads can otherwise be
                       corrupted into no-op display-list pointers. */
                    if ((i + 1 < item.limit) &&
                        (Opcode(ReadCommand(item.source, i + 1, stride, isBig).w0) == kOpBranchZ) &&
                        IsResolvableDisplayList(in.w1)) {
                        outW1 = TranslateDisplayListPointer(in.w1, item.source, i);
                    }
                    
                    // Emit 0xB4 for G_RDPHALF_1 in F3DEX2 instead of 0xE1
                    outW0 = (static_cast<uintptr_t>(0xB4) << 24) | (outW0 & 0x00FFFFFFu);
                    break;

                /*
                 * F-Zero X switches between stock F3DEX2 reject variants with
                 * gSPLoadUcodeL. The command's low 24 bits encode the ucode-data
                 * size, not libultraship's UcodeHandlers enum. Passing it through
                 * makes gfx_set_ucode_handler assert on an out-of-range value.
                 * The converted stream remains F3DEX2-compatible, so keep the
                 * current handler and treat the physical ucode load as a no-op.
                 */
                case 0xDD:
                    continue;

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

                // F3D G_TRI1 (0xBF): vertex indices in w0[23:0] → F3DEX2 G_TRI1 has them in w1[31:8].
                // GUARD: F3DEX2 uses 0xBF for G_CULLDL; only convert in F3D asset DLs.
                case 0xBF:
                    if (isF3DSource) {
                        outW0 = static_cast<uintptr_t>(0x05u) << 24;
                        outW1 = static_cast<uintptr_t>(in.w0 & 0x00FFFFFFu) << 8;
                    }
                    break;

                /* F3D G_MOVEMEM (0x03): in F3DEX2, 0x03 = G_CULLDL — cannot pass through.
                   For F3D sources, remap the DMEM target index to F3DEX2 and emit a proper
                   G_MOVEMEM (0xDC).  Only the viewport slot is handled for now; lights/lookat
                   are NOPed until their F3DEX2 DMEM layout is confirmed. */
                case 0x03:
                    if (isF3DSource) {
                        const uint8_t f3dSizeH = static_cast<uint8_t>((in.w0 >> 16) & 0xFFu);
                        const uint8_t f3dIndex = static_cast<uint8_t>((in.w0 >>  8) & 0xFFu);

                        /* F3DEX2 G_MV_VIEWPORT = 8; skip all other DMEM targets for now */
                        if (f3dIndex != 0x80u) {
                            continue;
                        }

                        /* F3D encodes size as sizeof(data)/2; F3DEX2 gDma1p uses the full byte count. */
                        const size_t xferSize = (f3dSizeH > 0u) ? (static_cast<size_t>(f3dSizeH) * 2u) : 16u;

                        uintptr_t addr = TranslateDataPointer(in.w1, xferSize);
                        if (addr == 0) {
                            addr = FallbackDataPointer(kOpMovemem);
                        }
                        if (addr == 0) {
                            if (mStats != nullptr) mStats->skippedDataCommands++;
                            continue;
                        }
                        addr = NormalizeLusDirectPointer(addr);

                        /* F3DEX2 gDma1p: w0 = (cmd<<24)|(p<<16)|l, w1 = data_addr */
                        outW0 = (static_cast<uintptr_t>(kOpMovemem) << 24) |
                                (static_cast<uintptr_t>(8u) << 16) | /* G_MV_VIEWPORT */
                                static_cast<uintptr_t>(xferSize);
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
                            uintptr_t translated = TranslateDataPointer(in.w1);
                            if (translated == 0 && in.w1 == 0 && gdx_rdram != nullptr) {
                                translated = reinterpret_cast<uintptr_t>(gdx_rdram);
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
                        outW1 = TranslateDataPointer(in.w1);
                        if (outW1 != 0) {
                            outW1 = NormalizeLusDirectPointer(outW1);
                        } else {
                            outW1 = FallbackDataPointer(kOpVtx);
                            if (outW1 == 0) {
                                if (mStats != nullptr) mStats->skippedDataCommands++;
                                continue;
                            }
                        }
                        outW0 = (static_cast<uintptr_t>(kOpVtx) << 24) |
                                (static_cast<uintptr_t>(n) << 12) |
                                static_cast<uintptr_t>((v0 + n) * 2u);
                    } else {
                        // F3DEX2 G_BRANCH_Z (0x04)
                        // NOP it to never branch (always draw highest LOD) and avoid infinite recursion
                        outW0 = 0;
                        outW1 = 0;
                    }
                    break;

                default:
                    break;
            }

            if (outW1 == 0 && (op == kOpVtx || op == kOpMtx || op == kOpMovemem || op == kOpSetTextureImage)) {
                outW1 = FallbackDataPointer(op);
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

            if (op == kOpEndDl) return;
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

    gdx_port_logf("[segment] loaded venue=%d segment=10 base=%p symbol=%08X offset=%08X\n",
                  venue, reinterpret_cast<void*>(base), symbol, offset);
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

extern "C" void gdx_gfx_run(void* dl, size_t dl_size) {
    auto wnd = Ship::Context::GetInstance()->GetWindow();
    auto* fw = static_cast<Fast::Fast3dWindow*>(wnd.get());
    if (fw == nullptr) { return; }
    auto interp = fw->GetInterpreterWeak().lock();
    if (!interp) { return; }

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
        gdx_port_logf("[bridge-init] DL root: ptr=%p size=%zu isBig=%d\n",
                      dl, dl_size, static_cast<int>(isBigEndian));
    }

    static uint64_t sDiagFrames = 0;
    const bool shouldLogDiagnostics =
        sDiagFrames < 8 || (sDiagFrames % 120) == 0 ||
        stats.noopDisplayLists != 0 || stats.fallbackDataCommands != 0 ||
        stats.skippedDataCommands != 0 || stats.textureCopyBytes != 0;
    if (shouldLogDiagnostics) {
        gdx_port_logf("[gfxdiag] lists=%zu f3d_lists=%zu cmds=%zu noop_dl=%zu noop_raw=%08X "
                      "miss_dl=%zu miss_raw=%08X bad_dl=%zu bad_raw=%08X "
                      "fallback_data=%zu skip_data=%zu skip_tex=%zu "
                      "tex_copy_bytes=%zu vtx=%zu mtx=%zu dl=%zu teximg=%zu settile=%zu "
                      "tlut=%zu loadblk=%zu loadtile=%zu tilesize=%zu texrect=%zu fillrect=%zu "
                      "setcimg=%zu setzimg=%zu tris=%zu end=%zu size=%zu\n",
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
                      stats.opCounts[kOpEndDl], dl_size);
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

    if (!gPendingTextureCacheDeletes.empty()) {
        std::sort(gPendingTextureCacheDeletes.begin(), gPendingTextureCacheDeletes.end());
        gPendingTextureCacheDeletes.erase(
            std::unique(gPendingTextureCacheDeletes.begin(), gPendingTextureCacheDeletes.end()),
            gPendingTextureCacheDeletes.end());
        for (uintptr_t ptr : gPendingTextureCacheDeletes) {
            interp->TextureCacheDelete(reinterpret_cast<const uint8_t*>(ptr));
        }
        gPendingTextureCacheDeletes.clear();
        gPersistentAllocations.clear();
    }

    interp->Run(reinterpret_cast<Gfx*>(converted), {});
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

    interp->Flush();
    const int framebuffer = interp->mRendersToFb ? interp->mGameFb : 0;
    interp->GetCurrentRenderingAPI()->ReadFramebufferToCPU(
        framebuffer, width, height, static_cast<uint16_t*>(rgba16Buffer));
    return 1;
}
