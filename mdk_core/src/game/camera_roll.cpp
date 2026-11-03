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

// MathF.Sign: 1 above zero, -1 below, and 0 for zero — measured on .NET 10 to be +0.0
// (bits 00000000) for BOTH +0.0 and -0.0, because Sign returns the value when it compares
// equal to zero. copysignf is NOT the same function: it returns -1 for -0.0.
//
// The NaN case is a DELIBERATE DIVERGENCE, not the oracle's value: MathF.Sign(NaN) throws
// ArithmeticException (System.Runtime.xml, and measured: `SIGN nan -> THROWS
// ArithmeticException`), so the oracle never returns anything for a NaN roll — Settle
// faults on the way there. This returns 0, which leaves the roll NaN (0 * NaN is NaN) and
// lets the port run on. A shipped frontend has no way to make the roll NaN, so nothing
// reaches the difference; the pin says "divergence", not "parity".
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
    // A NaN roll reaches signf here, where the ORACLE throws (see signf above); this
    // returns 0 and the NaN survives the subtraction, which is the documented divergence.
    roll_ = fabsf(roll_) <= step ? 0.0f : roll_ - signf(roll_) * step;
}

} // namespace mdk
