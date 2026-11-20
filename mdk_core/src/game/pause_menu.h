#pragma once

// ============================================================================
// pause_menu.h -- the pause menu (C# src/Mdk.Game/Menu/PauseMenu.cs;
// pause_menu.gd): Resume, Options, back to the main menu, Quit, over the dimmed
// game. The game waits while it is open.
//
// THE LINUX FRONTEND shows it as the C# does: Esc opens it during the game.
//
// THE 3DS shows the owner's START menu instead (game/enhanced_menu.h: Resume,
// the effect toggles and dials, Options, Main menu, Quit -- the owner's tuning
// instrument, kept whole). The C# PauseMenu's items live there as rows, and its
// OPTIONS row opens THIS class's options pages alone (openOptions): the same
// pages the main menu shows, and Back returns to the START menu.
//
// NOT PORTED, WITH CAUSE: the quick save / quick load keys' hint at the bottom
// of the first page (KeysHint) -- quick save is package #19's.
//
// Cold code. No virtuals, no templates, no exceptions (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include "game/game_flow.h"
#include "game/menu_items.h"
#include "render/render.h"

namespace mdk {

class FontView;
struct ScreenView;

namespace menu {

class PauseMenu {
public:
    bool open() const { return open_; }

    // C# Show: the first page.
    void show();
    // The options pages alone (the 3DS's OPTIONS row): Back and Esc close.
    void openOptions();

    // C# Update's logic half: Esc resumes (on the options' pages it goes up a
    // page), then the items. Returns the game's next screen (C# _next: Menu or
    // Quit; None while the game goes on).
    flow::Event update(const Keys& keys, float delta);

    // C# Update's drawing half: the dim, then the items (between
    // mdk_render_canvas_begin and _end).
    void draw(MdkRenderer renderer, const ScreenView& view, const FontView& big,
              const FontView& small);

    MenuItems& items() { return items_; }
    const MenuItems& items() const { return items_; }
    flow::Event next() const { return next_; }

private:
    enum Action { ActResume = 1, ActOptions, ActMainMenu, ActQuit, ActOptionsBack };

    void showMain();
    void resume();
    void act(const Event& event);

    MenuItems items_;
    flow::Event next_ = flow::Event::None;
    bool open_ = false;
    bool optionsOnly_ = false;
};

} // namespace menu
} // namespace mdk
