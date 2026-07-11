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
#include "PR/os_system.h" // OS_CLOCK_RATE, OS_IM_ALL
#include "PR/sptask.h"     // OSTask, OSYieldResult, osSpTask* (gfx/audio task submission)
#include "PR/ucode.h"
#include "fzx_thread.h"    // THREAD_ID_IDLE
#include "n64_gfx_bridge.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "port_log.h"

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

#ifdef _WIN32
// Phase 3 (port/gdx_audio_thread.cpp): the OS thread ID that called gdx_sched_init() (i.e. the
// one that converted itself to a fiber via ConvertThreadToFiber and therefore owns every fiber
// in sThreads[]/sHostFiber). Win32 fibers are only valid on the thread that created/converted
// them -- SwitchToFiber from any OTHER real OS thread corrupts or crashes. Recorded so
// __osEnqueueAndYield (below) can detect the dedicated audio thread ever reaching a blocking
// osRecvMesg/osSendMesg wait and refuse to touch the fiber scheduler instead of crashing. See
// gdx_audio_thread.cpp's header comment ("A SECOND, SEPARATE HAZARD...") for the full analysis
// of why/when this could theoretically happen and why it is not expected to in practice.
static DWORD sHostThreadId = 0;
#endif

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
    gdx_port_logf("[sched] thread id=%d entry\n", (int) tf->thread->id);
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
            gdx_port_logf("[sched] FATAL: dispatch unknown thread id=%d\n", (int) t->id);
            return;
        }
        SwitchToFiber(gdx_get_fiber(tf));
    }
}

// Enqueue the running thread onto `queue` (if any), then switch to the next runnable thread.
void __osEnqueueAndYield(OSThread** queue) {
#ifdef _WIN32
    // Phase 3 safety net (see sHostThreadId's comment above): a blocking osRecvMesg/osSendMesg
    // wait reached from the dedicated audio thread (port/gdx_audio_thread.cpp) must NOT touch
    // __osRunningThread/__osRunQueue or SwitchToFiber -- both are host-fiber-thread-affine and
    // doing so from here would corrupt or crash the scheduler. Log once (rate-limited) and
    // spin-yield instead: safe, if inefficient, until whatever this call was waiting on shows
    // up (expected in practice -- this port's DMA completions are synchronous, so the queue
    // this call is waiting on should never actually be empty when reached from the audio
    // thread; see gdx_audio_thread.cpp for the full reasoning).
    if (sHostThreadId != 0 && GetCurrentThreadId() != sHostThreadId) {
        static int sForeignYieldLogs = 0;
        if (sForeignYieldLogs < 8) {
            sForeignYieldLogs++;
            gdx_port_logf("[sched] WARNING: __osEnqueueAndYield called from non-host thread id=%lu "
                          "(host=%lu) -- spin-yielding instead of dispatching\n",
                          (unsigned long) GetCurrentThreadId(), (unsigned long) sHostThreadId);
        }
        Sleep(0);
        return;
    }
#endif
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

#ifdef _WIN32
// Last-resort crash logger. Fibers run on the single host OS thread, so a fault inside any
// game/audio fiber otherwise surfaces as a generic Windows "stopped working" dialog (or, in a
// GUI-subsystem build with no console, can look like the process just hangs/vanishes) with no
// indication of WHICH decomp thread or address was involved. Log thread id + fault info before
// falling through to default handling (EXCEPTION_CONTINUE_SEARCH leaves crash behavior itself
// unchanged) so a silent-looking death is diagnosable from gdiffuser-run.log alone.
static LONG WINAPI gdx_crash_vectored_handler(EXCEPTION_POINTERS* info) {
    EXCEPTION_RECORD* rec = info->ExceptionRecord;
    int threadId = __osRunningThread != NULL ? (int) __osRunningThread->id : -1;
    void* faultAddr = NULL;
    int isWrite = -1;

    // First-chance handler sees ALL exceptions, including ones a __try/__except elsewhere would
    // swallow. Only log fault-class codes so this doesn't spam the log on handled exceptions.
    switch (rec->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            break;
        default:
            return EXCEPTION_CONTINUE_SEARCH;
    }

    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        isWrite = (int) rec->ExceptionInformation[0];
        faultAddr = (void*) rec->ExceptionInformation[1];
    }

    gdx_port_logf(
        "[crash] UNHANDLED EXCEPTION code=0x%08X pc=%p thread_id=%d fault_addr=%p access=%s\n",
        (unsigned) rec->ExceptionCode, (void*) rec->ExceptionAddress, threadId, faultAddr,
        (isWrite < 0) ? "n/a" : (isWrite ? "write" : "read"));

    return EXCEPTION_CONTINUE_SEARCH; // do not swallow the fault, only log it
}
#endif

