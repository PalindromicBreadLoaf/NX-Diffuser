// G-Diffuser — GDX_INPUT_SCRIPT: dev-only deterministic tick-level input playback.
//
// Purpose: unattended automated testing. When the env var GDX_INPUT_SCRIPT=<path> is set at
// process start, the port replays a scripted sequence of pad-0 inputs at the game-input-poll
// cadence (once per gdx_controller_poll() call, i.e. once per host frame, ~60Hz), REPLACING
// physical/LUS input while the script runs. When the env var is unset, this is a single cached
// getenv() check per process and nothing else — zero overhead, zero behavior change.
//
// Dev-only: there is no menu/UI surface for this feature. See gdx_input_script.c for the full
// script-format documentation and the parser/state-machine implementation.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Plain scalar pad state, ABI-compatible with the decomp's u16 buttonCurrent (unsigned short)
// and s8 stickX/stickY (signed char) — see input_bridge.c's file-header comment on the shared
// N64 OSContPad bitmask. Kept as raw C types (not decomp's u16/s8 typedefs) so this header has
// no decomp include dependency.
typedef struct GdxInputPad {
    unsigned short buttons; // N64 OSContPad bitmask (CONT_A/B/G/START/UP/DOWN/LEFT/RIGHT/L/R/E/D/C/F)
    signed char stickX;     // -80..80
    signed char stickY;     // -80..80
} GdxInputPad;

// Called once per gdx_controller_poll(), AFTER the raw LUS pad read (and the legacy
// gdx-autoinput.txt mechanism) land in `pad`, and BEFORE input_bridge.c's digital-stick
// derivation / edge-accumulation logic runs. No-op unless GDX_INPUT_SCRIPT is set (a single
// cached getenv check on the first call). When active, this OVERWRITES *pad with the script's
// state for this poll and advances the script's cursor by exactly one poll, so scripted
// button/stick edges flow through the exact same accumulate-then-finalize path physical input
// uses — nothing downstream needs to know the input source changed.
void gdx_input_script_override(GdxInputPad* pad);

#ifdef __cplusplus
}
#endif
