// G-Diffuser — minimal host input bridge for PORT boot bring-up.
//
// The decomp's N64 SI polling path is disabled under PORT, so the title screen never sees a
// connected controller unless we feed the controller globals from the host.

#ifndef _LANGUAGE_C
#define _LANGUAGE_C 1
#endif

#include "PR/os_pfs.h"
#include "controller.h"
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

static int gdx_key_down(int key) {
#ifdef _WIN32
    return (GetAsyncKeyState(key) & 0x8000) != 0;
#else
    (void)key;
    return 0;
#endif
}

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
        gdx_port_logf("[input] keyboard controller connected (Enter=Start, Space/X=A, Z/Esc=B, arrows=stick)\n");
        s_logged = 1;
    }
}

/* Scripted input for headless/automated test runs: if gdx-autoinput.txt sits
   next to the exe, each line is "<seconds_since_boot> <button>" (START, A, B,
   UP, DOWN, LEFT, RIGHT). The named button is held for 0.25s starting at that
   time. Real keyboard input still works and is OR'd on top. */
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

void gdx_controller_poll(void) {
    gdx_ensure_controller_connected();

    Controller* controller = &gControllers[0];
    u16 buttons = 0;
    s8 stick_x = 0;
    s8 stick_y = 0;
    u8 stick = 0;

    buttons |= gdx_autoinput_buttons();

    if (gdx_key_down(VK_RETURN)) {
        buttons |= BTN_START;
    }
    if (gdx_key_down(VK_SPACE) || gdx_key_down('X')) {
        buttons |= BTN_A;
    }
    if (gdx_key_down('Z') || gdx_key_down(VK_BACK) || gdx_key_down(VK_ESCAPE)) {
        buttons |= BTN_B;
    }
    if (gdx_key_down(VK_UP)) {
        buttons |= BTN_UP;
        stick |= STICK_UP;
        stick_y = 80;
    }
    if (gdx_key_down(VK_DOWN)) {
        buttons |= BTN_DOWN;
        stick |= STICK_DOWN;
        stick_y = -80;
    }
    if (gdx_key_down(VK_LEFT)) {
        buttons |= BTN_LEFT;
        stick |= STICK_LEFT;
        stick_x = -80;
    }
    if (gdx_key_down(VK_RIGHT)) {
        buttons |= BTN_RIGHT;
        stick |= STICK_RIGHT;
        stick_x = 80;
    }

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
}
