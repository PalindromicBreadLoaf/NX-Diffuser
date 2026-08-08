// port/n64_sched.c — cooperative fiber scheduler.
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
#include "gdx_fiber.h"     // cooperative context abstraction (Win32 fibers / POSIX ucontext)

#include <stdint.h> // uintptr_t (MSVC leaks it transitively; GCC/glibc does not)

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h> /* crash-handler symbolization (PDB ships with Debug builds) */
#pragma comment(lib, "Dbghelp.lib")
#else
#include <sched.h>     /* sched_yield() for the audio-thread affinity spin-guard */
#include <pthread.h>   /* gdx_mq_lock: cross-OS-thread message-queue guard */
#endif

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "port_log.h"
#include "gdx_dev_gates.h" // Dev Tools gate layer: the diagnostic/behavior switches below

// ---------------------------------------------------------------------------------------------
// Per-thread fiber registry. We capture entry+arg here (with full 64-bit pointers) because the
// decomp's OSThread.context.pc/a0 are u32 and truncate a host function pointer to 32 bits.
// ---------------------------------------------------------------------------------------------
#define GDX_MAX_THREADS 32

typedef struct {
    OSThread* thread;
    GdxFiber* fiber;
    void (*entry)(void*);
    void*     arg;
} GdxThreadFiber;

static GdxThreadFiber sThreads[GDX_MAX_THREADS];
static int            sThreadCount = 0;
static GdxFiber*      sHostFiber = NULL;

// The OS thread that called gdx_sched_init(), and therefore owns every fiber in
// sThreads[]/sHostFiber. Cooperative contexts are only valid on the thread that created them, so
// switching from any OTHER real OS thread corrupts or crashes. Recorded so __osEnqueueAndYield
// below can detect the dedicated audio thread reaching a blocking osRecvMesg/osSendMesg wait and
// refuse to touch the scheduler. See gdx_audio_thread.cpp's header for the full analysis.
static unsigned long sHostThreadId = 0;

// ---------------------------------------------------------------------------------------------
// Cross-OS-thread message-queue guard.
//
// The dedicated audio OS thread (port/gdx_audio_thread.cpp) runs decomp code that calls
// osSendMesg/osRecvMesg. Their wake path (osStartThread(__osPopThread(&mq->mtqueue))) mutates the
// global __osRunQueue and the per-queue waiter lists — state that is thread-affine to the host
// fiber scheduler. __osDisableInt is a no-op here, so nothing serialized those mutations against
// the host thread's fibers: the 64DD boot thread and the audio thread corrupted the run queue
// concurrently (nondeterministic SIGABRT/SIGSEGV inside osSendMesg on Linux Release, where the
// dedicated audio thread is active during EK disk boot).
//
// The guard has two parts, used by the PORT paths in sendmesg.c/recvmesg.c/jammesg.c:
//  - gdx_mq_lock()/gdx_mq_unlock(): one process-wide lock making the queue DATA mutations
//    (msg[], first, validCount) atomic between the host thread and the audio thread. Never held
//    across a fiber switch (the blocking paths release it before __osEnqueueAndYield).
//  - gdx_sched_defer_wake(): on a NON-host thread the wake is recorded instead of performed;
//    gdx_sched_drain_deferred_wakes() (host loop, right before gdx_dispatch) re-checks the waiter
//    list and enqueues the woken thread on the run queue from the host thread. Deferral is
//    UNCONDITIONAL on non-host sends (not gated on a visible waiter) so the classic lost-wakeup
//    window — consumer checks empty, producer sends before the consumer enqueues itself — closes:
//    the pending defer re-checks the waiter list after the consumer has parked.
// ---------------------------------------------------------------------------------------------
#ifdef _WIN32
static SRWLOCK sMqLock = SRWLOCK_INIT; /* statically initialized: the audio thread starts before gdx_sched_init() */
void gdx_mq_lock(void) {
    AcquireSRWLockExclusive(&sMqLock);
}
void gdx_mq_unlock(void) {
    ReleaseSRWLockExclusive(&sMqLock);
}
#else
static pthread_mutex_t sMqMutex = PTHREAD_MUTEX_INITIALIZER;
void gdx_mq_lock(void) {
    pthread_mutex_lock(&sMqMutex);
}
void gdx_mq_unlock(void) {
    pthread_mutex_unlock(&sMqMutex);
}
#endif

