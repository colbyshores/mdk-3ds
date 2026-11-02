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
//
// The inputs are all small integers and halves, so every product and sum below
// is exact in binary and the comparisons are exact. Where a tolerance is used
// (case 6) it is because two association orders of one product are compared,
// not because the arithmetic is inexact.
// ============================================================================

#include "pica_matrix_tests.h"

#include <stdio.h>
#include <string.h>

#include "render/pica_matrix.h"

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
float shaderRow(const float reg[16], int i, const float p[4]) {
    return reg[i * 4 + 0] * p[0] + reg[i * 4 + 1] * p[1] + reg[i * 4 + 2] * p[2] +
           reg[i * 4 + 3] * p[3];
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
    // i holds COLUMN i, i.e. elements i, 4+i, 8+i, 12+i.
    float m[16];
    for (int i = 0; i < 16; i++) {
        m[i] = (float)i;
    }
    float reg[16];
    mdk::pica::registers(m, reg);
    const float want[16] = {
        0, 4, 8, 12,
        1, 5, 9, 13,
        2, 6, 10, 14,
        3, 7, 11, 15,
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

} // namespace

namespace test {

int runPicaMatrixTests() {
    testIdentityRecognition();
    testIdentityWorldChangesNothing();
    testThePicaRecipe();
    testTheEyeOffset();
    testAWorldTranslationPlacesThePoint();
    testTheWorldComposesOnTheLeftOfTheView();
    testTheRegistersAreColumns();
    testTheShaderReadsTheRegistersTheWayTheMatrixMeansThem();
    testTheDepthStateComesFromThePass();
    testTheDepthMapIncreasesWithDistance();
    testTheClearedBufferAcceptsTheWholeFrustum();
    testNearerFragmentsWinAndFartherOnesLose();
    testTheRealMatrixPutsTheNearPlaneNearest();
    testTheEyePlanFollowsTheSlider();
    return failures;
}

} // namespace test
} // namespace mdk
