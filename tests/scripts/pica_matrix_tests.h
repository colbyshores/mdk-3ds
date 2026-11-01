#pragma once

// ============================================================================
// pica_matrix_tests.h — the Citro3D backend's matrix math, checked on the host.
//
// The 3DS picture is UNVERIFIED on hardware (AGENTS.md P6) and every one of
// these conversions fails as INVISIBLE GEOMETRY rather than as a wrong picture,
// so this is the only place any of it can be pinned. See
// `mdk_3ds/src/render/pica_matrix.h` for what each function is for.
//
// WHERE THE EXPECTED NUMBERS COME FROM. The C# oracle pins the ENGINE side: the
// row-vector convention, the translation in the fourth row, and the order
// `world * view * projection` (Renderer.cs:1072). The three PICA conversions
// have no C# counterpart — they are this target's — so their expectations are
// derived by hand from the documented recipe (clip becomes `(y, -x, z-w, w)`)
// on inputs whose arithmetic is exact in binary, and cross-checked against an
// independent multiply written in the test, not copied from the implementation.
// Nothing here was read off the code under test.
//
// The function returns the number of failed checks, so the caller adds it to
// its own counter.
// ============================================================================

namespace mdk {
namespace test {

// The per-draw matrix, the PICA conversions, and the uniform register layout.
int runPicaMatrixTests();

} // namespace test
} // namespace mdk
