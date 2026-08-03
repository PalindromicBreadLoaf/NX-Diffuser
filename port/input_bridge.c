// G-Diffuser — host input bridge for the PORT.
//
// The decomp's N64 SI polling path is disabled under PORT (Controller_Init / Controller_Reset
// return early in decomp/src/sys/controller.c, and every Controller_UpdateInputs call site in
// game.c is #ifndef PORT), so the game never sees a connected controller unless we feed the decomp
// controller globals (gControllers[] / gControllerStatus[] / gPlayerControlPorts[] /
// gControllersConnected / gSharedController) from the host every frame. This file is the whole
// replacement for Controller_Init + Controller_UpdateInputs.
//
// ALL FOUR PORTS, NOT JUST PORT 1:
// -----------------------------------------------------------------------------------------------
// This bridge used to hardcode "one controller, in port 1" — gControllersConnected = 1 and
// gPlayerControlPorts[1..3] = PORT_DISCONNECTED. PORT_DISCONNECTED is 4 (controller.h:40), so
// players 2-4 resolved to the never-written gControllers[4] dummy at racer.c:4952 and every
// split-screen VS / Death Race mode was unplayable. Presence is now derived per port from what the
// LUS ControlDeck can actually read (see gdx_sync_controller_ports below), each port's resolved
// input lands in its own gControllers[i], and gSharedController is the OR-aggregate across
// connected ports exactly as Controller_UpdateInputs builds it on hardware.
//
// INPUT SOURCE — libultraship (LUS) ControlDeck (was: a hardcoded VK_* keyboard poll):
// -----------------------------------------------------------------------------------------------
// This file used to read the physical keyboard directly with GetAsyncKeyState(VK_*) and hardcode
// Enter=Start / Space,X=A / Z,Esc=B / arrows=stick. That bypassed LUS entirely, so the Input
// Editor (F1 -> Controls -> "Controller Configuration...") configured devices nobody read, and
// gamepads / rebinding / keyboard-as-a-device did not work.
//
// The rework routes the GAME's input read through the LUS ControlDeck, which already does SDL
// gamepad auto-detection, keyboard-as-a-mappable-device, mouse mappings, deadzone/sensitivity
// curves, and (crucially) the remapping the Input Editor writes. Because LUS's ControlDeck is
// C++ and this translation unit is C, the actual read happens in a tiny extern "C" shim in
// main.cpp (gdx_lus_read_pads) that calls ControlDeck::WriteToPad() once and hands us every port's
// resolved N64 state as plain scalars. This file keeps ownership of the decomp side: publishing
// per-port presence, deriving the digital stick direction, computing pressed/released/retrigger
// edges, and building gSharedController.
//
// N64 BUTTON BITMASK — no translation table needed:
//   The decomp (decomp/include/controller.h -> PR/os_cont.h) and LUS
//   (libultraship/libultra/controller.h) use the IDENTICAL standard N64 OSContPad bitmask:
//     A=0x8000  B=0x4000  Z=0x2000  START=0x1000  DUP=0x0800  DDOWN=0x0400  DLEFT=0x0200
//     DRIGHT=0x0100  L=0x0020  R=0x0010  CUP=0x0008  CDOWN=0x0004  CLEFT=0x0002  CRIGHT=0x0001
//   So LUS's OSContPad.button maps 1:1 onto the decomp's buttonCurrent — we just mask with
//   CONT_MASK. The analog stick (OSContPad.stick_x/stick_y) is already produced by LUS in the
//   N64 -80..80 range the decomp expects, so it copies straight into stickX/stickY.
//
// THREADING: gdx_controller_poll() runs on the host/main thread (main.cpp frame loop) before
// gdx_dispatch() runs the game fibers, which then read the globals we wrote. Single-threaded,
// no synchronization needed.

#ifndef _LANGUAGE_C
#define _LANGUAGE_C 1
#endif

#include "PR/os_pfs.h"
#include "controller.h"
#include "fzx_game.h" // GameMode values + GET_MODE (in-race safety checks)
#include "gdx_input_script.h" // GDX_INPUT_SCRIPT: dev-only deterministic input playback (see file header)
#include "port_log.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif

#ifdef errno
#undef errno
#endif

extern s32 gControllersConnected;

// Implemented in main.cpp (C++). Reads the LUS ControlDeck's resolved state for EVERY controller
// port — i.e. the InputEditor's mappings applied to the connected keyboard / SDL gamepad / mouse
// devices — into per-port standard N64 button bitmasks + analog sticks (-80..80), plus a per-port
// "this port can actually produce input right now" flag. Returns 1 on success, 0 if the
// ControlDeck is unavailable (every slot left zeroed; degrade to zero input, never crash). The
// scalar widths match the decomp's u16 (unsigned short) / s8 (signed char) exactly.
//
// It MUST be a single batched call rather than four per-port reads: ControlDeck::WriteToPad() has
// per-call side effects (mouse-wheel buffer decay, the simulated-input-lag pad deque) that only
// behave correctly when it runs once per frame. See the long comment above gdx_lus_read_pads() in
// main.cpp.
extern int gdx_lus_read_pads(int capacity, u16* out_buttons, s8* out_stick_x, s8* out_stick_y,
                             u8* out_connected);

