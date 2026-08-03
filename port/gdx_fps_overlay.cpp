#include "gdx_fps_overlay.h"

#include <imgui.h>

GdxFpsOverlay::GdxFpsOverlay()
    : Ship::GuiWindow("gOpenWindows.FpsCounter", false, "FPS Counter") {
}

void GdxFpsOverlay::InitElement() {
}

void GdxFpsOverlay::UpdateElement() {
}

void GdxFpsOverlay::Draw() {
    if (!IsVisible()) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 topRight = viewport->WorkPos + ImVec2(viewport->WorkSize.x - 12.0f, 12.0f);
    ImGui::SetNextWindowPos(topRight, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.72f);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoInputs;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.065f, 0.082f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.14f));

    if (ImGui::Begin("FPS Counter##GdxOverlay", nullptr, flags)) {
        DrawElement();
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    SyncVisibilityConsoleVariable();
}

// Real-FPS visibility. When Frame Interpolation is ON the sim runs
// at 60 Hz but the renderer PRESENTS multiple sub-frames per tick (each a full ImGui frame), so
// io.Framerate already tracks true presents/sec — but the raw number alone hides that the 60 Hz
// logic rate is unchanged. Annotate it as "144.0 FPS (sim 60 Hz)" so the presented rate and the fixed
// logic rate are both legible. Declared locally (extern "C") — same minimal-include idiom the menu
// uses for the other gdx_gfx_interp_* accessors; no n64_gfx_bridge.h dependency added here.
extern "C" int gdx_gfx_interp_host_active(void);
// Per-tick truth: main.cpp forces interpolation off THIS tick (Course Edit / Create Machine editors)
// even while the menu's raw CVar (gdx_gfx_interp_host_active) is still on. Reading the raw CVar alone
// here would keep showing "(sim 60 Hz)" while the editor override actually paused interpolation.
extern "C" int gdx_gfx_interp_tick_active(void);

void GdxFpsOverlay::DrawElement() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("%.1f FPS", io.Framerate);
    const bool hostActive = gdx_gfx_interp_host_active() != 0;
    const bool tickActive = gdx_gfx_interp_tick_active() != 0;
    if (hostActive && !tickActive) {
        ImGui::SameLine();
        ImGui::TextDisabled("(interp paused)");
    } else if (hostActive) {
        ImGui::SameLine();
        ImGui::TextDisabled("(sim 60 Hz)");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%.2f ms", io.DeltaTime * 1000.0f);
}
