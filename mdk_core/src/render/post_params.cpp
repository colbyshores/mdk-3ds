// post_params.cpp -- see post_params.h. Cold path: once per enhanced
// frame. The multiply is the System.Numerics ordering (sun_shadow.cpp's);
// the inverse is Gauss-Jordan with partial pivoting in float -- the C#'s
// Matrix4x4.Invert is an adjugate expansion, so the last bits can differ
// from its; the post pass consumes the result through dot products and
// normalizes, where a float ULP is not a visible quantity. The matrix
// product itself is pinned bit-exact in tests/formats.

#include "render/post_params.h"

#include <cmath>
#include <string.h>

namespace mdk {
namespace {

Vec3 normalize(const Vec3& v) {
    // Vector3.Normalize over Vector3.Length: the runtime's dot is
    // fmaf(z*z, fmaf(y*y, x*x)) on this host's .NET, as the repo
    // measured for Vector3.Lerp (docs/OPEN_ITEMS.md).
    const float len2 = std::fmaf(v.z, v.z, std::fmaf(v.y, v.y, v.x * v.x));
    const float len = std::sqrt(len2);
    if (len <= 0.0f) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }
    return Vec3{v.x / len, v.y / len, v.z / len};
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// out = a * b in the engine's row-major row-vector convention (apply a,
// then b), the System.Numerics Matrix4x4 multiply. Each element is a dot
// of a row of a with a column of b, and the runtime's dot is an fmaf
// chain (fmaf(z, fmaf(y, x)) -- the shape the repo measured for
// Vector3.Lerp), so the chain is spelled the same way here; plain sums
// land one ULP off the oracle's product in the last place.
void multiply(const float a[16], const float b[16], float out[16]) {
    float result[16];
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            result[row * 4 + col] =
                std::fmaf(a[row * 4 + 3], b[3 * 4 + col],
                          std::fmaf(a[row * 4 + 2], b[2 * 4 + col],
                                    std::fmaf(a[row * 4 + 1], b[1 * 4 + col],
                                              a[row * 4 + 0] * b[0 * 4 + col])));
        }
    }
    for (int i = 0; i < 16; i++) {
        out[i] = result[i];
    }
}

// The inverse of a 4x4, by Gauss-Jordan elimination with partial
// pivoting. False when a pivot column is all zeros (singular).
bool invert(const float m[16], float out[16]) {
    float a[4][8];
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            a[row][col] = m[row * 4 + col];
            a[row][col + 4] = row == col ? 1.0f : 0.0f;
        }
    }
    for (int col = 0; col < 4; col++) {
        int pivot = col;
        float best = std::fabs(a[col][col]);
        for (int row = col + 1; row < 4; row++) {
            const float v = std::fabs(a[row][col]);
            if (v > best) {
                best = v;
                pivot = row;
            }
        }
        if (best == 0.0f) {
            return false;
        }
        if (pivot != col) {
            for (int k = 0; k < 8; k++) {
                const float t = a[col][k];
                a[col][k] = a[pivot][k];
                a[pivot][k] = t;
            }
        }
        const float scale = 1.0f / a[col][col];
        for (int k = 0; k < 8; k++) {
            a[col][k] *= scale;
        }
        for (int row = 0; row < 4; row++) {
            if (row == col) {
                continue;
            }
            const float factor = a[row][col];
            if (factor == 0.0f) {
                continue;
            }
            for (int k = 0; k < 8; k++) {
                a[row][k] -= factor * a[col][k];
            }
        }
    }
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            out[row * 4 + col] = a[row][col + 4];
        }
    }
    return true;
}

} // namespace

bool postClipMatrices(const Vec3& forward, const Vec3& up, const float projection[16],
                      float clipToView[16], float viewToClip[16]) {
    // CreateLookAt(Zero, forward, up), as the core camera builder spells
    // it (the axes are the matrix's columns; no translation).
    const Vec3 zaxis = normalize(Vec3{-forward.x, -forward.y, -forward.z});
    const Vec3 xaxis = normalize(cross(up, zaxis));
    const Vec3 yaxis = cross(zaxis, xaxis);
    float rotation[16];
    rotation[0] = xaxis.x;  rotation[1] = yaxis.x;  rotation[2] = zaxis.x;  rotation[3] = 0.0f;
    rotation[4] = xaxis.y;  rotation[5] = yaxis.y;  rotation[6] = zaxis.y;  rotation[7] = 0.0f;
    rotation[8] = xaxis.z;  rotation[9] = yaxis.z;  rotation[10] = zaxis.z; rotation[11] = 0.0f;
    rotation[12] = 0.0f;    rotation[13] = 0.0f;    rotation[14] = 0.0f;    rotation[15] = 1.0f;

    float product[16];
    multiply(rotation, projection, product);
    float inverse[16];
    if (!invert(product, inverse)) {
        return false;
    }
    memcpy(viewToClip, product, sizeof(product));
    memcpy(clipToView, inverse, sizeof(inverse));
    return true;
}

} // namespace mdk
