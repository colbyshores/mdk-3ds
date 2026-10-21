#pragma once

// =============================================================================
// Fast sin/cos/atan2 via cache-resident lookup tables.
//
// ONE PATH, BOTH TARGETS. This header must NOT become a `#if __3DS__` LUT /
// `#else` libm split: that split made host harnesses prove the desktop
// arithmetic and say NOTHING about the 3DS, which ran different arithmetic.
// The table IS the arithmetic both targets execute.
//
// THE ARM11 (ARMv6K / VFPv2) has NO hardware transcendental instruction, so
// every std::sin/std::cos is a ~50-150-cycle libm call, and MDK's per-frame
// geometry build issues thousands of them (the arena's surfaces, the sprites,
// the camera's arcs, the script-driven rotations).
//
// Table: 1024 entries over [0, 2*PI), linearly interpolated. 4 KB -> fits the
// ARM11's 16 KB L1 D-cache on BOTH OG3DS and New3DS.
//
// ACCURACY -- AND THE PART THIS HEADER MUST STATE COMPLETELY. The error is
// PER-PERIOD; it GROWS WITH |rad|, because `rad * TRIG_SCALE` is a FLOAT
// multiply whose result's own quantum grows with magnitude. Measured against
// sinf on the 2026-10-21 harness:
//
//     range              max |fastSin(x) - sin(x)|
//     [0, 2*PI)                    4.76e-06
//     |x| <   100 rad              3.83e-05
//     |x| <  1000 rad              9.47e-05
//     |x| < 10000 rad              7.82e-04
//
//     (Re-measured 2026-10-21 on a 4M-point uniform grid per range against
//     libm. An earlier table understated the |x| >= 100 rows by 1.3-1.6x
//     because its sampling missed the worst-case rounding points; the grid
//     reproduces the [0, 2*PI) row exactly.)
//
// THE TABLE IS NOT THE CAUSE -- `rad * TRIG_SCALE` is, because it is a FLOAT
// multiply and its result's own quantum grows with magnitude: at |rad|~1 the
// index ULP is 1.5e-05 (an angle quantum of 9.4e-08 rad), at |rad|~10000 it
// is 0.125 (7.7e-04 rad). Past that the interpolation fraction is quantised
// more coarsely than the table is spaced.
//
// PRACTICAL READ: every phase here is time-derived, so a `time * 0.1f` phase
// reaches 1000 rad after ~10 seconds of play. If a NEW effect needs a phase
// stable over hours, wrap its argument into [0, 2*PI) at the CALL SITE
// rather than widening the table.
//
// fastAtan2's error does not have this problem -- its argument is a ratio in
// [0,1], so it stays at 3.6e-07 rad everywhere (grid-measured 2026-10-21,
// after the ax<ay branch's quarter-turn constant was corrected from 0.5 to
// 0.25 of kTwoPi; before that fix it was wrong by PI/2 on half its domain).
//
// NOTE: sqrt/div are deliberately NOT tabled -- VFPv2 has hardware
// vsqrt.f32/vdiv, which are faster AND exact than any table or bit-hack
// (Forsaken learned this the hard way ripping out the Quake-III fast-invsqrt).
//
// SEE ALSO: AGENTS.md R6 (the rule this implements) and the standing
// "LUT range reduction" exemption in tools/runner/contracts/vfp_hotpath.md.
// =============================================================================

#include <stdint.h>

