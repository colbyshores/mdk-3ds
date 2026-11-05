// game_state.cpp — see game_state.h.
//
// A transliteration of GameState.cs's two methods that do not take a SaveGame.
// The rows of what they leave behind are measured from the C# class itself and
// pinned in tests/scripts/flow_tests.cpp.

#include "game/game_state.h"

namespace mdk {
namespace flow {

bool GameState::newGame(int32_t index) {
    // C# Order[index]: an index outside the order throws IndexOutOfRange there.
    // This build has no exceptions (AGENTS.md §10), so the caller is told and
    // nothing is changed — the alternative is an out-of-bounds read on the 3DS.
    if (index < 0 || index >= kOrderCount) {
        return false;
    }
    level = kOrder[index];
    deaths = 0;
    strikeUsed = false;
    return true;
}

void GameState::enterLastLevel(int32_t health) {
    level = next(level);
    // A fresh carry with no pickups: 0x4325b0 clears the inventory.
    carry = FallCarry();
    carry.health = health;
    hasCarry = true;
}

} // namespace flow
} // namespace mdk
