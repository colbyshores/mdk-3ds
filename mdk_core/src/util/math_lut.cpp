// =============================================================================
// math_lut.cpp — build the trig tables.
//
// The table is built with std::sin/std::cos/std::atan AT BUILD TIME, once.
// That is not a violation of AGENTS.md R6: R6 forbids libm transcendentals
// in the HOT PATH, and this is init -- it runs once per process, from
// mathLutInit(), before any frame.
//
// The seed values are the exact functions, so the table's error is purely
// the linear interpolation's, which is what the header's accuracy table
// measures.
// =============================================================================

#include "math_lut.h"

#include <cmath>

namespace mdk {

float g_sinTable[kTrigTableSize + 1];
float g_cosTable[kTrigTableSize + 1];
float g_atanTable[kTrigTableSize + 2];

void mathLutInit() {
    const float step = kTwoPi / (float)kTrigTableSize;
    for (int i = 0; i < kTrigTableSize; i++) {
        const float angle = step * (float)i;
        g_sinTable[i] = std::sin(angle);
        g_cosTable[i] = std::cos(angle);
    }
    // The wrap entry: index kTrigTableSize is the same point as index 0.
    g_sinTable[kTrigTableSize] = g_sinTable[0];
    g_cosTable[kTrigTableSize] = g_cosTable[0];

    // atan's domain is the ratio in [0,1], so the table's argument is
    // i/kTrigTableSize and the last entry is atan(1) = PI/4.
    for (int i = 0; i <= kTrigTableSize; i++) {
        const float ratio = (float)i / (float)kTrigTableSize;
        g_atanTable[i] = std::atan(ratio);
    }
    // The interpolation partner of index kTrigTableSize (the ratio 1.0).
    g_atanTable[kTrigTableSize + 1] = g_atanTable[kTrigTableSize];
}

} // namespace mdk
