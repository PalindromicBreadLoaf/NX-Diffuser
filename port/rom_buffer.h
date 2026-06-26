#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t* gdx_rom_buffer;
extern size_t   gdx_rom_size;
void gdx_init_rom(int argc, char** argv);

#ifdef __cplusplus
}
#endif
