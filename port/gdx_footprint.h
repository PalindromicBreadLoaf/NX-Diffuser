/* Process footprint */

#ifndef GDX_FOOTPRINT_H
#define GDX_FOOTPRINT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Current and peak resident bytes, plus the ceiling available to the process */
int gdx_footprint_query(uint64_t* used, uint64_t* peak, uint64_t* total);

#ifdef __cplusplus
}
#endif

#endif /* GDX_FOOTPRINT_H */
