/* Horizon CPU exception handler */

#ifdef __SWITCH__

#include <switch.h>

#include <stdint.h>
#include <string.h>

#include "gdx_switch_memquery.h"
#include "port_log.h"

alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

static char sReport[4096];
static size_t sReportLen;

static void Emit(const char* s) {
    size_t n = strlen(s);
    if (sReportLen + n >= sizeof(sReport)) {
        n = sizeof(sReport) - 1 - sReportLen;
    }
    memcpy(sReport + sReportLen, s, n);
    sReportLen += n;
    sReport[sReportLen] = '\0';
}

static void EmitHex(u64 v) {
    char buf[19];
    int i;

    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 16; i++) {
        buf[2 + i] = "0123456789ABCDEF"[(v >> (60 - i * 4)) & 0xF];
    }
    buf[18] = '\0';
    Emit(buf);
}

static void EmitField(const char* name, u64 v) {
    Emit(name);
    Emit("=");
    EmitHex(v);
}

static const char* DescribeError(u32 desc) {
    switch (desc) {
        case ThreadExceptionDesc_InstructionAbort: return "instruction abort";
        case ThreadExceptionDesc_MisalignedPC:     return "misaligned PC";
        case ThreadExceptionDesc_MisalignedSP:     return "misaligned SP";
        case ThreadExceptionDesc_SError:           return "SError";
        case ThreadExceptionDesc_BadSVC:           return "bad SVC";
        case ThreadExceptionDesc_Trap:             return "trap";
        case ThreadExceptionDesc_Other:            return "other (data abort)";
        default:                                   return "unknown";
    }
}

/*  The RVA below is what `aarch64-none-elf-nm G-Diffuser.elf` can be searched against. */
static void EmitRva(const char* name, u64 addr, uintptr_t moduleBegin, uintptr_t moduleEnd) {
    Emit(name);
    if (addr >= (u64)moduleBegin && addr < (u64)moduleEnd) {
        Emit("=");
        EmitHex(addr - (u64)moduleBegin);
    } else {
        Emit("=outside-module");
    }
}

void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    uintptr_t moduleBegin = 0;
    uintptr_t moduleEnd = 0;
    int i;

    sReportLen = 0;
    sReport[0] = '\0';

    if (ctx == NULL) {
        Emit("\n==== gdiffuser-switch fault ====\n");
        gdx_crash_report_write(sReport);
        return;
    }

    gdx_switch_module_range(&moduleBegin, &moduleEnd);

    Emit("\n==== gdiffuser-switch fault ====\n[crash] ");
    Emit(DescribeError(ctx->error_desc));
    Emit(" desc=");
    EmitHex(ctx->error_desc);
    Emit(threadExceptionIsAArch64(ctx) ? " aarch64\n" : " aarch32\n");

    Emit("[crash] ");
    EmitField("pc", ctx->pc.x);
    Emit(" ");
    EmitField("lr", ctx->lr.x);
    Emit(" ");
    EmitField("sp", ctx->sp.x);
    Emit(" ");
    EmitField("fp", ctx->fp.x);
    Emit("\n");

    Emit("[crash] ");
    EmitField("far", ctx->far.x);
    Emit(" ");
    EmitField("esr", ctx->esr);
    Emit(" ");
    EmitField("pstate", ctx->pstate);
    Emit("\n");

    Emit("[crash] ");
    EmitField("module", (u64)moduleBegin);
    Emit("-");
    EmitHex((u64)moduleEnd);
    Emit(" ");
    EmitRva("pc_rva", ctx->pc.x, moduleBegin, moduleEnd);
    Emit(" ");
    EmitRva("lr_rva", ctx->lr.x, moduleBegin, moduleEnd);
    Emit("\n");

    for (i = 0; i < 29; i++) {
        char name[5];
        if (i % 4 == 0) {
            Emit("[crash] ");
        }
        name[0] = 'x';
        if (i < 10) {
            name[1] = (char)('0' + i);
            name[2] = '\0';
        } else {
            name[1] = (char)('0' + i / 10);
            name[2] = (char)('0' + i % 10);
            name[3] = '\0';
        }
        EmitField(name, ctx->cpu_gprs[i].x);
        Emit((i % 4 == 3 || i == 28) ? "\n" : " ");
    }

    gdx_crash_report_write(sReport);
}

#endif /* __SWITCH__ */
