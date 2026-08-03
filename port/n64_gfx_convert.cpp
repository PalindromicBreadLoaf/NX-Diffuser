// port/n64_gfx_convert.cpp -- narrow-to-wide display-list boundary converters (see header).

#include "n64_gfx_convert.h"

#include <cstring>

namespace gdx {

namespace {

// F3DEX2 / F3D G_ENDDL opcodes. A converted list always ends on one of these.
constexpr uint8_t kOpEndDlEx2 = 0xDF;
constexpr uint8_t kOpEndDlF3D = 0xB8;

inline uint32_t Read32LE(const uint8_t* p) {
    // The bytes are stored in the source's own byte order; ConvertList swaps
    // afterwards when is_big. Read them as a raw little-endian machine word here.
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline uint32_t Bswap32(uint32_t x) {
    return ((x & 0xFF000000u) >> 24) | ((x & 0x00FF0000u) >> 8) |
           ((x & 0x0000FF00u) << 8) | ((x & 0x000000FFu) << 24);
}

inline bool IsEndDl(uint8_t op) { return (op == kOpEndDlEx2) || (op == kOpEndDlF3D); }

}  // namespace

W1Kind ClassifyW1(uint8_t op, bool isF3d) {
    if (isF3d) {
        // Legacy F3D: opcodes overload differently from F3DEX2. Only the four
        // pointer-carrying cases matter; everything else is a value word.
        switch (op) {
            case 0x01: return W1Kind::DataPtr;   // F3D G_MTX
            case 0x03: return W1Kind::DataPtr;   // F3D G_MOVEMEM
            case 0x04: return W1Kind::DataPtr;   // F3D G_VTX
            case 0x06: return W1Kind::SubDlPtr;  // F3D G_DL
            default:   return W1Kind::Value;
        }
    }
    switch (op) {
        case 0x01: return W1Kind::DataPtr;   // F3DEX2 G_VTX
        case 0xDA: return W1Kind::DataPtr;   // F3DEX2 G_MTX
        case 0xDC: return W1Kind::DataPtr;   // F3DEX2 G_MOVEMEM
        case 0xDE: return W1Kind::SubDlPtr;  // F3DEX2 G_DL
        default:   return W1Kind::Value;
    }
}

std::vector<WideGfx> ConvertList(const void* src, size_t max_commands, bool is_big,
                                 bool is_f3d, const ConvertContext& ctx) {
    std::vector<WideGfx> out;
    if (src == nullptr) {
        return out;
    }
    out.reserve(max_commands + 1);

    const auto* bytes = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < max_commands; ++i) {
        const uint8_t* p = bytes + (i * 8);
        uint32_t w0 = Read32LE(p + 0);
        uint32_t w1 = Read32LE(p + 4);
        if (is_big) {
            w0 = Bswap32(w0);
            w1 = Bswap32(w1);
        }

        const uint8_t op = static_cast<uint8_t>(w0 >> 24);

        WideGfx wg;
        wg.w0 = w0;
        wg._pad = 0;
        // Default: keep the 32-bit token with high32 == 0. Value words route
        // through the draw-time value path; segmented and unresolved pointers
        // route through the draw-time (deterministic) segment/token path -- both
        // behave identically to the original narrow list.
        wg.w1 = static_cast<uint64_t>(w1);

        const W1Kind kind = ClassifyW1(op, is_f3d);
        if (kind != W1Kind::Value && !IsSegmentedToken(w1) && ctx.resolve_physical != nullptr) {
            // Physical (non-segmented) pointer: resolve ONCE against the known
            // load context. Only commit a real host pointer when it is genuinely
            // > 4 GB, which is the exact signal (high32 != 0) the bridge uses to
            // take the resolver-free fast path. If the deterministic host address
            // happens to fit in 32 bits (arena mapped low), leave the token and
            // let the draw-time path resolve it to the same address.
            uintptr_t host = 0;
            if (ctx.resolve_physical(ctx.user, w1, /*required_bytes=*/1, &host) &&
                ((static_cast<uint64_t>(host) >> 32) != 0)) {
                wg.w1 = static_cast<uint64_t>(host);
            }
        }

        out.push_back(wg);
        if (IsEndDl(op)) {
            break;
        }
    }

    // Guarantee termination: an unterminated list would run the interpreter off
    // the end of the buffer. Append an F3DEX2 G_ENDDL if the walk did not reach
    // one (empty list, or hit the command cap first).
    if (out.empty() || !IsEndDl(static_cast<uint8_t>(out.back().w0 >> 24))) {
        WideGfx end;
        end.w0 = static_cast<uint32_t>(kOpEndDlEx2) << 24;
        end._pad = 0;
        end.w1 = 0;
        out.push_back(end);
    }

    return out;
}

const std::vector<WideGfx>& GfxWideCache::GetOrBuild(const void* src, size_t max_commands,
                                                     bool is_big, bool is_f3d, uint64_t stamp) {
    auto it = mCache.find(src);
    if (it != mCache.end()) {
        it->second.lastUseFrame = mCurrentFrame;
        if (it->second.stamp == stamp) {
            return it->second.cmds;  // reuse: same source, same stamp -> same buffer
        }
        // Stale: the backing memory or asset epoch changed. Rebuild in place so
        // the entry's address stays stable for callers holding it.
        it->second.cmds = ConvertList(src, max_commands, is_big, is_f3d, mCtx);
        it->second.stamp = stamp;
        return it->second.cmds;
    }

    Entry entry;
    entry.cmds = ConvertList(src, max_commands, is_big, is_f3d, mCtx);
    entry.stamp = stamp;
    entry.lastUseFrame = mCurrentFrame;
    auto inserted = mCache.emplace(src, std::move(entry));
    return inserted.first->second.cmds;
}

void GfxWideCache::Invalidate(const void* src) {
    mCache.erase(src);
}

void GfxWideCache::Clear() {
    mCache.clear();
}

size_t GfxWideCache::BeginFrame() {
    ++mCurrentFrame;
    if (mCache.size() <= kEvictHighWatermark) {
        return 0;  // below the watermark: not worth a full sweep yet
    }

    size_t evicted = 0;
    for (auto it = mCache.begin(); it != mCache.end(); ) {
        if ((mCurrentFrame - it->second.lastUseFrame) > kStaleFrameLimit) {
            it = mCache.erase(it);
            ++evicted;
        } else {
            ++it;
        }
    }
    return evicted;
}

}  // namespace gdx
