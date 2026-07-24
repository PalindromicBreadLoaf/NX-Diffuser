// G-Diffuser — host input bridge for the PORT.
//
// The decomp's N64 SI polling path is disabled under PORT, so the game never sees a connected
// controller unless we feed the decomp controller globals (gControllers[0] / gSharedController)
// from the host every frame.
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
// main.cpp (gdx_lus_read_pad) that calls ControlDeck::WriteToPad() and hands us port 0's resolved
// N64 state as plain scalars. This file keeps ownership of the decomp side: forcing port 0
// connected, deriving the digital stick direction, computing pressed/released/retrigger edges,
// and mirroring into gSharedController — exactly as before, just sourced from LUS.
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
// no synchronization needed (see docs/menu/CONTROLS_TAB.md §5).

#ifndef _LANGUAGE_C
#define _LANGUAGE_C 1
#endif

#include "PR/os_pfs.h"
#include "controller.h"
#include "fzx_game.h" // GameMode values + GET_MODE (in-race safety checks)
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

// Implemented in main.cpp (C++). Reads the LUS ControlDeck's resolved state for controller
// `port` — i.e. the InputEditor's mappings applied to the connected keyboard / SDL gamepad /
// mouse devices — into a standard N64 button bitmask + analog stick (-80..80). Returns 1 on
// success, 0 if the ControlDeck is unavailable (degrade to zero input; never crash). The scalar
// widths match the decomp's u16 (unsigned short) / s8 (signed char) exactly.
extern int gdx_lus_read_pad(int port, u16* out_buttons, s8* out_stick_x, s8* out_stick_y);

static void gdx_ensure_controller_connected(void) {
    static int s_logged = 0;

    gControllersConnected = 1;
    gPlayerControlPorts[0] = PORT_1;
    for (int i = 1; i < MAXCONTROLLERS; i++) {
        gPlayerControlPorts[i] = PORT_DISCONNECTED;
        gControllers[i].errno = CONT_NO_RESPONSE_ERROR;
        gControllerStatus[i].errno = CONT_NO_RESPONSE_ERROR;
    }

    gControllers[0].errno = 0;
    gControllerStatus[0].errno = 0;
    gControllerStatus[0].type = CONT_TYPE_NORMAL;

    if (!s_logged) {
        // Bindings now come from the LUS ControlDeck and are fully remappable in the Input Editor
        // (F1 -> Controls -> "Controller Configuration..."). Out-of-the-box keyboard defaults
        // (LUS::ControllerDefaultMappings): Space=Start, X=A, C=B, Z=Z, E=L, R=R, WASD=analog
        // stick, arrows=C-buttons, TFGH=D-pad. Any SDL gamepad is auto-detected and mapped too.
        gdx_port_logf("[input] LUS ControlDeck active — remap in F1 > Controls > Input Editor "
                      "(default keyboard: Space=Start, X=A, C=B, WASD=stick, arrows=C)\n");
        s_logged = 1;
    }
}

