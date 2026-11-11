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

void canvasMatrix(float out[16], float canvasWidth) {
    // The C#'s CreateOrthographicOffCenter(0, W, H, 0, 0, 1) in the engine's
    // row-major, row-vector layout: the translation in row 3, and the y scale
    // NEGATIVE because the canvas's y runs down while the landscape clip's runs
    // up (the C# gets the same sign from passing bottom and top swapped).
    float m[16] = {0};
    m[0] = 2.0f / canvasWidth;
    m[5] = -2.0f / (float)kCanvasHeight;
    // THE DEPTH, and it is not zero. `eyeViewProjection` step (A) records that
    // this GPU's clip z runs [-w, 0] rather than the GL [-w, w]: the scene's own
    // matrices bake `clip.z -= clip.w` for exactly that reason. A canvas flat at
    // z = 0 therefore sits precisely on the far clip plane, which is the one place
    // a rasteriser is entitled to reject the whole triangle. Overlay tests no
    // depth, so a value in the middle of the range costs nothing and removes the
    // hazard.
    //
    // IT IS NOT THE REASON THE CANVAS IS INVISIBLE, AND THAT IS MEASURED
    // (2026-10-29, docs/PROGRESS_LOG.md): with z at -0.5 the HUD is still absent
    // from the capture. It stays because sitting on a clip plane is wrong whether
    // or not it is the current symptom, not because it fixed anything.
    m[10] = 0.0f;
    m[14] = -0.5f;
    m[12] = -1.0f;
    m[13] = 1.0f;
    m[15] = 1.0f;

    // The panel's quarter turn, the same column operation as step (B) of
    // `eyeViewProjection` above, for the same reason: the canvas is drawn over
    // the scene into the same portrait target.
    for (int k = 0; k < 4; k++) {
        out[k * 4 + 0] = m[k * 4 + 1];
        out[k * 4 + 1] = -m[k * 4 + 0];
        out[k * 4 + 2] = m[k * 4 + 2];
        out[k * 4 + 3] = m[k * 4 + 3];
    }
}

DepthState depthStateForPass(Pass pass) {
    // One line per pass, each readable against the oracle line it comes from
    // (Renderer.cs:565,606-607). Nothing here is inferred: a pass that is
    // neither blended nor exempt is an ordinary opaque surface.
    switch (pass) {
    case Pass::Blended:
        // Tested but never written. Glass occludes nothing, and it is sorted
        // back to front precisely because it cannot resolve against itself.
        // This is the pass whose absence the frame cannot show: with no depth
        // test at all the picture looks the same, which is exactly why the rule
        // is pinned here rather than left to a screenshot.
        return {true, false};
    case Pass::OnTop:
    case Pass::Overlay:
        // Neither tested nor written: these draw over the scene, and the oracle
        // exempts both (Renderer.cs:606-607).
        return {false, false};
    case Pass::Solid:
    case Pass::DoubleSided:
    case Pass::Mirror:
        break;
    case Pass::Count:
        // The cardinality sentinel, never a real pass. It is named here so this
        // switch stays exhaustive WITHOUT a `default`, which is what makes
        // `-Wall`'s `-Wswitch` fire in both builds when a seventh pass is added
        // and not given its own line above (review-4 F1: the guard that was
        // supposed to catch that compared an array's length with a literal 6,
        // so it could not fail).
        break;
    }
    return {true, true};
}

BlendState blendStateForPass(Pass pass, Blend blend) {
    // The oracle's one gate (Renderer.cs:565): `pass is Pass.Blended or
    // Pass.Overlay`. Everything else in the block is reached with the same
    // factors for every other pass, so the opaque case is one line and not
    // four.
    const bool blended = pass == Pass::Blended || pass == Pass::Overlay;
    if (!blended) {
        // enable_blend = false (Renderer.cs:570). SDL disables blending; the
        // PICA has no disable, so the equivalent is the replace pair, written
        // for the alpha channel too -- the oracle's alpha factors are only
        // read when blending is on, and with them left at SRC_ALPHA a Solid
        // draw would still mix the alpha channel with the target's.
        return kBlendReplace;
    }

    // Renderer.cs:571-576, factor by factor. The destination colour factor is
    // the only thing `blend` changes: Add is the oracle's additive mode, and
    // everything else -- which today is every live blended surface, the glass
    // included -- is the ordinary over.
    BlendState s;
    s.enable = true;
    s.srcColour = kBlendSrcAlpha;
    s.dstColour = blend == Blend::Add ? kBlendOne : kBlendOneMinusSrcAlpha;
    s.srcAlpha = kBlendOne;
    s.dstAlpha = kBlendOneMinusSrcAlpha;
    return s;
}

bool alphaTestPasses(int alphaByte) {
    // The predicate the hardware is INSTALLED with, not the predicate the
    // hardware holds: the backend writes a literal
    // `C3D_AlphaTest(true, GPU_GREATER, kAlphaTestReference)` and never calls
    // this (review-4 F5). Only the reference is shared. What holds the SPELLING
    // at the call site is the frame gate -- `tests/scripts/c3d_frame_test.sh`'s
    // `alpha-test-is-the-oracles-discard`, `alpha-test-reference-moved` and
    // `alpha-test-disabled` arms each go red if it drifts -- and what holds this
    // predicate is `PASS PicaMatrix`. The two agree because both are pinned to
    // the same oracle line, not because one calls the other; a build that made
    // the renderer consume this function would make the claim true instead of
    // documented, and would have to keep the frame gate's literal in step.
    return alphaByte > kAlphaTestReference;
}

float storedDepth(float zNdc) {
    // The fixed-function transform: `z*zScale + zOffset`, with the two constants
    // the backend hands to C3D_DepthMap. Plain float arithmetic, no promotion:
    // both constants are float, so no double reaches this expression on either
    // target (AGENTS.md R4 does not apply to a cold path anyway, but there is no
    // reason to invite one).
    return zNdc * kDepthMapScale + kDepthMapOffset;
}

bool depthPasses(const DepthState& state, float zNdc, float stored) {
    if (!state.test) {
        return true;
    }
    // GPU_LEQUAL. See the note on the equality at the declaration.
    return storedDepth(zNdc) <= stored;
}

EyePlan eyePlan(float slider, float separation) {
    // Clamp first, before anything is multiplied: see the declaration. A NaN
    // fails both comparisons and passes through, which is what a NaN should do
    // here -- it is not a value this function can rescue, and the two comparisons
    // are cheaper than a NaN test on this target.
    float depth = slider;
    if (depth < 0.0f) depth = 0.0f;
    if (depth > 1.0f) depth = 1.0f;

    EyePlan plan;
    plan.offset[0] = 0.0f;
    plan.offset[1] = 0.0f;
    if (depth <= 0.0f) {
        // Mono. The panel shows the left buffer, and the left eye of a mono
        // frame is the camera itself: no offset, one pass.
        plan.passes = 1;
        return plan;
    }
    // Stereo: the camera splits into two eyes half the separation each way along
    // the camera's right vector, which is the sign `eyeViewProjection` expects.
    plan.passes = 2;
    const float half = depth * separation * 0.5f;
    plan.offset[0] = -half;
    plan.offset[1] = half;
    return plan;
}

} // namespace pica
} // namespace mdk
