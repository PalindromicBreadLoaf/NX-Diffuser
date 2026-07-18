#pragma once

#include <string>

#include "ship/window/gui/GuiWindow.h"

/**
 * Full-screen modern settings menu inspired by Ship of Harkinian's 9.2.3 menu shell.
 *
 * The menu remains port-owned: all existing G-Diffuser CVars and callbacks are preserved, while
 * the presentation changes from a row of ImGui dropdowns to header navigation, a sidebar, search,
 * responsive scrolling, embedded tool windows, and confirmation modals.
 */
class GdxMenu final : public Ship::GuiWindow {
  public:
    GdxMenu();

    void Draw() override;
    void DrawElement() override;

  protected:
    void InitElement() override;
    void UpdateElement() override;

  private:
    enum class Header : int {
        Settings,
        Enhancements,
        Workshop,
        Online,
        DevTools,
    };

    enum class Page : int {
        General,
        Audio,
        Graphics,
        Controls,
        InputViewer,
        EnhancementGraphics,
        Gameplay,
        Practice,
        Ghosts,
        Content,
        OnlineOverview,
        DeveloperGeneral,
        Stats,
        Console,
        GfxDebugger,
    };

    void DrawHeader();
    void DrawSidebar();
    void DrawCurrentPage();
    void DrawSearchResults();
    void DrawQuitModal();

    void DrawGeneralPage();
    void DrawGraphicsMenu(bool enhancementsOnly);
    void DrawAudioMenu();
    void DrawGameplayMenu();
    void DrawPracticeMenu();
    void DrawControlsMenu();
    void DrawInputViewerMenu();
    void DrawGhostsMenu();
    void DrawWorkshopMenu();
    void DrawOnlineMenu();
    void DrawDeveloperMenu();
    void DrawToolWindowPage(const char* name, const char* description);
    void DrawAboutMenu();

    void SelectHeader(Header header);
    void SelectPage(Page page);
    Header HeaderForPage(Page page) const;
    Page FirstPageForHeader(Header header) const;
    const char* PageTitle(Page page) const;

    // Restore io.KeyRepeatDelay/Rate to their pre-menu values if the menu tightened them for
    // gamepad nav. Safe to call when nothing was applied (no-op).
    void RestoreNavRepeatTuning();

    Header mActiveHeader = Header::Settings;
    Page mActivePage = Page::General;
    char mSearch[128] = {};
    bool mOpenQuitModal = false;
    int mLastLowPassHz = 15000;

    // Gamepad-navigation state (only meaningful while gControlNav is on).
    bool mFocusSidebar = false;    // one-shot: land nav focus on the active sidebar page next draw
    bool mMenuWasVisible = false;  // tracks open transitions so focus is seeded on each open
    bool mNavTuningApplied = false;     // io.KeyRepeat* currently overridden by the menu
    float mSavedKeyRepeatDelay = 0.0f;  // pre-override io.KeyRepeatDelay
    float mSavedKeyRepeatRate = 0.0f;   // pre-override io.KeyRepeatRate
};
