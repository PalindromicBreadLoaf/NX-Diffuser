/* G-Diffuser thin driver for the cxd4 RSP interpreter (LLE audio).
 *
 * cxd4's su.c is the scalar interpreter; the vu sources are the vector unit. We do
 * NOT vendor cxd4's module.c (mupen64plus plugin framework). This file supplies the
 * module-level symbols su.c references that module.c would normally define
 * (RSP_INFO_NAME, GBI_phase, and message/update_conf/export/no_LLE stubs; the
 * MFC0_count spin counters are already defined in su.c), plus a minimal "run one
 * audio task" entry point that boots the real aspMain microcode and executes the
 * game's Acmd command list — the LLE replacement for gdx_audio_hle_run.
 *
 * License note: cxd4 is CC0. This driver is original G-Diffuser code.
 */
#include <string.h>
#include <stdint.h>
#include <setjmp.h>

#include "rsp.h"
#include "su.h"
#include "module.h"

/* ---- module-level globals su.c/su.h expect (were in cxd4's module.c) ---- */
/* Note: MFC0_count, DRAM/DMEM/IMEM, CR[], conf[], SR[], MF_SP_STATUS_TIMEOUT are all
 * DEFINED in su.c -- do not redefine here. su.c's init_regs() is static in module.c
 * (not vendored); it only zeroes VU accumulator/flag globals, which are already
 * zero-initialized at load, so we skip it. */
RSP_INFO RSP_INFO_NAME;                              /* the RCP register/memory info */
p_func GBI_phase;                                    /* GFX LLE hook (unused for audio) */

/* su.c calls these (cxd4 module.c debug/plumbing) -- stub them out. */
void no_LLE(void) { }
NOINLINE void message(const char* body) { (void)body; }
NOINLINE void update_conf(const char* source) { (void)source; }
NOINLINE void export_data_cache(void) { }
NOINLINE void export_instruction_cache(void) { }
void export_SP_memory(void) { }

/* ---- our RSP backing store ---- */
static unsigned char sDMEM[4096];
static unsigned char sIMEM[4096];
static u32 sSP_MEM_ADDR, sSP_DRAM_ADDR, sSP_RD_LEN, sSP_WR_LEN, sSP_STATUS;
static u32 sSP_DMA_FULL, sSP_DMA_BUSY, sSP_PC, sSP_SEMAPHORE;
static u32 sDPC[8];
static u32 sMI_INTR;

/* cxd4 externs (su.c) we drive */
extern pu8 DRAM, DMEM, IMEM;
extern pu32 CR[];
extern u8 conf[];
extern int MF_SP_STATUS_TIMEOUT;
extern unsigned long su_max_address;   /* cxd4 SP-DMA upper bound; default 0x7FFFFF (8MB) */
extern short MFC0_count[];              /* per-scalar-reg MFC0 spin counters (su.c) */
extern void run_task(void);

/* ---- Watchdog: cxd4's run_task() loops until the ucode executes BREAK. A
 * mis-marshalled task could produce a ucode stream that never BREAKs, which would
 * spin the audio thread forever. Compiled with -DSP_EXECUTE_LOG, cxd4 calls
 * step_SP_commands() on every executed instruction; we count and longjmp out after a
 * generous cap (a real audio task is well under this). gdx_rsp_lle_run_task treats a
 * watchdog trip as "task did not complete" so the caller can discard it and fall back
 * to the HLE for that tick. ---- */
#define GDX_RSP_INSN_CAP 2000000L
static jmp_buf sWatchdog;
static long    sInsnCount;
static int     sCompleted;   /* 1 = last run_task reached BREAK; 0 = watchdog-tripped/no-BREAK */

/* The watchdog only fires because cxd4 calls step_SP_commands() per instruction, which su.c
 * gates on SP_EXECUTE_LOG. If this TU (compiled with the same target defines as su.c) is built
 * WITHOUT that flag, the hook is dead and a runaway/mis-marshalled task hangs the audio thread
 * with no recovery. Fail the build instead of shipping that. (The unit-test target defines
 * GDX_RSP_TEST_STEP_HOOK and supplies its own budgeted hook, so it is exempt.) */
#if !defined(SP_EXECUTE_LOG) && !defined(GDX_RSP_TEST_STEP_HOOK)
#error "gdx_rsp_driver.c requires SP_EXECUTE_LOG so the per-instruction watchdog is active."
#endif

/* The boot unit test provides its own step_SP_commands (it captures the instruction
 * stream); it defines GDX_RSP_TEST_STEP_HOOK so the driver's watchdog version is
 * excluded there and the two definitions don't collide at link time. */
#ifndef GDX_RSP_TEST_STEP_HOOK
void step_SP_commands(u32 inst) {
    (void)inst;
    if (++sInsnCount > GDX_RSP_INSN_CAP) {
        longjmp(sWatchdog, 1);
    }
}
#endif

