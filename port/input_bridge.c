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
#include "fzx_game.h" // GameMode / GameModeChangeState / MenuChangeMode enums + GET_MODE (fast restart)
#include "port_log.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
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

/* Scripted input for headless/automated test runs: if gdx-autoinput.txt sits
   next to the exe, each line is "<seconds_since_boot> <button>" (START, A, B,
   UP, DOWN, LEFT, RIGHT). The named button is held for 0.25s starting at that
   time. Real (LUS-sourced) input still works and is OR'd on top. */
static u16 gdx_autoinput_buttons(void) {
#ifdef _WIN32
    static int s_state = -1; /* -1 unread, 0 absent, 1 loaded */
    static struct { double at; u16 btn; } s_events[64];
    static int s_count = 0;
    static ULONGLONG s_start = 0;

    if (s_state == -1) {
        FILE* f = fopen("gdx-autoinput.txt", "r");
        s_state = 0;
        if (f != NULL) {
            char name[16];
            double at;
            while (s_count < 64 && fscanf(f, "%lf %15s", &at, name) == 2) {
                u16 b = 0;
                if (strcmp(name, "START") == 0) b = BTN_START;
                else if (strcmp(name, "A") == 0) b = BTN_A;
                else if (strcmp(name, "B") == 0) b = BTN_B;
                else if (strcmp(name, "UP") == 0) b = BTN_UP;
                else if (strcmp(name, "DOWN") == 0) b = BTN_DOWN;
                else if (strcmp(name, "LEFT") == 0) b = BTN_LEFT;
                else if (strcmp(name, "RIGHT") == 0) b = BTN_RIGHT;
                if (b != 0) {
                    s_events[s_count].at = at;
                    s_events[s_count].btn = b;
                    s_count++;
                }
            }
            fclose(f);
            s_state = 1;
            s_start = GetTickCount64();
            gdx_port_logf("[input] autoinput script loaded: %d events\n", s_count);
        }
    }
    if (s_state != 1) {
        return 0;
    }
    {
        double now = (double) (GetTickCount64() - s_start) / 1000.0;
        u16 buttons = 0;
        int i;
        for (i = 0; i < s_count; i++) {
            if (now >= s_events[i].at && now < s_events[i].at + 0.25) {
                buttons |= s_events[i].btn;
            }
        }
        return buttons;
    }
#else
    return 0;
#endif
}

// ── Fast restart (hold-to-retry) ────────────────────────────────────────────────────────────────
// Opt-in QoL: holding a deliberate button combo for a short dwell during a race triggers the
// game's OWN race retry -- the exact path the pause-menu RETRY uses. The pause menu sets
// gMenuChangeMode = MENU_CHANGE_RETRY (decomp/src/overlays/ovl_i3/menus.c:272); the mode driver
// func_800690FC -> func_80068BC0 (decomp/src/game/game.c:200-211) turns that into
// GAMEMODE_CHANGE_START_RELOAD, a fast IN-PLACE race re-init (Racer_Init/Camera_Init via
// func_i2_80103A70 -- no Segment_LoadOverlays/Segment_LoadAssets/Course_Init). The records screen's
// auto-retry writes the same global (records.c:502), confirming a bare write is the sanctioned
// trigger. We replicate exactly that write, gated so it only fires during stable racing.
//
// Runs on the host thread inside gdx_controller_poll(), BEFORE gdx_dispatch() runs the game fibers
// (the same single-threaded seam that owns gControllers[0]) -- no synchronization needed. The game
// resets gMenuChangeMode to INACTIVE every tick (game.c:504) after func_80068BC0 consumes it, so a
// per-frame write behaves identically to the pause menu's.

// Decomp game-mode state (decomp/src/game/game.c). Declared extern (not via a decomp umbrella
// header) to keep this TU's include surface minimal.
extern s32 gGameMode;            // :9   current mode (may carry the F3D gfx-mode bits, mask 0xC000)
extern s32 gQueuedGameMode;      // :10  requested next mode (== gGameMode when idle)
extern s16 gGameModeChangeState; // :38  GAMEMODE_UPDATE (0) when no mode change is in flight
extern s16 gMenuChangeMode;      // :39  one-shot menu-request mailbox, consumed each tick
extern s8  gGamePaused;          // :15  nonzero while the pause menu owns input

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