int gdx_sched_on_host_thread(void) {
    /* Before gdx_sched_init() records the host id, no game fibers exist and no thread can be
       parked on a queue, so treating every caller as "host" is safe. */
    return sHostThreadId == 0 || gdx_fiber_current_thread_id() == sHostThreadId;
}

#define GDX_DEFERRED_WAKES 128
static OSThread** sDeferredWakes[GDX_DEFERRED_WAKES];
static int sDeferredWakeCount = 0;

/* Caller holds gdx_mq_lock. Coalesced: the audio thread defers on EVERY cross-thread send
   (see sendmesg.c), which floods the ring with duplicates of the same handful of queues —
   one pending entry per distinct wait list is enough, because the drain wakes every eligible
   waiter on it. Distinct wait lists are bounded by the number of message queues, so the ring
   cannot realistically overflow once deduplicated. */
void gdx_sched_defer_wake(OSThread** waitList) {
    int i;
    for (i = 0; i < sDeferredWakeCount; i++) {
        if (sDeferredWakes[i] == waitList) {
            return;
        }
    }
    if (sDeferredWakeCount < GDX_DEFERRED_WAKES) {
        sDeferredWakes[sDeferredWakeCount++] = waitList;
    } else {
        static int sOverflowLogs = 0;
        if (sOverflowLogs < 4) {
            sOverflowLogs++;
            gdx_port_logf("[sched] WARNING: deferred-wake ring overflow — a cross-thread mesg "
                          "wake was dropped\n");
        }
    }
}

/* Host thread only (main loop, before gdx_dispatch). Enqueue-only: osStartThread would dispatch
   fibers immediately when __osRunningThread is NULL; the imminent gdx_dispatch runs them at the
   frame's normal point instead. Wakes EVERY waiter on each deferred list (coalescing loses the
   send count): a spuriously woken thread re-checks its queue condition in osSendMesg/osRecvMesg's
   while loop and re-parks — standard condvar-broadcast semantics, safe by construction. */
void gdx_sched_drain_deferred_wakes(void) {
    OSThread** lists[GDX_DEFERRED_WAKES];
    int n;
    int i;

    gdx_mq_lock();
    n = sDeferredWakeCount;
    for (i = 0; i < n; i++) {
        lists[i] = sDeferredWakes[i];
    }
    sDeferredWakeCount = 0;

    for (i = 0; i < n; i++) {
        while (*lists[i] != NULL && (*lists[i])->next != NULL && (*lists[i])->state == OS_STATE_WAITING) {
            OSThread* t = __osPopThread(lists[i]);
            t->state = OS_STATE_RUNNABLE;
            __osEnqueueThread(&__osRunQueue, t);
        }
    }
    gdx_mq_unlock();
}

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
// Portable GdxFiberEntry: receives the GdxThreadFiber* passed at creation, runs the decomp
// thread's real entry, then reschedules. __osCleanupThread never returns here (it dispatches to
// another context), so this function does not fall off its end in practice.
static void gdx_fiber_main(void* param) {
    GdxThreadFiber* tf = (GdxThreadFiber*) param;
    gdx_port_logf("[sched] thread id=%d entry\n", (int) tf->thread->id);
    tf->entry(tf->arg);
    __osCleanupThread(); // entry returned -> thread is done
}

static GdxFiber* gdx_get_fiber(GdxThreadFiber* tf) {
    if (tf->fiber == NULL) {
        tf->fiber = gdx_fiber_create(gdx_fiber_main, tf, 0 /* default 1 MB stack */);
    }
    return tf->fiber;
}

// Run the highest-priority runnable thread. When the run queue is empty, return to the host
// fiber so the main loop can pump a frame and post the events that make threads runnable again.
void __osDispatchThread(void) {
    OSThread* t = __osRunQueue; // head = highest priority runnable

    if (t == (OSThread*) &__osThreadTail) {
        __osRunningThread = NULL;
        gdx_fiber_switch(sHostFiber);
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
        gdx_fiber_switch(gdx_get_fiber(tf));
    }
}

