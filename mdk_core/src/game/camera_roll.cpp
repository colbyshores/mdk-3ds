// ============================================================================
// camera_roll.cpp — see camera_roll.h.
// ============================================================================

#include "game/camera_roll.h"

#include <math.h>

namespace mdk {

namespace {

// MathF.Min / MathF.Max: NaN-propagating. fminf/fmaxf are NOT, because they return
// the numeric side when exactly one argument is NaN, which would silently turn a
// NaN roll into +/-10 degrees. The oracle writes MathF.Min/MathF.Max here
// (CameraRoll.cs:47,52), so a NaN roll stays NaN, as it does there. Same pair as
// scripts/script_runtime.cpp's, for the same reason.
float mathMin(float a, float b) {
    if (a != a) {
        return a;
    }
    if (b != b) {
        return b;
    }
    return a < b ? a : b;
}

float mathMax(float a, float b) {
    if (a != a) {
        return a;
    }
    if (b != b) {
        return b;
    }
    return a > b ? a : b;
}

// Math.Clamp(value, min, max) (System.Math): below min -> min, above max -> max,
// and anything that compares to neither (a NaN) passes through unchanged.
float clampDecay(float value, float lo, float hi) {
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

// MathF.Sign: 1 above zero, -1 below, 0 for zero (including -0.0) and for a NaN,
// because both comparisons fail. copysignf is NOT the same function: it returns -1
// for -0.0. Only reachable here through a NaN roll, but the oracle's own value is
// what the port owes.
float signf(float value) {
    if (value > 0.0f) {
        return 1.0f;
    }
    return value < 0.0f ? -1.0f : 0.0f;
}

} // namespace

void CameraRoll::walk(float right, float forward, float delta) {
    // C# CameraRoll.Walk (CameraRoll.cs:36-53). Both axes or nothing: the C# tests
    // them before it sets the held flag, so a step with the turn keys pressed and no
    // walk input neither banks nor holds the leveller.
    if (forward == 0.0f || right == 0.0f) {
        return;
    }

    held_ = true;
    const float step = kWalkRate * delta;
    if (right * forward > 0.0f) {
        roll_ += step;
        // Turning the other way first jumps back towards level (CameraRoll.cs:47).
        roll_ = mathMin(roll_ < 0.0f ? roll_ + kWalkReverse : roll_, kWalkLimit);
        return;
    }
    roll_ -= step;
    roll_ = mathMax(roll_ > 0.0f ? roll_ - kWalkReverse : roll_, -kWalkLimit);
}

void CameraRoll::settle(float delta) {
    // C# CameraRoll.Settle (CameraRoll.cs:71-80). The held step is the oracle's, not
    // a smoothing detail: see the header.
    if (held_) {
        held_ = false;
        return;
    }

    // The C#'s own order: clamp first, then * Kurt.Ticks * delta.
    const float step = clampDecay(kDecayFactor * fabsf(roll_), kDecayMin, kDecayMax) *
                       kTicks * delta;
    roll_ = fabsf(roll_) <= step ? 0.0f : roll_ - signf(roll_) * step;
}

} // namespace mdk