namespace mdk {

// The table's period, and the scale from radians to the table's index space.
static constexpr float kTwoPi = 6.28318530717958647692f;
static constexpr float kTrigScale = 1024.0f / kTwoPi;
static constexpr int kTrigTableSize = 1024;
static constexpr int kTrigTableMask = kTrigTableSize - 1;

// Build the tables. MUST be called before any fast* call -- an uninitialised
// table is all zeroes, so every trig call returns 0. Call it from EVERY new
// entry point, including the host test harnesses.
void mathLutInit();

// The raw tables, for the harnesses that diff them against libm.
//
// The sin/cos tables carry ONE extra entry (index kTrigTableSize) so the
// interpolation's "next" is a straight index and not a masked one.
//
// The atan table carries TWO extra entries, because its domain is a RATIO in
// [0,1] and the ratio 1.0 maps to index kTrigTableSize, whose interpolation
// partner is kTrigTableSize+1. Both extras hold atan(1).
extern float g_sinTable[kTrigTableSize + 1];
extern float g_cosTable[kTrigTableSize + 1];
extern float g_atanTable[kTrigTableSize + 2];

// ---------------------------------------------------------------------------
// The leaf reduction.
//
// The `(int)` truncation is LOAD-BEARING. Do not "clean" it into a std::floor,
// and do not route through __builtin_floorf: VFPv2 has no dedicated
// float-result floor instruction, and the recorded t2k build lowered that path
// to a `bl floorf` plus a spill frame, defeating the whole optimisation.
//
// The reduction is: scale the argument into the table's index space, truncate
// to an integer index, and linearly interpolate between it and the next
// entry.
//
// THE SIGN HANDLING IS ALSO LOAD-BEARING, and it is why the code looks the
// way it does. The truncating cast truncates TOWARD ZERO, so for a negative
// argument the fractional part comes out in (-1,0] rather than [0,1). The
// index is wrapped into the table's period with a bitmask -- which is exact
// for a negative int in two's complement -- and the SIGNED fraction is kept.
// Because -k and (period - k) name the same point in the table, the
// interpolation is then correct for negative arguments with no extra branch
// on the value itself.
//
// The `frac` is NOT forced into [0,1): doing so would need a floor, and the
// floor is what this whole construction exists to avoid.
// ---------------------------------------------------------------------------
inline float fastSin(float x) {
    float scaled = x * kTrigScale;     // index space
    int index = (int)scaled;           // truncates toward zero
    float frac = scaled - (float)index;
    int wrapped = index & kTrigTableMask;
    int next = wrapped + 1;            // the table has one extra entry == [0]
    float a = g_sinTable[wrapped];
    float b = g_sinTable[next];
    return a + (b - a) * frac;
}

// The cosine has its OWN table rather than being sin(x + PI/2): the offset
// would add a constant to the argument, and the argument's magnitude is
// exactly what the table's error grows with. A separate table keeps the
// argument untouched.
inline float fastCos(float x) {
    float scaled = x * kTrigScale;
    int index = (int)scaled;
    float frac = scaled - (float)index;
    int wrapped = index & kTrigTableMask;
    int next = wrapped + 1;
    float a = g_cosTable[wrapped];
    float b = g_cosTable[next];
    return a + (b - a) * frac;
}

// atan2's argument is a RATIO in [0,1] after the quadrant reduction, so the
// table's index quantum is CONSTANT and the error does not grow with
// magnitude the way fastSin's does. That is the whole reason atan2 gets a
// table of its own rather than a ratio through the sin table.
inline float fastAtan2(float y, float x) {
    float ax = x < 0.0f ? -x : x;
    float ay = y < 0.0f ? -y : y;
    float base;
    if (ay == 0.0f) {
        base = 0.0f;
    } else if (ax == 0.0f) {
        base = kTwoPi * 0.25f;
    } else if (ax < ay) {
        // r in (0,1): atan(r) = PI/2 - atan(1/r).
        float r = ax / ay;
        float scaled = r * kTrigTableSize;
        int index = (int)scaled;
        float frac = scaled - (float)index;
        int next = index + 1;
        float a = g_atanTable[index];
        float b = g_atanTable[next];
        float at = a + (b - a) * frac;
        base = kTwoPi * 0.25f - at;
    } else {
        // r in (0,1]: atan(r) directly.
        float r = ay / ax;
        float scaled = r * kTrigTableSize;
        int index = (int)scaled;
        float frac = scaled - (float)index;
        int next = index + 1;
        float a = g_atanTable[index];
        float b = g_atanTable[next];
        base = a + (b - a) * frac;
    }
    // Restore the quadrant. atan2's range is (-PI, PI].
    if (x < 0.0f) base = kTwoPi * 0.5f - base;
    if (y < 0.0f) base = -base;
    return base;
}

} // namespace mdk
