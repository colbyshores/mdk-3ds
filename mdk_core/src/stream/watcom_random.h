#pragma once

// ============================================================================
// watcom_random.h — Watcom's rand(), the stream's random numbers
// (C# src/Mdk.Game/Stream/WatcomRandom.cs).
//
// The original (0x4794dd) is the C library's linear congruence:
//
//   state = state * 0x41C64E6D + 0x3039      (mod 2^32)
//   rand  = bits 16-30 of state              (0 .. 0x7FFF)
//
// The stream draws its tube, its lights and its wall damage from ONE of these:
// StreamTube and Flight share the instance, so the order of every draw is part
// of the game (golden/stream_rand.txt pins the sequence, golden/stream_*.txt
// the order).
//
// It is NOT mdk::Random (util/random.h): that one is .NET's seeded
// System.Random, which the levels' scripts use. The stream is the original's
// C library generator, which is why the C# has a class of its own for it.
// ============================================================================

#include <stdint.h>

namespace mdk {
namespace stream {

class WatcomRandom {
public:
    // The C library's seed when srand was never called (WatcomRandom.DefaultSeed).
    static constexpr uint32_t kDefaultSeed = 1;
    static constexpr int32_t kMax = 0x7FFF;

    explicit WatcomRandom(uint32_t seed = kDefaultSeed) : state_(seed) {}

    // WatcomRandom.Next: 0 .. 0x7FFF. The multiply wraps mod 2^32 as the C#'s
    // unchecked uint arithmetic does (unsigned overflow is defined in C++).
    int32_t next()
    {
        state_ = state_ * kMultiplier + kIncrement;
        return (int32_t)(state_ >> kShift) & kMax;
    }

    // The generator's state (the tests print it to pin every draw).
    uint32_t state() const { return state_; }

private:
    static constexpr uint32_t kMultiplier = 0x41C64E6Du;
    static constexpr uint32_t kIncrement = 0x3039u;
    static constexpr int kShift = 16;

    uint32_t state_;
};

} // namespace stream
} // namespace mdk
