#pragma once

// =============================================================================
// The C#'s MathF.Min / MathF.Max, as ONE pair of functions for the whole tree.
//
// This is one behaviour, so it is one implementation: the same pair used to be
// spelled out four times (scripts/script_runtime.cpp, scripts/script_vm.cpp,
// audio/sound_mixer.cpp, game/camera_roll.cpp), and the third of those had
// already drifted into a different shape. Same reasoning as the single
// `accelerate()` in scripts/script_runtime.cpp (batch 52): the oracle has ONE
// MathF.Min, so the port has ONE mathMin.
//
// WHY NOT fminf/fmaxf — NaN. Both C functions are NaN-SILENT: fminf(x, NaN)
// and fminf(NaN, x) return x, so a NaN speed, roll, friction or half-height
// would quietly become the finite bound. MathF.Min/MathF.Max propagate the NaN,
// and every call site is a place the C# writes MathF.Min/MathF.Max, not a clamp
// it wrote as a ternary.
//
// WHY NOT A TERNARY EITHER — the SIGN OF ZERO. Measured on .NET 10, the rows
// printed by the oracle probe (docs/PROGRESS_LOG.md, "Iteration 83", probe mode
// `nanclamp`):
//
//     MathF.Min(-0.0, +0.0) = -0.0 bits 80000000   MathF.Max(-0.0, +0.0) = +0.0 bits 00000000
//     MathF.Min(+0.0, -0.0) = -0.0 bits 80000000   MathF.Max(+0.0, -0.0) = +0.0 bits 00000000
//     MathF.Min(-0.0, -0.0) = -0.0                MathF.Max(-0.0, -0.0) = -0.0
//     MathF.Min(-2.0, -2.0) = -2.0                MathF.Max(-2.0, -2.0) = -2.0
//     MathF.Min(+inf, -inf) = -inf                MathF.Max(+inf, -inf) = +inf
//
// i.e. the oracle orders -0.0 BELOW +0.0. A float compare cannot see that,
// because -0.0 == +0.0, so `a < b ? a : b` returns the wrong zero for one of the
// two argument orders, and so does `a > b ? a : b` for Max. fminf/fmaxf DO agree
// with .NET here — the ternary is the divergence, and it is the only one.
// Pinned by MathMinMaxMatchTheOracleOnNaNAndSignedZero in tests/scripts/vm_tests.cpp.
//
// The NaN PAYLOAD is not part of the contract: the oracle returns whichever
// operand is the NaN, payload and all (probe rows MINMAX nan_one / one_nan), so
// the pins assert NaN-ness rather than a canonical payload.
//
// ARM11 NOTES (AGENTS.md 1, 3, 4; R2/R3 do not bite here). The tie arm reads the
// sign off the bit pattern with an INTEGER compare, so it is a VFP-to-core
// register move, not the float->int conversion R2 restricts and not a vcmp/vmrs
// pair R3 restricts, and it is reached only when the two operands already
// compare equal. No libm, no division, no double.
// =============================================================================

#include <stdint.h>
#include <string.h>

namespace mdk {

// -0.0f is the ONLY value whose bits are 0x80000000, so the sign of a zero is a
// bit test: no libm signbit, no reciprocal, no float comparison.
inline bool isNegativeZero(float value) {
    uint32_t bits = 0u;
    memcpy(&bits, &value, sizeof(bits));
    return bits == 0x80000000u;
}

// MathF.Min: NaN-propagating, and -0.0 sorts below +0.0.
inline float mathMin(float a, float b) {
    if (a != a) {
        return a;
    }
    if (b != b) {
        return b;
    }
    if (a < b) {
        return a;
    }
    if (b < a) {
        return b;
    }
    // Equal in value. The pair can still be {+0.0, -0.0}, and the oracle keeps the
    // -0.0; for any other tie both operands hold the same bits, so either will do.
    return isNegativeZero(a) ? a : b;
}

// MathF.Max: NaN-propagating, and +0.0 sorts above -0.0.
inline float mathMax(float a, float b) {
    if (a != a) {
        return a;
    }
    if (b != b) {
        return b;
    }
    if (a > b) {
        return a;
    }
    if (b > a) {
        return b;
    }
    // Equal in value; the oracle returns +0.0 for the {+0.0, -0.0} pair, so a
    // first operand of -0.0 loses to anything it ties with.
    return isNegativeZero(a) ? b : a;
}

} // namespace mdk