// Zero exactly the input fields Controller_ClearInputs() clears (decomp/src/sys/controller.c:47-51)
// without touching pfs / rumble bookkeeping. Used when a port goes away mid-session so a pad that
// was unplugged mid-corner cannot leave a latched throttle behind, and to initialise the
// gControllers[MAXCONTROLLERS] sentinel that disconnected player slots resolve to.
static void gdx_clear_controller_inputs(Controller* controller) {
    controller->retriggerCurrentButtonPress = 0;
    controller->buttonCurrent = 0;
    controller->buttonPressed = 0;
    controller->buttonReleased = 0;
    controller->buttonPrev = 0;
    controller->stickX = 0;
    controller->stickY = 0;
    controller->stickCurrent = 0;
    controller->stickPressed = 0;
    controller->stickReleased = 0;
    controller->stickPrev = 0;
    controller->sameInputsHeldCounter = 0;
}

// Per-port host-frame edge accumulator state. One instance per controller port; see the long
// "ALIGNED TO THE GAME FRAME" comment in gdx_update_port_inputs() for what each field is for.
typedef struct GdxPortInputState {
    u16 prevHostButtons;
    u16 accumPressed;
    u16 accumReleased;
    u16 prevGameButtons;
    u8 prevHostStick;
    u8 accumStickPressed;
    u8 accumStickReleased;
    u8 prevGameStick;
} GdxPortInputState;

static GdxPortInputState sPortInput[MAXCONTROLLERS];

// ── Connected-port bookkeeping ───────────────────────────────────────────────────────────────────
// Publishes the ControlDeck's per-port availability into the three decomp globals the game reads:
//
//   gControllers[i].errno / gControllerStatus[i].errno — per-PORT presence. Read by
//       Controller_ClearInputs (controller.c:47).
//   gPlayerControlPorts[p]                             — PLAYER p -> port index, or
//       PORT_DISCONNECTED (4). Indexed by racer id at racer.c:4952 and by camera index at
//       camera.c:3174/3253, so it is the map that decides which pad drives which car in
//       split-screen VS / Death Race.
//   gControllersConnected                              — number of live ports. The main menu gates
//       VS Battle on it (main_menu.c:205) and clamps the 2/3/4-player selector to it
//       (main_menu.c:323-324, 869), so an inaccurate count is what made split-screen unreachable.
//
// This used to hardcode "port 1 connected, ports 2-4 dead", which pointed players 2-4 at
// PORT_DISCONNECTED and therefore at the never-written gControllers[4] dummy.
//
// SLOT STABILITY (why this is incremental instead of recomputed): the naive "recompute the compact
// map every frame" approach reshuffles players when a pad drops. With ports 0,1,2 live, losing
// port 1 would renumber player 2 onto port 2 — the human holding pad 3 would suddenly be driving
// car 2 mid-race. Instead each port owns its player slot for as long as it stays connected, exactly
// like the hardware disconnect path (controller.c:98-105): a departing port frees only its own
// slot. A returning port takes the lowest free slot, which is the port's hot-plug improvement over
// hardware (where Controller_Init only ever ran once at boot).
//
// PORT 1 IS PINNED CONNECTED by the caller. gControllers[gPlayerControlPorts[0]] is the menu/editor
// input source in dozens of places (e.g. course_edit/188850.c:114), and a host has no
// "please reconnect controller 1" screen to recover from, so letting player 1's slot ever become
// PORT_DISCONNECTED would wedge the UI rather than degrade it.
static void gdx_sync_controller_ports(const u8* connected) {
    // Zero-initialised: every port starts out "was disconnected", so the first call runs every live
    // port through the connect path in ascending order and produces exactly the compact assignment
    // Controller_Init builds on hardware (controller.c:220-237).
    static u8 s_prevConnected[MAXCONTROLLERS];
    static int s_initialized = 0;
    int i;
    int j;

    if (!s_initialized) {
        for (i = 0; i < MAXCONTROLLERS; i++) {
            gPlayerControlPorts[i] = PORT_DISCONNECTED;
        }
        gControllersConnected = 0;

        // The sentinel every disconnected player slot resolves to. Nothing under PORT ever writes
        // it (Controller_UpdateInputs is compiled out in game.c), so zeroing it once guarantees an
        // absent player reads neutral input forever instead of whatever the BSS happened to hold.
        gdx_clear_controller_inputs(&gControllers[MAXCONTROLLERS]);
        gControllers[MAXCONTROLLERS].errno = CONT_NO_RESPONSE_ERROR;

        // Bindings come from the LUS ControlDeck and are fully remappable in the Input Editor
        // (F1 -> Controls -> "Controller Configuration..."). Out-of-the-box keyboard defaults
        // (LUS::ControllerDefaultMappings): Space=Start, X=A, C=B, Z=Z, E=L, R=R, WASD=analog
        // stick, arrows=C-buttons, TFGH=D-pad. SDL gamepads are auto-detected, and with two or
        // more plugged in they are routed one-per-port (see gdxSyncGamepadPortRouting in main.cpp).
        gdx_port_logf("[input] LUS ControlDeck active — remap in F1 > Controls > Input Editor "
                      "(default keyboard: Space=Start, X=A, C=B, WASD=stick, arrows=C)\n");
        s_initialized = 1;
    }

    for (i = 0; i < MAXCONTROLLERS; i++) {
        const u8 isConnected = (connected[i] != 0);

        gControllers[i].errno = isConnected ? 0 : CONT_NO_RESPONSE_ERROR;
        gControllerStatus[i].errno = isConnected ? 0 : CONT_NO_RESPONSE_ERROR;
        gControllerStatus[i].type = CONT_TYPE_NORMAL;

        if (isConnected == s_prevConnected[i]) {
            continue;
        }

        if (isConnected) {
            for (j = 0; j < MAXCONTROLLERS; j++) {
                if (gPlayerControlPorts[j] == PORT_DISCONNECTED) {
                    gPlayerControlPorts[j] = i;
                    break;
                }
            }
            gControllersConnected++;
            gdx_port_logf("[input] port %d connected (players now %d)\n", i + 1, gControllersConnected);
        } else {
            for (j = 0; j < MAXCONTROLLERS; j++) {
                if (gPlayerControlPorts[j] == i) {
                    gPlayerControlPorts[j] = PORT_DISCONNECTED;
                    break;
                }
            }
            gControllersConnected--;
            // A pad that vanishes mid-race must not leave its last frame latched: the racer it was
            // driving now reads the neutral gControllers[4] sentinel, but the port's own struct is
            // still live for anything holding a direct pointer (e.g. course_edit/188850.c:859).
            gdx_clear_controller_inputs(&gControllers[i]);
            memset(&sPortInput[i], 0, sizeof(sPortInput[i]));
            gdx_port_logf("[input] port %d disconnected (players now %d)\n", i + 1, gControllersConnected);
        }

        s_prevConnected[i] = isConnected;
    }
}

