// ============================================================================
// flow_tests.cpp — see flow_tests.h.
//
// WHAT IS PINNED HERE.
//
//   1. THE ORDER OF PLAY, and everything derived from it: the index of a LEVELn
//      number, what follows a level, what comes next, the save prompt's number.
//      MEASURED from the C# classes.
//   2. WHAT A NEW GAME AND THE LAST LEVEL LEAVE IN GameState. MEASURED.
//   3. THE TRANSITION TABLE: which event shows which screen, and the state each
//      one leaves behind. TRANSLITERATED from Game.cs's switch.
//   4. THE STATISTICS' TEST COUNTS, including the crossing the C#'s destructure
//      performs. TRANSLITERATED.
//   5. THE STEP the frame runs on. TRANSLITERATED, with exact bits.
//
// The measured rows came from the read-only C# tree through the scratch probe's
// `flow` mode (the command and its raw output are in docs/PROGRESS_LOG.md); the
// scratch copy's Flow/*.cs were byte-identical to the repository's, checked with
// cmp, before any of them ran.
// ============================================================================

#include "flow_tests.h"

#include <stdio.h>
#include <string.h>

#include "game/game_flow.h"
#include "game/game_state.h"
#include "game/level_flow.h"

using namespace mdk;

namespace {

int failures = 0;

void expect(const char* what, long long got, long long want) {
    if (got != want) {
        printf("  FAIL: %s was %lld, the oracle has %lld\n", what, got, want);
        failures++;
    }
}

unsigned bitsOf(float value) {
    unsigned bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void expectBits(const char* what, float got, unsigned want) {
    if (bitsOf(got) != want) {
        printf("  FAIL: %s was %08X, the oracle has %08X\n", what, bitsOf(got), want);
        failures++;
    }
}

// ---------------------------------------------------------------------------
// 1. The order of play and what is derived from it. Every row is the C#'s own
//    output: INDEX / AFTER / NEXT / SAVENAME lines of the probe's `flow` mode.
// ---------------------------------------------------------------------------
void testOrderOfPlay() {
    const int32_t order[6] = {7, 6, 3, 4, 8, 5};
    for (int i = 0; i < 6; i++) {
        char label[48];
        snprintf(label, sizeof(label), "ORDER[%d]", i);
        expect(label, flow::kOrder[i], order[i]);
    }

    struct Row {
        int32_t level;
        int index;
    };
    // A level number outside the order is -1, not a clamp: every caller has its
    // own "not in the order" branch and none of them is a clamp.
    static const Row kIndex[] = {
        {-2, -1}, {-1, -1}, {0, -1}, {1, -1}, {2, -1}, {3, 2},  {4, 3},
        {5, 5},  {6, 1},    {7, 0},  {8, 4},  {9, -1}, {10, -1}, {11, -1},
    };
    for (const Row& row : kIndex) {
        char label[48];
        snprintf(label, sizeof(label), "IndexOf(%d)", row.level);
        expect(label, flow::indexOf(row.level), row.index);
    }

    struct AfterRow {
        int32_t level;
        flow::After overNo;
        flow::After overYes;
    };
    // LEVEL7-LEVEL4 (index 0-3) go to the statistics, LEVEL8 (index 4) goes to
    // the last level, LEVEL5 (index 5, the last of the order) has nothing after
    // it and goes to the menu, and a flattened town goes to the menu from any of
    // them. A level outside the order goes to the menu whatever happened.
    static const AfterRow kAfter[] = {
        {7, flow::After::Statistics, flow::After::Menu},
        {6, flow::After::Statistics, flow::After::Menu},
        {3, flow::After::Statistics, flow::After::Menu},
        {4, flow::After::Statistics, flow::After::Menu},
        {8, flow::After::LastLevel, flow::After::Menu},
        {5, flow::After::Menu, flow::After::Menu},
        {0, flow::After::Menu, flow::After::Menu},
        {1, flow::After::Menu, flow::After::Menu},
        {2, flow::After::Menu, flow::After::Menu},
        {9, flow::After::Menu, flow::After::Menu},
        {10, flow::After::Menu, flow::After::Menu},
        {100, flow::After::Menu, flow::After::Menu},
        {-3, flow::After::Menu, flow::After::Menu},
    };
    for (const AfterRow& row : kAfter) {
        char label[48];
        snprintf(label, sizeof(label), "AfterLevel(%d, No)", row.level);
        expect(label, static_cast<long long>(flow::afterLevel(row.level, flow::GameOver::No)),
               static_cast<long long>(row.overNo));
        snprintf(label, sizeof(label), "AfterLevel(%d, Yes)", row.level);
        expect(label, static_cast<long long>(flow::afterLevel(row.level, flow::GameOver::Yes)),
               static_cast<long long>(row.overYes));
    }

    struct NextRow {
        int32_t level;
        int32_t next;
    };
    // The last of the order stays, and a level outside it restarts at the order's
    // first entry (the C# clamps IndexOf+1 = 0 rather than reporting anything).
    static const NextRow kNext[] = {
        {-2, 7}, {-1, 7}, {0, 7}, {1, 7}, {2, 7}, {3, 4}, {4, 8},
        {5, 5},  {6, 3},   {7, 6}, {8, 5}, {9, 7}, {10, 7}, {11, 7},
    };
    for (const NextRow& row : kNext) {
        char label[48];
        snprintf(label, sizeof(label), "Next(%d)", row.level);
        expect(label, flow::next(row.level), row.next);
    }

    static const Row kSaveName[] = {
        {-2, 0}, {-1, 0}, {0, 0}, {1, 0}, {2, 0}, {3, 3}, {4, 4},
        {5, 6},  {6, 2},   {7, 1}, {8, 5}, {9, 0}, {10, 0}, {11, 0},
    };
    for (const Row& row : kSaveName) {
        char label[48];
        snprintf(label, sizeof(label), "SaveName(%d)", row.level);
        expect(label, flow::saveNameNumber(row.level), row.index);
    }
}

// ---------------------------------------------------------------------------
// 2. What NewGame and EnterLastLevel leave in GameState. The NEWGAME and
//    LASTLEVEL lines of the probe's `flow` mode.
// ---------------------------------------------------------------------------
void testGameStateRows() {
    struct NewRow {
        int32_t index;
        int32_t level;
    };
    static const NewRow kNewGame[] = {{0, 7}, {1, 6}, {2, 3}, {3, 4}, {4, 8}, {5, 5}};
    for (const NewRow& row : kNewGame) {
        flow::GameState state;
        expect("NewGame returns true inside the order", state.newGame(row.index) ? 1 : 0, 1);
        char label[48];
        snprintf(label, sizeof(label), "NewGame(%d) level", row.index);
        expect(label, state.level, row.level);
        expect("NewGame deaths", state.deaths, 0);
        expect("NewGame strike", state.strikeUsed ? 1 : 0, 0);
        // The splash, the debug keys and the fall's carry are what a fresh state
        // already had: NewGame touches only the three fields above.
        expect("NewGame splash", state.splash ? 1 : 0, 1);
        expect("NewGame debug keys", state.debugKeys ? 1 : 0, 0);
        expect("NewGame carry", state.hasCarry ? 1 : 0, 0);
        expect("NewGame notice", state.notice.empty() ? 1 : 0, 1);
        expect("NewGame start arena", state.startArena.empty() ? 1 : 0, 1);
    }

    // An index outside the order is where the C# throws; here it changes nothing.
    flow::GameState untouched;
    expect("NewGame(-1) refused", untouched.newGame(-1) ? 1 : 0, 0);
    expect("NewGame(6) refused", untouched.newGame(6) ? 1 : 0, 0);
    expect("NewGame out of range left the level", untouched.level, 7);

    struct LastRow {
        int32_t level;
        int32_t next;
    };
    static const LastRow kLastLevel[] = {
        {7, 6}, {6, 3}, {3, 4}, {4, 8}, {8, 5}, {5, 5}, {0, 7},
        {1, 7}, {2, 7}, {9, 7}, {10, 7}, {100, 7}, {-3, 7},
    };
    for (const LastRow& row : kLastLevel) {
        flow::GameState state;
        state.level = row.level;
        state.deaths = 2;
        state.strikeUsed = true;
        state.enterLastLevel(63);
        char label[48];
        snprintf(label, sizeof(label), "EnterLastLevel(%d) level", row.level);
        expect(label, state.level, row.next);
        expect("EnterLastLevel carry health", state.hasCarry ? state.carry.health : -1, 63);
        expect("EnterLastLevel carry pickups", static_cast<long long>(state.carry.pickups.size()), 0);
        // Deaths and the air strike survive: LEVEL5 is the same game, not a new one.
        expect("EnterLastLevel deaths", state.deaths, 2);
        expect("EnterLastLevel strike", state.strikeUsed ? 1 : 0, 1);
    }
}

// ---------------------------------------------------------------------------
// 3. The transition table. TRANSLITERATED from Game.cs's Handle switch; each
//    group cites the case it came from.
// ---------------------------------------------------------------------------
void testStartEvents() {
    struct StartRow {
        flow::Start start;
        flow::Event first;
    };
    // Game.FirstEvent's switch (Game.cs:349-362). Menu is the fall-through, and
    // it is the only one that touches state.Splash.
    static const StartRow kFirst[] = {
        {flow::Start::Menu, flow::Event::Menu},
        {flow::Start::Level, flow::Event::Play},
        {flow::Start::Statistics, flow::Event::Statistics},
        {flow::Start::Briefing, flow::Event::Briefing},
        {flow::Start::EndMovie, flow::Event::GameFinished},
        {flow::Start::Stream, flow::Event::Stream},
        {flow::Start::Fall, flow::Event::Fall},
    };
    for (const StartRow& row : kFirst) {
        flow::FlowOptions options;
        options.start = row.start;
        options.level = 7;
        options.splash = false;
        flow::GameFlow game(options);
        char label[48];
        snprintf(label, sizeof(label), "FirstEvent(start=%d)", static_cast<int>(row.start));
        expect(label, static_cast<long long>(game.firstEvent()), static_cast<long long>(row.first));
        expect("FirstEvent sets the level", game.state().level, 7);
        // Only the menu start copies the command line's splash onto the state; the
        // others leave the state's own default, which is true. --splash=false on a
        // --menu start therefore clears it, and on any other start does nothing.
        snprintf(label, sizeof(label), "FirstEvent(start=%d) splash", static_cast<int>(row.start));
        expect(label, game.state().splash ? 1 : 0, row.start == flow::Start::Menu ? 0 : 1);
    }
}

flow::FlowOptions levelOptions(int32_t level) {
    flow::FlowOptions options;
    options.start = flow::Start::Level;
    options.level = level;
    return options;
}

void testHandleTable() {
    // The plain cases: one event, one screen, nothing else (Game.cs:392-424).
    struct Case {
        flow::Event event;
        flow::Screen screen;
    };
    static const Case kPlain[] = {
        {flow::Event::Quit, flow::Screen::None},
        {flow::Event::Menu, flow::Screen::Menu},
        {flow::Event::Briefing, flow::Screen::Briefing},
        {flow::Event::Statistics, flow::Screen::Statistics},
        {flow::Event::Fall, flow::Screen::Fall},
    };
    for (const Case& c : kPlain) {
        flow::GameFlow game(levelOptions(7));
        char label[48];
        snprintf(label, sizeof(label), "Handle(event=%d)", static_cast<int>(c.event));
        expect(label, static_cast<long long>(game.handle(c.event)), static_cast<long long>(c.screen));
    }

    // Event::None leaves the screen alone (the switch has no case for it).
    flow::GameFlow idle(levelOptions(7));
    idle.handle(flow::Event::Menu);
    expect("Handle(None) keeps the screen",
           static_cast<long long>(idle.handle(flow::Event::None)),
           static_cast<long long>(flow::Screen::Menu));

    // Play shows no screen twice (the loading screen) and then the level, and it
    // is the only case that clears the first-level flag before it ends.
    flow::GameFlow played(levelOptions(7));
    expect("Play shows the level", static_cast<long long>(played.handle(flow::Event::Play)),
           static_cast<long long>(flow::Screen::Level));
    expect("Play clears the first-level flag", played.firstLevel() ? 1 : 0, 0);

    // GameFinished shows the end movies and THEN sets the splash (the C# writes
    // them in that order; nothing observes the difference yet, so it is kept).
    flow::GameFlow ended(levelOptions(7));
    ended.state().splash = false;
    expect("GameFinished shows the end movies",
           static_cast<long long>(ended.handle(flow::Event::GameFinished)),
           static_cast<long long>(flow::Screen::EndMovie));
    expect("GameFinished wants the splash next", ended.state().splash ? 1 : 0, 1);

    // KurtDied: the splash and the menu. The LASTGAME write the C# does first is
    // deferred with the save subsystem (game_flow.h).
    flow::GameFlow died(levelOptions(7));
    died.state().splash = false;
    expect("KurtDied shows the menu", static_cast<long long>(died.handle(flow::Event::KurtDied)),
           static_cast<long long>(flow::Screen::Menu));
    expect("KurtDied wants the splash next", died.state().splash ? 1 : 0, 1);
    expect("KurtDied is not the first level any more", died.firstLevel() ? 1 : 0, 0);

    // A stream started from the command line takes --stream's health, and 100
    // when there was none (Game.cs:415, SaveGame.FullHealth).
    flow::GameFlow bare(levelOptions(7));
    expect("Stream health defaults to full",
           static_cast<long long>(bare.handle(flow::Event::Stream)),
           static_cast<long long>(flow::Screen::Stream));
    expect("Stream default health", bare.streamHealth(), 100);

    flow::FlowOptions hurt = levelOptions(7);
    hurt.hasHealth = true;
    hurt.health = 40;
    flow::GameFlow wounded(hurt);
    wounded.handle(flow::Event::Stream);
    expect("Stream keeps --stream's health", wounded.streamHealth(), 40);
}

void testLevelEndPaths() {
    // LEVEL7 ends with the tornado: statistics first, so the stream, with Kurt's
    // health off the viewer.
    flow::GameFlow game(levelOptions(7));
    game.firstEvent();
    game.handle(flow::Event::Play);
    flow::LevelResult result;
    result.health = 40;
    result.stats.shots = 11;
    result.stats.shotHits = 5;
    game.reportLevel(result);
    expect("LEVEL7 ends to the stream", static_cast<long long>(game.handle(flow::Event::LevelEnded)),
           static_cast<long long>(flow::Screen::Stream));
    expect("the stream takes Kurt's health", game.streamHealth(), 40);
    expect("the level's shots are kept", game.state().stats.shots, 11);
    expect("the level's hits are kept", game.state().stats.shotHits, 5);
    expect("the stream is over: the statistics",
           static_cast<long long>(game.handle(flow::Event::StreamEnded)),
           static_cast<long long>(flow::Screen::Statistics));

    // A flattened town goes to the menu from the same level, and the counts are
    // kept just the same (AfterLevel's first test is the game-over one).
    flow::GameFlow bad(levelOptions(7));
    bad.firstEvent();
    bad.handle(flow::Event::Play);
    bad.reportLevel(result);
    expect("LEVEL7 flattened goes to the menu",
           static_cast<long long>(bad.handle(flow::Event::GameOver)),
           static_cast<long long>(flow::Screen::Menu));
    expect("a flattened level still keeps its counts", bad.state().stats.shots, 11);

    // LEVEL8 is index 4: the Gunter stream, then the save prompt, then LEVEL5 at
    // the stream's health and with no pickups. This is the whole of the C#'s
    // AfterStream LastLevel arm, end to end.
    flow::GameFlow gunter(levelOptions(8));
    gunter.firstEvent();
    gunter.handle(flow::Event::Play);
    gunter.reportLevel(result);
    expect("LEVEL8 ends to the stream", static_cast<long long>(gunter.handle(flow::Event::LevelEnded)),
           static_cast<long long>(flow::Screen::Stream));
    expect("the Gunter stream takes Kurt's health", gunter.streamHealth(), 40);
    expect("the stream is over: the save prompt",
           static_cast<long long>(gunter.handle(flow::Event::StreamEnded)),
           static_cast<long long>(flow::Screen::SavePrompt));
    expect("LEVEL5 follows LEVEL8", gunter.state().level, 5);
    expect("LEVEL5 keeps the stream's health", gunter.state().carry.health, 40);
    expect("LEVEL5 gets no pickups", static_cast<long long>(gunter.state().carry.pickups.size()), 0);

    // LEVEL5 is the last of the order: nothing follows it, so the level ends at
    // the menu and the stream never opens.
    flow::GameFlow last(levelOptions(5));
    last.firstEvent();
    last.handle(flow::Event::Play);
    last.reportLevel(result);
    expect("LEVEL5 ends at the menu", static_cast<long long>(last.handle(flow::Event::LevelEnded)),
           static_cast<long long>(flow::Screen::Menu));

    // A stream that never followed a level has no viewer to read a health off, so
    // it gets the full one (the C#'s `_viewer?.Health ?? FullHealth`).
    flow::GameFlow blind(levelOptions(7));
    blind.firstEvent();
    blind.handle(flow::Event::Play);
    expect("a level with no result still streams",
           static_cast<long long>(blind.handle(flow::Event::LevelEnded)),
           static_cast<long long>(flow::Screen::Stream));
    expect("and its health is full", blind.streamHealth(), 100);
    expect("and its counts are untouched", blind.state().stats.shots, 0);

    // A stream that was never shown cannot lend a health to the last level; the
    // C# reads `_screen as StreamScreen` and gets null.
    flow::GameFlow orphan(levelOptions(8));
    orphan.state().level = 8;
    expect("a stream over with no stream", static_cast<long long>(orphan.handle(flow::Event::StreamEnded)),
           static_cast<long long>(flow::Screen::SavePrompt));
    expect("and LEVEL5 starts whole", orphan.state().carry.health, 100);
}

// ---------------------------------------------------------------------------
// 4. The statistics' test counts (Game.ApplyTestCounts, Game.cs:396-411).
//    TRANSLITERATED. The crossing is the trap: the C# destructures
//    (Shots, ShotHits, SniperShots, SniperHits) then (Kills, Enemies, HeadShots)
//    from counts[0..6], and GameStats' fields are NOT in that order.
// ---------------------------------------------------------------------------
void testTestCounts() {
    const int32_t seven[7] = {11, 22, 33, 44, 55, 66, 77};
    flow::FlowOptions options;
    options.setCounts(seven, 7);
    flow::GameFlow game(options);
    game.firstEvent();
    const scripts::GameStats& stats = game.state().stats;
    expect("counts[0] is shots", stats.shots, 11);
    expect("counts[1] is shot hits", stats.shotHits, 22);
    expect("counts[2] is sniper rounds", stats.sniperShots, 33);
    expect("counts[3] is sniper hits", stats.sniperHits, 44);
    expect("counts[4] is kills", stats.kills, 55);
    expect("counts[5] is enemies", stats.enemies, 66);
    expect("counts[6] is head shots", stats.headShots, 77);

    // Fewer than seven are padded with zeros, and the padding still WRITES all
    // seven fields: three counts clear the last four rather than leaving them.
    const int32_t three[3] = {9, 8, 7};
    flow::FlowOptions few;
    few.setCounts(three, 3);
    flow::GameFlow fewGame(few);
    fewGame.state().stats.kills = 999;
    fewGame.firstEvent();
    expect("three counts still set shots", fewGame.state().stats.shots, 9);
    expect("three counts still set sniper rounds", fewGame.state().stats.sniperShots, 7);
    expect("three counts zero the kills", fewGame.state().stats.kills, 0);
    expect("three counts zero the head shots", fewGame.state().stats.headShots, 0);

    // No counts at all leaves the state's counts alone (the C# tests Count != 0).
    flow::FlowOptions none;
    flow::GameFlow kept(none);
    kept.state().stats.shots = 5;
    kept.firstEvent();
    expect("no counts leaves shots alone", kept.state().stats.shots, 5);

    // --towns is bits 31-29 of the town flags, on a System.Int32, which wraps.
    flow::FlowOptions towns;
    towns.hasTowns = true;
    towns.towns = 3;
    flow::GameFlow townGame(towns);
    townGame.firstEvent();
    expect("towns 3 shifts to bit 30", townGame.state().stats.townFlags, 1610612736);

    flow::FlowOptions wraps;
    wraps.hasTowns = true;
    wraps.towns = 7;
    flow::GameFlow wrapGame(wraps);
    wrapGame.firstEvent();
    // 7 << 29 is 3758096384, which is -536870912 as a signed 32-bit int. The C#
    // wraps; a signed left shift that overflows is undefined in C++, so the port
    // shifts unsigned and reinterprets back to the same bits.
    expect("towns 7 wraps like the C#'s int", wrapGame.state().stats.townFlags, -536870912);
}

// ---------------------------------------------------------------------------
// 5. The step the frame runs on (Game.Run's `step`, Game.cs:246-249, and its
//    `elapsed`, Game.cs:255). TRANSLITERATED, with exact bits: the C# writes
//    Viewer.Step as 1f/60f and the soak frame as Viewer.Step * 6, and neither is
//    the decimal a reader would write down.
// ---------------------------------------------------------------------------
void testStepSelection() {
    expectBits("Viewer.Step", flow::kViewerStep, 0x3C888889);       // 1f/60f
    expectBits("the soak step", flow::GameFlow::stepFor(true, false, 0.0f), 0x3DCCCCCE);
    expectBits("the default step", flow::GameFlow::stepFor(false, false, 0.0f), 0x3C888889);
    expectBits("30 fps", flow::GameFlow::stepFor(false, true, 30.0f), 0x3D088889);
    expectBits("144 fps", flow::GameFlow::stepFor(false, true, 144.0f), 0x3BE38E39);
    expectBits("1 fps", flow::GameFlow::stepFor(false, true, 1.0f), 0x3F800000);
    // The soak test wins over an explicit frame rate, exactly as the C#'s ternary
    // order says: a soak run at 240 fps still steps six game steps per frame.
    expectBits("soak beats the frame rate", flow::GameFlow::stepFor(true, true, 240.0f), 0x3DCCCCCE);

    // A test run's frames ARE game steps, so it never reads the clock.
    expectBits("a test frame steps the fixed step", flow::GameFlow::elapsedFor(true, 0.25f, 99.0f), 0x3E800000);
    expectBits("a live frame uses the clock", flow::GameFlow::elapsedFor(false, 0.25f, 99.0f), 0x42C60000);
}

} // namespace

namespace mdk {
namespace test {

int runFlowTests() {
    failures = 0;
    testOrderOfPlay();
    testGameStateRows();
    testStartEvents();
    testHandleTable();
    testLevelEndPaths();
    testTestCounts();
    testStepSelection();
    return failures;
}

} // namespace test
} // namespace mdk
