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
 * R6-P2 — main-loop render/logic decoupling (frame interpolation) host API.
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
double gdx_gfx_interp_last_t(void);

#ifdef __cplusplus
}
#endif

#endif
