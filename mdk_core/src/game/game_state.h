#pragma once

// ============================================================================
// game_state.h — what the game keeps between its screens
// (C# src/Mdk.Game/Flow/GameState.cs).
//
// The screens come and go; this is the bag that survives them: which level to
// play, how many times Kurt has died, whether the one air strike is spent, the
// last level's counts for the Score-o-matic, whether the menu shows its splash
// next, and the handful of one-shot notes the console and the fall hand on to
// the next level.
//
// THE ORDER OF PLAY IS NOT THE ORDER OF THE NUMBERS. `level` is a LEVELn number
// (3-8) and it starts at kOrder[0] == 7, not at 3. Anything that counts levels
// counts them in the order of play, so it goes through game/level_flow.h.
//
// Load(SaveGame) and Save(SaveKind) landed with the save subsystem (package #19,
// game/save_games.h), which the owner un-cut on 2026-11-20.
//
// A null C# string field is an empty std::string here, and a null FallCarry is
// `hasCarry == false`: the C# tests them with `is {}` / `?? `, which is exactly
// "empty means unset" for these.
//
// Plain data with two small methods: no virtuals, no templates, no exceptions
// (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "game/level_flow.h"
#include "game/save_games.h"
#include "scripts/game_stats.h"

namespace mdk {
namespace flow {

// Kurt's health and the pickups (model names, in order) he took in the fall —
// C# FallCarry.
struct FallCarry {
    int32_t health = 0;
    std::vector<std::string> pickups;
};

// C# GameState.
struct GameState {
    // LEVELn to play (3-8). The C# initialises it to Order[0].
    int32_t level = kOrder[0];
    // Deaths so far (0x574407).
    int32_t deaths = 0;
    // The one air strike of the last two levels was used (0x57440b).
    bool strikeUsed = false;
    // The last level's counts and town flags, for the statistics.
    scripts::GameStats stats;
    // The INTRO1A splash shows when the main menu opens next: at start, after
    // Kurt died and after the end movies.
    bool splash = true;
    // The cheat SEETHEWHOLEGAME (0x5742bc): keys 3-8 in the main menu start that
    // level, D the statistics with random counts.
    bool debugKeys = false;
    // What the fall hands on to the next level: Kurt's health and the pickups he
    // took. No carry until a fall has run.
    bool hasCarry = false;
    FallCarry carry;
    // The level's state of a full save being loaded, until the level restores it.
    std::string snapshot;
    // This session's last full save (its name), quick load's first choice.
    std::string quickSlot;
    // A HUD message the next level shows when it starts ("Game loaded"), once.
    std::string notice;
    // The console's map: the arena the next level starts in (on a floor of it),
    // once.
    std::string startArena;

    // A new game from the level at `index` of the order (C# NewGame). It does NOT
    // clear the counts: the statistics of the game before it are still on screen.
    // Returns false, having changed nothing, for an index outside the order, where
    // the C# throws IndexOutOfRange (this build has no exceptions, AGENTS.md §10).
    bool newGame(int32_t index);

    // The state of a save: its level, deaths and air strike, and a full save's
    // level state, kept until the level restores it (C# Load).
    void load(const SaveGame& save);

    // A light save of the level to play (health 100, as the original's), made now
    // (C# Save: `DateTime.Now.ToString("s")`, the local time).
    SaveGame save(SaveKind kind) const;
    // The same with the time given, for the tests.
    SaveGame saveAt(SaveKind kind, const std::string& time) const;

    // LEVEL5 after the Gunter stream: Kurt keeps the stream's health, not the
    // pickups (0x4325b0 clears the inventory) — C# EnterLastLevel.
    void enterLastLevel(int32_t health);
};

} // namespace flow
} // namespace mdk