// Enqueue the running thread onto `queue` (if any), then switch to the next runnable thread.
void __osEnqueueAndYield(OSThread** queue) {
    // Safety net (see sHostThreadId above): a blocking wait reached from the dedicated audio
    // thread must NOT touch __osRunningThread/__osRunQueue or gdx_fiber_switch — all are affine to
    // the host context thread. Spin-yield instead: inefficient but safe, and this port's DMA
    // completions are synchronous, so the queue should never actually be empty here.
    if (sHostThreadId != 0 && gdx_fiber_current_thread_id() != sHostThreadId) {
        static int sForeignYieldLogs = 0;
        if (sForeignYieldLogs < 8) {
            sForeignYieldLogs++;
            gdx_port_logf("[sched] WARNING: __osEnqueueAndYield called from non-host thread id=%lu "
                          "(host=%lu) -- spin-yielding instead of dispatching\n",
                          gdx_fiber_current_thread_id(), sHostThreadId);
        }
#ifdef _WIN32
        Sleep(0);
#else
        sched_yield();
#endif
        return;
    }
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
    char reportLine[512];

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

    // Re-entry guard: the dbghelp lookup and file I/O below can themselves fault on a sufficiently
    // corrupted process, and without this a fault-while-handling-a-fault recurses until the stack
    // is exhausted. The release MUST stay inside __finally — if the guarded body faults, the
    // nested exception hits the guard above and never reaches the release, latching the flag for
    // the rest of the process and silently dropping every later crash report.
    {
        static volatile LONG sInHandler = 0;
        if (InterlockedCompareExchange(&sInHandler, 1, 0) != 0) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

      __try {
        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
            isWrite = (int) rec->ExceptionInformation[0];
            faultAddr = (void*) rec->ExceptionInformation[1];
        }

        // Every gdx_port_logf line below is mirrored into gdiffuser-crash.txt: field testers run a
        // plain double-clicked build with no diagnostic gate set, so gdx_port_logf's file sink
        // never opens and a crash would leave nothing on disk. gdx_crash_report_write bypasses
        // that opt-in gate.
        {
            SYSTEMTIME st;
            int n;
            GetSystemTime(&st);
            n = snprintf(reportLine, sizeof(reportLine),
                         "\n==== gdiffuser-x64 crash %04u-%02u-%02u %02u:%02u:%02u.%03u UTC ====\n",
                         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                         st.wMilliseconds);
            if (n > 0) {
                gdx_crash_report_write(reportLine);
            }
        }

        gdx_port_logf(
            "[crash] UNHANDLED EXCEPTION code=0x%08X pc=%p thread_id=%d fault_addr=%p access=%s\n",
            (unsigned) rec->ExceptionCode, (void*) rec->ExceptionAddress, threadId, faultAddr,
            (isWrite < 0) ? "n/a" : (isWrite ? "write" : "read"));
        if (snprintf(reportLine, sizeof(reportLine),
                     "[crash] UNHANDLED EXCEPTION code=0x%08X pc=%p thread_id=%d fault_addr=%p access=%s\n",
                     (unsigned) rec->ExceptionCode, (void*) rec->ExceptionAddress, threadId, faultAddr,
                     (isWrite < 0) ? "n/a" : (isWrite ? "write" : "read")) > 0) {
            gdx_crash_report_write(reportLine);
        }

        /* Microsoft C++ exception (0xE06D7363): the pc above is KERNELBASE!RaiseException, which
           names neither the throw site nor the message, so such a report is undiagnosable without
           this. Pull the mangled type name and, for a std::exception, its what() string out of the
           throw record. Both helpers are SEH-guarded in the bridge, so a garbage record degrades
           to silence. */
        if (rec->ExceptionCode == 0xE06D7363u) {
            extern const char* gdx_cxx_exception_name(const EXCEPTION_RECORD* rec);
            extern const char* gdx_cxx_exception_what(const EXCEPTION_RECORD* rec);
            const char* cxxName = gdx_cxx_exception_name(rec);
            const char* cxxWhat = gdx_cxx_exception_what(rec);
            gdx_port_logf("[crash]   cxx type=%s what=%s\n", cxxName ? cxxName : "?",
                          cxxWhat ? cxxWhat : "?");
            if (snprintf(reportLine, sizeof(reportLine), "[crash]   cxx type=%s what=%s\n",
                         cxxName ? cxxName : "?", cxxWhat ? cxxWhat : "?") > 0) {
                gdx_crash_report_write(reportLine);
            }
        }

        /* Best-effort symbolization: Debug builds ship the PDB next to the exe, so a raw pc can be
           named here instead of hand-mapping ASLR'd addresses across runs. Any failure just leaves
           the raw-pc line above. */
        {
            static int sSymInit = 0;
            HANDLE proc = GetCurrentProcess();
            DWORD64 pc = (DWORD64) (uintptr_t) rec->ExceptionAddress;
            char symBuf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO* sym = (SYMBOL_INFO*) symBuf;
            DWORD64 disp = 0;
            IMAGEHLP_LINE64 line;
            DWORD lineDisp = 0;
            HMODULE mod = NULL;

            if (!sSymInit) {
                sSymInit = 1;
                SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
                SymInitialize(proc, NULL, TRUE);
            }
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR) rec->ExceptionAddress, &mod)) {
                gdx_port_logf("[crash]   module_base=%p rva=0x%llX\n", (void*) mod,
                              (unsigned long long) (pc - (DWORD64) (uintptr_t) mod));
                if (snprintf(reportLine, sizeof(reportLine), "[crash]   module_base=%p rva=0x%llX\n",
                             (void*) mod, (unsigned long long) (pc - (DWORD64) (uintptr_t) mod)) > 0) {
                    gdx_crash_report_write(reportLine);
                }
            }
            memset(symBuf, 0, sizeof(symBuf));
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            if (SymFromAddr(proc, pc, &disp, sym)) {
                gdx_port_logf("[crash]   at %s+0x%llX\n", sym->Name, (unsigned long long) disp);
                if (snprintf(reportLine, sizeof(reportLine), "[crash]   at %s+0x%llX\n", sym->Name,
                             (unsigned long long) disp) > 0) {
                    gdx_crash_report_write(reportLine);
                }
            }
            memset(&line, 0, sizeof(line));
            line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            if (SymGetLineFromAddr64(proc, pc, &lineDisp, &line)) {
                gdx_port_logf("[crash]   %s:%lu\n", line.FileName, (unsigned long) line.LineNumber);
                if (snprintf(reportLine, sizeof(reportLine), "[crash]   %s:%lu\n", line.FileName,
                             (unsigned long) line.LineNumber) > 0) {
                    gdx_crash_report_write(reportLine);
                }
            }
        }
      } __finally {
        InterlockedExchange(&sInHandler, 0);
      }
    }

    return EXCEPTION_CONTINUE_SEARCH; // do not swallow the fault, only log it
}
#endif