/* Scripted input for headless/automated test runs. If gdx-autoinput.txt sits next to the exe it
   is parsed once and combined with the live (LUS-sourced) input every poll.
   ---------------------------------------------------------------------------------------------
   FORMAT (two timebases; auto-detected by a version marker on the FIRST non-blank line):
   ---------------------------------------------------------------------------------------------
   TICK MODE (deterministic — required for the R2 bit-identical PCM gate, C-R2.3):
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
// as gdx_lus_read_pad above -- to avoid pulling a LUS/C++ umbrella header into this decomp-
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
// STALENESS CONTRACT (2026-07-16 squeeze audit): the per-frame tick below reads gGameMode as of
// the END of the previous dispatch, but the game flips gGameMode MID-dispatch (game.c mode-flip)
// and renders the new mode's transition in the same dispatch. The flip frame therefore rendered
// with a stale flag -- one whole menu frame pillarboxed on every editor exit (owner-visible
// "tries to do 4:3" on main menu -> Cup Select). gdx_fixed_aspect_publish() is therefore ALSO
// called from the decomp's mode-flip site under PORT so consumers that read the CVar live
// (Transition_Draw, Fast3dGui::DrawGame) and the interpreter's per-task re-latch see the
// post-flip truth within the same frame.
// The flag is a plain process global in libultraship (interpreter.cpp), NOT a CVar: the
// 2026-07-16 audit found the old CVar form persisted as 1 into the owner's Debug
// gdiffuser.cfg.json (saved while inside an editor), pillarboxing pre-tick boot frames.
// Runtime state never belongs in the config file.
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

    // TRANSIENT CUP-SELECT SQUEEZE FIX (obs #1758): evaluate the flag on the mode that will
    // actually RENDER this dispatch, not on the mode currently latched in gGameMode.
    //
    // The game flips gGameMode = gQueuedGameMode MID-dispatch (game.c) and then renders the NEW
    // mode's appear transition in that SAME dispatch. The renderer, however, commits its
    // whole-frame pillarbox target from the PRE-dispatch flag (interpreter.cpp StartFrame), and
    // that target is NOT recomputed after the mid-dispatch flip. So on an editor->menu flip the
    // pre-dispatch tick still sees the OLD editor mode, forces 4:3, and the destination menu
    // frame gets composited into a centered 4:3 pillarbox for one frame -- the owner-visible
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
    // to 4:3. This is the low-risk gate documented in #1758 (it can render the outgoing screen in
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

void gdx_controller_poll(void) {
    gdx_ensure_controller_connected();

    Controller* controller = &gControllers[0];
    u16 buttons = 0;
    s8 stick_x = 0;
    s8 stick_y = 0;
    u8 stick = 0;

    // ── Read the mapped controller state for port 0 from the LUS ControlDeck ──────────────────
    // gdx_lus_read_pad() (main.cpp) pumps SDL + reads every LUS device mapped to port 0 into a
    // standard N64 OSContPad. The button field is already the N64 bitmask (identical layout to
    // the decomp — see file header), so we OR it straight in; the analog stick is already in the
    // -80..80 range. If the ControlDeck is unavailable we degrade to zero input (never crash) —
    // gdx_ensure_controller_connected() still reports port 0 connected so the game does not error.
    {
        u16 lus_buttons = 0;
        s8 lus_x = 0;
        s8 lus_y = 0;
        if (gdx_lus_read_pad(0, &lus_buttons, &lus_x, &lus_y)) {
            buttons |= lus_buttons;
            stick_x = lus_x;
            stick_y = lus_y;
        }
    }

    // Scripted test input is applied last so analog events are not overwritten
    // by the real controller read above.
    gdx_autoinput_apply(&buttons, &stick_x, &stick_y);

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

    // ── Write decomp globals + compute edges, ALIGNED TO THE GAME FRAME ────────────────────────
    // gdx_controller_poll() runs once per HOST frame (main.cpp loop, ~60Hz). But screens that set
    // a VI retrace divider run their game loop SLOWER: the scheduler (sys_main.c:428) posts the
    // game's frame tick only every D_800CCFB8-th VI. The Expansion Kit Course Edit / Create
    // Machine editors set D_800CCFBC=3 (19DD60.c:193), so their game loop consumes input at ~20Hz.
    //
    // If pressed/released EDGES are computed every host frame (as the original bridge did), a press
    // edge that lands on a host frame the game skips is lost -> the editor felt "unresponsive"
    // (owner report). Fix: ACCUMULATE press/release edges across every host frame, and only
    // finalize them (and advance the prev baseline + retrigger counter) on the host frame that the
    // scheduler will deliver a game tick on. That host frame is predicted from the same divider
    // counters the scheduler uses: it posts the tick this VI when (++D_800CCFB0 - D_800CCFB4) >=
    // D_800CCFB8. gdx_controller_poll runs before that increment, so the predicate is
    // (D_800CCFB0 + 1 - D_800CCFB4) >= D_800CCFB8. For default screens (divider 1) this is true
    // every frame -> behaviour identical to the original per-frame edges (no regression).
    // Because accumulators only reset on a game boundary, no rising/falling edge is ever dropped.
    extern s32 D_800CCFB0, D_800CCFB4, D_800CCFB8;

    const u16 cur_buttons = buttons & CONT_MASK;
    const u8 cur_stick = stick;

    static int s_have_baseline = 0;
    static u16 s_prev_host_buttons = 0;
    static u8 s_prev_host_stick = 0;
    static u16 s_accum_pressed = 0, s_accum_released = 0;
    static u8 s_accum_stick_pressed = 0, s_accum_stick_released = 0;
    static u16 s_prev_game_buttons = 0;
    static u8 s_prev_game_stick = 0;

    // Accumulate every transition seen since the last game-frame boundary.
    s_accum_pressed |= (u16)(cur_buttons & ~s_prev_host_buttons);
    s_accum_released |= (u16)(s_prev_host_buttons & ~cur_buttons);
    s_accum_stick_pressed |= (u8)(cur_stick & ~s_prev_host_stick);
    s_accum_stick_released |= (u8)(s_prev_host_stick & ~cur_stick);
    s_prev_host_buttons = cur_buttons;
    s_prev_host_stick = cur_stick;

    // Always expose the freshest raw state (the game reads *current on its tick).
    controller->buttonCurrent = cur_buttons;
    controller->stickX = stick_x;
    controller->stickY = stick_y;
    controller->stickCurrent = cur_stick;

    const s32 divider = (D_800CCFB8 > 0) ? D_800CCFB8 : 1;
    const int game_boundary = !s_have_baseline || ((D_800CCFB0 + 1 - D_800CCFB4) >= divider);
    if (game_boundary) {
        s_have_baseline = 1;
        controller->buttonPrev = s_prev_game_buttons;
        controller->buttonPressed = s_accum_pressed;
        controller->buttonReleased = s_accum_released;
        controller->stickPrev = s_prev_game_stick;
        controller->stickPressed = s_accum_stick_pressed;
        controller->stickReleased = s_accum_stick_released;

        controller->retriggerCurrentButtonPress = false;
        if (((cur_buttons != 0) || (cur_stick != 0)) &&
            (s_prev_game_buttons == cur_buttons) &&
            (s_prev_game_stick == cur_stick)) {
            controller->sameInputsHeldCounter++;
            if ((controller->sameInputsHeldCounter >= 15) &&
                (((controller->sameInputsHeldCounter - 15) % 5) == 0)) {
                controller->retriggerCurrentButtonPress = true;
            }
        } else {
            controller->sameInputsHeldCounter = 0;
        }

        // Advance the game-frame baseline and clear the per-boundary accumulators.
        s_prev_game_buttons = cur_buttons;
        s_prev_game_stick = cur_stick;
        s_accum_pressed = 0;
        s_accum_released = 0;
        s_accum_stick_pressed = 0;
        s_accum_stick_released = 0;
    }
    // Non-boundary host frames: leave buttonPressed/Released/prev/retrigger untouched. The game
    // only reads them on its tick (which is a boundary frame), so stale values are never consumed.

    gSharedController.errno = 0;
    gSharedController.buttonPrev = controller->buttonPrev;
    gSharedController.buttonCurrent = controller->buttonCurrent;
    gSharedController.buttonPressed = controller->buttonPressed;
    gSharedController.buttonReleased = controller->buttonReleased;
    gSharedController.stickPrev = controller->stickPrev;
    gSharedController.stickX = controller->stickX;
    gSharedController.stickY = controller->stickY;
    gSharedController.stickCurrent = controller->stickCurrent;
    gSharedController.stickPressed = controller->stickPressed;
    gSharedController.stickReleased = controller->stickReleased;
    gSharedController.retriggerCurrentButtonPress = controller->retriggerCurrentButtonPress;
    gSharedController.sameInputsHeldCounter = controller->sameInputsHeldCounter;
}
