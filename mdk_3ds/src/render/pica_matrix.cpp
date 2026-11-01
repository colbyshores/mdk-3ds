// ============================================================================
// pica_matrix.cpp — see pica_matrix.h.
//
// THE ARITHMETIC IS MOVED, NOT REWRITTEN. `eyeViewProjection` is the body
// `renderer_c3d.cpp` used to call `buildEyeMvp`, element for element and in the
// same order, so the frame's per-eye matrix is bit-identical across the split:
// the level geometry the 3DS draws today (every one of its draws carries an
// identity world) cannot move because of this file. The same plain triple-loop
// multiply is what the Vulkan backend uses (`renderer_vk.cpp`'s `mulMatrix`),
// so the two backends do the same arithmetic on the renderer's matrices.
//
// WHY NOT `mdk::multiply` from `util/matrix.h`? Because that one spells
// `std::fma` out on purpose — it is bit-exact against System.Numerics, which is
// what the OBJECT transform needs — and VFPv2 has no fused multiply-add, so on
// this target `std::fma` is a libm call. Four of them per element, sixteen
// elements, once per draw: a per-draw libm call is exactly what the
// `FrameBlend.Lerp` note in `docs/OPEN_ITEMS.md` warns about. The renderer's
// matrices are not pinned to .NET's last bit (the picture is pinned by the frame
// golden instead), so this multiply stays a plain multiply.
// ============================================================================

#include "render/pica_matrix.h"

#include <string.h>

namespace mdk {
namespace pica {

namespace {

// out = left * right, row-major, row-vector, plain float arithmetic.
//
// `out` must not alias either input; every caller here passes a local.
void multiply(const float left[16], const float right[16], float out[16]) {
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += left[row * 4 + k] * right[k * 4 + col];
            }
            out[row * 4 + col] = sum;
        }
    }
}

} // namespace

bool isIdentity(const float m[16]) {
    // The identity the seam builds: `MeshDraw`'s producers memset to zero and
    // then set elements 0, 5, 10 and 15, which is byte-for-byte this pattern.
    // memcmp is the exact comparison — no float compare, no -0.0 or NaN question,
    // and nothing to round.
    static const float kIdentity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    return memcmp(m, kIdentity, sizeof(kIdentity)) == 0;
}

void eyeViewProjection(const float view[16], const float projection[16],
                       float eyeOffset, float out[16]) {
    // The view matrix, with the eye's offset applied.
    //
    // The view is a row-major, row-vector look-at matrix: its translation row
    // is (-dot(right, eye), -dot(trueUp, eye), -dot(forward, eye)), and its
    // COLUMNS are the camera axes. Moving the eye by d along its own right axis
    // therefore changes ONLY the first translation component, by -d: the other
    // two axes are perpendicular to `right`, so their dot products are
    // unchanged. This is the C# oracle's View.Eye (Renderer.cs:92), which
    // rebuilds CreateLookAt at the shifted position; patching the row is the
    // algebraically identical shortcut. A former version scaled all three
    // translation components by the right axis, which moved the eyes along a
    // skewed direction for any camera that was not axis-aligned.
    float shifted[16];
    memcpy(shifted, view, sizeof(shifted));
    if (eyeOffset != 0.0f) {
        shifted[12] -= eyeOffset;
    }

    // The combined matrix: view * projection, in the row-vector convention.
    // A row vector p transforms as p' = p * V * P, so the combined matrix is
    // V * P with view as the LEFT operand -- the C# oracle's `view *
    // projection` (Renderer.cs:115, FollowCamera.cs, FallView.cs). The
    // product below is the ordinary row-times-column product of that.
    float m[16];
    multiply(shifted, projection, m);

    // (A) The depth remap: D3D [0,1] -> PICA [-1,0].
    //
    // In REGISTER space this is Forsaken's `c3d_vp.r[2] -= c3d_vp.r[3]`
    // (render_c3d.c:1122), i.e. clip.z -= clip.w. Register i holds COLUMN i
    // of the engine's row-vector matrix (see `registers`), so the register-row
    // operation is a COLUMN operation here: column 2 -= column 3. A row
    // operation would perturb clip.x/.y/.w and leave z/w untouched.
    for (int k = 0; k < 4; k++) {
        m[k * 4 + 2] -= m[k * 4 + 3];
    }

    // (B) The 90-degree screen rotation: the top screen is portrait-native,
    // the game is landscape. Forsaken's register recipe is r[0] = old r[1],
    // r[1] = -old r[0] (a proper rotation, determinant +1 -- citro3d's own
    // Mtx_PerspTilt bakes the same quarter turn as r[0].y / r[1].x = -).
    // Register rows are engine COLUMNS, so this is a column operation:
    // new column 0 = old column 1, new column 1 = -old column 0.
    float rot[16];
    for (int k = 0; k < 4; k++) {
        rot[k * 4 + 0] = m[k * 4 + 1];
        rot[k * 4 + 1] = -m[k * 4 + 0];
        rot[k * 4 + 2] = m[k * 4 + 2];
        rot[k * 4 + 3] = m[k * 4 + 3];
    }

    memcpy(out, rot, sizeof(rot));
}

void drawMatrix(const float world[16], const float eyeVp[16], float out[16]) {
    // The world on the LEFT: a row vector meets it first, so it places the
    // object and the eye matrix then puts the placed object in front of the
    // camera. The two PICA conversions are already inside `eyeVp`, and they are
    // right-multiplications by fixed matrices, so
    //     (W * V * P) * E * R  ==  W * (V * P * E * R)
    // and composing against the baked matrix is the same product, baked once per
    // frame instead of rebuilt per draw.
    multiply(world, eyeVp, out);
}

void registers(const float m[16], float out[16]) {
    for (int i = 0; i < 4; i++) {
        out[i * 4 + 0] = m[0 * 4 + i];
        out[i * 4 + 1] = m[1 * 4 + i];
        out[i * 4 + 2] = m[2 * 4 + i];
        out[i * 4 + 3] = m[3 * 4 + i];
    }
}

} // namespace pica
} // namespace mdk
