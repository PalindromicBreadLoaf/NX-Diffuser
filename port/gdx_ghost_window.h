#pragma once

#include "gdx_ghost_io.h"
#include "ship/window/gui/GuiWindow.h"

/**
 * @brief Browser for the host-side per-course player-ghost library.
 *
 * The vanilla SRAM slot remains compatible with the base game, while validated GDG1 copies under
 * `ghosts/` next to the executable remove the one-total-saved-ghost cartridge limitation. Players
 * can select any three local/imported entries per exact course. Staff ghosts remain owned by the
 * original ROM/EK unlock and loading paths.
 */
class GdxGhostWindow : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

  protected:
    void InitElement() override;
    void UpdateElement() override;
    void DrawElement() override;

  private:
    void RefreshLibrary();

    GdxGhostLibraryEntry mEntries[GDX_GHOST_LIBRARY_MAX_ENTRIES] = {};
    int mEntryCount = 0;
    int32_t mSelectedEncodedCourse = 0;
    uint64_t mSelectedGhostId = 0;
    char mStatus[256] = {};
};
