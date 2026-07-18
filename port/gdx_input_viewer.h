#pragma once

#include "ship/window/gui/GuiWindow.h"

/**
 * Ship of Harkinian-style N64 input overlay. The visual is built from SoH's composited texture
 * layers while the state comes from the final mapped input delivered to F-Zero X.
 */
class GdxInputViewer final : public Ship::GuiWindow {
  public:
    GdxInputViewer();

    void Draw() override;
    void DrawElement() override;

  protected:
    void InitElement() override;
    void UpdateElement() override;

  private:
    bool mTexturesLoaded = false;
};
