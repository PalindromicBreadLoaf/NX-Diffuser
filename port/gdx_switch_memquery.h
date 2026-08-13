/* svcQueryMemory behind a libultra-safe seam */

#ifndef GDX_SWITCH_MEMQUERY_H
#define GDX_SWITCH_MEMQUERY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reports the region containing `addr`, or the next one up if `addr` is unmapped. */
int gdx_switch_query_memory(uintptr_t addr, uintptr_t* begin, uintptr_t* end, int* readable);

/* The NRO's load range */
void gdx_switch_module_range(uintptr_t* begin, uintptr_t* end);

#ifdef __cplusplus
}
#endif

#endif /* GDX_SWITCH_MEMQUERY_H */
