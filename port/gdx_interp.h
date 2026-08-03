// port/gdx_interp.h — matrix frame-interpolation math + per-tick snap state.
//
// The architecture is a GfxPool double-buffer lerp, DRIVEN by port/n64_gfx_bridge.cpp's G_MTX
// scratch-slot indirection: the bridge reroutes every pool-span modelview matrix into a stable
// scratch slot and, before each sub-frame replay, refills the scratch with LerpMtx(prev, cur, t).
//
// PRIME DIRECTIVE: interpolation is render-only. Nothing here ever writes back into game logic.
//
// Everything is a strict no-op unless GDX_INTERP_P1 is set in the environment (default OFF,
// same discipline as P0 / FramePacing). The module is standalone C++ (no decomp/LUS headers
// in the interface) so it compiles into the G-Diffuser executable target beside the bridge.

#ifndef GDX_INTERP_H
#define GDX_INTERP_H

#include <cstddef>
#include <cstdint>

// --- P3: discontinuity cut epoch ------------------------------------------------------------
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
// ever writes back into game logic (prime directive).
extern "C" void gdx_interp_mark_cut(void);
extern "C" void gdx_interp_mark_cut_src(const char* tag);

namespace gdx_interp {

// --- Activation surface ----------------------------------------------------------------------
// P1 is the in-bridge, env-gated diagnostic mode, selected by GDX_INTERP_P1. It is separate from
// the shipping user-facing toggle gEnhancements.Graphics.FrameInterpolation, which drives P2's
// host-side sub-frame loop (see P2HostActive below). Values:
//   unset / "0"      -> Off (bit-exact stock path)
//   "mid"            -> Mid: the "poor man's 120 Hz" demo — the presented (2nd) pass renders the
//                       t=0.5 midpoint frame.
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

// --- N64 Mtx <-> float --------------------------------------------------------------------
// The pool matrices this module lerps are HOST-built (decomp guMtxF2L output, little-endian
// s15.16 split hi/lo layout — the bridge only reroutes the !isBig pool-span path). MtxToF /
// MtxFromF reproduce libultra guMtxL2F / guMtxF2L bit-for-bit on the raw 16 host int32 words,
// so a MtxToF->MtxFromF round-trip is exact and a t=1 lerp is byte-transparent (P0 invariant).
// `mtx` points at 64 readable bytes.
void MtxToF(const void* mtx, float mf[4][4]);
void MtxFromF(const float mf[4][4], void* mtx);

// --- Per-element float lerp (matrix lerp, NOT slerp) ---------------------------------------
// Mirrors SoH interpolate_mtxf: res[i][j] = w*prev[i][j] + t*cur[i][j], w = 1-t. Whole-matrix
// element lerp is what SoH shipped (decomposed-input interpolation caused rolling artifacts);
// inter-tick rotation deltas at 60 Hz are tiny, and hard cuts are handled by the snap rules
// (referenced-set + teleport) below and by P3's gdx_interp_mark_cut(), not by better math.
// At t=1 the output equals `cur` byte-for-byte. prev/cur/out each point at 64 bytes; out may
// alias neither prev nor cur.
void LerpMtx(const void* prev, const void* cur, float t, void* out);

// --- Snap rule: translation-magnitude teleport (belt-and-suspenders) ----------------------
// True if the camera-space translation delta between prev and cur exceeds kTeleportThreshold.
// A coarse guard for teleports/cuts of entities present in BOTH ticks that P3's event shims
// have not yet been wired for; normal per-tick motion is far below the threshold.
extern const float kTeleportThreshold;
bool TranslationTeleport(const void* prev, const void* cur);

// --- Pairing-quality measurement ------------------------------------------------------------
// Magnitude of the prev->cur translation delta for a slot that PAIRED (offset referenced in both
// ticks, under the teleport threshold). This is the number that separates a correct pairing from a
// silent mispairing, and nothing measured it before.
//
// Slot identity is the GfxPool BYTE OFFSET (n64_gfx_bridge.cpp GdxP0RerouteMtx:
// offset = origPtr - mCurPoolBase, then prevPtr = mPrevPoolBase + offset). That is only valid while
// the pool layout is stable frame to frame -- but the pool fills in draw-submission order, so when
// the camera turns and the visible set changes (track-chunk cull, objects entering/leaving), offset
// N holds a DIFFERENT logical object than it did last tick and the lerp runs between two unrelated
// transforms. kTeleportThreshold (2000 units) cannot catch that: its own comment puts normal motion
// at "a few tens of units", and adjacent track chunks are far closer together than 2000, so a
// mispaired floor chunk passes the guard silently.
//
// Reported per tick so a camera sweep can be compared against a straight: if the delta population
// grows a fat tail exactly when the view rotates, byte-offset identity is the defect and the fix is
// a stable key (Starship keys on the destination Mtx* instead).
float TranslationDelta(const void* prev, const void* cur);

// --- Snap rule: referenced-set tracking (spawn/despawn) ------------------------------------
// The port cannot observe which pool slots game code wrote without instrumenting the decomp,
// so instead we track the set of pool OFFSETS referenced by each tick's display list (the lerp
// list itself is that set). A slot whose offset was NOT referenced last tick has a stale/absent
// previous keyframe -> snap (t=1) for that slot only. This covers spawn/despawn purely
// port-side. Offsets are stable across ticks (the pool layout is fixed and slots are keyed by
// entity id), so the same entity resolves to the same offset each tick.
//
// Lifecycle, ONCE PER RENDERED TICK -- which is NOT once per gdx_gfx_run. The game submits 2-6
// GFX tasks per 60 Hz tick and gdx_gfx_run executes per task, so bracketing these calls inside it
// compared each task's offsets against the PREVIOUS TASK's set rather than the previous tick's:
// slots that should lerp snapped, and a fully-snapped task tripped the sub-frame loop's
// `degenerate` check into rendering every pass at t=1. Both calls are now driven from the real
// tick boundary (gGdxInterpNewTick in n64_gfx_bridge.cpp, armed by gdx_gfx_interp_tick_config).
//   BeginTick()  -> clears the current-tick offset set.
//   NoteReferencedOffset(off) -> records `off` for this tick; returns true iff it was in the
//                                PREVIOUS tick's set (i.e. a usable prev keyframe exists).
//                                Called by EVERY task in the tick, accumulating into one set.
//   CommitTick() -> promotes the current set to "previous". Invoked at the START of the next tick,
//                   immediately before BeginTick, so nothing has to identify the tick's last task.
void BeginTick();
bool NoteReferencedOffset(uint32_t offset);
void CommitTick();

// --- P3 cut consume ---------------------------------------------------------------------------
// Returns true iff gdx_interp_mark_cut() / gdx_interp_mark_cut_src() fired since the PREVIOUS
// call — a consume-once edge, so WHO calls it and HOW OFTEN is load-bearing. It is called exactly
// once per rendered tick, by the tick's FIRST gfx task, and the result is latched for every later
// task in that tick (gGdxInterpCutThisTick in n64_gfx_bridge.cpp). Calling it per task instead let
// the first task eat the cut while the rest saw false, so half a frame snapped and the other half
// lerped across the cut — the opposite of the whole-frame snap this exists to provide. Cut sites
// fire during the game-logic half of a dispatch, which precedes that same dispatch's gfx-task
// submission (where BeginTick runs), so a cut is observed on the tick it happens.
bool CutPendingForThisTick();

// --- Dual-pool resolution ------------------------------------------------------------------
// The two GfxPools are D_8024DCE0[2]; the current one is selected by D_800DCCFC parity and its
// host base is what Segment_SetPhysicalAddress(1, gGfxPool) stored in gSegments[1]. The previous
// tick's copy of the same matrix lives at the SAME offset in the sibling pool. Given the current
// pool base (gSegments[1]), return the sibling (previous) pool base, or 0 if the passed base does
// not match the parity-selected pool (defensive: caller then skips the lerp for this tick). This
// keeps all decomp GfxPool-layout knowledge in one place and self-verifies against the real pool
// addresses rather than guessing the stride/direction.
uintptr_t PrevPoolBase(uintptr_t curPoolBase);

// --- P4: pool-quiescence guard ---------------------------------------------------------------
// The raw GfxPool double-buffer parity (D_800DCCFC & 1). The toggle (D_800DCCFC ^= 1) happens
// ONLY in the NEXT tick's Gfx_InitBuffer, which runs inside gdx_dispatch and therefore CANNOT run
// during the bridge's sub-frame present loop (that loop wraps StartFrame/EndFrame only, never
// gdx_dispatch). The bridge latches this before the loop and re-checks it after: an unchanged
// value confirms both pools stayed quiescent across the whole replay window. Read-only; keeps the
// one decomp-global read co-located with PrevPoolBase.
int PoolParity();

// --- P2 activation: host-driven main-loop render/logic decoupling -------------------------
// True when the decoupled sub-frame present loop is active this process. Two sources:
//   * the integer CVar gEnhancements.Graphics.FrameInterpolation != 0 (read LIVE so the menu
//     toggle takes effect between ticks — same idiom as gEnhancements.Graphics.FramePacing), OR
//   * the env override GDX_INTERP_P2 set to a non-"0" value (cached for the process lifetime; a
//     test hook to force the decoupled loop without touching the config).
// When active, port/n64_gfx_bridge.cpp reuses the P1 dual-pool lerp machinery (P2 is OR-ed into
// the adapter's P1-enable) and the host (port/main.cpp) drives M sub-frame presents per one 60 Hz
// logic tick. Strictly render-only; nothing here writes back into game logic (prime directive).
bool P2HostActive();

// --- Camera/projection interpolation ------------------------------------------------------
// True when G_MTX_PROJECTION pool matrices should ALSO be rerouted through the interpolation
// scratch. Sources, in precedence order: the env override GDX_INTERP_CAMERA (non-"0", cached for
// the process), then the integer CVar gEnhancements.Graphics.InterpolateCamera (read live so the
// menu toggle applies on the next tick, same idiom as P2HostActive). The Bucket B dev gate of the
// same name remains a dev-build force-on and is OR-ed in by the bridge.
//
// WHY THIS IS ON BY DEFAULT, unlike the other interpolation switches. race.c loads the COMBINED
// projection*view camera with G_MTX_PROJECTION, and course.c emits no gSPMatrix at all -- course
// geometry is world-space vertices viewed through that camera. Excluding projection therefore
// froze BOTH the camera and the entire track at 60 Hz, leaving the racer model matrices as very
// nearly the only thing interpolating: objects were being smoothed against a static world. Since
// camera motion dominates the visual field in a racer, this is where the smoothness actually is.
//
// Lerping the combined matrix is exact, not an approximation. The projection P is identical across
// a tick pair, so lerp(P*V0, P*V1) = P*(V0*(1-t) + V1*t) = P*lerp(V0,V1) -- element-wise lerp of
// the product equals projecting the lerped view. The only inexactness is the linear blend of the
// view rotation, which is the same approximation the racer modelview matrices already use.
bool CameraInterpActive();

} // namespace gdx_interp

#endif // GDX_INTERP_H
