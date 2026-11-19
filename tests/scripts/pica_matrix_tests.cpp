// ============================================================================
// pica_matrix_tests.cpp — see pica_matrix_tests.h.
//
// WHAT IS PINNED HERE, and why each one is worth a check:
//
//   1. IDENTITY RECOGNITION. The seam's "already in world space" marker, and the
//      fast path that keeps a frame of arena batches from paying a multiply and
//      a uniform write. Getting it wrong in the permissive direction silently
//      draws objects at the arena's origin; in the strict direction it only
//      costs a multiply.
//   2. THE IDENTITY WORLD CHANGES NOTHING. The level geometry the 3DS draws
//      today carries an identity world in every draw, so this is the statement
//      that the per-draw matrix cannot move the picture that exists now.
//   3. THE TWO PICA CONVERSIONS, as the recipe they are documented as: the
//      engine's clip point (x, y, z, w) becomes (y, -x, z-w, w). One check for
//      the depth remap and the quarter turn together, on inputs that are exact
//      in binary.
//   4. THE EYE OFFSET moves the picture, along one axis only.
//   5. A WORLD TRANSLATION places the point where the oracle's order puts it.
//   6. THE SIDE THE WORLD COMPOSES ON, cross-checked against rebuilding the
//      chain from a world-shifted view -- a different association of the same
//      product, so agreement is evidence and not a tautology. The reversed
//      order is checked to be materially DIFFERENT, or the pin would be vacuous.
//   7. THE UNIFORM REGISTER LAYOUT, applied the way the shader applies it
//      (`dp4 outpos.x, mvp[0], r0`), which is the only form of the layout that
//      matters.
//   8. THE DEPTH STATE: the oracle's per-pass rule, the frame's depth map, and
//      the ordering they imply. Every one of those is a bit of GPU state that
//      cannot be looked at on this host (no 3DS emulator, no Old-3DS, so
//      AGENTS.md P6 is open), which is the whole argument for pinning it here.
//      The far-plane tie is the sharpest of them: it distinguishes the
//      LESS_OR_EQUAL the oracle uses from the LESS that would reject it.
//   9. THE TEXEL TILE ORDER AND THE PATTERN THAT PROVED IT. The PICA reads an
//      8x8 Morton tile, so a row-major upload is sampled scrambled while
//      raising nothing; the fixture reproduces the measured "four times too
//      many bands" and the vertical-split-to-horizontal-bands permutation.
//
// The inputs are all small integers and halves, so every product and sum below
// is exact in binary and the comparisons are exact. Where a tolerance is used
// (case 6) it is because two association orders of one product are compared,
// not because the arithmetic is inexact.
// ============================================================================

#include <math.h>
#include "pica_matrix_tests.h"

#include <stdio.h>
#include <string.h>

#include "render/pica_matrix.h"
#include "render/sun_shadow.h"

namespace mdk {
namespace {

int failures = 0;

#define PICA_CHECK(cond)                                                        \
    do {                                                                         \
        if (!(cond)) {                                                           \
            printf("FAIL pica_matrix:%d: %s\n", __LINE__, #cond);                \
            failures++;                                                          \
        }                                                                        \
    } while (0)

// An independent row-vector product, written here so that a comparison against
// it is not a comparison against the code under test.
void mulIndependent(const float a[16], const float b[16], float out[16]) {
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a[row * 4 + k] * b[k * 4 + col];
            }
            out[row * 4 + col] = sum;
        }
    }
}

// The engine's row-vector transform of a point: out = p * m.
void xform(const float m[16], const float p[4], float out[4]) {
    for (int col = 0; col < 4; col++) {
        out[col] = p[0] * m[col] + p[1] * m[4 + col] + p[2] * m[8 + col] + p[3] * m[12 + col];
    }
}

// What the vertex shader computes for one component: register i of the uniform
// dotted with the position (`dp4 outpos.x, mvp[0], r0`).
//
// THE WORD ORDER INSIDE A REGISTER IS W, Z, Y, X, NOT X, Y, Z, W. `C3D_FVec`
// (c3d/types.h:37-64) declares `w, z, y, x` in that memory order, citro3d's own
// Mtx_* builders write to it the same way (`Mtx_Scale` multiplies offset 12 by
// its x argument; `Mtx_OrthoTilt` writes the y scale to offset 8), and
// `C3D_UpdateUniforms` copies the staging buffer to the GPU verbatim, so the
// LAST word of a register is the one the shader calls `.x` and the first is the
// one multiplied by 1. This helper is the only place that model is written down
// for the host suite, and it used to say the opposite -- which is why the row
// that reads the layout through it stayed green while the console painted a
// sunburst. See `pica_matrix.h`'s `registers` for the full evidence.
float shaderRow(const float reg[16], int i, const float p[4]) {
    return reg[i * 4 + 3] * p[0] + reg[i * 4 + 2] * p[1] + reg[i * 4 + 1] * p[2] +
           reg[i * 4 + 0] * p[3];
}

bool allEqual(const float* a, const float* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

bool allNear(const float* a, const float* b, int n, float tol) {
    for (int i = 0; i < n; i++) {
        const float d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
        if (!(d <= tol)) {
            return false;
        }
    }
    return true;
}

// The identity exactly as the seam's producers build it: zeroed, then the
// diagonal set (level_view.cpp, kurt_sprite.cpp).
const float kIdentity[16] = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1,
};

// A view with a real rotation in it (a quarter yaw) and a translation, all
// exact. Row-vector: the rows are the camera's right, up and forward axes and
// the translation row.
const float kView[16] = {
    0, 0, -1, 0,
    0, 1, 0, 0,
    1, 0, 0, 0,
    0.5f, -2, 3, 1,
};

// A projection with the oracle's shape: clip.w = -z, NDC z in [0,1]. Exact.
const float kProjection[16] = {
    2, 0, 0, 0,
    0, 4, 0, 0,
    0, 0, -1, -1,
    0, 0, -4, 0,
};

// A world transform: a quarter turn about z plus a translation. Exact.
const float kWorld[16] = {
    0, 1, 0, 0,
    -1, 0, 0, 0,
    0, 0, 1, 0,
    10, 2, -3, 1,
};

void testIdentityRecognition() {
    PICA_CHECK(mdk::pica::isIdentity(kIdentity));

    // A translation is not the identity, however small.
    const float shifted[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0.5f, 0, 0, 1,
    };
    PICA_CHECK(!mdk::pica::isIdentity(shifted));

    // MeshDraw's default `world` is ALL ZEROS, not the identity, and a zero
    // matrix is not "already in world space": it must get the multiply (and
    // collapse), exactly as the Vulkan backend already does with the same
    // record. Treating it as identity would hide a producer that never set one.
    const float zeros[16] = {0};
    PICA_CHECK(!mdk::pica::isIdentity(zeros));

    // -0.0 where the identity has +0.0 transforms identically but is not the
    // marker. The strict answer costs a multiply, which is the safe direction.
    float signedZero[16];
    memcpy(signedZero, kIdentity, sizeof(signedZero));
    signedZero[1] = -0.0f;
    PICA_CHECK(!mdk::pica::isIdentity(signedZero));
}

void testIdentityWorldChangesNothing() {
    // The claim that lets the backend skip the uniform write for the level
    // geometry: composing an identity world onto the frame's matrix reproduces
    // it BIT FOR BIT, for either eye offset.
    float eye[16];
    float out[16];

    mdk::pica::eyeViewProjection(kView, kProjection, 0.0f, eye);
    mdk::pica::drawMatrix(kIdentity, eye, out);
    PICA_CHECK(allEqual(out, eye, 16));

    mdk::pica::eyeViewProjection(kView, kProjection, 0.25f, eye);
    mdk::pica::drawMatrix(kIdentity, eye, out);
    PICA_CHECK(allEqual(out, eye, 16));
}

void testThePicaRecipe() {
    // With a mono identity view and projection the engine's clip point IS the
    // point, so the whole of the matrix's effect is the two conversions:
    // clip becomes (y, -x, z-w, w).
    float eye[16];
    mdk::pica::eyeViewProjection(kIdentity, kIdentity, 0.0f, eye);

    const float p[4] = {1, 2, 3, 1};
    float got[4];
    xform(eye, p, got);
    const float want[4] = {2, -1, 3 - 1, 1};
    PICA_CHECK(allEqual(got, want, 4));

    // A second point, to catch a swap of the two components that the quarter
    // turn exchanges.
    const float q[4] = {-4, 0.5f, 0, 1};
    float gotQ[4];
    xform(eye, q, gotQ);
    const float wantQ[4] = {0.5f, 4, 0 - 1, 1};
    PICA_CHECK(allEqual(gotQ, wantQ, 4));
}

void testTheEyeOffset() {
    // The offset moves the eye along its own right axis, so for an identity
    // view the origin lands at x = -offset in ENGINE clip space, which the
    // quarter turn then carries into the second component.
    float eye[16];
    mdk::pica::eyeViewProjection(kIdentity, kIdentity, 0.5f, eye);

    const float origin[4] = {0, 0, 0, 1};
    float got[4];
    xform(eye, origin, got);
    const float want[4] = {0, 0.5f, -1, 1};
    PICA_CHECK(allEqual(got, want, 4));

    // And it changes nothing else: with the offset at zero the same point gives
    // the unshifted answer.
    float mono[16];
    mdk::pica::eyeViewProjection(kIdentity, kIdentity, 0.0f, mono);
    float unshifted[4];
    xform(mono, origin, unshifted);
    const float wantMono[4] = {0, 0, -1, 1};
    PICA_CHECK(allEqual(unshifted, wantMono, 4));
}

void testAWorldTranslationPlacesThePoint() {
    // p = (1,2,3,1) through a translation of (10,2,-3) is (11,4,0,1), and the
    // recipe makes that (4, -11, -1, 1). Hand-derived, exact.
    float eye[16];
    mdk::pica::eyeViewProjection(kIdentity, kIdentity, 0.0f, eye);

    float world[16];
    memcpy(world, kIdentity, sizeof(world));
    world[12] = 10;
    world[13] = 2;
    world[14] = -3;

    float m[16];
    mdk::pica::drawMatrix(world, eye, m);

    const float p[4] = {1, 2, 3, 1};
    float got[4];
    xform(m, p, got);
    const float want[4] = {4, -11, -1, 1};
    PICA_CHECK(allEqual(got, want, 4));
}

void testTheWorldComposesOnTheLeftOfTheView() {
    float eye[16];
    mdk::pica::eyeViewProjection(kView, kProjection, 0.25f, eye);

    float composed[16];
    mdk::pica::drawMatrix(kWorld, eye, composed);

    // The same product, associated the other way: put the world in front of the
    // view and rebuild the chain. If the world were composed on the RIGHT of
    // the eye matrix this would not match, and the difference is not subtle.
    float worldView[16];
    mulIndependent(kWorld, kView, worldView);
    float rebuilt[16];
    mdk::pica::eyeViewProjection(worldView, kProjection, 0.25f, rebuilt);
    PICA_CHECK(allNear(composed, rebuilt, 16, 1e-4f));

    // The negative control: the reversed order must be materially different,
    // or the check above would pass on anything.
    float reversed[16];
    mulIndependent(eye, kWorld, reversed);
    bool differs = false;
    for (int i = 0; i < 16; i++) {
        const float d = composed[i] > reversed[i] ? composed[i] - reversed[i]
                                                 : reversed[i] - composed[i];
        if (d > 1.0f) {
            differs = true;
        }
    }
    PICA_CHECK(differs);
}

void testTheRegistersAreColumns() {
    // A matrix of distinct values, so a transposed upload cannot pass: register
    // i holds COLUMN i, i.e. elements i, 4+i, 8+i, 12+i -- stored W, Z, Y, X, so
    // the column's LAST element is the register's first word and its FIRST
    // element is the word the shader dots with x.
    float m[16];
    for (int i = 0; i < 16; i++) {
        m[i] = (float)i;
    }
    float reg[16];
    mdk::pica::registers(m, reg);
    const float want[16] = {
        12, 8, 4, 0,
        13, 9, 5, 1,
        14, 10, 6, 2,
        15, 11, 7, 3,
    };
    PICA_CHECK(allEqual(reg, want, 16));
}

