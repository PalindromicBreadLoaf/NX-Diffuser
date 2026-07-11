/*
 * gfx_pack_tests.c -- Phase G1 unit tests for the wide (pointer-width) display-list
 * word.  Standalone console exe: no libultraship, no game objects, no bridge object.
 *
 * It compiles the REAL decomp gbi.h with the exact defines the game build uses
 * (PORT, F3DEX_GBI_2, _LANGUAGE_C, VERSION_US) so the gSP and gDP macros under
 * test are the same ones the game emits at runtime.  It then verifies:
 *
 *   1. sizeof(Gfx) == 16 and the wide layout places w0 at byte 0 and the pointer
 *      word w1 at byte 8 (matching the bridge's kHostBuiltGfxStride == 16 read).
 *   2. gSPVertex / gSPDisplayList / gSPMatrix / gDPSetTextureImage / gSPBranchList
 *      each pack a FULL 64-bit host pointer into w1 with zero truncation, and a
 *      bridge-style reader (w0 @0, w1 @8) recovers the exact pointer.
 *   3. Segmented addresses (e.g. 0x02000000) round-trip unchanged and keep their
 *      high 32 bits zero -- the signal the bridge uses to route them through the
 *      segment table instead of treating them as host pointers.
 *   4. A mixed synthetic display list walks correctly: right command count and a
 *      terminating ENDDL, with each command's opcode read from w0[31:24].
 *
 * Returns 0 iff every check passes; non-zero (and prints [FAIL]) otherwise.
 */

/* mbi.h supplies _SHIFTL / G_MAXZ and then includes <PR/gbi.h>, exactly as the
 * game build reaches gbi.h. */
#include <PR/mbi.h>
/* sptask.h: the OSTask whose t.data_ptr carries the ROOT display list to the
 * bridge. The soak-fix relies on this field being a full-width u64* (never a
 * truncated low32), so assert its type/width here in the standalone harness. */
#include <PR/sptask.h>
/* R4300.h supplies K0_TO_PHYS. Campaign-soak-fix-4 proves the PORT branch passes
 * the FULL host pointer width through (not (u32)-truncated), so the classic
 * gSPMatrix(gfx, K0_TO_PHYS(&mtx), ...) idiom packs a real >4GB host pointer. */
#include <PR/R4300.h>

#include <stdio.h>
#include <string.h>

/* Match the bridge's wide read exactly: host-built packets are 16 bytes, w0 is a
 * 32-bit little-endian word at byte 0, and the pointer word is a full-width value
 * at byte 8.  See port/n64_gfx_bridge.cpp (kHostBuiltGfxStride / sourceIsWide). */
#define HOST_GFX_STRIDE 16u

typedef unsigned long long u64_t;

static int g_failures = 0;

static void check_u64(const char* name, u64_t got, u64_t want) {
    if (got == want) {
        printf("[ OK ] %-34s got=0x%016llX\n", name, (unsigned long long)got);
    } else {
        printf("[FAIL] %-34s got=0x%016llX want=0x%016llX\n",
               name, (unsigned long long)got, (unsigned long long)want);
        ++g_failures;
    }
}

static void check_sz(const char* name, size_t got, size_t want) {
    if (got == want) {
        printf("[ OK ] %-34s got=%zu\n", name, got);
    } else {
        printf("[FAIL] %-34s got=%zu want=%zu\n", name, got, want);
        ++g_failures;
    }
}

/* Bridge-style raw reader: pull the opcode and the full pointer word straight out
 * of the packet bytes, using the wide 16-byte stride and byte offset 8 for w1.
 * This deliberately does NOT read the typed struct field -- it proves the on-wire
 * layout the bridge relies on is what the macros actually produce. */
static u64_t read_w1_full(const void* base, size_t index) {
    u64_t w1 = 0;
    memcpy(&w1, (const unsigned char*)base + index * HOST_GFX_STRIDE + 8, sizeof(w1));
    return w1;
}
static unsigned int read_w0(const void* base, size_t index) {
    unsigned int w0 = 0;
    memcpy(&w0, (const unsigned char*)base + index * HOST_GFX_STRIDE, sizeof(w0));
    return w0;
}
static unsigned int read_opcode(const void* base, size_t index) {
    return (read_w0(base, index) >> 24) & 0xFFu;
}

