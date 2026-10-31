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
