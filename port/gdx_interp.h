// port/gdx_interp.h — R6-P1: matrix frame-interpolation math + per-tick snap state.
//
// This is the P1 slice of the Option-C (GfxPool double-buffer lerp) architecture in
// docs/investigation/2026-07-18/MATRIX_INTERPOLATION_PLAN.md (§2 Step 2/2a, §5 rotation,
// §6 Corrections 1 & 2). It is DRIVEN by port/n64_gfx_bridge.cpp's G_MTX scratch-slot
// indirection (the P0 precursor): the bridge reroutes every pool-span modelview matrix into
// a stable scratch slot and, before each sub-frame replay, refills the scratch with
// LerpMtx(prev, cur, t). Nothing here ever writes back into game logic — interpolation is
// render-only (the plan's prime directive).
//
// Everything is a strict no-op unless GDX_INTERP_P1 is set in the environment (default OFF,
// same discipline as P0 / FramePacing). The module is standalone C++ (no decomp/LUS headers
// in the interface) so it compiles into the G-Diffuser executable target beside the bridge.

#ifndef GDX_INTERP_H
#define GDX_INTERP_H

#include <cstddef>
#include <cstdint>

// --- P3: discontinuity cut epoch (Step 7) -------------------------------------------------
// A hard discontinuity — race start/restart, respawn, retire/knockout, a camera hard-cut, or a
// mode/screen transition — makes the PREVIOUS keyframe meaningless for EVERY on-screen entity, so
// the whole frame must SNAP (t=1) for one tick instead of streaking from stale poses. Every such
// site bumps a single process-global epoch through one of these entry points:
//   * gdx_interp_mark_cut(void)          — C linkage with NO string dependency, so the one-line
//                                          PORT-gated decomp shims (racer.c, camera.c, ...) stay
//                                          true one-liners. Logs source="decomp".
//   * gdx_interp_mark_cut_src(tag)       — for port-side hooks that carry a label (e.g. the
//                                          decomp_port.c mode-load hook passes "mode-change").
// The bridge consumes the epoch once per rendered tick (gdx_interp::CutPendingForThisTick) and, on
// a changed epoch, forces every pool matrix that tick to snap. Strictly render-only; nothing here
// ever writes back into game logic (the plan's prime directive).
extern "C" void gdx_interp_mark_cut(void);
extern "C" void gdx_interp_mark_cut_src(const char* tag);

