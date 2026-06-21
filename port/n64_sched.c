// port/n64_sched.c — R6 cooperative fiber scheduler (Option B).
//
// Runs the decomp's REAL N64 threads as cooperative fibers. The decomp's own libultra scheduler
// (startthread.c / recvmesg.c / sendmesg.c / yieldthread.c / stopthread.c) calls the low-level
// primitives below; on N64 these were assembly register context switches — here they are fiber
// switches. One OS thread, cooperative: no real preemption, no cross-thread rendering, no races.
//
// The host (main loop) is itself a fiber. When no game thread is runnable (all blocked on a
// message queue, or only the idle thread remains), control returns to the host fiber so it can
// pump a window frame and post VI/SP/DP events that wake the game threads again.

#include "PR/os_thread.h"
#include "PR/osint.h"
#include "PR/sptask.h"  // OSTask, OSYieldResult, osSpTask* (gfx/audio task submission)
#include "fzx_thread.h" // THREAD_ID_IDLE

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <stddef.h>
#include <stdio.h>

// ---------------------------------------------------------------------------------------------
// Per-thread fiber registry. We capture entry+arg here (with full 64-bit pointers) because the
// decomp's OSThread.context.pc/a0 are u32 and truncate a host function pointer to 32 bits.
// ---------------------------------------------------------------------------------------------
#define GDX_MAX_THREADS 32

typedef struct {
    OSThread* thread;
    void*     fiber;
    void (*entry)(void*);
    void*     arg;
} GdxThreadFiber;

static GdxThreadFiber sThreads[GDX_MAX_THREADS];
static int            sThreadCount = 0;
static void*          sHostFiber = NULL;

static GdxThreadFiber* gdx_find(OSThread* t) {
    int i;
    for (i = 0; i < sThreadCount; i++) {
        if (sThreads[i].thread == t) {
            return &sThreads[i];
        }
    }
    return NULL;
}

// Cooperative interrupt mask: a single OS thread running cooperative fibers has no real
// interrupts to mask. These exist only so the decomp's critical sections compile/run.
u32 __osDisableInt(void) {
    return 0;
}
void __osRestoreInt(u32 mask) {
    (void) mask;
}

// ---------------------------------------------------------------------------------------------
// Priority-ordered thread queues (standard libultra). `queue` is an OSThread**; the head pointer
// is treated as a fake node's `next` field (next is the first member of OSThread).
// ---------------------------------------------------------------------------------------------
void __osEnqueueThread(OSThread** queue, OSThread* t) {
    OSThread* pred = (OSThread*) queue;
    OSThread* succ = pred->next;
    OSPri pri = t->priority;

    while (succ != NULL && succ->priority >= pri) {
        pred = succ;
        succ = succ->next;
    }
    t->next = succ;
    pred->next = t;
    t->queue = queue;
}

OSThread* __osPopThread(OSThread** queue) {
    OSThread* t = *queue;
    *queue = t->next;
    return t;
}

// ---------------------------------------------------------------------------------------------
// Fiber trampoline + context switch.
// ---------------------------------------------------------------------------------------------
#ifdef _WIN32
static void __stdcall gdx_fiber_main(void* param) {
    GdxThreadFiber* tf = (GdxThreadFiber*) param;
    tf->entry(tf->arg);
    __osCleanupThread(); // entry returned -> thread is done
}
#endif

static void* gdx_get_fiber(GdxThreadFiber* tf) {
    if (tf->fiber == NULL) {
#ifdef _WIN32
        tf->fiber = CreateFiber(0, gdx_fiber_main, tf);
#endif
    }
    return tf->fiber;
}

// Run the highest-priority runnable thread. When the run queue is empty, return to the host
// fiber so the main loop can pump a frame and post the events that make threads runnable again.
void __osDispatchThread(void) {
    OSThread* t = __osRunQueue; // head = highest priority runnable

    if (t == (OSThread*) &__osThreadTail) {
        __osRunningThread = NULL;
        SwitchToFiber(sHostFiber);
        return;
    }

    __osPopThread(&__osRunQueue);
    __osRunningThread = t;
    t->state = OS_STATE_RUNNING;

    {
        GdxThreadFiber* tf = gdx_find(t);
        if (tf == NULL) {
            fprintf(stderr, "[sched] FATAL: dispatch unknown thread id=%d\n", (int) t->id);
            fflush(stderr);
            return;
        }
        SwitchToFiber(gdx_get_fiber(tf));
    }
}

// Enqueue the running thread onto `queue` (if any), then switch to the next runnable thread.
void __osEnqueueAndYield(OSThread** queue) {
    if (queue != NULL) {
        __osEnqueueThread(queue, __osRunningThread);
    }
    __osDispatchThread();
}

// A thread's entry function returned: mark it stopped and dispatch away (never comes back).
void __osCleanupThread(void) {
    __osRunningThread->state = OS_STATE_STOPPED;
    __osDispatchThread();
}

// ---------------------------------------------------------------------------------------------
// osCreateThread: our version captures entry+arg with full pointers (the decomp's stores them in
// u32 context fields, which truncates on a 64-bit host). Fibers manage their own stacks, so the
// caller-provided N64 stack is ignored.
// ---------------------------------------------------------------------------------------------
void osCreateThread(OSThread* t, OSId id, void (*entry)(void*), void* arg, void* sp, OSPri p) {
    GdxThreadFiber* tf;
    u32 saveMask;

    (void) sp;

    t->id = id;
    t->priority = p;
    t->next = NULL;
    t->queue = NULL;
    t->state = OS_STATE_STOPPED;
    t->flags = 0;
    t->fp = 0;

    tf = gdx_find(t);
    if (tf == NULL) {
        tf = &sThreads[sThreadCount++];
        tf->thread = t;
        tf->fiber = NULL;
    }
    tf->entry = entry;
    tf->arg = arg;

    saveMask = __osDisableInt();
    t->tlnext = __osActiveQueue;
    __osActiveQueue = t;
    __osRestoreInt(saveMask);
}

// ---------------------------------------------------------------------------------------------
// Host integration.
// ---------------------------------------------------------------------------------------------

// Convert the main OS thread into a fiber so the scheduler can switch to/from it. Call once
// before bootproc().
void gdx_sched_init(void) {
#ifdef _WIN32
    sHostFiber = ConvertThreadToFiber(NULL);
#endif
    fprintf(stderr, "[sched] host fiber ready\n");
    fflush(stderr);
}

// Called by the (port-patched) idle thread's spin: hand the CPU back to the host loop.
void gdx_yield_to_host(void) {
#ifdef _WIN32
    SwitchToFiber(sHostFiber);
#endif
}

// ---------------------------------------------------------------------------------------------
// RDRAM size + SP task submission.
// osMemSize: 4 MB N64 base RAM (US base game; the Expansion Kit / 64DD needs the 8 MB pak).
// The osSpTask* functions are STUBS for now — R6 piece 4 routes the GFX OSTask's display list to
// libultraship's Fast3D (Fast3dWindow::DrawAndRunGraphicsCommands) and posts SP/DP completion.
// ---------------------------------------------------------------------------------------------
u32 osMemSize = 0x400000;

void osSpTaskLoad(OSTask* tp) {
    (void) tp;
}
void osSpTaskStartGo(OSTask* tp) {
    (void) tp; // TODO R6 piece 4: GFX task -> Fast3D
}
void osSpTaskYield(void) {
}
OSYieldResult osSpTaskYielded(OSTask* tp) {
    (void) tp;
    return 0;
}
