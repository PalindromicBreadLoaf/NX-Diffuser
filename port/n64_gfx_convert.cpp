// port/n64_gfx_convert.cpp -- narrow-to-wide display-list boundary converters (see header).

#include "n64_gfx_convert.h"

#include <algorithm>
#include <cstring>

namespace gdx {

namespace {

// F3DEX2 / F3D G_ENDDL opcodes. A converted list always ends on one of these.
constexpr uint8_t kOpEndDlEx2 = 0xDF;
constexpr uint8_t kOpEndDlF3D = 0xB8;

constexpr size_t kReserveCommands = 256;

inline uint32_t Read32LE(const uint8_t* p) {
    // Raw machine word: the bytes are in the source's own byte order and ConvertList swaps
    // afterwards when is_big.
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
        // Legacy F3D overloads these opcodes differently from F3DEX2 -- notably 0x06, which is a
        // sub-DL pointer here and a value word (G_TRI2) there.
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

void ConvertListInto(std::vector<WideGfx>& out, const void* src, size_t max_commands, bool is_big,
                     bool is_f3d, const ConvertContext& ctx, size_t* out_source_commands) {
    out.clear();
    if (out_source_commands != nullptr) {
        *out_source_commands = 0;
    }
    if (src == nullptr) {
        return;
    }
    const size_t walk_limit = std::min(max_commands, kMaxConvertedCommands);
    out.reserve(std::min(walk_limit + 1, kReserveCommands));

    size_t consumed = 0;
    const auto* bytes = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < walk_limit; ++i) {
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
        // Default is the 32-bit token with high32 == 0, so value words and segmented/unresolved
        // pointers both take the draw-time path and behave identically to the narrow list.
        wg.w1 = static_cast<uint64_t>(w1);

        const W1Kind kind = ClassifyW1(op, is_f3d);
        if (kind != W1Kind::Value && !IsSegmentedToken(w1) && ctx.resolve_physical != nullptr) {
            // Only commit a host pointer when it is genuinely > 4 GB: high32 != 0 is the exact
            // signal the bridge uses to take the resolver-free fast path. If the deterministic
            // host address fits in 32 bits (arena mapped low), leave the token and let the
            // draw-time path resolve it to the same address.
            uintptr_t host = 0;
            if (ctx.resolve_physical(ctx.user, w1, /*required_bytes=*/1, &host) &&
                ((static_cast<uint64_t>(host) >> 32) != 0)) {
                wg.w1 = static_cast<uint64_t>(host);
            }
        }

        out.push_back(wg);
        ++consumed;
        if (IsEndDl(op)) {
            break;
        }
    }

    // An unterminated list would run the interpreter off the end of the buffer, so append an
    // F3DEX2 G_ENDDL if the walk did not reach one (empty list, or hit the command cap first).
    if (out.empty() || !IsEndDl(static_cast<uint8_t>(out.back().w0 >> 24))) {
        WideGfx end;
        end.w0 = static_cast<uint32_t>(kOpEndDlEx2) << 24;
        end._pad = 0;
        end.w1 = 0;
        out.push_back(end);
    }

    if (out_source_commands != nullptr) {
        *out_source_commands = consumed;
    }
}

std::vector<WideGfx> ConvertList(const void* src, size_t max_commands, bool is_big,
                                 bool is_f3d, const ConvertContext& ctx,
                                 size_t* out_source_commands) {
    std::vector<WideGfx> out;
    ConvertListInto(out, src, max_commands, is_big, is_f3d, ctx, out_source_commands);
    return out;
}

bool GfxWideCache::SourceMutated(const void* src, const Entry& entry) const {
    if (mCtx.range_changed == nullptr || entry.srcBytes == 0) {
        return false;
    }
    return mCtx.range_changed(mCtx.user, src, entry.srcBytes, entry.writeGen);
}

void GfxWideCache::Build(const void* src, size_t max_commands, bool is_big, bool is_f3d,
                         uint64_t stamp, Entry& entry) {
    entry.writeGen = (mCtx.write_generation != nullptr) ? mCtx.write_generation(mCtx.user) : 0;
    size_t sourceCommands = 0;
    ConvertListInto(entry.cmds, src, max_commands, is_big, is_f3d, mCtx, &sourceCommands);
    entry.srcBytes = sourceCommands * 8;
    entry.stamp = stamp;
    entry.lastUseFrame = mCurrentFrame;
}

const std::vector<WideGfx>& GfxWideCache::GetOrBuild(const void* src, size_t max_commands,
                                                     bool is_big, bool is_f3d, uint64_t stamp) {
    auto it = mCache.find(src);
    if (it != mCache.end()) {
        it->second.lastUseFrame = mCurrentFrame;
        if (it->second.stamp == stamp && !SourceMutated(src, it->second)) {
            return it->second.cmds;
        }
        // Rebuild IN PLACE so the entry's address stays stable for callers holding it.
        Build(src, max_commands, is_big, is_f3d, stamp, it->second);
        return it->second.cmds;
    }

    auto inserted = mCache.emplace(src, Entry{});
    Entry& entry = inserted.first->second;
    Build(src, max_commands, is_big, is_f3d, stamp, entry);
    return entry.cmds;
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
        return 0;  // not worth a full sweep yet
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