namespace gdx_interp {

// --- Activation surface (Step 6 / Step 10 precursor) --------------------------------------
// P1 ships behind the env gate GDX_INTERP_P1 (the CVar gEnhancements.Graphics.FrameInterpolation
// is reserved for P5's menu wiring; P1's in-bridge deliverable is env-gated like P0). Values:
//   unset / "0"      -> Off (bit-exact stock path)
//   "mid"            -> Mid: the "poor man's 120 Hz" demo — the presented (2nd) pass renders the
//                       t=0.5 midpoint frame (§5 minimal-viable-smoothness milestone).
//   "half"           -> same 0.5 midpoint, distinct label for experimentation.
//   a float in (0,1) -> Numeric: presented pass renders at that fixed t (experimentation).
enum class P1Mode { Off, Mid, Half, Numeric };

struct P1Config {
    bool enabled;   // any P1 mode active (GDX_INTERP_P1 set to a non-"0" value)
    P1Mode mode;
    float presentT; // t used for the presented (2nd) replay pass; 0.5 for Mid/Half
};

// Read GDX_INTERP_P1 exactly once; cached for the process lifetime.
const P1Config& P1();

// --- N64 Mtx <-> float (Step 2 rounding note) ---------------------------------------------
// The pool matrices this module lerps are HOST-built (decomp guMtxF2L output, little-endian
// s15.16 split hi/lo layout — the bridge only reroutes the !isBig pool-span path). MtxToF /
// MtxFromF reproduce libultra guMtxL2F / guMtxF2L bit-for-bit on the raw 16 host int32 words,
// so a MtxToF->MtxFromF round-trip is exact and a t=1 lerp is byte-transparent (P0 invariant).
// `mtx` points at 64 readable bytes.
void MtxToF(const void* mtx, float mf[4][4]);
void MtxFromF(const float mf[4][4], void* mtx);

// --- Per-element float lerp (Step 5: matrix lerp, NOT slerp) -------------------------------
// Mirrors SoH interpolate_mtxf: res[i][j] = w*prev[i][j] + t*cur[i][j], w = 1-t. Whole-matrix
// element lerp is what SoH shipped (decomposed-input interpolation caused rolling artifacts);
// inter-tick rotation deltas at 60 Hz are tiny, and hard cuts are handled by the snap rules
// (referenced-set + teleport) below and by P3's gdx_interp_mark_cut(), not by better math.
// At t=1 the output equals `cur` byte-for-byte. prev/cur/out each point at 64 bytes; out may
// alias neither prev nor cur.
void LerpMtx(const void* prev, const void* cur, float t, void* out);

// --- Snap rule: translation-magnitude teleport (Correction 2 belt-and-suspenders) ---------
// True if the camera-space translation delta between prev and cur exceeds kTeleportThreshold.
// A coarse guard for teleports/cuts of entities present in BOTH ticks that P3's event shims
// have not yet been wired for; normal per-tick motion is far below the threshold.
extern const float kTeleportThreshold;
bool TranslationTeleport(const void* prev, const void* cur);

// --- Snap rule: referenced-set tracking (Correction 2 primary, spawn/despawn) --------------
// The port cannot observe which pool slots game code wrote without instrumenting the decomp,
// so instead we track the set of pool OFFSETS referenced by each tick's display list (the lerp
// list itself is that set). A slot whose offset was NOT referenced last tick has a stale/absent
// previous keyframe -> snap (t=1) for that slot only. This covers spawn/despawn purely
// port-side. Offsets are stable across ticks (the pool layout is fixed and slots are keyed by
// entity id), so the same entity resolves to the same offset each tick.
//
// Lifecycle, once per rendered tick (bracketing all reroutes in one gdx_gfx_run):
//   BeginTick()  -> clears the current-tick offset set.
//   NoteReferencedOffset(off) -> records `off` for this tick; returns true iff it was in the
//                                PREVIOUS tick's set (i.e. a usable prev keyframe exists).
//   CommitTick() -> promotes the current set to "previous" for the next tick.
void BeginTick();
bool NoteReferencedOffset(uint32_t offset);
void CommitTick();

// --- P3 cut consume (Step 7 + Step 8) ------------------------------------------------------
// Returns true iff gdx_interp_mark_cut() / gdx_interp_mark_cut_src() fired since the PREVIOUS
// call. Called EXACTLY ONCE per rendered tick from the bridge's GdxInterpBeginTick, so "changed
// since last call" == "changed since last tick" — the plan's whole-frame snap trigger. Cut sites
// fire during the game-logic half of a dispatch, which precedes that same dispatch's gfx-task
// submission (where BeginTick runs), so a cut is observed on the tick it happens.
bool CutPendingForThisTick();

// --- Dual-pool resolution (Step 2 + Correction 1) -----------------------------------------
// The two GfxPools are D_8024DCE0[2]; the current one is selected by D_800DCCFC parity and its
// host base is what Segment_SetPhysicalAddress(1, gGfxPool) stored in gSegments[1]. The previous
// tick's copy of the same matrix lives at the SAME offset in the sibling pool. Given the current
// pool base (gSegments[1]), return the sibling (previous) pool base, or 0 if the passed base does
// not match the parity-selected pool (defensive: caller then skips the lerp for this tick). This
// keeps all decomp GfxPool-layout knowledge in one place and self-verifies against the real pool
// addresses rather than guessing the stride/direction.
uintptr_t PrevPoolBase(uintptr_t curPoolBase);

// --- P4 (plan §3 edge #1): pool-quiescence guard ------------------------------------------
// The raw GfxPool double-buffer parity (D_800DCCFC & 1). The toggle (D_800DCCFC ^= 1) happens
// ONLY in the NEXT tick's Gfx_InitBuffer, which runs inside gdx_dispatch and therefore CANNOT run
// during the bridge's sub-frame present loop (that loop wraps StartFrame/EndFrame only, never
// gdx_dispatch — edge #7). The bridge latches this before the loop and re-checks it after: an
// unchanged value confirms both pools stayed quiescent across the whole replay window (edge #1's
// defensive assertion). Read-only; keeps the one decomp-global read co-located with PrevPoolBase.
int PoolParity();

// --- P2 activation: host-driven main-loop render/logic decoupling (R6-P2) ------------------
// True when the decoupled sub-frame present loop is active this process. Two sources:
//   * the integer CVar gEnhancements.Graphics.FrameInterpolation != 0 (read LIVE so the menu
//     toggle takes effect between ticks — same idiom as gEnhancements.Graphics.FramePacing), OR
//   * the env override GDX_INTERP_P2 set to a non-"0" value (cached for the process lifetime; a
//     test hook so QA can force the decoupled loop without touching the config).
// When active, port/n64_gfx_bridge.cpp reuses the P1 dual-pool lerp machinery (P2 is OR-ed into
// the adapter's P1-enable) and the host (port/main.cpp) drives M sub-frame presents per one 60 Hz
// logic tick. Strictly render-only; nothing here writes back into game logic (prime directive).
bool P2HostActive();

} // namespace gdx_interp

#endif // GDX_INTERP_H
