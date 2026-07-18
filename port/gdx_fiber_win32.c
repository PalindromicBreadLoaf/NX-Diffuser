/* port/gdx_fiber_win32.c -- Win32 fiber backend for gdx_fiber.h.
 *
 * Thin wrappers over ConvertThreadToFiber / CreateFiber / SwitchToFiber. Behavior
 * is byte-identical to the pre-abstraction n64_sched.c: default stack (stackSize
 * 0 -> CreateFiber's default 1 MB reserve), the same trampoline shape, the same
 * GetCurrentThreadId affinity source.
 */
#include "gdx_fiber.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>

struct GdxFiber {
    void*         handle; /* fiber handle from CreateFiber, or ConvertThreadToFiber */
    GdxFiberEntry entry;
    void*         arg;
};

/* CreateFiber requires a __stdcall LPFIBER_START_ROUTINE. Keep the __stdcall
 * detail contained to this backend: the portable GdxFiberEntry stays cdecl. */
static void __stdcall gdx_fiber_win32_trampoline(void* param) {
    GdxFiber* f = (GdxFiber*) param;
    f->entry(f->arg);
    /* Entry is not expected to return (the decomp threads reschedule through
     * __osCleanupThread). If one ever does, there is no valid context to hand
     * control back to; falling off the end here matches the original code, which
     * likewise had no return path past tf->entry(). */
}

GdxFiber* gdx_fiber_convert_thread(void) {
    GdxFiber* f = (GdxFiber*) calloc(1, sizeof(*f));
    if (f == NULL) {
        return NULL;
    }
    f->handle = ConvertThreadToFiber(NULL);
    if (f->handle == NULL) {
        free(f);
        return NULL;
    }
    return f;
}

GdxFiber* gdx_fiber_create(GdxFiberEntry entry, void* arg, size_t stackSize) {
    GdxFiber* f = (GdxFiber*) calloc(1, sizeof(*f));
    if (f == NULL) {
        return NULL;
    }
    f->entry = entry;
    f->arg = arg;
    /* stackSize 0 -> CreateFiber default reserve (1 MB from the exe header),
     * matching the original CreateFiber(0, ...) call. */
    f->handle = CreateFiber((SIZE_T) stackSize, gdx_fiber_win32_trampoline, f);
    if (f->handle == NULL) {
        free(f);
        return NULL;
    }
    return f;
}

void gdx_fiber_switch(GdxFiber* to) {
    SwitchToFiber(to->handle);
}

unsigned long gdx_fiber_current_thread_id(void) {
    return (unsigned long) GetCurrentThreadId();
}
