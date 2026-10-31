#pragma once

// ============================================================================
// matrix.h — a 4x4 float matrix for the game layer.
//
// The C# oracle uses System.Numerics.Matrix4x4 throughout the object model
// (the rolling basis, the object transform). The renderer keeps its own matrix
// for upload; the game layer needs a plain one it can store in an object and
// do its own arithmetic with, without depending on the renderer's upload types.
//
// Row-major, row-vector convention (a row vector v transforms as v * M), which
// matches the C#'s Matrix4x4 (System.Numerics is row-vector too). The pieces
// here are the ones the object model composes with: identity, the three
// builders the C#'s MdkObject.Transform uses, and the multiply that puts them
// together in the C#'s order.
//
// WHY THE MULTIPLY IS WRITTEN OUT. System.Numerics.Matrix4x4.Multiply computes
// each element as one rounded product followed by three fused multiply-adds,
// because the JIT contracts `a*b + c*d + e*f + g*h` into FMA on hardware that
// has it. Writing the same expression here lets the compiler contract however
// IT likes, which lands on a different last bit -- and the object transform
// feeds vertices. std::fma is spelled out so both targets land where the
// oracle lands. This is the same choice MdkObject::rotation() already makes.
// ============================================================================

#include <cmath>
#include <cstring>

namespace mdk {

struct Mat4 {
    // Row-major: m[row * 4 + column].
    float m[16];

    // The identity matrix (the C#'s Matrix4x4.Identity).
    static constexpr Mat4 identity()
    {
        Mat4 r{};
        for (int i = 0; i < 16; ++i) {
            r.m[i] = 0.0f;
        }
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }
};

// left * right, the C#'s `left * right`.
inline Mat4 multiply(const Mat4& left, const Mat4& right)
{
    Mat4 product{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            float value = left.m[row * 4] * right.m[column];
            value = std::fma(left.m[row * 4 + 1], right.m[4 + column], value);
            value = std::fma(left.m[row * 4 + 2], right.m[8 + column], value);
            value = std::fma(left.m[row * 4 + 3], right.m[12 + column], value);
            product.m[row * 4 + column] = value;
        }
    }
    return product;
}

// Matrix4x4.CreateFromAxisAngle(axis, angle): the rotation of `angle` RADIANS
// about the axis (x,y,z), in the row-vector form System.Numerics builds (the
// transpose of the column-vector textbook matrix: CreateFromAxisAngle(0,0,1,t)
// is the same matrix as create_rotation_z above).
//
// THE AXIS IS NOT NORMALIZED, and that is the oracle's behaviour too: measured
// against .NET 10, CreateFromAxisAngle(0,3,0,0.5) is a SKEWED matrix there as
// well (its M13 is 3*sin(0.5)). The rolling code passes an axis it divided by
// its own length, so it is unit length up to rounding; normalizing here would
// be a difference from the oracle, not a fix.
//
// BIT-EXACT, unlike the element-wise axis-angle formula. .NET does not build
// this matrix from sin/cos of the angle: it builds a quaternion (sin/cos of the
// HALF angle) and converts that, and the extra roundings are visible -- the
// element-wise spelling `1 - (yy+zz)*(1-cos)` differs from .NET by 2-8 ULP on
// every element, which is enough to move the rolling basis at the sixth decimal
// after a dozen ticks. The quaternion spelling below reproduces all nine
// elements bit for bit against .NET 10 on the rolling axes and on non-unit,
// negative-angle and large-angle inputs. The element signs are the quaternion
// conversion's (M12 = xy+wz, M13 = xz-wy), which are the transpose of the
// textbook axis-angle matrix's, because Matrix4x4.CreateFromQuaternion is
// written for row vectors like everything else in System.Numerics.
//
// sinf/cosf are load-bearing, not presentation: MathF.Sin/Cos(float) agree with
// them bit for bit on these inputs (probed), and fastSin/fastCos would not.
inline Mat4 create_from_axis_angle(float x, float y, float z, float angle)
{
    /* @vfp-exempt R6 — the rolling basis is gameplay (it decides where a rolling
     * model is drawn and what its transform reads), and .NET builds this matrix
     * from MathF.Sin/Cos of the HALF angle; the fastSin/fastCos LUT is not
     * bit-identical to them, so routing this through the LUT would move the
     * basis. The pin is on the trig, not on the matrix: the element arithmetic
     * below is bit-exact against .NET 10 as measured. measured n/a. Verified
     * 2026-10-31. */
    const float half = angle * 0.5f;
    const float s = sinf(half);
    const float c = cosf(half);
    const float qx = x * s;
    const float qy = y * s;
    const float qz = z * s;
    const float qw = c;
    const float x2 = qx + qx;
    const float y2 = qy + qy;
    const float z2 = qz + qz;
    const float xx = qx * x2;
    const float xy = qx * y2;
    const float xz = qx * z2;
    const float yy = qy * y2;
    const float yz = qy * z2;
    const float zz = qz * z2;
    const float wx = qw * x2;
    const float wy = qw * y2;
    const float wz = qw * z2;

    Mat4 r = Mat4::identity();
    r.m[0] = 1.0f - (yy + zz);
    r.m[1] = xy + wz;
    r.m[2] = xz - wy;
    r.m[4] = xy - wz;
    r.m[5] = 1.0f - (xx + zz);
    r.m[6] = yz + wx;
    r.m[8] = xz + wy;
    r.m[9] = yz - wx;
    r.m[10] = 1.0f - (xx + yy);
    return r;
}

// The C#'s ObjectMotion.Orthonormal: make a rotation's rows unit and mutually
// perpendicular again (Gram-Schmidt), because float products drift and a long
// rolling model would skew and scale. The rows are x, y and z, in that order,
// and z is the cross product of the first two -- which is why the result has
// no scale left in it.
//
// Vector3.Normalize DIVIDES by the length (it does not multiply by a
// reciprocal), and Vector3.Dot is the plain three-term sum; both are spelled
// out here because the last bit of a long roll depends on it.
inline void normalize3(float* v)
{
    const float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
}

inline Mat4 orthonormal(const Mat4& m)
{
    float x[3] = {m.m[0], m.m[1], m.m[2]};
    float y[3] = {m.m[4], m.m[5], m.m[6]};
    normalize3(x);
    const float d = y[0] * x[0] + y[1] * x[1] + y[2] * x[2];
    y[0] -= d * x[0];
    y[1] -= d * x[1];
    y[2] -= d * x[2];
    normalize3(y);

    // Vector3.Cross(x, y).
    const float z0 = x[1] * y[2] - x[2] * y[1];
    const float z1 = x[2] * y[0] - x[0] * y[2];
    const float z2 = x[0] * y[1] - x[1] * y[0];

    Mat4 r = Mat4::identity();
    r.m[0] = x[0];
    r.m[1] = x[1];
    r.m[2] = x[2];
    r.m[4] = y[0];
    r.m[5] = y[1];
    r.m[6] = y[2];
    r.m[8] = z0;
    r.m[9] = z1;
    r.m[10] = z2;
    return r;
}

// Matrix4x4.CreateScale(float): the same scale on all three axes.
inline Mat4 create_scale(float scale)
{
    Mat4 r = Mat4::identity();
    r.m[0] = scale;
    r.m[5] = scale;
    r.m[10] = scale;
    return r;
}

// Matrix4x4.CreateTranslation(Vector3): the translation is in the FOURTH ROW,
// not the fourth column, because the convention is row vectors (v * M).
inline Mat4 create_translation(float x, float y, float z)
{
    Mat4 r = Mat4::identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}

} // namespace mdk
