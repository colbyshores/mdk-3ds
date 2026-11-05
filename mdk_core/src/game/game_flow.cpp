// game_flow.cpp — see game_flow.h.
//
// A transliteration of Game.cs's flow half. Every branch below is one line of the
// C# switch, in the C#'s order, with the C#'s recursion; the transition table and
// the state changes it leaves behind are pinned in tests/scripts/flow_tests.cpp.

#include "game/game_flow.h"

namespace mdk {
namespace flow {

void FlowOptions::setCounts(const int32_t* values, int32_t count) {
    // C# GameOptions.Counts is a list of any length; ApplyTestCounts pads it with
    // seven zeros and reads only the first seven. The padding is what makes a
    // three-count --counts still clear the last four.
    countCount_ = count;
    const int32_t given = count > 7 ? 7 : count;
    for (int32_t i = 0; i < 7; i++) {
        counts_[i] = (values != nullptr && i < given) ? values[i] : 0;
    }
}

Event GameFlow::firstEvent() {
    applyTestCounts();

    // C# --load: read the save, state().Load(save), and start at its briefing or
    // the level itself. Deferred with the save subsystem (game_state.h).

    state_.level = options_.level;
    switch (options_.start) {
        case Start::Level:
            return Event::Play;
        case Start::Statistics:
            return Event::Statistics;
        case Start::Briefing:
            return Event::Briefing;
        case Start::EndMovie:
            return Event::GameFinished;
        case Start::Stream:
            return Event::Stream;
        case Start::Fall:
            return Event::Fall;
        case Start::Menu:
            break;
    }

    state_.splash = options_.splash;
    return Event::Menu;
}

Screen GameFlow::handle(Event next) {
    Screen shown = screen_;
    switch (next) {
        case Event::Quit:
            shown = show(Screen::None);
            break;
        case Event::Menu:
            shown = show(Screen::Menu);
            break;
        case Event::Play:
            shown = play();
            break;
        case Event::Briefing:
            shown = show(Screen::Briefing);
            break;
        case Event::Statistics:
            shown = show(Screen::Statistics);
            break;
        case Event::LevelEnded:
            shown = finishLevel(GameOver::No);
            break;
        case Event::GameOver:
            shown = finishLevel(GameOver::Yes);
            break;
        case Event::KurtDied:
            // The level is saved as LASTGAME and the menu's "Continue" starts it
            // again (not the 1996 demo's). The write takes a SaveGame, so it is
            // deferred with the save subsystem; the splash and the menu are not.
            state_.splash = true;
            shown = handle(Event::Menu);
            break;
        case Event::GameFinished:
            shown = show(Screen::EndMovie);
            state_.splash = true;
            break;
        case Event::Stream:
            shown = showStream(options_.hasHealth ? options_.health : kFullHealth);
            break;
        case Event::Fall:
            shown = show(Screen::Fall);
            break;
        case Event::StreamEnded:
            shown = afterStream();
            break;
        case Event::None:
            break;
    }

    // The first level gets the command line's test options; later ones don't. This
    // runs after the switch, so an event that recursed into Handle saw it still on.
    firstLevel_ = false;
    return shown;
}

void GameFlow::reportLevel(const LevelResult& result) {
    level_ = result;
    hasLevel_ = true;
}

Screen GameFlow::play() {
    show(Screen::None);
    // C# LoadingScreen.Show(_ui, _state.Level, 0f) and the Viewer's construction.
    // Menu/ is cut and the Viewer is not ported; the level's own test options
    // (--frames, --event, the sound mode) land with it.
    show(Screen::None);
    // C# `_viewer = new Viewer(...)` sits at this exact point, so from here the C#
    // reads a FRESH viewer's counts and health: the previous level's result must
    // not outlive the level that produced it. Without this, a level that ends
    // before its frontend calls reportLevel would stream the LAST level's shots
    // and health (review-5 F1). A fresh LevelResult is zero counts and
    // kFullHealth, which is what a viewer that has taken no damage yet reports;
    // when the Viewer is ported, its start health comes from the carry instead.
    level_ = LevelResult();
    hasLevel_ = true;
    return show(Screen::Level);
}

Screen GameFlow::finishLevel(GameOver over) {
    if (hasLevel_) {
        state_.stats = level_.stats;
    }

    if (afterLevel(state_.level, over) == After::Menu) {
        return handle(Event::Menu);
    }

    return showStream(hasLevel_ ? level_.health : kFullHealth);
}

Screen GameFlow::showStream(int32_t health) {
    // C# ShowStream: a stream started straight from the command line gets
    // WatcomRandom.DefaultSeed so tests are reproducible; one that follows a level
    // gets Environment.TickCount. The stream screen is cut, so there is no random
    // to seed yet — see kStreamDefaultSeed.
    streamHealth_ = health;
    return show(Screen::Stream);
}

Screen GameFlow::afterStream() {
    switch (afterLevel(state_.level, GameOver::No)) {
        case After::Statistics:
            return handle(Event::Statistics);
        case After::LastLevel: {
            // LEVEL5 follows at once, after a save prompt (kind 3, "6"), with the
            // stream's health. The C# reads it off the stream screen it is about
            // to replace, so the health is taken before EnterLastLevel and Show.
            const int32_t health = screen_ == Screen::Stream ? streamHealth_ : kFullHealth;
            state_.enterLastLevel(health);
            return show(Screen::SavePrompt);
        }
        case After::Menu:
            return handle(Event::Menu);
    }
    return handle(Event::Menu);
}

void GameFlow::applyTestCounts() {
    scripts::GameStats& stats = state_.stats;
    const int32_t* counts = options_.counts();
    if (options_.countCount() != 0) {
        // The C# destructures the padded array in this order, which is NOT the
        // field order of GameStats: 4/5/6 are kills, enemies and head shots.
        stats.shots = counts[0];
        stats.shotHits = counts[1];
        stats.sniperShots = counts[2];
        stats.sniperHits = counts[3];
        stats.kills = counts[4];
        stats.enemies = counts[5];
        stats.headShots = counts[6];
    }

    if (options_.hasTowns) {
        // C# `towns << 29` on a System.Int32, which wraps. A signed left shift that
        // overflows is undefined in C++, so the shift happens in the unsigned
        // domain and is reinterpreted back — the same bits, no UB.
        stats.townFlags = static_cast<int32_t>(static_cast<uint32_t>(options_.towns) << kTownShift);
    }
}

float GameFlow::stepFor(bool soak, bool hasFrameRate, float frameRate) {
    if (soak) {
        return kViewerStep * kSoakSteps;
    }
    return hasFrameRate ? 1.0f / frameRate : kViewerStep;
}

float GameFlow::elapsedFor(bool test, float step, float wallSeconds) {
    return test ? step : wallSeconds;
}

Screen GameFlow::show(Screen next) {
    screen_ = next;
    return screen_;
}

} // namespace flow
} // namespace mdk
