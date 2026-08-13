/*
 * Unit tests for the fiber backends behind port/gdx_fiber.h.
 *
 * Standalone console exe: no libultraship, no game objects, no window, no assets. Exits 0 iff
 * every check passes.
 */
#include "gdx_fiber.h"
#include "test_console_out.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void check(const char* name, int ok, const char* detail) {
    if (ok) {
        printf("[ OK ] %-52s %s\n", name, detail ? detail : "");
    } else {
        printf("[FAIL] %-52s %s\n", name, detail ? detail : "");
        ++g_failures;
    }
}

#if defined(__aarch64__)
#define GDX_FIBER_TEST_REGISTERS 1

/*
 * Load a distinct value into every AAPCS64 callee-saved register, switch away, and
 * on resume, verify that every one came back unchanged.
 */
unsigned long gdx_fiber_test_roundtrip(GdxFiber* to, unsigned long seed);

__asm__(
    ".text\n"
    ".balign 4\n"
    ".global gdx_fiber_test_roundtrip\n"
    ".type   gdx_fiber_test_roundtrip, %function\n"

    /* Walk a chain of seed + n*stride while writing each value into the next register under test. */
    ".macro GDXT_SET_X reg\n"
    "    add  x12, x12, x11\n"
    "    mov  \\reg, x12\n"
    ".endm\n"
    ".macro GDXT_SET_D reg\n"
    "    add  x12, x12, x11\n"
    "    fmov \\reg, x12\n"
    ".endm\n"
    /* Verify everything is identical to before. */
    ".macro GDXT_CHK_X reg, bit\n"
    "    add  x12, x12, x11\n"
    "    cmp  \\reg, x12\n"
    "    cset x13, ne\n"
    "    orr  x0, x0, x13, lsl #\\bit\n"
    ".endm\n"
    ".macro GDXT_CHK_D reg, bit\n"
    "    add  x12, x12, x11\n"
    "    fmov x14, \\reg\n"
    "    cmp  x14, x12\n"
    "    cset x13, ne\n"
    "    orr  x0, x0, x13, lsl #\\bit\n"
    ".endm\n"

    "gdx_fiber_test_roundtrip:\n"
    /* 160 for our own callee-saved save area plus 16 for the two spilled arguments. */
    "    sub  sp, sp, #176\n"
    "    stp  x19, x20, [sp, #16]\n"
    "    stp  x21, x22, [sp, #32]\n"
    "    stp  x23, x24, [sp, #48]\n"
    "    stp  x25, x26, [sp, #64]\n"
    "    stp  x27, x28, [sp, #80]\n"
    "    stp  x29, x30, [sp, #96]\n"
    "    stp  d8,  d9,  [sp, #112]\n"
    "    stp  d10, d11, [sp, #128]\n"
    "    stp  d12, d13, [sp, #144]\n"
    "    stp  d14, d15, [sp, #160]\n"
    "    str  x0, [sp, #0]\n" /* to   */
    "    str  x1, [sp, #8]\n" /* seed */

    /* stride = 0x0101010101010101 */
    "    movz x11, #0x0101, lsl #48\n"
    "    movk x11, #0x0101, lsl #32\n"
    "    movk x11, #0x0101, lsl #16\n"
    "    movk x11, #0x0101\n"

    "    mov  x12, x1\n"
    "    GDXT_SET_X x19\n"
    "    GDXT_SET_X x20\n"
    "    GDXT_SET_X x21\n"
    "    GDXT_SET_X x22\n"
    "    GDXT_SET_X x23\n"
    "    GDXT_SET_X x24\n"
    "    GDXT_SET_X x25\n"
    "    GDXT_SET_X x26\n"
    "    GDXT_SET_X x27\n"
    "    GDXT_SET_X x28\n"
    "    GDXT_SET_D d8\n"
    "    GDXT_SET_D d9\n"
    "    GDXT_SET_D d10\n"
    "    GDXT_SET_D d11\n"
    "    GDXT_SET_D d12\n"
    "    GDXT_SET_D d13\n"
    "    GDXT_SET_D d14\n"
    "    GDXT_SET_D d15\n"

    "    ldr  x0, [sp, #0]\n"
    "    bl   gdx_fiber_switch\n" /* returns only when someone switches back to us */

    /* Rebuild x11 rather than assume it survived the switch. */
    "    movz x11, #0x0101, lsl #48\n"
    "    movk x11, #0x0101, lsl #32\n"
    "    movk x11, #0x0101, lsl #16\n"
    "    movk x11, #0x0101\n"
    "    ldr  x12, [sp, #8]\n" /* seed */
    "    mov  x0, xzr\n"
    "    GDXT_CHK_X x19, 0\n"
    "    GDXT_CHK_X x20, 1\n"
    "    GDXT_CHK_X x21, 2\n"
    "    GDXT_CHK_X x22, 3\n"
    "    GDXT_CHK_X x23, 4\n"
    "    GDXT_CHK_X x24, 5\n"
    "    GDXT_CHK_X x25, 6\n"
    "    GDXT_CHK_X x26, 7\n"
    "    GDXT_CHK_X x27, 8\n"
    "    GDXT_CHK_X x28, 9\n"
    "    GDXT_CHK_D d8,  10\n"
    "    GDXT_CHK_D d9,  11\n"
    "    GDXT_CHK_D d10, 12\n"
    "    GDXT_CHK_D d11, 13\n"
    "    GDXT_CHK_D d12, 14\n"
    "    GDXT_CHK_D d13, 15\n"
    "    GDXT_CHK_D d14, 16\n"
    "    GDXT_CHK_D d15, 17\n"

    "    ldp  x19, x20, [sp, #16]\n"
    "    ldp  x21, x22, [sp, #32]\n"
    "    ldp  x23, x24, [sp, #48]\n"
    "    ldp  x25, x26, [sp, #64]\n"
    "    ldp  x27, x28, [sp, #80]\n"
    "    ldp  x29, x30, [sp, #96]\n"
    "    ldp  d8,  d9,  [sp, #112]\n"
    "    ldp  d10, d11, [sp, #128]\n"
    "    ldp  d12, d13, [sp, #144]\n"
    "    ldp  d14, d15, [sp, #160]\n"
    "    add  sp, sp, #176\n"
    "    ret\n"
    ".size   gdx_fiber_test_roundtrip, .-gdx_fiber_test_roundtrip\n"

    ".purgem GDXT_SET_X\n"
    ".purgem GDXT_SET_D\n"
    ".purgem GDXT_CHK_X\n"
    ".purgem GDXT_CHK_D\n"
);
#else
#define GDX_FIBER_TEST_REGISTERS 0
#endif