/* Scripted input for headless/automated test runs. If gdx-autoinput.txt sits next to the exe it
   is parsed once and combined with the live (LUS-sourced) input every poll.
   ---------------------------------------------------------------------------------------------
   FORMAT (two timebases; auto-detected by a version marker on the FIRST non-blank line):
   ---------------------------------------------------------------------------------------------
   TICK MODE (deterministic — required for the bit-identical PCM gate):
       The first non-blank line is the literal word `ticks` (case-insensitive). Every later line is
           <tick> <INPUT> [holdTicks]
       where <tick> is an integer VI-tick index (the counter below, incremented once per
       gdx_controller_poll() call — i.e. once per host frame, wall-clock-independent), and the
       optional [holdTicks] is how many ticks the input stays asserted (default 15). Because the
       schedule is expressed purely in ticks, a given script produces the exact same input on the
       exact same frames on every run and every machine — the property golden PCM capture needs.

   LEGACY SECONDS MODE (unchanged; kept working for existing scripts):
       No `ticks` marker. Every line is
           <secondsSinceLoad> <INPUT>
       clocked off the host monotonic wall clock; each input is held for 0.25 s. This is the
       original behavior and is selected whenever the first token is not `ticks`.

   INPUT names (both modes): START A B UP DOWN LEFT RIGHT (buttons); STICK_UP STICK_DOWN
   STICK_LEFT STICK_RIGHT (analog, ±80). Blank lines and lines starting with '#' are ignored.
   Up to 64 events. */
/* Monotonic milliseconds for the legacy seconds-mode autoinput clock (host-agnostic). */
static unsigned long long gdx_autoinput_now_ms(void) {
#ifdef _WIN32
    return (unsigned long long) GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long) ts.tv_sec * 1000ull + (unsigned long long) (ts.tv_nsec / 1000000);
#endif
}

/* Default tick-mode hold window (~0.25 s at 60 Hz, matching the legacy seconds-mode hold). */
#define GDX_AUTOINPUT_DEFAULT_HOLD_TICKS 15

/* Map an INPUT token to a button bit and/or analog stick offset. Returns 1 if recognized. */
static int gdx_autoinput_parse_input(const char* name, u16* out_btn, s8* out_x, s8* out_y) {
    u16 b = 0;
    s8 x = 0;
    s8 y = 0;
    if (strcmp(name, "START") == 0) b = BTN_START;
    else if (strcmp(name, "A") == 0) b = BTN_A;
    else if (strcmp(name, "B") == 0) b = BTN_B;
    else if (strcmp(name, "UP") == 0) b = BTN_UP;
    else if (strcmp(name, "DOWN") == 0) b = BTN_DOWN;
    else if (strcmp(name, "LEFT") == 0) b = BTN_LEFT;
    else if (strcmp(name, "RIGHT") == 0) b = BTN_RIGHT;
    else if (strcmp(name, "STICK_UP") == 0) y = 80;
    else if (strcmp(name, "STICK_DOWN") == 0) y = -80;
    else if (strcmp(name, "STICK_LEFT") == 0) x = -80;
    else if (strcmp(name, "STICK_RIGHT") == 0) x = 80;
    else return 0;
    *out_btn = b;
    *out_x = x;
    *out_y = y;
    return (b != 0 || x != 0 || y != 0);
}

