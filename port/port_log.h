#pragma once

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// gdx_ck()/gdx_cki()/gdx_ckp()/gdx_seg_log()/gdx_addr_log() (see n64_sched.c) are per-frame,
// unconditional trace breadcrumbs sprinkled through the decomp sources (every VI frame emits
// ~14 of them from sys_gfx.c's func_80067D64 + func_800690FC alone; a game-mode transition adds
// another half dozen from game.c's GMI_* checkpoints). Each call reaches gdx_port_write_log(),
// which does OutputDebugStringA + a synchronous WriteFile to gdiffuser-run.log + fputs/fflush on
// stderr -- three OS calls per line. That's cheap in isolation, but paid unconditionally on the
// GAME thread every single frame, and it spikes hardest exactly during a mode-change tick (the
// same tick that also does the segment loads), stacking on top of the freeze this trace was meant
// to help diagnose rather than causing it. Gate the high-frequency trace helpers behind an
// opt-in env var (GDX_TRACE=1) so they're silent by default and re-enabled on demand for
// debugging, without touching any of the many decomp call sites (GDX_CK/GDX_CKI macros, raw
// gdx_ck()/gdx_cki() calls). Lower-frequency one-time/startup gdx_port_logf() calls (ROM/disk
// load, sched init, crash handler, [transition] timing) are unaffected -- they call
// gdx_port_logf() directly, not through this gate.
/* Default: Debug builds trace ON (developer workflow), Release builds trace
   OFF (end-user performance). GDX_TRACE overrides in BOTH directions:
   GDX_TRACE=1 on a Release build enables full diagnostics for an end-user
   bug report; GDX_TRACE=0 on a Debug build silences the trace for profiling.
   Error-class logging (crash handler, [transition] timings, boot/ROM/disk
   one-shots) calls gdx_port_logf directly and is always on in both configs. */
/* GDX_DIAG_VERBOSE gate: the high-frequency per-frame diagnostic log families
   ([gfxdiag], [game], [seg], [sched], [phasegeom], [bigtri]) are silenced by
   default and re-enabled with GDX_DIAG_VERBOSE=1 (any non-"0" value). Defined
   in n64_sched.c and cached there; declared here (and in global.h for decomp
   sources) so the gate is callable from both the C++ bridge and decomp C. */
#ifdef __cplusplus
extern "C" {
#endif
int gdx_diag_verbose(void);
#ifdef __cplusplus
}
#endif

static inline int gdx_trace_enabled(void) {
    static int sCached = -1;
    if (sCached < 0) {
        const char* env = getenv("GDX_TRACE");
        if (env != NULL && env[0] != '\0') {
            sCached = (env[0] != '0') ? 1 : 0;
        } else {
#ifdef NDEBUG
            sCached = 0;
#else
            sCached = 1;
#endif
        }
    }
    return sCached;
}

// Safe logging that works from any thread/fiber context.
// Avoids fopen/fclose per-call which causes heap corruption in Debug CRT
// when called from the GFX fiber (the CRT's internal dynamic buffer
// deallocates on a stack frame that the fiber scheduler may have switched out).
static inline void gdx_port_write_log(const char* message) {
    if (message == NULL) {
        return;
    }

#ifdef _WIN32
    OutputDebugStringA(message);
    // GUI-subsystem builds have no console. Keep a lightweight file sink so boot
    // diagnostics remain available without reopening the file on every log call.
    {
        static HANDLE sLogFile = INVALID_HANDLE_VALUE;
        if (sLogFile == INVALID_HANDLE_VALUE) {
            sLogFile = CreateFileA("gdiffuser-run.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        }
        if (sLogFile != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(sLogFile, message, (DWORD)lstrlenA(message), &written, NULL);
        }
    }
    // Also write to stderr when launched from an existing console.
    fputs(message, stderr);
    fflush(stderr);
#else
    fputs(message, stderr);
    fflush(stderr);
#endif
}

static inline void gdx_port_vlogf(const char* fmt, va_list args) {
    char buffer[2048];
    size_t prefixLen = 0;
#ifdef _WIN32
    /* Wall-clock prefix with millisecond precision so log lines can be
       correlated with frame timing and external events. */
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        int n = snprintf(buffer, sizeof(buffer),
                         "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
                         st.wYear, st.wMonth, st.wDay,
                         st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        if (n > 0 && (size_t)n < sizeof(buffer)) {
            prefixLen = (size_t)n;
        }
    }
#endif
    vsnprintf(buffer + prefixLen, sizeof(buffer) - prefixLen, fmt, args);
    buffer[sizeof(buffer) - 1] = '\0';
    gdx_port_write_log(buffer);
}

static inline void gdx_port_logf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    gdx_port_vlogf(fmt, args);
    va_end(args);
}