// Convert the main OS thread into a fiber so the scheduler can switch to/from it. Call once
// before bootproc().
void gdx_sched_init(void) {
    sHostFiber = gdx_fiber_convert_thread();
    sHostThreadId = gdx_fiber_current_thread_id();
#ifdef _WIN32
    AddVectoredExceptionHandler(1 /* call first */, gdx_crash_vectored_handler);
#endif
    gdx_port_logf("[sched] host fiber ready\n");
}

// Called by the (port-patched) idle thread's spin: hand the CPU back to the host loop.
void gdx_yield_to_host(void) {
    gdx_fiber_switch(sHostFiber);
}

// Diagnostic log helpers for decomp .c files that cannot include <stdio.h> (libc/stdint.h clash).
// Called unconditionally and at high frequency from the decomp checkpoints, hence the GDX_TRACE
// gate — see gdx_trace_enabled() in port_log.h.
/* Real (non-inline) extern printf-style logger so the bounded
   [GDX-DBG ...] probes in transition.c (decomp) and interpreter.cpp (libultraship) can
   link against a single symbol -- gdx_port_logf is static inline (header-only) and does
   not export. Always on (mirrors gdx_port_logf's own ungated boot/one-shot behaviour).
   Remove together with the probes it serves. */
void gdx_dbg_logf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    gdx_port_vlogf(fmt, args);
    va_end(args);
}

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

/* The gate accessors below read the shared Dev Tools gate cache (port/gdx_dev_gates.{h,c}), so a
   gate read is one aligned int load — cheap enough for the per-frame call sites. Keep these exact
   names: the decomp C and the gfx bridge call them directly. */
int gdx_unlock_diag_enabled(void) {
    return gdx_dev_gate(GDX_GATE_DIAG_UNLOCK);
}

void gdx_unlock_diagf(const char* fmt, ...) {
    va_list args;

    if (!gdx_unlock_diag_enabled()) {
        return;
    }
    va_start(args, fmt);
    gdx_port_vlogf(fmt, args);
    va_end(args);
}

/* GDX_DIAG_VERBOSE gate for the per-frame diagnostic log families
   ([gfxdiag], [game], [seg], [sched], [phasegeom], [bigtri]). Silent (0) by
   default; enabled by GDX_DIAG_VERBOSE at launch or live from
   F1 > Dev Tools > Logging (Bucket D: see port/gdx_dev_gates.h). */
int gdx_diag_verbose(void) {
    return gdx_dev_gate(GDX_GATE_DIAG_VERBOSE);
}

