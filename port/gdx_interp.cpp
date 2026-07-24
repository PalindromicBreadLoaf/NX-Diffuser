// port/gdx_interp.cpp — R6-P1: matrix frame-interpolation math + per-tick snap state.
// See gdx_interp.h for the architecture context. Everything here is a no-op unless
// GDX_INTERP_P1 is set. Render-only: nothing in this file writes back into game logic.

#include "gdx_interp.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#endif

// gdx_port_logf is a header-only static-inline facility (port/port_log.h pulls only <stdio.h> etc.
// plus a lean <windows.h>), so include it in the .cpp for the P3 cut telemetry. The header
// interface stays free of any port/decomp/LUS dependency (per gdx_interp.h's standalone contract).
#include "port_log.h"

// =============================================================================================
// P3 cut epoch (Step 7 / Step 8). Bumped by the discontinuity sites via gdx_interp_mark_cut*;
// consumed once per rendered tick by gdx_interp::CutPendingForThisTick. Kept at global scope with
// C linkage so the one-line PORT-gated decomp shims reach it without any C++/namespace surface.
// atomic is defensive: today every producer (decomp game fibers) and the single consumer (the gfx
// bridge) run on one cooperatively-scheduled thread, but a relaxed atomic costs nothing and keeps
// the counter well-defined if that ever changes. Never read back into game logic (prime directive).
// =============================================================================================
static std::atomic<std::uint32_t> g_cutEpoch{0};

static void GdxInterpMarkCutImpl(const char* tag) {
    const char* t = (tag != nullptr && tag[0] != '\0') ? tag : "decomp";
    // Epoch bump is UNCONDITIONAL and cheap: it must stay consistent so the consumer's first tick
    // after a toggle-on still snaps. The discontinuity sites call this during normal play whether or
    // not interpolation is enabled.
    const std::uint32_t ep = g_cutEpoch.fetch_add(1, std::memory_order_relaxed) + 1u;

    // Telemetry (spec item 5): the "[interp-p3] cut" line exists so QA can confirm cuts are caught,
    // which only matters while interpolation is on — so gate the LOG (not the bump) on interp being
    // active, matching the [interp-p2] convention and keeping normal-play logs clean. It is further
    // rate-limited so a shim that ends up in a hot path (fires every tick) cannot spam 60 lines/s:
    // always log the first few and whenever the source tag changes (distinct events stay visible),
    // otherwise a heartbeat at most every 60 cuts. Normal play produces a handful of cuts per race.
    if (!gdx_interp::P1().enabled && !gdx_interp::P2HostActive()) {
        return;
    }
    static std::uint32_t sLastLoggedEpoch = 0;
    static char sLastTag[32] = {0};
    const bool firstFew = (ep <= 8u);
    const bool tagChanged = (std::strncmp(sLastTag, t, sizeof(sLastTag)) != 0);
    const bool heartbeat = (ep - sLastLoggedEpoch) >= 60u;
    if (firstFew || tagChanged || heartbeat) {
        sLastLoggedEpoch = ep;
        std::strncpy(sLastTag, t, sizeof(sLastTag) - 1);
        sLastTag[sizeof(sLastTag) - 1] = '\0';
        gdx_port_logf("[interp-p3] cut epoch=%u source=%s\n", static_cast<unsigned>(ep), t);
    }
}

extern "C" void gdx_interp_mark_cut(void) {
    GdxInterpMarkCutImpl(nullptr);
}
extern "C" void gdx_interp_mark_cut_src(const char* tag) {
    GdxInterpMarkCutImpl(tag);
}

