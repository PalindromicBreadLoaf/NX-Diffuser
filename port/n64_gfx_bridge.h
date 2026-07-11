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

#ifdef __cplusplus
}
#endif

#endif