static void gdx_autoinput_apply(u16* io_buttons, s8* io_stick_x, s8* io_stick_y) {
    static int s_state = -1; /* -1 unread, 0 absent, 1 loaded */
    static int s_tickmode = 0;
    /* `at` and `hold` are in the active timebase's native unit: VI-ticks (tick mode) or seconds
       (legacy mode). */
    static struct { double at; double hold; u16 btn; s8 stick_x; s8 stick_y; } s_events[64];
    static int s_count = 0;
    static unsigned long long s_start_ms = 0;    /* seconds-mode wall-clock baseline */
    static unsigned long long s_tick = 0;        /* tick-mode VI-tick counter */

    if (s_state == -1) {
        FILE* f = fopen("gdx-autoinput.txt", "r");
        s_state = 0;
        if (f != NULL) {
            char line[64];
            int sniffed = 0;
            while (s_count < 64 && fgets(line, (int) sizeof(line), f) != NULL) {
                char name[16];
                double at;
                int hold_ticks;
                u16 b;
                s8 x, y;
                /* Skip blank / comment lines. */
                {
                    const char* p = line;
                    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
                    if (*p == '\0' || *p == '#') continue;
                }
                /* First meaningful line: sniff the timebase marker. */
                if (!sniffed) {
                    char first[16];
                    if (sscanf(line, "%15s", first) == 1 &&
                        (strcmp(first, "ticks") == 0 || strcmp(first, "TICKS") == 0 ||
                         strcmp(first, "Ticks") == 0)) {
                        s_tickmode = 1;
                        sniffed = 1;
                        continue; /* the marker line carries no event */
                    }
                    s_tickmode = 0;
                    sniffed = 1;
                    /* fall through: this line is the first event (legacy seconds mode) */
                }
                if (s_tickmode) {
                    int n = sscanf(line, "%lf %15s %d", &at, name, &hold_ticks);
                    if (n < 2) continue;
                    if (n < 3 || hold_ticks <= 0) hold_ticks = GDX_AUTOINPUT_DEFAULT_HOLD_TICKS;
                    if (gdx_autoinput_parse_input(name, &b, &x, &y)) {
                        s_events[s_count].at = at;
                        s_events[s_count].hold = (double) hold_ticks;
                        s_events[s_count].btn = b;
                        s_events[s_count].stick_x = x;
                        s_events[s_count].stick_y = y;
                        s_count++;
                    }
                } else {
                    if (sscanf(line, "%lf %15s", &at, name) != 2) continue;
                    if (gdx_autoinput_parse_input(name, &b, &x, &y)) {
                        s_events[s_count].at = at;
                        s_events[s_count].hold = 0.25; /* legacy fixed 0.25 s hold */
                        s_events[s_count].btn = b;
                        s_events[s_count].stick_x = x;
                        s_events[s_count].stick_y = y;
                        s_count++;
                    }
                }
            }
            fclose(f);
            s_state = 1;
            s_start_ms = gdx_autoinput_now_ms();
            gdx_port_logf("[input] autoinput script loaded: %d events (%s timebase)\n", s_count,
                          s_tickmode ? "tick" : "seconds");
        }
    }
    if (s_state != 1) {
        return;
    }
    {
        double now = s_tickmode ? (double) s_tick
                                : (double) (gdx_autoinput_now_ms() - s_start_ms) / 1000.0;
        int i;
        for (i = 0; i < s_count; i++) {
            if (now >= s_events[i].at && now < s_events[i].at + s_events[i].hold) {
                *io_buttons |= s_events[i].btn;
                if (s_events[i].stick_x != 0) *io_stick_x = s_events[i].stick_x;
                if (s_events[i].stick_y != 0) *io_stick_y = s_events[i].stick_y;
            }
        }
    }
    /* Advance the VI-tick counter once per poll (tick mode only; harmless otherwise). This runs
       exactly once per gdx_controller_poll() call — see the single call site below. */
    s_tick++;
}

// Decomp race state. Kept behind small C helpers so every photo-mode consumer uses the same
// definition and the ImGui ghost importer can avoid writing persistence while a race is live.
extern s32 gGameMode;
extern s8 gGamePaused;

// libultraship consolevariablebridge.h (extern "C", API_EXPORT). Declared locally -- same pattern
// as gdx_lus_read_pads above -- to avoid pulling a LUS/C++ umbrella header into this decomp-
// environment C TU. int32_t is `int` on this target, so the plain-int prototype is ABI-identical
// and links to the same undecorated C symbol.
extern int CVarGetInteger(const char* name, int defaultValue);

