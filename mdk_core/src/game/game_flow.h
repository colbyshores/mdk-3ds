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
// THE MENU IS BACK (package #20a, the owner un-cut 2026-11-20): the frontends
// show Screen::Menu (game/main_menu.h) and Screen::Briefing (game/briefing.h).
//
// THE SAVES (package #19, un-cut by the owner 2026-11-20): FirstEvent's --load
// branch, Handle(KurtDied)'s LASTGAME write and Play's --save write go through the
// SaveGames the frontend hands over (setSaves); with none, the C#'s writes fail
// quietly as a full disk's would, and the menu has no "Continue".
//
// NOT PORTED, WITH CAUSE — each is the menu subsystem (package #20), not a stub:
//   * Play's LoadingScreen.Show and the screens' own construction: Menu/ is cut.
//   * ShowStream's WatcomRandom seed is the frontend's to make (streamSeed): the
//     screen itself is the frontends' (stream/stream_screen.h, package #18b).
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

#include <string>

#include "game/game_state.h"
#include "game/level_flow.h"
#include "game/save_games.h"
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
// C# Stream.Flight.LowHealth (Flight.cs:90): a stream that ends below it is
// Outcome.Died, and the StreamScreen goes to the menu.
constexpr int32_t kStreamLowHealth = 1;
// C# Kurt.Inventory.MaxHealth (Inventory.cs:23): the fall's starting health
// (FallSim.Health), which a skipped fall hands on.
constexpr int32_t kFallStartHealth = 100;
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
    // --load NAME: the save to start from (C# GameOptions.Load).
    std::string load;
    // --save NAME: a light save of the first level, made as it starts (C#
    // GameOptions.Save, the tests').
    std::string save;

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

    // The saved games (C# Game._saves, `SaveGames.In(userFolder)`). The frontend
    // sets them before firstEvent, and deletes LASTGAME first, as the C#'s Game
    // constructor does ("the original deletes LASTGAME.SAV when it quits").
    void setSaves(const SaveGames& saves) { saves_ = saves; }
    const SaveGames& saves() const { return saves_; }
    // What the flow's own save work did last, for the frontends' log: the --load
    // refusal ("No saved game NAME"), "Loaded game NAME: level N", "Saved game
    // NAME: level N", the LASTGAME write's failure. Empty when nothing happened.
    // Cleared by takeReport.
    std::string takeReport();

    // The screen to show. None is the C#'s null screen.
    Screen screen() const { return screen_; }
    // Whether the level shown takes the command line's test options: the first
    // level does, later ones do not (C# _firstLevel).
    bool firstLevel() const { return firstLevel_; }
    // The health the stream was opened with (C# ShowStream's argument, and what
    // AfterStream reads back off the stream screen).
    int32_t streamHealth() const { return streamHealth_; }
    // THE STREAM SCREEN'S HALF OF THE FLOW (package #18b). The C#'s ShowStream
    // seeds the stream's WatcomRandom with DefaultSeed when the stream was started
    // directly (`_firstLevel && _options.Start == Start.Stream`, Game.cs:515-516)
    // and with Environment.TickCount otherwise; streamSeed() is that choice, the
    // tick count being the frontend's clock (or, in a fixed-step harness run, a
    // constant, so the run is reproducible).
    bool streamDirect() const { return streamDirect_; }
    uint32_t streamSeed(uint32_t tickCount) const { return streamDirect_ ? kStreamDefaultSeed : tickCount; }
    // The health the stream ENDED with (C# AfterStream's
    // `(_screen as StreamScreen)?.Health`, Game.cs:531): the frontend that showed
    // the stream sets it from Flight::health() before it handles the stream's
    // Event, and the LEVEL5 entry after the Gunter stream takes it.
    void setStreamHealth(int32_t health) { streamHealth_ = health; }
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

    // WHAT A SCREEN THIS PORT DOES NOT HAVE ENDS WITH (package 2, the next-level
    // transition). The stream (#18), the fall (#17), the statistics, the briefing,
    // the save prompt and the end movies (#19/#20) are not ported, so a frontend
    // that is shown one of them takes it as SKIPPED: this returns the Event that
    // screen ends with when the player skips straight through it, and applies the
    // state that screen leaves behind, operation for operation with the C#'s own
    // skip paths:
    //   Stream     StreamScreen.Frame's Back -> Flight.Stop (Flight.cs:161): the
    //              fade-out ends on the next update with the health unchanged, so
    //              Event.StreamEnded (Event.Menu if that health is below
    //              Flight.LowHealth, the C#'s Outcome.Died; StreamScreen.cs:114).
    //   Statistics StatsScreen's pages Intermission, Debriefing, Score, Briefing
    //              (StatsScreen.cs:136-138); after the Score page EndPhase moves the
    //              level on, `Order[min(max(IndexOf(level), 0) + 1, 5)]`
    //              (:263-264); the save prompt closes (Esc; #20a draws it); the briefing ends
    //              in Event.Fall (NextPhase, :273-276).
    //   Briefing   StatsPages.Briefing's one page, then Event.Fall.
    //   Fall       FallScreen's Back -> FallSim.Skip (FallSim.cs:203-209): Landed at
    //              once with the sim's starting health (Inventory.MaxHealth, 100)
    //              and nothing collected, so the carry is (100, no pickups) and
    //              Event.Play (FallScreen.Next, :120-124).
    //   SavePrompt the prompt closed without a save (Esc): Event.Play
    //              (Game.cs:681). A frontend that draws the prompt
    //              (game/save_prompt.h) shows it instead of skipping it.
    //   EndMovie   skipped: Event.Menu (EndMovie.cs:58).
    // Menu, Level and None are not skipped through (Event::None).
    Event skipScreen();
    // The skip for the screen shown, handled: handle(skipScreen()) until a screen
    // that is not skipped through, at most kMaxSkips times. Since package #20a
    // the frontends SHOW the Menu and the Briefing (game/main_menu.h,
    // game/briefing.h), so the walk stops at those as at a Level or None;
    // skipScreen keeps the Briefing's arm, which is the C#'s skip of it (Esc,
    // then a key: Event.Fall). `trail`, when given, receives the screens skipped in order (up to
    // kMaxSkips of them), so the frontend can log each one.
    static constexpr int32_t kMaxSkips = 8;
    struct SkipTrail {
        Screen skipped[kMaxSkips];
        int32_t count = 0;
    };
    Screen skipUnported(SkipTrail* trail);
    // The screen's name and, for one this port does not have, the gap package
    // that ports it (docs/PORT_GAPS.md), for the frontends' "flow: skipped" lines:
    // 0 for the ones the frontends show (the Level; the Menu and the Briefing
    // since #20a).
    static const char* screenName(Screen screen);
    static int32_t screenPackage(Screen screen);

    // The health the level shown was ENTERED with, which is what a frontend seeds
    // its Kurt from. It is the fresh viewer's health: the fall's carry when the
    // level was entered through a fall, full health otherwise (Viewer.cs:246 and
    // :347-361). `play()` consumes the carry into it, so a frontend must read it
    // HERE and not `state().carry`, which play() has already cleared (review-8
    // F3). After reportLevel it is that report's health instead, so the only
    // moment it is the entry health is when the level is shown -- which is also
    // the only moment a frontend has a fresh Kurt to seed.
    int32_t levelHealth() const { return level_.health; }

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
    bool streamDirect_ = false;
    bool hasLevel_ = false;
    LevelResult level_;
    SaveGames saves_;
    std::string report_;
    // C# MainMenu.Load / FirstEvent's load: the state of the save, and the event
    // that starts it. Event::None when it does not read back.
    Event loadSave(const std::string& name);
    void say(const std::string& line);
};

} // namespace flow
} // namespace mdk
