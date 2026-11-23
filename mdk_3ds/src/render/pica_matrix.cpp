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

#include <math.h>
#include <string.h>

#include "render/sun_shadow.h"

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

void eyeViewProjectionEngine(const float view[16], const float projection[16],
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
    multiply(shifted, projection, out);
}

void eyeViewProjection(const float view[16], const float projection[16],
                       float eyeOffset, float out[16]) {
    float m[16];
    eyeViewProjectionEngine(view, projection, eyeOffset, m);

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

void frustumPlanes(const float vp[16], float planes[6][4]) {
    // The engine's clip convention (row-vector, row-major): clip.x = dot(p4,
    // COLUMN 0 of vp), and so on for y/z/w, with clip.w > 0 in front and NDC z
    // in [0,1] between the planes. So the six side tests
    //     -w <= x, x <= w, -w <= y, y <= w, 0 <= z, z <= w
    // are dot products with column sums and differences -- Gribb-Hartmann read
    // in the row-vector convention, where the planes are COLUMNS, not rows.
    // The near plane is column 2 (z >= 0) and the far plane column 3 - column 2
    // (w - z >= 0), which is what the engine's [0,1] depth makes them.
    for (int j = 0; j < 4; j++) {
        // Component j of COLUMN k is element [j][k] = vp[j * 4 + k] -- the
        // plane's component that multiplies p_j. Reading vp[k * 4 + j] instead
        // reads ROW k, which is the transpose: wrong for every camera that is
        // not axis-aligned (the identity check passes either way, which is how
        // this bug hides).
        const float c0 = vp[j * 4 + 0], c1 = vp[j * 4 + 1];
        const float c2 = vp[j * 4 + 2], c3 = vp[j * 4 + 3];
        planes[0][j] = c3 + c0;  // left
        planes[1][j] = c3 - c0;  // right
        planes[2][j] = c3 + c1;  // bottom
        planes[3][j] = c3 - c1;  // top
        planes[4][j] = c2;       // near
        // The far plane is z_ndc = 1 -- the SAME plane the projection matrix
        // itself places: beyond it the hardware clips the triangle (the PICA
        // clip cube is the converted image of this volume), so culling here
        // drops nothing the rasteriser would have shown.
        planes[5][j] = c3 - c2;  // far
    }
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
    // Register i holds COLUMN i of `m`, and the four words of a register are
    // stored W, Z, Y, X -- see the declaration for the three measurements that
    // establish that. Reading the column bottom-up is therefore what puts the
    // column's first element in the slot the shader calls `.x`.
    //
    // The previous version of this loop wrote the column top-down, which delivered
    // every register component-reversed: the shader's `.x` got the engine matrix's
    // TRANSLATION row and its `.w` got row 0. That is why an identity MVP painted
    // nothing, why the 2D canvas painted nothing, and why the level's view is a
    // sunburst instead of a corridor.
    for (int i = 0; i < 4; i++) {
        out[i * 4 + 0] = m[3 * 4 + i]; // register w <- row 3 of m
        out[i * 4 + 1] = m[2 * 4 + i]; // register z <- row 2 of m
        out[i * 4 + 2] = m[1 * 4 + i]; // register y <- row 1 of m
        out[i * 4 + 3] = m[0 * 4 + i]; // register x <- row 0 of m
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
    // The ORIGINAL look's predicate, not the predicate the hardware holds: the
    // backend installs `C3D_AlphaTest(true, GPU_GREATER, reference)` through
    // installAlphaTest with `alphaTestReferenceFor(filtering, pass)`, which is
    // kAlphaTestReference whenever Enhanced Mode filtering is off, and never
    // calls this (review-4 F5). Only the reference is shared. What holds the SPELLING
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

float hazeExponent(float density) {
    // log2(e): ex2(x) = 2^x, so 2^(-density * log2(e) * d) = e^(-density * d).
    return density > 0.0f ? -density * 1.44269504089f : 0.0f;
}

bool hazeApplies(bool toggleOn, const Lighting& lighting, Shading shading, Pass pass) {
    return toggleOn && lighting.enabled && lighting.haze_density > 0.0f &&
           shading != Shading::Original && pass != Pass::Overlay;
}

uint32_t hazeColourWord(const Colour& colour) {
    const float c[3] = {colour.r, colour.g, colour.b};
    uint32_t word = 0xFF000000u;
    for (int32_t k = 0; k < 3; k++) {
        float v = c[k] < 0.0f ? 0.0f : (c[k] > 1.0f ? 1.0f : c[k]);
        word |= (uint32_t)(v * 255.0f + 0.5f) << (8 * k);
    }
    return word;
}

// Inverse of morton8InTile: slot m -> (x, y) within the 8x8 tile. Used by the
// blur (stage placement and the Morton-order write walk) and by the mip
// downsample's tile walk.
static const uint8_t kMortonInvX[64] = {
    0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3, 4, 5, 4, 5, 6, 7, 6, 7, 4, 5, 4, 5, 6, 7, 6, 7,
    0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3, 4, 5, 4, 5, 6, 7, 6, 7, 4, 5, 4, 5, 6, 7, 6, 7};
static const uint8_t kMortonInvY[64] = {
    0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3, 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3,
    4, 4, 5, 5, 4, 4, 5, 5, 6, 6, 7, 7, 6, 6, 7, 7, 4, 4, 5, 5, 4, 4, 5, 5, 6, 6, 7, 7, 6, 6, 7, 7};

void prepareForFiltering(uint8_t* tiled, int32_t width, int32_t height, int32_t paddedWidth,
                         int32_t paddedHeight, bool realHasTransparent) {
    if (!tiled || width <= 0 || height <= 0 || width > paddedWidth || height > paddedHeight) {
        return;
    }
    // [A, B, G, R] per texel (texelBytes).
    const uint32_t pw = (uint32_t)paddedWidth;
    // (1) the colour under each transparent real texel. Only OPAQUE neighbours are
    // read and only alpha-0 texels are written (their alpha untouched), so the
    // pass is order-independent: no written texel is ever read as a source.
    // Skipped when the caller knows the real region has no alpha-0 texel: with
    // none to write, this is a no-op, and it is the nine-Morton-lookups-a-texel
    // cost (see the header). Step 2 still runs -- padding is always transparent.
    //
    // TILE-STAGED. The per-texel form scattered up to nine Morton lookups and a
    // cache miss apiece over the whole buffer; measured 2026-11-22 on
    // old3ds-lan167 (L2 off): 348 ms for a 1024x256 sky. Staging each 8x8
    // tile's 10x10 neighbourhood turns a fully transparent tile's 576 lookups
    // into 100 and makes every neighbour read local. The stage is filled before
    // the tile's writes, and writes never change an alpha -- so a texel written
    // by an earlier tile still reads as alpha-0 here exactly as it did reading
    // `tiled` live. Same sources, same means, same slots.
    if (realHasTransparent) {
        // The mean without a divide: the ARM11 has no integer divide, and this
        // can run mid-frame when an evicted texture is made again (06_draw.inc).
        // ceil(65536 / n) is exact here: a sum is at most 8 * 255, so the
        // product's error stays below 0.03 and no mean's fraction (at most
        // 6/7) is pushed over an integer.
        static const uint32_t kReciprocal[9] = {0, 65536, 32768, 21846, 16384,
                                                13108, 10923, 9363, 8192};
        for (int32_t ty = 0; ty < height; ty += 8) {
            for (int32_t tx = 0; tx < width; tx += 8) {
                uint8_t stage[10][10][4];
                // The interior is the tile itself: one sequential 256-byte
                // walk, placed by the inverse Morton table. The ring around it
                // is 36 texels of the eight neighbouring tiles -- read where
                // they live rather than 100 scattered lookups a tile (the
                // 10x10 scatter was itself the miss storm after the first
                // staging pass: 182 ms a 1024x256 sky, measured 2026-11-22).
                // Texels past the real region sit in padding, which step 1
                // always finds alpha 0 (the expansion zeroed it and step 2 has
                // not run), so they read as non-sources exactly as the bounds
                // check made them before.
                const uint32_t tilesPerRow = pw >> 3;
                const uint32_t txi = (uint32_t)tx >> 3;
                const uint32_t tyi = (uint32_t)ty >> 3;
                uint8_t* tile = tiled + ((size_t)tyi * tilesPerRow + txi) * 256u;
                for (uint32_t m = 0; m < 64u; m++) {
                    memcpy(stage[kMortonInvY[m] + 1][kMortonInvX[m] + 1], tile + m * 4u, 4);
                }
                const uint32_t paddedTilesW = (uint32_t)paddedWidth >> 3;
                const uint32_t paddedTilesH = (uint32_t)paddedHeight >> 3;
                // top and bottom rows of the ring (gy = ty-1 / ty+8).
                for (int32_t sx = 1; sx <= 8; sx++) {
                    if (tyi > 0) {
                        memcpy(stage[0][sx],
                               tiled + ((size_t)(tyi - 1) * tilesPerRow + txi) * 256u +
                                   morton8InTile((uint32_t)(sx - 1), 7u) * 4u, 4);
                    } else {
                        memset(stage[0][sx], 0, 4);
                    }
                    if (tyi + 1 < paddedTilesH) {
                        memcpy(stage[9][sx],
                               tiled + ((size_t)(tyi + 1) * tilesPerRow + txi) * 256u +
                                   morton8InTile((uint32_t)(sx - 1), 0u) * 4u, 4);
                    } else {
                        memset(stage[9][sx], 0, 4);
                    }
                }
                // left and right columns (gx = tx-1 / tx+8) and the corners.
                for (int32_t sy = 1; sy <= 8; sy++) {
                    if (txi > 0) {
                        memcpy(stage[sy][0],
                               tiled + ((size_t)tyi * tilesPerRow + txi - 1) * 256u +
                                   morton8InTile(7u, (uint32_t)(sy - 1)) * 4u, 4);
                        if (tyi > 0) {
                            memcpy(stage[0][0],
                                   tiled + ((size_t)(tyi - 1) * tilesPerRow + txi - 1) * 256u +
                                       morton8InTile(7u, 7u) * 4u, 4);
                        } else {
                            memset(stage[0][0], 0, 4);
                        }
                        if (tyi + 1 < paddedTilesH) {
                            memcpy(stage[9][0],
                                   tiled + ((size_t)(tyi + 1) * tilesPerRow + txi - 1) * 256u +
                                       morton8InTile(7u, 0u) * 4u, 4);
                        } else {
                            memset(stage[9][0], 0, 4);
                        }
                    } else {
                        memset(stage[sy][0], 0, 4);
                        memset(stage[0][0], 0, 4);
                        memset(stage[9][0], 0, 4);
                    }
                    if (txi + 1 < paddedTilesW) {
                        memcpy(stage[sy][9],
                               tiled + ((size_t)tyi * tilesPerRow + txi + 1) * 256u +
                                   morton8InTile(0u, (uint32_t)(sy - 1)) * 4u, 4);
                        if (tyi > 0) {
                            memcpy(stage[0][9],
                                   tiled + ((size_t)(tyi - 1) * tilesPerRow + txi + 1) * 256u +
                                       morton8InTile(0u, 7u) * 4u, 4);
                        } else {
                            memset(stage[0][9], 0, 4);
                        }
                        if (tyi + 1 < paddedTilesH) {
                            memcpy(stage[9][9],
                                   tiled + ((size_t)(tyi + 1) * tilesPerRow + txi + 1) * 256u +
                                       morton8InTile(0u, 0u) * 4u, 4);
                        } else {
                            memset(stage[9][9], 0, 4);
                        }
                    } else {
                        memset(stage[sy][9], 0, 4);
                        memset(stage[0][9], 0, 4);
                        memset(stage[9][9], 0, 4);
                    }
                }
                // Walk in SLOT order, not row order: the write then lands at
                // `tile + m*4`, sequential within the tile. The scattered
                // write-per-transparent-texel was the blur's remaining cost
                // once the fill was sequential (182 ms a 1024x256 sky,
                // measured 2026-11-22). Same texels, same stage reads.
                for (uint32_t m = 0; m < 64u; m++) {
                    const int32_t lx = kMortonInvX[m];
                    const int32_t ly = kMortonInvY[m];
                    const int32_t gx = tx + lx;
                    const int32_t gy = ty + ly;
                    if (gx >= width || gy >= height) {
                        continue;
                    }
                    const uint8_t* self = stage[ly + 1][lx + 1];
                    if (self[0] != 0) {
                        continue;
                    }
                    uint32_t sum[3] = {0, 0, 0};
                    uint32_t n = 0;
                    for (int32_t dy = -1; dy <= 1; dy++) {
                        for (int32_t dx = -1; dx <= 1; dx++) {
                            if (dx == 0 && dy == 0) {
                                continue;
                            }
                            const uint8_t* s = stage[ly + 1 + dy][lx + 1 + dx];
                            if (s[0] == 0) {
                                continue;
                            }
                            sum[0] += s[1];
                            sum[1] += s[2];
                            sum[2] += s[3];
                            n++;
                        }
                    }
                    if (n == 0) {
                        continue;
                    }
                    const uint32_t k = kReciprocal[n];
                    uint8_t* t = tile + m * 4u;
                    t[1] = (uint8_t)((sum[0] * k) >> 16);
                    t[2] = (uint8_t)((sum[1] * k) >> 16);
                    t[3] = (uint8_t)((sum[2] * k) >> 16);
                }
            }
        }
    }
    // (2) the clamp, one texel into the padding on each padded axis: the column
    // first, then the row (which also fills the corner from the copied column).
    if (width < paddedWidth) {
        for (int32_t y = 0; y < height; y++) {
            memcpy(tiled + textureTexelIndex((uint32_t)width, (uint32_t)y, pw) * 4u,
                   tiled + textureTexelIndex((uint32_t)(width - 1), (uint32_t)y, pw) * 4u, 4);
        }
    }
    if (height < paddedHeight) {
        const int32_t columns = width < paddedWidth ? width + 1 : width;
        for (int32_t x = 0; x < columns; x++) {
            memcpy(tiled + textureTexelIndex((uint32_t)x, (uint32_t)height, pw) * 4u,
                   tiled + textureTexelIndex((uint32_t)x, (uint32_t)(height - 1), pw) * 4u, 4);
        }
    }
}

int32_t mipLevelsFor(int32_t paddedWidth, int32_t paddedHeight) {
    int32_t levels = 0;
    int32_t w = paddedWidth;
    int32_t h = paddedHeight;
    while (levels < 15 && w >= 16 && h >= 16) {
        w >>= 1;
        h >>= 1;
        levels++;
    }
    return levels;
}

void downsampleTiled(const uint8_t* src, int32_t width, int32_t height, uint8_t* dst) {
    if (!src || !dst || width < 16 || height < 16) {
        return;
    }
    const uint32_t sw = (uint32_t)width;
    const uint32_t dw = sw >> 1;
    const uint32_t dh = (uint32_t)height >> 1;
    // TILE-SEQUENTIAL, AND THAT IS THE WHOLE POINT. The 2x2 block an output
    // texel averages is four CONSECUTIVE slots of one source tile --
    // morton(2u,2v) = morton(u,v)<<2, and +1/+2/+3 are the odd partners -- so
    // walking output tiles in order makes both sides sequential: 64 contiguous
    // 16-byte source reads, 64 contiguous output words. The per-texel form
    // scattered five Morton lookups and a cache miss apiece over the whole
    // level; measured 2026-11-22 on old3ds-lan167 (L2 off): 157 ms for a
    // 1024x256 level 0, ~480 cycles an output texel. Same texels, same mean,
    // same slots -- a walk order, not a formula change.
    const uint32_t srcTilesPerRow = sw >> 3;
    const uint32_t dstTilesPerRow = dw >> 3;
    for (uint32_t oty = 0; oty < dh; oty += 8u) {
        for (uint32_t otx = 0; otx < dw; otx += 8u) {
            uint8_t* out =
                dst + ((size_t)((oty >> 3) * dstTilesPerRow + (otx >> 3))) * 256u;
            // Source tile of slot m: the output tile's 16x16 source region
            // starts at source tile (2*otxTile, 2*otyTile) = (otx>>2, oty>>2);
            // the high bits of lx/ly pick which of those four tiles slot m's
            // block lives in, and the low 4 Morton bits, shifted up two, are
            // the block's first slot within it.
            const size_t srcBase =
                (size_t)(oty >> 2) * srcTilesPerRow + (size_t)(otx >> 2);
            for (uint32_t m = 0; m < 64u; m++) {
                const size_t s =
                    (srcBase + (size_t)(kMortonInvY[m] >> 2) * srcTilesPerRow +
                     (size_t)(kMortonInvX[m] >> 2)) * 256u +
                    ((size_t)(m & 0xFu) << 4);
                const uint8_t* p = src + s;
                for (int32_t k = 0; k < 4; k++) {
                    out[m * 4 + k] = (uint8_t)(((uint32_t)p[k] + p[4 + k] + p[8 + k] +
                                                p[12 + k] + 2u) >> 2);
                }
            }
        }
    }
}

size_t textureBytes(TexelFormat format, int32_t paddedWidth, int32_t paddedHeight,
                    int32_t levels) {
    const size_t texels = mipChainTexels(paddedWidth, paddedHeight, levels);
    switch (format) {
        case TexelFormat::Etc1: return texels / 2;   // 4 bits per texel
        case TexelFormat::Etc1A4: return texels;     // 8 bits per texel
        case TexelFormat::Rgb565:
        case TexelFormat::Rgba5551:
        case TexelFormat::Rgba4: return texels * 2;  // 16 bits per texel
        default: return texels * 4;                  // RGBA8
    }
}

CompressedRect compressedFrameRect(int32_t realWidth, int32_t realHeight, int32_t textureWidth,
                                   int32_t textureHeight) {
    CompressedRect rect;
    if (realWidth <= 0 || realHeight <= 0 || textureWidth <= 0 || textureHeight <= 0) {
        return rect;
    }
    const int32_t w = realWidth < textureWidth ? realWidth : textureWidth;
    const int32_t h = realHeight < textureHeight ? realHeight : textureHeight;
    rect.left = 0.0f;
    rect.top = 1.0f;
    rect.right = (float)w / (float)textureWidth;
    rect.bottom = 1.0f - (float)h / (float)textureHeight;
    return rect;
}

static int32_t potAtLeast8(int32_t value) {
    int32_t result = 8;
    while (result < value && result < 1024) {
        result *= 2;
    }
    return result;
}

size_t spriteFrameBytes(TexelFormat format, int32_t realWidth, int32_t realHeight, int32_t levels) {
    if (realWidth <= 0 || realHeight <= 0) {
        return 0;
    }
    return textureBytes(format, potAtLeast8(realWidth), potAtLeast8(realHeight), levels);
}

TexelFormat expandedTexelFormat(bool texture16, bool keepRgba8, int32_t paddedWidth,
                                int32_t paddedHeight, const uint8_t* level0, size_t level0Texels) {
    if (!texture16 || keepRgba8 || !level0 || level0Texels == 0 ||
        (size_t)paddedWidth * (size_t)paddedHeight <= 64u) {
        return TexelFormat::Rgba8;
    }
    bool opaque = true;
    bool binary = true;
    for (size_t i = 0; i < level0Texels; i++) {
        const uint8_t alpha = level0[i * 4u];   // [A, B, G, R] (texelBytes)
        if (alpha != 255u) {
            opaque = false;
            if (alpha != 0u) {
                binary = false;
                break;
            }
        }
    }
    if (opaque) {
        return TexelFormat::Rgb565;
    }
    return binary ? TexelFormat::Rgba5551 : TexelFormat::Rgba4;
}

TexelFormat indexTexelFormatBound(bool texture16, int32_t paddedWidth, int32_t paddedHeight) {
    if (!texture16 || (size_t)paddedWidth * (size_t)paddedHeight <= 64u) {
        return TexelFormat::Rgba8;
    }
    return TexelFormat::Rgba5551;
}

void packTexels16(uint8_t* chain, size_t texels, TexelFormat format) {
    if (!chain || (format != TexelFormat::Rgb565 && format != TexelFormat::Rgba5551 &&
                   format != TexelFormat::Rgba4)) {
        return;
    }
    // The format is loop-invariant; testing it per texel cost a branch a texel
    // over the whole chain (measured 2026-11-22: 140 ms for a 780x360 chain).
    // Three loops, one per format -- same words, same order, same in-place
    // compaction (read texel i whole before its word lands at 2i).
    if (format == TexelFormat::Rgb565) {
        for (size_t i = 0; i < texels; i++) {
            const uint8_t* t = chain + i * 4u;
            texel16Bytes(chain + i * 2u, rgb565Word(t[3], t[2], t[1]));
        }
    } else if (format == TexelFormat::Rgba5551) {
        for (size_t i = 0; i < texels; i++) {
            const uint8_t* t = chain + i * 4u;
            texel16Bytes(chain + i * 2u, rgba5551Word(t[3], t[2], t[1], t[0]));
        }
    } else {
        for (size_t i = 0; i < texels; i++) {
            const uint8_t* t = chain + i * 4u;
            texel16Bytes(chain + i * 2u, rgba4Word(t[3], t[2], t[1], t[0]));
        }
    }
}

void streamTexelsRgb565(const uint8_t* indices, int32_t width, int32_t height,
                        const uint8_t* paletteRgba, int32_t paddedWidth, uint8_t* out) {
    if (!indices || !paletteRgba || !out || width <= 0 || height <= 0 || paddedWidth < width) {
        return;
    }
    uint16_t words[256];
    words[0] = 0;   // index 0: black (see the header)
    for (int32_t i = 1; i < 256; i++) {
        const uint8_t* c = paletteRgba + i * 4;
        words[i] = rgb565Word(c[0], c[1], c[2]);
    }
    // morton8InTile(x, y) == mortonX[x] | mortonY[y]: x's bits on the even
    // positions, y's on the odd.
    uint32_t mortonX[8];
    uint32_t mortonY[8];
    for (uint32_t v = 0; v < 8u; v++) {
        mortonX[v] = morton8InTile(v, 0u);
        mortonY[v] = morton8InTile(0u, v);
    }
    const size_t tilesPerRow = (size_t)((uint32_t)paddedWidth >> 3);
    for (int32_t y = 0; y < height; y++) {
        const uint8_t* row = indices + (size_t)y * (size_t)width;
        const size_t rowBase = (size_t)((uint32_t)y >> 3) * tilesPerRow * 64u + mortonY[y & 7];
        for (int32_t x = 0; x < width; x++) {
            const size_t slot = rowBase + (size_t)((uint32_t)x >> 3) * 64u + mortonX[x & 7];
            texel16Bytes(out + slot * 2u, words[row[x]]);
        }
    }
}

size_t mipChainTexels(int32_t paddedWidth, int32_t paddedHeight, int32_t levels) {
    size_t total = 0;
    size_t w = (size_t)paddedWidth;
    size_t h = (size_t)paddedHeight;
    for (int32_t level = 0; level <= levels; level++) {
        total += w * h;
        w >>= 1;
        h >>= 1;
    }
    return total;
}

void buildMipChain(uint8_t* chain, int32_t paddedWidth, int32_t paddedHeight, int32_t realWidth,
                   int32_t realHeight, int32_t levels) {
    if (!chain) {
        return;
    }
    uint8_t* above = chain;
    int32_t w = paddedWidth;
    int32_t h = paddedHeight;
    int32_t rw = realWidth;
    int32_t rh = realHeight;
    for (int32_t level = 1; level <= levels && w >= 16 && h >= 16; level++) {
        uint8_t* below = above + (size_t)w * (size_t)h * 4u;
        downsampleTiled(above, w, h, below);
        w >>= 1;
        h >>= 1;
        rw = (rw + 1) >> 1;
        rh = (rh + 1) >> 1;
        prepareForFiltering(below, rw, rh, w, h);
        above = below;
    }
}

int32_t hdEvictionVictim(const HdSlot* slots, int32_t count, uint32_t protectFrom,
                         bool unlinkedOnly) {
    int32_t victim = -1;
    for (int32_t i = 0; i < count; i++) {
        if (!slots[i].resident || slots[i].lastUsed >= protectFrom ||
            (unlinkedOnly && slots[i].linked)) {
            continue;
        }
        if (victim < 0) {
            victim = i;
            continue;
        }
        // Unlinked before linked; then the least recently drawn.
        const bool better = slots[victim].linked != slots[i].linked
                                ? !slots[i].linked
                                : slots[i].lastUsed < slots[victim].lastUsed;
        if (better) {
            victim = i;
        }
    }
    return victim;
}

bool hdMayDispatch(size_t residentBytes, size_t readsInFlight, size_t largestBytes,
                   size_t budgetBytes, bool evictable) {
    return evictable || residentBytes + (readsInFlight + 1) * largestBytes <= budgetBytes;
}

void glowLevelCorner(float s, float t, float* clipX, float* clipY) {
    *clipX = 2.0f * s - 1.0f;
    *clipY = 2.0f * t - 1.0f;
}

void glowTexCoord(float fx, float fy, float* s, float* t) {
    // Framebuffer column = 239 - panel row, framebuffer row = panel column
    // (03_frame.inc writeScreenshotBmp's rotation). A texture's s runs along its
    // rows (the framebuffer's width) and t along its height -- DOWNWARD from the
    // first row in memory: t = 1 is memory row 0. Measured in Mandarine
    // (2026-11-18): with t = fx * 400/512 the blur came out mirrored left to
    // right and shifted by the 112 padding rows.
    *s = (1.0f - fy) * (240.0f / (float)kGlowTexWidth);
    *t = 1.0f - fx * (400.0f / (float)kGlowTexHeight);
}

void sunShadowRampTexel(int32_t x, int32_t y, ShadowByteOrder order, uint8_t rgba[4]) {
    // The row is the depth's middle byte; the column is its high byte and, in
    // its lowest bit, which half of that byte's step the depth is in.
    const int32_t row = kSunShadowRampHeight - 1 - y;
    int32_t step = x >> 1;
    const bool secondHalf = (x & 1) != 0;
    if (secondHalf && row < kSunShadowRampHeight / 4 && step < 255) {
        step++;      // the row is already past the boundary the column has not reached
    } else if (!secondHalf && row >= 3 * kSunShadowRampHeight / 4 && step > 0) {
        step--;      // the row has not reached the boundary the column is past
    }
    const uint8_t high = (uint8_t)step;
    const uint8_t middle = (uint8_t)row;
    if (order == kShadowBytesReversed) {
        rgba[0] = 0xFF;      // the softness
        rgba[1] = 0;         // the depth's low byte
        rgba[2] = middle;
        rgba[3] = high;
    } else {
        rgba[0] = high;
        rgba[1] = middle;
        rgba[2] = 0;         // the depth's low byte
        rgba[3] = 0xFF;      // the softness
    }
}

float sunShadowStoredDepth(float depth) {
    if (depth < 0.0f) {
        depth = 0.0f;
    }
    // Column = floor(depth * 256) (clamped to the last), row = floor(frac(depth
    // * 256) * 256): the 16 high bits of the 24-bit depth.
    float scaled = depth * 65536.0f;
    if (scaled > 65535.0f) {
        scaled = 65535.0f;
    }
    const uint32_t high16 = (uint32_t)scaled;
    return (float)(high16 << 8) / 16777215.0f;
}

float sunShadowNormalOffset(int32_t mapSize) {
    if (mapSize <= 0) {
        mapSize = kSunShadowMapSize;
    }
    return 0.5f * (float)kSunShadowPcMapSize / (float)mapSize;
}

float sunShadowDepthBias(int32_t mapSize) {
    return sunShadowNormalOffset(mapSize) / (2.0f * kSunShadowReach);
}

void sunCasterMatrix(const float sun[16], float out[16]) {
    // Row-vector layout: element [row * 4 + col]; a clip component is a COLUMN.
    for (int row = 0; row < 4; row++) {
        out[row * 4 + 0] = sun[row * 4 + 0];
        out[row * 4 + 1] = sun[row * 4 + 1];
        out[row * 4 + 2] = sun[row * 4 + 2] - sun[row * 4 + 3];
        out[row * 4 + 3] = sun[row * 4 + 3];
    }
}

void sunMapMatrix(const float sun[16], float out[16]) {
    for (int row = 0; row < 4; row++) {
        out[row * 4 + 0] = 0.5f * sun[row * 4 + 0] + 0.5f * sun[row * 4 + 3];
        out[row * 4 + 1] = 0.5f * sun[row * 4 + 1] + 0.5f * sun[row * 4 + 3];
        out[row * 4 + 2] = sun[row * 4 + 2];
        out[row * 4 + 3] = sun[row * 4 + 3];
    }
}

bool textureSourceHasHoles(const uint8_t* source, size_t byteCount, bool indexed) {
    if (!source) {
        return false;
    }
    if (indexed) {
        return memchr(source, 0, byteCount) != nullptr;
    }
    for (size_t at = 3; at < byteCount; at += 4) {
        if (source[at] < 128) {
            return true;
        }
    }
    return false;
}

bool castsSunShadow(Pass pass, Shading shading, Primitive primitive) {
    return primitive == Primitive::Triangles && shading == Shading::Lit &&
           (pass == Pass::Solid || pass == Pass::DoubleSided);
}

float sunShadowRatio(float lit, float shadowed) {
    // F / 2 of each (to_srgb, halved, clamped at 1: the lit program's light
    // stage scales by 2), then their quotient, never above 1.
    const float kInvGamma = 0.45454545f;
    float fShadow = 0.5f * powf(shadowed > 0.0f ? shadowed : 0.0f, kInvGamma);
    float fLit = 0.5f * powf(lit > 0.0f ? lit : 0.0f, kInvGamma);
    if (fShadow > 1.0f) {
        fShadow = 1.0f;
    }
    if (fLit > 1.0f) {
        fLit = 1.0f;
    }
    if (fLit < 1.0e-12f) {
        fLit = 1.0e-12f;
    }
    const float ratio = fShadow / fLit;
    return ratio > 1.0f ? 1.0f : ratio;
}

void sunShadowTintColumn(int32_t x, const float sky[3], const float ground[3], float exposure,
                         uint8_t rgb[3]) {
    const float kInvGamma = 0.45454545f;
    const float t = (float)x / (float)(kShadowTintWidth - 1);
    for (int c = 0; c < 3; c++) {
        const float light = exposure * (ground[c] + (sky[c] - ground[c]) * t);
        float f = 0.5f * powf(light > 0.0f ? light : 0.0f, kInvGamma);
        if (f > 1.0f) {
            f = 1.0f;
        }
        rgb[c] = (uint8_t)(f * 255.0f + 0.5f);
    }
}

uint8_t sunShadowTintAlpha(int32_t y) {
    const int32_t survive = (kShadowTintHeight - 1) - y;
    return (uint8_t)((survive * 255 + (kShadowTintHeight - 1) / 2) / (kShadowTintHeight - 1));
}

void sunShadowTintTexel(int32_t x, int32_t y, const float sky[3], const float ground[3],
                        float exposure, uint8_t rgba[4]) {
    sunShadowTintColumn(x, sky, ground, exposure, rgba);
    rgba[3] = sunShadowTintAlpha(y);
}

void sunShadowTintMap(float out[4]) {
    out[0] = (float)(kShadowTintWidth - 1) / (float)kShadowTintWidth;
    out[1] = 0.5f / (float)kShadowTintWidth;
    out[2] = (float)(kShadowTintHeight - 1) / (float)kShadowTintHeight;
    out[3] = 0.5f / (float)kShadowTintHeight;
}

uint8_t sunShadowTintAlphaDialled(int32_t y, int32_t darknessPercent) {
    const uint8_t alpha = sunShadowTintAlpha(y);
    if (darknessPercent >= 100) {
        return alpha;
    }
    if (darknessPercent <= 0) {
        return 0;
    }
    // Integer, rounded: alpha * percent / 100 (constant divisor; cold, a
    // refill of the 1024-texel tint when the dial moves).
    return (uint8_t)(((int32_t)alpha * darknessPercent + 50) / 100);
}

namespace {

// One colour's channels about its Rec. 709 luminance (level_light.cpp
// skyLuminance's weights), by `saturation`; never below 0.
void saturate(Colour* colour, float saturation) {
    const float l = colour->r * 0.2126f + colour->g * 0.7152f + colour->b * 0.0722f;
    const float r = l + (colour->r - l) * saturation;
    const float g = l + (colour->g - l) * saturation;
    const float b = l + (colour->b - l) * saturation;
    colour->r = r > 0.0f ? r : 0.0f;
    colour->g = g > 0.0f ? g : 0.0f;
    colour->b = b > 0.0f ? b : 0.0f;
}

} // namespace

void dialledLighting(const Lighting& rig, const EnhancedToggles& dials, Lighting* out) {
    *out = rig;
    if (dials.hazeDensity != 100) {
        out->haze_density = rig.haze_density * ((float)dials.hazeDensity / 100.0f);
    }
    if (dials.exposure != 100) {
        out->exposure = rig.exposure * ((float)dials.exposure / 100.0f);
    }
    if (dials.tintSaturation != 100) {
        const float s = (float)dials.tintSaturation / 100.0f;
        saturate(&out->sun, s);
        saturate(&out->sky, s);
        saturate(&out->ground, s);
    }
}

} // namespace pica
} // namespace mdk
