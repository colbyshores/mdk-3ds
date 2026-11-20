// game_state.cpp — see game_state.h.
//
// A transliteration of GameState.cs's two methods that do not take a SaveGame.
// The rows of what they leave behind are measured from the C# class itself and
// pinned in tests/scripts/flow_tests.cpp.

#include "game/game_state.h"

#include <time.h>

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

void GameState::load(const SaveGame& save) {
    // C# Load (GameState.cs:51-56).
    level = save.level;
    deaths = save.deaths;
    strikeUsed = save.strikeUsed;
    snapshot = save.isKind(SaveKind::Snapshot) && save.hasState ? save.state : std::string();
}

SaveGame GameState::saveAt(SaveKind kind, const std::string& time) const {
    // C# Save (GameState.cs:69-70): new(kind, Level, FullHealth, Deaths, StrikeUsed, time).
    SaveGame light;
    light.kind = (int32_t)kind;
    light.level = level;
    light.health = SaveGame::kFullHealth;
    light.deaths = deaths;
    light.strikeUsed = strikeUsed;
    light.time = time;
    return light;
}

SaveGame GameState::save(SaveKind kind) const {
    // DateTime.Now.ToString("s"): the sortable local time, yyyy-MM-ddTHH:mm:ss.
    char text[32] = "";
    const time_t now = time(nullptr);
    struct tm local;
    if (localtime_r(&now, &local) != nullptr) {
        strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &local);
    }
    return saveAt(kind, text);
}

} // namespace flow
} // namespace mdk
