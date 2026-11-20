// ============================================================================
// camera_roll.cpp — see camera_roll.h.
// ============================================================================

#include "game/camera_roll.h"

#include <math.h>

#include "util/math_minmax.h"

namespace mdk {

namespace {

// mathMin/mathMax are the oracle's MathF.Min/MathF.Max and live in
// util/math_minmax.h, ONE definition for the whole tree. The oracle writes them
// here (CameraRoll.cs:47,52), so a NaN roll stays NaN instead of snapping to the
// limit, which fminf/fmaxf would not do: they return the numeric side when
// exactly one argument is NaN, turning a NaN roll into +/-10 degrees.

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

// MathF.Sign: 1 above zero, -1 below, and 0 for zero. It returns an INT, and the
// int 0 for BOTH +0.0 and -0.0 (measured on .NET 10: `SIGN pos_zero -> 0`,
// `SIGN neg_zero -> 0`), so the port's float 0.0f is a conversion of that int, not
// the oracle's own bit pattern. copysignf is NOT the same function: it returns -1
// for -0.0.
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

/* @vfp-exempt R6 — the slide's bank: atan2f and powf are the oracle's MathF.Atan2 and
 * MathF.Pow (glibc on both sides of the host goldens), and .NET's left-associative
 * RadiansToDegrees; the fastAtan2 LUT would move the roll golden/L6_wind_*.txt pins. Once
 * a step while sliding, not a loop body. measured n/a (corpus empty). Verified 2026-11-20. */
void CameraRoll::slide(const Vec3& normal, const Vec2& facing, float delta) {
    // C# CameraRoll.Slide (CameraRoll.cs:54-59), in its order: the held flag, the angle
    // of the slope across the facing, then float.Lerp(SlideLevel - angle, Roll,
    // Pow(SlideKeep, Ticks * delta)). .NET 10's float.Lerp is
    // MultiplyAddEstimate(a, 1 - t, b * t): FUSED where the CPU has FMA, which the host
    // that prints the goldens does -- measured 2026-11-20, 200000 random (a, b, t): float.Lerp
    // equals MathF.FusedMultiplyAdd(a, 1 - t, b * t) for all of them and the unfused
    // a * (1 - t) + b * t for 151471. fmaf is that fused form on every target (a library
    // call on the ARM11's VFPv2, once a step); the unfused spelling put
    // golden/L6_wind_tunnel.txt's roll a 4th-decimal unit off at step 205.
    held_ = true;
    const float radians = atan2f(normal.z, normal.x * facing.y - normal.y * facing.x);
    const float angle = radians * 180.0f / 3.14159265358979323846f;
    const float t = powf(kSlideKeep, kTicks * delta);
    roll_ = fmaf(kSlideLevel - angle, 1.0f - t, roll_ * t);
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
