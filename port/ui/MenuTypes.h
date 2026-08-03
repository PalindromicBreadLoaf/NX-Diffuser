// port/ui/MenuTypes.h — declarative menu registry for the G-Diffuser menu.
//
// PROVENANCE
// ----------
// Ported from HarbourMasters/Lighthouse, file src/port/UI/MenuTypes.h on branch `develop`.
// Lighthouse is published under CC0 1.0 Universal (Creative Commons Public Domain Dedication) —
// copy, modify and redistribute freely, no attribution required; this notice is courtesy, not
// obligation. Same upstream libultraship as this fork; the registry is pure port-side data and
// touches nothing below the public ImGui + CVar API. See port/ui/UIWidgets.hpp for the widget
// library this file is the registry layer for.
//
// WHAT THIS REPLACES
// ------------------
// port/gdx_menu.cpp used to describe its menu twice, in two places that could silently disagree:
//   1. a hand-rolled `enum class Page` with ~30 switch cases (PageTitle / HeaderForPage /
//      FirstPageForHeader / DrawCurrentPage), plus one Draw<Name>Menu() function per page whose
//      body was a straight-line sequence of widget calls; and
//   2. a hand-maintained `static const SearchPage pages[]` table of PAGE-level keywords, typed by
//      hand, which is what the search box matched against. Search could therefore only ever say
//      "the Graphics page exists" — never "the control you are looking for is called Texture
//      filter and it lives in Settings -> Graphics".
// Describing every control as DATA instead makes both fall out for free: the sidebar is the
// registry's key order, the page body is the registry's widget list, and search walks the same
// list the drawer does, so a control cannot be visible-but-unsearchable (or vice versa) ever again.
//
// THE MODEL
// ---------
//   MainMenuEntry (a header tab: "Settings")
//     └ SidebarEntry (a sidebar page: "Graphics"), columnCount 1..3
//         └ WidgetInfo[column] — one entry per individual control
//
// ADAPTATIONS vs Lighthouse
// -------------------------
//  1. Namespaced. Upstream declares WidgetInfo/SidebarEntry/DisableOption/... at global scope;
//     everything here lives in namespace GdxUI so the registry adds no unqualified global symbols
//     to a port that already carries plenty (same reasoning as UIWidgets ADAPTATION #10).
//  2. `Options()` is a set of type-safe overloads instead of upstream's
//     `Options(OptionsVariant)` + `switch (type)` + `std::get<T>`. Upstream's version compiles for
//     any Options struct and only discovers a type/widget mismatch at RUNTIME, as a
//     std::bad_variant_access thrown out of the draw loop (Menu.cpp:547 catches it and asserts).
//     Here a mismatched pair is a compile error, and MenuDrawItem needs no try/catch at all.
//  3. `comboItems` (ordered std::vector<const char*>) added. Upstream drives every combobox from
//     UIWidgets::ComboboxOptions::comboMap, a std::unordered_map whose iteration order is
//     unspecified — see UIWidgets.hpp's gap list: "CVarCombobox's std::unordered_map overload
//     renders its rows in unspecified order, so it is unusable for any dropdown whose ordering is
//     meaningful". Every dropdown in this menu is index-ordered (MSAA, texture filter, z-fighting,
//     button outlines, audio backend), so the registry carries an ordered list and MenuDrawItem
//     uses UIWidgets' vector overload.
//  4. `disableWhen` / `hideWhen` added: declarative lists of DisableOption evaluated by
//     MenuDrawItem against the once-per-frame disabled map. Upstream can only reach the disabled
//     map from a hand-written `preFunc` lambda, so its every conditional control carries a lambda
//     whose body is the same three lines. preFunc survives here for the cases that genuinely need
//     code (updating a valuePointer, computing a note), but the common "grey this out while X"
//     case is now one line of data.
//  5. `note` added: the greyed suffix drawn on the same line after a control ("(restart)",
//     "(applies on restart)", "(disabled in-race)"). gdx_menu.cpp drew these with an explicit
//     SameLine + TextDisabled at the call site because UIWidgets has no slot for one; making it a
//     field is what lets those call sites become data without losing the marker.
//  6. `searchTerms` added, and SidebarEntry gained one too. This is where the old page-keyword
//     table (gdx_menu.cpp's `static const SearchPage pages[]`) went: its terms are preserved as
//     per-page search terms so nothing that used to be findable stopped being findable, while
//     widget-level matching is the new capability on top.
//  7. `modifiedMarker` added — the "changed from the default" asterisk that gdx_menu.cpp's
//     GdxCVarCheckboxMarked() helper drew after ~15 checkboxes. Folding it into the registry keeps
//     the marker and its tooltip while removing the helper.
//  8. WIDGET_WINDOW_BUTTON dropped. Upstream's implementation calls UIWidgets::WindowButton, which
//     toggles a GuiWindow through its visibility CVar — and gdx_menu.cpp:156-161 documents at
//     length that a bare CVarSetInteger is a NO-OP for an already-constructed window in this fork
//     (the window reads mIsVisible each frame and only samples the CVar at construction). The
//     port's tool pages therefore stay WIDGET_CUSTOM over the existing DrawToolWindowPage(), which
//     uses ToggleVisibility() and additionally embeds the window's DrawElement() inline.
//  9. WIDGET_AUDIO_BACKEND / WIDGET_VIDEO_BACKEND dropped: both call
//     Ship::Context::GetRawInstance(), which does not exist in this fork (UIWidgets ADAPTATION #3),
//     and this port picks its audio backend through its own gEnhancements.Audio.Backend CVar.
// 10. WIDGET_CVAR_RADIO_BUTTON added (upstream has no radio type at all); the Audio engine
//     selector is a two-button radio group over one CVar.
// 11. `raceDisable` dropped: it keys off Lighthouse's CVAR_SETTING("DisableChanges") race-lockout,
//     which has no equivalent here. The one in-race lockout this port has (ghost import) is
//     expressed as a normal DisableOption instead.
// 12. MenuInit / RegisterMenuInitFunc / RegisterMenuUpdateFunc dropped: they exist so Lighthouse's
//     many separate menu TUs can self-register at static-init time. This port registers from one
//     place (port/gdx_menu_registry.cpp), called explicitly from GdxMenu::InitElement(), which is
//     both simpler and free of static-initialisation-order questions.
// 13. The audioBackendsMap / windowBackendsMap tables dropped with the widget types that used them.
//
// LICENSE: CC0 1.0 Universal. Original: https://github.com/HarbourMasters/Lighthouse