/* Freezes the rail chevron color sawtooth (decomp course.c primcolor sites) so a strobe can be
   attributed to interpolation rather than the color animation. CHANGES WHAT IS RENDERED — Bucket
   B, off by default and compiled out entirely without GDX_DEV_TOOLS. */
int gdx_rail_color_test_enabled(void) {
    return gdx_dev_gate(GDX_GATE_RAIL_COLOR_TEST);
}

/* Once-per-second dump of the rival-icon draw condition (decomp racer.c) plus the emitted texrect
   coords, which separates "gate never passes" from "draw emitted but invisible". OFF by default. */
int gdx_diag_rival_enabled(void) {
    return gdx_dev_gate(GDX_GATE_DIAG_RIVAL);
}

/* Dumps the gCustomMachine record the Create Machine draw path reads. The struct is defined
   `= { 0 }` (expansion_kit_data.c) while machine_create_draw.c indexes its texture arrays as
   `logo - 1` etc., so an uninitialized record means index -1 and a black env color. One logged
   entry separates that from an asset-layer fault. OFF by default. */
int gdx_diag_custommachine_enabled(void) {
    return gdx_dev_gate(GDX_GATE_DIAG_CUSTOMMACHINE);
}

/* Logs both Light_SetLookAtSource call sites so their matrices can be compared. The same
   custom-machine reflection display lists render correctly from machine.c (ovl_i4/machine.c:1530,
   real camera view matrix) and flat from func_xk3_80130698 (gGfxPool->unk_20108), so a degenerate
   source there is the suspect. Diagnostic only, no rendering change. */
int gdx_diag_lookat_enabled(void) {
    return gdx_dev_gate(GDX_GATE_DIAG_LOOKAT);
}


/* Shares GDX_DIAG_NODEINFO with the SETTIMG-side probe in n64_gfx_bridge.cpp so
   one run captures both ends of the same question: whether the Course Edit info
   text is drawn at all, and whether its font sheets resolve when it is. */