// Per-frame hold detector. `buttons` is the already-resolved N64 bitmask the game will see this
// tick (controller->buttonCurrent). When Fast restart is enabled and the combo is held for the
// configured dwell during stable racing, request the game's retry exactly once per hold.
static void gdx_fast_restart_tick(u16 buttons) {
    // Deliberate 3-button combo, unlikely to be held for a full second in normal play (and the
    // feature is opt-in). Deliberately excludes START (that would open the pause menu). Rebind by
    // editing this mask.
    static const u16 kFastRestartCombo = BTN_L | BTN_R | BTN_Z; // 0x2030
    static int sHeldFrames = 0;
    static int sFired = 0;
    int holdMs;
    int needFrames;

    // Off, not in a real race, a mode change already in flight, or paused -> reset and bail. The
    // (gGameMode == gQueuedGameMode && gGameModeChangeState == GAMEMODE_UPDATE) pair is exactly the
    // condition under which func_80068BC0 will honor the write (game.c:197-201,500-502); gating on
    // it avoids setting the mailbox on a frame where it would be discarded.
    if (!CVarGetInteger("gEnhancements.Gameplay.FastRestart", 0) ||
        !gdx_gamemode_is_race(gGameMode) ||
        gGameMode != gQueuedGameMode ||
        gGameModeChangeState != GAMEMODE_UPDATE ||
        gGamePaused != 0 ||
        (buttons & kFastRestartCombo) != kFastRestartCombo) {
        sHeldFrames = 0;
        sFired = 0;
        return;
    }

    holdMs = CVarGetInteger("gEnhancements.Gameplay.FastRestartHoldMs", 1000);
    if (holdMs < 100) {
        holdMs = 100;
    }
    // One poll == one game tick, and F-Zero X is natively 60Hz, so convert the dwell at 60 ticks/s.
    // (An uncapped host frame rate shortens the wall-clock dwell proportionally -- acceptable for an
    // opt-in toggle; the 1000ms default = ~60 ticks feels like ~1s at the native rate.)
    needFrames = holdMs * 60 / 1000;
    if (needFrames < 1) {
        needFrames = 1;
    }

    if (sHeldFrames < needFrames) {
        sHeldFrames++;
    }
    if (!sFired && sHeldFrames >= needFrames) {
        gMenuChangeMode = MENU_CHANGE_RETRY; // consumed next tick by func_80068BC0 (game.c:201)
        sFired = 1;
    }
}

void gdx_controller_poll(void) {
    gdx_ensure_controller_connected();

    Controller* controller = &gControllers[0];
    u16 buttons = 0;
    s8 stick_x = 0;
    s8 stick_y = 0;
    u8 stick = 0;

    // Scripted test input (OR'd on top of real input).
    buttons |= gdx_autoinput_buttons();

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

    // ── Write decomp globals + compute edges (unchanged from the original bridge) ──────────────
    controller->buttonPrev = controller->buttonCurrent;
    controller->buttonCurrent = buttons & CONT_MASK;
    u16 button_diff = controller->buttonPrev ^ controller->buttonCurrent;
    controller->buttonPressed = button_diff & controller->buttonCurrent;
    controller->buttonReleased = button_diff & controller->buttonPrev;

    controller->stickPrev = controller->stickCurrent;
    controller->stickX = stick_x;
    controller->stickY = stick_y;
    controller->stickCurrent = stick;
    u8 stick_diff = controller->stickPrev ^ controller->stickCurrent;
    controller->stickPressed = stick_diff & controller->stickCurrent;
    controller->stickReleased = stick_diff & controller->stickPrev;

    controller->retriggerCurrentButtonPress = false;
    if (((controller->buttonCurrent != 0) || (controller->stickCurrent != 0)) &&
        (controller->buttonPrev == controller->buttonCurrent) &&
        (controller->stickPrev == controller->stickCurrent)) {
        controller->sameInputsHeldCounter++;
        if ((controller->sameInputsHeldCounter >= 15) &&
            (((controller->sameInputsHeldCounter - 15) % 5) == 0)) {
            controller->retriggerCurrentButtonPress = true;
        }
    } else {
        controller->sameInputsHeldCounter = 0;
    }

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

    // ── Fast restart (hold-to-retry) ──────────────────────────────────────────────────────────
    // Opt-in: hold L+R+Z during a race to trigger the game's own retry. Reads the resolved pad we
    // just wrote; a no-op unless gEnhancements.Gameplay.FastRestart is enabled (default off).
    gdx_fast_restart_tick(controller->buttonCurrent);
}
