#pragma once

#include <stdarg.h>
#include <stdio.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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