void testTheShaderReadsTheRegistersTheWayTheMatrixMeansThem() {
    // The end of the chain: the layout applied as the shader applies it. The
    // engine's clip point (x, y, z, w) must come out of four dp4s as
    // (y, -x, z-w, w) -- the same answer `testThePicaRecipe` gets through the
    // matrix, reached the way the PICA200 actually reaches it.
    float eye[16];
    mdk::pica::eyeViewProjection(kIdentity, kIdentity, 0.0f, eye);
    float reg[16];
    mdk::pica::registers(eye, reg);

    const float p[4] = {1, 2, 3, 1};
    const float want[4] = {2, -1, 2, 1};
    for (int i = 0; i < 4; i++) {
        PICA_CHECK(shaderRow(reg, i, p) == want[i]);
    }
}

// ---------------------------------------------------------------------------
// THE CANVAS MATRIX
//
// The 2D canvas is drawn in its own pixel space through its own orthographic
// projection, and the one thing that projection has to agree with is the SCENE's
// quarter turn: the panel is portrait and both the world and the HUD are
// landscape, so if the two rotations differ the HUD lands rotated relative to
// the picture it is drawn over. On the host there is no panel to look at, so the
// agreement is pinned against the scene's own recipe instead.
// ---------------------------------------------------------------------------

// The C#'s canvas projection, expanded the way System.Numerics expands it, from
// the oracle's own arguments and nothing else: `CreateOrthographicOffCenter(
// -margin.X, CanvasWidth + margin.X, CanvasHeight + margin.Y, -margin.Y, 0, 1)`
// at Renderer.cs:736 with the VR margin at zero. Read against the signature
// `(left, right, bottom, top, near, far)` that means left = 0, right = W,
// BOTTOM = H and TOP = 0: the y arguments ARE swapped, and that swap is what
// makes the oracle y-UP. System.Numerics' expansion is
// `M22 = 2/(top-bottom) = 2/(0-H) = -2/H` and
// `M42 = (top+bottom)/(bottom-top) = H/H = +1`, so canvas y = 0 (its top row of
// pixels) goes to clip +1.
//
// THIS TARGET COPIES THAT, UNCHANGED, AND USED NOT TO SAY SO. An earlier version
// of this row expanded the oracle as `+2/H, -1` — i.e. with top and bottom
// unswapped — and then applied a `flipToYUpClip` step to get back to the numbers
// `canvasMatrix` actually builds. The arithmetic came out right by two wrongs
// cancelling, which is exactly why it survived: the row passed while the record
// of why was inverted (review-1 F2). The step is gone; what is left is the
// oracle's own matrix, so this row now really does compare the 3DS canvas
// against the oracle instead of against a flipped copy of it.
//
// THE VULKAN BACKEND IS THE ONE THAT FLIPS, AND IT IS RIGHT TO: `renderer_vk.cpp`
// `canvasOrtho` writes `out[5] = +2/H`, `out[13] = -1`, which is this matrix with
// the clip y negated, because the Vulkan viewport transform is y-DOWN. That flip
// belongs to the API that needs it, not to the matrix.
void oracleCanvasOrtho(float out[16], float w, float h) {
    for (int i = 0; i < 16; i++) {
        out[i] = 0.0f;
    }
    out[0] = 2.0f / (w - 0.0f);
    out[5] = 2.0f / (0.0f - h);              // = -2/H: the oracle is y-UP
    out[10] = 1.0f / (0.0f - 1.0f);
    out[12] = (0.0f + w) / (0.0f - w);
    out[13] = (0.0f + h) / (h - 0.0f);       // = +1
    out[14] = 0.0f / (0.0f - 1.0f);
    out[15] = 1.0f;
}

