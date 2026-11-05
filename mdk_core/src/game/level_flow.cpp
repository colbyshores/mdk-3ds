// level_flow.cpp — see level_flow.h.
//
// A transliteration of LevelFlow.cs. The three functions are three lines each in
// the C#, and the whole of their behaviour is which branch a level number that is
// NOT in the order takes; tests/scripts/flow_tests.cpp pins every row of that,
// measured from the C# classes themselves.

#include "game/level_flow.h"

namespace mdk {
namespace flow {

// C# GameState.Order (0x490030).
const int32_t kOrder[6] = {7, 6, 3, 4, 8, 5};

// The last level index followed by the statistics (C# LastStatisticsIndex).
namespace {
constexpr int kLastStatisticsIndex = 3;

int clampInt(int value, int low, int high) {
    // C# Math.Clamp(value, min, max).
    return value < low ? low : (value > high ? high : value);
}
} // namespace

int indexOf(int32_t level) {
    for (int i = 0; i < kOrderCount; i++) {
        if (kOrder[i] == level) {
            return i;
        }
    }
    return -1;
}

After afterLevel(int32_t level, GameOver over) {
    const int index = indexOf(level);
    if (over == GameOver::Yes || index < 0 || index + 1 >= kOrderCount) {
        return After::Menu;
    }
    return index <= kLastStatisticsIndex ? After::Statistics : After::LastLevel;
}

int32_t next(int32_t level) {
    const int index = clampInt(indexOf(level) + 1, 0, kOrderCount - 1);
    return kOrder[index];
}

int32_t saveNameNumber(int32_t level) {
    return indexOf(level) + 1;
}

} // namespace flow
} // namespace mdk
