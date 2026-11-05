#pragma once

// ============================================================================
// game_flow.h — the screens one after the other, as the original's game states
// (C# src/Mdk.Game/Flow/Game.cs, the flow half of it).
//
//   splash -> menu -new game-> briefing --fall--> loading -> level -tornado->
//   stream -> statistics            ^ died: LASTGAME          briefing --fall-->
//   next level                      | event 81: end movies -> splash -> menu
//
// This is the state machine, not the screens. The C# `Game` both decides the next
// screen and builds it; here the decision is the whole of the class and the
// frontend builds what it was told to build, because the screens are the desktop
// UI (menu, statistics, briefing, stream, fall, end movies) and the frontends own
// the window, the renderer and the audio device. `handle` therefore returns a
// Screen and applies the state changes; it never builds anything.
//
// WHAT `Show` DOES, AND WHO DOES IT. The C# Show() disposes the old screen, stops
// every voice, releases the renderer's scope and then creates the new screen. The
// first three belong to whoever owns the audio device and the renderer — the
// frontend — so `show` records the new screen and the frontend does those three
// when the returned Screen differs from the previous one. That is the same split
// the renderer seam already uses (render/render.h): the decision here, the
// resources there.
//
// THE RECURSION IS THE ORACLE'S. Handle(KurtDied) calls Handle(Menu), and
// AfterLevel and AfterStream call Handle too, so `_firstLevel` is still true
// inside those inner calls and the menu they build gets the command line's page.
// A flattened loop that cleared firstLevel_ before dispatching is a different
// game, and flow_tests.cpp pins the inner call's view of it.
//
// NOT PORTED, WITH CAUSE — each is the save/menu subsystem the owner directive of
// 2026-11-05 cut ("boot straight into a level"), not a stub:
//   * FirstEvent's --load branch and Handle(KurtDied)'s LASTGAME write: both take
//     a SaveGame, which is Flow/SaveGames.cs (see game_state.h).
//   * Play's LoadingScreen.Show and the screens' own construction: Menu/ is cut.
//   * ShowStream's WatcomRandom seed (the C#'s Environment.TickCount for a stream
//     started from the command line): the stream screen is cut, so there is no
//     random to seed yet. kStreamDefaultSeed is the C#'s DefaultSeed, ready for it.
//   * GameOptions' window, renderer, mods, dev tools, soak keys, perf log and
//     screenshot plumbing: desktop-only, and each belongs to a frontend.
//
// THE TICK ORDER is Game.Run's loop body, and it is a contract the frontend
// implements in this order (see the numbered list by stepFor):
//   1. elapsed = a test run's fixed step, else the wall seconds since last frame
//   2. the soak keys and the --press keys, if any
//   3. the dev console gets the input and the frame limit
//   4. screen->frame(elapsed * timeScale)   <- the one simulation step
//   5. a console command's Event overrides the screen's
//   6. the audio device updates with the SAME elapsed
//   7. the display change is logged, then the frame is limited
//   8. an Event other than None is handled, which is where the screen changes
// Updating gameplay once per step and drawing once per eye is AGENTS.md P2; the
// step is stepFor() below, and it is NOT the frame rate.
//
// Plain enum-and-class code: no virtuals, no templates, no exceptions, no
// std::function for the "what to show next" answer (AGENTS.md §10) — that is why
// this returns an enum where the C# passes a Func<IScreen?>.
// ============================================================================

#include <stdint.h>

#include "game/game_state.h"
#include "game/level_flow.h"
#include "scripts/game_stats.h"

