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

#ifdef __cplusplus
}
#endif

#endif
