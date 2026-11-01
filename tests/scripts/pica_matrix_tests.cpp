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
    return failures;
}

} // namespace test
} // namespace mdk
