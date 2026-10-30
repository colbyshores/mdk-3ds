// ============================================================================
// depth_pull.cpp — see depth_pull.h.
// ============================================================================

#include "render/depth_pull.h"

#include <string.h>

namespace mdk {

namespace {

// The C# DepthPull.cs shares. A layer moves 1/256 of the eye-to-surface
// distance; a line moves 1/128, so an outline covers the edge it lies on.
const float kLayerShare = 1.0f / 256.0f;
const float kLineShare = 1.0f / 128.0f;

} // namespace

void buildDepthPullMatrix(const Vec3& eye, Primitive primitive, int32_t layer, float out[16]) {
    if (!out) {
        return;
    }

    const float share = (float)layer * kLayerShare +
                        (primitive == Primitive::Lines ? kLineShare : 0.0f);
    if (share == 0.0f) {
        memset(out, 0, sizeof(float) * 16);
        out[0] = 1.0f;
        out[5] = 1.0f;
        out[10] = 1.0f;
        out[15] = 1.0f;
        return;
    }

    // System.Numerics.Matrix4x4.CreateScale(scale, centre) in the oracle's
    // row-vector layout:
    //
    //     p' = (p - centre) * scale + centre
    //
    // The translation is centre * (1 - scale). The oracle's SIMD path uses
    // this product, not centre - centre * scale; the two differ by an ULP
    // and the oracle's form is the reference.
    const float scale = 1.0f - share;
    const float offset = 1.0f - scale;
    memset(out, 0, sizeof(float) * 16);
    out[0] = scale;
    out[5] = scale;
    out[10] = scale;
    out[12] = eye.x * offset;
    out[13] = eye.y * offset;
    out[14] = eye.z * offset;
    out[15] = 1.0f;
}

} // namespace mdk