void testTheCanvasUsesTheScenesQuarterTurn() {
    // The width `mdk_render_canvas_width` returns on this target: 360 high and as
    // wide as the rotated framebuffer's aspect makes it (600 for a 240x400 panel).
    const float kW = 600.0f;
    const float kH = mdk::kCanvasHeight;

    float canvas[16];
    mdk::pica::canvasMatrix(canvas, kW);

    // The scene's rotation, taken from the function the scene actually uses. With
    // an identity view and projection `eyeViewProjection` IS the two conversions
    // and nothing else, and `testThePicaRecipe` pins that it maps a point to
    // (y, -x, z-w, w). Composing the oracle's ortho with THAT is the expected
    // answer, so this row compares the canvas against the scene rather than
    // against a copy of itself.
    float rotate[16];
    mdk::pica::eyeViewProjection(kIdentity, kIdentity, 0.0f, rotate);
    float ortho[16];
    oracleCanvasOrtho(ortho, kW, kH);

    // Corners and centre of the canvas, in its own pixels: x right, y DOWN.
    const float pts[5][2] = {{0.0f, 0.0f},
                             {kW, 0.0f},
                             {0.0f, kH},
                             {kW, kH},
                             {kW / 2.0f, kH / 2.0f}};
    for (int i = 0; i < 5; i++) {
        const float p[4] = {pts[i][0], pts[i][1], 0.0f, 1.0f};
        float landscape[4];
        xform(ortho, p, landscape);
        float want[4];
        xform(rotate, landscape, want);
        float got[4];
        xform(canvas, p, got);

        // The panel position agrees with the scene's, to the bit.
        PICA_CHECK(got[0] == want[0]);
        PICA_CHECK(got[1] == want[1]);
        PICA_CHECK(got[3] == want[3]);
        // And it is inside the clip volume, or the quad is not drawn at all --
        // the failure mode every other matrix row in this file is about.
        PICA_CHECK(got[0] >= -1.0f && got[0] <= 1.0f);
        PICA_CHECK(got[1] >= -1.0f && got[1] <= 1.0f);
        // THE THIRD AXIS IS THE ONE THAT WAS OUT. This GPU's clip z runs
        // [-w, 0] -- `testThePicaRecipe` pins that the scene's own recipe puts a
        // point at z-w, and the whole reason it does is that range. A canvas flat
        // at z = 0 sits PRECISELY on the far plane, and a triangle on a clip plane
        // may be rejected whole, silently, with the x/y the census checks looking
        // perfect. So the canvas goes to the middle of the range instead, and what
        // is pinned here is the invariant, not the number: strictly inside.
        PICA_CHECK(got[2] < 0.0f);
        PICA_CHECK(got[2] > -got[3]);
        PICA_CHECK(got[2] == -0.5f * got[3]);
    }

    // The four corners land on the four corners of the panel, which is the whole
    // of "the canvas fills the view" (Renderer.cs:735, margin zero). Written out
    // rather than derived, because which corner goes WHERE is the thing a wrong
    // sign gets wrong: the canvas's top-left is the panel's TOP-RIGHT, and the
    // Vulkan backend's y-down spelling would put it at the top-LEFT.
    float tl[4];
    const float tlPx[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    xform(canvas, tlPx, tl);
    PICA_CHECK(tl[0] == 1.0f && tl[1] == 1.0f);
    float br[4];
    const float brPx[4] = {kW, kH, 0.0f, 1.0f};
    xform(canvas, brPx, br);
    PICA_CHECK(br[0] == -1.0f && br[1] == -1.0f);
    float mid[4];
    const float midPx[4] = {kW / 2.0f, kH / 2.0f, 0.0f, 1.0f};
    xform(canvas, midPx, mid);
    PICA_CHECK(mid[0] == 0.0f && mid[1] == 0.0f);
}

// ---------------------------------------------------------------------------
// THE DEPTH STATE
//
// Three things have to agree: which passes test and which write (the oracle's
// rule), what the fixed-function depth map stores for a converted NDC z, and
// what the buffer was cleared to. The failure mode of their disagreeing is a
// screen showing only the clear colour, and there is no 3DS to look at here, so
// all three are pinned.
// ---------------------------------------------------------------------------

// The cleared buffer, as the normalised depth it stands for. The clear value is
// the raw 24-bit fill pattern for a DEPTH24_STENCIL8 buffer, so 0xFFFFFF is 1.0.
const float kClearedDepth = (float)mdk::pica::kDepthClearValue / 16777215.0f;

void testTheDepthStateComesFromThePass() {
    // Renderer.cs:606-607, one line per pass. The three opaque passes are
    // ordinary: tested and written.
    PICA_CHECK(mdk::pica::depthStateForPass(mdk::Pass::Solid).test);
    PICA_CHECK(mdk::pica::depthStateForPass(mdk::Pass::Solid).write);
    PICA_CHECK(mdk::pica::depthStateForPass(mdk::Pass::DoubleSided).test);
    PICA_CHECK(mdk::pica::depthStateForPass(mdk::Pass::DoubleSided).write);
    PICA_CHECK(mdk::pica::depthStateForPass(mdk::Pass::Mirror).test);
    PICA_CHECK(mdk::pica::depthStateForPass(mdk::Pass::Mirror).write);

    // The blended pass is tested but must NOT write: glass that writes depth
    // hides whatever is behind it, and the queue sorts it back to front for
    // exactly that reason. This is the pass the frame cannot show -- the pinned
    // scene's arenas are all Solid -- so the rule is pinned here instead.
    const mdk::pica::DepthState blended = mdk::pica::depthStateForPass(mdk::Pass::Blended);
    PICA_CHECK(blended.test);
    PICA_CHECK(!blended.write);

    // The two exemptions neither test nor write (Renderer.cs:606-607).
    const mdk::pica::DepthState onTop = mdk::pica::depthStateForPass(mdk::Pass::OnTop);
    PICA_CHECK(!onTop.test);
    PICA_CHECK(!onTop.write);
    const mdk::pica::DepthState overlay = mdk::pica::depthStateForPass(mdk::Pass::Overlay);
    PICA_CHECK(!overlay.test);
    PICA_CHECK(!overlay.write);
}

// ---------------------------------------------------------------------------
// THE BLEND STATE, AND THE DISCARD THE ALPHA TEST STANDS IN FOR
// ---------------------------------------------------------------------------
//
// Pinned for the same reason the depth table is: the citro3d initialiser leaves
// blending ON with the oracle's colour pair, so a build that never calls
// C3D_AlphaBlend looks correct on the glass and is wrong on every other pass.
// A screenshot of the shipped level cannot tell those two apart; this can.

void testTheOpaquePassesDoNotBlend() {
    // Renderer.cs:565,570: `blended = pass is Blended or Overlay`, and
    // `enable_blend = blended`. FOUR of the enum's six passes are unblended and
    // this array is all four of them: `OnTop` is not part of `Blended or
    // Overlay`, so it replaces too (review-3 F5 — the array named three, so a
    // pass added to the enum inherited Solid's state with nothing noticing).
    const mdk::Pass opaque[] = {mdk::Pass::Solid, mdk::Pass::DoubleSided,
                                mdk::Pass::Mirror, mdk::Pass::OnTop};
    for (int i = 0; i < 4; i++) {
        const mdk::pica::BlendState s =
            mdk::pica::blendStateForPass(opaque[i], mdk::Blend::Alpha);
        PICA_CHECK(!s.enable);
        // The PICA has no blend-disable, so "disabled" has to be spelled as the
        // replace pair on BOTH channels. An alpha channel left at SRC_ALPHA is
        // the mistake this line exists to catch.
        PICA_CHECK(s.srcColour == mdk::pica::kBlendOne);
        PICA_CHECK(s.dstColour == mdk::pica::kBlendZero);
        PICA_CHECK(s.srcAlpha == mdk::pica::kBlendOne);
        PICA_CHECK(s.dstAlpha == mdk::pica::kBlendZero);
    }

    // And the mode cannot turn an opaque pass back on: the oracle reads `blend`
    // only inside the blended branch (Renderer.cs:572), so a Solid surface
    // asked for Add is still a Solid surface.
    const mdk::pica::BlendState solidAdd =
        mdk::pica::blendStateForPass(mdk::Pass::Solid, mdk::Blend::Add);
    PICA_CHECK(!solidAdd.enable);
    PICA_CHECK(solidAdd.dstColour == mdk::pica::kBlendZero);

    // AND THE ENUM IS COVERED, not sampled (review-3 F5; review-4 F1 for the
    // version of this that could not fail, which compared the array's own length
    // with a literal 6 -- `6 == 6`, whatever the enum held). The array below is
    // every value `mdk::Pass` has, and the static_asserts are what make that a
    // CHECKED claim instead of a counted one: a seventh enumerator stops the
    // build until someone names it here and gives it its own line in
    // `blendStateForPass` and `depthStateForPass`. `Pass::Count` exists for
    // this, and `mdk::kDrawQueuePassCount` derives from the same sentinel, so
    // the draw queue's bucket count cannot drift from the enum either.
    //
    // The static_asserts hold the ARRAY's length; the loop has to hold the
    // VERDICTS, and it must accumulate them. The first version of this loop
    // assigned `coversAll` inside the body instead of accumulating it, so five
    // of the six verdicts were thrown away and the one `PICA_CHECK` asserted
    // only `Overlay` (review-5 F2). One check per pass now, so a failure also
    // names which pass lost its verdict, and a `visited` bitmask that makes
    // "the loop judged all six" a checked claim rather than a property of the
    // loop condition nobody looks at.
    const mdk::Pass every[] = {mdk::Pass::Solid, mdk::Pass::DoubleSided,
                               mdk::Pass::Mirror, mdk::Pass::Blended,
                               mdk::Pass::OnTop, mdk::Pass::Overlay};
    static_assert(static_cast<int>(mdk::Pass::Count) == 6,
                  "mdk::Pass no longer has the six members the oracle's enum has "
                  "(src/Mdk.Engine/Render/Renderer.cs:34-48). Add the pass to "
                  "this array, to blendStateForPass and depthStateForPass, and "
                  "to the draw queue, then change this number deliberately.");
    static_assert(sizeof(every) / sizeof(every[0]) ==
                      static_cast<int>(mdk::Pass::Count),
                  "this array must name every mdk::Pass member: that is the "
                  "whole of the coverage claim, so the compiler has to hold it");
    static_assert(sizeof(opaque) / sizeof(opaque[0]) ==
                      static_cast<int>(mdk::Pass::Count) - 2,
                  "the unblended set is the oracle's NOT (Blended or Overlay), "
                  "so it is every pass but two");
    unsigned visited = 0;
    for (size_t i = 0; i < sizeof(every) / sizeof(every[0]); i++) {
        const mdk::Pass pass = every[i];
        const int blends = pass == mdk::Pass::Blended || pass == mdk::Pass::Overlay;
        const int installed =
            mdk::pica::blendStateForPass(pass, mdk::Blend::Alpha).enable;
        visited |= 1u << static_cast<unsigned>(pass);
        if (installed != blends) {
            printf("FAIL pica_matrix:%d: pass %u expects enable=%d, matrix says %d\n",
                   __LINE__, static_cast<unsigned>(pass), blends, installed);
            failures++;
        }
    }
    // Every enumerator was judged, not just the last one the loop reached.
    // `Pass::Count` is 6, so the full set is bits 0..5.
    PICA_CHECK(visited == (1u << static_cast<unsigned>(mdk::Pass::Count)) - 1u);
}

void testTheBlendedPassUsesTheOraclesFactors() {
    // Renderer.cs:571-576, factor by factor, for the ordinary over.
    const mdk::pica::BlendState s =
        mdk::pica::blendStateForPass(mdk::Pass::Blended, mdk::Blend::Alpha);
    PICA_CHECK(s.enable);
    PICA_CHECK(s.srcColour == mdk::pica::kBlendSrcAlpha);
    PICA_CHECK(s.dstColour == mdk::pica::kBlendOneMinusSrcAlpha);
    PICA_CHECK(s.srcAlpha == mdk::pica::kBlendOne);
    PICA_CHECK(s.dstAlpha == mdk::pica::kBlendOneMinusSrcAlpha);

    // The alpha pair is NOT the colour pair, and that is the whole point of
    // pinning them separately: src_alpha is ONE where src_colour is SRC_ALPHA.
    // Copying the colour pair down is a one-character edit that no picture on
    // this host can show, because the panel discards the alpha channel.
    PICA_CHECK(s.srcAlpha != s.srcColour);

    // Overlay is blended too (Renderer.cs:565 names both), which is what makes
    // it a different state from its depth rule: Overlay is exempt from the
    // depth test but still mixes.
    const mdk::pica::BlendState overlay =
        mdk::pica::blendStateForPass(mdk::Pass::Overlay, mdk::Blend::Alpha);
    PICA_CHECK(overlay.enable);
    PICA_CHECK(overlay.dstColour == mdk::pica::kBlendOneMinusSrcAlpha);
}

void testAddDiffersOnlyInTheDestinationColour() {
    // Renderer.cs:572 is the only line `blend` reaches: ONE instead of
    // ONE_MINUS_SRC_ALPHA, and nothing else moves.
    const mdk::pica::BlendState alpha =
        mdk::pica::blendStateForPass(mdk::Pass::Blended, mdk::Blend::Alpha);
    const mdk::pica::BlendState add =
        mdk::pica::blendStateForPass(mdk::Pass::Blended, mdk::Blend::Add);
    PICA_CHECK(add.enable);
    PICA_CHECK(add.srcColour == mdk::pica::kBlendSrcAlpha);
    PICA_CHECK(add.dstColour == mdk::pica::kBlendOne);
    PICA_CHECK(add.srcAlpha == alpha.srcAlpha);
    PICA_CHECK(add.dstAlpha == alpha.dstAlpha);
}

void testTheDiscardIsAlphaAboveZero() {
    // The oracle's `if (index == 0) discard;` (shaders/palette.hlsl), and the
    // expansion this backend feeds the GPU encodes index 0 as alpha 0 and every
    // other index as alpha 255 (renderer_c3d.cpp, expandIndices). So the two
    // predicates select the same texels -- and the reference has to stay 0 for
    // that to remain true.
    PICA_CHECK(mdk::pica::kAlphaTestReference == 0);
    PICA_CHECK(!mdk::pica::alphaTestPasses(0));
    PICA_CHECK(mdk::pica::alphaTestPasses(1));
    PICA_CHECK(mdk::pica::alphaTestPasses(255));

    // The glass case: a flat colour carries the DTI's alpha, and a partially
    // transparent surface must SURVIVE the test -- it is the blending that makes
    // it translucent, not the discard. An alpha test written as GREATER than
    // some opaque threshold would silently make every window opaque.
    PICA_CHECK(mdk::pica::alphaTestPasses(128));
}

void testTheDepthMapIncreasesWithDistance() {
    // The converted NDC z runs [-1,0]: -1 at the near plane, 0 at the far. The
    // stored depth must INCREASE with distance, or the test function that keeps
    // nearer fragments would keep farther ones instead and the scene would be
    // occluded by its own background. A sign error in the map, or in the
    // [0,1] -> [-1,0] conversion, lands here.
    PICA_CHECK(mdk::pica::storedDepth(-1.0f) < mdk::pica::storedDepth(-0.5f));
    PICA_CHECK(mdk::pica::storedDepth(-0.5f) < mdk::pica::storedDepth(0.0f));

    // And the whole frustum stays inside the buffer's [0,1], so the 24-bit clear
    // is at or above everything the frame can produce. The loop counter is an
    // int and the float is derived from it (AGENTS.md R5, T2).
    for (int i = 0; i <= 100; i++) {
        const float z = -(float)(100 - i) / 100.0f;
        const float depth = mdk::pica::storedDepth(z);
        PICA_CHECK(depth >= 0.0f && depth <= 1.0f);
    }

    // The cleared buffer really is the top of that range.
    PICA_CHECK(kClearedDepth == 1.0f);
}

void testTheClearedBufferAcceptsTheWholeFrustum() {
    const mdk::pica::DepthState solid = mdk::pica::depthStateForPass(mdk::Pass::Solid);

    // The near plane passes, and strictly.
    PICA_CHECK(mdk::pica::depthPasses(solid, -1.0f, kClearedDepth));
    PICA_CHECK(mdk::pica::storedDepth(-1.0f) < kClearedDepth);

    // The FAR plane passes ONLY because the test is LESS_OR_EQUAL: it stores
    // exactly the value the buffer was cleared to. A strict GPU_LESS would
    // reject the far plane and anything coplanar with it -- invisible geometry
    // with no error, which is the failure mode this whole file exists to catch.
    // The oracle's compare_op is LESS_OR_EQUAL (Renderer.cs:606).
    PICA_CHECK(mdk::pica::storedDepth(0.0f) == kClearedDepth);
    PICA_CHECK(mdk::pica::depthPasses(solid, 0.0f, kClearedDepth));
}

void testNearerFragmentsWinAndFartherOnesLose() {
    const mdk::pica::DepthState solid = mdk::pica::depthStateForPass(mdk::Pass::Solid);
    const float nearDepth = mdk::pica::storedDepth(-1.0f);
    const float farDepth = mdk::pica::storedDepth(0.0f);

    // A nearer surface drawn after a farther one still lands.
    PICA_CHECK(mdk::pica::depthPasses(solid, -1.0f, farDepth));
    // A farther surface cannot paint over a nearer one. This is the check the
    // old "depth test disabled" frame could not make at all: with no test, the
    // draw order was the only occlusion there was.
    PICA_CHECK(!mdk::pica::depthPasses(solid, 0.0f, nearDepth));

    // The exempt passes land whatever the scene did, which is what OnTop means.
    const mdk::pica::DepthState onTop = mdk::pica::depthStateForPass(mdk::Pass::OnTop);
    PICA_CHECK(mdk::pica::depthPasses(onTop, 0.0f, nearDepth));

    // A blended surface is tested like an opaque one, and having not written
    // depth it cannot hide the next draw: the buffer keeps what the opaque pass
    // left there. `write` is the half of the rule that no depthPasses call can
    // show, so it is asserted directly.
    const mdk::pica::DepthState blended = mdk::pica::depthStateForPass(mdk::Pass::Blended);
    PICA_CHECK(!blended.write);
    PICA_CHECK(mdk::pica::depthPasses(blended, -0.5f, farDepth));
    PICA_CHECK(!mdk::pica::depthPasses(blended, 0.0f, nearDepth));
}

void testTheRealMatrixPutsTheNearPlaneNearest() {
    // The whole chain rather than the model of it: points go through
    // `eyeViewProjection` -- which carries the [0,1] -> [-1,0] conversion -- and
    // come out in the order the depth test needs. If the conversion's sign were
    // flipped, or applied twice, this is what catches it: the depths would come
    // out in the opposite order, and every check above would still pass.
    //
    // kProjection is the oracle's shape: clip.z = -z-4, clip.w = -z, so z = -4
    // is the near plane (engine NDC z 0) and z -> -infinity is the far plane.
    // After the conversion zNdc = 4/z, which is exact for all three points.
    float eye[16];
    mdk::pica::eyeViewProjection(kIdentity, kProjection, 0.0f, eye);

    const float zs[3] = {-4.0f, -8.0f, -16.0f};
    const float wantNdc[3] = {-1.0f, -0.5f, -0.25f};
    const float wantDepth[3] = {0.0f, 0.5f, 0.75f};

    float previous = -1.0f; // below every stored depth: the first one must pass
    for (int i = 0; i < 3; i++) {
        const float p[4] = {0, 0, zs[i], 1};
        float clip[4];
        xform(eye, p, clip);
        PICA_CHECK(clip[3] > 0.0f); // in front of the camera
        const float zNdc = clip[2] / clip[3];
        // The conversion produced [-1,0] -- not [0,1], and not [-2,-1].
        PICA_CHECK(zNdc >= -1.0f && zNdc <= 0.0f);
        PICA_CHECK(zNdc == wantNdc[i]);

        const float depth = mdk::pica::storedDepth(zNdc);
        PICA_CHECK(depth == wantDepth[i]);
        PICA_CHECK(depth > previous);
        previous = depth;
    }
}

void testTheEyePlanFollowsTheSlider() {
    // The slider is the physical 3D depth, 0..1. Closed means the panel shows the
    // LEFT buffer only, so a second pass is invisible work: one pass, no offset.
    const float kSep = 30.0f;
    const mdk::pica::EyePlan mono = mdk::pica::eyePlan(0.0f, kSep);
    PICA_CHECK(mono.passes == 1);
    PICA_CHECK(mono.offset[0] == 0.0f);

    // Half depth: two eyes, half the separation each way, left negative. The
    // sign is the convention `testTheEyeOffset` pins for the matrix, and a swap
    // here is inverted stereo -- the same geometry, crossed, which is a headache
    // and not a crash, so it is pinned as an equality rather than argued.
    const mdk::pica::EyePlan half = mdk::pica::eyePlan(0.5f, kSep);
    PICA_CHECK(half.passes == 2);
    PICA_CHECK(half.offset[0] == -7.5f);
    PICA_CHECK(half.offset[1] == 7.5f);
    PICA_CHECK(half.offset[1] - half.offset[0] == kSep * 0.5f);

    // Full depth is the whole separation, split.
    const mdk::pica::EyePlan full = mdk::pica::eyePlan(1.0f, kSep);
    PICA_CHECK(full.passes == 2);
    PICA_CHECK(full.offset[0] == -15.0f);
    PICA_CHECK(full.offset[1] == 15.0f);

    // THE CLAMP. Mandarine returns ~1.34e8 for a slider it does not model, and a
    // sibling port multiplied that straight into the eye offset and got a black
    // screen. Out-of-range in, in-range out -- and still stereo, because a
    // positive reading is a slider that is open.
    const mdk::pica::EyePlan junk = mdk::pica::eyePlan(1.34e8f, kSep);
    PICA_CHECK(junk.passes == 2);
    PICA_CHECK(junk.offset[0] == -15.0f);
    PICA_CHECK(junk.offset[1] == 15.0f);

    // A negative reading clamps to closed, not to a full-depth stereo with the
    // eyes crossed.
    const mdk::pica::EyePlan neg = mdk::pica::eyePlan(-5.0f, kSep);
    PICA_CHECK(neg.passes == 1);
    PICA_CHECK(neg.offset[0] == 0.0f);

    // And a zero separation is still stereo, just with both eyes where the
    // camera is: the pass count is the slider's business, not the hardware's.
    const mdk::pica::EyePlan flat = mdk::pica::eyePlan(0.5f, 0.0f);
    PICA_CHECK(flat.passes == 2);
    PICA_CHECK(flat.offset[0] == 0.0f && flat.offset[1] == 0.0f);
}

void testTheClearColourByteOrder() {
    // The seam is 0xAARRGGBB; the PICA's clear word is 0xRRGGBBAA — RED IN THE
    // TOP BYTE. That is a MEASUREMENT, not a reading of a header, and it is the
    // reason this row exists. Two independent readings of the same build on the
    // pinned instrument (Mandarine r1.2.1, DISPLAY=:99):
    //
    //   word passed          panel pixel
    //   0xFF102030 (raw)  ->  #ff1020     the alpha byte became red
    //   0x102030FF (here) ->  #102030     exactly what the frontend asked for
    //
    // So the contract is about the WORD's bit fields, and it must be asserted as
    // bit fields. Asserting it as memory bytes was the first draft of this row
    // and it was wrong: on a little-endian machine the byte at offset 0 is the
    // LOW byte, which is the alpha here, so a byte-indexed expectation encodes
    // the opposite of what the hardware sees even when the conversion is right.
    const uint32_t navy = mdk::pica::clearColour(0xFF102030u);   // the 3DS frontend's clear
    PICA_CHECK((navy >> 24) == 0x10u);                       // R
    PICA_CHECK(((navy >> 16) & 0xFFu) == 0x20u);             // G
    PICA_CHECK(((navy >> 8) & 0xFFu) == 0x30u);              // B
    PICA_CHECK((navy & 0xFFu) == 0xFFu);                     // A
    // The defect, stated as the failure it caused: passing the seam value
    // through untouched makes the alpha byte the red channel, which is how a
    // dark navy filled the panel with #ff1020 for this backend's whole history.
    PICA_CHECK(navy != 0xFF102030u);
    PICA_CHECK((0xFF102030u >> 24) == 0xFFu);   // the byte that was painting the screen red

    // The seam's own default, and a colour whose alpha is not 255, so the
    // conversion cannot be a constant-255 shortcut that happens to pass above.
    const uint32_t black = mdk::pica::clearColour(0xFF000000u);
    PICA_CHECK(black == 0x000000FFu);
    const uint32_t half = mdk::pica::clearColour(0x80FF7F00u);
    PICA_CHECK(half == 0xFF7F0080u);

    // The general property: whichever byte the seam means by R/G/B/A, that is
    // the field the PICA must find it in. An identity, a swap of the wrong pair,
    // or a rotate by 8 the other way each break one of these four.
    bool channelsLand = true;
    for (uint32_t v = 0; v < 256; v++) {
        const uint32_t argb = (0x11u << 24) | (v << 16) | (v << 8) | 0x22u;
        const uint32_t got = mdk::pica::clearColour(argb);
        if ((got >> 24) != v || ((got >> 16) & 0xFFu) != v ||
            ((got >> 8) & 0xFFu) != 0x22u || (got & 0xFFu) != 0x11u) {
            channelsLand = false;
        }
    }
    PICA_CHECK(channelsLand);
}

void testTheTexelByteOrder() {
    // THE SAME TRAP ONE UNIT EARLIER. `testTheClearColourByteOrder` pins the
    // colour that FILLS the frame; this pins the colour that gets DRAWN on it.
    // Both are the PICA's RGBA8, so both are `[A, B, G, R]` in memory — red in
    // the top byte of the little-endian word — and the seam's RGBA-in-memory
    // order has to be rotated before it is handed to the GPU.
    //
    // ASSERTED AS BYTES, ON PURPOSE. The clear row above is a word and is
    // checked as bit fields; this one writes memory and the whole question is
    // which byte lands at which address. Same hardware convention, opposite
    // assertion shape — see the comment on `pica::texelBytes`.
    uint8_t t[4] = {0, 0, 0, 0};
    mdk::pica::texelBytes(t, 0x11u, 0x22u, 0x33u, 0x44u);
    PICA_CHECK(t[0] == 0x44u);   // A
    PICA_CHECK(t[1] == 0x33u);   // B
    PICA_CHECK(t[2] == 0x22u);   // G
    PICA_CHECK(t[3] == 0x11u);   // R

    // Read it back the way the hardware does, as a little-endian word whose red
    // is the TOP byte, and require the channels to land where the seam meant
    // them. This is the property the shipped defect broke: it is what lets a
    // future reader see WHY the order is not the seam's own.
    const uint32_t word = (uint32_t)t[0] | ((uint32_t)t[1] << 8) |
                          ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
    PICA_CHECK((word >> 24) == 0x11u);               // R
    PICA_CHECK(((word >> 16) & 0xFFu) == 0x22u);     // G
    PICA_CHECK(((word >> 8) & 0xFFu) == 0x33u);      // B
    PICA_CHECK((word & 0xFFu) == 0x44u);             // A

    // THE DEFECT, STATED AS THE FAILURE IT CAUSED. Writing the seam's RGBA
    // order straight through put the forced alpha of 255 in the hardware's red
    // channel, so EVERY painted pixel came back with R = 255: measured on the
    // pinned instrument, 96000 of 96000 pixels of
    // docs/evidence/2026-11-09-L7-shader-w-current.png, 131 distinct colours and
    // not one of them with a red that was not 255. The naive order is exactly
    // what this check forbids.
    uint8_t naive[4] = {0x11u, 0x22u, 0x33u, 0x44u};
    PICA_CHECK(naive[3] != t[3] || naive[0] != t[0]);
    PICA_CHECK(!(naive[0] == t[0] && naive[1] == t[1] &&
                 naive[2] == t[2] && naive[3] == t[3]));

    // The index-0 discard survives the rotation, which is the property that made
    // the defect invisible: an all-zero texel has alpha 0 under ANY byte order,
    // so the discard kept working while every colour it let through was wrong.
    uint8_t zero[4] = {9, 9, 9, 9};
    mdk::pica::texelBytes(zero, 0u, 0u, 0u, 0u);
    PICA_CHECK(zero[0] == 0u);   // the alpha the discard tests, still zero

    // And a general sweep: no channel may collide, and the alpha must never land
    // in red. A swap of the wrong pair, or an identity, breaks one of these.
    bool lands = true;
    for (uint32_t v = 0; v < 256u; v++) {
        uint8_t s[4];
        mdk::pica::texelBytes(s, (uint8_t)v, (uint8_t)(255u - v), (uint8_t)(v ^ 0x5Au), 0xF0u);
        const uint32_t w = (uint32_t)s[0] | ((uint32_t)s[1] << 8) |
                           ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
        if ((w >> 24) != v || ((w >> 16) & 0xFFu) != 255u - v ||
            ((w >> 8) & 0xFFu) != (v ^ 0x5Au) || (w & 0xFFu) != 0xF0u) {
            lands = false;
        }
    }
    PICA_CHECK(lands);
}

void testSceneUvRunsFromTheLastRow() {
    // A 306-row image padded to 512 (a real level-7 arena texture). The image
    // occupies memory rows 0..305; the PICA's t = 1 is memory row 0.
    const float realH = 306.0f, paddedH = 512.0f;
    const mdk::pica::SceneUv uv = mdk::pica::sceneUvFromTop(realH / paddedH, 0.0f);
    const float tTop = 0.0f * uv.scaleY + uv.offsetY;      // oracle v = 0 (image top)
    const float tBottom = 1.0f * uv.scaleY + uv.offsetY;   // oracle v = 1 (image bottom)
    // t -> memory row, counted from the LAST row as the PICA does.
    const float rowTop = (1.0f - tTop) * paddedH;
    const float rowBottom = (1.0f - tBottom) * paddedH;
    PICA_CHECK(rowTop == 0.0f);                 // v = 0 reads image row 0
    PICA_CHECK(rowBottom == realH);             // v = 1 reads the image's last row, not padding
    // The unflipped mapping is the defect: v in [0,1] lands in rows 511..206,
    // i.e. the padding for every v below (paddedH - realH) / realH.
    const float wrongRowTop = (1.0f - 0.0f) * paddedH;
    PICA_CHECK(wrongRowTop > realH);
    // A frame offset (stacked animation frames) moves down the image, not up.
    const mdk::pica::SceneUv framed = mdk::pica::sceneUvFromTop(0.25f, 0.25f);
    PICA_CHECK((1.0f - (0.0f * framed.scaleY + framed.offsetY)) == 0.25f);
}

void testTheHazeIsThePcCurveOnTheRightDraws() {
    // THE CURVE. The shader keeps ex2(hazeExponent(density) * depth) of a
    // vertex's colour; that must be the PC's exp(-density * distance)
    // (enhanced.frag:187) at every distance the camera reaches (far = 20000).
    const float density = 0.0002f;   // level_light.cpp kHazeDensity
    const float x = mdk::pica::hazeExponent(density);
    PICA_CHECK(x < 0.0f);
    const float depths[6] = {0.5f, 10.0f, 100.0f, 1000.0f, 5000.0f, 20000.0f};
    for (int i = 0; i < 6; i++) {
        const float kept = exp2f(x * depths[i]);
        const float pc = expf(-density * depths[i]);
        PICA_CHECK(fabsf(kept - pc) < 1e-5f);
    }
    // No density, no haze: ex2(0) = 1 keeps the whole colour.
    PICA_CHECK(mdk::pica::hazeExponent(0.0f) == 0.0f);
    PICA_CHECK(mdk::pica::hazeExponent(-1.0f) == 0.0f);

    // THE DRAWS. The PC hazes its lit and sprite modes only: the sky (sky.frag)
    // and the canvas take none, and both are Original-shaded here.
    mdk::Lighting rig;
    rig.enabled = true;
    rig.haze_density = density;
    PICA_CHECK(mdk::pica::hazeApplies(true, rig, mdk::Shading::Lit, mdk::Pass::Solid));
    PICA_CHECK(mdk::pica::hazeApplies(true, rig, mdk::Shading::Sprite, mdk::Pass::Blended));
    PICA_CHECK(!mdk::pica::hazeApplies(true, rig, mdk::Shading::Original, mdk::Pass::DoubleSided));
    PICA_CHECK(!mdk::pica::hazeApplies(true, rig, mdk::Shading::Lit, mdk::Pass::Overlay));
    PICA_CHECK(!mdk::pica::hazeApplies(false, rig, mdk::Shading::Lit, mdk::Pass::Solid));
    mdk::Lighting off = rig;
    off.enabled = false;
    PICA_CHECK(!mdk::pica::hazeApplies(true, off, mdk::Shading::Lit, mdk::Pass::Solid));
    mdk::Lighting thin = rig;
    thin.haze_density = 0.0f;
    PICA_CHECK(!mdk::pica::hazeApplies(true, thin, mdk::Shading::Lit, mdk::Pass::Solid));

    // THE COLOUR WORD: 0xAABBGGRR, opaque, clamped.
    const mdk::Colour c = {1.0f, 0.5f, 0.0f, 0.3f};
    PICA_CHECK(mdk::pica::hazeColourWord(c) == 0xFF0080FFu);
    const mdk::Colour wild = {-1.0f, 2.0f, 0.2f, 1.0f};
    PICA_CHECK(mdk::pica::hazeColourWord(wild) == 0xFF33FF00u);
}

void testTheFilteredEdge() {
    // THE ALPHA-TEST REFERENCE, per draw: the oracle's discard with filtering
    // off; the PC's half cover (keep alpha >= 0.5, i.e. byte > 127) for a
    // filtered SCENE draw; and never for the canvas, which enhanced.frag returns
    // from before its HALF_COVER discard (the hurt flash is alpha 0.4).
    using mdk::Pass;
    const Pass scene[5] = {Pass::Solid, Pass::DoubleSided, Pass::Mirror, Pass::Blended, Pass::OnTop};
    for (int i = 0; i < 5; i++) {
        PICA_CHECK(mdk::pica::alphaTestReferenceFor(false, scene[i]) == mdk::pica::kAlphaTestReference);
        PICA_CHECK(mdk::pica::alphaTestReferenceFor(true, scene[i]) == 127);
    }
    PICA_CHECK(mdk::pica::alphaTestReferenceFor(true, Pass::Overlay) == mdk::pica::kAlphaTestReference);
    PICA_CHECK(mdk::pica::alphaTestReferenceFor(false, Pass::Overlay) == mdk::pica::kAlphaTestReference);
    const int half = mdk::pica::alphaTestReferenceFor(true, Pass::Solid);
    PICA_CHECK(128 > half);      // 128/255 >= 0.5: kept
    PICA_CHECK(!(127 > half));   // 127/255 <  0.5: cut
    PICA_CHECK(102 > mdk::pica::alphaTestReferenceFor(true, Pass::Overlay));  // the hurt flash survives

    // A 5x5 image in an 8x8 buffer (one whole tile, a legal PICA layout): opaque
    // red with a transparent hole at (1,1) and a transparent texel at (2,2) whose
    // opaque neighbours include two greens.
    const uint32_t pw = 8, ph = 8;
    uint8_t buf[8 * 8 * 4];
    memset(buf, 0, sizeof(buf));
    auto at = [&](uint8_t* base, uint32_t x, uint32_t y, uint32_t w) {
        return base + mdk::pica::textureTexelIndex(x, y, w) * 4u;
    };
    for (uint32_t y = 0; y < 5; y++) {
        for (uint32_t x = 0; x < 5; x++) {
            mdk::pica::texelBytes(at(buf, x, y, pw), 200, 0, 0, 255);   // red
        }
    }
    mdk::pica::texelBytes(at(buf, 1, 1, pw), 0, 0, 0, 0);       // the hole
    mdk::pica::texelBytes(at(buf, 2, 2, pw), 0, 0, 0, 0);       // the second hole
    mdk::pica::texelBytes(at(buf, 2, 1, pw), 0, 100, 0, 255);   // green
    mdk::pica::texelBytes(at(buf, 1, 2, pw), 0, 100, 0, 255);   // green
    mdk::pica::prepareForFiltering(buf, 5, 5, (int32_t)pw, (int32_t)ph);

    // [A, B, G, R]. (1,1)'s opaque 8-neighbours: five red, two green ((2,2) is
    // transparent and is not a source). Its alpha stays 0.
    PICA_CHECK(at(buf, 1, 1, pw)[0] == 0);
    PICA_CHECK(at(buf, 1, 1, pw)[3] == (uint8_t)(200u * 5u / 7u));
    PICA_CHECK(at(buf, 1, 1, pw)[2] == (uint8_t)(100u * 2u / 7u));
    PICA_CHECK(at(buf, 1, 1, pw)[1] == 0);
    // (2,2)'s: (1,1) transparent (not a source even after it was coloured); (2,1)
    // and (1,2) green; (3,1), (1,3), (3,2), (2,3), (3,3) red -> 5 red, 2 green.
    PICA_CHECK(at(buf, 2, 2, pw)[0] == 0);
    PICA_CHECK(at(buf, 2, 2, pw)[3] == (uint8_t)(200u * 5u / 7u));
    PICA_CHECK(at(buf, 2, 2, pw)[2] == (uint8_t)(100u * 2u / 7u));
    // Opaque texels are untouched.
    PICA_CHECK(at(buf, 0, 0, pw)[0] == 255 && at(buf, 0, 0, pw)[3] == 200);
    // THE CLAMP: the first padding column and row copy the last real ones, alpha
    // included, the corner from the copied column; beyond them, still padding.
    for (uint32_t y = 0; y < 5; y++) {
        PICA_CHECK(memcmp(at(buf, 5, y, pw), at(buf, 4, y, pw), 4) == 0);
        PICA_CHECK(at(buf, 6, y, pw)[0] == 0);
    }
    for (uint32_t x = 0; x < 6; x++) {
        PICA_CHECK(memcmp(at(buf, x, 5, pw), at(buf, x, 4, pw), 4) == 0);
        PICA_CHECK(at(buf, x, 6, pw)[0] == 0);
    }

    // ONE PADDED AXIS: 8 wide (no column to fill), 5 tall in 8.
    uint8_t one[8 * 8 * 4];
    memset(one, 0, sizeof(one));
    for (uint32_t y = 0; y < 5; y++) {
        for (uint32_t x = 0; x < 8; x++) {
            mdk::pica::texelBytes(at(one, x, y, 8), (uint32_t)(x * 10), (uint32_t)y, 7, 255);
        }
    }
    mdk::pica::prepareForFiltering(one, 8, 5, 8, 8);
    for (uint32_t x = 0; x < 8; x++) {
        PICA_CHECK(memcmp(at(one, x, 5, 8), at(one, x, 4, 8), 4) == 0);
        PICA_CHECK(at(one, x, 6, 8)[0] == 0);
    }

    // A POWER-OF-TWO image fills no padding, but its transparent texels still
    // take colour (a vacuous all-opaque buffer would pass a no-op).
    uint8_t pot[8 * 8 * 4];
    memset(pot, 0, sizeof(pot));
    for (uint32_t y = 0; y < 8; y++) {
        for (uint32_t x = 0; x < 8; x++) {
            mdk::pica::texelBytes(at(pot, x, y, 8), 0, 0, 90, 255);   // blue
        }
    }
    mdk::pica::texelBytes(at(pot, 7, 7, 8), 0, 0, 0, 0);
    mdk::pica::prepareForFiltering(pot, 8, 8, 8, 8);
    PICA_CHECK(at(pot, 7, 7, 8)[0] == 0 && at(pot, 7, 7, 8)[1] == 90);
    PICA_CHECK(at(pot, 6, 6, 8)[0] == 255);
}

void testTheMipChain() {
    // THE LEVEL COUNT: every level >= 8x8, the short axis decides.
    PICA_CHECK(mdk::pica::mipLevelsFor(8, 8) == 0);
    PICA_CHECK(mdk::pica::mipLevelsFor(16, 16) == 1);
    PICA_CHECK(mdk::pica::mipLevelsFor(256, 256) == 5);     // 256..8: levels 1-5
    PICA_CHECK(mdk::pica::mipLevelsFor(1024, 1024) == 7);
    PICA_CHECK(mdk::pica::mipLevelsFor(1024, 16) == 1);     // the short axis
    PICA_CHECK(mdk::pica::mipLevelsFor(128, 256) == 4);

    // THE BYTES, per format, chain included: 256x256 with 5 levels is
    // 256^2 + 128^2 + ... + 8^2 = 87360 texels. RGBA8 4 bytes, ETC1 half a byte,
    // ETC1A4 one byte -- the HD layer's 8x and 4x against the original look.
    PICA_CHECK(mdk::pica::mipChainTexels(256, 256, 5) == 87360u);
    PICA_CHECK(mdk::pica::textureBytes(mdk::pica::TexelFormat::Rgba8, 256, 256, 0) == 262144u);
    PICA_CHECK(mdk::pica::textureBytes(mdk::pica::TexelFormat::Rgba8, 256, 256, 5) == 349440u);
    PICA_CHECK(mdk::pica::textureBytes(mdk::pica::TexelFormat::Etc1, 256, 256, 5) == 43680u);
    PICA_CHECK(mdk::pica::textureBytes(mdk::pica::TexelFormat::Etc1A4, 256, 256, 5) == 87360u);

    // THE BOX FILTER, on the tiled layout: a 16x16 level of known texels, each
    // byte a function of (x, y, channel), halved to 8x8. Every output byte is
    // the rounded mean of its four parents -- read back through the tile index,
    // so a row-major write (the Morton trap) fails here.
    uint8_t src[16 * 16 * 4];
    for (uint32_t y = 0; y < 16; y++) {
        for (uint32_t x = 0; x < 16; x++) {
            uint8_t* t = src + mdk::pica::textureTexelIndex(x, y, 16) * 4u;
            for (uint32_t k = 0; k < 4; k++) {
                t[k] = (uint8_t)((x * 13u + y * 7u + k * 31u) & 0xFFu);
            }
        }
    }
    uint8_t dst[8 * 8 * 4];
    memset(dst, 0xEE, sizeof(dst));
    mdk::pica::downsampleTiled(src, 16, 16, dst);
    bool exact = true;
    for (uint32_t y = 0; y < 8; y++) {
        for (uint32_t x = 0; x < 8; x++) {
            const uint8_t* o = dst + mdk::pica::textureTexelIndex(x, y, 8) * 4u;
            for (uint32_t k = 0; k < 4; k++) {
                uint32_t sum = 0;
                for (uint32_t dy = 0; dy < 2; dy++) {
                    for (uint32_t dx = 0; dx < 2; dx++) {
                        sum += src[mdk::pica::textureTexelIndex(2 * x + dx, 2 * y + dy, 16) * 4u + k];
                    }
                }
                if (o[k] != (uint8_t)((sum + 2u) >> 2)) {
                    exact = false;
                }
            }
        }
    }
    PICA_CHECK(exact);
    // A half-covered 2x2 (two alpha 255, two alpha 0) is alpha 128: above the
    // half-cover cut, so a distant cut-out edge thins rather than vanishes.
    uint8_t half[16 * 16 * 4];
    memset(half, 0, sizeof(half));
    mdk::pica::texelBytes(half + mdk::pica::textureTexelIndex(0, 0, 16) * 4u, 10, 20, 30, 255);
    mdk::pica::texelBytes(half + mdk::pica::textureTexelIndex(1, 0, 16) * 4u, 10, 20, 30, 255);
    mdk::pica::downsampleTiled(half, 16, 16, dst);
    PICA_CHECK(dst[mdk::pica::textureTexelIndex(0, 0, 8) * 4u] == 128);

    // THE CHAIN IN ONE BUFFER: buildMipChain lays level 1 right after level 0
    // and equals a downsample (then the edge preparation, a no-op on an opaque
    // power-of-two level) of it.
    const size_t chainTexels = mdk::pica::mipChainTexels(16, 16, 1);
    PICA_CHECK(chainTexels == 16u * 16u + 8u * 8u);
    uint8_t chain[(16 * 16 + 8 * 8) * 4];
    memcpy(chain, src, sizeof(src));
    for (uint32_t i = 0; i < 16 * 16; i++) {
        chain[i * 4u] = 255;   // opaque, so the preparation leaves it alone
    }
    uint8_t level1[8 * 8 * 4];
    mdk::pica::downsampleTiled(chain, 16, 16, level1);
    mdk::pica::buildMipChain(chain, 16, 16, 16, 16, 1);
    PICA_CHECK(memcmp(chain + 16 * 16 * 4, level1, sizeof(level1)) == 0);

    // A PADDED TEXTURE, TWO LEVELS DEEP: 20x20 real in 32x32. Each level sits at
    // the offset makeTexture uploads it from (the level above's bytes later), is
    // the downsample of the level above, and is re-prepared at its own scaled real
    // size ((20+1)/2 = 10, then 5). A chain that wrote level 2 over level 1, or
    // prepared at the padded size, fails here.
    const int32_t levels = mdk::pica::mipLevelsFor(32, 32);
    PICA_CHECK(levels == 2);
    PICA_CHECK(mdk::pica::textureBytes(mdk::pica::TexelFormat::Rgba8, 32, 32, levels) ==
               (32u * 32u + 16u * 16u + 8u * 8u) * 4u);
    static uint8_t padded[(32 * 32 + 16 * 16 + 8 * 8) * 4];
    memset(padded, 0, sizeof(padded));
    for (uint32_t y = 0; y < 20; y++) {
        for (uint32_t x = 0; x < 20; x++) {
            const bool hole = ((x + y) % 7u) == 0;
            mdk::pica::texelBytes(padded + mdk::pica::textureTexelIndex(x, y, 32) * 4u,
                                  (x * 11u) & 0xFFu, (y * 9u) & 0xFFu, 40, hole ? 0u : 255u);
        }
    }
    mdk::pica::prepareForFiltering(padded, 20, 20, 32, 32);
    // The expected chain, built level by level from the primitives.
    static uint8_t want1[16 * 16 * 4];
    static uint8_t want2[8 * 8 * 4];
    mdk::pica::downsampleTiled(padded, 32, 32, want1);
    mdk::pica::prepareForFiltering(want1, 10, 10, 16, 16);
    mdk::pica::downsampleTiled(want1, 16, 16, want2);
    mdk::pica::prepareForFiltering(want2, 5, 5, 8, 8);
    mdk::pica::buildMipChain(padded, 32, 32, 20, 20, levels);
    const size_t offset1 = (size_t)32 * 32 * 4;                 // makeTexture's level 1
    const size_t offset2 = offset1 + ((size_t)32 >> 1) * ((size_t)32 >> 1) * 4;   // level 2
    PICA_CHECK(memcmp(padded + offset1, want1, sizeof(want1)) == 0);
    PICA_CHECK(memcmp(padded + offset2, want2, sizeof(want2)) == 0);
    PICA_CHECK(offset2 + sizeof(want2) ==
               mdk::pica::textureBytes(mdk::pica::TexelFormat::Rgba8, 32, 32, levels));
}

void testTheHdResidencyPolicy() {
    using mdk::pica::HdSlot;
    // THE VICTIM: the least recently drawn resident copy that the protected frame
    // did not draw.
    HdSlot slots[5];
    slots[0] = {true, 10};
    slots[1] = {true, 3};
    slots[2] = {false, 1};    // not resident: never a victim, however old
    slots[3] = {true, 25};    // drawn in the protected frame
    slots[4] = {true, 7};
    PICA_CHECK(mdk::pica::hdEvictionVictim(slots, 5, 20) == 1);
    slots[1].resident = false;
    PICA_CHECK(mdk::pica::hdEvictionVictim(slots, 5, 20) == 4);
    // Everything resident was drawn at or after the protect stamp: none.
    PICA_CHECK(mdk::pica::hdEvictionVictim(slots, 5, 7) == -1);
    PICA_CHECK(mdk::pica::hdEvictionVictim(slots, 0, 100) == -1);
    // UNLINKED FIRST: an unlinked copy goes before an older linked one, the
    // protect stamp still binds it, and unlinkedOnly never names a linked one.
    {
        HdSlot u[4];
        u[0] = {true, 2, true};
        u[1] = {true, 9, false};    // unlinked, newer than u[0]
        u[2] = {true, 30, false};   // unlinked but drawn in the protected frame
        u[3] = {true, 5, false};    // unlinked, the oldest unlinked
        PICA_CHECK(mdk::pica::hdEvictionVictim(u, 4, 20) == 3);
        u[3].resident = false;
        PICA_CHECK(mdk::pica::hdEvictionVictim(u, 4, 20) == 1);
        PICA_CHECK(mdk::pica::hdEvictionVictim(u, 4, 20, true) == 1);
        u[1].resident = false;
        PICA_CHECK(mdk::pica::hdEvictionVictim(u, 4, 20) == 0);        // only a linked one left
        PICA_CHECK(mdk::pica::hdEvictionVictim(u, 4, 20, true) == -1); // and unlinkedOnly refuses it
    }

    // THE GATE: one more read only if it could be imported.
    const size_t largest = 349504u, budget = 1048576u;
    PICA_CHECK(mdk::pica::hdMayDispatch(0, 0, largest, budget, false));
    PICA_CHECK(mdk::pica::hdMayDispatch(0, 2, largest, budget, false));    // 3 * largest fits
    PICA_CHECK(!mdk::pica::hdMayDispatch(0, 3, largest, budget, false));   // 4 does not
    PICA_CHECK(!mdk::pica::hdMayDispatch(900000, 0, largest, budget, false));
    PICA_CHECK(mdk::pica::hdMayDispatch(900000, 0, largest, budget, true));  // a victim exists

    // A CAMERA TURN, simulated with the policy alone: 12 textures of 300 KB, a
    // 700 KB budget (2 fit) against a view drawing 3 at a time -- the protected
    // frame's working set does NOT fit, which is the case protection exists for --
    // turning one texture per frame. Every frame: the textures it draws are stamped; at the next
    // boundary each wanted-but-absent one is imported if the gate and the victim
    // allow. Asserted: nothing the previous frame drew is ever evicted, and no
    // read is handed out that could not be imported.
    const int32_t kCount = 12;
    const size_t kBytes = 300000u;
    const size_t simBudget = 700000u;
    HdSlot sim[kCount];
    size_t resident = 0;
    uint32_t stamp = 0;
    uint32_t previousFrameStart = 0;
    bool protectedHeld = true;
    int32_t dispatched = 0;
    // The camera HOLDS for 10 frames (three textures wanted, two fit, all
    // drawn every frame: the protected working set overflows the budget, and
    // only refusing to evict it prevents thrashing), then turns one texture per
    // frame.
    auto windowOf = [](int32_t frame) { return frame < 10 ? 0 : frame - 10; };
    int32_t evictions = 0, imports = 0;
    for (int32_t frame = 0; frame < 40; frame++) {
        const uint32_t frameStart = stamp + 1;
        // The boundary: import what the previous frame wanted.
        for (int32_t k = 0; k < 3; k++) {
            const int32_t want = (windowOf(frame - 1) + k) % kCount;
            if (frame == 0 || sim[want].resident) {
                continue;
            }
            const bool evictable =
                mdk::pica::hdEvictionVictim(sim, kCount, previousFrameStart) >= 0;
            if (!mdk::pica::hdMayDispatch(resident, 0, kBytes, simBudget, evictable)) {
                continue;
            }
            // Dispatched: an SD read the real pump would pay for. It must end in
            // an import, or the gate let through a read only to drop it.
            dispatched++;
            while (resident + kBytes > simBudget) {
                const int32_t v = mdk::pica::hdEvictionVictim(sim, kCount, previousFrameStart);
                if (v < 0) {
                    break;
                }
                if (sim[v].lastUsed >= previousFrameStart) {
                    protectedHeld = false;
                }
                sim[v].resident = false;
                resident -= kBytes;
                evictions++;
            }
            if (resident + kBytes <= simBudget) {
                sim[want].resident = true;
                sim[want].lastUsed = previousFrameStart;
                resident += kBytes;
                imports++;
            }

        }
        // The frame: draw three textures, the window moving by one each frame.
        for (int32_t k = 0; k < 3; k++) {
            const int32_t drawn = (windowOf(frame) + k) % kCount;
            stamp++;
            if (sim[drawn].resident) {
                sim[drawn].lastUsed = stamp;
            }
        }
        previousFrameStart = frameStart;
    }
    PICA_CHECK(protectedHeld);
    // Every read handed out was imported: none was wasted. (The budget itself
    // cannot be exceeded here -- an import is behind its own room check -- so it
    // is not asserted; the GATE is what this line holds.)
    PICA_CHECK(dispatched == imports);
    PICA_CHECK(imports > 8);       // the view kept getting HD copies as it turned
    PICA_CHECK(evictions > 0);     // and the budget made room for them
}

void testTheTexelTileOrder() {
    // THE SAME TRAP ONE UNIT BEFORE THE BYTES: the byte order above says which
    // byte of a texel goes where; this says WHICH TEXEL SLOT a (x, y) goes in.
    // The PICA reads an 8x8 Morton tile, not a row (`pica::textureTexelIndex`
    // carries the rule and its citations), and `C3D_TexLoadImage` copies a
    // linear buffer verbatim, so a row-major upload is sampled scrambled
    // without any error being raised.
    const uint32_t w = 64, h = 64;

    // (1) A SLOT PER TEXEL, NO COLLISION AND NO HOLE. A permutation with a
    // duplicated slot would silently lose texels and could still pass every
    // anchor below.
    bool seen[64 * 64] = {false};
    bool bijective = true;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const size_t slot = mdk::pica::textureTexelIndex(x, y, w);
            if (slot >= (size_t)(w * h) || seen[slot]) {
                bijective = false;
            } else {
                seen[slot] = true;
            }
        }
    }
    PICA_CHECK(bijective);

    // (2) THE NESTING, AS ANCHORS. Inside a tile: the 2x2 cells and the texels
    // within them are Z-ordered, so (0,0) (1,0) (0,1) (1,1) are slots 0..3 and
    // (2,0) — still in the first 2x2 CELL ROW — is slot 4, not slot 2. Tiles
    // are row-major, so tile (1,0) starts 64 slots in and tile (0,1) starts a
    // whole tile row (8 tiles here) after that.
    PICA_CHECK(mdk::pica::textureTexelIndex(0, 0, w) == 0u);
    PICA_CHECK(mdk::pica::textureTexelIndex(1, 0, w) == 1u);
    PICA_CHECK(mdk::pica::textureTexelIndex(0, 1, w) == 2u);
    PICA_CHECK(mdk::pica::textureTexelIndex(1, 1, w) == 3u);
    PICA_CHECK(mdk::pica::textureTexelIndex(2, 0, w) == 4u);
    PICA_CHECK(mdk::pica::textureTexelIndex(8, 0, w) == 64u);
    PICA_CHECK(mdk::pica::textureTexelIndex(0, 8, w) == 512u);
    PICA_CHECK(mdk::pica::textureTexelIndex(63, 63, w) == 4095u);

    // (3) THE PATTERN FIXTURE: the documented defect, reproduced and pinned.
    // A period-32 vertical square wave across a 64-wide texture is the exact
    // probe that came back "four times too many bands" (docs/OPEN_ITEMS.md,
    // batch 173). Written through the tile order it reads back as written;
    // written ROW-MAJOR it reads back at period 8, which IS the four-times
    // signature. Both arms are asserted, so the check cannot be satisfied by a
    // helper that happens to differ from row-major in some other way.
    uint8_t tiled[64 * 64];
    uint8_t linear[64 * 64];
    memset(tiled, 0, sizeof tiled);
    memset(linear, 0, sizeof linear);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t on = ((x & 31u) < 16u) ? 1u : 0u;
            tiled[mdk::pica::textureTexelIndex(x, y, w)] = on;
            linear[y * w + x] = on;
        }
    }
    int tiledTransitions = 0, linearTransitions = 0;
    for (uint32_t x = 1; x < w; x++) {
        const size_t a = mdk::pica::textureTexelIndex(x, 0, w);
        const size_t b = mdk::pica::textureTexelIndex(x - 1, 0, w);
        if (tiled[a] != tiled[b]) tiledTransitions++;
        if (linear[a] != linear[b]) linearTransitions++;
    }
    PICA_CHECK(tiledTransitions == 3);     // the source's own 2 bands
    PICA_CHECK(linearTransitions == 15);   // period 8: four times too many

    // (4) AND THE OTHER MEASURED SIGNATURE, WHICH NO UV SCALE COULD PRODUCE: a
    // left-white/right-black image read row-major through the tile order comes
    // back as FULL-WIDTH HORIZONTAL bands — zero horizontal transitions, many
    // vertical ones. That is why the open item says the three observations are
    // "not consistent with any single scale factor": a vertical split turning
    // into horizontal bands is a permutation, not a scale.
    memset(linear, 0, sizeof linear);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            linear[y * w + x] = (x < 32u) ? 1u : 0u;
        }
    }
    int across = 0, down = 0;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const size_t here = mdk::pica::textureTexelIndex(x, y, w);
            if (x > 0 && linear[here] != linear[mdk::pica::textureTexelIndex(x - 1, y, w)]) {
                across++;
            }
            if (y > 0 && linear[here] != linear[mdk::pica::textureTexelIndex(x, y - 1, w)]) {
                down++;
            }
        }
    }
    PICA_CHECK(across == 0);
    PICA_CHECK(down > 0);
}

