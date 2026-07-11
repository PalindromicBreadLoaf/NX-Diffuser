/* port/mio0_wrap.c — mio0Decode wrapper for decomp callers.
 * Signature matches decomp/include/functions.h:282.
 * Delegates to torch/lib/libmio0/mio0_decode.
 * T1 audit: torch/lib/libmio0/utils.h is self-contained (macros + <stdio.h> only;
 * no torch-internal headers; safe to compile outside the torch CMake context). */
#include "mio0.h"
#include "n64_rdram.h"

extern void gdx_record_dma_load(unsigned int rdram_phys, unsigned int rom_offset, unsigned int size);

void mio0Decode(unsigned char* src, void* dst) {
    int written = mio0_decode(src, (unsigned char*)dst, NULL);
    /* The renderer's texture-staleness tracking (HostRangeChanged in
       n64_gfx_bridge.cpp) only sees recorded writes; a mio0 decode is plain
       CPU stores. With the per-mode arena rewind reusing addresses across
       mode transitions, an unrecorded decode leaves stale persistent texture
       copies — previous-mode pixels rendered on race tracks. Record the
       decoded range whenever the destination is RDRAM-backed. */
    if (written > 0 && gdx_rdram != NULL) {
        unsigned char* d = (unsigned char*)dst;
        if (d >= gdx_rdram && d < gdx_rdram + GDX_RDRAM_SIZE) {
            gdx_record_dma_load((unsigned int)(d - gdx_rdram), 0u, (unsigned int)written);
        }
    }
}
