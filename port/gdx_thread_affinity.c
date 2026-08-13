#include "gdx_thread_affinity.h"

#include "port_log.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

int gdx_thread_affinity_pin(const char* threadName, int preferredCore) {
#ifdef __SWITCH__
    u64 processMask = 0;
    u32 placed;

    if (R_FAILED(svcGetInfo(&processMask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
        gdx_port_logf("[affinity] %s: core mask unavailable; placement skipped\n", threadName);
        return -1;
    }

    if (preferredCore < 0 || (processMask & (1ull << preferredCore)) == 0) {
        gdx_port_logf("[affinity] %s: core %d not in process mask 0x%llX; left unplaced\n",
                      threadName, preferredCore, (unsigned long long) processMask);
        return -1;
    }

    if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, preferredCore, 1u << preferredCore))) {
        gdx_port_logf("[affinity] %s: svcSetThreadCoreMask(core %d) failed; left unplaced\n",
                      threadName, preferredCore);
        return -1;
    }

    placed = svcGetCurrentProcessorNumber();
    gdx_port_logf("[affinity] %s: requested core %d, mask 0x%llX, running on core %u\n", threadName,
                  preferredCore, (unsigned long long) processMask, placed);
    return (int) placed;
#else
    (void) threadName;
    (void) preferredCore;
    return -1;
#endif
}
