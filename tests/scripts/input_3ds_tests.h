#pragma once

// ============================================================================
// input_3ds_tests.h — the 3DS control mapping, checked on the host.
//
// The 3DS cannot be run here, and the C# has no 3DS device to measure against
// (it reads SDL), so this is the ONLY thing that checks the mapping at all — and
// it must therefore say, row by row, where each expected value came from:
//
//   * the C# `GamepadMap` / `Input` constants, for the look's rates, dead zone,
//     curve and degrees-per-count;
//   * hand-derived float arithmetic, for every pinned bit pattern (the
//     derivation is written next to the row, and each one is a case where the
//     rounding can be done by hand);
//   * properties of the C# formula (symmetry, monotonicity, the dead zone, the
//     frame-gap clamp), which a wrong implementation breaks without knowing it.
//
// Nothing here was read off `input_3ds.cpp`'s output.
//
// The function returns the number of failed checks, so the caller adds it to
// its own counter.
// ============================================================================

namespace mdk {
namespace test {

// The digital map, the circle pad's look, and the accumulated mouse.
int runInput3dsTests();

} // namespace test
} // namespace mdk
