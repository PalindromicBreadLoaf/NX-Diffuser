#include "gdx_imgui_nav.h"

#include <cmath> // fabsf (analog scroll response curve)

#include <imgui.h>
#include <SDL2/SDL.h>

#include "libultraship/bridge/consolevariablebridge.h" // CVarGetInteger

namespace {

// Our own controller handle for menu navigation. libultraship's ControlDeck opens the controller
// for game input; ImGui's platform backend may open it for its own nav reader. Opening it a third
// time here is harmless (SDL allows multiple handles) and keeps this module self-contained: we read
// physical state directly rather than the game-facing pad, which is intentionally zeroed while the
// menu blocks game input.
SDL_GameController* sController = nullptr;

// Left stick -> directional nav (digital, folded into the D-pad — see the tick below). A firm
// threshold so a deliberate push registers one move and resting drift never does. Menu nav is
// discrete, so this behaves like a D-pad press, not a variable-speed axis.
constexpr float kNavStickThreshold = 0.50f;

// Right stick -> content-pane scroll. Onset deadzone for the analog scroll feed; below this the
// stick is centered. Lower than the nav threshold because scrolling is a continuous, forgiving
// action where an early, smooth onset feels better than a firm detent.
constexpr float kScrollDeadzone = 0.30f;

SDL_GameController* AcquireController() {
    if (sController != nullptr && SDL_GameControllerGetAttached(sController)) {
        return sController;
    }
    if (sController != nullptr) {
        SDL_GameControllerClose(sController);
        sController = nullptr;
    }
    const int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        if (SDL_IsGameController(i)) {
            sController = SDL_GameControllerOpen(i);
            if (sController != nullptr) {
                break;
            }
        }
    }
    return sController;
}

void FeedButton(ImGuiIO& io, SDL_GameController* c, SDL_GameControllerButton sdlBtn, ImGuiKey key) {
    io.AddKeyEvent(key, SDL_GameControllerGetButton(c, sdlBtn) != 0);
}

// Read one stick axis as a normalized -1..1 value.
float ReadAxis(SDL_GameController* c, SDL_GameControllerAxis axis) {
    return static_cast<float>(SDL_GameControllerGetAxis(c, axis)) / 32767.0f;
}

// Map one stick axis (raw -32768..32767) to a pair of opposing analog nav keys (0..1 each) with a
// rescaled deadzone and a mild ease-in curve: past the deadzone the value ramps from 0 (not from a
// hard 0.30 step), and the quadratic curve gives finer control near center. Used for the right
// stick's content-pane scroll, which ImGui drives from the LStick* analog keys.
void FeedScrollAxis(ImGuiIO& io, SDL_GameController* c, SDL_GameControllerAxis axis, ImGuiKey negKey,
                    ImGuiKey posKey) {
    const float v = ReadAxis(c, axis);
    const float mag = fabsf(v);
    float out = 0.0f;
    if (mag > kScrollDeadzone) {
        const float rescaled = (mag - kScrollDeadzone) / (1.0f - kScrollDeadzone); // 0..1 past deadzone
        out = rescaled * rescaled;                                                 // quadratic ease-in
    }
    const float neg = (v < -kScrollDeadzone) ? out : 0.0f;
    const float pos = (v > kScrollDeadzone) ? out : 0.0f;
    io.AddKeyAnalogEvent(negKey, neg > 0.0f, neg);
    io.AddKeyAnalogEvent(posKey, pos > 0.0f, pos);
}

} // namespace

extern "C" void gdx_imgui_nav_tick(void) {
    // Stock behavior unless the user enabled gamepad menu navigation.
    if (CVarGetInteger("gControlNav", 0) == 0) {
        return;
    }
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    SDL_GameController* c = AcquireController();
    if (c == nullptr) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();

    // ImGui's nav system ignores gamepad keys unless a backend declares
    // ImGuiBackendFlags_HasGamepad. On Windows the Win32 backend's XInput
    // reader is compiled out (IMGUI_IMPL_WIN32_DISABLE_GAMEPAD in
    // libultraship/cmake/dependencies/windows.cmake) so this feed owns the
    // flag; on SDL-backend platforms the backend also sets it — harmless.
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

    // Menu toggle (Back/Create/View) — fed every frame so libultraship's Gui toggle can open the
    // menu from a closed state, and D-pad + face buttons for navigation once it is open. When the
    // menu is closed ImGui ignores the nav keys (NavEnableGamepad is off) but still reads the toggle.
    FeedButton(io, c, SDL_CONTROLLER_BUTTON_BACK, ImGuiKey_GamepadBack);
    FeedButton(io, c, SDL_CONTROLLER_BUTTON_START, ImGuiKey_GamepadStart);

    // Directional navigation: D-pad OR left stick. ImGui's menu nav reads ONLY the D-pad keys for
    // item movement (imgui.cpp NavUpdate) — it never moves the selection from the analog LStick*
    // keys (those only scroll or move windows). So a raw left stick feels dead in menus. We fix that
    // by folding the left stick into the D-pad: either input navigates. Holding a direction lets
    // ImGui's own typematic repeat scroll a long list. The left stick uses a firm threshold so it
    // acts like a discrete D-pad press rather than a variable-speed axis.
    const float lx = ReadAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    const float ly = ReadAxis(c, SDL_CONTROLLER_AXIS_LEFTY); // SDL: up is negative
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp,
                   SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0 || ly < -kNavStickThreshold);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown,
                   SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0 || ly > kNavStickThreshold);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,
                   SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0 || lx < -kNavStickThreshold);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight,
                   SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0 || lx > kNavStickThreshold);

    // Face buttons — SDL A/B/X/Y are position-named (A = bottom). ImGui's Face* keys are also
    // position-named: FaceDown activates, FaceRight cancels/back. This keeps the physical bottom
    // button = "confirm" on both Xbox and DualSense (Cross), matching ImGui's default expectation.
    FeedButton(io, c, SDL_CONTROLLER_BUTTON_A, ImGuiKey_GamepadFaceDown);
    FeedButton(io, c, SDL_CONTROLLER_BUTTON_B, ImGuiKey_GamepadFaceRight);
    FeedButton(io, c, SDL_CONTROLLER_BUTTON_X, ImGuiKey_GamepadFaceLeft);
    FeedButton(io, c, SDL_CONTROLLER_BUTTON_Y, ImGuiKey_GamepadFaceUp);

    // Shoulders — ImGui uses L1/R1 to switch focus between windows/columns.
    FeedButton(io, c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, ImGuiKey_GamepadL1);
    FeedButton(io, c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, ImGuiKey_GamepadR1);

    // Right stick scrolls the focused content pane. ImGui reads scroll from the LStick* analog keys
    // (imgui.cpp NavUpdate "Manual scroll with LStick"), so we feed the RIGHT stick into those keys:
    // left stick = navigate, right stick = scroll — the console-standard split. The RStick* ImGui
    // keys are not consumed by nav in this build, so they are intentionally left unfed.
    FeedScrollAxis(io, c, SDL_CONTROLLER_AXIS_RIGHTX, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight);
    FeedScrollAxis(io, c, SDL_CONTROLLER_AXIS_RIGHTY, ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown);
}
