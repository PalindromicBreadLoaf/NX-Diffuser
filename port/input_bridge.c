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

void gdx_controller_poll(void) {
    gdx_ensure_controller_connected();

    Controller* controller = &gControllers[0];
    u16 buttons = 0;
    s8 stick_x = 0;
    s8 stick_y = 0;
    u8 stick = 0;

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
