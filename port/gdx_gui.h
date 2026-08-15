#pragma once

#include "fast/Fast3dGui.h"

struct ImFont;
struct ImDrawData;

/**
 * Fast3D GUI specialization that installs the fonts used by the modern G-Diffuser menu before
 * the renderer backend uploads the ImGui font atlas.
 */
class GdxFast3dGui final : public Fast::Fast3dGui {
  public:
    using Fast3dGui::Fast3dGui;

  protected:
    void ImGuiWMInit() override;
    void ImGuiWMNewFrame() override;
    void ImGuiRenderDrawData(ImDrawData* data) override;
};

ImFont* GdxGuiFontStandard();
ImFont* GdxGuiFontLarge();
ImFont* GdxGuiFontMono();
