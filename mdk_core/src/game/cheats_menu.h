#pragma once

// ============================================================================
// cheats_menu.h -- the 3DS pause menu's CHEATS page (gap package #21).
//
// WHY A PAGE OF TOGGLES. The C#'s cheats are typed (TOOSCARYFORME in a level,
// game/cheats.h) or the console's (god, noclip: DevTools/LevelCommands.cs).
// The 3DS has neither a keyboard nor a console, so its pause menu (the START
// menu, game/enhanced_menu.h) has a CHEATS row that opens this page: one toggle
// per cheat, each doing exactly what the C# cheat does --
//
//   GORE       C# TOOSCARYFORME: the settings' gore flipped for the rest of the
//              game, NOT saved, and the running level's script Option with it
//              (Viewer.cs:874). The frontend applies it.
//   GOD MODE   C# console `god`: Kurt::Mortality God (no damage, no fall-out
//              death), held for the session and handed to every level's Kurt
//              (DevSession.God, Viewer.cs:287).
//   NOCLIP     C# console `noclip`: Player::Clipping Off (Kurt flies through
//              everything; jump rises, the `down` action sinks), held the same way.
//
// SEETHEWHOLEGAME's debug keys are left out: they are typed keys of the main
// menu, which this target cannot type.
//
// The page owns no state but its selection: the frontend keeps the switches
// (they outlive a level) and acts on Changed. Cold code: the menu runs only
// while the game is paused. No virtuals, no templates, no exceptions.
// ============================================================================

#include <stdint.h>

#include "game/enhanced_menu.h"
#include "render/render.h"

namespace mdk {

class FontView;

class CheatsMenu {
public:
    // The cheats' state, the frontend's.
    struct Switches {
        bool gore = true;
        bool god = false;
        bool noclip = false;
    };

    enum class Action : uint8_t {
        None,
        Changed,  // a cheat flipped (the caller applies it)
        Closed,   // BACK, or a close edge: the START menu shows again
    };

    // The rows: the three cheats, then BACK.
    static const int32_t kGoreRow = 0;
    static const int32_t kGodRow = 1;
    static const int32_t kNoclipRow = 2;
    static const int32_t kBackRow = 3;
    static const int32_t kRowCount = 4;

    bool open() const { return open_; }
    // Opening starts on the first cheat.
    void setOpen(bool open) {
        open_ = open;
        if (open) {
            selected_ = kGoreRow;
        }
    }
    int32_t selected() const { return selected_; }

    // One frame's edges (the START menu's own struct): up and down wrap, A or
    // left/right flip the selected cheat, A on BACK or a close edge closes.
    Action handle(const EnhancedMenu::Input& input, Switches* switches);

    static const char* rowLabel(int32_t row);

    // The panel, in the START menu's look (enhanced_menu.cpp's draw).
    void draw(MdkRenderer renderer, FontView& big, FontView& small, float canvasWidth,
              const Switches& switches) const;

private:
    bool open_ = false;
    int32_t selected_ = kGoreRow;
};

} // namespace mdk