int gdx_dev_gate_by_name_nodeinfo(void) {
    return gdx_dev_gate(GDX_GATE_DIAG_NODEINFO);
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
// [transition] timing probe. game.c's GAMEMODE_CHANGE_INIT runs Segment_LoadOverlays(), the
// mode's init function and Segment_LoadAssets() synchronously on the GAME thread in one tick with
// no host frame in between — that tick is the visible freeze. Deliberately NOT gated by
// GDX_TRACE: a transition happens a few times a minute, so the volume is negligible and a user
// can quantify a freeze without turning on the full per-frame trace.
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
// [transition] per-step breakdown within mode_change_tick (GMI_A..GMI_F). A tick ranges from 6ms
// to 227ms and the top-level timer alone doesn't say which step ate it. Buffered rather than
// logged live so a normal sub-50ms transition contributes nothing beyond the one summary line
// gdx_transition_timer_end() already prints.
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
/* Count of completed yields, read-only diagnostic — nothing branches on it. A yield hands control
   to the HOST fiber, which finishes its frame before re-dispatching, so one yield costs however
   much of the host frame remained when it landed: measured between ~1.5ms and ~15ms, NOT a fixed
   amount. That is where the Cup Select stall went — twelve decodes measuring 4.5-134ms on the
   game fiber (2..9 yields each) complete in 2.2ms total on the host thread, where yielding is
   impossible. */
unsigned long gdx_yield_count = 0;

void gdx_yield(void) {
    if (__osRunningThread == NULL) {
        return; // host context: nothing to yield
    }
    ++gdx_yield_count;
    // Re-enqueue as runnable, then return to the HOST loop (NOT __osDispatchThread, which would
    // just re-pick this same highest-priority thread and never let the host advance VI). The host
    // pumps a frame (gdx_vi_tick advances the framebuffer) and re-dispatches us to re-check.
    __osRunningThread->state = OS_STATE_RUNNABLE;
    __osEnqueueThread(&__osRunQueue, __osRunningThread);
    __osRunningThread = NULL;
    gdx_fiber_switch(sHostFiber);
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
// Task submission is SPLIT: osSpTaskStartGo below is real — it dispatches on OSTask_t.type,
// running M_GFXTASK display lists through gdx_gfx_run (the Fast3D bridge) and M_AUDTASK command
// lists through gdx_audio_lle_run, then posts SP + DP completion. osSpTaskLoad / osSpTaskYield /
// osSpTaskYielded stay no-ops: there is no RSP to load ucode into and nothing to preempt, since
// gdx_gfx_run/gdx_audio_lle_run complete synchronously inside StartGo.
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
/* LLE dispatch seam (port/gdx_audio_lle.c): file-toggled real-RSP path, HLE fallback. */
extern void gdx_audio_lle_run(const void* dataPtr, unsigned int dataSizeBytes);

/* ------------------------------------------------------------------------------------------
 * Dedicated-thread audio production entry point (port/gdx_audio_thread.cpp).
 * ------------------------------------------------------------------------------------------
 * A local view of decomp's AudioTask, so this TU stays out of decomp's "global.h"/"audio.h" —
 * those umbrella headers collide with the fiber-scheduler internals above. Reading the real
 * AudioThread_CreateTask's return through this type is only safe because AudioTask's layout
 * (decomp/src/audio/disk/lib/audio.h) puts `OSTask task` at offset 0 and this file touches
 * nothing else. Do NOT add fields here without cross-checking that layout first.
 */
typedef struct GdxAudioTaskView {
    OSTask task;
} GdxAudioTaskView;

extern GdxAudioTaskView* AudioThread_CreateTask(void);

/* Defined in decomp/src/audio/disk/lib/load.c as `bool`, which decomp typedefs to plain `int`
   (decomp/include/libc/stdbool.h) — so declaring it `int` here is layout-identical and avoids
   pulling in that header. */
extern int gAudioContextInitialized;

/* One production tick, called by gdx_audio_thread.cpp's AudioThreadMain while it holds
 * sAudioCtxMutex. Fuses the two steps that used to run in separate fiber contexts per VI wake:
 * Audio_ThreadEntry's task build, and the Sched thread's M_AUDTASK execution of the resulting
 * Acmd list, so the buffer holds PCM before it is due for submission.
 *
 * The SP/DP-done osSendMesg calls that osSpTaskStartGo makes for M_AUDTASK are deliberately
 * skipped: they exist only to drive sys_main.c's SP_TASK_STATE arbitration between GFX and AUDIO
 * over the simulated shared RSP, which audio no longer participates in now that it owns a real
 * thread. The M_GFXTASK path below still uses that arbitration unchanged.
 *
 * Returns 0 for a legitimate no-op tick (decomp's own totalTaskCount gating, a heap reset in
 * progress, or gAudioCtx not yet initialized during early boot) — expected, not an error.
 */
int gdx_audio_produce_one_tick(void) {
    /* Audio_SetupCreateTask, NOT AudioThread_CreateTask. The wrapper
       (decomp/src/audio/disk/external.c:2703) is the game's whole per-tick audio frame — the EK
       sequence/bank load state-machine pump, the BGM fade/pause routines and
       AudioThread_ScheduleProcessCmds — before it calls AudioThread_CreateTask itself. Calling
       the low-level creator directly silently drops all of that and strands the boot riff in
       SEQ_LOAD_BANK forever. */
    extern GdxAudioTaskView* Audio_SetupCreateTask(void);
    GdxAudioTaskView* task;

    if (!gAudioContextInitialized) {
        return 0;
    }

    task = Audio_SetupCreateTask();
    if (task == NULL) {
        return 0;
    }

    gdx_audio_lle_run(task->task.t.data_ptr, (unsigned int) task->task.t.data_size);
    return 1;
}

void osSpTaskStartGo(OSTask* tp) {
    /* Only graphics tasks may be fed to the Fast3D interpreter. Audio RSP tasks (M_AUDTASK)
       carry an ABI command list, not a GBI display list; running them through gdx_gfx_run reads
       audio words as pointers and crashes. Route audio tasks to the software audio HLE
       interpreter instead; anything else (neither GFX nor AUDIO) is acked so the game's
       scheduler still advances, then return. */
    if (tp->t.type == M_AUDTASK) {
        gdx_audio_lle_run(tp->t.data_ptr, tp->t.data_size);
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
    /* The root DL pointer MUST be carried through intact. OSTask_t.data_ptr already holds the
       full 64-bit pointer to gGfxPool->gfxBuffer (see Gfx_SetTask), and the bridge takes the root
       BY POINTER, so it never suffers the low32 truncation + module-window guessing that corrupts
       sub-DL and vertex pointers. high32==0 is only anomalous when the EXE is based above 4GB, so
       log it and do NOT skip: a low-based EXE has legitimately zero high32, and ConvertRoot's own
       validation already renders nothing if the root is genuinely garbage. */
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