/* Did the last gdx_rsp_lle_run_task complete (execute BREAK) rather than time out? */
int gdx_rsp_lle_completed(void) { return sCompleted; }

static void gdx_noop_checkintr(void) { }

/* Wire RSP_INFO once, pointing RDRAM at the game's flat gdx_rdram arena.
 * dmem/imem are our private 4KB buffers. Big-endian handling: memorySwapped==1
 * means the memory is stored host-endian (little), which matches gdx_rdram; the
 * ucode blobs loaded into IMEM/DMEM are byte-swapped to match (see loader below). */
void gdx_rsp_lle_init(unsigned char* rdram_base) {
    memset(&RSP_INFO_NAME, 0, sizeof(RSP_INFO_NAME));
    RSP_INFO_NAME.MemorySwapped   = 1;
    RSP_INFO_NAME.RDRAM           = (pu8)rdram_base;
    RSP_INFO_NAME.DMEM            = sDMEM;
    RSP_INFO_NAME.IMEM            = sIMEM;
    RSP_INFO_NAME.MI_INTR_REG     = &sMI_INTR;
    RSP_INFO_NAME.SP_MEM_ADDR_REG = &sSP_MEM_ADDR;
    RSP_INFO_NAME.SP_DRAM_ADDR_REG= &sSP_DRAM_ADDR;
    RSP_INFO_NAME.SP_RD_LEN_REG   = &sSP_RD_LEN;
    RSP_INFO_NAME.SP_WR_LEN_REG   = &sSP_WR_LEN;
    RSP_INFO_NAME.SP_STATUS_REG   = &sSP_STATUS;
    RSP_INFO_NAME.SP_DMA_FULL_REG = &sSP_DMA_FULL;
    RSP_INFO_NAME.SP_DMA_BUSY_REG = &sSP_DMA_BUSY;
    RSP_INFO_NAME.SP_PC_REG       = &sSP_PC;
    RSP_INFO_NAME.SP_SEMAPHORE_REG= &sSP_SEMAPHORE;
    RSP_INFO_NAME.DPC_START_REG   = &sDPC[0];
    RSP_INFO_NAME.DPC_END_REG     = &sDPC[1];
    RSP_INFO_NAME.DPC_CURRENT_REG = &sDPC[2];
    RSP_INFO_NAME.DPC_STATUS_REG  = &sDPC[3];
    RSP_INFO_NAME.DPC_CLOCK_REG   = &sDPC[4];
    RSP_INFO_NAME.DPC_BUFBUSY_REG = &sDPC[5];
    RSP_INFO_NAME.DPC_PIPEBUSY_REG= &sDPC[6];
    RSP_INFO_NAME.DPC_TMEM_REG    = &sDPC[7];
    RSP_INFO_NAME.CheckInterrupts = gdx_noop_checkintr;

    DRAM = RSP_INFO_NAME.RDRAM;
    DMEM = RSP_INFO_NAME.DMEM;
    IMEM = RSP_INFO_NAME.IMEM;

    CR[0x0] = RSP_INFO_NAME.SP_MEM_ADDR_REG;
    CR[0x1] = RSP_INFO_NAME.SP_DRAM_ADDR_REG;
    CR[0x2] = RSP_INFO_NAME.SP_RD_LEN_REG;
    CR[0x3] = RSP_INFO_NAME.SP_WR_LEN_REG;
    CR[0x4] = RSP_INFO_NAME.SP_STATUS_REG;
    CR[0x5] = RSP_INFO_NAME.SP_DMA_FULL_REG;
    CR[0x6] = RSP_INFO_NAME.SP_DMA_BUSY_REG;
    CR[0x7] = RSP_INFO_NAME.SP_SEMAPHORE_REG;
    CR[0x8] = RSP_INFO_NAME.DPC_START_REG;
    CR[0x9] = RSP_INFO_NAME.DPC_END_REG;
    CR[0xA] = RSP_INFO_NAME.DPC_CURRENT_REG;
    CR[0xB] = RSP_INFO_NAME.DPC_STATUS_REG;
    CR[0xC] = RSP_INFO_NAME.DPC_CLOCK_REG;
    CR[0xD] = RSP_INFO_NAME.DPC_BUFBUSY_REG;
    CR[0xE] = RSP_INFO_NAME.DPC_PIPEBUSY_REG;
    CR[0xF] = RSP_INFO_NAME.DPC_TMEM_REG;
    MF_SP_STATUS_TIMEOUT = 32767;
    /* Raise the SP-DMA address ceiling from cxd4's 8MB default to the full 16MB gdx_rdram
     * (cxd4 masks DMA addresses to 24 bits). Without this, any audio buffer the bridge
     * stages above 8MB -- e.g. the scratch carved from the TOP of RDRAM -- DMAs as ZEROS
     * (su.c SP_DMA_READ: offD > su_max_address => memset 0), which fed the ucode a garbage
     * command stream and spun it forever. */
    su_max_address = 0x00FFFFFFul;   /* 16MB - 1 == cxd4's 24-bit DMA max == gdx_rdram size */
    GBI_phase = no_LLE;
    conf[0x00] = 1; /* CFG_HLE_GFX: bounce any gfx task (we never feed one) */
    conf[0x01] = 0; /* CFG_HLE_AUD: LLE audio -- run the real ucode */
}

