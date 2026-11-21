#pragma once

// ============================================================================
// cheats.h -- the cheats typed in a level (C# src/Mdk.Game/Flow/Cheats.cs, the
// original's 0x42c5f0, main.gd's _type), and Viewer.TypeCheats' filter
// (Viewer.cs:862-882), gap package #21.
//
//   TOOSCARYFORME     gore on or off for the rest of the game: the settings'
//                     gore, NOT saved (the C#'s doc comment), and the running
//                     level's script Option with it (Viewer.cs:874)
//   SEETHEWHOLEGAME   the main menu's debug keys on or off (GameState's
//                     debugKeys, 0x5742bc): 3-8 start that level, F the fall,
//                     S the stream, D the statistics (main_menu.h, DebugKey)
//
// God mode and noclip are NOT typed cheats in the C#: they are the console's
// (DevTools/LevelCommands.cs SetGod/SetNoclip, held for the session by
// DevSession and handed to every level's Kurt, Viewer.cs:286-289). Their
// effect is Kurt's (Kurt.Cheats.cs; game/player.h, setMortality/setClipping);
// how a frontend turns them on is the frontend's (Linux `--god`/`--noclip`,
// the 3DS pause menu's CHEATS page).
//
// WHY A WINDOW AND NOT A STATE MACHINE. The C# keeps the last letters typed,
// as long as the longest cheat (15), and asks whether they END with a cheat.
// An overlapping prefix ("TOOSCARYFORTOOSCARYFORME") therefore still completes,
// and a completed cheat empties the window. That is what is ported, letter for
// letter: `_typed = (_typed + letter)[Math.Max(0, _typed.Length + 1 - 15)..]`.
//
// Cold code: a few letters a frame at most. No virtuals, no templates, no
// exceptions (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include <string>

namespace mdk {

struct GameSettings;

namespace flow {

struct GameState;

class Cheats {
public:
    // C# Cheats.Cheat.
    enum Cheat { None, Gore, DebugKeys };

    // C# Cheats(Settings settings, GameState state): the two objects the cheats
    // flip. Both outlive this object (the frontend's run-scoped settings and the
    // flow's state).
    Cheats(GameSettings* settings, GameState* state) : settings_(settings), state_(state) {}

    // C# Cheats.Type: one more letter (A-Z); the cheat it completes, applied.
    Cheat type(char letter);

    // The window (C# _typed), for the goldens.
    const std::string& typed() const { return typed_; }

    // C# Cheat's names ("None", "Gore", "DebugKeys"), as the probe prints them.
    static const char* name(Cheat cheat);

private:
    GameSettings* settings_;
    GameState* state_;
    std::string typed_;
};

// C# Viewer.TypeCheats' filter: `typed.ToUpperInvariant().Where(char.IsAsciiLetterUpper)`.
// The letters of `typed` that reach Cheats::type, upper-cased, in order. ASCII
// only: the one place the C#'s invariant upper-casing reaches past ASCII is a
// handful of letters (U+0131 dotless i, U+017F long s) a keyboard's text event
// does not produce for these words; their UTF-8 bytes are dropped here.
std::string cheatLetters(const std::string& typed);

} // namespace flow
} // namespace mdk
