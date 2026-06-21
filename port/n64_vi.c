// port/n64_vi.c — R6 piece 3: VI (video interface) bridge.
//
// libultraship's VI stubs (os_vi.cpp) are disabled for this port (os_vi.cpp is filtered out of the
// libultraship build). We provide the VI surface here so the decomp's frame pacing works under the
// cooperative fiber scheduler:
//
//   - osViSwapBuffer(fb) records the framebuffer the game wants shown next.
//   - osViGetCurrent/NextFramebuffer() are used by the decomp in busy-wait loops; they YIELD to the
//     host (gdx_yield) so the host can advance the framebuffer rotation between checks, then return
//     the tracked value. Without the yield those spins would deadlock the single cooperative thread.
//   - osViSetEvent records the queue/message the game wants on each retrace; gdx_vi_tick() (called
//     by the host loop) advances current<-next and posts that retrace message, which wakes the Main
//     scheduler thread.

#include "PR/os_vi.h"
#include "PR/os_message.h"

extern void gdx_yield(void);

static void* sCurrentFb = (void*) 0;
static void* sNextFb = (void*) 0;
static OSMesgQueue* sViQueue = (void*) 0;
static OSMesg sViMsg = (void*) 0;

void osViSwapBuffer(void* fb) {
    sNextFb = fb;
}

void* osViGetCurrentFramebuffer(void) {
    gdx_yield(); // cooperative: let the host advance VI between busy-wait checks
    return sCurrentFb;
}

void* osViGetNextFramebuffer(void) {
    gdx_yield();
    return sNextFb;
}

void osViSetEvent(OSMesgQueue* mq, OSMesg msg, u32 retraceCount) {
    (void) retraceCount;
    sViQueue = mq;
    sViMsg = msg;
}

// --- mode/feature setters: no-ops (libultraship owns the actual window/output). ---
void osCreateViManager(OSPri pri) {
    (void) pri;
}
void osViSetMode(OSViMode* mode) {
    (void) mode;
}
void osViBlack(u8 active) {
    (void) active;
}
void osViSetSpecialFeatures(u32 features) {
    (void) features;
}
void osViSetXScale(f32 scale) {
    (void) scale;
}
void osViSetYScale(f32 scale) {
    (void) scale;
}

// --- host driver: advance the framebuffer rotation and post the VI retrace message. ---
void gdx_vi_tick(void) {
    sCurrentFb = sNextFb;
    if (sViQueue != (void*) 0) {
        osSendMesg(sViQueue, sViMsg, OS_MESG_NOBLOCK);
    }
}