// True for the game modes that are an actual on-track race (where a retry is meaningful). GET_MODE
// (fzx_game.h:30) strips the high F3D gfx-mode bits so this matches whether or not they are set.
static int gdx_gamemode_is_race(s32 mode) {
    switch (GET_MODE(mode)) {
        case GAMEMODE_GP_RACE:
        case GAMEMODE_PRACTICE:
        case GAMEMODE_VS_2P:
        case GAMEMODE_VS_3P:
        case GAMEMODE_VS_4P:
        case GAMEMODE_TIME_ATTACK:
        case GAMEMODE_DEATH_RACE:
            return 1;
        default:
            return 0;
    }
}

// Exposed for the Practice-tab ghost import/export UI (gdx_menu.cpp). Ghost SRAM writes must not
// race the game fiber, so the menu disables Import while an on-track race is live; it is safe to
// touch the ghost save from the menus/attract screens (where the game saves ghosts itself).
int gdx_input_in_gameplay(void) {
    return gdx_gamemode_is_race(gGameMode);
}

int gdx_photo_mode_active(void) {
    return CVarGetInteger("gEnhancements.Practice.PhotoMode", 0) && (gGamePaused != 0);
}

// Shared predicate for the true-widescreen 2D UI slice. Anchor/stretch extra-geometry-mode
// emitters in decomp draw code must gate on this at the call site so that CVar-off builds emit
// a bit-identical display list to stock, instead of relying solely on the interpreter's own
// CVar check when it consumes the mode bits (defense in depth; see GfxDrawRectangle).
int gdx_widescreen_ui_active(void) {
    return CVarGetInteger("gEnhancements.Graphics.Widescreen", 1) &&
           CVarGetInteger("gEnhancements.Graphics.WidescreenUI", 0);
}

// Split-screen (VS Battle / Death Race 2P-4P) variant of the predicate above.
//
// WHY A SEPARATE SWITCH. Every widescreen 2D anchor scope shipped so far is gated on
// `gNumPlayers == 1` at the call site (hud.c:1044 `case 1:`, menus.c:5617-5657,
// machine.c:1226-1884). That was a deliberate deferral: split-screen HUD placement was
// recorded as an owner-judgment item, not an oversight. Extending the anchors to 2P/3P/4P
// is therefore a NEW visual policy for shipped modes, and per the brief it gets its own
// opt-out instead of silently riding gEnhancements.Graphics.WidescreenUI -- a player who
// likes the 1P corner HUD must still be able to keep split-screen at stock placement.
//
// It is a strict subset of the 1P predicate (Widescreen AND WidescreenUI must both be on),
// so turning either parent off also turns this off; there is no state where split-screen is
// anchored while 1P is not. Default 1 matches WidescreenUI's shipped default -- a widescreen
// frame whose split-screen HUD still clusters in the middle 4:3 band is the defect being
// fixed, so the fix ships on.
//
// NOTE (owner action required): the CVar has no menu widget yet. gdx_menu.cpp /
// gdx_menu_registry.cpp were off-limits for this change, so `CVarRegisterInteger` and the
// checkbox are not wired. CVarGetInteger's default (1) makes the feature work regardless;
// registering it only adds the toggle and the config-file entry.
int gdx_widescreen_split_ui_active(void) {
    return gdx_widescreen_ui_active() && CVarGetInteger("gEnhancements.Graphics.WidescreenSplitUI", 1);
}

// The Expansion Kit editors are 4:3-authored 2D/3D composites: their 3D layers (Course Edit's
// canvas through the shared Background/Course/Racer_Draw path, Create Machine's preview and
// parts grid) would otherwise pick up the global hor+ widescreen vertex correction while their
// 2D layers stay pillarboxed, splitting one screen across two aspect ratios. Publish a runtime
// flag the renderer folds into its existing Widescreen==0 pillarbox path (AdjXForAspectRatio,
// StartFrame's forced offscreen target, and Fast3dGui::DrawGame's centered 4:3 composite), so
// editor frames render exactly like the stock 4:3 mode regardless of the widescreen settings.
// The WHOLE Create Machine experience is pillarboxed: every gWorksMachineMode sub-state --
// including the ENTRY "SELECT MACHINE" screen and its file/clear sub-menus -- is forced 4:3
// exactly like Course Edit, so the classification no longer depends on gWorksMachineMode and the
// machine_create.c background gradient's STRETCH scope is gated off while the mode forces 4:3.
// CVar writes are cheap but not free -- publish only on change.
//
// STALENESS CONTRACT: the per-frame tick below reads gGameMode as of
// the END of the previous dispatch, but the game flips gGameMode MID-dispatch (game.c mode-flip)
// and renders the new mode's transition in the same dispatch. The flip frame therefore rendered
// with a stale flag -- one whole menu frame pillarboxed on every editor exit (visible as a
// brief "tries to do 4:3" on Main Menu -> Cup Select). gdx_fixed_aspect_publish() is therefore ALSO
// called from the decomp's mode-flip site under PORT so consumers that read the CVar live
// (Transition_Draw, Fast3dGui::DrawGame) and the interpreter's per-task re-latch see the
// post-flip truth within the same frame.
// The flag is a plain process global in libultraship (interpreter.cpp), NOT a CVar: the old CVar
// form persisted as 1 into gdiffuser.cfg.json when the config was saved from inside an editor,
// pillarboxing pre-tick boot frames on the next launch. Runtime state never belongs in the
// config file.
// True iff a given raw game mode must render stock 4:3 (the EK editors). The entire Create Machine
// experience is pillarboxed 4:3 regardless of gWorksMachineMode: GET_MODE masks off the F3D-variant
// bits (0xC000), so GAMEMODE_CREATE_MACHINE (0x10) matches every sub-state of the editor. The
// in-race GAMEMODE_LX_MACHINE_SETTINGS is a different mode value (0x9) and is intentionally NOT
// covered here.
static int gdx_mode_forces_fixed_aspect(s32 gamemode) {
    s32 mode = GET_MODE(gamemode);
    return (mode == GAMEMODE_COURSE_EDIT) || (mode == GAMEMODE_CREATE_MACHINE);
}

