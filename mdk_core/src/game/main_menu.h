#pragma once

// ============================================================================
// main_menu.h -- the main menu (C# src/Mdk.Game/Menu/MainMenu.cs;
// main_menu.gd). The game's entry screen on both frontends.
//
//   Continue      (the level Kurt last died in: LASTGAME)
//   New Game
//   Level: n      (where a new game starts, 1-6)
//   Saved Game    (the list of saves)
//   Beta Levels   (the 1996 demo's, when it's found)
//   Options
//   Quit          (Esc: "Really Quit?")
//
// This is the menu's logic and its drawing over MDKOPT (the C#'s background
// when the video is missing); the frontend owns the renderer, the input, the
// files and the flow, and acts on the flow::Event a frame returns: Briefing (a
// new game, a save made before a level), Play (Continue, a save, a beta level)
// or Quit.
//
// THE SAVES ARE PACKAGE #19's (docs/PORT_GAPS.md). The menu reaches them only
// through MenuSaves: with no hooks the folder reads as empty -- no Continue,
// "No saved games" on the Saved Game page -- which is exactly the C#'s menu on a
// fresh user folder, and #19 fills the hooks.
//
// NOT PORTED, WITH CAUSE:
//   * MDK12.FLC behind the menu, the slideshow after it and the INTRO1A splash:
//     VideoPlayer, Slideshow and IntroSplash are packages #20b/#20c. The C#'s
//     own fallback without the video is MDKOPT, which is what is drawn, and
//     without the slideshow the items always show (ItemsShown).
//   * MAINSONG and the menus' sounds (menu_items.h).
//   * The typed keys: "Really Quit?"'s Y/J/O/S/T and N, and SEETHEWHOLEGAME's
//     debug keys (3-8, D, F, S; the cheat is package #21). Yes and No are items
//     the D-pad and Enter reach.
//
// Cold code. No virtuals, no templates, no exceptions (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "game/game_flow.h"
#include "game/menu_items.h"

namespace mdk {

namespace formats {
class Fti;
}

namespace menu {

// The saved games, as the menu asks for them (C# SaveGames.List/Read and
// GameState.Load). Every hook may be null: the folder is then empty.
struct MenuSaves {
    // C# SaveGames.List(): the names, in its order.
    void (*list)(void* user, std::vector<std::string>* names) = nullptr;
    // C# SaveGames.Read(name) != null, and the save's level (LEVELn).
    bool (*peek)(void* user, const char* name, int32_t* level) = nullptr;
    // C# MainMenu.Load: read the save and GameState.Load it into `state`; false
    // when it is missing or invalid (the menu then stays). `beforeLevel` says
    // the save was made before a level (SaveKind.BeforeLevel: its briefing
    // first).
    bool (*load)(void* user, const char* name, flow::GameState* state, bool* beforeLevel) = nullptr;
    void* user = nullptr;
};

// C# MenuPage: the menu's first page (the command line's --options etc.).
enum class MenuPage { Main, Options };

class MainMenu {
public:
    // C# SaveGames.LastGame.
    static const char* const kLastGame;

    // The C# constructor: MDKFONT.FTI's texts (null: the fallbacks), the game
    // state (the level a new game starts from), the saves, whether the 1996 demo
    // was found, and the first page. The items' fonts, settings and print hook
    // are set on items() BEFORE open.
    void open(const formats::Fti* fti, flow::GameState* state, const MenuSaves& saves,
              bool hasBeta, MenuPage first);

    // C# Frame's input half: the keys (HandleKeys, then the items), the growth.
    // Returns the event for the game (C# _next), None while the menu stays.
    flow::Event frame(const Keys& keys, float elapsed);

    MenuItems& items() { return items_; }
    const MenuItems& items() const { return items_; }
    // "Main", "Quit" or "Other" (C# Page), for the goldens and the log.
    const char* pageName() const;
    int32_t levelIndex() const { return levelIndex_; }
    flow::Event next() const { return next_; }

private:
    enum class Page { Main, Quit, Other };
    // The owner's actions (below MenuItems::kOptionsBase).
    enum Action {
        ActContinue = 1, ActNewGame, ActNextLevel, ActSaves, ActBeta, ActOptions, ActQuit,
        ActQuitYes, ActQuitNo, ActBack, ActOptionsBack, ActSave = 100, ActBetaLevel = 200
    };

    void handleKeys(const Keys& keys);
    void act(const Event& event);
    void showMain();
    void showQuit();
    void showSaves();
    void showOptions();
    void showBetaLevels();
    void load(const std::string& name);
    void playBeta(int32_t level);
    void newGame();
    void nextLevel();
    bool hasLastGame() const;
    std::string text(const char* name, const char* fallback) const;

    MenuItems items_;
    const formats::Fti* fti_ = nullptr;
    flow::GameState* state_ = nullptr;
    MenuSaves saves_;
    bool hasBeta_ = false;
    int32_t levelIndex_ = 0;
    Page page_ = Page::Main;
    flow::Event next_ = flow::Event::None;
    // The Saved Game page's names, by action (ActSave + i).
    std::vector<std::string> saveNames_;
};

} // namespace menu
} // namespace mdk
