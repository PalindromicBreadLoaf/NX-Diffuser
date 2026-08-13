#ifdef __SWITCH__

#include <switch.h>

#include "gdx_switch_memquery.h"

extern char __start__[];
extern char __end__[];

int gdx_switch_query_memory(uintptr_t addr, uintptr_t* begin, uintptr_t* end, int* readable) {
    MemoryInfo info = { 0 };
    u32 pageInfo = 0;

    if (R_FAILED(svcQueryMemory(&info, &pageInfo, (u64)addr))) {
        return 0;
    }

    *begin = (uintptr_t)info.addr;
    *end = (info.size > (u64)(UINTPTR_MAX - info.addr)) ? 0u : (uintptr_t)(info.addr + info.size);
    *readable = (info.type != MemType_Unmapped && (info.perm & Perm_R) != 0) ? 1 : 0;
    return 1;
}

void gdx_switch_module_range(uintptr_t* begin, uintptr_t* end) {
    *begin = (uintptr_t)__start__;
    *end = (uintptr_t)__end__;
}

#endif /* __SWITCH__ */
