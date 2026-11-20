#pragma once

// ============================================================================
// tube_basis.h — the stream's 3x3 frames and its System.Numerics arithmetic
// (C# src/Mdk.Game/Stream/TubeBasis.cs).
//
// A TubeBasis is a basis by its columns, in MDK coordinates: x right, y
// forward, z up (as Godot's Basis, which the stream's reference used).
//
// THE ARITHMETIC IS THE ORACLE'S, OPERATION FOR OPERATION. The stream's tube is
// a random walk: every ring is turned from the one before it, so one ULP in a
// rotation is carried into every later ring, and the goldens print IEEE bits.
// The helpers below are the System.Numerics operations the C# calls, each with
// the exact rounding the .NET 10 oracle was measured to have on its host
// (200000 random cases each, 2026-11-20, linux-x64 with FMA):
//
//   Vector3.Dot        (x*x' + y*y') + z*z'                plain, left to right
//   Vector3.Cross      y*z' - z*y', ...                    plain (NOT fused)
//   Vector3.Normalize  v / sqrt(Dot(v, v))                 a divide, not * 1/len
//   Vector3.Lerp       fma(a, 1 - t, b * t)                FUSED (MultiplyAddEstimate)
//   Vector3.Distance   sqrt(Dot(a - b, a - b))
//   Vector2.Length     sqrt(x*x + y*y)
//   MathF.SinCos       sinf, cosf (the C library's, bit-equal on glibc)
//   DegreesToRadians   (degrees * pi) / 180                float
//
// Lerp is the one that bites: .NET lowers it to a fused multiply-add on a host
// that has one, and 109111 of the 200000 cases differ from the unfused form.
// std::fma reproduces that exactly; on the 3DS (VFPv2 has no fused multiply-add)
// it is newlib's software fmaf, which is the same function at a few dozen cycles
// for the handful of Centre() calls a frame makes. util/matrix.h's multiply()
// uses std::fma for the same reason.
//
// Plain structs and free functions: no operators, no templates (AGENTS.md §10).
// ============================================================================

#include "formats/vector_types.h"
#include "util/matrix.h"

namespace mdk {
namespace stream {

using Vec3 = formats::Vec3f;
using Vec2 = formats::Vec2f;

// --- System.Numerics, as the oracle rounds it (see the header) -------------

inline Vec3 vec3(float x, float y, float z) { return Vec3{x, y, z}; }
inline Vec3 nAdd(Vec3 a, Vec3 b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 nSub(Vec3 a, Vec3 b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
// Vector3 * float and float * Vector3: the same three products.
inline Vec3 nScale(Vec3 a, float s) { return Vec3{a.x * s, a.y * s, a.z * s}; }
// Unary minus: -0 stays -0 (Rotation(-UnitY, ...) depends on it).
inline Vec3 nNeg(Vec3 a) { return Vec3{-a.x, -a.y, -a.z}; }
float nDot(Vec3 a, Vec3 b);
Vec3 nCross(Vec3 a, Vec3 b);
float nLength(Vec3 v);
Vec3 nNormalize(Vec3 v);
Vec3 nLerp(Vec3 a, Vec3 b, float t);
float nDistance(Vec3 a, Vec3 b);
float nLength2(float x, float y);
// float.DegreesToRadians.
float degreesToRadians(float degrees);

// --- TubeBasis ---------------------------------------------------------------

struct TubeBasis {
    Vec3 x{1.0f, 0.0f, 0.0f};
    Vec3 y{0.0f, 1.0f, 0.0f};
    Vec3 z{0.0f, 0.0f, 1.0f};
};

// TubeBasis.Identity.
inline TubeBasis basisIdentity() { return TubeBasis{}; }

// TubeBasis.Apply: X*v.x + Y*v.y + Z*v.z, summed left to right.
Vec3 basisApply(const TubeBasis& b, Vec3 v);

// TubeBasis operator *: (a.Apply(b.X), a.Apply(b.Y), a.Apply(b.Z)).
TubeBasis basisMultiply(const TubeBasis& a, const TubeBasis& b);

// TubeBasis.Rotation: by `degrees` about a unit axis, counter-clockwise seen
// from the axis's tip (Rodrigues, each column turned as the C#'s Turn does).
TubeBasis basisRotation(Vec3 axis, float degrees);

// TubeBasis.At: the model transform of the basis at `origin`, row vectors as
// System.Numerics.Matrix4x4 (rows X, Y, Z, origin; column 3 is 0, 0, 0, 1).
Mat4 basisAt(const TubeBasis& b, Vec3 origin);

} // namespace stream
} // namespace mdk
