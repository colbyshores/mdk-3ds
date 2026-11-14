#pragma once

// ============================================================================
// sun_shadow.h -- the sun's shadow map matrix, ported 1:1 from the C#'s
// SunShadow.cs: an orthographic view along the sunlight, a square of
// 2 x radius around a centre (the camera), the centre snapped to whole
// texels so shadows do not shimmer when the camera moves.
// ============================================================================

#include "render/render_types.h"

namespace mdk {

// How far the map reaches towards and away from the sun, from the centre.
const float kSunShadowReach = 4000.0f;

// World to the shadow map's clip space (x, y in -1..1; depth 0..1 growing
// along the light), row-major row-vector like every engine matrix.
// `size` is the shadow map's texel size (2048 in the enhanced look).
void sunShadowMatrix(const Vec3& sunDirection, const Vec3& centre, float radius,
                     uint32_t size, float out[16]);

} // namespace mdk
