// port/n64_gfx_convert.h -- narrow-to-wide display-list boundary converters.
//
// Converts a binary N64-format (8-byte) display list into the wide 16-byte
// layout the bridge's wide fast-path consumes, ONCE, and caches the result so
// the per-frame path never re-parses the narrow list nor consults the guessing
// resolver for the pointers this stage already resolved deterministically.
//
// The output packet layout matches decomp's PORT `Gwords` and the bridge's wide
// reader exactly: w0 at byte 0, four bytes of padding, w1 (pointer-width) at
// byte 8, 16 bytes total (see n64_gfx_bridge.cpp kHostBuiltGfxStride /
// sourceIsWide).
//
// This module is deliberately self-contained: it depends only on the standard
// library and an injected ConvertContext of deterministic lookups, so it builds
// and unit-tests as a standalone console exe (gdx_gfx_convert_tests) with no
// bridge, libultraship, or decomp headers.
#ifndef GDX_N64_GFX_CONVERT_H
#define GDX_N64_GFX_CONVERT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace gdx {

// Wide 16-byte display-list packet: w0 @0, 4 bytes pad, w1 @8. Byte-identical to
// decomp PORT `Gwords { u32 w0; GfxW1 w1; }` and the bridge wide reader.
struct WideGfx {
    uint32_t w0;
    uint32_t _pad;
    uint64_t w1;
};
static_assert(sizeof(WideGfx) == 16, "wide Gfx packet must be 16 bytes");
static_assert(offsetof(WideGfx, w0) == 0, "w0 must be at byte 0");
static_assert(offsetof(WideGfx, w1) == 8, "w1 must be at byte 8");

// Deterministic resolution context supplied by the caller. The converter NEVER
// guesses: it asks the context to resolve a physical pointer token against the
// known load context (RDRAM arena / ROM buffer / asset-segment base). This is a
// one-time lookup with full information, not the per-frame low32 reconstruction.
struct ConvertContext {
    // Resolve a NON-segmented pointer token `raw` (a KSEG0/KSEG1 or bare
    // physical RDRAM address) to a real host address. Return true and set
    // *out_host on a deterministic hit; return false to leave the 32-bit token
    // in place (high32 == 0) so the draw-time path still handles it unchanged.
    // Only invoked for pointer-carrying opcodes, never for value words.
    bool (*resolve_physical)(void* user, uint32_t raw, size_t required_bytes, uintptr_t* out_host) = nullptr;
    void* user = nullptr;
};

// How the w1 word of a given opcode is used, per microcode dialect. Anything not
// explicitly a pointer is treated as a VALUE and copied verbatim (high32 == 0) --
// this is what keeps colors / combine modes / vertex indices from ever being
// mistaken for a pointer and corrupted.
enum class W1Kind : uint8_t { Value, DataPtr, SubDlPtr };

// Classify the w1 word for `op` under the F3D (isF3d) or F3DEX2 dialect. Mirrors
// the pointer-carrying cases of the bridge's ProcessList switch.
W1Kind ClassifyW1(uint8_t op, bool isF3d);

// True when `raw`'s top byte is a segment index (0x01..0x0F): the converter
// leaves these as 32-bit values so draw-time segment-table lookup resolves them,
// exactly as the game-emitted wide lists do.
inline bool IsSegmentedToken(uint32_t raw) {
    const uint8_t top = static_cast<uint8_t>(raw >> 24);
    return (top >= 0x01) && (top <= 0x0F);
}

// Stateless conversion of a single narrow list. Walks up to `max_commands`
// 8-byte packets starting at `src`, byteswapping when `is_big`, and stops after
// the first G_ENDDL (F3DEX2 0xDF / F3D 0xB8). The output is always terminated.
std::vector<WideGfx> ConvertList(const void* src, size_t max_commands, bool is_big,
                                 bool is_f3d, const ConvertContext& ctx);

// Cross-frame cache of converted lists keyed by narrow source address, with a
// caller-driven invalidation stamp: a cached entry is reused only when its
// stamp matches the current one, otherwise it is rebuilt. Reference stability of
// std::unordered_map mapped values guarantees the returned buffer address stays
// valid (and identical for the same source) until it is invalidated or evicted.
class GfxWideCache {
  public:
    void SetContext(const ConvertContext& ctx) { mCtx = ctx; }
    const ConvertContext& Context() const { return mCtx; }

    // Get-or-build the wide conversion of the narrow list at `src`. Rebuilds when
    // no entry exists or the stored stamp differs from `stamp`.
    const std::vector<WideGfx>& GetOrBuild(const void* src, size_t max_commands, bool is_big,
                                           bool is_f3d, uint64_t stamp);

    // Drop a single source (e.g. its backing memory was reloaded).
    void Invalidate(const void* src);
    // Drop everything (e.g. a global asset epoch bump).
    void Clear();
    size_t CachedCount() const { return mCache.size(); }
    bool Contains(const void* src) const { return mCache.find(src) != mCache.end(); }

    // Frame-boundary bookkeeping: the bridge calls this once per
    // gdx_gfx_run, before any GetOrBuild calls for that frame. Advances the
    // internal frame counter and, only once the cache has grown past
    // kEvictHighWatermark entries, sweeps it in a single O(size) pass evicting
    // entries whose lastUseFrame is more than kStaleFrameLimit frames behind the
    // current one. Unconditionally safe: GetOrBuild's stamp-mismatch rebuild
    // already handles a source whose backing memory changed, so an evicted (then
    // later revisited) source just rebuilds on its next GetOrBuild instead of
    // returning a dangling reference -- eviction never needs its own
    // invalidation path. Returns the number of entries evicted (0 on most
    // frames) so the caller can log it.
    size_t BeginFrame();

  private:
    struct Entry {
        std::vector<WideGfx> cmds;
        uint64_t stamp = 0;
        uint64_t lastUseFrame = 0;
    };
    static constexpr size_t kEvictHighWatermark = 512;
    static constexpr uint64_t kStaleFrameLimit = 600;

    ConvertContext mCtx{};
    std::unordered_map<const void*, Entry> mCache;
    uint64_t mCurrentFrame = 0;
};

}  // namespace gdx

#endif  // GDX_N64_GFX_CONVERT_H