int main(void) {
    printf("=== Phase G1 wide-Gfx packing tests ===\n");

    /* 1. Layout invariants. */
    check_sz("sizeof(Gfx)", sizeof(Gfx), 16);
    check_sz("sizeof(Gwords)", sizeof(Gwords), 16);
    {
        Gfx probe;
        memset(&probe, 0, sizeof(probe));
        probe.words.w0 = 0xAABBCCDDu;
        probe.words.w1 = 0x1122334455667788ULL;
        check_u64("w0 @ byte 0", (u64_t)read_w0(&probe, 0), 0xAABBCCDDu);
        check_u64("w1 @ byte 8 (full pointer word)", read_w1_full(&probe, 0), 0x1122334455667788ULL);
    }

    /* A fabricated high host address (bit 46 set) proves all 64 bits survive; the
     * old (unsigned int) cast would have truncated this to 0xABCD1234. */
    void* const kHostPtr = (void*)(u64_t)0x00007FF6ABCD1234ULL;
    void* const kDlPtr   = (void*)(u64_t)0x00007FFEDEADBEEFULL;
    void* const kTexPtr  = (void*)(u64_t)0x0000023400001000ULL;
    Mtx   dummyMtx;
    Vtx   dummyVtx[4];

    /* 2. Each pointer-family macro packs the full pointer into w1. */
    {
        Gfx cmd;
        memset(&cmd, 0, sizeof(cmd));
        gSPVertex(&cmd, (Vtx*)kHostPtr, 4, 0);
        check_u64("gSPVertex w1 == host ptr", read_w1_full(&cmd, 0), (u64_t)kHostPtr);
        check_u64("gSPVertex opcode G_VTX", (u64_t)read_opcode(&cmd, 0), G_VTX);
    }
    {
        Gfx cmd;
        memset(&cmd, 0, sizeof(cmd));
        gSPDisplayList(&cmd, (Gfx*)kDlPtr);
        check_u64("gSPDisplayList w1 == host ptr", read_w1_full(&cmd, 0), (u64_t)kDlPtr);
        check_u64("gSPDisplayList opcode G_DL", (u64_t)read_opcode(&cmd, 0), (u64_t)G_DL);
    }
    {
        Gfx cmd;
        memset(&cmd, 0, sizeof(cmd));
        gSPBranchList(&cmd, (Gfx*)kDlPtr);
        check_u64("gSPBranchList w1 == host ptr", read_w1_full(&cmd, 0), (u64_t)kDlPtr);
        check_u64("gSPBranchList opcode G_DL", (u64_t)read_opcode(&cmd, 0), (u64_t)G_DL);
    }
    {
        Gfx cmd;
        memset(&cmd, 0, sizeof(cmd));
        gSPMatrix(&cmd, (Mtx*)kHostPtr, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
        check_u64("gSPMatrix w1 == host ptr", read_w1_full(&cmd, 0), (u64_t)kHostPtr);
        check_u64("gSPMatrix opcode G_MTX", (u64_t)read_opcode(&cmd, 0), (u64_t)G_MTX);
    }
    {
        Gfx cmd;
        memset(&cmd, 0, sizeof(cmd));
        gDPSetTextureImage(&cmd, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, kTexPtr);
        check_u64("gDPSetTextureImage w1 == host ptr", read_w1_full(&cmd, 0), (u64_t)kTexPtr);
    }

    /* Real object addresses (not just fabricated ones) also survive intact. */
    {
        Gfx cmd;
        memset(&cmd, 0, sizeof(cmd));
        gSPVertex(&cmd, &dummyVtx[0], 4, 0);
        check_u64("gSPVertex real &Vtx", read_w1_full(&cmd, 0), (u64_t)(void*)&dummyVtx[0]);
        gSPMatrix(&cmd, &dummyMtx, G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_PUSH);
        check_u64("gSPMatrix real &Mtx", read_w1_full(&cmd, 0), (u64_t)(void*)&dummyMtx);
    }

    /* Campaign-soak-fix-4: the K0_TO_PHYS(...) matrix idiom (course_model.c /
     * course_gadgets.c) must pack the FULL host pointer, not a high32=0 truncation.
     * The pre-fix `(u32)(uintptr_t)` PORT macro dropped the high 32 bits, so the
     * bridge saw high32==0, ran the guessing resolver, hit fallback_buffer, and the
     * matrix loaded garbage -> invisible 3D models (832 [datafail] op=DA/frame). */
    {
        Gfx cmd;
        memset(&cmd, 0, sizeof(cmd));
        gSPMatrix(&cmd, (Mtx*)K0_TO_PHYS(kHostPtr), G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
        check_u64("gSPMatrix K0_TO_PHYS full ptr", read_w1_full(&cmd, 0), (u64_t)kHostPtr);
        check_u64("gSPMatrix K0_TO_PHYS high32!=0", (read_w1_full(&cmd, 0) >> 32) != 0, 1);
    }
    {
        Gfx cmd;
        memset(&cmd, 0, sizeof(cmd));
        gSPMatrix(&cmd, (Mtx*)K0_TO_PHYS(&dummyMtx), G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH);
        check_u64("gSPMatrix K0_TO_PHYS real &Mtx", read_w1_full(&cmd, 0), (u64_t)(void*)&dummyMtx);
    }

    /* 3. Segmented address round-trip: a 32-bit value stays 32-bit, high half zero. */
    {
        Gfx cmd;
        const unsigned int kSeg = 0x02000000u; /* segment 2, offset 0 */
        memset(&cmd, 0, sizeof(cmd));
        gSPDisplayList(&cmd, (Gfx*)(u64_t)kSeg);
        check_u64("segmented DL low32 preserved", read_w1_full(&cmd, 0) & 0xFFFFFFFFu, kSeg);
        check_u64("segmented DL high32 == 0 (seg path)", read_w1_full(&cmd, 0) >> 32, 0);
    }
    {
        Gfx cmd;
        const unsigned int kSeg = 0x0A001000u; /* segment 0x0A venue texture bank */
        memset(&cmd, 0, sizeof(cmd));
        gDPSetTextureImage(&cmd, G_IM_FMT_RGBA, G_IM_SIZ_16b, 64, (void*)(u64_t)kSeg);
        check_u64("segmented TIMG low32 preserved", read_w1_full(&cmd, 0) & 0xFFFFFFFFu, kSeg);
        check_u64("segmented TIMG high32 == 0", read_w1_full(&cmd, 0) >> 32, 0);
    }

    /* 4. Mixed synthetic display list walk: build, then walk via the bridge-style
     *    reader counting commands until ENDDL. */
    {
        Gfx dl[8];
        Gfx* p = dl;
        memset(dl, 0, sizeof(dl));
        gSPMatrix(p++, (Mtx*)kHostPtr, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_PUSH);
        gSPVertex(p++, (Vtx*)kHostPtr, 4, 0);
        gDPSetTextureImage(p++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, kTexPtr);
        gSPDisplayList(p++, (Gfx*)kDlPtr);
        gSPEndDisplayList(p++);
        const size_t built = (size_t)(p - dl);
        check_sz("mixed DL built command count", built, 5);

        size_t walked = 0;
        int sawEnd = 0;
        for (size_t i = 0; i < (sizeof(dl) / sizeof(dl[0])); ++i) {
            unsigned int op = read_opcode(dl, i);
            ++walked;
            if (op == (G_ENDDL & 0xFFu)) { sawEnd = 1; break; }
        }
        check_sz("mixed DL walked to ENDDL", walked, 5);
        check_u64("mixed DL ENDDL seen", (u64_t)sawEnd, 1);
        /* Spot-check pointer fidelity mid-list after the walk. */
        check_u64("walk: cmd0 (MTX) ptr", read_w1_full(dl, 0), (u64_t)kHostPtr);
        check_u64("walk: cmd3 (DL) ptr", read_w1_full(dl, 3), (u64_t)kDlPtr);
    }

    /* 5. ROOT display-list pointer carry (campaign soak fix).
     *    Gfx_SetTask assigns `task->t.data_ptr = (u64*) gGfxPool->gfxBuffer;`
     *    and osSpTaskStartGo feeds tp->t.data_ptr straight to the bridge. That
     *    field MUST carry the full 64-bit host pointer -- if it were ever
     *    narrowed to 32 bits, the root DL would fall to the module-window guess
     *    (the first-frame crash). Prove the field is pointer-width and a
     *    fabricated >4GB pool pointer survives a store/load round-trip. */
    {
        OSTask task;
        memset(&task, 0, sizeof(task));
        check_sz("OSTask_t.data_ptr is pointer-width", sizeof(task.t.data_ptr), sizeof(void*));
        check_sz("host pointer is 64-bit", sizeof(void*), 8);
        /* Simulate Gfx_SetTask's assignment with a high (>4GB) pool address. */
        task.t.data_ptr = (u64*)kHostPtr;
        check_u64("root data_ptr carries full 64-bit ptr",
                  (u64_t)(size_t)task.t.data_ptr, (u64_t)kHostPtr);
        check_u64("root data_ptr high32 preserved (>4GB)",
                  (u64_t)((size_t)task.t.data_ptr >> 32), (u64_t)kHostPtr >> 32);
    }

    /* 6. gSPSegment wide round-trip (campaign soak fix #2).
     *    Under F3DEX_GBI_2 gSPSegment(seg, base) expands to
     *      gMoveWd(G_MW_SEGMENT, seg*4, base) -> gDma1p(G_MOVEWORD, base, seg*4, G_MW_SEGMENT)
     *    so the segment BASE is packed through the SAME widened gDma1p / _GFXW1_PTR
     *    path as gSPVertex above -- the full 64-bit host pointer, NOT a truncated
     *    low32. The segment table is the central base for ALL segmented addressing;
     *    a truncated base cascades into missing textures and garbage geometry.
     *    Verify BOTH the on-wire pointer fidelity AND the exact w0 field layout the
     *    bridge parses (index @ w0[23:16] == G_MW_SEGMENT, seg*4 @ w0[15:0]). */
    {
        Gfx cmd;
        const unsigned int kSegNo = 8u; /* segment 8: course_track_gfx base */
        memset(&cmd, 0, sizeof(cmd));
        gSPSegment(&cmd, kSegNo, kHostPtr);
        check_u64("gSPSegment opcode G_MOVEWORD",
                  (u64_t)read_opcode(&cmd, 0), (u64_t)G_MOVEWORD);
        check_u64("gSPSegment index == G_MW_SEGMENT",
                  (u64_t)((read_w0(&cmd, 0) >> 16) & 0xFFu), (u64_t)G_MW_SEGMENT);
        check_u64("gSPSegment offset == seg*4",
                  (u64_t)(read_w0(&cmd, 0) & 0xFFFFu), (u64_t)(kSegNo * 4u));
        check_u64("gSPSegment base == FULL host ptr (no low32 truncation)",
                  read_w1_full(&cmd, 0), (u64_t)kHostPtr);
        check_u64("gSPSegment base high32 preserved (>4GB)",
                  read_w1_full(&cmd, 0) >> 32, (u64_t)kHostPtr >> 32);
    }
    {
        /* A 32-bit segmented/physical base (e.g. RDRAM offset) keeps high32 == 0,
         * the exact signal the bridge uses to route it through the resolver/segment
         * path instead of treating it as an already-resolved host pointer. */
        Gfx cmd;
        const unsigned int kPhysBase = 0x80200000u; /* KSEG0 physical base */
        memset(&cmd, 0, sizeof(cmd));
        gSPSegment(&cmd, 3u, (void*)(u64_t)kPhysBase);
        check_u64("gSPSegment 32-bit base low32 preserved",
                  read_w1_full(&cmd, 0) & 0xFFFFFFFFu, kPhysBase);
        check_u64("gSPSegment 32-bit base high32 == 0",
                  read_w1_full(&cmd, 0) >> 32, 0);
    }

    printf("=== %s (%d failure%s) ===\n",
           g_failures == 0 ? "ALL PASS" : "FAILURES",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
