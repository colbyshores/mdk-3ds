#pragma once

// ============================================================================
// enhanced_menu.h -- the in-game PAUSE menu: Resume, the Video/Graphics
// Settings (SPEC.md 7.9: the Enhanced Mode toggles, flipped live during
// gameplay for side-by-side A/B comparison), and Quit.
//
// START opens it (owner, 2026-11-19: START used to quit the game outright on
// one press, and the effects menu on SELECT was undiscovered). SELECT opened the
// same menu until 2026-11-27, when it became the run toggle (the original 3DS
// has no ZL; platform/input_3ds.h). It opens on RESUME, so a stray A resumes rather than quits; QUIT is
// a row of its own, reached with the D-pad (up from RESUME wraps to it) and
// confirmed with A -- never a single press.
//
// The settings are the Enhanced Mode toggles, the look dials (sliders) and the
// shadow map's two choices; there are more rows than the top screen holds, so
// the panel shows a scrolling window of them.
//
// THE C# PAUSE MENU'S ROWS (package #20a, owner un-cut 2026-11-20). The C#
// PauseMenu (src/Mdk.Game/Menu/PauseMenu.cs) is Resume, Options, Main menu and
// Quit; Resume and Quit were already here, so OPTIONS and MAIN MENU sit just
// above QUIT (which stays the last row, one up from RESUME). OPTIONS opens the
// C#'s options pages (game/pause_menu.h's openOptions: volumes, gore,
// difficulty), MAIN MENU leaves the level for the main menu; both are A on the
// row, never a single press from where the menu opens.
//
// CHEATS (gap package #21) sits just above OPTIONS: A on it opens the cheats
// page (game/cheats_menu.h: gore, god mode, noclip), which this target has no
// keyboard or console to reach otherwise.
//
// This class is the menu's LOGIC and its canvas drawing. It owns no
// platform input and no file: the frontend translates its buttons into
// the edge struct below, calls handle(), and acts on its answer: a setting
// changed (apply with mdk_render_set_enhanced_toggles, persist with
// writeEnhancedConfig to the platform's config.ini), the menu closed, or
// QUIT chosen. The gameplay is paused by the frontend while the menu is
// open -- this class only ever sees one frame's edges at a time.
// ============================================================================

#include <stdint.h>

#include "render/render.h"
#include "render/render_types.h"

namespace mdk {

class FontView;
struct GameSettings;
struct LookDial;

class EnhancedMenu {
public:
    // One frame's button EDGES (pressed this frame, not held).
    struct Input {
        bool up = false;
        bool down = false;
        bool left = false;
        bool right = false;
        bool confirm = false;  // A: flip a toggle, cycle a choice, or take RESUME / QUIT
        bool close = false;    // START or B: leave the menu (resume)
    };

    // What one frame's edges did.
    enum class Action : uint8_t {
        None,
        Changed,   // a setting's value changed (the caller applies and persists)
        Closed,    // the menu closed (RESUME, or a close edge): play resumes
        Quit,      // QUIT confirmed with A: the caller ends the run
        Options,   // OPTIONS with A: the caller opens the options pages
        MainMenu,  // MAIN MENU with A: the caller leaves for the main menu
        Cheats,    // CHEATS with A: the caller opens the cheats page
    };

    // THE SETTING ROWS, t2k's menu kinds (t2k_core/src/ui/menu.cpp TOG / SLD /
    // CHO) bound straight to EnhancedToggles fields: a TOGGLE flips a bool; a
    // SLIDER moves an int by its step within [lo, hi] (the look dials, ranges
    // from enhanced_config.h kLookDials) and shows it as a percent; a CHOICE
    // cycles an int through a fixed list of values, each with its own label.
    // A SETTING steps one of the game's own float settings (GameSettings, the
    // stereo pair) through a fixed list like a choice -- `choices` in
    // hundredths -- but stops at both ends like a slider.
    enum class Kind : uint8_t { Toggle, Slider, Choice, Setting };
    struct Item {
        const char* label;
        Kind kind;
        bool EnhancedToggles::*flag;      // Toggle
        const LookDial* dial;             // Slider: its field, range and step
        int32_t EnhancedToggles::*value;  // Choice: its field,
        const int32_t* choices;           // ... the values, in order (Setting too)
        const char* const* choiceLabels;  // ... and what each is shown as
        int32_t choiceCount;
        float GameSettings::*setting;     // Setting: its field
    };

