// ============================================================================
// part_fit.cpp -- see part_fit.h. The 3x3 inverse is the adjugate over
// the determinant; the C# goes through Matrix4x4.Invert on the same
// covariance with the same degeneracy gate, so the fitted transforms
// agree wherever both are defined.
// ============================================================================

#include "game/part_fit.h"

#include <cmath>

namespace mdk {

namespace {

const float kDegenerate = 1e-6f;
static constexpr int32_t kTriangleCorners = 3;

Vec3 sub(const Vec3& a, const Vec3& b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 add(const Vec3& a, const Vec3& b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 scale(const Vec3& a, float s) { return Vec3{a.x * s, a.y * s, a.z * s}; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length(const Vec3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

// The vertices, then each triangle's apex (centroid + normal/|normal|).
void points(const std::vector<Vec3>& vertices, const std::vector<int32_t>& triangles,
            std::vector<Vec3>* out) {
    out->assign(vertices.begin(), vertices.end());
    const int32_t count = (int32_t)triangles.size() / kTriangleCorners;
    for (int32_t t = 0; t < count; t++) {
        const Vec3& a = vertices[(size_t)triangles[(size_t)t * kTriangleCorners]];
        const Vec3& b = vertices[(size_t)triangles[(size_t)t * kTriangleCorners + 1]];
        const Vec3& c = vertices[(size_t)triangles[(size_t)t * kTriangleCorners + 2]];
        const Vec3 n = cross(sub(b, a), sub(c, a));
        const float len = length(n);
        const Vec3 centroid = scale(add(add(a, b), c), 1.0f / (float)kTriangleCorners);
        out->push_back(len > 0.0f ? add(centroid, scale(n, 1.0f / std::sqrt(len))) : centroid);
    }
}

Vec3 centre(const std::vector<Vec3>& pts) {
    Vec3 sum{};
    for (size_t i = 0; i < pts.size(); i++) {
        sum = add(sum, pts[i]);
    }
    return pts.empty() ? sum : scale(sum, 1.0f / (float)pts.size());
}

// v * M for a row vector and a row-major 3x3 (in the 16-float matrix).
Vec3 transform(const Vec3& v, const float m[16]) {
    return Vec3{v.x * m[0] + v.y * m[4] + v.z * m[8] + m[12],
                v.x * m[1] + v.y * m[5] + v.z * m[9] + m[13],
                v.x * m[2] + v.y * m[6] + v.z * m[10] + m[14]};
}

void identity(float out[16]) {
    for (int i = 0; i < 16; i++) {
        out[i] = 0.0f;
    }
    out[0] = out[5] = out[10] = out[15] = 1.0f;
}

} // namespace

PartFit::PartFit(const std::vector<Vec3>& vertices, const std::vector<int32_t>& triangles)
    : triangles_(triangles), vertices_(vertices), vertexCount_((int32_t)vertices.size()) {
    posed_.resize(vertices.size() + triangles.size() / kTriangleCorners);
    points(vertices, triangles, &rest_);
    restCentre_ = centre(rest_);
    for (size_t i = 0; i < rest_.size(); i++) {
        rest_[i] = sub(rest_[i], restCentre_);
    }

    // The covariance sum p pT in the 3x3 corner.
    float cov[9] = {};
    for (size_t i = 0; i < rest_.size(); i++) {
        const Vec3& p = rest_[i];
        const float pv[3] = {p.x, p.y, p.z};
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                cov[r * 3 + c] += pv[r] * pv[c];
            }
        }
    }
    const float scaleSum = cov[0] + cov[4] + cov[8];
    const float det = cov[0] * (cov[4] * cov[8] - cov[5] * cov[7]) -
                      cov[1] * (cov[3] * cov[8] - cov[5] * cov[6]) +
                      cov[2] * (cov[3] * cov[7] - cov[4] * cov[6]);
    if (!rest_.empty() && std::fabs(det) > kDegenerate * scaleSum * scaleSum * scaleSum) {
        const float inv = 1.0f / det;
        inverse_[0] = (cov[4] * cov[8] - cov[5] * cov[7]) * inv;
        inverse_[1] = (cov[2] * cov[7] - cov[1] * cov[8]) * inv;
        inverse_[2] = (cov[1] * cov[5] - cov[2] * cov[4]) * inv;
        inverse_[3] = (cov[5] * cov[6] - cov[3] * cov[8]) * inv;
        inverse_[4] = (cov[0] * cov[8] - cov[2] * cov[6]) * inv;
        inverse_[5] = (cov[2] * cov[3] - cov[0] * cov[5]) * inv;
        inverse_[6] = (cov[3] * cov[7] - cov[4] * cov[6]) * inv;
        inverse_[7] = (cov[1] * cov[6] - cov[0] * cov[7]) * inv;
        inverse_[8] = (cov[0] * cov[4] - cov[1] * cov[3]) * inv;
        hasInverse_ = true;
    }
}

void PartFit::fit(const std::vector<Vec3>& posed, float out[16]) const {
    identity(out);
    if (rest_.empty() || (int32_t)posed.size() != vertexCount_) {
        return;
    }
    bool same = posed.size() == vertices_.size();
    if (same) {
        for (size_t i = 0; i < posed.size(); i++) {
            if (posed[i].x != vertices_[i].x || posed[i].y != vertices_[i].y ||
                posed[i].z != vertices_[i].z) {
                same = false;
                break;
            }
        }
    }
    if (same) {
        return;  // the rest pose itself (the C#'s posed == _vertices)
    }

    points(posed, triangles_, &posed_);
    const Vec3 c = centre(posed_);
    if (!hasInverse_) {
        const Vec3 t = sub(c, restCentre_);
        out[12] = t.x;
        out[13] = t.y;
        out[14] = t.z;
        return;
    }

    // cross = sum p qT (p rest-centred, q posed-centred), then
    // linear = inverse * cross in row-major multiplication.
    float crossM[9] = {};
    for (size_t i = 0; i < rest_.size(); i++) {
        const Vec3 p = rest_[i];
        const Vec3 q = sub(posed_[i], c);
        const float pv[3] = {p.x, p.y, p.z};
        const float qv[3] = {q.x, q.y, q.z};
        for (int r = 0; r < 3; r++) {
            for (int col = 0; col < 3; col++) {
                crossM[r * 3 + col] += pv[r] * qv[col];
            }
        }
    }
    float linear[9] = {};
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            float sum = 0.0f;
            for (int k = 0; k < 3; k++) {
                sum += inverse_[r * 3 + k] * crossM[k * 3 + col];
            }
            linear[r * 3 + col] = sum;
        }
    }
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            out[r * 4 + col] = linear[r * 3 + col];
        }
    }
    float withLinear[16];
    identity(withLinear);
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            withLinear[r * 4 + col] = linear[r * 3 + col];
        }
    }
    const Vec3 moved = transform(restCentre_, withLinear);
    out[12] = c.x - moved.x;
    out[13] = c.y - moved.y;
    out[14] = c.z - moved.z;
}

} // namespace mdk
