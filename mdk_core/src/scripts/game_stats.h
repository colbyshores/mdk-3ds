#pragma once

// ============================================================================
// game_stats.h — the level's counts for the Score-o-matic.
//
// A 1:1 port of src/Mdk.Game/Scripts/GameStats.cs: chain-gun shots (6 per
// tick of fire, 0x573c3c) and ticks on target (0x573c40), sniper rounds
// (0x573c44) and their hits (0x573c48), head shots (0x573c4c), enemies
// created (0x573c50) and killed by Kurt (0x573c54). A level starts with none.
//
// HEADER ONLY, on purpose: plain counters and two small predicates, and a
// header needs no object file, so it cannot collide with another file's
// object in the 3DS build (which keys objects by `notdir`) — the same
// reason formats/ascii_case.h gives. Opcode 217 (global_573c4c_add) writes
// HeadShots through this struct; the other writers (chain-gun fire, sniper
// rounds, the kill path, town flags) land with their own slices, exactly as
// the C# reads them while those subsystems are unported — the struct starts
// at zero, which is what a fresh level has. GameStats.IndexOf is NOT here:
// it is GameState.Order (the save-game/Viewer slice), and nothing ported so
// far reads it.
//
// The goldens do not dump these counters, so the writers land without
// golden churn; opcode 217 is pinned by vm_tests instead.
// ============================================================================

#include <stdint.h>

#include <string>

#include "formats/ascii_case.h"

namespace mdk {
namespace scripts {

// Whether an object type counts as an enemy (0x491c38, GameStats.IsEnemy).
// The table holds the C#'s upper-case names; the lookup upper-cases the
// input first, as the C#'s ToUpperInvariant does.
inline bool isEnemyType(const std::string& type) {
    static const char* const kEnemyTypes[] = {
        "XB", "XB1", "XB2", "XB3", "XBSHARK", "XBSHIP", "XBT", "XC",
        "XCARGO", "XD", "XD6GUN", "XE", "XEARTH", "XF", "XFORK", "XG",
        "XG_BOMB1", "XGEN", "XGHTARG", "XGSNOW", "XGSMOKE", "XG_MISS",
        "XGTARG", "XGUNTA", "XM3", "XMART", "XPER", "XS", "XT", "XTANK",
        "XTGUN", "XTUR", "XU",
    };
    const std::string upper = formats::toUpperAscii(type);
    for (size_t i = 0; i < sizeof(kEnemyTypes) / sizeof(kEnemyTypes[0]); i++) {
        if (upper == kEnemyTypes[i]) {
            return true;
        }
    }
    return false;
}

struct GameStats {
    int32_t shots = 0;
    int32_t shotHits = 0;
    int32_t sniperShots = 0;
    int32_t sniperHits = 0;
    int32_t headShots = 0;
    int32_t enemies = 0;
    int32_t kills = 0;
    // The global flags when the level ended (which towns were flattened).
    int32_t townFlags = 0;
    bool strikeUsed = false;

    enum Kill { Created, Killed };

    // An enemy created (0x43bc20) or killed by Kurt (0x43357c).
    void countEnemy(const std::string& type, Kill kill) {
        if (!isEnemyType(type)) {
            return;
        }
        if (kill == Killed) {
            kills++;
        } else {
            enemies++;
        }
    }

    // Accuracy in percent (truncated), 0 without shots. Score-o-matic
    // level-end math, cold: the runtime int division is the C#'s shape.
    static int32_t percent(int32_t hits, int32_t shots) {
        return shots > 0 ? hits * 100 / shots : 0;
    }
};

} // namespace scripts
} // namespace mdk