// THE GLOW (pica_matrix.h). The quad's corners land on the picture's corners in
// the glow texture, through the panel's quarter turn: the panel's left edge is
// the texture's first memory row (t = 1), and the panel's bottom is s = 0.
void testTheGlowQuadMapsThePanelOntoThePicture() {
    float s = -1.0f, t = -1.0f;
    mdk::pica::glowTexCoord(0.0f, 0.0f, &s, &t);   // top-left
    PICA_CHECK(s == 240.0f / 256.0f && t == 1.0f);
    mdk::pica::glowTexCoord(1.0f, 0.0f, &s, &t);   // top-right
    PICA_CHECK(s == 240.0f / 256.0f && t == 1.0f - 400.0f / 512.0f);
    mdk::pica::glowTexCoord(0.0f, 1.0f, &s, &t);   // bottom-left
    PICA_CHECK(s == 0.0f && t == 1.0f);
    mdk::pica::glowTexCoord(1.0f, 1.0f, &s, &t);   // bottom-right
    PICA_CHECK(s == 0.0f && t == 1.0f - 400.0f / 512.0f);
}

// A small-pass quad covers its whole target with the identity: corner (s, t) at
// clip (2s - 1, 2t - 1), so the four corners are the clip square's.
void testTheGlowLevelPassIsTheIdentity() {
    float x = 0.0f, y = 0.0f;
    mdk::pica::glowLevelCorner(0.0f, 0.0f, &x, &y);
    PICA_CHECK(x == -1.0f && y == -1.0f);
    mdk::pica::glowLevelCorner(1.0f, 1.0f, &x, &y);
    PICA_CHECK(x == 1.0f && y == 1.0f);
    mdk::pica::glowLevelCorner(1.0f, 0.0f, &x, &y);
    PICA_CHECK(x == 1.0f && y == -1.0f);
    mdk::pica::glowLevelCorner(0.5f, 0.25f, &x, &y);
    PICA_CHECK(x == 0.0f && y == -0.5f);
}

