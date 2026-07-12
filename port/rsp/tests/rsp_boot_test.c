/* LLE audio core unit test: proves the vendored cxd4 RSP interpreter boots and runs
 * the REAL aspMain audio microcode (nead/ABI2) to a clean task-complete BREAK, driven
 * entirely through the driver shim's public entry points.
 *
 * Task setup (established by reverse-engineering the nead command dispatcher):
 *   - The ucode's OWN boot code DMAs OSTask.ucode_data (ucode_data_size bytes) from
 *     RDRAM into DMEM[0], rebuilding its runtime jump table at DMEM[0x10]. So the data
 *     section (aspmain_data.bin, 0x2E0 bytes) MUST live in RDRAM with ucode_data(+0x18)/
 *     ucode_data_size(+0x1C) pointing at it -- a manual DMEM[0] preload alone is clobbered.
 *   - The dispatcher advances a running RDRAM pointer (data_ptr) and decrements a byte
 *     counter k1 = data_size by 8 per command; when blez k1, it sets SP_STATUS SET_SIG2
 *     (0x4000) and executes BREAK at IMEM 0x10D0. Termination is the data_size counter,
 *     NOT a terminator opcode or fixed count.
 *   - Command opcode 0x00 is a HEAVY handler; the true no-op (loop-continue) opcodes are
 *     0x03/0x06/0x07/0x09/0x0E/0x18/0x19/0x1B/0x1C/0x1D. We use 0x03.
 *   - All multi-byte OSTask fields and command words are big-endian (RSP reads RDRAM/DMEM
 *     big-endian via cxd4's BES element-swap).
 *
 * Asserts:
 *   1. First executed instruction is 0x200a0fc0 (ADDI $10,$0,0xFC0) -> IMEM BE->LE swap
 *      + boot shortcut correct.
 *   2. The run reaches BREAK with SP_STATUS_BROKE set (and SET_SIG2 = task-complete) ->
 *      the LLE core executes a full audio task to completion.
 * A high instruction budget (via cxd4's SP_EXECUTE_LOG hook) bounds the run so a
 * regression that fails to terminate is caught as a FAIL rather than hanging.
 */
#include <stdio.h>
#include <string.h>
#include <setjmp.h>

extern void gdx_rsp_lle_init(unsigned char*);
extern void gdx_rsp_lle_run_task(const void*, unsigned, const void*, unsigned, const void*);
extern unsigned gdx_rsp_lle_status(void);

#ifndef RSP_BLOB_DIR
#define RSP_BLOB_DIR "."
#endif

typedef unsigned int u32;

#define SP_STATUS_BROKE  0x0002u
#define SP_STATUS_SIG2   0x4000u

/* RDRAM layout for the offline test */
#define RD_UCODE_DATA  0x1000u   /* aspmain_data.bin copy (ucode_data) */
#define RD_CMD_LIST    0x2000u   /* command list (data_ptr), 8-byte aligned */

static jmp_buf g_bail;
static u32 g_log[512];
static int g_n = 0;
static int g_overrun = 0;
#define RUN_BUDGET 20000   /* correct BREAK hits in <200 steps; only a hang reaches this */

void step_SP_commands(u32 inst) {
    if (g_n < (int)(sizeof g_log / sizeof g_log[0]))
        g_log[g_n] = inst;
    g_n++;
    if (g_n >= RUN_BUDGET) { g_overrun = 1; longjmp(g_bail, 1); }
}

static unsigned load_blob(const char* name, unsigned char* buf, unsigned cap) {
    char path[1024];
    FILE* f;
    unsigned n;
    snprintf(path, sizeof path, "%s/%s", RSP_BLOB_DIR, name);
    f = fopen(path, "rb");
    if (!f) return 0;
    n = (unsigned)fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static unsigned char rdram[0x1000000];
static unsigned char text[8192], data[4096], ostask[64];

/* cxd4 stores DMEM/RDRAM/IMEM as host-native little-endian 32-bit words (its BES/HES
 * element-swap macros, ENDIAN_M=~0, reconstruct big-endian byte/half access from that
 * storage). So values written for the RSP to read via LW are stored host-native... */
static void le32(unsigned char* p, unsigned v) {
    p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
/* ...and N64 big-endian ROM blobs are byte-swapped per 32-bit word on the way in. */
static void copy_be_words_to_native(unsigned char* dst, const unsigned char* src, unsigned n) {
    unsigned i;
    for (i = 0; i + 4u <= n; i += 4u) {
        dst[i + 0] = src[i + 3]; dst[i + 1] = src[i + 2];
        dst[i + 2] = src[i + 1]; dst[i + 3] = src[i + 0];
    }
}

int main(void) {
    unsigned tn = load_blob("aspmain_text.bin", text, sizeof text);
    unsigned dn = load_blob("aspmain_data.bin", data, sizeof data);
    const unsigned NCMD = 8;
    unsigned data_size = NCMD * 8;
    unsigned i, st, saw_entry;

    if (!tn || !dn) {
        printf("SKIP: ucode blobs not present -- LLE boot test skipped.\n");
        return 0; /* ROM-extracted blobs may be absent in a bare checkout */
    }

    /* RDRAM: the data section (byte-swapped BE->native words) that the ucode's boot DMA
     * pulls into DMEM to rebuild its jump table... */
    memset(rdram, 0, 0x4000);
    copy_be_words_to_native(rdram + RD_UCODE_DATA, data, dn);
    /* ...and a command list of no-op packets. Opcode 0x03 is a true loop-continue no-op
     * (opcode 0x00 is a heavy handler); stored host-native so w0>>24 == 0x03. */
    for (i = 0; i < NCMD; i++)
        le32(rdram + RD_CMD_LIST + i * 8, 0x03000000u); /* w0: opcode 0x03; w1 stays 0 */

    memset(ostask, 0, sizeof ostask);
    le32(ostask + 0x00, 2);              /* type = M_AUDTASK */
    le32(ostask + 0x18, RD_UCODE_DATA);  /* ucode_data -> RDRAM data-section copy */
    le32(ostask + 0x1C, dn);             /* ucode_data_size = 0x2E0 (736) */
    le32(ostask + 0x30, RD_CMD_LIST);    /* data_ptr -> command list */
    le32(ostask + 0x34, data_size);      /* data_size = 0x40 (counts down to BREAK) */

    gdx_rsp_lle_init(rdram);
    if (setjmp(g_bail) == 0)
        gdx_rsp_lle_run_task(text, tn, data, dn, ostask); /* returns on BREAK */
    st = gdx_rsp_lle_status();

    saw_entry = (g_n > 0 && g_log[0] == 0x200a0fc0u);

    printf("executed %d instrs; entry=%d overrun=%d SP_STATUS=0x%08x (BROKE=%d SIG2=%d)\n",
           g_n, saw_entry, g_overrun, st,
           (st & SP_STATUS_BROKE) != 0, (st & SP_STATUS_SIG2) != 0);

    if (!saw_entry) {
        printf("FAIL: entry marker missing (IMEM byte-swap/boot wrong).\n");
        return 1;
    }
    if (g_overrun || !(st & SP_STATUS_BROKE)) {
        printf("FAIL: ucode did not reach task-complete BREAK.\n");
        return 1;
    }
    printf("PASS: cxd4 LLE core boots and runs the real aspMain ucode to a clean BREAK.\n");
    return 0;
}
