/* AAPCS64 fiber backend for gdx_fiber.h. */

#include "gdx_fiber.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>

#if !defined(__aarch64__)
#error "gdx_fiber_aarch64.c is an aarch64-only backend"
#endif

#if defined(__has_include)
#if __has_include(<sys/syscall.h>) && !defined(__SWITCH__)
#include <sys/syscall.h>
#include <unistd.h>
#define GDX_FIBER_HAVE_GETTID 1
#endif
#endif

#ifndef GDX_FIBER_GUARD
#define GDX_FIBER_GUARD 1
#endif

/* Deliberately not a plausible pointer */
#define GDX_FIBER_FILL_WORD 0xFEEDFACECAFEBEEFull
#define GDX_FIBER_CANARY_BYTES 16u

#define GDX_FIBER_DEFAULT_STACK (1024u * 1024u)

/* Callee-saved save area laid down by gdx_fiber_asm_switch. Offsets are hard-coded in the asm
 * below AND in gdx_fiber_create's initial-frame construction which the both must agree.
 *
 *   0  x19 x20      48  x25 x26      96  d8  d9      144  d14 d15
 *  16  x21 x22      64  x27 x28     112  d10 d11
 *  32  x23 x24      80  x29 x30     128  d12 d13
 */
#define GDX_FIBER_FRAME_BYTES 160u
#define GDX_FIBER_FRAME_X29 80u
#define GDX_FIBER_FRAME_X30 88u

struct GdxFiber {
    void* sp;
    void* stack;
    size_t stackSize;
    GdxFiberEntry entry;
    void* arg;
    int started; /* has the first switch armed the trampoline yet? */
    int isHost;  /* converted-thread context. */
    struct GdxFiber* next; /* registry link */
};

static __thread GdxFiber* sCurrent = NULL;
static __thread GdxFiber* sTrampolineArg = NULL;

/* Every fiber ever created */
static GdxFiber* sRegistry = NULL;

void gdx_fiber_asm_switch(void** saveSp, void* resumeSp);
void gdx_fiber_asm_entry(void);

__asm__(
    ".text\n"
    ".balign 4\n"
    ".global gdx_fiber_asm_switch\n"
    ".type   gdx_fiber_asm_switch, %function\n"
    "gdx_fiber_asm_switch:\n"
    "    sub  sp, sp, #160\n"
    "    stp  x19, x20, [sp, #0]\n"
    "    stp  x21, x22, [sp, #16]\n"
    "    stp  x23, x24, [sp, #32]\n"
    "    stp  x25, x26, [sp, #48]\n"
    "    stp  x27, x28, [sp, #64]\n"
    "    stp  x29, x30, [sp, #80]\n"
    "    stp  d8,  d9,  [sp, #96]\n"
    "    stp  d10, d11, [sp, #112]\n"
    "    stp  d12, d13, [sp, #128]\n"
    "    stp  d14, d15, [sp, #144]\n"
    "    mov  x2, sp\n"
    "    str  x2, [x0]\n"          /* *saveSp = sp */
    "    mov  sp, x1\n"            /* adopt the incoming stack */
    "    ldp  x19, x20, [sp, #0]\n"
    "    ldp  x21, x22, [sp, #16]\n"
    "    ldp  x23, x24, [sp, #32]\n"
    "    ldp  x25, x26, [sp, #48]\n"
    "    ldp  x27, x28, [sp, #64]\n"
    "    ldp  x29, x30, [sp, #80]\n"
    "    ldp  d8,  d9,  [sp, #96]\n"
    "    ldp  d10, d11, [sp, #112]\n"
    "    ldp  d12, d13, [sp, #128]\n"
    "    ldp  d14, d15, [sp, #144]\n"
    "    add  sp, sp, #160\n"
    "    ret\n" /* either a prior switch's caller or gdx_fiber_asm_entry */
    ".size   gdx_fiber_asm_switch, .-gdx_fiber_asm_switch\n"
);

/*
 * First-run entry reached by the `ret` above */
__asm__(
    ".text\n"
    ".balign 4\n"
    ".global gdx_fiber_asm_entry\n"
    ".type   gdx_fiber_asm_entry, %function\n"
    "gdx_fiber_asm_entry:\n"
    "    .cfi_startproc\n"
    "    .cfi_undefined x30\n"
    "    mov  x29, xzr\n"
    "    bl   gdx_fiber_trampoline\n"
    "    brk  #0\n"
    "    .cfi_endproc\n"
    ".size   gdx_fiber_asm_entry, .-gdx_fiber_asm_entry\n"
);

void gdx_fiber_trampoline(void);

void gdx_fiber_trampoline(void) {
    GdxFiber* f = sTrampolineArg;
    f->entry(f->arg);
    fprintf(stderr, "[gdx_fiber] fiber entry %p returned\n", (void*)f->entry);
    abort();
}

