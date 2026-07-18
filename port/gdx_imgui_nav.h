// port/gdx_imgui_nav.h — feed ImGui menu navigation from the SDL controller.
//
// WHY: libultraship drives ImGui gamepad navigation through the ImGui platform backend's own
// gamepad reader — ImGui_ImplSDL2 (Linux) or ImGui_ImplWin32 (Windows). The Win32 backend reads
// gamepads via XInput, which does NOT see a raw DualSense (a HID/PS5 device). So on Windows a
// DualSense can drive the game (libultraship reads it via SDL) but cannot navigate the menu. This
// module closes that gap: it reads the first connected SDL game controller directly and feeds the
// ImGui gamepad nav keys, so ANY SDL-recognized pad drives the menu on every platform, regardless
// of the ImGui backend. Gated on the "gControlNav" CVar (off => no feed, stock behavior).
//
// Call gdx_imgui_nav_tick() once per frame BEFORE the Gui's StartDraw() (which runs ImGui's
// NewFrame): the fed key events are consumed by that frame's ImGui::NewFrame.

#ifdef __cplusplus
extern "C" {
#endif

// Read the active SDL controller and feed ImGui gamepad nav keys for this frame. No-op when
// gControlNav is 0 or no controller is connected. Safe to call every frame from boot.
void gdx_imgui_nav_tick(void);

#ifdef __cplusplus
}
#endif
