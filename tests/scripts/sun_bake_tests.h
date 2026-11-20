#pragma once

// ============================================================================
// sun_bake_tests.h -- the 3DS's baked static light on the host
// (EnhancedToggles::lightBaked): the build-time baker
// (mdk_3ds/tools/sun_bake/sun_baker.cpp) against hand-built scenes, read back
// through the runtime's own lookup (mdk_3ds/src/render/sun_bake.cpp: the side's
// vertex colour times the grey map's m) and held against an independent rewrite
// of the lit geometry shader's formula (mdk_c3d.g.pica) -- a box over a floor
// darkens the floor under it by the shadow's ratio and leaves the rest at F_lit,
// to the byte's rounding; a one-sided caster
// blocks the sun only with its front to it, a two-sided one either way; a
// two-sided receiver has both sides, each lit by its own normal; a covered
// arena has no shadow (the white block); the same input bakes the
// same bytes; the file's sections, keys and refusals; the objects' depth map.
// Returns the number of failed checks.
// ============================================================================

namespace mdk {
namespace test {
int runSunBakeTests();
} // namespace test
} // namespace mdk
