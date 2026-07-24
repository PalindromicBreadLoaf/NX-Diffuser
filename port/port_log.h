#pragma once

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
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
   are silenced by default in BOTH Debug and Release and re-enabled with
   GDX_DIAG_VERBOSE=1 (any non-"0" value). Defined in n64_sched.c and cached
   there; declared here (and in global.h for decomp sources) so the gate is
   callable from both the C++ bridge and decomp C.

   Family -> gate mapping (a tester sets GDX_DIAG_VERBOSE=1 to reveal the
   "verbose" families for a bug report; error/one-shot families are always on):

   Gated behind GDX_DIAG_VERBOSE=1 (silent by default):
     n64_sched.c    : [gfxdiag] [game] [seg] [sched] [phasegeom] [bigtri]
     n64_gfx_bridge : [geodiag] [gpustate] [gfxfail] (per-frame aggregate)
                      [datafail] [tex-census] [ci-dump] [dl-census]
                      [seg-dl] (successful repoint) [seg-dl-race] [seed] (quad probe)

   Always on (bounded and/or error/one-shot, high signal for bug reports):
     [bridge-init]  one-shot boot state
     [segment]      mode-transition cadence (venue segment loads)
     [stub-miss]    bounded to 24 unique SETTIMG module pointers
     [gdl-miss]     error family, per-phase budget: race 400 / non-race 40
     [gdl-bad]      error family, per-phase budget: race 400 / non-race 40
     [gfxfail] ROOT rejected  bounded to 16 (frame-skip crash failsafe)
     [seg-dl] moveword FAILED bounded to 24 (untranslatable repoint = garbage)
     [seed] boot-logo config  one-shot launch/env state
     [transition]/[gdxcap]/[dump]/[vifallback] boot/capture one-shots

   Not modified here (already env-gated or tightly bounded, did not flood):
     [setupdl] (GDX_DIAG_SETUPDL + cap 40), [trect] (cap 8),
     [vtx-*]/[mtx-*]/[texdiag]/[resolve-fail]/[signext] (small per-cause caps),
     [transition-cap] (cap 8), GDX_DIAG_SETTIMG race trace (env-gated). */
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

// Opt-in gate for the persistent gdiffuser-run.log file sink. File logging is
// OFF by default (a normal play session must not silently create/append a log
// at the process CWD, which for shortcuts/packaged installs may be write-
// protected). It is enabled when any diagnostic mode is requested via env:
//   GDX_LOG          (=1 / any non-"0" value)  -- dedicated file-log opt-in
//   GDX_TRACE        (set to any non-"0" value) -- high-frequency trace mode
//   GDX_DIAG_VERBOSE (set to any non-"0" value) -- verbose diagnostic families
//   GDX_DIAG_UNLOCK  (set to any non-"0" value) -- unlock-code/audio path only
// Checked once and static-cached (benign race: every thread computes the same
// value), matching gdx_trace_enabled() above. The always-on stderr /
// OutputDebugStringA sinks are unaffected by this gate.
static inline int gdx_log_file_enabled(void) {
    static int sCached = -1;
    if (sCached < 0) {
        int enabled = 0;
        const char* log = getenv("GDX_LOG");
        const char* trace = getenv("GDX_TRACE");
        const char* diag = getenv("GDX_DIAG_VERBOSE");
        const char* unlock = getenv("GDX_DIAG_UNLOCK");
        if (log != NULL && log[0] != '\0' && log[0] != '0') {
            enabled = 1;
        } else if (trace != NULL && trace[0] != '\0' && trace[0] != '0') {
            enabled = 1;
        } else if (diag != NULL && diag[0] != '\0' && diag[0] != '0') {
            enabled = 1;
        } else if (unlock != NULL && unlock[0] != '\0' && unlock[0] != '0') {
            enabled = 1;
        }
        sCached = enabled;
    }
    return sCached;
}

