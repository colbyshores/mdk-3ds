// game_flow.cpp — see game_flow.h.
//
// A transliteration of Game.cs's flow half. Every branch below is one line of the
// C# switch, in the C#'s order, with the C#'s recursion; the transition table and
// the state changes it leaves behind are pinned in tests/scripts/flow_tests.cpp.

#include "game/game_flow.h"

#include <stdio.h>

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

    // C# --load (Game.cs:372-382): read the save, state().Load(save), and start at
    // its briefing or the level itself; a save that is not there quits.
    if (!options_.load.empty()) {
        const Event loaded = loadSave(options_.load);
        if (loaded == Event::None) {
            say("No saved game " + options_.load);
            return Event::Quit;
        }
        return loaded;
    }

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
            // again (not the 1996 demo's, which this port does not load: every
            // level here is retail, BetaDemo.IsBeta is false).
            {
                std::string why;
                if (!saves_.write(SaveGames::kLastGame, state_.save(SaveKind::LevelStart), &why)) {
                    say("LASTGAME not written: " + why);
                }
            }
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

Event GameFlow::skipScreen() {
    switch (screen_) {
        case Screen::Stream:
            // C# Flight.Stop then Update: the fade ends before Kurt moves, so the
            // health the stream was opened with is the one it ends with
            // (Flight.cs:161, :220-228; StreamScreen.cs:114).
            return streamHealth_ < kStreamLowHealth ? Event::Menu : Event::StreamEnded;
        case Screen::Statistics: {
            // C# StatsScreen: _index = Math.Max(IndexOf(level), 0) (:135); after
            // the Score page, EndPhase (:263-264).
            const int index = indexOf(state_.level) < 0 ? 0 : indexOf(state_.level);
            const int nextIndex = index + 1 < kOrderCount - 1 ? index + 1 : kOrderCount - 1;
            state_.level = kOrder[nextIndex];
            return Event::Fall;
        }
        case Screen::Briefing:
            return Event::Fall;
        case Screen::Fall:
            // C# FallSim.Skip -> Landed; FallScreen.Next: Carry = (Health, Collected).
            state_.carry = FallCarry();
            state_.carry.health = kFallStartHealth;
            state_.hasCarry = true;
            return Event::Play;
        case Screen::SavePrompt:
            return Event::Play;
        case Screen::EndMovie:
            return Event::Menu;
        case Screen::None:
        case Screen::Menu:
        case Screen::Level:
            break;
    }
    return Event::None;
}

Screen GameFlow::skipUnported(SkipTrail* trail) {
    if (trail) {
        trail->count = 0;
    }
    for (int32_t i = 0; i < kMaxSkips; i++) {
        const Screen skipped = screen_;
        // The Briefing is shown since package #20a; skipScreen keeps its arm.
        if (skipped == Screen::Briefing) {
            break;
        }
        // The Fall is shown since package #17b (fall/fall_screen.h), and the
        // Statistics since #20b (game/stats_screen.h); skipScreen keeps both arms
        // (a frontend's --skip-falls and --skip-statistics).
        if (skipped == Screen::Fall || skipped == Screen::Statistics) {
            break;
        }
        const Event next = skipScreen();
        if (next == Event::None) {
            break;
        }
        if (trail) {
            trail->skipped[trail->count++] = skipped;
        }
        handle(next);
    }
    return screen_;
}

const char* GameFlow::screenName(Screen screen) {
    switch (screen) {
        case Screen::None: return "None";
        case Screen::Menu: return "Menu";
        case Screen::Briefing: return "Briefing";
        case Screen::Statistics: return "Statistics";
        case Screen::Level: return "Level";
        case Screen::Stream: return "Stream";
        case Screen::Fall: return "Fall";
        case Screen::EndMovie: return "EndMovie";
        case Screen::SavePrompt: return "SavePrompt";
    }
    return "?";
}

int32_t GameFlow::screenPackage(Screen screen) {
    switch (screen) {
        case Screen::Stream: return 18;
        case Screen::Fall: return 17;
        case Screen::SavePrompt: return 19;
        case Screen::EndMovie: return 20;
        case Screen::Menu:
        case Screen::Briefing:
        case Screen::Statistics:
        case Screen::None:
        case Screen::Level: break;
    }
    return 0;
}

Event GameFlow::loadSave(const std::string& name) {
    // C# MainMenu.Load (and FirstEvent's): the save's state, then its briefing (a
    // before-level save) or the level.
    SaveGame save;
    if (!saves_.read(name, &save)) {
        return Event::None;
    }
    state_.load(save);
    char line[96];
    snprintf(line, sizeof(line), "Loaded game %s: level %d", name.c_str(), (int)save.level);
    say(line);
    return save.isKind(SaveKind::BeforeLevel) ? Event::Briefing : Event::Play;
}

void GameFlow::say(const std::string& line) {
    report_ += report_.empty() ? line : "\n" + line;
}

std::string GameFlow::takeReport() {
    std::string report;
    report.swap(report_);
    return report;
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
    // and health (review-5 F1).
    //
    // A fresh viewer's health is NOT always full, and that is review-7 F1. The
    // Viewer's constructor calls CarryFromFall(state) (Viewer.cs:246, :347-361),
    // which does `_kurt.SetHealth(carry.Health)` and then `state.Carry = null`, so
    // a level entered through the fall starts at the CARRY's health and the carry
    // is consumed by the level that takes it. The live case is LEVEL5 after the
    // Gunter stream (AfterStream -> EnterLastLevel, Game.cs:466 with
    // GameState.cs:60-64 storing the stream's health); a level entered through the
    // Fall screen would be the other. With no carry the fresh result stays at
    // kFullHealth, which is what a viewer that has taken no damage reports.
    //
    // The carry's PICKUPS go to the level's Kurt too since package #17b: the
    // carry is kept whole as levelCarry() for the frontend's
    // fall::carryIntoLevel (Viewer.CarryFromFall: the pickups, then the health).
    // C# Play's --save (Game.cs:488-491): the first level's light save, made as
    // it starts.
    if (firstLevel_ && !options_.save.empty() &&
        saves_.write(options_.save, state_.save(SaveKind::LevelStart))) {
        char line[96];
        snprintf(line, sizeof(line), "Saved game %s: level %d", options_.save.c_str(),
                 (int)state_.level);
        say(line);
    }
    level_ = LevelResult();
    levelCarry_ = FallCarry();
    levelCarry_.health = kFullHealth;
    if (state_.hasCarry) {
        level_.health = state_.carry.health;
        levelCarry_ = state_.carry;
        state_.carry = FallCarry();
        state_.hasCarry = false;
    }
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
    // gets Environment.TickCount (Game.cs:515-516). The random itself is the
    // frontend's StreamScreen's, seeded through streamSeed().
    streamDirect_ = firstLevel_ && options_.start == Start::Stream;
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