namespace gdx_interp {

bool CutPendingForThisTick() {
    static std::uint32_t sLastSeen = 0;
    const std::uint32_t cur = g_cutEpoch.load(std::memory_order_relaxed);
    const bool changed = (cur != sLastSeen);
    sLastSeen = cur;
    return changed;
}


// =============================================================================================
// Windows-aware environment read (consistency fix: matches the GDX_INTERP_P0 idiom in
// port/n64_gfx_bridge.cpp's GdxInterpP0Enabled). std::getenv can miss variables set after CRT
// init on some Windows configurations (the documented rationale lives with the P0 idiom); prefer
// GetEnvironmentVariableA there and fall back to std::getenv elsewhere. `buf` is caller-owned
// storage sized for the longest value this module actually parses ("mid"/"half"/a float string);
// returns nullptr if the variable is unset (or too long to fit `buf` on Windows).
// =============================================================================================
static const char* GdxGetEnvVarWinAware(const char* name, char* buf, size_t bufSize) {
#ifdef _WIN32
    const DWORD n = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(bufSize));
    return (n > 0 && n < bufSize) ? buf : nullptr;
#else
    (void)buf;
    (void)bufSize;
    return std::getenv(name);
#endif
}

// =============================================================================================
// Activation surface — parse GDX_INTERP_P1 once.
// =============================================================================================
static P1Config ParseP1() {
    P1Config c{false, P1Mode::Off, 0.5f};

    char envBuf[64] = {0};
    const char* v = GdxGetEnvVarWinAware("GDX_INTERP_P1", envBuf, sizeof(envBuf));
    if (v == nullptr || v[0] == '\0' || (v[0] == '0' && v[1] == '\0')) {
        return c; // unset / "0" -> Off
    }

    if (std::strcmp(v, "mid") == 0) {
        return P1Config{true, P1Mode::Mid, 0.5f};
    }
    if (std::strcmp(v, "half") == 0) {
        return P1Config{true, P1Mode::Half, 0.5f};
    }

    // Numeric t in (0,1) for experimentation.
    char* end = nullptr;
    const double d = std::strtod(v, &end);
    if (end != v && d > 0.0 && d < 1.0) {
        float t = static_cast<float>(d);
        if (t > 0.999f) {
            t = 0.999f; // never let the presented sub-frame reach live state (Step 2a)
        }
        return P1Config{true, P1Mode::Numeric, t};
    }

    // Any other non-"0" value: enable the poor-man's midpoint demo by default.
    return P1Config{true, P1Mode::Mid, 0.5f};
}

const P1Config& P1() {
    static const P1Config cfg = ParseP1();
    return cfg;
}

// =============================================================================================
// N64 Mtx <-> float. Bit-for-bit libultra guMtxL2F / guMtxF2L on 16 host int32 words.
//   words[0..7]  = integer half (s15.16 high words), words[8..15] = fraction half (low words).
// The pool matrices are host-built (decomp guMtxF2L output, host byte order), so reading the
// 64 bytes as native int32 matches the decomp's own native reads exactly.
// =============================================================================================
static constexpr float kFix32ToF = 1.0f / 65536.0f;
static constexpr float kFToFix32 = 65536.0f;

void MtxToF(const void* mtx, float mf[4][4]) {
    const int32_t* w = reinterpret_cast<const int32_t*>(mtx);
    const uint32_t* intw = reinterpret_cast<const uint32_t*>(w);       // words[0..7]
    const uint32_t* fracw = reinterpret_cast<const uint32_t*>(w + 8);  // words[8..15]

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 2; j++) {
            const int idx = i * 2 + j;
            const uint32_t A = intw[idx];
            const uint32_t F = fracw[idx];
            const uint32_t e1 = (A & 0xFFFF0000u) | ((F >> 16) & 0xFFFFu);
            const uint32_t e2 = ((A << 16) & 0xFFFF0000u) | (F & 0xFFFFu);
            mf[i][j * 2]     = static_cast<float>(static_cast<int32_t>(e1)) * kFix32ToF;
            mf[i][j * 2 + 1] = static_cast<float>(static_cast<int32_t>(e2)) * kFix32ToF;
        }
    }
}

