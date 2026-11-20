// fall_layout.cpp — see fall_layout.h. FallView.cs's statics and tables.

#include "fall/fall_layout.h"

#include <math.h>

#include "fall/fall_sim.h"
#include "util/math_lut.h"

namespace mdk {
namespace fall {

namespace {

using formats::Vec3f;

// FallView.HazeBlend: the blend of each haze table towards white (0x490d1c, / 256),
// tables 1-24.
constexpr int32_t kHazeBlend[kHazeTables] = {0,  0,  0,  0,  0,   0,   0,   0,
                                             3,  6,  12, 18, 24,  48,  72,  96,
                                             120, 144, 168, 192, 215, 230, 245, 255};
// FallView.RadarAlpha (approximated colours, / 256).
constexpr int32_t kRadarAlpha[kRadarColours] = {48, 64, 80, 96, 128};
constexpr float kBlendUnit = 256.0f;
// FallView.NearClearance.
constexpr float kNearClearance = 5.0f;
constexpr float kExplosionGrowth = 2.0f;
constexpr float kExplosionMinScale = 0.01f;
constexpr float kHalfPi = 1.57079632679489661923f;

Vec3f cross(const Vec3f& a, const Vec3f& b) {
    Vec3f r;
    r.x = a.y * b.z - a.z * b.y;
    r.y = a.z * b.x - a.x * b.z;
    r.z = a.x * b.y - a.y * b.x;
    return r;
}

float lengthSquared(const Vec3f& v) {
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

// Vector3.Normalize: the vector over its length.
Vec3f normalize(const Vec3f& v) {
    const float length = sqrtf(lengthSquared(v));
    Vec3f r;
    r.x = v.x / length;
    r.y = v.y / length;
    r.z = v.z / length;
    return r;
}

Mat4 rotationY(float radians) {
    // Presentation trig through the table (AGENTS.md R6: one path on both targets).
    const float c = fastCos(radians);
    const float s = fastSin(radians);
    Mat4 m = Mat4::identity();
    m.m[0] = c;
    m.m[2] = -s;
    m.m[8] = s;
    m.m[10] = c;
    return m;
}

} // namespace

float hazeAlpha(int32_t level, int32_t b) {
    int32_t table = level + b;
    table = table < 1 ? 1 : (table > kHazeTables ? kHazeTables : table);
    return (float)kHazeBlend[table - 1] / kBlendUnit;
}

float radarAlpha(int32_t band) {
    return (float)kRadarAlpha[band] / kBlendUnit;
}

bool nearCamera(const Vec3f& position, const Vec3f& camera) {
    return position.z > camera.z - kNearClearance;
}

Mat4 orient(const Vec3f& direction) {
    const Vec3f y = normalize(direction);
    Vec3f unitZ;
    unitZ.z = 1.0f;
    Vec3f x = cross(y, unitZ);
    if (lengthSquared(x) < 1e-12f) {
        x = Vec3f();
        x.x = 1.0f;
    } else {
        x = normalize(x);
    }
    const Vec3f z = cross(x, y);
    Mat4 m = Mat4::identity();
    m.m[0] = x.x;
    m.m[1] = x.y;
    m.m[2] = x.z;
    m.m[4] = y.x;
    m.m[5] = y.y;
    m.m[6] = y.z;
    m.m[8] = z.x;
    m.m[9] = z.y;
    m.m[10] = z.z;
    return m;
}

void radarRing(const Vec3f& basePoint, const Vec3f& spot, float radius, int32_t ring, Vec3f* centre,
               float* ringRadius) {
    // 1 − 2^−(ring+1): exact powers of two, as MathF.Pow(2, -k) is for these k.
    const float f = ring == kRadarRings - 1 ? 1.0f : 1.0f - ldexpf(1.0f, -(ring + 1));
    // Vector3.Lerp: value1 × (1 − amount) + value2 × amount, the add fused (.NET 10's
    // MultiplyAddEstimate on an FMA machine; measured against the probe).
    const float keep = 1.0f - f;
    centre->x = fmaf(basePoint.x, keep, spot.x * f);
    centre->y = fmaf(basePoint.y, keep, spot.y * f);
    centre->z = fmaf(basePoint.z, keep, spot.z * f);
    *ringRadius = radius * f;
}

Mat4 kurtTurn() {
    return multiply(rotationY(kHalfPi), rotationZ(kHalfPi));
}

Mat4 rotationZ(float radians) {
    // Presentation trig through the table (AGENTS.md R6: one path on both targets).
    const float c = fastCos(radians);
    const float s = fastSin(radians);
    Mat4 m = Mat4::identity();
    m.m[0] = c;
    m.m[1] = s;
    m.m[4] = -s;
    m.m[5] = c;
    return m;
}

Mat4 scaling(float scale) {
    Mat4 m = Mat4::identity();
    m.m[0] = scale;
    m.m[5] = scale;
    m.m[10] = scale;
    return m;
}

Mat4 translation(const Vec3f& at) {
    Mat4 m = Mat4::identity();
    m.m[12] = at.x;
    m.m[13] = at.y;
    m.m[14] = at.z;
    return m;
}

float degreesToRadians(float degrees) {
    return (degrees * 3.14159265358979323846f) / 180.0f;
}

float explosionScale(int32_t frame) {
    const float scale = kExplosionGrowth * (float)frame / (float)FallSim::kExplosionTicks;
    return scale > kExplosionMinScale ? scale : kExplosionMinScale;
}

} // namespace fall
} // namespace mdk