// Resolve "<exe dir>/<fileName>" into outPath (the exe-relative saves convention
// sram_buffer.cpp already uses). Falls back to the bare CWD-relative filename if
// the exe path is unavailable or does not fit. Shared by gdx_log_file_path (opt-in
// run log) and gdx_crash_report_path (always-on crash report) so both sinks agree
// on where "next to the executable" means. Returns outPath.
static inline const char* gdx_exe_relative_path(char* outPath, size_t outCap, const char* fileName) {
#ifdef _WIN32
    {
        DWORD n = GetModuleFileNameA(NULL, outPath, (DWORD)outCap);
        if (n > 0 && n < outCap) {
            char* slash = strrchr(outPath, '\\');
            if (slash != NULL) {
                slash[1] = '\0';
                if (strlen(outPath) + strlen(fileName) < outCap) {
                    strcat(outPath, fileName);
                    return outPath;
                }
            }
        }
    }
#else
    {
        ssize_t rl = readlink("/proc/self/exe", outPath, outCap - 1);
        if (rl > 0 && (size_t)rl < outCap) {
            outPath[rl] = '\0';
            char* slash = strrchr(outPath, '/');
            if (slash != NULL &&
                (size_t)(slash - outPath) + 1 + strlen(fileName) < outCap) {
                strcpy(slash + 1, fileName);
                return outPath;
            }
        }
    }
#endif
    if (strlen(fileName) < outCap) {
        strcpy(outPath, fileName);
    } else if (outCap > 0) {
        outPath[0] = '\0';
    }
    return outPath;
}

// Resolve "<exe dir>/gdiffuser-run.log" into outPath. See gdx_exe_relative_path.
static inline const char* gdx_log_file_path(char* outPath, size_t outCap) {
    return gdx_exe_relative_path(outPath, outCap, "gdiffuser-run.log");
}

// Resolve "<exe dir>/gdiffuser-crash.txt" into outPath. See gdx_exe_relative_path.
static inline const char* gdx_crash_report_path(char* outPath, size_t outCap) {
    return gdx_exe_relative_path(outPath, outCap, "gdiffuser-crash.txt");
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
    // diagnostics remain available without reopening the file on every log call
    // -- but only when opt-in diagnostics are requested (see gdx_log_file_enabled).
    if (gdx_log_file_enabled()) {
        static HANDLE sLogFile = INVALID_HANDLE_VALUE;
        static int sTriedOpen = 0;
        if (!sTriedOpen) {
            sTriedOpen = 1;
            char logPath[MAX_PATH];
            gdx_log_file_path(logPath, sizeof(logPath));
            sLogFile = CreateFileA(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
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
    // Opt-in persistent file sink (mirrors the Windows path; stderr is always on).
    if (gdx_log_file_enabled()) {
        static FILE* sLogFile = NULL;
        static int sTriedOpen = 0;
        if (!sTriedOpen) {
            sTriedOpen = 1;
            char logPath[4096];
            gdx_log_file_path(logPath, sizeof(logPath));
            if (logPath[0] != '\0') {
                sLogFile = fopen(logPath, "ab");
            }
        }
        if (sLogFile != NULL) {
            fputs(message, sLogFile);
            fflush(sLogFile);
        }
    }
    fputs(message, stderr);
    fflush(stderr);
#endif
}

// Always-on crash-report sink: writes straight to "<exe dir>/gdiffuser-crash.txt",
// bypassing gdx_log_file_enabled() entirely. Field testers running a plain double-
// clicked build set none of GDX_LOG/GDX_TRACE/GDX_DIAG_VERBOSE/GDX_DIAG_UNLOCK, so
// the opt-in run-log sink above never opens -- a crash would otherwise leave zero
// artifacts on disk. This is called ONLY from the crash handler (n64_sched.c), a
// rare/exceptional path, so opening+closing the handle per call (rather than
// caching a handle for the process lifetime like gdx_port_write_log does) is fine
// and keeps the file free to inspect immediately after a crash without waiting on
// process exit to flush/close it.
//
// Same CRT-FILE* avoidance as gdx_port_write_log above: the crash handler can run
// on the GFX/audio fiber, whose stack may be mid-switch from the scheduler's point
// of view, so no fopen/FILE* buffering here -- raw CreateFileA/WriteFile (Windows)
// or open/write (POSIX) only.
static inline void gdx_crash_report_write(const char* message) {
    if (message == NULL || message[0] == '\0') {
        return;
    }
#ifdef _WIN32
    {
        char path[MAX_PATH];
        HANDLE file;
        gdx_crash_report_path(path, sizeof(path));
        file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, message, (DWORD) lstrlenA(message), &written, NULL);
            CloseHandle(file);
        }
    }
#else
    {
        char path[4096];
        int fd;
        gdx_crash_report_path(path, sizeof(path));
        if (path[0] != '\0') {
            fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                write(fd, message, strlen(message));
                close(fd);
            }
        }
    }
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