void MtxFromF(const float mf[4][4], void* mtx) {
    uint32_t* intw = reinterpret_cast<uint32_t*>(mtx);        // words[0..7]
    uint32_t* fracw = reinterpret_cast<uint32_t*>(intw + 8);  // words[8..15]

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 2; j++) {
            const int idx = i * 2 + j;
            const int32_t e1 = static_cast<int32_t>(mf[i][j * 2] * kFToFix32);
            const int32_t e2 = static_cast<int32_t>(mf[i][j * 2 + 1] * kFToFix32);
            intw[idx]  = (static_cast<uint32_t>(e1) & 0xFFFF0000u) |
                         ((static_cast<uint32_t>(e2) >> 16) & 0xFFFFu);
            fracw[idx] = ((static_cast<uint32_t>(e1) << 16) & 0xFFFF0000u) |
                         (static_cast<uint32_t>(e2) & 0xFFFFu);
        }
    }
}

// =============================================================================================
// Per-element float lerp (Step 5) — SoH interpolate_mtxf.
// =============================================================================================
void LerpMtx(const void* prev, const void* cur, float t, void* out) {
    float pf[4][4];
    float cf[4][4];
    float of[4][4];
    MtxToF(prev, pf);
    MtxToF(cur, cf);
    const float w = 1.0f - t;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            of[i][j] = w * pf[i][j] + t * cf[i][j];
        }
    }
    MtxFromF(of, out);
}

// =============================================================================================
// Translation-magnitude teleport snap (Correction 2 belt-and-suspenders).
// Threshold in camera-space units per tick. Normal inter-tick motion at 60 Hz is a few tens of
// units; a respawn/cut/teleport is hundreds to thousands. This is intentionally conservative —
// the authoritative cut coverage is P3's gdx_interp_mark_cut(); this only catches gross jumps
// that no event shim has been wired for yet. Tunable.
const float kTeleportThreshold = 2000.0f;

bool TranslationTeleport(const void* prev, const void* cur) {
    float pf[4][4];
    float cf[4][4];
    MtxToF(prev, pf);
    MtxToF(cur, cf);
    // Translation lives in row 3 (guTranslateF writes mf[3][0..2]).
    const float dx = cf[3][0] - pf[3][0];
    const float dy = cf[3][1] - pf[3][1];
    const float dz = cf[3][2] - pf[3][2];
    const float dist2 = dx * dx + dy * dy + dz * dz;
    return dist2 > (kTeleportThreshold * kTeleportThreshold);
}

// =============================================================================================
// Referenced-set tracking (Correction 2 primary). Graphics-thread only; no locking.
// =============================================================================================
static std::unordered_set<uint32_t>& CurSet() {
    static std::unordered_set<uint32_t> s;
    return s;
}
static std::unordered_set<uint32_t>& PrevSet() {
    static std::unordered_set<uint32_t> s;
    return s;
}

void BeginTick() {
    CurSet().clear();
}

bool NoteReferencedOffset(uint32_t offset) {
    const bool wasPresent = (PrevSet().find(offset) != PrevSet().end());
    CurSet().insert(offset);
    return wasPresent;
}

void CommitTick() {
    PrevSet().swap(CurSet());
}

// =============================================================================================
// Dual-pool resolution (Step 2 + Correction 1). All GfxPool-layout knowledge lives here.
// =============================================================================================
extern "C" {
extern unsigned char D_8024DCE0[]; // decomp: GfxPool D_8024DCE0[2]; addressed as raw bytes here
extern int D_800DCCFC;             // decomp: s32 double-buffer parity toggle
}

// GfxPool size selects on EXPANSION_KIT (decomp/include/sys.h). The G-Diffuser exe target and the
// gdiffuser_game object library are both compiled EXPANSION_KIT=1, so this matches the array the
// decomp allocated. PrevPoolBase self-verifies against gSegments[1] and bails on any mismatch, so
// a wrong constant safely disables interpolation instead of corrupting memory.
#ifdef EXPANSION_KIT
static constexpr size_t kGfxPoolSize = 0x36730;
#else
static constexpr size_t kGfxPoolSize = 0x2C6F0;
#endif

