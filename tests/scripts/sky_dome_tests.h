#pragma once

// ============================================================================
// sky_dome_tests.h -- the 3DS sky dome checked against the oracle's panorama
// mapping (shaders/panorama.hlsli), on the host. Every vertex of the band must
// map, through the oracle's own formula, back to the column and row its uv
// names; slices must fit the PICA's 1024-wide limit and cover every column
// exactly once; index 0 must be remapped (the sky has no discard).
// Returns the number of failed checks.
// ============================================================================

namespace mdk {
namespace test {
int runSkyDomeTests();
} // namespace test
} // namespace mdk
