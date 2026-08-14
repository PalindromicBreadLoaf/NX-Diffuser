#include "gdx_swkbd.h"

#include <imgui.h>
#include <SDL2/SDL.h>

namespace {

bool sQueried = false;
bool sSupported = false;
bool sWanted = false;

} // namespace

extern "C" void gdx_swkbd_tick(void) {
    if (!sQueried) {
        if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
            return;
        }
        sSupported = SDL_HasScreenKeyboardSupport() == SDL_TRUE;
        sQueried = true;
    }
    if (!sSupported || ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    const bool wanted = ImGui::GetIO().WantTextInput;
    if (wanted && !sWanted) {
        SDL_StartTextInput();
    } else if (!wanted && sWanted && SDL_IsTextInputActive() == SDL_TRUE) {
        SDL_StopTextInput();
    }
    sWanted = wanted;
}
