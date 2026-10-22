#include "random.h"

#include <climits>

namespace mdk {

namespace {
// The golden-ratio-derived magic number the C# seeding starts from.
const int kMj = 161803398;
}  // namespace

Random::Random(int seed) {
    // Mirrors CompatPrng.Initialize verbatim. The array is zero-initialised
    // (the C# allocates `new int[56]`); the old .NET Framework SeedArray table
    // is NOT used by this generator.
    for (int i = 0; i < 56; i++) m_seedArray[i] = 0;

    int subtraction = (seed == INT_MIN) ? INT_MAX : (seed < 0 ? -seed : seed);
    int mj = kMj - subtraction;
    m_seedArray[55] = mj;
    int mk = 1;

    int ii = 0;
    for (int i = 1; i < 55; i++) {
        ii += 21;
        if (ii >= 55) ii -= 55;
        m_seedArray[ii] = mk;
        mk = mj - mk;
        if (mk < 0) mk += INT_MAX;
        mj = m_seedArray[ii];
    }

    for (int k = 1; k < 5; k++) {
        for (int i = 1; i < 56; i++) {
            int n = i + 30;
            if (n >= 55) n -= 55;
            // The C# does unchecked `int` subtraction, which wraps on overflow.
            // For seeds whose `mj` is very negative this subtraction exceeds
            // INT_MAX, so do it in uint32_t (defined modular arithmetic) and
            // reinterpret; the following `< 0` test then matches the C# exactly.
            m_seedArray[i] = (int)((unsigned)m_seedArray[i] - (unsigned)m_seedArray[1 + n]);
            if (m_seedArray[i] < 0) m_seedArray[i] += INT_MAX;
        }
    }

    m_inext = 0;
    m_inextp = 21;
}

int Random::internalSample() {
    int locINext = m_inext;
    if (++locINext >= 56) locINext = 1;

    int locINextp = m_inextp;
    if (++locINextp >= 56) locINextp = 1;

    int retVal = m_seedArray[locINext] - m_seedArray[locINextp];

    if (retVal == INT_MAX) retVal--;
    if (retVal < 0) retVal += INT_MAX;

    m_seedArray[locINext] = retVal;
    m_inext = locINext;
    m_inextp = locINextp;
    return retVal;
}

double Random::sample() {
    // The division by int.MaxValue (not 2^31) is what the C# does and is what
    // makes the differential match; changing the denominator breaks parity.
    return internalSample() * (1.0 / INT_MAX);
}

int Random::next() { return internalSample(); }

int Random::next(int maxValue) {
    // C# `Next(maxValue)` is `(int)(Sample() * maxValue)`: it ALWAYS consumes a
    // sample, including for maxValue == 0 (where it yields 0). Returning early
    // without consuming would shift every later draw against the oracle, so the
    // only guard is the negative bound C# rejects by throwing. This build has no
    // exceptions and the ported logic never passes a negative bound, so a
    // negative bound yields 0 (a deliberate deviation from the throw).
    if (maxValue < 0) return 0;
    return (int)(sample() * maxValue);
}

double Random::nextDouble() { return sample(); }

float Random::nextSingle() {
    while (true) {
        float f = (float)sample();
        if (f < 1.0f) return f;  // reject the rare 1.0f from double->float rounding.
    }
}

}  // namespace mdk
