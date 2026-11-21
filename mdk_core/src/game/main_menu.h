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
// THE SPLASH AND THE VIDEO (package #20c): with setMedia, the INTRO1A splash
// comes first when GameState::splash (game/intro_splash.h; any key skips it,
// and that key is the splash's), then MDK12.FLC plays behind the items each
// time the menu opens and its last frame stays (game/video_player.h). The
// frontend draws: the splash while splashFrame(), else MDKOPT when the player
// has no image (the C#'s fallback when the video is missing), the player's
// image, the items. Without setMedia (the goldens of #20a) neither plays.
//
// THE SLIDESHOW (package #20b's game/slideshow.h) starts when MDK12.FLC ends:
// the player's Finished is the C#'s `_video.Finished += _slideshow.Start`, the
// video's last frame is kept as image 0, and each image the slideshow moves to
// is shown on the player (the C#'s Slideshow.Next -> VideoPlayer.ShowStill).
// While the bare first slide shows, the items are hidden (ItemsShown).
//
// NOT PORTED, WITH CAUSE:
//   * MAINSONG and the menus' sounds (menu_items.h).
//   (The typed keys -- "Really Quit?"'s Y/J/O/S/T and N, and SEETHEWHOLEGAME's
//   debug keys 3-8, D, F, S -- are ported with the cheat, package #21: Keys::typed.
//   The 3DS has no keyboard, so there they are never typed.)
//
// Cold code. No virtuals, no templates, no exceptions (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "game/game_flow.h"
#include "game/intro_splash.h"
#include "game/menu_items.h"
#include "game/slideshow.h"
#include "game/video_player.h"

namespace mdk {

namespace formats {
class Bni;
class Fti;
class MdkData;
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

    // Package #20c: OPTIONS.BNI (the INTRO1A splash) and the game's data (where
    // the player reads MDK12.FLC), BEFORE open. Either may be null: no splash, no
    // video.
    void setMedia(const formats::Bni* options, const formats::MdkData* data);

    // C# Frame's input half: the splash while it shows (it takes the frame: no
    // keys reach the menu); else the video started and updated, the keys
    // (HandleKeys, then the items), the growth. Returns the event for the game
    // (C# _next), None while the menu stays.
    flow::Event frame(const Keys& keys, float elapsed);

    // Whether the last frame was the splash's (C# Frame's early return): the
    // frontend draws the splash then (nothing once it is done), not the menu.
    bool splashFrame() const { return splashFrame_; }
    IntroSplash& splash() { return splash_; }
    VideoPlayer& video() { return video_; }
    const VideoPlayer& video() const { return video_; }
    // The slideshow after the video (C# _slideshow), for the tests and the log.
    const Slideshow& slideshow() const { return slideshow_; }
    // C# Status: "intro splash", "main menu: slideshow" (the bare first slide),
    // "main menu: <page>, video", "main menu: <page>".
    std::string status() const;

    MenuItems& items() { return items_; }
    const MenuItems& items() const { return items_; }
    // "Main", "Quit" or "Other" (C# Page), for the goldens and the log.
    const char* pageName() const;
    int32_t levelIndex() const { return levelIndex_; }
    flow::Event next() const { return next_; }

    // C# MainMenu.DebugKey (MainMenu.cs:202-243), the debug keys SEETHEWHOLEGAME turns
    // on: in the upper-cased text, the first of 3-8 plays that level, F the fall before
    // Order[4], S the stream after Order[0], D the statistics with random counts
    // (Random.Shared.Next(100) each). True when one was taken: `state` and `next` are
    // set. Static so the golden reaches it without a Ui (the C# probe calls the private
    // method on a MainMenu made without its constructor).
    static bool debugKey(const std::string& typed, flow::GameState* state, flow::Event* next);

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
    // Package #20c: the splash and the video (C# _splash, _video, _videoStarted).
    const formats::Bni* options_ = nullptr;
    const formats::MdkData* data_ = nullptr;
    IntroSplash splash_;
    VideoPlayer video_;
    bool hasSplash_ = false;
    bool splashFrame_ = false;
    bool videoStarted_ = false;
    // The slideshow (C# _slideshow, Slideshow.cs): started by the video's Finished
    // (videoFinished), its image shown on the player (showSlide). `last*` is the
    // video's last frame when it started (C# Slideshow._last = video.Still).
    static void videoFinished(void* user);
    void showSlide();
    Slideshow slideshow_;
    bool hasLast_ = false;
    int32_t lastWidth_ = 0;
    int32_t lastHeight_ = 0;
    std::vector<uint8_t> lastIndices_;
    std::vector<uint8_t> lastPalette_;
};

} // namespace menu
} // namespace mdk
