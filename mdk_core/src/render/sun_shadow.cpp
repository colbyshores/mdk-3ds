// sun_shadow.cpp -- see sun_shadow.h. The arithmetic is the C#'s own
// (cold path: once per enhanced frame at most).

#include "render/sun_shadow.h"

#include <cmath>

namespace mdk {
namespace {

Vec3 normalize(const Vec3& v) {
    // Vector3.Normalize over Vector3.Length: the runtime's dot is
    // fmaf(z*z, fmaf(y*y, x*x)) on this host's .NET, as the repo
    // measured for Vector3.Lerp (docs/OPEN_ITEMS.md).
    const float len2 = std::fmaf(v.z, v.z, std::fmaf(v.y, v.y, v.x * v.x));
    const float len = std::sqrt(len2);
    if (len <= 0.0f) {
        return Vec3{0.0f, 0.0f, 1.0f};
    }
    return Vec3{v.x / len, v.y / len, v.z / len};
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// out = a * b in the engine's row-major row-vector convention (apply a,
// then b), the System.Numerics Matrix4x4 multiply.
void multiply(const float a[16], const float b[16], float out[16]) {
    float result[16];
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            result[row * 4 + col] = a[row * 4 + 0] * b[0 * 4 + col] +
                                    a[row * 4 + 1] * b[1 * 4 + col] +
                                    a[row * 4 + 2] * b[2 * 4 + col] +
                                    a[row * 4 + 3] * b[3 * 4 + col];
        }
    }
    for (int i = 0; i < 16; i++) {
        out[i] = result[i];
    }
}

} // namespace

void sunShadowMatrix(const Vec3& sunDirection, const Vec3& centre, float radius,
                     uint32_t size, float out[16]) {
    const Vec3 direction = normalize(sunDirection);
    const Vec3 up = std::fabs(direction.z) > 0.99f ? Vec3{0.0f, 1.0f, 0.0f}
                                                   : Vec3{0.0f, 0.0f, 1.0f};

    // CreateLookAt(Zero, direction, up), as the core camera builder
    // spells it (the axes are the view matrix's columns; no translation).
    const Vec3 zaxis = normalize(Vec3{-direction.x, -direction.y, -direction.z});
    const Vec3 xaxis = normalize(cross(up, zaxis));
    const Vec3 yaxis = cross(zaxis, xaxis);
    float view[16];
    view[0] = xaxis.x;  view[1] = yaxis.x;  view[2] = zaxis.x;  view[3] = 0.0f;
    view[4] = xaxis.y;  view[5] = yaxis.y;  view[6] = zaxis.y;  view[7] = 0.0f;
    view[8] = xaxis.z;  view[9] = yaxis.z;  view[10] = zaxis.z; view[11] = 0.0f;
    view[12] = 0.0f;    view[13] = 0.0f;    view[14] = 0.0f;    view[15] = 1.0f;

    // The centre in light space, on whole texels (MathF.Round is
    // round-half-even; nearbyint under the default rounding mode is too).
    const float texel = 2.0f * radius / (float)size;
    const Vec3 at{centre.x * view[0] + centre.y * view[4] + centre.z * view[8] + view[12],
                  centre.x * view[1] + centre.y * view[5] + centre.z * view[9] + view[13],
                  centre.x * view[2] + centre.y * view[6] + centre.z * view[10] + view[14]};
    const float x = std::nearbyint(at.x / texel) * texel;
    const float y = std::nearbyint(at.y / texel) * texel;

    // CreateOrthographicOffCenter(x-r, x+r, y-r, y+r, -at.Z-Reach, -at.Z+Reach).
    const float zn = -at.z - kSunShadowReach;
    const float zf = -at.z + kSunShadowReach;
    float projection[16] = {};
    projection[0] = 2.0f / (2.0f * radius);
    projection[5] = 2.0f / (2.0f * radius);
    projection[10] = 1.0f / (zn - zf);
    projection[12] = (x - radius + x + radius) / (x - radius - (x + radius));
    projection[13] = (y - radius + y + radius) / (y - radius - (y + radius));
    projection[14] = zn / (zn - zf);
    projection[15] = 1.0f;

    multiply(view, projection, out);
}

} // namespace mdk