#ifndef GDX_MENU_TYPES_H
#define GDX_MENU_TYPES_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "UIWidgets.hpp"

namespace GdxUI {

// ─────────────────────────────────────────────────────────────────────────────────────────────
// Named disable reasons.
//
// A greyed-out control that does not say WHY is a support burden: the user sees a setting they
// cannot move and has no way to discover what unlocks it. Each entry below is a reason a control
// can be unavailable; GdxMenu owns a map from reason -> DisabledInfo, whose `evaluation` lambda
// runs EXACTLY ONCE PER FRAME (that is the entire point of the struct — see DisabledInfo below),
// and MenuDrawItem turns the active reasons for a widget into a "This setting is disabled
// because:" tooltip that can list several at once.
//
// Some of these are used as HIDE conditions rather than disable conditions (WidgetInfo::hideWhen);
// the enum is shared so their evaluations are also amortised to once per frame.
// ─────────────────────────────────────────────────────────────────────────────────────────────
// Deliberately kept to reasons that are ACTUALLY WIRED to a control. An unused entry here is dead
// scaffolding that reads like a feature: it makes the enum look richer than the menu behaves.
enum DisableOption {
    // Renderer / window state
    DISABLE_FOR_NO_WINDOW, // the Ship::Window is not up yet (fullscreen has nothing to toggle)

    // Visual-enhancement dependencies
    DISABLE_FOR_WIDESCREEN_OFF,        // the 2D widescreen layout needs 16:9 rendering on
    DISABLE_FOR_WIDESCREEN_UI_OFF,     // split-screen HUD anchoring is a subset of the 2D layout
    DISABLE_FOR_INTERPOLATION_ON,      // interpolation owns pacing while it is on
    DISABLE_FOR_INTERPOLATION_OFF,     // (hide condition) interpolation's own sub-controls
    DISABLE_FOR_INTERP_OVERLAY_OFF,    // (hide condition) the sub-frame stat line
    DISABLE_FOR_MATCH_REFRESH_RATE_ON, // the fixed target FPS is unused while the target follows the display

    // Audio
    DISABLE_FOR_LOW_PASS_FILTER_OFF, // the cutoff slider is inert while the filter is disabled

    // Game state
    DISABLE_FOR_RACE_IN_PROGRESS, // mutating ghost state must not race the game fiber

