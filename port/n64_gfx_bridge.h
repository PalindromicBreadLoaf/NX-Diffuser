#ifndef GDIFFUSER_N64_GFX_BRIDGE_H
#define GDIFFUSER_N64_GFX_BRIDGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GdxTaskUcode {
    GDX_TASK_UCODE_F3DEX2 = 0,
    GDX_TASK_UCODE_F3DLX2_REJ = 1,
    GDX_TASK_UCODE_F3DFLX2_REJ = 2,
} GdxTaskUcode;

void gdx_gfx_run(void* dl, size_t dlSize, GdxTaskUcode taskUcode);
void gdx_register_n64_framebuffer(void* cpuAddr, unsigned int width, unsigned int height);
void gdx_vi_set_next_framebuffer(void* cpuAddr);
void gdx_vi_set_current_framebuffer(void* cpuAddr);

/* VI-scanout fallback (host-side). Call once per host frame AFTER the game
 * threads have been dispatched and BEFORE the window's EndFrame. If a real GFX
 * task rendered this frame it is a cheap no-op; otherwise it presents the
 * current VI framebuffer's CPU-written pixels (boot logo, etc.) as a single
 * textured quad, preserving N64 "VI scans out whatever is in RDRAM" semantics.
 * See port/n64_gfx_bridge.cpp for the design rationale. */
void gdx_vi_present_fallback(void);

/* ======================================================================================
 * Main-loop render/logic decoupling (frame interpolation) host API.
 *
 * The retained display list + per-tick lerp scratch live only inside gdx_gfx_run (they are
 * freed at its tail and the GfxPool toggles on the next tick), so the M-sub-frame present
 * loop necessarily runs inside gdx_gfx_run. The HOST (port/main.cpp) owns pacing: it supplies
 * a monotonic clock, configures the per-tick schedule, then reads back what was presented.
 * All of this is inert unless gEnhancements.Graphics.FrameInterpolation != 0 (or GDX_INTERP_P2)
 * — see gdx_interp::P2HostActive(). Default OFF is a byte-identical single-pass path.
 * ==================================================================================== */

/* Monotonic wall-clock (seconds) the bridge samples once per sub-frame to derive the
 * interpolation fraction t = clamp((now - tickStart)/tickDuration, 0, 0.999). */
typedef double (*GdxInterpNowFn)(void);
void gdx_gfx_interp_set_now_fn(GdxInterpNowFn fn);

/* 1 if the decoupled loop is active this process (CVar/env). The host reads this ONCE per
 * iteration to choose the interpolation present path over the single-present default path. */
int gdx_gfx_interp_host_active(void);

/* Configure this tick's sub-frame schedule. Called by the host BEFORE gdx_dispatch (the game
 * fiber, and thus gdx_gfx_run, run inside dispatch). active=0 forces the single-pass path.
 * tickStart/tickDuration are in the same clock units as the registered now-fn; maxSubframes
 * caps the loop when VSync is off (presents don't block). Resets the "presented" flag. */
void gdx_gfx_interp_tick_config(int active, double tickStart, double tickDuration, int maxSubframes);

/* 1 if gdx_gfx_run presented the frame(s) itself this tick (a real gfx task ran AND interp was
 * active). When 0 on an interp tick, the host must present once itself (taskless VI fallback). */
int gdx_gfx_interp_presented_last_tick(void);

/* Telemetry for the host's rate-limited [interp-p2] line. */
int gdx_gfx_interp_last_subframes(void);
// Sub-frames the swapchain limiter refused last tick. Non-zero means the tick overran its budget;
// these were previously miscounted as presented, which capped every rate reading at a fiction.
int gdx_gfx_interp_last_dropped(void);
double gdx_gfx_interp_last_t(void);

/* GFX tasks (gdx_gfx_run calls) the previous tick submitted. This is normally 2-6, and that number
 * is load-bearing: gdx_gfx_run runs PER TASK, so any interpolation state that must roll once per
 * TICK cannot be rolled there. The referenced-offset set was rolled per task, which made every task
 * test its offsets against the previous TASK's set and snap instead of lerp. Reported so the
 * distinction stays visible in a log rather than living only in a comment. */
int gdx_gfx_interp_last_tasks(void);

/* [interp-pair] Pairing quality. Slot identity is the GfxPool BYTE OFFSET, which is only valid
 * while the pool layout is stable frame to frame -- and the pool fills in draw-submission order, so
 * a change in the visible set (track-chunk cull, objects entering/leaving view) shifts it and pairs
 * offset N against a DIFFERENT object than last tick. The existing 2000-unit teleport guard cannot
 * see that: normal motion is "a few tens of units" and adjacent track chunks are far closer than
 * 2000 apart. These two report the largest delta among slots that actually paired, and how many
 * paired slots moved further than a tick plausibly can. A fat tail that appears exactly when the
 * camera sweeps is the signature of byte-offset identity being the defect.
 *
 * pair_max is the worst delta since the PREVIOUS telemetry line (read-and-reset); pair_susp and
 * pair_tot are CUMULATIVE since boot, so their ratio is the mispairing rate. They accumulate
 * because the line prints one tick in every 120 -- a per-tick snapshot made a low-rate fault
 * statistically invisible, which is exactly how the first version of this probe wasted a run. */
float gdx_gfx_interp_pair_max_delta(void);
int gdx_gfx_interp_pair_suspect(void);
int gdx_gfx_interp_pair_lerped_total(void);

/* [interp-idem] Ticks whose sub-frame replays bound DIFFERENT textures than pass 0, over ticks that
 * replayed more than once. Non-zero means re-executing one tick's display list is NOT idempotent --
 * the second replay drew with different texture state than the first, which is visible as flicker.
 * Suspected mechanism: mRdp->loaded_texture survives Run(), and StoreLoadedTexture
 * (interpreter.cpp:4421) erases overlapping entries, so replay 2 starts from replay 1's end-state.
 * Zero across a flickering race kills that theory outright. */
int gdx_gfx_interp_idem_divergent(void);
int gdx_gfx_interp_idem_multipass(void);

#ifdef __cplusplus
}
#endif

#endif
