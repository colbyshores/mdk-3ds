#pragma once

// ============================================================================
// part_fit.h -- how a model part moves from its rest pose to a pose, as
// one affine transform, ported 1:1 from src/Mdk.Game/Mods/PartFit.cs.
//
// The least-squares fit of the posed points to the rest ones. MDK
// animates parts by moving their vertices (rigid matrix tracks, or
// per-vertex deltas); a replaced part (a mod's mesh) follows the
// original part through this fit. Rigid motion is fitted exactly;
// deformation approximately. The points are the part's vertices and,
// per triangle, a point off its plane (so flat parts fit).
//
//   rest p, posed q (centred):  A = (sum q pT)(sum p pT)^-1,  t = q - A p
//
// Degenerate parts (a point, a line) only move. Matrices are the
// engine's row-major, row-vector convention throughout.
// ============================================================================

#include <stdint.h>

#include <vector>

#include "render/render_types.h"

namespace mdk {

class PartFit {
public:
    PartFit() = default;
    PartFit(const std::vector<Vec3>& vertices, const std::vector<int32_t>& triangles);

    // The transform taking the rest pose to `posed` (the part's vertices
    // in a pose), as 16 row-major floats. Identity for the rest pose
    // itself and for a part with no points.
    void fit(const std::vector<Vec3>& posed, float out[16]) const;

private:
    std::vector<int32_t> triangles_;
    std::vector<Vec3> vertices_;
    int32_t vertexCount_ = 0;
    Vec3 restCentre_{};
    // The rest points, centred (vertices, then triangle apexes).
    std::vector<Vec3> rest_;
    // (sum p pT)^-1 in the 3x3 corner; degenerate when inverse_[0] is NaN-free
    // but hasInverse_ is false.
    float inverse_[9] = {};
    bool hasInverse_ = false;
    // The posed points (scratch).
    mutable std::vector<Vec3> posed_;
};

} // namespace mdk