// The curve's constants are the ones the combiner can reach: the threshold and
// the band sum to 1; stage 2's factor Q / (4 band) is a byte at most 1; two x4
// stages make 16 Q u, which is u / u0 only if u0 = 1 / (16 Q); and the shine is
// at most the PC's peak lift over black, 0.15^(1/2.2) = 0.422.
void testTheGlowCurveIsReachableByTheCombiner() {
    const float t = mdk::pica::kGlowThresholdSrgb;
    const float band = mdk::pica::kGlowBand;
    PICA_CHECK(t + band > 0.9999f && t + band < 1.0001f);
    const float stage2 = mdk::pica::kGlowCurveQ / (4.0f * band);
    PICA_CHECK(stage2 > 0.0f && stage2 <= 1.0f);
    const float u0 = 1.0f / (16.0f * mdk::pica::kGlowCurveQ);
    PICA_CHECK(u0 > mdk::pica::kGlowCurveU0 - 0.001f && u0 < mdk::pica::kGlowCurveU0 + 0.001f);
    const float peak = mdk::pica::kGlowCurveP + mdk::pica::kGlowCurveQ;   // u = 1
    PICA_CHECK(peak > 0.0f && peak < 0.422f);
    // The level mix is (level 3 + level 5) / 2 with the blend colour's alpha at
    // 1/2: src * a + dst * (1 - a), alpha untouched.
    PICA_CHECK(mdk::pica::kBlendGlowMix.enable);
    PICA_CHECK(mdk::pica::kBlendGlowMix.srcColour == mdk::pica::kBlendConstantAlpha);
    PICA_CHECK(mdk::pica::kBlendGlowMix.dstColour == mdk::pica::kBlendOneMinusConstantAlpha);
    PICA_CHECK(mdk::pica::kBlendGlowMix.srcAlpha == mdk::pica::kBlendZero);
    PICA_CHECK(mdk::pica::kBlendGlowMix.dstAlpha == mdk::pica::kBlendOne);
    // The screen blend is the PC's: 1 - (1 - s)(1 - d) = s * (1 - d) + d.
    PICA_CHECK(mdk::pica::kBlendScreen.enable);
    PICA_CHECK(mdk::pica::kBlendScreen.srcColour == mdk::pica::kBlendOneMinusDstColour);
    PICA_CHECK(mdk::pica::kBlendScreen.dstColour == mdk::pica::kBlendOne);
    PICA_CHECK(mdk::pica::kBlendScreen.srcAlpha == mdk::pica::kBlendZero);
    PICA_CHECK(mdk::pica::kBlendScreen.dstAlpha == mdk::pica::kBlendOne);
}

} // namespace