void gdx_fixed_aspect_publish(void) {
    extern void gdx_set_force_fixed_aspect(int on); // libultraship interpreter.cpp
    extern s32 gQueuedGameMode;                     // decomp/src/game/game.c

    // TRANSIENT CUP-SELECT SQUEEZE FIX: evaluate the flag on the mode that will
    // actually RENDER this dispatch, not on the mode currently latched in gGameMode.
    //
    // The game flips gGameMode = gQueuedGameMode MID-dispatch (game.c) and then renders the NEW
    // mode's appear transition in that SAME dispatch. The renderer, however, commits its
    // whole-frame pillarbox target from the PRE-dispatch flag (interpreter.cpp StartFrame), and
    // that target is NOT recomputed after the mid-dispatch flip. So on an editor->menu flip the
    // pre-dispatch tick still sees the OLD editor mode, forces 4:3, and the destination menu
    // frame gets composited into a centered 4:3 pillarbox for one frame -- the visible
    // "squeeze then normal" on Main Menu -> Cup Select after having been in an editor. Because
    // the flag is republished from the flip site too, the value ends up oscillating across the
    // transition (stale editor 1 on the flip frame, 0 the next) rather than staying stable.
    //
    // While a mode change is pending (gGameMode != gQueuedGameMode) the frame shown after the
    // flip is gQueuedGameMode, and the outgoing mode's remaining frames are hidden under the
    // transition wipe (Transition_HideSet), so keying on the DESTINATION mode makes the flag
    // stable across the whole transition and never pillarboxes an incoming menu. Settled frames
    // (no pending change) key on gGameMode exactly as before, so a real editor in steady state
    // -- and the Create Machine sub-mode toggles, which never change gGameMode -- is still forced
    // to 4:3. This is the deliberate low-risk gate: it can render the outgoing screen in
    // the destination aspect during its wipe, which is covered, instead of squeezing the menu).
    s32 effectiveMode = (gGameMode != gQueuedGameMode) ? gQueuedGameMode : gGameMode;
    gdx_set_force_fixed_aspect(gdx_mode_forces_fixed_aspect(effectiveMode));
}

void gdx_fixed_aspect_tick(void) {
    gdx_fixed_aspect_publish();
}

// Read-only bridge used by the ImGui input viewer. It exposes the exact mapped N64 state already
// delivered to the game, rather than polling SDL/ControlDeck a second time from the GUI pass.
int gdx_input_viewer_state(u16* out_buttons, s8* out_stick_x, s8* out_stick_y) {
    if (out_buttons != NULL) {
        *out_buttons = gControllers[0].buttonCurrent;
    }
    if (out_stick_x != NULL) {
        *out_stick_x = gControllers[0].stickX;
    }
    if (out_stick_y != NULL) {
        *out_stick_y = gControllers[0].stickY;
    }
    return gControllers[0].errno == 0;
}

