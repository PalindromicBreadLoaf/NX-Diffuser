/*
 * The process-wide owner of gdiffuser-run.log.
 */
#include "port_log.h"

// Holds the file handle open instead of fopen/fclose per call.
void gdx_port_log_file_write(const char* message) {
    if (message == NULL || !gdx_log_file_enabled()) {
        return;
    }

#ifdef _WIN32
    {
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
            WriteFile(sLogFile, message, (DWORD) lstrlenA(message), &written, NULL);
        }
    }
#else
    {
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
#if defined(__SWITCH__)
            fsync(fileno(sLogFile));
#endif
        }
    }
#endif
}