// Convert the main OS thread into a fiber so the scheduler can switch to/from it. Call once
// before bootproc().
void gdx_sched_init(void) {
#ifdef _WIN32
    sHostFiber = ConvertThreadToFiber(NULL);
    sHostThreadId = GetCurrentThreadId();
    AddVectoredExceptionHandler(1 /* call first */, gdx_crash_vectored_handler);
#endif
    gdx_port_logf("[sched] host fiber ready\n");
}

// Called by the (port-patched) idle thread's spin: hand the CPU back to the host loop.
void gdx_yield_to_host(void) {
#ifdef _WIN32
    SwitchToFiber(sHostFiber);
#endif
}

// Diagnostic log helper for decomp .c files that can't include <stdio.h> (libc/stdint.h clash).
// These four are called unconditionally, at high frequency (every VI frame from sys_gfx.c's
// GDX_CK checkpoints and func_800690FC's entry trace; a burst more during every game-mode
// transition from game.c's GMI_* checkpoints) -- see the comment on gdx_trace_enabled() in
// port_log.h for why they're gated off by default (GDX_TRACE=1 re-enables them).
void gdx_ck(const char* s) {
    if (!gdx_trace_enabled()) return;
    gdx_port_logf("%s\n", s);
}

void gdx_cki(const char* s, int v) {
    if (!gdx_trace_enabled()) return;
    gdx_port_logf("%s=%d (0x%x)\n", s, v, (unsigned) v);
}

void gdx_ckp(const char* s, void* p) {
    if (!gdx_trace_enabled()) return;
    gdx_port_logf("%s=%p\n", s, p);
}

/* GDX_DIAG_VERBOSE gate for the per-frame diagnostic log families
   ([gfxdiag], [game], [seg], [sched], [phasegeom], [bigtri]). Cached like
   gdx_trace_enabled(); silent (0) by default, enabled by GDX_DIAG_VERBOSE set
   to any non-"0" value. */