    DISABLE_OPTION_COUNT
};

struct WidgetInfo;
struct DisabledInfo;

using WidgetFunc = std::function<void(WidgetInfo&)>;
using DisableEvalFunc = std::function<bool(DisabledInfo&)>;
using DisableVec = std::vector<DisableOption>;

// `DisabledInfo` holds a reason's human-readable text and the evaluation that decides whether it
// currently applies. GdxMenu::DrawElement() runs every evaluation once at the top of the frame and
// caches the answer in `active`; MenuDrawItem then only reads the cached bool. Without this, a
// condition shared by several widgets (e.g. "is interpolation on?") would re-read its CVar once per
// widget per frame — which is exactly the redundancy the struct exists to prevent.
// `value` is scratch space for evaluations that want to carry a number alongside the bool (e.g. a
// count) without a second lookup at draw time.
struct DisabledInfo {
    DisableEvalFunc evaluation;
    const char* reason = "";
    bool active = false;
    int32_t value = 0;
};

// How a registered entry is drawn. The CVAR_* variants read and write a CVar themselves (via the
// UIWidgets CVar* wrappers); the plain variants operate on WidgetInfo::valuePointer and leave the
// write to the widget's `callback`, which is what every non-CVar control in this menu needs (live
// window state, a derived boolean, a remembered-value pair, ...).
enum WidgetType {
    WIDGET_SEPARATOR,      // ImGui::Separator()
    WIDGET_SEPARATOR_TEXT, // ImGui::SeparatorText(name)
    WIDGET_TEXT,           // ImGui::TextWrapped(name), optionally coloured via TextOptions::color
    WIDGET_TEXT_DISABLED,  // ImGui::TextDisabled(name) — the unwrapped greyed note used throughout
    WIDGET_COMING_SOON,    // "<name>  -  Coming soon" (the old GdxComingSoon() helper, as data)
    WIDGET_CHECKBOX,
    WIDGET_CVAR_CHECKBOX,
    WIDGET_COMBOBOX,
    WIDGET_CVAR_COMBOBOX,
    WIDGET_SLIDER_INT,
    WIDGET_CVAR_SLIDER_INT,
    WIDGET_SLIDER_FLOAT,
    WIDGET_CVAR_SLIDER_FLOAT,
    WIDGET_BUTTON,
    WIDGET_CVAR_RADIO_BUTTON,
    WIDGET_CUSTOM, // customFunction draws whatever it likes (tables, status blocks, popups)
};

enum SectionColumns {
    SECTION_COLUMN_1,
    SECTION_COLUMN_2,
    SECTION_COLUMN_3,
};

// Everything needed to DRAW and to SEARCH one control.
//
//  name            the visible label, and the primary search key
//  cVar            the CVar backing the value (CVAR_* widget types only)
//  type            selects the draw path in GdxMenu::MenuDrawItem
//  options         the matching UIWidgets Options struct; set through the Options() overloads
//  valuePointer    where a non-CVar widget reads/writes (preFunc typically refreshes it first)
//  comboItems      ordered dropdown rows (see ADAPTATION #3)
//  radioValue      the value a WIDGET_CVAR_RADIO_BUTTON writes when picked
//  callback        run after the widget reports a change — this is where side effects live
//  preFunc         run before drawing: refresh valuePointer, set isHidden, compute a note
//  postFunc        run after drawing: react to state the widget itself does not report
//  customFunction  the body of a WIDGET_CUSTOM
//  disableWhen     reasons that grey this control out (evaluated once per frame, see DisabledInfo)
//  hideWhen        reasons that remove it from the page entirely
//  activeDisables  scratch: the subset of disableWhen that is active this frame, used for the tooltip
//  note            greyed suffix drawn on the same line after the control
//  searchTerms     extra keywords the search box matches, beyond name + tooltip
//  modifiedMarker  draw the "changed from the stock default" asterisk after this control
//  isHidden        set by preFunc (or hideWhen) to skip this control this frame
//  sameLine        draw on the same line as the previous control
//  hideInSearch    never appear in search results (for controls that only make sense in context)
struct WidgetInfo {
    std::string name;
    const char* cVar = "";
    WidgetType type = WIDGET_TEXT;
    std::shared_ptr<UIWidgets::WidgetOptions> options;
    std::variant<bool*, int32_t*, float*> valuePointer = static_cast<bool*>(nullptr);
    std::vector<const char*> comboItems = {};
    int32_t radioValue = 0;
    WidgetFunc callback = nullptr;
    WidgetFunc preFunc = nullptr;
    WidgetFunc postFunc = nullptr;
    WidgetFunc customFunction = nullptr;
    DisableVec disableWhen = {};
    DisableVec hideWhen = {};
    DisableVec activeDisables = {};
    const char* note = "";
    std::string searchTerms = "";
    bool modifiedMarker = false;
    bool isHidden = false;
    bool sameLine = false;
    bool hideInSearch = false;

