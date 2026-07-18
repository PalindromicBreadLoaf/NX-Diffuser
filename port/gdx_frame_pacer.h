#ifndef GDX_FRAME_PACER_H
#define GDX_FRAME_PACER_H

// Wall-clock frame pacer for the host loop (port/main.cpp).
//
// Call gdx_frame_pacer_tick() exactly ONCE per host-loop iteration, immediately
// after the window EndFrame(). One loop iteration == one VI tick == one 60 Hz
// game frame, so pacing the loop paces the simulation.
//
// Gated on the integer CVar "gEnhancements.Graphics.FramePacing":
//     0 = OFF -> gdx_frame_pacer_tick() is a pure no-op.
//     1 = ON  -> the loop is held to the N64 NTSC field rate
//                (60 / 1.001 ~= 59.94 Hz) with a sleep-to-deadline wait.
//
// DEFAULT IS PLATFORM-SPECIFIC (set via CVarRegisterInteger in port/main.cpp before the
// menu registers the CVar, so a persisted user toggle still wins):
//     Windows : OFF. The DXGI/DX11 backend's own sleep+spin limiter in SwapBuffersBegin
//               reliably holds EndFrame() -- and therefore this loop -- to mTargetFps (60),
//               independent of VSync. Enabling this pacer there only STACKS a second,
//               slightly slower (59.94 Hz) limiter, so it stays opt-in.
//     Linux   : ON. The Fast3D SDL2/OpenGL limiter (SyncFramerateWithTime) sleeps with a
//               *relative* nanosleep() and does NOT retry on EINTR, so a signal can cut the
//               sleep short every frame and the loop free-runs at the panel refresh (e.g.
//               144 Hz on the ROG Ally = ~2.4x game speed). This pacer's absolute-deadline,
//               EINTR-safe wait fixes that regardless of VSync or refresh rate.
//
// See gdx_frame_pacer.c for the full rationale and the VSync interaction. Interpolated
// high-FPS rendering (>60 fps by tweening two sim ticks) is intentionally out of scope.

#ifdef __cplusplus
extern "C" {
#endif

// Pace the host loop to the native field rate when FramePacing is enabled.
// No-op when disabled. Safe to call every frame; not thread-safe (main loop only).
void gdx_frame_pacer_tick(void);

#ifdef __cplusplus
}
#endif

#endif // GDX_FRAME_PACER_H