#if GDX_FIBER_GUARD
static void gdx_fiber_check_canary(const GdxFiber* f, const char* when) {
    const uint64_t* base;
    unsigned i;

    if (f == NULL || f->isHost || f->stack == NULL) {
        return;
    }
    base = (const uint64_t*)f->stack;
    for (i = 0; i < GDX_FIBER_CANARY_BYTES / sizeof(uint64_t); ++i) {
        if (base[i] != GDX_FIBER_FILL_WORD) {
            fprintf(stderr,
                    "[gdx_fiber] STACK OVERFLOW: fiber %p (entry %p, %zu byte stack) overran its "
                    "low end, detected %s. canary[%u]=0x%016llX want 0x%016llX\n",
                    (const void*)f, (void*)f->entry, f->stackSize, when, i,
                    (unsigned long long)base[i], (unsigned long long)GDX_FIBER_FILL_WORD);
            abort();
        }
    }
}
#else
#define gdx_fiber_check_canary(f, when) ((void)0)
#endif

GdxFiber* gdx_fiber_convert_thread(void) {
    GdxFiber* f = (GdxFiber*)calloc(1, sizeof(*f));
    if (f == NULL) {
        return NULL;
    }
    f->isHost = 1;
    f->started = 1;
    sCurrent = f;
    f->next = sRegistry;
    sRegistry = f;
    return f;
}

GdxFiber* gdx_fiber_create(GdxFiberEntry entry, void* arg, size_t stackSize) {
    GdxFiber* f;
    unsigned char* mem;
    unsigned char* top;
    unsigned char* frame;
    uint64_t* fill;
    size_t words;
    size_t i;

    if (stackSize == 0) {
        stackSize = GDX_FIBER_DEFAULT_STACK;
    }
    stackSize = (stackSize + 15u) & ~(size_t)15u;

    if (stackSize < GDX_FIBER_FRAME_BYTES + GDX_FIBER_CANARY_BYTES) {
        return NULL;
    }

    f = (GdxFiber*)calloc(1, sizeof(*f));
    if (f == NULL) {
        return NULL;
    }

    mem = (unsigned char*)aligned_alloc(16u, stackSize);
    if (mem == NULL) {
        free(f);
        return NULL;
    }

    fill = (uint64_t*)mem;
    words = stackSize / sizeof(uint64_t);
    for (i = 0; i < words; ++i) {
        fill[i] = GDX_FIBER_FILL_WORD;
    }

    top = mem + stackSize;
    frame = top - GDX_FIBER_FRAME_BYTES;

    memset(frame, 0, GDX_FIBER_FRAME_BYTES);
    *(void**)(frame + GDX_FIBER_FRAME_X29) = NULL;
    *(void**)(frame + GDX_FIBER_FRAME_X30) = (void*)&gdx_fiber_asm_entry;

    f->sp = frame;
    f->stack = mem;
    f->stackSize = stackSize;
    f->entry = entry;
    f->arg = arg;
    f->started = 0; /* trampoline arming deferred to the first switch */
    f->isHost = 0;
    f->next = sRegistry;
    sRegistry = f;
    return f;
}

void gdx_fiber_switch(GdxFiber* to) {
    GdxFiber* from = sCurrent;

    gdx_fiber_check_canary(from, "leaving it");
    gdx_fiber_check_canary(to, "entering it");

    sCurrent = to;
    if (!to->started) {
        to->started = 1;
        sTrampolineArg = to;
    }
    gdx_fiber_asm_switch(&from->sp, to->sp);
}

unsigned long gdx_fiber_current_thread_id(void) {
#if defined(GDX_FIBER_HAVE_GETTID) && defined(SYS_gettid)
    return (unsigned long)syscall(SYS_gettid);
#else
    return (unsigned long)(uintptr_t)pthread_self();
#endif
}

void gdx_fiber_stack_report(void);

void gdx_fiber_stack_report(void) {
    const GdxFiber* f;

    fprintf(stderr, "[gdx_fiber] stack report (fill=0x%016llX)\n",
            (unsigned long long)GDX_FIBER_FILL_WORD);
    for (f = sRegistry; f != NULL; f = f->next) {
        const uint64_t* base;
        size_t words;
        size_t i;
        size_t used;

        if (f->isHost || f->stack == NULL) {
            fprintf(stderr, "  %p  host context OS thread stack\n", (const void*)f);
            continue;
        }
        base = (const uint64_t*)f->stack;
        words = f->stackSize / sizeof(uint64_t);
        for (i = 0; i < words && base[i] == GDX_FIBER_FILL_WORD; ++i) {
        }
        used = (words - i) * sizeof(uint64_t);
        fprintf(stderr, "  %p  entry=%p  used %zu of %zu bytes (%zu%%)\n", (const void*)f,
                (void*)f->entry, used, f->stackSize,
                f->stackSize ? (used * 100u) / f->stackSize : 0u);
    }
}