    // Type-safe Options() overloads (ADAPTATION #2). Overload resolution picks the exact-match
    // derived overload over the WidgetOptions base one, so `.Options(UIWidgets::CheckboxOptions{})`
    // stores a CheckboxOptions and MenuDrawItem's static_pointer_cast is sound by construction.
    WidgetInfo& Options(const UIWidgets::CheckboxOptions& options_) {
        options = std::make_shared<UIWidgets::CheckboxOptions>(options_);
        return *this;
    }
    WidgetInfo& Options(const UIWidgets::ComboboxOptions& options_) {
        options = std::make_shared<UIWidgets::ComboboxOptions>(options_);
        return *this;
    }
    WidgetInfo& Options(const UIWidgets::IntSliderOptions& options_) {
        options = std::make_shared<UIWidgets::IntSliderOptions>(options_);
        return *this;
    }
    WidgetInfo& Options(const UIWidgets::FloatSliderOptions& options_) {
        options = std::make_shared<UIWidgets::FloatSliderOptions>(options_);
        return *this;
    }
    WidgetInfo& Options(const UIWidgets::ButtonOptions& options_) {
        options = std::make_shared<UIWidgets::ButtonOptions>(options_);
        return *this;
    }
    WidgetInfo& Options(const UIWidgets::RadioButtonsOptions& options_) {
        options = std::make_shared<UIWidgets::RadioButtonsOptions>(options_);
        return *this;
    }
    WidgetInfo& Options(const UIWidgets::TextOptions& options_) {
        options = std::make_shared<UIWidgets::TextOptions>(options_);
        return *this;
    }
    WidgetInfo& Options(const UIWidgets::WidgetOptions& options_) {
        options = std::make_shared<UIWidgets::WidgetOptions>(options_);
        return *this;
    }

    WidgetInfo& CVar(const char* cVar_) {
        cVar = cVar_;
        return *this;
    }
    WidgetInfo& ComboItems(std::vector<const char*> comboItems_) {
        comboItems = std::move(comboItems_);
        return *this;
    }
    WidgetInfo& RadioValue(int32_t radioValue_) {
        radioValue = radioValue_;
        return *this;
    }
    WidgetInfo& Callback(WidgetFunc callback_) {
        callback = std::move(callback_);
        return *this;
    }
    WidgetInfo& PreFunc(WidgetFunc preFunc_) {
        preFunc = std::move(preFunc_);
        return *this;
    }
    WidgetInfo& PostFunc(WidgetFunc postFunc_) {
        postFunc = std::move(postFunc_);
        return *this;
    }
    WidgetInfo& CustomFunction(WidgetFunc customFunction_) {
        customFunction = std::move(customFunction_);
        return *this;
    }
    WidgetInfo& ValuePointer(std::variant<bool*, int32_t*, float*> valuePointer_) {
        valuePointer = valuePointer_;
        return *this;
    }
    WidgetInfo& DisableWhen(DisableVec disableWhen_) {
        disableWhen = std::move(disableWhen_);
        return *this;
    }
    WidgetInfo& HideWhen(DisableVec hideWhen_) {
        hideWhen = std::move(hideWhen_);
        return *this;
    }
    WidgetInfo& Note(const char* note_) {
        note = note_;
        return *this;
    }
    WidgetInfo& SearchTerms(std::string searchTerms_) {
        searchTerms = std::move(searchTerms_);
        return *this;
    }
    WidgetInfo& ModifiedMarker(bool modifiedMarker_ = true) {
        modifiedMarker = modifiedMarker_;
        return *this;
    }
    WidgetInfo& SameLine(bool sameLine_ = true) {
        sameLine = sameLine_;
        return *this;
    }
    WidgetInfo& HideInSearch(bool hideInSearch_ = true) {
        hideInSearch = hideInSearch_;
        return *this;
    }

    // Per-frame scratch reset. `options` is shared and persists across frames, so a disable applied
    // last frame would otherwise stick after its reason cleared.
    void ResetDisables() {
        isHidden = false;
        if (options != nullptr) {
            options->disabled = false;
            options->disabledTooltip = "";
        }
        activeDisables.clear();
    }
};

// One sidebar page. `columnCount` (1..3) is how many columns the page is drawn in; `columnWidgets`
// holds the controls grouped by column. The two do not have to agree: a page may declare 2 columns
// but only fill the first, which is how a dense page is split without reordering its registration.
// `searchTerms` carries the page-level keywords the old `SearchPage pages[]` table held, so a query
// like "netplay" still surfaces the page even when no individual control mentions the word.
struct SidebarEntry {
    uint32_t columnCount = 1;
    std::vector<std::vector<WidgetInfo>> columnWidgets = {};
    std::string searchTerms = "";
};

// One header tab. `sidebarCvar` persists which sidebar page was last viewed inside this tab, BY
// NAME — upstream does the same, and it is strictly better than the integer index gdx_menu.cpp used
// to store, which silently pointed at a different page whenever the page order changed (hence the
// kMenuLayoutVersion reset that used to be needed).
struct MainMenuEntry {
    std::string label;
    const char* sidebarCvar = "";
    std::unordered_map<std::string, SidebarEntry> sidebars = {};
    std::vector<std::string> sidebarOrder = {};
};

} // namespace GdxUI

#endif /* GDX_MENU_TYPES_H */
