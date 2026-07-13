#ifndef GDX_FRAME_PACER_H
#define GDX_FRAME_PACER_H

// Wall-clock frame pacer for the host loop (port/main.cpp).
//
// Call gdx_frame_pacer_tick() exactly ONCE per host-loop iteration, immediately
// after the window EndFrame(). One loop iteration == one VI tick == one 60 Hz
// game frame, so pacing the loop paces the simulation.
//
// Gated on the integer CVar "gEnhancements.Graphics.FramePacing":
//     0 (default) = OFF -> gdx_frame_pacer_tick() is a pure no-op.
//     1           = ON  -> the loop is held to the N64 NTSC field rate
//                          (60 / 1.001 ~= 59.94 Hz) with a hybrid sleep+spin wait.
//
// The default is OFF ON PURPOSE. libultraship's Fast3D backends (DXGI/DX11 and
// SDL2/OpenGL) already cap the present rate -- and therefore this loop -- to
// mTargetFps (60) via their own hybrid sleep+spin limiter in SwapBuffersBegin,
// independent of VSync. So the game already runs at the correct speed today on a
// high-refresh display; enabling this pacer STACKS a second, slightly slower
// (59.94 Hz) limiter on top. See gdx_frame_pacer.c for the full rationale and the
// VSync interaction. Interpolated high-FPS rendering (>60 fps by tweening two
// sim ticks) is intentionally out of scope for this module.

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
