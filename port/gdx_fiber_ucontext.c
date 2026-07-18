/* port/gdx_fiber_ucontext.c -- POSIX (ucontext) fiber backend for gdx_fiber.h.
 *
 * Mirrors the Win32 backend's semantics using getcontext/makecontext/swapcontext.
 *
 * DESIGN NOTES
 * ------------
 * makecontext-arg hazard: makecontext passes its extra arguments as ints, so a
 * 64-bit pointer would be split across two int slots on LP64 (the classic
 * "pass hi/lo halves" idiom). We sidestep that entirely: the trampoline takes
 * ZERO makecontext arguments and reads the target fiber from a thread-local set
 * immediately before the switch. Simpler and safe on any ABI.
 *
 * Deferred makecontext: getcontext() runs at create time (it only needs a valid
 * uc_stack, which we set then), but makecontext() is deferred to the first
 * gdx_fiber_switch so sTrampolineArg is written in the same breath as the resume
 * -- no window where a stale value could be read.
 *
 * Stacks: 1 MB by default, allocated with mmap(MAP_PRIVATE|MAP_ANONYMOUS) plus a
 * PROT_NONE guard page at the low end (stacks grow down, so an overflow faults on
 * the guard instead of silently smashing neighbouring memory). This approximates
 * Win32's stack-reserve-with-guard behavior closely enough for the scheduler.
 *
 * swapcontext cost: swapcontext performs a sigprocmask syscall on every switch to
 * save/restore the signal mask. The scheduler switches a few times per VI frame,
 * so this is acceptable for now. If profiling ever shows it hurts, the escape
 * hatch is to replace the swapcontext internals here with a ~40-line x86-64 asm
 * switch (save/restore callee-saved regs + rsp only, no signal-mask syscall)
 * behind this exact same API -- no caller in n64_sched.c changes.
 */
#include "gdx_fiber.h"

#include <stdlib.h>
#include <stdint.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>

#ifndef MAP_STACK
#define MAP_STACK 0
#endif
#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
#define MAP_ANONYMOUS MAP_ANON
#else
#define MAP_ANONYMOUS 0
#endif
#endif

struct GdxFiber {
    ucontext_t    ctx;
    void*         mapping;    /* mmap base (guard page + usable stack); NULL for host */
    size_t        mappingLen; /* full mapping length for munmap */
    GdxFiberEntry entry;
    void*         arg;
    int           started;    /* makecontext done? */
    int           isHost;     /* converted-thread context (no mmap stack) */
};

/* Who is currently running on this OS thread: the save slot for swapcontext. */
static __thread GdxFiber* sCurrent = NULL;
/* Target of the pending first switch; read once at trampoline entry. */
static __thread GdxFiber* sTrampolineArg = NULL;

static void gdx_fiber_ucontext_trampoline(void) {
    GdxFiber* f = sTrampolineArg;
    f->entry(f->arg);
    /* Entry is not expected to return (the decomp threads reschedule through
     * __osCleanupThread). uc_link is NULL by design -- there is nowhere to go --
     * so abort loudly rather than execute undefined behavior. */
    abort();
}

GdxFiber* gdx_fiber_convert_thread(void) {
    GdxFiber* f = (GdxFiber*) calloc(1, sizeof(*f));
    if (f == NULL) {
        return NULL;
    }
    f->isHost = 1;
    f->started = 1; /* the host context is already "running" */
    sCurrent = f;   /* the calling thread IS this context right now */
    return f;
}

GdxFiber* gdx_fiber_create(GdxFiberEntry entry, void* arg, size_t stackSize) {
    long pageSc;
    size_t page;
    size_t usable;
    size_t total;
    void* mem;
    GdxFiber* f;

    if (stackSize == 0) {
        stackSize = 1024u * 1024u; /* 1 MB default, matching Win32 */
    }

    pageSc = sysconf(_SC_PAGESIZE);
    page = (pageSc > 0) ? (size_t) pageSc : 4096u;

    /* Round the usable stack up to a page multiple, then add one guard page. */
    usable = (stackSize + page - 1u) & ~(page - 1u);
    total = usable + page;

    f = (GdxFiber*) calloc(1, sizeof(*f));
    if (f == NULL) {
        return NULL;
    }

    mem = mmap(NULL, total, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (mem == MAP_FAILED) {
        free(f);
        return NULL;
    }

    /* Guard page at the lowest address; the stack grows down into `usable`. */
    if (mprotect(mem, page, PROT_NONE) != 0) {
        /* Non-fatal: without the guard an overflow is undiagnosed, but the
         * fiber still works. Keep going rather than fail creation. */
    }

    if (getcontext(&f->ctx) != 0) {
        munmap(mem, total);
        free(f);
        return NULL;
    }
    f->ctx.uc_link = NULL; /* entry never returns; see trampoline */
    f->ctx.uc_stack.ss_sp = (char*) mem + page; /* above the guard page */
    f->ctx.uc_stack.ss_size = usable;
    f->ctx.uc_stack.ss_flags = 0;

    f->mapping = mem;
    f->mappingLen = total;
    f->entry = entry;
    f->arg = arg;
    f->started = 0; /* makecontext deferred to first switch */
    f->isHost = 0;
    return f;
}

void gdx_fiber_switch(GdxFiber* to) {
    GdxFiber* from = sCurrent;
    sCurrent = to;
    if (!to->started) {
        to->started = 1;
        /* Set the trampoline target immediately before arming/resuming so the
         * thread-local read at trampoline entry sees exactly this fiber. */
        sTrampolineArg = to;
        makecontext(&to->ctx, gdx_fiber_ucontext_trampoline, 0);
    }
    swapcontext(&from->ctx, &to->ctx);
}

unsigned long gdx_fiber_current_thread_id(void) {
#ifdef SYS_gettid
    return (unsigned long) syscall(SYS_gettid);
#else
    return (unsigned long) (uintptr_t) pthread_self();
#endif
}