#define NUM_FIBERS 4
#define ROUNDS 2000

typedef struct {
    int index;
    unsigned long seed;
    void* expectedArg;     /* the arg the fiber should have been handed */
    int argOk;             /* did it receive said arg */
    int entered;           /* has entry run */
    long resumes;          /* how many times this fiber was resumed */
    unsigned long badRegs; /* union of every mismatch mask seen */
    char stackProbe;       /* address taken */
    void* stackAddr;
} FiberCtx;

static GdxFiber* gHost = NULL;
static GdxFiber* gFibers[NUM_FIBERS];
static FiberCtx gCtx[NUM_FIBERS];

static void fiber_body(void* arg) {
    FiberCtx* c = (FiberCtx*)arg;

    c->entered = 1;
    c->argOk = (arg == c->expectedArg);
    c->stackAddr = (void*)&c->stackProbe;
    {
        volatile char onStack = (char)c->index;
        c->stackAddr = (void*)&onStack;
        (void)onStack;
    }

    for (;;) {
        c->resumes++;
#if GDX_FIBER_TEST_REGISTERS
        c->badRegs |= gdx_fiber_test_roundtrip(gHost, c->seed);
#else
        gdx_fiber_switch(gHost);
#endif
    }
}

int main(void) {
    int i;
    long r;
#if GDX_FIBER_TEST_REGISTERS
    unsigned long hostBad = 0;
#endif
    int allEntered = 1;
    int noneEnteredEarly = 1;
    int allArgsOk = 1;
    int allResumed = 1;
    int stacksDistinct = 1;

    gdx_test_console_out("gdx_fiber_tests.log");
    printf("=== gdx_fiber_tests ===\n");
#if GDX_FIBER_TEST_REGISTERS
    printf("backend under test: aarch64\n");
#else
    printf("backend under test: non-aarch64\n");
#endif

    gHost = gdx_fiber_convert_thread();
    check("gdx_fiber_convert_thread returns a context", gHost != NULL, "");
    if (gHost == NULL) {
        return 1;
    }

    memset(gCtx, 0, sizeof(gCtx));
    for (i = 0; i < NUM_FIBERS; ++i) {
        gCtx[i].index = i;
        gCtx[i].seed = 0x9E3779B97F4A7C15ull * (unsigned long)(i + 1);
        gCtx[i].expectedArg = &gCtx[i];
        gFibers[i] = gdx_fiber_create(fiber_body, &gCtx[i], 256u * 1024u);
        if (gFibers[i] == NULL) {
            check("gdx_fiber_create succeeds", 0, "allocation failed");
            return 1;
        }
    }
    check("gdx_fiber_create returns a context for each fiber", 1, "4 fibers, 256 KB stacks");

    for (i = 0; i < NUM_FIBERS; ++i) {
        if (gCtx[i].entered) {
            noneEnteredEarly = 0;
        }
    }
    check("entry does NOT run at create time", noneEnteredEarly, "no fiber entered before a switch");

    for (r = 0; r < ROUNDS; ++r) {
        for (i = 0; i < NUM_FIBERS; ++i) {
#if GDX_FIBER_TEST_REGISTERS
            /* Verify the host side too. */
            hostBad |= gdx_fiber_test_roundtrip(gFibers[i], 0xC0FFEE0000000000ull + (unsigned long)i);
#else
            gdx_fiber_switch(gFibers[i]);
#endif
        }
    }

    for (i = 0; i < NUM_FIBERS; ++i) {
        if (!gCtx[i].entered) {
            allEntered = 0;
        }
        if (!gCtx[i].argOk) {
            allArgsOk = 0;
        }
        if (gCtx[i].resumes != ROUNDS) {
            allResumed = 0;
        }
    }
    check("every fiber ran after its first switch", allEntered, "");
    check("every fiber received its own arg pointer", allArgsOk, "");
    {
        char detail[96];
        snprintf(detail, sizeof(detail), "%d fibers x %d rounds", NUM_FIBERS, (int)ROUNDS);
        check("every fiber resumed exactly ROUNDS times", allResumed, detail);
    }

    for (i = 0; i < NUM_FIBERS; ++i) {
        int j;
        for (j = i + 1; j < NUM_FIBERS; ++j) {
            if (gCtx[i].stackAddr == gCtx[j].stackAddr) {
                stacksDistinct = 0;
            }
        }
    }
    check("each fiber has its own stack", stacksDistinct, "local addresses all differ");

#if GDX_FIBER_TEST_REGISTERS
    {
        char detail[160];
        unsigned long anyBad = hostBad;
        for (i = 0; i < NUM_FIBERS; ++i) {
            anyBad |= gCtx[i].badRegs;
        }
        snprintf(detail, sizeof(detail), "mask=0x%05lX (bits 0-9 x19-x28, 10-17 d8-d15)", anyBad);
        check("callee-saved registers survive every switch", anyBad == 0, detail);

        snprintf(detail, sizeof(detail), "mask=0x%05lX", hostBad);
        check("  ... on the host side", hostBad == 0, detail);
    }
#endif

    printf("=== %s (%d failure%s) ===\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
           g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
