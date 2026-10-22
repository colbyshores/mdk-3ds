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
// matches the C#'s Matrix4x4 (System.Numerics is row-vector too). Only the
// pieces the object model needs now are here: storage and identity. The
// rotation/multiply helpers land with the motion slice that needs them.
// ============================================================================

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

} // namespace mdk
