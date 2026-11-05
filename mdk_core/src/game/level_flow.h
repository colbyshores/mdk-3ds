#pragma once

// ============================================================================
// level_flow.h — which level follows which, and what follows a level
// (C# src/Mdk.Game/Flow/LevelFlow.cs and GameState.Order/IndexOf).
//
// MDK plays its six levels in a fixed order that is NOT the order of their
// numbers: the level index (0x574268, 0-5) picks the TRAVERSE/LEVELn directory,
// and that index runs 7, 6, 3, 4, 8, 5 — so LEVEL7 is first and Gunter's
// LEVEL5 is last. Everything that asks "what's next" goes through that index,
// which is why a level number outside the order is not clamped into it: the C#
// returns -1 and every caller takes its own "not in the order" branch.
//
// WHAT FOLLOWS A LEVEL is the original's own state list (main loop 0x401cb8):
//
//   index 0-3: tornado -> stream -> statistics, save prompt, next briefing
//                -> [fall] -> next level
//   index 4:   tornado -> Gunter stream -> save prompt -> LEVEL5
//                (no statistics, no fall)
//   index 5:   event 81 -> end movies -> menu
//   game over (the town flattened) or a level outside the order -> menu
//
// LEVEL5 is the last entry, so index 5 has no level after it: AfterLevel
// answers Menu for it, and Next keeps it where it is (the C#'s clamp to the
// last index). Both of those are the oracle's answers, not a fallback, and
// tests/scripts/flow_tests.cpp pins them alongside every other row.
//
// GameOver is the C#'s ScriptRuntime.GameOver (ScriptRuntime.cs:250), named
// here because the C++ ScriptRuntime has no LevelEnded event yet; when that
// lands it names this enum rather than declaring its own.
//
// Header-only constants plus a small .cpp: no virtuals, no templates, no
// exceptions (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

namespace mdk {
namespace flow {

// The order the levels are played in (0x490030) — C# GameState.Order.
extern const int32_t kOrder[6];
constexpr int kOrderCount = 6;

// How a level ended: the tornado is over, or the town was flattened.
enum class GameOver { No, Yes };

// What follows a level (C# LevelFlow.After).
enum class After { Menu, Statistics, LastLevel };

// The index (0-5) of a LEVELn number in the order of play, -1 if none.
int indexOf(int32_t level);

// What follows the level (C# LevelFlow.AfterLevel).
After afterLevel(int32_t level, GameOver over);

// The level after `level` in the order of play (the last stays) — C#
// LevelFlow.Next. A level outside the order restarts the order at its first
// entry, because the C# clamps IndexOf+1 = 0 rather than reporting an error.
int32_t next(int32_t level);

// The number the save prompt offers for a level: its position in the order of
// play, 1-6 (C# LevelFlow.SaveName renders this as a decimal string; a level
// outside the order renders "0", which is IndexOf+1 with IndexOf == -1).
int32_t saveNameNumber(int32_t level);

} // namespace flow
} // namespace mdk
