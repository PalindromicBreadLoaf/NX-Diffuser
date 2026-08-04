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

// Real-FPS visibility. When Frame Interpolation is ON the renderer PRESENTS multiple sub-frames per
// tick (each a full ImGui frame), so io.Framerate tracks true presents/sec — but that number alone
// says nothing about whether the SIMULATION is keeping up. Annotate it as "144.0 FPS (sim 59.9 Hz)"
// so both rates are legible at a glance.
//
// The sim figure is MEASURED (gdx_host_sim_hz, written by the frame loop in main.cpp). It used to be
// the hardcoded string "(sim 60 Hz)", which was an assertion dressed as a readout: the sim was
// observed running as low as 8.6 Hz — the game visibly in slow motion — while this line still read
// 60. Never print a rate here that was not measured; a confident wrong number sends people looking
// in the wrong place for hours.
//
// Declared locally (extern "C") — same minimal-include idiom the menu uses for the other
// gdx_gfx_interp_* accessors; no n64_gfx_bridge.h dependency added here.
extern "C" int gdx_gfx_interp_host_active(void);
extern "C" double gdx_host_sim_hz(void);
// Per-tick truth: main.cpp forces interpolation off THIS tick (only Course Edit's ~20 Hz cursor
// mode now — test runs and Create Machine interpolate since the divider-conditional gate)
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
        // Colour is the whole point of showing this: a sim rate below 60 means the GAME CLOCK is
        // losing time (everything animates and counts down slow), which is a correctness fault, not
        // a smoothness one. Amber below 58 Hz, red below 50 — so it is visible at a glance without
        // reading the number. 0.0 means the rolling window has not closed yet on this run.
        const double simHz = gdx_host_sim_hz();
        ImGui::SameLine();
        if (simHz <= 0.0) {
            ImGui::TextDisabled("(sim --)");
        } else if (simHz < 50.0) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "(sim %.1f Hz)", simHz);
        } else if (simHz < 58.0) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f), "(sim %.1f Hz)", simHz);
        } else {
            ImGui::TextDisabled("(sim %.1f Hz)", simHz);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%.2f ms", io.DeltaTime * 1000.0f);
}