namespace mdk {
namespace flow {

// What a screen asks the game to do next (C# Event, Flow/Screen.cs).
enum class Event {
    None,
    Quit,
    // The main menu (with the splash when state().splash).
    Menu,
    // Load and play state().level.
    Play,
    // The briefing of state().level, then the level.
    Briefing,
    // The screens after the level state().level (statistics, save prompt, next
    // briefing).
    Statistics,
    // The level ended: the tornado is over.
    LevelEnded,
    // The level ended badly (the town flattened).
    GameOver,
    KurtDied,
    // The end of the game (event 81): the end movies.
    GameFinished,
    // The fall before state().level, then the level.
    Fall,
    // The stream after the level state().level.
    Stream,
    // The stream is over: the statistics, or the last level after LEVEL8.
    StreamEnded,
};

// The screen the game shows next (C# Game.Handle, one case per screen). None is
// the C#'s null screen: the game is quitting, or is between screens inside Play.
enum class Screen {
    None,
    Menu,
    Briefing,
    Statistics,
    Level,
    Stream,
    Fall,
    EndMovie,
    // The save prompt alone (after LEVEL8), then the level.
    SavePrompt,
};

// How the game starts (command line): the screen shown first — C# Start.
enum class Start { Menu, Level, Statistics, Briefing, EndMovie, Stream, Fall };

// C# SaveGame.FullHealth (Flow/SaveGames.cs:14). Kurt's health when nothing has
// taken any: the light saves and a stream started without one both use it.
constexpr int32_t kFullHealth = 100;
// C# Stream.WatcomRandom.DefaultSeed, the seed of a stream started directly.
constexpr uint32_t kStreamDefaultSeed = 1;
// C# Game.TownShift: --towns gives bits 31-29 of the debriefing's town flags.
constexpr int kTownShift = 29;

// C# Viewer.Step (Viewer.cs:96) — the level's fixed simulation step in seconds.
constexpr float kViewerStep = 1.0f / 60.0f;
// The soak test's frames are this many game steps (0.1 s): it runs faster.
constexpr int kSoakSteps = 6;

// The command line's choices the flow reads. Everything else in the C#'s
// GameOptions is a window, a renderer, a mod, a dev tool or a screenshot, and
// belongs to a frontend.
struct FlowOptions {
    Start start = Start::Menu;
    // --level: the first level to play (a LEVELn number).
    int32_t level = kOrder[0];
    // The menu shows the splash first even when it is started directly.
    bool splash = false;
    // --counts: shots, shot hits, sniper rounds, sniper hits, kills, enemies,
    // heads. countCount is how many were given; 0 leaves the counts alone.
    void setCounts(const int32_t* values, int32_t count);
    const int32_t* counts() const { return counts_; }
    int32_t countCount() const { return countCount_; }
    // --towns: the town flags' bits 31-29 for the debriefing.
    bool hasTowns = false;
    int32_t towns = 0;
    // --stream: Kurt's health in a stream started directly.
    bool hasHealth = false;
    int32_t health = 0;

private:
    int32_t counts_[7] = {0, 0, 0, 0, 0, 0, 0};
    int32_t countCount_ = 0;
};

// The level's counts and Kurt's health, which the C# reads off its live `_viewer`
// when the level ends. A frontend that has a level passes them to
// GameFlow::reportLevel when the level ends.
struct LevelResult {
    scripts::GameStats stats;
    int32_t health = kFullHealth;
};

// The game's screen state machine (C# Game's FirstEvent/Handle/Play/AfterLevel/
// AfterStream/Show).
class GameFlow {
public:
    explicit GameFlow(const FlowOptions& options) : options_(options) {}

    GameState& state() { return state_; }
    const GameState& state() const { return state_; }
    const FlowOptions& options() const { return options_; }

    // The screen to show. None is the C#'s null screen.
    Screen screen() const { return screen_; }
    // Whether the level shown takes the command line's test options: the first
    // level does, later ones do not (C# _firstLevel).
    bool firstLevel() const { return firstLevel_; }
    // The health the stream was opened with (C# ShowStream's argument, and what
    // AfterStream reads back off the stream screen).
    int32_t streamHealth() const { return streamHealth_; }
    // Whether a level is in play, which is the C#'s `_viewer != null`: the C# sets
    // it inside Play, so it is true from the moment a level is shown and NOT only
    // once the level has reported. play() therefore clears the remembered result
    // when it shows a level, and a level that ends without reporting streams this
    // level's fresh counts and health rather than the last level's (review-5 F1).
    bool hasLevel() const { return hasLevel_; }

    // The first screen from the command line (C# FirstEvent). It applies the
    // statistics' test counts and sets state().level first, exactly as there.
    Event firstEvent();

    // What follows a screen's event (C# Handle), and the screen to show for it.
    // The C#'s recursion is kept: an event that shows another screen is handled by
    // a nested call, so the inner one still sees firstLevel() == true.
    Screen handle(Event next);

    // The level's live counts and health (C# _viewer.Stats and _viewer.Health).
    // Call it with the level's current values when the level ends, which is when
    // the C# reads them; it is what AfterLevel keeps for the statistics. It is
    // optional: play() already stands a fresh result in its place, exactly as the
    // C#'s new Viewer does — the fall's carry health when the level was entered
    // through a fall, full health otherwise (Viewer.cs:246 and :347-361) — so a
    // frontend that forgets it degrades to a level that took no damage and fired
    // nothing rather than to the previous level's numbers.
    void reportLevel(const LevelResult& result);

    // The step Game.Run picks (C# Game.cs:246-249): the soak test's frames are
    // kSoakSteps game steps, an explicit frame rate is 1/fps, and the default is
    // the level's own step. This is the SIMULATION step, not the frame rate.
    static float stepFor(bool soak, bool hasFrameRate, float frameRate);

    // The elapsed for one frame (C# Game.cs:255): a test run steps the fixed step
    // and never reads the clock, because its frames ARE game steps; a live run
    // uses the wall seconds since the last frame.
    static float elapsedFor(bool test, float step, float wallSeconds);

private:
    // C# Show: the screen changes; disposing the old one, stopping every voice and
    // releasing the renderer's scope are the frontend's (see the header).
    Screen show(Screen next);
    // C# Play: the loading screen, then the level.
    Screen play();
    // C# AfterLevel.
    Screen finishLevel(GameOver over);
    // C# ShowStream.
    Screen showStream(int32_t health);
    // C# AfterStream.
    Screen afterStream();
    // C# ApplyTestCounts.
    void applyTestCounts();

    FlowOptions options_;
    GameState state_;
    Screen screen_ = Screen::None;
    bool firstLevel_ = true;
    int32_t streamHealth_ = kFullHealth;
    bool hasLevel_ = false;
    LevelResult level_;
};

} // namespace flow
} // namespace mdk
