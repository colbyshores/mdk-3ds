// ============================================================================
// tube_basis.cpp — C# src/Mdk.Game/Stream/TubeBasis.cs, and the System.Numerics
// operations the stream calls (see tube_basis.h for the measured rounding).
// ============================================================================

#include "stream/tube_basis.h"

#include <cmath>

namespace mdk {
namespace stream {

float nDot(Vec3 a, Vec3 b)
{
    // Vector3.Dot: (x*x' + y*y') + z*z', no fused step.
    float sum = a.x * b.x;
    sum = sum + a.y * b.y;
    return sum + a.z * b.z;
}

Vec3 nCross(Vec3 a, Vec3 b)
{
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float nLength(Vec3 v) { return std::sqrt(nDot(v, v)); }

Vec3 nNormalize(Vec3 v)
{
    // Vector3.Normalize: value / value.Length(), three divides.
    const float length = nLength(v);
    return Vec3{v.x / length, v.y / length, v.z / length};
}

Vec3 nLerp(Vec3 a, Vec3 b, float t)
{
    // Vector3.Lerp on the oracle's host: MultiplyAddEstimate(a, 1 - t, b * t),
    // a fused multiply-add (tube_basis.h).
    const float keep = 1.0f - t;
    return Vec3{std::fma(a.x, keep, b.x * t), std::fma(a.y, keep, b.y * t), std::fma(a.z, keep, b.z * t)};
}

float nDistance(Vec3 a, Vec3 b) { return nLength(nSub(a, b)); }

float nLength2(float x, float y)
{
    float sum = x * x;
    sum = sum + y * y;
    return std::sqrt(sum);
}

float degreesToRadians(float degrees)
{
    // float.DegreesToRadians: (degrees * float.Pi) / 180.
    const float kPi = 3.14159265358979323846f;
    return (degrees * kPi) / 180.0f;
}

Vec3 basisApply(const TubeBasis& b, Vec3 v)
{
    // X * v.X + Y * v.Y + Z * v.Z: three vector products, summed left to right.
    return nAdd(nAdd(nScale(b.x, v.x), nScale(b.y, v.y)), nScale(b.z, v.z));
}

TubeBasis basisMultiply(const TubeBasis& a, const TubeBasis& b)
{
    TubeBasis r;
    r.x = basisApply(a, b.x);
    r.y = basisApply(a, b.y);
    r.z = basisApply(a, b.z);
    return r;
}

namespace {

// The C#'s local Turn: v cos + (axis x v) sin + axis (axis . v)(1 - cos).
Vec3 turn(Vec3 axis, Vec3 v, float sine, float cosine)
{
    const Vec3 a = nAdd(nScale(v, cosine), nScale(nCross(axis, v), sine));
    return nAdd(a, nScale(axis, nDot(axis, v) * (1.0f - cosine)));
}

} // namespace

TubeBasis basisRotation(Vec3 axis, float degrees)
{
    /* @vfp-exempt R6 — the stream's frames: three rotations per new ring (once per
     * segment) and two per drawn thing; glibc sinf/cosf match the C# oracle's
     * MathF.SinCos bit for bit on this machine (20000 cases, tube_basis.h), and the
     * tube is a random walk whose every later ring carries this result, so the LUT
     * would move every ring the stream goldens pin. measured n/a (corpus empty).
     * Verified 2026-11-20. */
    const float angle = degreesToRadians(degrees);
    const float sine = std::sin(angle);
    const float cosine = std::cos(angle);
    TubeBasis r;
    r.x = turn(axis, Vec3{1.0f, 0.0f, 0.0f}, sine, cosine);
    r.y = turn(axis, Vec3{0.0f, 1.0f, 0.0f}, sine, cosine);
    r.z = turn(axis, Vec3{0.0f, 0.0f, 1.0f}, sine, cosine);
    return r;
}

Mat4 basisAt(const TubeBasis& b, Vec3 origin)
{
    Mat4 m = Mat4::identity();
    m.m[0] = b.x.x;
    m.m[1] = b.x.y;
    m.m[2] = b.x.z;
    m.m[4] = b.y.x;
    m.m[5] = b.y.y;
    m.m[6] = b.y.z;
    m.m[8] = b.z.x;
    m.m[9] = b.z.y;
    m.m[10] = b.z.z;
    m.m[12] = origin.x;
    m.m[13] = origin.y;
    m.m[14] = origin.z;
    return m;
}

} // namespace stream
} // namespace mdk
