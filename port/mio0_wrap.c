/* port/mio0_wrap.c — mio0Decode wrapper for decomp callers.
 * Signature matches decomp/include/functions.h:282.
 * Delegates to torch/lib/libmio0/mio0_decode.
 * T1 audit: torch/lib/libmio0/utils.h is self-contained (macros + <stdio.h> only;
 * no torch-internal headers; safe to compile outside the torch CMake context). */
#include "mio0.h"

void mio0Decode(unsigned char* src, void* dst) {
    mio0_decode(src, (unsigned char*)dst, NULL);
}
