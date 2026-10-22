#pragma once

// ============================================================================
// random.h — the seeded PRNG that decides gameplay randomness.
//
// WHY THIS EXISTS
// ---------------
// The C# oracle drives every random gameplay decision through
// `System.Random`. `ScriptRuntime` builds it as `new Random(seed)` when a seed
// is supplied (ScriptRuntime.cs:323), and a seeded `System.Random` is NOT the
// modern generator: .NET routes a seeded base-type instance to the legacy
// subtractive generator (`Random.Net5CompatImpl.cs`, the `CompatPrng` struct)
// so that a sequence a game relied on before .NET 5 stays reproducible. Only
// the UNSEEDED `new Random()` uses the modern xoshiro path.
//
// So byte-for-byte parity with the oracle REQUIRES reimplementing that legacy
// subtractive algorithm exactly — its seeding, its scramble, its wrap rules,
// and its `1.0 / int.MaxValue` scaling. A different PRNG (xoshiro, xorshift,
// the C library's) reproduces NOTHING the oracle produced, which is why an
// earlier xoshiro-based port failed its differential and was removed.
//
// THE CONTRACT
// ------------
// This is a faithful port of `CompatPrng` as shipped in dotnet/runtime v8.0.0
// (`src/libraries/System.Private.CoreLib/src/System/Random.Net5CompatImpl.cs`).
// It is verified against the real .NET runtime, not against a memory of the
// algorithm: an adversarial differential over thousands of seeds — including
// negatives, int.MinValue/int.MaxValue, and seeds large enough that the
// seeding's `mj` goes negative — matches `Next()`, `Next(max)`, `NextDouble()`
// and `NextSingle()` bit-for-bit (see tests/formats/random_tests.cpp). Any
// change here must keep that differential green — a single altered wrap or
// scaling constant silently moves every random decision in the game.
//
// The surface is only what the game logic calls: `next()`, `next(max)`,
// `nextDouble()`, `nextSingle()`. The C# also exposes `Next(min,max)`,
// `NextBytes`, `NextInt64`; none are used by the ported logic, so they are not
// here. Add them (and extend the differential) only when a caller needs one.
// ============================================================================

#include <cstdint>

namespace mdk {

class Random {
public:
    // Seed matches `new System.Random(seed)`, including the int.MinValue ->
    // int.MaxValue abs() special case the C# performs.
    explicit Random(int seed);

    // `Next()` — a value in [0, int.MaxValue).
    int next();

    // `Next(maxValue)` — a value in [0, maxValue). Like the C#, it always
    // consumes a sample (so `next(0)` consumes and returns 0). A negative bound
    // returns 0 without consuming, a deliberate no-exceptions deviation from the
    // C# throw; the ported logic never passes one.
    int next(int maxValue);

    // `NextDouble()` — a value in [0, 1).
    double nextDouble();

    // `NextSingle()` — a value in [0, 1f), rejecting the rare 1.0f that the
    // double->float rounding can otherwise produce.
    float nextSingle();

private:
    // The single source of randomness: the subtractive step. Every public
    // method advances the state exactly once through here, exactly as the C#
    // routes every public method through InternalSample()/Sample().
    int internalSample();
    double sample();

    int m_seedArray[56];  // [0] is unused by the algorithm; [1..55] are live.
    int m_inext;
    int m_inextp;
};

}  // namespace mdk