/* Load aspMain into IMEM and execute one audio task.
 * cxd4 stores IMEM/DMEM/RDRAM as HOST-NATIVE little-endian 32-bit words (BES/HES
 * element-swap, ENDIAN_M=~0), so:
 *   ucodeText : big-endian aspMain text blob (raw ROM order) -> byte-swapped per
 *               32-bit word into IMEM (host-native).
 *   ucodeData : written to DMEM[0] as a convenience, but aspMain's boot code re-DMAs
 *               its data section from OSTask.ucode_data (RDRAM) over DMEM[0] anyway --
 *               so the CALLER must place a byte-swapped-to-native copy of this blob in
 *               RDRAM and set OSTask.ucode_data/ucode_data_size. (We load it here byte-
 *               swapped too, so a task with ucode_data==0 still has a sane DMEM[0].)
 *   dmemTaskHeader : 64-byte OSTask for DMEM[0xFC0], fields HOST-NATIVE little-endian
 *               (type, ucode_data, ucode_data_size, data_ptr, data_size). Addresses are
 *               physical RDRAM offsets.
 * Runs until the task-complete BREAK sets SP_STATUS_BROKE. */
void gdx_rsp_lle_run_task(const void* ucodeText, unsigned textBytes,
                          const void* ucodeData, unsigned dataBytes,
                          const void* dmemTaskHeader) {
    const unsigned char* t = (const unsigned char*)ucodeText;
    unsigned i;
    if (textBytes > 4096u) textBytes = 4096u;
    if (dataBytes > 0xFC0u) dataBytes = 0xFC0u;
    memset(sIMEM, 0, sizeof(sIMEM));
    for (i = 0; i + 4u <= textBytes; i += 4u) {   /* BE -> host-native 32-bit swap */
        sIMEM[i + 0] = t[i + 3];
        sIMEM[i + 1] = t[i + 2];
        sIMEM[i + 2] = t[i + 1];
        sIMEM[i + 3] = t[i + 0];
    }
    {   /* DMEM data section: same BE -> host-native 32-bit swap (see header note) */
        const unsigned char* d = (const unsigned char*)ucodeData;
        for (i = 0; i + 4u <= dataBytes; i += 4u) {
            sDMEM[i + 0] = d[i + 3];
            sDMEM[i + 1] = d[i + 2];
            sDMEM[i + 2] = d[i + 1];
            sDMEM[i + 3] = d[i + 0];
        }
    }
    memcpy(sDMEM + 0xFC0, dmemTaskHeader, 64);
    sSP_STATUS = 0;
    sSP_PC = 0;
    *RSP_INFO_NAME.SP_PC_REG = 0;
    sInsnCount = 0;
    /* Reset cxd4's MFC0 spin counters every run. Stock cxd4 does this per task inside the
     * (un-vendored) module.c DoRspCycles; without it MFC0_count[] accumulates for the life
     * of the process and, once it reaches MF_SP_STATUS_TIMEOUT, spuriously force-HALTs a
     * later task with no BREAK -- which the BROKE gate below would then (correctly) route to
     * the HLE fallback, but resetting it keeps the RSP faithful. */
    memset(MFC0_count, 0, sizeof(short) * (size_t)NUMBER_OF_SCALAR_REGISTERS);
    if (setjmp(sWatchdog) == 0) {
        run_task();
        /* "Completed" means the ucode executed the task-complete BREAK -- NOT merely that
         * run_task() returned. cxd4 also returns on an early HALT-without-BROKE (MFC0/
         * semaphore/interrupt halt paths); those must NOT be trusted as a finished task, or
         * the caller would copy a partial DMEM state to the speakers. Require SP_STATUS_BROKE. */
        sCompleted = (sSP_STATUS & SP_STATUS_BROKE) ? 1 : 0;
    } else {
        sCompleted = 0;        /* watchdog tripped: runaway/mis-marshalled task */
    }
}

/* Post-run introspection for tests. */
unsigned gdx_rsp_lle_status(void) { return sSP_STATUS; }
const unsigned char* gdx_rsp_lle_dmem(void) { return sDMEM; }