    // The rows: RESUME, the settings (MOVIES HALF RATE, package #20c, then the
    // 3DS draw culls: TINY DEBRIS, TINY ENEMIES, then SCOPE IN 2D, SCOPE CLIP and
    // ROUND CAMERAS, then 3D DEPTH and 3D SCREEN DISTANCE, then SWAP STICKS),
    // CHEATS (#21), OPTIONS, MAIN MENU, QUIT.
    static const int32_t kItemCount = 27;
    // SWAP STICKS (owner 2026-11-27): a New 3DS's only row. Without a C-Stick
    // (setCStick(false), the default) it is SHOWN DISABLED -- drawn greyed with
    // "NO C-STICK" and inert to every key -- rather than hidden, so the rows keep
    // one numbering on both models and an Old 3DS player can see why.
    static const int32_t kSwapSticksRow = 27;
    static const int32_t kResumeRow = 0;
    static const int32_t kCheatsRow = kItemCount + 1;
    static const int32_t kOptionsRow = kItemCount + 2;
    static const int32_t kMainMenuRow = kItemCount + 3;
    static const int32_t kQuitRow = kItemCount + 4;
    static const int32_t kRowCount = kItemCount + 5;
    // The rows the panel shows at once (the 240-line top screen holds twelve at
    // the small font's 1.5x); the window scrolls to keep the selection in it.
    static const int32_t kVisibleRows = 12;

    bool open() const { return open_; }
    // Opening always starts on RESUME.
    void setOpen(bool open) {
        open_ = open;
        if (open) {
            selected_ = kResumeRow;
            top_ = 0;
        }
    }
    int32_t selected() const { return selected_; }
    // Whether the console has a C-Stick (APT_CheckNew3DS): SWAP STICKS is usable
    // only then.
    void setCStick(bool present) { cstick_ = present; }
    bool cstick() const { return cstick_; }
    // The first row the panel shows.
    int32_t top() const { return top_; }

    // Advance the menu by one frame's edges over `toggles` and `settings` (the
    // SETTING rows' fields; null leaves those rows inert). Up and down wrap
    // (up from RESUME is QUIT). On a TOGGLE row left/right/A flip it; on a
    // SLIDER left/right step it (A does nothing); on a CHOICE left/right cycle
    // it backwards/forwards and A forwards; on a SETTING left/right step it and
    // stop at its ends (A does nothing). Changed only when a value really
    // moved: a slider at its end answers None, so nothing is written. A on
    // RESUME closes; A on QUIT is Quit and on MAIN MENU MainMenu (the menu
    // stays open: the caller is leaving); A on OPTIONS is Options and on CHEATS
    // Cheats (the menu stays open under the page).
    Action handle(const Input& input, EnhancedToggles* toggles,
                  GameSettings* settings = nullptr);

    // The label of a row ("RESUME", "HAZE", "OPTIONS", "MAIN MENU", "QUIT").
    static const char* rowLabel(int32_t row);
    // The setting a row edits, or null for RESUME / OPTIONS / MAIN MENU / QUIT /
    // out of range.
    static const Item* rowItem(int32_t row);
    // A row's value as the panel shows it ("ON", "125%", "AUTO", "FAR"); empty
    // for RESUME and QUIT, and for a SETTING row without `settings`. `out` holds
    // at least 16 bytes.
    static void rowValue(int32_t row, const EnhancedToggles& toggles, char* out, int32_t size,
                         const GameSettings* settings = nullptr);

    // The panel's footer line.
    static const char* const kHint;

    // Draw the panel and its rows on the canvas (between
    // mdk_render_canvas_begin/end): the title in the big font, the
    // rows and the hint in the small one (the oracle's Fonts pair).
    // `canvasWidth` is the canvas's current width; the panel centres
    // itself in it.
    void draw(MdkRenderer renderer, FontView& big, FontView& small, float canvasWidth,
              const EnhancedToggles& toggles, const GameSettings* settings = nullptr) const;

private:
    bool open_ = false;
    int32_t selected_ = kResumeRow;
    int32_t top_ = 0;
    bool cstick_ = false;
};

} // namespace mdk
