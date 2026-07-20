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

void GdxFpsOverlay::DrawElement() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("%.1f FPS", io.Framerate);
    ImGui::SameLine();
    ImGui::TextDisabled("%.2f ms", io.DeltaTime * 1000.0f);
}
