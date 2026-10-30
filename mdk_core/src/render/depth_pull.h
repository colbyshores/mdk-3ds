#pragma once

// ============================================================================
// depth_pull.h — the oracle's depth pull for coplanar arena surfaces.
//
// A poster drawn over a wall is on the wall's plane. With a depth buffer the
// two would fight for the same depth, so the C# oracle assigns the poster a
// depth layer (src/Mdk.Game/Level/Layers.cs) and pulls it towards the eye by
// a small fraction of its distance (src/Mdk.Engine/Render/DepthPull.cs).
//
// This is the shared half of that transform. The renderer applies it between
// the surface's world transform and the view, exactly as the oracle does:
//
//     world * depth_pull * view * projection
//
// The pull keeps the surface in the same place on screen while making it
// nearer in depth. Lines get a slightly larger share so their edge stays
// visible over the surface it outlines.
// ============================================================================

#include <stdint.h>

#include "render/render_types.h"

namespace mdk {

// Builds the row-major, row-vector world transform that scales a surface
// towards `eye`. `layer` is the arena triangle's depth layer.
//
// The result is the identity for an ordinary triangle on layer zero.
void buildDepthPullMatrix(const Vec3& eye, Primitive primitive, int32_t layer, float out[16]);

} // namespace mdk