// R6-P2 FIELD-DEFECT FIX (2026-07-23): the hand-copied kGfxPoolSize (the N64 struct-comment size
// in decomp/include/sys.h) is the WRONG inter-pool stride on the 64-bit host. sizeof(Gfx) doubles
// there (pointer-width w1), so gfxBuffer[13313] — and therefore the whole GfxPool — is 0x1A008
// bytes larger than the N64 layout (measured: host sizeof = 0x50738 vs const 0x36730). The old code
// treated that mismatch as a hard-disable: PrevPoolBase returned 0 forever, every P1 slot snapped to
// cur (t=1), GdxP1Lerped() stayed 0, and the P2 sub-frame loop went permanently degenerate — one
// t=1 present per tick, i.e. interpolation paid full cost while rendering the disabled path (the
// owner's "no smoothness" report). The array stride between D_8024DCE0[0] and [1] is exactly
// sizeof(GfxPool), so use the decomp TU's ground-truth sizeof as the stride instead of a constant.
// port/decomp_port.c compiles WITH the real GfxPool type, so gdx_gfxpool_sizeof() is authoritative.
extern "C" size_t gdx_gfxpool_sizeof(void);

// The real host inter-pool stride, latched once. Logs the N64-vs-host delta the first time so the
// discrepancy stays visible in the log without disabling the feature. 0 only if the ground-truth
// query fails (can't happen given the linked decomp TU), which PrevPoolBase treats as "no lerp".
static size_t GfxPoolStride() {
    static const size_t stride = [] {
        const size_t real = gdx_gfxpool_sizeof();
        if (real != kGfxPoolSize) {
            gdx_port_logf("[interp] GfxPool host stride 0x%zX (N64 struct-comment size 0x%X) — "
                          "using host sizeof for dual-pool lerp\n",
                          real, static_cast<unsigned>(kGfxPoolSize));
        }
        return real;
    }();
    return stride;
}

uintptr_t PrevPoolBase(uintptr_t curPoolBase) {
    const size_t stride = GfxPoolStride();
    if (stride == 0) {
        return 0; // ground-truth query failed: degrade safely (every slot snaps to cur)
    }
    const int parity = D_800DCCFC & 1;
    const uintptr_t arrayBase = reinterpret_cast<uintptr_t>(&D_8024DCE0[0]);
    const uintptr_t expectedCur = arrayBase + static_cast<uintptr_t>(parity) * stride;
    if (curPoolBase != expectedCur) {
        // gSegments[1] does not match the parity-selected pool: our layout assumption is off
        // (or the pool moved). Defensive: disable the dual-pool lerp for this tick.
        return 0;
    }
    return arrayBase + static_cast<uintptr_t>(parity ^ 1) * stride;
}

int PoolParity() {
    return D_800DCCFC & 1;
}

// =============================================================================================
// P2 activation (R6-P2). CVar bridge declared locally (same minimal-include idiom as
// port/gdx_frame_pacer.c and port/input_bridge.c) so this standalone TU does not pull the LUS
// C++ console-variable header. CVarGetInteger is linked from libultraship in the same target.
// =============================================================================================
extern "C" int CVarGetInteger(const char* name, int defaultValue);

// GDX_INTERP_P2 env override — cached once for the process lifetime (test hook).
static bool P2EnvOverride() {
    static const bool on = [] {
        char envBuf[8] = {0}; // only v[0]/v[1] are inspected -- same idiom as GDX_INTERP_P0's buf
        const char* v = GdxGetEnvVarWinAware("GDX_INTERP_P2", envBuf, sizeof(envBuf));
        return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
    }();
    return on;
}

bool P2HostActive() {
    if (P2EnvOverride()) {
        return true; // forced on for testing regardless of the CVar
    }
    // Live read so the menu toggle (P5) applies on the next tick, exactly like FramePacing.
    return CVarGetInteger("gEnhancements.Graphics.FrameInterpolation", 0) != 0;
}

} // namespace gdx_interp