int gdx_diag_verbose(void) {
    static int sCached = -1;
    if (sCached < 0) {
        const char* env = getenv("GDX_DIAG_VERBOSE");
        sCached = (env != NULL && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }
    return sCached;
}

void gdx_seg_log(const char* kind, int seg, uintptr_t raw, void* resolved) {
    if (!gdx_trace_enabled()) return;
    if (!gdx_diag_verbose()) return;
    gdx_port_logf("[seg] %s seg=%d raw=%p raw32=%08X resolved=%p\n",
                  kind, seg, (void*)raw, (unsigned)(raw & 0xFFFFFFFFu), resolved);
}

void gdx_addr_log(const char* kind, uintptr_t raw, void* resolved) {
    if (!gdx_trace_enabled()) return;
    gdx_port_logf("[addr-resolve] %s raw=%p raw32=%08X resolved=%p\n",
                  kind, (void*)raw, (unsigned)(raw & 0xFFFFFFFFu), resolved);
}

// ---------------------------------------------------------------------------------------------
// [transition] timing probe. Game-mode transitions (title->menu, menu->race, race->results, ...)
// run decomp/src/game/game.c's GAMEMODE_CHANGE_INIT case synchronously on the GAME thread:
// Segment_LoadOverlays() + the mode's init function + Segment_LoadAssets(), all in one state-
// machine tick with no host frame submitted in between (see game.c's GMI_A..GMI_F checkpoints).
// That whole tick, and Segment_LoadAssets() specifically, are exactly the work that shows up as
// the visible freeze. Always-on (not gated by GDX_TRACE): a transition happens at most a few
// times a minute, so the log volume is negligible, and this is the number the user needs to
// quantify each transition without having to flip on the full per-frame trace.
#define GDX_TIMER_SLOTS 4
static const char* sGdxTimerLabel[GDX_TIMER_SLOTS];
#ifdef _WIN32
static LARGE_INTEGER sGdxTimerStart[GDX_TIMER_SLOTS];
static LARGE_INTEGER sGdxTimerFreq;
static int sGdxTimerFreqInit = 0;
#endif

static int gdx_timer_slot_for(const char* label) {
    int i;
    int freeSlot = -1;
    for (i = 0; i < GDX_TIMER_SLOTS; i++) {
        if (sGdxTimerLabel[i] != NULL && strcmp(sGdxTimerLabel[i], label) == 0) {
            return i;
        }
        if (sGdxTimerLabel[i] == NULL && freeSlot < 0) {
            freeSlot = i;
        }
    }
    if (freeSlot < 0) {
        freeSlot = 0; // slots exhausted: reuse slot 0 rather than crash
    }
    sGdxTimerLabel[freeSlot] = label;
    return freeSlot;
}

void gdx_transition_timer_begin(const char* label) {
#ifdef _WIN32
    int slot = gdx_timer_slot_for(label);
    if (!sGdxTimerFreqInit) {
        QueryPerformanceFrequency(&sGdxTimerFreq);
        sGdxTimerFreqInit = 1;
    }
    QueryPerformanceCounter(&sGdxTimerStart[slot]);
#else
    (void)label;
#endif
}

void gdx_transition_timer_end(const char* label) {
#ifdef _WIN32
    LARGE_INTEGER end;
    int slot = gdx_timer_slot_for(label);
    double ms;
    if (!sGdxTimerFreqInit || sGdxTimerFreq.QuadPart == 0) {
        return;
    }
    QueryPerformanceCounter(&end);
    ms = (double)(end.QuadPart - sGdxTimerStart[slot].QuadPart) * 1000.0 / (double)sGdxTimerFreq.QuadPart;
    gdx_port_logf("[transition] %s took %.2fms\n", label, ms);
#else
    (void)label;
#endif
}

// ---------------------------------------------------------------------------------------------
// [transition] per-step breakdown within mode_change_tick (GMI_A..GMI_F). One tick can be as
// fast as 6ms or as slow as 227ms (see the header comment above) and the top-level timer alone
// doesn't say which of Segment_LoadOverlays / the mode's init function / Segment_LoadAssets /
// Transition_AppearSet ate the time. Buffered rather than logged live: a transition happens at
// most a few times a minute so log volume isn't the concern, but a normal (sub-50ms) transition
// has nothing interesting to report and printing 5 extra lines for it would just be noise next
// to the one summary line gdx_transition_timer_end() already prints. Only dump the per-step
// breakdown once the whole tick is slow enough to matter.
#define GDX_TRANSITION_STEP_SLOTS 8
static const char* sGdxStepLabel[GDX_TRANSITION_STEP_SLOTS];
static double sGdxStepMs[GDX_TRANSITION_STEP_SLOTS];
static int sGdxStepCount = 0;
#ifdef _WIN32
static LARGE_INTEGER sGdxStepLast;
static int sGdxStepActive = 0;
#endif

void gdx_transition_step_begin(void) {
#ifdef _WIN32
    if (!sGdxTimerFreqInit) {
        QueryPerformanceFrequency(&sGdxTimerFreq);
        sGdxTimerFreqInit = 1;
    }
    sGdxStepCount = 0;
    sGdxStepActive = 1;
    QueryPerformanceCounter(&sGdxStepLast);
#endif
}

// Records the elapsed time since the previous mark (or since gdx_transition_step_begin()) under
// `label`. Call once per GMI_* checkpoint reached inside the timed A..F range.
void gdx_transition_step_mark(const char* label) {
#ifdef _WIN32
    LARGE_INTEGER now;
    double ms;
    if (!sGdxStepActive || !sGdxTimerFreqInit || sGdxTimerFreq.QuadPart == 0) {
        return;
    }
    QueryPerformanceCounter(&now);
    ms = (double)(now.QuadPart - sGdxStepLast.QuadPart) * 1000.0 / (double)sGdxTimerFreq.QuadPart;
    if (sGdxStepCount < GDX_TRANSITION_STEP_SLOTS) {
        sGdxStepLabel[sGdxStepCount] = label;
        sGdxStepMs[sGdxStepCount] = ms;
        sGdxStepCount++;
    }
    sGdxStepLast = now;
#else
    (void)label;
#endif
}

// Dumps the buffered per-step breakdown, one "[transition] <label> took X.XXms" line per step,
// but only when the steps recorded since gdx_transition_step_begin() sum to >= thresholdMs.
// Otherwise the buffer is silently dropped -- the top-level gdx_transition_timer_end() line
// already covers the common (fast) case.
void gdx_transition_step_flush(double thresholdMs) {
#ifdef _WIN32
    int i;
    double total = 0.0;
    if (!sGdxStepActive) {
        return;
    }
    sGdxStepActive = 0;
    for (i = 0; i < sGdxStepCount; i++) {
        total += sGdxStepMs[i];
    }
    if (total < thresholdMs) {
        return;
    }
    for (i = 0; i < sGdxStepCount; i++) {
        gdx_port_logf("[transition] %s took %.2fms\n", sGdxStepLabel[i], sGdxStepMs[i]);
    }
#else
    (void)thresholdMs;
#endif
}

// Cooperative yield that keeps the running thread RUNNABLE — for N64 busy-waits that on hardware
// spun while the VI/RDP advanced in the background (e.g. `while (osViGetCurrentFramebuffer() !=
// fb) {}`). Re-enqueue self on the run queue and return to the host so it can advance VI state,
// then we get re-dispatched to re-check the condition.
void gdx_yield(void) {
    if (__osRunningThread == NULL) {
        return; // host context: nothing to yield
    }
    // Re-enqueue as runnable, then return to the HOST loop (NOT __osDispatchThread, which would
    // just re-pick this same highest-priority thread and never let the host advance VI). The host
    // pumps a frame (gdx_vi_tick advances the framebuffer) and re-dispatches us to re-check.
    __osRunningThread->state = OS_STATE_RUNNABLE;
    __osEnqueueThread(&__osRunQueue, __osRunningThread);
    __osRunningThread = NULL;
#ifdef _WIN32
    SwitchToFiber(sHostFiber);
#endif
}

// Host: run runnable game threads until the game goes quiescent (all blocked / yielded to host).
void gdx_dispatch(void) {
    if (__osRunQueue != (OSThread*) &__osThreadTail) {
        __osDispatchThread();
    }
}

// ---------------------------------------------------------------------------------------------
// RDRAM size + SP task submission.
// osMemSize: 4 MB N64 base RAM (US base game; the Expansion Kit / 64DD needs the 8 MB pak).
// The osSpTask* functions are STUBS for now — R6 piece 4 routes the GFX OSTask's display list to
// libultraship's Fast3D (Fast3dWindow::DrawAndRunGraphicsCommands) and posts SP/DP completion.
// ---------------------------------------------------------------------------------------------
u32 osMemSize = 0x1000000; /* 16 MB: base 8MB + Expansion Pak (required by F-Zero X US) */

// libultra app NMI buffer (decomp uses it as an s32[] — must be real writable data, not a stub).
// 64 bytes (OS_APP_NMI_BUFSIZE) = 16 s32.
s32 osAppNMIBuffer[16];

// libultra init globals — the decomp's initialize.c is excluded (it does real N64 hardware I/O).
// osInitialize is a no-op on the host; these globals just need sane values.
OSTime osClockRate = OS_CLOCK_RATE;
u32 __osShutdown = 0;
u32 __OSGlobalIntMask = OS_IM_ALL;
u32 __kmc_pt_mode = 0;
void* __printfunc = (void*) 0;

void osInitialize(void) {
    // N64 RCP / PIF / PI hardware init is not needed on the host.
}

// The decomp's osGetMemSize probes RAM by dereferencing N64 bus addresses (crashes on host).
// Just report our RAM size.
u32 osGetMemSize(void) {
    return osMemSize;
}

extern OSMesgQueue gMainThreadMesgQueue;
extern OSMesgQueue D_800DCAC8;

void osSpTaskLoad(OSTask* tp) {
    (void) tp;
}

/* Software (HLE) audio RSP interpreter -- port/n64_audio_hle.c. Prototype uses plain "void
   pointer" and "unsigned int" (not the decomp's Acmd/u64/u32 types), so this decomp-environment
   TU doesn't need to expose the Acmd union to the host-side file: tp->t.data_ptr (a u64 pointer)
   and tp->t.data_size (a u32) convert implicitly. See n64_audio_hle.c's header comment for why
   the task's own data_ptr is NOT subject to the Acmd-word pointer-truncation hazard (only the
   command payload is). */
extern void gdx_audio_hle_run(const void* dataPtr, unsigned int dataSizeBytes);

/* ------------------------------------------------------------------------------------------
 * Phase 3 (port/gdx_audio_thread.cpp): dedicated-thread audio production entry point.
 * ------------------------------------------------------------------------------------------
 * AudioThread_CreateTask (decomp/src/audio/disk/lib/audio.h) returns a decomp `AudioTask*`
 * (decomp/src/audio/disk/lib/audio.h: `{ OSTask task; OSMesgQueue* msgQueue; void* unk_44;
 * char unk_48[8]; }`). Rather than pulling decomp's "global.h"/"audio.h" into this
 * deliberately minimal, hand-picked-PR-headers TU (real risk of macro/typedef collisions with
 * the fiber-scheduler internals above -- this file untouched-builds today specifically because
 * it does NOT include those umbrella headers), declare a local view struct whose ONLY member
 * is `OSTask task` (OSTask/OSTask_t already fully known here via "PR/sptask.h", used the same
 * way by osSpTaskStartGo below). C does not type-check a function's return type across TUs at
 * link time, only within each TU's own visible declarations, so treating the real
 * AudioThread_CreateTask's returned pointer as a GdxAudioTaskView* is safe as long as this file
 * only ever reads the LEADING member -- which AudioTask's real layout guarantees is `OSTask
 * task` at offset 0. Do not add more fields here without cross-checking
 * decomp/src/audio/disk/lib/audio.h's real AudioTask layout first.
 */
typedef struct GdxAudioTaskView {
    OSTask task;
} GdxAudioTaskView;

extern GdxAudioTaskView* AudioThread_CreateTask(void);

/* decomp/src/audio/disk/lib/load.c: `bool gAudioContextInitialized = false;` set true once
   Audio_Init() finishes. decomp's `bool` is `typedef int bool;` (decomp/include/libc/
   stdbool.h), i.e. plain 4-byte `int` -- declaring it here as `int` (not `bool`, which this TU
   does not typedef) is layout-identical to the real declaration without needing that header. */
extern int gAudioContextInitialized;

/* Called once per production tick by the dedicated audio thread (gdx_audio_thread.cpp's
 * AudioThreadMain, itself holding sAudioCtxMutex around this call -- see that file's MUTEX
 * BOUNDARY comment). Reproduces, unmodified and reusing the real decomp machinery, exactly the
 * two steps that used to run split across two different fiber contexts every VI wake:
 *   1) decomp/src/sys/sys_audio.c's Audio_ThreadEntry: AudioThread_CreateTask() ->
 *      AudioThread_CreateTaskImpl (audio/disk/lib/thread.c) builds this tick's Acmd command
 *      list, drains the game-thread's queued AUDIOCMD_* control commands (thread-cmd ring --
 *      see gdx_audio_thread.cpp's touchpoint enumeration), runs AudioLoad_ProcessLoads/
 *      ProcessScriptLoads (audio-heap-only, confirmed self-contained -- same enumeration), and
 *      calls osAiSetNextBuffer for the buffer that finished synthesizing two ticks ago.
 *   2) what used to be the "Sched"/"Main" thread's handling of EVENT_MESG_AUDIO_TASK_SET for
 *      that same task (sys_main.c's Sched_SpTaskStartAudio -> osSpTaskStart -> osSpTaskLoad +
 *      osSpTaskStartGo, i.e. exactly the M_AUDTASK branch below): execute the Acmd list through
 *      the software RSP interpreter (gdx_audio_hle_run) so the buffer actually contains PCM
 *      before it is due to be submitted two ticks from now.
 * Deliberately skips the SP/DP-done osSendMesg calls osSpTaskStartGo makes for M_AUDTASK
 * (gMainThreadMesgQueue/D_800DCAC8): those exist only to drive sys_main.c's SP_TASK_STATE
 * arbitration between GFX and AUDIO for the (simulated) shared RSP, which audio no longer
 * participates in once it has its own real thread -- the GFX-task path (osSpTaskStartGo's
 * M_GFXTASK branch, unchanged below) keeps using that arbitration exactly as before.
 * Returns 1 if a task was produced and executed this call, 0 if this tick was a legitimate
 * no-op (decomp's own totalTaskCount % specUnk4 gating inside CreateTaskImpl, a heap reset in
 * progress, or gAudioCtx not initialized yet during early boot) -- all expected, not errors.
 */
int gdx_audio_produce_one_tick(void) {
    /* Audio_SetupCreateTask, NOT AudioThread_CreateTask: the wrapper
       (decomp/src/audio/disk/external.c:2703) is the game's whole per-tick
       audio frame — the EK sequence/bank load state-machine pump
       (func_807427C0; skipping it left the boot riff stalled in
       SEQ_LOAD_BANK forever, proven by the [seq-load] probe timeline),
       BGM fade/pause routines, and AudioThread_ScheduleProcessCmds (the
       command-group drain scheduler), before it finally calls
       AudioThread_CreateTask itself. Calling the low-level creator
       directly silently dropped all of that game-level work. Runs under
       the same recursive gAudioCtx mutex as before (caller holds it);
       the game-side flag variables it reads were equally unsynchronized
       under the cooperative fiber, which interleaved at arbitrary yield
       points. */
    extern GdxAudioTaskView* Audio_SetupCreateTask(void);
    GdxAudioTaskView* task;

    if (!gAudioContextInitialized) {
        return 0;
    }

    task = Audio_SetupCreateTask();
    if (task == NULL) {
        return 0;
    }

    gdx_audio_hle_run(task->task.t.data_ptr, (unsigned int) task->task.t.data_size);
    return 1;
}

void osSpTaskStartGo(OSTask* tp) {
    /* Only graphics tasks may be fed to the Fast3D interpreter. Audio RSP tasks (M_AUDTASK)
       carry an ABI command list, not a GBI display list; running them through gdx_gfx_run reads
       audio words as pointers and crashes. Route audio tasks to the software audio HLE
       interpreter instead; anything else (neither GFX nor AUDIO) is acked so the game's
       scheduler still advances, then return. */
    if (tp->t.type == M_AUDTASK) {
        gdx_audio_hle_run(tp->t.data_ptr, tp->t.data_size);
        osSendMesg(&gMainThreadMesgQueue, (OSMesg)(uintptr_t)EVENT_MESG_SP, OS_MESG_NOBLOCK);
        osSendMesg(&D_800DCAC8, (OSMesg)(uintptr_t)0x2A, OS_MESG_NOBLOCK);
        return;
    }
    if (tp->t.type != M_GFXTASK) {
        if (gdx_diag_verbose()) {
            gdx_port_logf("[sched] osSpTaskStartGo: non-gfx task type=%u ucode=%p — acked, not run\n",
                          (unsigned)tp->t.type, (void*)tp->t.ucode);
        }
        osSendMesg(&gMainThreadMesgQueue, (OSMesg)(uintptr_t)EVENT_MESG_SP, OS_MESG_NOBLOCK);
        osSendMesg(&D_800DCAC8, (OSMesg)(uintptr_t)0x2A, OS_MESG_NOBLOCK);
        return;
    }

    GdxTaskUcode taskUcode = GDX_TASK_UCODE_F3DEX2;
    if (tp->t.ucode == (u64*) gspF3DLX2_Rej_fifoTextStart) {
        taskUcode = GDX_TASK_UCODE_F3DLX2_REJ;
    } else if (tp->t.ucode == (u64*) gspF3DFLX2_Rej_fifoTextStart) {
        taskUcode = GDX_TASK_UCODE_F3DFLX2_REJ;
    }
    if (gdx_diag_verbose()) {
        gdx_port_logf("[sched] osSpTaskStartGo: dl=%p size=%u\n",
                      (void*)tp->t.data_ptr, (unsigned)tp->t.data_size);
    }
    /* DETERMINISTIC ROOT-DL POINTER CARRY (campaign soak fix, 2026-07-08):
       OSTask_t.data_ptr is a u64* -- on the host it already holds the FULL
       64-bit pointer to gGfxPool->gfxBuffer (see Gfx_SetTask). This is the one
       task pointer the wide-Gfx campaign relies on being carried intact: the
       root DL is fed to the bridge BY POINTER, so it is NEVER subject to the
       low32 truncation + module-window guessing that corrupts sub-DL/vertex
       pointers. Passing tp->t.data_ptr straight through preserves that. The
       high32==0 case is only anomalous when the EXE is based above 4GB (as on
       the crashing run, module base 0x00007FF7........): log it once for
       diagnosis, but do NOT skip -- a low-based EXE has valid high32==0
       pointers, and the ROOT-validation failsafe in gdx_gfx_run/ConvertRoot
       already renders nothing if the root is genuinely unreadable/garbage. */
    {
        unsigned long long dlBits = (unsigned long long)(uintptr_t)tp->t.data_ptr;
        if (dlBits != 0ull && (dlBits >> 32) == 0ull) {
            static int sRootLow32Logs = 0;
            if (gdx_diag_verbose() && sRootLow32Logs < 4) {
                ++sRootLow32Logs;
                gdx_port_logf("[sched] NOTE: ROOT data_ptr high32==0 (%08X) -- "
                              "carried through by pointer, validated in ConvertRoot\n",
                              (unsigned)dlBits);
            }
        }
    }
    gdx_gfx_run(tp->t.data_ptr, tp->t.data_size, taskUcode);                           /* synchronous Fast3D Run() */
    if (gdx_diag_verbose()) {
        gdx_port_logf("[sched] osSpTaskStartGo: gdx_gfx_run done\n");
    }
    osSendMesg(&gMainThreadMesgQueue, (OSMesg)(uintptr_t)EVENT_MESG_SP, OS_MESG_NOBLOCK); /* SP done */
    osSendMesg(&D_800DCAC8,           (OSMesg)(uintptr_t)0x2A,           OS_MESG_NOBLOCK); /* DP done */
}

/* DP hardware status: always idle on host — no RDP hardware. */
u32  osDpGetStatus(void)         { return 0; }
void osDpSetStatus(u32 status)   { (void)status; }
void osSpTaskYield(void) {
}
OSYieldResult osSpTaskYielded(OSTask* tp) {
    (void) tp;
    return 0;
}
