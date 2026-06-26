#include <stdint.h>
#include <stddef.h>

static inline uint32_t Byteswap32(uint32_t x) {
    return ((x & 0xFF000000) >> 24) |
           ((x & 0x00FF0000) >> 8)  |
           ((x & 0x0000FF00) << 8)  |
           ((x & 0x000000FF) << 24);
}

static inline uint16_t Byteswap16(uint16_t x) {
    return ((x & 0xFF00) >> 8) | ((x & 0x00FF) << 8);
}