// ── One port's raw pad state -> its decomp Controller, ALIGNED TO THE GAME FRAME ─────────────────
// gdx_controller_poll() runs once per HOST frame (main.cpp loop, ~60Hz). But screens that set a VI
// retrace divider run their game loop SLOWER: the scheduler (sys_main.c:428) posts the game's frame
// tick only every D_800CCFB8-th VI. The Expansion Kit Course Edit / Create Machine editors set
// D_800CCFBC=3 (19DD60.c:193), so their game loop consumes input at ~20Hz.
//
// If pressed/released EDGES are computed every host frame (as the original bridge did), a press
// edge that lands on a host frame the game skips is lost -> the editor feels unresponsive.
// Fix: ACCUMULATE press/release edges across every host frame, and only finalize them (and advance
// the prev baseline + retrigger counter) on the host frame that the scheduler will deliver a game
// tick on — `gameBoundary`, computed once per poll by the caller because the divider is a property
// of the scheduler, not of any one port. For default screens (divider 1) it is true every frame ->
// behaviour identical to the original per-frame edges (no regression). Because accumulators only
// reset on a game boundary, no rising/falling edge is ever dropped.
static void gdx_update_port_inputs(int port, u16 buttons, s8 stick_x, s8 stick_y, int gameBoundary) {
    Controller* controller = &gControllers[port];
    GdxPortInputState* state = &sPortInput[port];
    u8 stick = 0;

    // ── Derive the digital stick direction from the analog stick ──────────────────────────────
    // Mirrors the decomp's own SI-read logic (decomp/src/sys/controller.c:119-133): a direction
    // latches once the axis passes ±50 (of 80). N64 convention: +Y is UP, -Y is DOWN. The game
    // reads stickCurrent for menu navigation, so keeping the exact threshold preserves feel.
    if (stick_x < -50) {
        stick |= STICK_LEFT;
    } else if (stick_x > 50) {
        stick |= STICK_RIGHT;
    }
    if (stick_y < -50) {
        stick |= STICK_DOWN;
    } else if (stick_y > 50) {
        stick |= STICK_UP;
    }

    {
        const u16 cur_buttons = (u16) (buttons & CONT_MASK);
        const u8 cur_stick = stick;

        // Accumulate every transition seen since the last game-frame boundary.
        state->accumPressed |= (u16) (cur_buttons & ~state->prevHostButtons);
        state->accumReleased |= (u16) (state->prevHostButtons & ~cur_buttons);
        state->accumStickPressed |= (u8) (cur_stick & ~state->prevHostStick);
        state->accumStickReleased |= (u8) (state->prevHostStick & ~cur_stick);
        state->prevHostButtons = cur_buttons;
        state->prevHostStick = cur_stick;

        // Always expose the freshest raw state (the game reads *current on its tick).
        controller->buttonCurrent = cur_buttons;
        controller->stickX = stick_x;
        controller->stickY = stick_y;
        controller->stickCurrent = cur_stick;

        if (gameBoundary) {
            controller->buttonPrev = state->prevGameButtons;
            controller->buttonPressed = state->accumPressed;
            controller->buttonReleased = state->accumReleased;
            controller->stickPrev = state->prevGameStick;
            controller->stickPressed = state->accumStickPressed;
            controller->stickReleased = state->accumStickReleased;

            controller->retriggerCurrentButtonPress = false;
            if (((cur_buttons != 0) || (cur_stick != 0)) &&
                (state->prevGameButtons == cur_buttons) &&
                (state->prevGameStick == cur_stick)) {
                controller->sameInputsHeldCounter++;
                if ((controller->sameInputsHeldCounter >= 15) &&
                    (((controller->sameInputsHeldCounter - 15) % 5) == 0)) {
                    controller->retriggerCurrentButtonPress = true;
                }
            } else {
                controller->sameInputsHeldCounter = 0;
            }

            // Advance the game-frame baseline and clear the per-boundary accumulators.
            state->prevGameButtons = cur_buttons;
            state->prevGameStick = cur_stick;
            state->accumPressed = 0;
            state->accumReleased = 0;
            state->accumStickPressed = 0;
            state->accumStickReleased = 0;
        }
        // Non-boundary host frames: leave buttonPressed/Released/prev/retrigger untouched. The game
        // only reads them on its tick (which is a boundary frame), so stale values are never
        // consumed.
    }
}

// ── gSharedController: the "any controller" aggregate ────────────────────────────────────────────
// Every menu / title / credits screen drives itself from gSharedController via
// Controller_SetGlobalInputs (common.c:184), NOT from a specific port — on hardware that is what
// lets player 2 press Start on the title screen or back out of a menu. The old bridge just mirrored
// port 0 here, which was consistent with "only port 0 exists" but silently ignores pads 2-4 in the
// menus even now that they feed the game.
//
// This reproduces Controller_UpdateInputs' aggregation exactly (controller.c:78-206): the button
// and digital-stick fields are OR-ed across every CONNECTED port, retrigger is OR-ed, and the
// analog stick is the arithmetic mean over the connected ports. buttonPrev / stickPrev /
// sameInputsHeldCounter are never written by the hardware aggregate and no decomp code reads them
// on gSharedController; they are mirrored from port 0 purely so a debugger/overlay peeking at the
// struct sees something coherent.
static void gdx_publish_shared_controller(const u8* connected) {
    s32 connectedCount = 0;
    s32 sumStickX = 0;
    s32 sumStickY = 0;
    int i;

    gSharedController.errno = 0;
    gSharedController.buttonCurrent = 0;
    gSharedController.buttonPressed = 0;
    gSharedController.buttonReleased = 0;
    gSharedController.stickCurrent = 0;
    gSharedController.stickPressed = 0;
    gSharedController.stickReleased = 0;
    gSharedController.retriggerCurrentButtonPress = 0;

    for (i = 0; i < MAXCONTROLLERS; i++) {
        const Controller* controller = &gControllers[i];

        if (connected[i] == 0) {
            continue;
        }
        connectedCount++;
        gSharedController.buttonCurrent |= controller->buttonCurrent;
        gSharedController.buttonPressed |= controller->buttonPressed;
        gSharedController.buttonReleased |= controller->buttonReleased;
        gSharedController.stickCurrent |= controller->stickCurrent;
        gSharedController.stickPressed |= controller->stickPressed;
        gSharedController.stickReleased |= controller->stickReleased;
        gSharedController.retriggerCurrentButtonPress |= controller->retriggerCurrentButtonPress;
        sumStickX += controller->stickX;
        sumStickY += controller->stickY;
    }

    if (connectedCount != 0) {
        gSharedController.stickX = (s8) (sumStickX / connectedCount);
        gSharedController.stickY = (s8) (sumStickY / connectedCount);
    } else {
        gSharedController.stickX = 0;
        gSharedController.stickY = 0;
    }

    gSharedController.buttonPrev = gControllers[0].buttonPrev;
    gSharedController.stickPrev = gControllers[0].stickPrev;
    gSharedController.sameInputsHeldCounter = gControllers[0].sameInputsHeldCounter;
}