namespace test {

// ---------------------------------------------------------------------------
// THE SUN'S SHADOW (Enhanced Mode enableSkyShadows): the two matrices against
// the PC's own sun matrix, the ramp against the map's encoding in both byte
// orders, the guards against self-shadowing, the factor, and the state.
// ---------------------------------------------------------------------------
void transformRow(const float m[16], const float p[3], float out[4]) {
    for (int c = 0; c < 4; c++) {
        out[c] = p[0] * m[0 * 4 + c] + p[1] * m[1 * 4 + c] + p[2] * m[2 * 4 + c] + m[3 * 4 + c];
    }
}

// What the shadow unit makes of a texel: the depth (24 bits) and the softness.
void decodeShadowTexel(const uint8_t rgba[4], mdk::pica::ShadowByteOrder order,
                       uint32_t* depth, uint32_t* softness) {
    if (order == mdk::pica::kShadowBytesReversed) {
        *depth = ((uint32_t)rgba[3] << 16) | ((uint32_t)rgba[2] << 8) | rgba[1];
        *softness = rgba[0];
    } else {
        *depth = ((uint32_t)rgba[0] << 16) | ((uint32_t)rgba[1] << 8) | rgba[2];
        *softness = rgba[3];
    }
}

void testTheSunsShadow() {
    // --- the matrices, from the PC's sunShadowMatrix (level 3's own sun) ---
    const mdk::Vec3 sun = {-0.33f, 0.47f, -0.82f};
    const mdk::Vec3 centre = {12.0f, 500.0f, 150.0f};
    float pc[16];
    mdk::sunShadowMatrix(sun, centre, 300.0f, (uint32_t)mdk::pica::kSunShadowMapSize, pc);
    float caster[16];
    float map[16];
    mdk::pica::sunCasterMatrix(pc, caster);
    mdk::pica::sunMapMatrix(pc, map);
    float mapRegisters[16];
    mdk::pica::registers(map, mapRegisters);
    const float points[5][3] = {{12.0f, 500.0f, 150.0f},   // the centre
                                {100.0f, 600.0f, 100.0f},
                                {-200.0f, 400.0f, 200.0f},
                                {40.0f, 520.0f, 1500.0f},  // towards the sun
                                {0.0f, 480.0f, -900.0f}};  // away from it
    for (int i = 0; i < 5; i++) {
        float atPc[4];
        float atCaster[4];
        float atMap[4];
        transformRow(pc, points[i], atPc);
        transformRow(caster, points[i], atCaster);
        transformRow(map, points[i], atMap);
        PICA_CHECK(atPc[3] == 1.0f);   // orthographic: no divide anywhere below
        // The caster pass: the PC's x and y, and a clip z whose STORED depth (the
        // backend's depth map) is the PC's 0-1 depth.
        PICA_CHECK(atCaster[0] == atPc[0] && atCaster[1] == atPc[1] && atCaster[3] == 1.0f);
        PICA_CHECK(fabsf(mdk::pica::storedDepth(atCaster[2]) - atPc[2]) < 1.0e-6f);
        PICA_CHECK(atCaster[2] >= -1.0f && atCaster[2] <= 0.0f);   // inside the PICA's clip z
        // The caster shader's own depth (clip z + w) is the same number.
        PICA_CHECK(fabsf((atCaster[2] + atCaster[3]) - atPc[2]) < 1.0e-6f);
        // The receive pass: the texel the caster pass put this point in (a
        // texture target's clip x, y are its s, t: glowLevelCorner) and the
        // same depth.
        float cornerX = 0.0f;
        float cornerY = 0.0f;
        mdk::pica::glowLevelCorner(atMap[0], atMap[1], &cornerX, &cornerY);
        PICA_CHECK(fabsf(cornerX - atCaster[0]) < 1.0e-5f && fabsf(cornerY - atCaster[1]) < 1.0e-5f);
        PICA_CHECK(fabsf(atMap[2] - atPc[2]) < 1.0e-7f && atMap[3] == 1.0f);
        // ... and the geometry shader reads those registers to the same three.
        const float p[4] = {points[i][0], points[i][1], points[i][2], 1.0f};
        PICA_CHECK(fabsf(shaderRow(mapRegisters, 0, p) - atMap[0]) < 1.0e-5f);
        PICA_CHECK(fabsf(shaderRow(mapRegisters, 1, p) - atMap[1]) < 1.0e-5f);
        PICA_CHECK(fabsf(shaderRow(mapRegisters, 2, p) - atMap[2]) < 1.0e-6f);
    }
    // The centre is the middle of the map (to the whole texel it is snapped to)
    // and of its depth range.
    {
        float atMap[4];
        transformRow(map, points[0], atMap);
        const float texel = 1.0f / (float)mdk::pica::kSunShadowMapSize;
        PICA_CHECK(fabsf(atMap[0] - 0.5f) <= texel && fabsf(atMap[1] - 0.5f) <= texel);
        PICA_CHECK(fabsf(atMap[2] - 0.5f) < 1.0e-4f);
    }

    // --- the ramp: a depth sampled at (s, t) = (d, 256 d) is the map's word ---
    const float depths[12] = {0.0f, 0.00001f, 0.25f, 0.4999f, 0.5f, 0.50001f, 0.5039f,
                              128.0f / 256.0f + 255.5f / 65536.0f, 0.75f, 0.999f,
                              0.99998f, 1.0f};
    for (int orderIndex = 0; orderIndex < 2; orderIndex++) {
        const mdk::pica::ShadowByteOrder order =
            orderIndex == 0 ? mdk::pica::kShadowBytesPica : mdk::pica::kShadowBytesReversed;
        for (int i = 0; i < 12; i++) {
            const float d = depths[i];
            // NEAREST: the column is floor(s * 512), clamped; the row is
            // floor(frac(t) * 256) counted from the LAST memory row.
            int column = (int)(d * (float)mdk::pica::kSunShadowRampWidth);
            if (column > mdk::pica::kSunShadowRampWidth - 1) {
                column = mdk::pica::kSunShadowRampWidth - 1;
            }
            const float t = d * 256.0f;
            int rowFromBottom = (int)((t - floorf(t)) * 256.0f);
            if (d >= 1.0f) {
                rowFromBottom = 255;   // the clamped column's last row: depth 1 is the far plane
            }
            const int memoryRow = mdk::pica::kSunShadowRampHeight - 1 - rowFromBottom;
            uint8_t rgba[4];
            mdk::pica::sunShadowRampTexel(column, memoryRow, order, rgba);
            uint32_t depth = 0;
            uint32_t softness = 0;
            decodeShadowTexel(rgba, order, &depth, &softness);
            PICA_CHECK(softness == 0xFFu);   // "lit" answers 1, not a shade
            // The depth's 16 high bits, the low byte 0: rounded DOWN, never up.
            uint32_t want = (uint32_t)(d * 65536.0f);
            if (want > 65535u) {
                want = 65535u;
            }
            PICA_CHECK(depth == (want << 8));
            PICA_CHECK(fabsf(mdk::pica::sunShadowStoredDepth(d) - (float)depth / 16777215.0f) <
                       1.0e-6f);
            PICA_CHECK(mdk::pica::sunShadowStoredDepth(d) <= d + 1.0e-7f);
        }
        // THE TWO COORDINATES DISAGREEING AT A STEP'S BOUNDARY (they are
        // interpolated separately): the column from a depth a hair BELOW k/256
        // with the row from a depth a hair ABOVE it, and the reverse. Either
        // way the texel must be one of the two true neighbours -- the end of
        // step k - 1 or the start of step k -- never a whole step away.
        for (int k = 1; k < 256; k += 17) {
            const float boundary = (float)k / 256.0f;
            const float below = boundary - 1.0f / 131072.0f;   // half a 16-bit step
            const float above = boundary + 1.0f / 131072.0f;
            const int columnBelow = (int)(below * (float)mdk::pica::kSunShadowRampWidth);
            const int columnAbove = (int)(above * (float)mdk::pica::kSunShadowRampWidth);
            const int rowBelow = (int)((below * 256.0f - floorf(below * 256.0f)) * 256.0f);
            const int rowAbove = (int)((above * 256.0f - floorf(above * 256.0f)) * 256.0f);
            PICA_CHECK(columnBelow == 2 * k - 1 && columnAbove == 2 * k);
            PICA_CHECK(rowBelow == 255 && rowAbove == 0);
            const int pairs[4][2] = {{columnBelow, rowBelow},     // agree: the end of k - 1
                                     {columnAbove, rowAbove},     // agree: the start of k
                                     {columnBelow, rowAbove},     // the column lags
                                     {columnAbove, rowBelow}};    // the row lags
            const uint32_t endOfPrevious = ((uint32_t)(k - 1) << 16) | (255u << 8);
            const uint32_t startOfThis = (uint32_t)k << 16;
            for (int pair = 0; pair < 4; pair++) {
                uint8_t rgba[4];
                mdk::pica::sunShadowRampTexel(
                    pairs[pair][0], mdk::pica::kSunShadowRampHeight - 1 - pairs[pair][1], order,
                    rgba);
                uint32_t depth = 0;
                uint32_t softness = 0;
                decodeShadowTexel(rgba, order, &depth, &softness);
                PICA_CHECK(softness == 0xFFu);
                PICA_CHECK(depth == (pair == 0 || pair == 3 ? endOfPrevious : startOfThis));
            }
        }
        // The two orders are each other's reverse, byte for byte.
        uint8_t a[4];
        uint8_t b[4];
        // (Column 75 is the second half of step 37; memory row 55 is row 200 from
        // the bottom, in that half: the two agree.)
        mdk::pica::sunShadowRampTexel(75, 55, mdk::pica::kShadowBytesPica, a);
        mdk::pica::sunShadowRampTexel(75, 55, mdk::pica::kShadowBytesReversed, b);
        PICA_CHECK(a[0] == b[3] && a[1] == b[2] && a[2] == b[1] && a[3] == b[0]);
        PICA_CHECK(a[0] == 37 && a[1] == 200 && a[2] == 0 && a[3] == 0xFF);
    }

    // --- the guards: the PC's, scaled to the map's texel ---
    PICA_CHECK(mdk::pica::sunShadowNormalOffset(mdk::pica::kSunShadowPcMapSize) == 0.5f);
    PICA_CHECK(mdk::pica::sunShadowNormalOffset(512) == 2.0f);
    PICA_CHECK(mdk::pica::sunShadowNormalOffset(256) == 4.0f);
    PICA_CHECK(mdk::pica::sunShadowDepthBias(mdk::pica::kSunShadowPcMapSize) ==
               0.5f / (2.0f * mdk::kSunShadowReach));
    PICA_CHECK(mdk::pica::sunShadowDepthBias(512) == 2.0f / (2.0f * mdk::kSunShadowReach));
    // The push is the same 1.7 texels at every size (a texel is 600 / size).
    for (int size = 256; size <= 2048; size *= 2) {
        const float texels = mdk::pica::sunShadowNormalOffset(size) / (600.0f / (float)size);
        PICA_CHECK(fabsf(texels - 0.5f / (600.0f / 2048.0f)) < 1.0e-4f);
    }
    // A surface does not shadow itself: what the map stores for a depth is in
    // front of that depth pulled by the bias only if something else is.
    for (int i = 0; i < 12; i++) {
        const float d = depths[i];
        if (d < 0.001f) {
            continue;   // nearer the sun than the bias: nothing is in front of it
        }
        const float receiver = d - mdk::pica::sunShadowDepthBias(512);
        PICA_CHECK(mdk::pica::sunShadowStoredDepth(d) > receiver);              // itself: lit
        PICA_CHECK(mdk::pica::sunShadowStoredDepth(d - 0.001f) <= receiver);    // 8 units nearer: shadow
    }

    // --- the factor the receive pass multiplies by ---
    PICA_CHECK(mdk::pica::sunShadowRatio(0.8f, 0.8f) == 1.0f);          // no sun on the face
    PICA_CHECK(fabsf(mdk::pica::sunShadowRatio(1.0f, 0.5f) - powf(0.5f, 0.45454545f)) < 1.0e-5f);
    PICA_CHECK(mdk::pica::sunShadowRatio(1.0f, 0.5f) < 1.0f);
    // Both past the lit program's white (F = 2 at 2^2.2): the shadow changes nothing.
    PICA_CHECK(mdk::pica::sunShadowRatio(9.0f, 5.0f) == 1.0f);
    // Only the lit one past it: the ratio is to the CLAMPED light.
    PICA_CHECK(fabsf(mdk::pica::sunShadowRatio(9.0f, 1.0f) - 0.5f) < 1.0e-5f);
    PICA_CHECK(mdk::pica::sunShadowRatio(0.0f, 0.0f) <= 1.0f);          // no division by zero
    PICA_CHECK(mdk::pica::sunShadowRatio(0.5f, 1.0f) == 1.0f);          // never brightens

    // --- who casts and receives, and what has holes ---
    using mdk::Pass;
    using mdk::Primitive;
    using mdk::Shading;
    PICA_CHECK(mdk::pica::castsSunShadow(Pass::Solid, Shading::Lit, Primitive::Triangles));
    PICA_CHECK(mdk::pica::castsSunShadow(Pass::DoubleSided, Shading::Lit, Primitive::Triangles));
    PICA_CHECK(!mdk::pica::castsSunShadow(Pass::Blended, Shading::Lit, Primitive::Triangles));
    PICA_CHECK(!mdk::pica::castsSunShadow(Pass::Mirror, Shading::Lit, Primitive::Triangles));
    PICA_CHECK(!mdk::pica::castsSunShadow(Pass::OnTop, Shading::Lit, Primitive::Triangles));
    PICA_CHECK(!mdk::pica::castsSunShadow(Pass::Overlay, Shading::Lit, Primitive::Triangles));
    PICA_CHECK(!mdk::pica::castsSunShadow(Pass::Solid, Shading::Original, Primitive::Triangles));
    PICA_CHECK(!mdk::pica::castsSunShadow(Pass::Solid, Shading::Sprite, Primitive::Triangles));
    PICA_CHECK(!mdk::pica::castsSunShadow(Pass::Solid, Shading::Lit, Primitive::Lines));
    const uint8_t solidIndices[4] = {1, 2, 3, 255};
    const uint8_t holedIndices[4] = {1, 2, 0, 255};
    PICA_CHECK(!mdk::pica::textureSourceHasHoles(solidIndices, 4, true));
    PICA_CHECK(mdk::pica::textureSourceHasHoles(holedIndices, 4, true));
    const uint8_t solidRgba[8] = {0, 0, 0, 128, 0, 0, 0, 255};     // a black texel is not a hole
    const uint8_t holedRgba[8] = {255, 255, 255, 255, 9, 9, 9, 127};
    PICA_CHECK(!mdk::pica::textureSourceHasHoles(solidRgba, 8, false));
    PICA_CHECK(mdk::pica::textureSourceHasHoles(holedRgba, 8, false));
    PICA_CHECK(!mdk::pica::textureSourceHasHoles(nullptr, 8, true));
    PICA_CHECK(!mdk::pica::textureSourceHasHoles(solidIndices, 0, true));

    // --- the state: multiply, depth-equal, and the VRAM arithmetic ---
    PICA_CHECK(mdk::pica::kBlendShadowMultiply.enable);
    PICA_CHECK(mdk::pica::kBlendShadowMultiply.srcColour == mdk::pica::kBlendDstColour);
    PICA_CHECK(mdk::pica::kBlendShadowMultiply.dstColour == mdk::pica::kBlendZero);
    PICA_CHECK(mdk::pica::kBlendShadowMultiply.srcAlpha == mdk::pica::kBlendZero);
    PICA_CHECK(mdk::pica::kBlendShadowMultiply.dstAlpha == mdk::pica::kBlendOne);
    PICA_CHECK(mdk::pica::kDepthShadowReceive.test && !mdk::pica::kDepthShadowReceive.write &&
               mdk::pica::kDepthShadowReceive.equal);
    for (int pass = 0; pass < static_cast<int>(Pass::Count); pass++) {
        PICA_CHECK(!mdk::pica::depthStateForPass(static_cast<Pass>(pass)).equal);
    }
    PICA_CHECK(mdk::pica::sunShadowVramBytes(512) == 1572864u);
    PICA_CHECK(mdk::pica::sunShadowVramBytes(256) == 393216u);
    // The glow's share covers its textures: 256x512 and levels 1-5 and the mix.
    const size_t glowTexels = 256u * 512u + 128u * 256u + 64u * 128u + 32u * 64u + 16u * 32u +
                              8u * 16u + 32u * 64u;
    PICA_CHECK(mdk::pica::kGlowVramBytes >= glowTexels * 4u);
}

int runPicaMatrixTests() {
    testIdentityRecognition();
    testTheGlowQuadMapsThePanelOntoThePicture();
    testTheGlowCurveIsReachableByTheCombiner();
    testTheGlowLevelPassIsTheIdentity();
    testIdentityWorldChangesNothing();
    testThePicaRecipe();
    testTheEyeOffset();
    testAWorldTranslationPlacesThePoint();
    testTheWorldComposesOnTheLeftOfTheView();
    testTheRegistersAreColumns();
    testTheShaderReadsTheRegistersTheWayTheMatrixMeansThem();
    testTheCanvasUsesTheScenesQuarterTurn();
    testTheDepthStateComesFromThePass();
    testTheOpaquePassesDoNotBlend();
    testTheBlendedPassUsesTheOraclesFactors();
    testAddDiffersOnlyInTheDestinationColour();
    testTheDiscardIsAlphaAboveZero();
    testTheDepthMapIncreasesWithDistance();
    testTheClearedBufferAcceptsTheWholeFrustum();
    testNearerFragmentsWinAndFartherOnesLose();
    testTheRealMatrixPutsTheNearPlaneNearest();
    testTheEyePlanFollowsTheSlider();
    testTheClearColourByteOrder();
    testTheTexelByteOrder();
    testTheTexelTileOrder();
    testSceneUvRunsFromTheLastRow();
    testTheHazeIsThePcCurveOnTheRightDraws();
    testTheFilteredEdge();
    testTheMipChain();
    testTheHdResidencyPolicy();
    testTheSunsShadow();
    return failures;
}

} // namespace test
} // namespace mdk
