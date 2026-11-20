// ============================================================================
// voice_lifecycle.cpp -- see voice_lifecycle.h.
// ============================================================================

#include "audio/voice_lifecycle.h"

#include <string.h>

namespace mdk {
namespace audio {

namespace {

// The exponent of the lowest set bit of a nonzero float: v = odd * 2^result.
// Read off the bit pattern, so nothing here rounds.
int32_t lowBitExponent(float v) {
    uint32_t bits = 0;
    memcpy(&bits, &v, sizeof(bits));
    const uint32_t field = (bits >> 23) & 0xFFu;
    uint32_t mantissa = bits & 0x7FFFFFu;
    if (field == 0u) {
        return -149 + (int32_t)__builtin_ctz(mantissa);  // subnormal: mantissa * 2^-149
    }
    mantissa |= 0x800000u;
    return (int32_t)field - 150 + (int32_t)__builtin_ctz(mantissa);
}

// The same for a nonzero double.
int32_t lowBitExponent(double v) {
    uint64_t bits = 0;
    memcpy(&bits, &v, sizeof(bits));
    const uint32_t field = (uint32_t)((bits >> 52) & 0x7FFu);
    uint64_t mantissa = bits & 0xFFFFFFFFFFFFFull;
    if (field == 0u) {
        return -1074 + (int32_t)__builtin_ctzll(mantissa);
    }
    mantissa |= 0x10000000000000ull;
    return (int32_t)field - 1075 + (int32_t)__builtin_ctzll(mantissa);
}

// floor(log2(v)) for a positive finite double (a subnormal reads as -1023,
// which only makes the bound below stricter).
int32_t topExponent(double v) {
    uint64_t bits = 0;
    memcpy(&bits, &v, sizeof(bits));
    return (int32_t)((bits >> 52) & 0x7FFu) - 1023;
}

// Finite and not NaN, by the bit pattern (an exponent field of all ones is an
// infinity or a NaN), so the test does not depend on the compiler's float flags.
bool finite(double v) {
    uint64_t bits = 0;
    memcpy(&bits, &v, sizeof(bits));
    return ((bits >> 52) & 0x7FFu) != 0x7FFu;
}

} // namespace

bool lifecycleStep(double* position, float step, int32_t frames, bool looping, int32_t count) {
    double p = *position;
    /* @vfp-exempt R3,R4 — SoundDevice::mixVoice's own lifecycle arithmetic, kept
     * as the oracle's: the double position (R4) and the float compare that
     * decides the wrap (R3). It is the fallback of lifecycleAdvance(), which
     * solves the same loop whenever the solved answer is provably the loop's,
     * so on the DSP path this runs only for a position or step outside that
     * bound. measured n/a (corpus empty). Verified 2026-11-19. */
    for (int32_t f = 0; f < count; f++) {
        if (p >= (double)frames) {
            if (!looping) {
                *position = p;
                return false;
            }
            p -= (double)frames;
        }
        p += (double)step;
    }
    *position = p;
    return true;
}

bool lifecycleAdvance(double* position, float step, int32_t frames, bool looping, int32_t count,
                      bool* solved) {
    if (solved != nullptr) {
        *solved = false;
    }
    if (count <= 0) {
        return true;
    }
    const double p0 = *position;
    // The solved form needs a positive finite step, a sound with frames and a
    // finite position that is not negative; anything else is the loop's.
    if (!(step > 0.0f) || frames <= 0 || !finite((double)step) || !finite(p0) || !(p0 >= 0.0)) {
        return lifecycleStep(position, step, frames, looping, count);
    }

    const double s = (double)step;
    const double n = (double)frames;
    // The quantum: a power of two that divides the step, the position and the
    // (integer) frame count.
    int32_t q = lowBitExponent(step);
    if (p0 != 0.0) {
        const int32_t qp = lowBitExponent(p0);
        q = qp < q ? qp : q;
    }
    q = q < 0 ? q : 0;
    // Every value this update can form is below max(p0, frames) + step * (count + 1):
    // the position only grows by `step` between checks and only shrinks by a wrap.
    // One power of two of margin over the 2^53 * 2^q the proof needs.
    const double bound = (p0 > n ? p0 : n) + s * ((double)count + 1.0);
    if (!finite(bound) || topExponent(bound) >= 52 + q) {
        return lifecycleStep(position, step, frames, looping, count);
    }

    double p = p0;
    int32_t remaining = count;
    // Each pass consumes at least one output frame, so `remaining` alone ends the
    // loop (AGENTS.md T2: the condition is an integer); with a normal sound a
    // whole update is one pass, or two around a wrap.
    for (int32_t pass = 0; remaining > 0 && pass <= count; pass++) {
        if (p >= n) {
            // The check that fires: the loop's own end or wrap, then its step.
            if (!looping) {
                *position = p;
                if (solved != nullptr) {
                    *solved = true;
                }
                return false;
            }
            p -= n;
            p += s;
            remaining--;
            continue;
        }
        // p < frames: the checks at p, p+s, ... pass until the first p + k*s >=
        // frames, k >= 1, which is ceil((frames - p) / s). The quotient is within an
        // ulp of the real one, so its floor is k or k - 1; one exact compare picks.
        const double ratio = (n - p) / s;
        if (ratio >= (double)remaining + 1.0) {
            // k > remaining: every remaining frame passes its check.
            p += (double)remaining * s;
            remaining = 0;
            break;
        }
        int32_t k = (int32_t)ratio;
        if (!(k >= 1 && p + (double)k * s >= n)) {
            k++;
        }
        if (k >= remaining) {
            p += (double)remaining * s;
            remaining = 0;
        } else {
            p += (double)k * s;
            remaining -= k;
        }
    }
    *position = p;
    if (solved != nullptr) {
        *solved = true;
    }
    return true;
}

} // namespace audio
} // namespace mdk