void gdx_controller_poll(void) {
    // See the comment on the scheduler counters below; declared here so the boundary predicate can
    // be evaluated once for every port.
    extern s32 D_800CCFB0, D_800CCFB4, D_800CCFB8;

    u16 buttons[MAXCONTROLLERS];
    s8 stick_x[MAXCONTROLLERS];
    s8 stick_y[MAXCONTROLLERS];
    u8 connected[MAXCONTROLLERS];
    static int s_have_baseline = 0;
    s32 divider;
    int gameBoundary;
    int i;

    // ── Read every port's mapped controller state from the LUS ControlDeck ────────────────────
    // gdx_lus_read_pads() (main.cpp) pumps SDL once and reads every LUS device mapped to every port
    // into a per-port standard N64 OSContPad. The button field is already the N64 bitmask (identical
    // layout to the decomp — see file header), so it copies straight across; the analog stick is
    // already in the -80..80 range. On failure the shim leaves every slot zeroed, so an unavailable
    // ControlDeck degrades to zero input rather than crashing.
    if (!gdx_lus_read_pads(MAXCONTROLLERS, buttons, stick_x, stick_y, connected)) {
        memset(buttons, 0, sizeof(buttons));
        memset(stick_x, 0, sizeof(stick_x));
        memset(stick_y, 0, sizeof(stick_y));
        memset(connected, 0, sizeof(connected));
    }

    // Port 1 is pinned connected — see the PORT 1 IS PINNED note on gdx_sync_controller_ports().
    // This is also what keeps the "ControlDeck unavailable" path behaving exactly as before: zero
    // input, but still a controller the game will accept.
    connected[0] = 1;

    // Scripted test input is applied last so analog events are not overwritten by the real
    // controller read above. Both dev harnesses drive PORT 1 only: they exist for deterministic
    // single-player capture runs, and giving them a port argument would change their file formats.
    gdx_autoinput_apply(&buttons[0], &stick_x[0], &stick_y[0]);

    // ── GDX_INPUT_SCRIPT (dev-only) deterministic playback override ───────────────────────────
    // Substituted HERE -- at the rawest point where the pad state lands, before the digital-stick
    // derivation and the edge-accumulation in gdx_update_port_inputs() -- so a scripted
    // press/release flows through the EXACT same accumulate-then-finalize path physical input uses.
    // No-op unless GDX_INPUT_SCRIPT is set (a single cached getenv check inside gdx_input_script.c);
    // wins over both the live LUS read and the legacy gdx-autoinput.txt mechanism above when active.
    // See gdx_input_script.h.
    {
        GdxInputPad scriptPad;
        scriptPad.buttons = buttons[0];
        scriptPad.stickX = stick_x[0];
        scriptPad.stickY = stick_y[0];
        gdx_input_script_override(&scriptPad);
        buttons[0] = scriptPad.buttons;
        stick_x[0] = scriptPad.stickX;
        stick_y[0] = scriptPad.stickY;
    }

    // Publish presence BEFORE the per-port update: a port that just went away has its Controller
    // cleared in here, and clearing it after the update would throw away the frame we just read.
    gdx_sync_controller_ports(connected);

    // The scheduler posts the game's frame tick this VI when (++D_800CCFB0 - D_800CCFB4) >=
    // D_800CCFB8. gdx_controller_poll runs before that increment, so the predicate is
    // (D_800CCFB0 + 1 - D_800CCFB4) >= D_800CCFB8. The first poll is forced to be a boundary so the
    // controllers get a baseline before the game's first tick.
    divider = (D_800CCFB8 > 0) ? D_800CCFB8 : 1;
    gameBoundary = !s_have_baseline || ((D_800CCFB0 + 1 - D_800CCFB4) >= divider);
    s_have_baseline = 1;

    for (i = 0; i < MAXCONTROLLERS; i++) {
        if (connected[i] == 0) {
            continue; // gdx_sync_controller_ports() already neutralised the port on the way out.
        }
        gdx_update_port_inputs(i, buttons[i], stick_x[i], stick_y[i], gameBoundary);
    }

    gdx_publish_shared_controller(connected);
}
