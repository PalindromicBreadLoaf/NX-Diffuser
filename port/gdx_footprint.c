#include "gdx_footprint.h"

#if defined(__SWITCH__)
#include "gdx_switch_memquery.h"
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <stdio.h>
#include <unistd.h>
#endif

static uint64_t sPeak;

static int QueryPlatform(uint64_t* used, uint64_t* total) {
#if defined(__SWITCH__)
    return gdx_switch_memory_usage(used, total);
#elif defined(_WIN32)
    {
        PROCESS_MEMORY_COUNTERS pmc;
        if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
            return 0;
        }
        *used = (uint64_t) pmc.WorkingSetSize;
        *total = 0;
        return 1;
    }
#else
    {
        /* statm field 2 is resident pages. /proc/self/status's VmRSS is the same number after a
           parse. */
        FILE* f = fopen("/proc/self/statm", "r");
        unsigned long long sizePages = 0;
        unsigned long long residentPages = 0;
        int got;

        if (f == NULL) {
            return 0;
        }
        got = fscanf(f, "%llu %llu", &sizePages, &residentPages);
        fclose(f);
        if (got != 2) {
            return 0;
        }

        *used = (uint64_t) residentPages * (uint64_t) sysconf(_SC_PAGESIZE);
        *total = 0;
        return 1;
    }
#endif
}

int gdx_footprint_query(uint64_t* used, uint64_t* peak, uint64_t* total) {
    uint64_t nowUsed = 0;
    uint64_t nowTotal = 0;

    if (!QueryPlatform(&nowUsed, &nowTotal)) {
        return 0;
    }

    if (nowUsed > sPeak) {
        sPeak = nowUsed;
    }

    *used = nowUsed;
    *peak = sPeak;
    *total = nowTotal;
    return 1;
}
