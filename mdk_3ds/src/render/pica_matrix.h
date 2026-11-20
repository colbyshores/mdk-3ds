#pragma once

#include <stddef.h>
#include <stdint.h>

#include "render/render_types.h"

// ============================================================================
// pica_matrix.h — the Citro3D backend's matrix math, split out so a host test
// can reach it.
//
// WHY THIS FILE EXISTS. The engine's matrices and the PICA200's are three
// conventions apart (see the long comment above `eyeViewProjection` below), and
// every one of the three conversions has a failure mode that shows up as
// INVISIBLE GEOMETRY rather than as a wrong picture: a transposed upload mirrors
// the world, a depth remap applied twice pushes everything behind the far plane,
// a world matrix multiplied on the wrong side orbits the camera instead of
// placing the object. None of it can be seen on the host, and the 3DS picture is
// UNVERIFIED on hardware (AGENTS.md P6), so the arithmetic is the only part of
// this that CAN be pinned here — and pinning it needs it to compile on the host,
// which a file that includes <3ds.h> and <citro3d.h> cannot.
//
// That is the whole of the reason for the split, and it is the same shape the
// Vulkan backend already uses for the sky's clip-to-direction inverse
// (`mdk_linux/src/render/system_numerics_inverse.{h,cpp}`, compiled into both
// its own target and `tests/scripts`). Nothing in here names a libctru type.
//
// WHAT IS NOT HERE: the per-frame bookkeeping (which eye is installed, whether
// the uniform already holds the frame's matrix, the cull mode cache). That is
// the backend's, and it cannot be tested without a GPU.
//
// WHAT ELSE IS HERE: the frame's DEPTH state, below. Same reason — a bit of GPU
// state that no picture on this host can show, so it is pinned instead.
//
// THE CONVENTION, in one paragraph. Every matrix in here is ROW-MAJOR with a
// ROW-VECTOR convention — a point is a row and it transforms as `p * M` — which
// is the engine's convention and the C# oracle's (`Renderer.cs:1072` composes
// `world * depth_pull * view * projection` in that order, and System.Numerics is
// row-vector too). The translation therefore lives in the FOURTH ROW (elements
// 12, 13, 14), not the fourth column.
// ============================================================================

namespace mdk {
namespace pica {

// True when `m` is EXACTLY the identity: 1.0f on the diagonal, 0.0f elsewhere.
//
// This is the marker the seam uses for "these vertices are already in world
// space", and it is a bit test, not a float comparison: `memcmp` against the
// identity's bytes. Two consequences, both deliberate:
//
//   * A matrix that is the identity to within rounding is NOT identity, and gets
//     the multiply. The test errs toward doing work, never toward skipping it.
//   * A matrix holding -0.0 where the identity holds +0.0 is NOT identity, for
//     the same reason. It would transform identically; it just costs a multiply.
//
// A ZERO matrix is not the identity and is not treated as one. `MeshDraw`'s
// default `world` is all zeros, so a producer that never sets one asks for a
// multiply by zero and collapses — which is exactly what the Vulkan backend
// already does with the same record, and the two targets agreeing is the point.
bool isIdentity(const float m[16]);

// The frame's per-eye matrix: the view-projection with BOTH PICA conversions
// baked in, in the engine's row-major row-vector layout.
//
// `eyeOffset` moves the eye along its own right axis (the stereo pair); 0 for a
// mono pass.
//
// THE THREE CONVENTIONS, which are the whole difficulty:
//
//   The engine's matrices are ROW-MAJOR with a ROW-VECTOR convention: the point
//   is a row and the transform is `p_clip = p_world * M`.
//
//   The PICA200's vertex shader is COLUMN-VECTOR: the transform is
//   `p_clip = M_pica * p_world`, and uniform register i holds COLUMN i of
//   M_pica.
//
//   For the two to describe the SAME transform we need M_pica = M^T, and the
//   column-major register layout means that M^T's column i is M's ROW i. So
//   register i gets our row i, verbatim, and the transpose is FREE — exactly the
//   "implicit transpose" the D3D->GL conversion already relies on. (Uploading our
//   memory verbatim would give M^T's ROW i, which is M's column i, i.e. the
//   transpose of what is wanted: a rotation would come out INVERTED. The two are
//   one transpose apart, and only one of them is right.)
//
// (A) THE DEPTH CONVERSION. The engine's projection is the C# oracle's
//     right-handed one, whose NDC z still runs [0,1] (0 at the near plane,
//     1 at the far), with clip.w = -v.z so a point in front has w > 0. PICA200
//     wants [-1,0]. The conversion is `row[2] -= row[3]`, in element terms
//     _33' = _33 - 1 and _43' = _43. _34 is LEFT ALONE: dividing the converted
//     z by the untouched w is exactly the -1 shift in NDC.
//
//     The conversion is applied ONCE, to the COMBINED matrix.
//
// (B) THE 90-DEGREE SCREEN ROTATION. The top screen is portrait-native (240x400)
//     and the game is landscape, so the projection carries a quarter turn. It is
//     a PROPER ROTATION (determinant +1): the new row 0 is the old row 1 and the
//     new row 1 is the NEGATED old row 0. The upstream picaGL version used a
//     reflection (determinant -1), which flips the winding a second time and
//     undoes the cull inversion.
//
// Both conversions are RIGHT-multiplications by fixed matrices, which is what
// makes `drawMatrix` below legal: for any world W,
//
//     (W * V * P) * E * R  ==  W * (V * P * E * R)
//
// so the conversions can be baked once per frame into the eye matrix and a
// per-draw world composed against the result.
void eyeViewProjection(const float view[16], const float projection[16],
                       float eyeOffset, float out[16]);

// The matrix ONE DRAW must upload: its world transform composed with the frame's
// per-eye matrix, i.e. `out = world * eyeVp` in the row-vector convention.
//
// `world` goes on the LEFT because a row vector meets it first: the oracle's
// `world * view * projection` (Renderer.cs:1072). Composing it on the right
// transforms the camera instead of the object, and the picture it produces is
// not "wrong", it is a different scene — which is why this is a named function
// with a test rather than a multiply at the call site.
//
// With `world` the identity the result is bit-identical to `eyeVp` (multiplying
// by an exact identity is exact), which is what lets the backend skip the uniform
// write for the level geometry: see `isIdentity`.
void drawMatrix(const float world[16], const float eyeVp[16], float out[16]);

// The uniform layout: register i holds COLUMN i of `m` — and a register's four
// words go into memory in the PICA's order, which is W, Z, Y, X, so
// out[i*4+0..3] = m[3*4+i], m[2*4+i], m[1*4+i], m[0*4+i].
//
// THE STRIDED READ. The shader consumes register i as the vector dotted with the
// position (`dp4 outpos.x, mvp[0], r0`), so register i is ROW i of the
// column-vector matrix M_pica, and M_pica = M^T for our row-vector M. Therefore
// register i must hold COLUMN i of our row-major matrix. A verbatim row copy
// uploads the transpose: rotations come out inverted and the picture is mirrored.
//
// THE WORD ORDER INSIDE A REGISTER IS NOT x, y, z, w, AND THIS IS THE BUG THAT
// ATE THE PICTURE. `C3D_FVec` (c3d/types.h:37-64) declares its members
// `w, z, y, x` and the header's own comment says the union "Matches PICA
// layout", so the FIRST word of a register is its W and the LAST is its X.
// Three independent readings of the shipped library agree:
//
//   * `Mtx_Scale` (libcitro3d.a, mtx_scale.o) multiplies offset 12 of every row
//     by its x argument: `.x` lives at offset 12, `.w` at offset 0.
//   * `Mtx_OrthoTilt` (mtx_orthotilt.o) writes the y scale to offset 8 of row 0
//     and the y translation to offset 0. Read as x,y,z,w that matrix projects the
//     scene with x and z swapped and no y term at all; read as w,z,y,x it maps
//     [bottom,top] onto [-1,+1] and — because it is a Tilt matrix — puts the
//     screen's x where the scene's y was, which is the documented behaviour.
//   * Nothing reorders on the way out: `C3D_FVUnifMtxNx4` (c3d/uniforms.h:30-36)
//     is a struct copy of `mtx->r[i]`, and `C3D_UpdateUniforms` (uniforms.o,
//     the `GPUCMD_Add` at +0xe8) passes `C3D_FVUnif + start*16` verbatim with
//     count*4 words. The staging buffer IS the register file.
//
// So a register written into memory in the order (x,y,z,w) reaches the shader
// component-reversed, which is what this function used to do, and the three
// measured symptoms of that are all in the record: an IDENTITY matrix painted
// nothing at all (every register became (0,0,0,1), so every vertex mapped to the
// single clip point (1,1,1,1)); the 2D canvas painted nothing at all (its
// registers came out (1,0,-2/360,0) etc., so clip x = clip w = pixel x and the
// whole HUD collapsed onto the panel corner); and the level's own view was the
// radial sunburst every committed panel crop shows, the picture the open items
// have been calling "the one-sided view". Forsaken — hardware-validated — never hit it
// because its matrix_to_c3d (render_c3d.c:1121) writes the NAMED fields
// `.x .y .z .w` in that order instead of raw slots, and the named fields are
// already reversed in memory. That is the line to read next to this one.
//
// `out` may be the `m` member of a C3D_Mtx (that union's raw view of the same 16
// floats), which is why this writes floats instead of a citro3d type — and the
// raw view is WZYX per row, which is why nothing else in this backend writes a
// matrix through it.
void registers(const float m[16], float out[16]);

// The matrix a CANVAS draw uploads: the orthographic projection of the canvas's
// pixel space onto the whole panel, with the PICA's quarter turn baked in.
//
// THE ORACLE'S, LINE FOR LINE. `Renderer.cs:736` builds the canvas transform as
//
//     CreateOrthographicOffCenter(-margin.X, CanvasWidth + margin.X,
//                                 CanvasHeight + margin.Y, -margin.Y, 0, 1)
//
// and hands it to the command as its WORLD (`Renderer.cs:1073`,
// `material.Pass == Pass.Overlay ? command.World : nearer * view.ViewProjection`).
// The canvas is therefore not composed with the eye matrix at all: it is already
// clip space. `margin` is the VR viewer's inset and is zero for every stereo mode
// this port builds (`StereoModes.CanvasZoom` returns 1 for None and SideBySide),
// so this function takes the width and no margin.
//
// THE y AXIS, STATED RIGHT (review-1 F2, and it replaces a version of this
// comment that had it backwards). System.Numerics expands
// `CreateOrthographicOffCenter(left, right, bottom, top, zNear, zFar)` as
// `M22 = 2/(top-bottom)`, `M42 = (top+bottom)/(bottom-top)`. The oracle's call
// passes `bottom = CanvasHeight + margin.Y` and `top = -margin.Y` — the two ARE
// swapped against the intuitive reading, and that swap is the whole point — so
// with margin 0:
//
//     M22 = 2/(0 - H) = -2/H      M42 = (0 + H)/(H - 0) = +1
//
// i.e. THE ORACLE IS y-UP: canvas y = 0 (its top row of pixels) lands at clip
// +1. That is what this function builds — `m[5] = -2/H`, `m[13] = +1` — so the
// 3DS canvas agrees with the oracle LINE FOR LINE and there is NO negation
// against it. The earlier text here claimed the oracle expanded to `+2/H, -1`
// and that this target therefore negated the y row; both halves were wrong, the
// expansion and the consequence, and the numbers did not move when the sentence
// was corrected, which is how it survived a review.
//
// THE VULKAN BACKEND IS THE ONE THAT DIFFERS, AND FOR A REAL REASON:
// `renderer_vk.cpp`'s `canvasOrtho` writes `out[5] = +2/H`, `out[13] = -1`,
// which is the oracle's matrix with the clip y negated — the Vulkan viewport
// transform is y-DOWN, so a Vulkan build has to flip what System.Numerics
// produced. That flip belongs to the API that needs it and not to the matrix.
//
// `tests/scripts/pica_matrix_tests.cpp`'s canvas row now starts from the
// oracle's expansion and states, as a named step, that this target copies it
// unchanged; the Vulkan difference is recorded there too, so the two spellings
// are two named decisions and not a convention silently copied.
//
// THE QUARTER TURN IS THE SCENE'S, APPLIED THE SAME WAY. The panel is portrait
// (240x400) and the canvas is landscape, exactly like the scene, so the same
// proper rotation `eyeViewProjection` applies at its step (B) goes here: new
// column 0 = old column 1, new column 1 = -old column 0. That is the whole
// argument for the orientation: whatever the panel does with the scene's clip
// +x, it does with the canvas's, so the HUD lands where the scene's top and
// right are, and the two cannot drift apart.
//
// NO DEPTH CONVERSION, BUT NOT AT z = 0 EITHER. The scene's step (A) maps the
// engine's [0,1] NDC z into the PICA's [-1,0]; the canvas has no depth to convert
// (its pass neither tests nor writes depth, `depthStateForPass`). It is still not
// left at 0: this GPU's clip z runs [-w, 0], so a canvas flat at z = 0 sits exactly
// on the far clip plane, which is the one place a rasteriser may reject the whole
// triangle. The matrix translates z to -0.5 -- the middle of the range, free for a
// pass that writes no depth -- and `pica_matrix_tests.cpp`'s canvas row pins the
// invariant (strictly inside) rather than the number. It is NOT the reason the
// canvas is invisible; that is measured, and `canvasMatrix`'s own body says so.
//
// `canvasWidth` is `mdk_render_canvas_width`'s value (360 high, as wide as the
// panel's aspect makes it: 600 on the 3DS). A zero or negative width is refused
// by the caller, not clamped here: a canvas with no width has no draws.
void canvasMatrix(float out[16], float canvasWidth);

// ---------------------------------------------------------------------------
// THE FRAME'S DEPTH STATE
// ---------------------------------------------------------------------------

// What one draw does with the depth buffer.
struct DepthState {
    // Is the fragment tested against the buffer at all? False means the draw
    // lands over the scene.
    bool test = true;
    // Does a passing fragment update the buffer? False is what lets a blended
    // surface be seen through.
    bool write = true;
    // Does the test pass only at the SAME depth (GPU_EQUAL, not GPU_LEQUAL)?
    // Only the sun shadow's receive pass asks: it draws a surface a second time
    // over itself and must touch no pixel another surface won (kDepthShadowReceive).
    bool equal = false;
};

// THE SUN SHADOW'S RECEIVE PASS: tested for equality, never written.
constexpr DepthState kDepthShadowReceive = {true, false, true};

// The oracle's rule, in one place so a host test can hold it to the C#.
//
// `Renderer.cs:605-607` builds every pipeline's depth state from the pass:
//
//     compare_op         = LESS_OR_EQUAL
//     enable_depth_test  = mesh && pass is not Overlay and not OnTop
//     enable_depth_write = mesh && !blended && pass != OnTop
//
// with `blended` meaning `pass is Blended or Overlay` (Renderer.cs:565). This
// seam only ever receives meshes, so the `mesh` conjunct is always true here.
// The table is spelled out per pass rather than computed from the two
// expressions, because the point of spelling it out is that each of the six
// lines can be read against the oracle line it comes from.
DepthState depthStateForPass(Pass pass);


// ---------------------------------------------------------------------------
// THE FRAME'S BLEND STATE
// ---------------------------------------------------------------------------

// The blend factors the oracle's rule can name, in host-readable form. The
// backend maps these onto the GPU enums; this file must not name them.
enum BlendFactor {
    kBlendZero = 0,
    kBlendOne = 1,
    kBlendSrcAlpha = 2,
    kBlendOneMinusSrcAlpha = 3,
    kBlendOneMinusDstColour = 4,   // the glow's screen blend only (kBlendScreen)
    kBlendConstantAlpha = 5,       // the glow's level mix only (kBlendGlowMix)
    kBlendOneMinusConstantAlpha = 6,
    kBlendDstColour = 7,           // the sun shadow's multiply only (kBlendShadowMultiply)
};

// What one draw does with what is already in the target.
struct BlendState {
    // Does the draw mix with the target at all? False is the oracle's
    // `enable_blend = false`, which on the PICA is not a switch but the
    // replace pair (ONE, ZERO). The backend READS this rather than trusting the
    // four factors beside it: a state with `enable == false` is installed as
    // `kBlendReplace` whatever factors it carries, so the type cannot say "off"
    // and have the hardware mix anyway (review-4 F6).
    bool enable = true;
    BlendFactor srcColour = kBlendSrcAlpha;
    BlendFactor dstColour = kBlendOneMinusSrcAlpha;
    // The oracle sets the ALPHA channel's factors separately from the
    // colour's, and they are not the same pair.
    BlendFactor srcAlpha = kBlendOne;
    BlendFactor dstAlpha = kBlendOneMinusSrcAlpha;
};

// The oracle's `enable_blend = false`, spelled the only way the PICA can spell
// it: replace on both channels. `blendStateForPass` returns this for the four
// opaque passes, and it is what the backend installs for any state whose
// `enable` is false. It is a named constant rather than four literals inside
// `blendStateForPass` so the host suite and the renderer agree on one pair.
const BlendState kBlendReplace = {false, kBlendOne, kBlendZero, kBlendOne,
                                  kBlendZero};

// THE GLOW'S BLEND (Enhanced Mode glow, 06_draw.inc drawGlow): a SCREEN,
// s * (1 - d) + d = 1 - (1 - s)(1 - d), which is the PC's post.frag
// `1.0 - (1.0 - colour) * (1.0 - shine)` exactly -- here in the panel's sRGB
// values, where the PC's is in linear light. Alpha is left as it was.
const BlendState kBlendScreen = {true, kBlendOneMinusDstColour, kBlendOne, kBlendZero,
                                 kBlendOne};

// THE GLOW'S LEVEL MIX: level 3 := a * level5 + (1 - a) * level3 with the blend
// colour's alpha a = 1/2 -- the PC's (scene@mip3 + scene@mip5) / 2, folded into
// level 3 so one texture unit reads both (06_draw.inc drawGlow).
const BlendState kBlendGlowMix = {true, kBlendConstantAlpha, kBlendOneMinusConstantAlpha,
                                  kBlendZero, kBlendOne};

// THE SUN SHADOW'S MULTIPLY: colour := colour * source (the receive pass's
// source is 1 in the light and R = F_shadow / F_lit in the shadow, see
// mdk_shadow.g.pica). Alpha is left as it was.
const BlendState kBlendShadowMultiply = {true, kBlendDstColour, kBlendZero, kBlendZero,
                                         kBlendOne};

// ---------------------------------------------------------------------------
// THE GLOW (Enhanced Mode enableGlowBillboards; the PC's post.frag shine).
//
// The PC:   blurred = to_linear((scene@mip3 + scene@mip5) / 2)
//           shine   = clamp(max(blurred - 0.8, 0) * 0.8)          (glow.x = 0)
//           colour  = 1 - (1 - colour)(1 - shine)                   (linear)
// The PICA works in the panel's sRGB values and its combiner has no power
// function, so two things are carried over:
//
// THE THRESHOLD. 0.8 linear is 0.8^(1/2.2) = 0.9036 sRGB, and over the
// [0.9036, 1] band the PC's shine is, to first order, 0.15 * u with
// u = (b_s - 0.9036) / 0.0964 in [0, 1] (0.15 = 0.8 * (1 - 0.8)).
//
// THE BLEND. A screen in sRGB values (kBlendScreen) cannot equal a screen in
// linear light for every background: the PC's lift over black is
// shine^(1/2.2), over mid-grey about a quarter of that. So the shine the PICA
// screens is the curve s*(u) that minimises the squared error against the PC's
// result over every background colour (fitted 2026-11-18): the PC's own shine
// (s = 0.15u) misses it by up to 69/255, s* by up to 30/255. The combiner draws
// s* as P * min(1, u / u0) + Q * u (within 2.8/255 of s*): six stages, the
// buffer carrying Q * u past the clamp (06_draw.inc drawGlow).
// ---------------------------------------------------------------------------
const float kGlowThresholdSrgb = 0.9036f;
const float kGlowBand = 0.0964f;          // 1 - kGlowThresholdSrgb: u's unit
const float kGlowCurveP = 0.0693f;
const float kGlowCurveQ = 0.2450f;
const float kGlowCurveU0 = 0.2551f;       // = 1 / (16 Q): two x4 stages reach it

// The glow texture: the eye's 240x400 picture, copied tile row by tile row
// into the bottom-left of a 256x512 RGBA8 texture whose mip levels 3 and 5 are
// the PC's two blur levels (30x50 and 7.5x12.5 of picture).
const int32_t kGlowTexWidth = 256;
const int32_t kGlowTexHeight = 512;
const int32_t kGlowMidLevel = 3;    // 32x64: the PC's mip 3
const int32_t kGlowLowLevel = 5;    // 8x16: the PC's mip 5
// Every blur level is its own single-level texture: level k (1..5) at index
// k - 1, (256 >> k) x (512 >> k), made by a render-to-texture draw from the
// level above that samples bilinearly at texel corners (an exact 2x2 average).
// Not the transfer engine's scaling (citro3d itself never uses a tiled-to-tiled
// scaled transfer, so nothing shows the console does one at these widths -- review,
// 2026-11-18), and not the picture texture's own mip levels (rendering into
// those never reached the sampler in the emulator). Index kGlowSmallMix is
// level 3 with level 5 mixed in -- the PC's (mip3 + mip5) / 2 -- and the one
// the final quad reads.
const int32_t kGlowSmallCount = 6;
const int32_t kGlowSmallMix = 5;    // 32x64

// A draw into a render target made from a texture covers texel (s, t) at
// clip (2s - 1, 2t - 1): the framebuffer's first memory row is the clip's top
// (y = +1) and a texture's first memory row is t = 1 (both measured, see
// glowTexCoord), and its columns run with clip x as the panel's do. So the
// level-pass quad is the identity: corner (s, t) at clip (2s - 1, 2t - 1),
// sampling (s, t).
void glowLevelCorner(float s, float t, float* clipX, float* clipY);

// The glow quad's texture coordinate for a canvas corner at (fx, fy), each in
// [0, 1] across the panel (y down). The panel is the portrait framebuffer
// rotated: a framebuffer row is a panel column, so s runs along the panel's
// height (bottom to top) and t along its width (t = 1 at the first row in
// memory, the panel's left edge), each scaled by the picture's share of the
// texture (240 / 256, 400 / 512).
void glowTexCoord(float fx, float fy, float* s, float* t);

// The oracle's rule, in one place so a host test can hold it to the C#.
//
// `Renderer.cs:565-577`, the same block every pipeline state is built from:
//
//     blended        = pass is Blended or Overlay
//     enable_blend   = blended
//     src_color      = SRC_ALPHA
//     dst_color      = blend is Add ? ONE : ONE_MINUS_SRC_ALPHA
//     color_op       = ADD
//     src_alpha      = ONE
//     dst_alpha      = ONE_MINUS_SRC_ALPHA
//     alpha_op       = ADD
//
// Two things in that block are easy to get wrong and are the reason this is a
// table and not a comment. The first is that `enable_blend` is FALSE for the
// four opaque passes: a Solid surface does not mix, so a texture whose alpha
// is neither 0 nor 255 is opaque here and translucent on a build that leaves
// blending on. The second is that the alpha channel's source factor is ONE
// while the colour's is SRC_ALPHA; copying the colour pair to the alpha
// channel is a plausible-looking mistake a reader cannot see.
//
// `blend` is only read when the pass is blended, because the oracle only
// reads it there: Blend::Add changes the destination colour factor and nothing
// else, and an unblended pass never reaches that line.
BlendState blendStateForPass(Pass pass, Blend blend);


// ---------------------------------------------------------------------------
// THE ORACLE'S DISCARD
// ---------------------------------------------------------------------------

// The reference value of the alpha test that stands in for the oracle's
// discard. The backend installs `GPU_GREATER` against this, i.e. it keeps a
// fragment whose alpha is strictly above it.
const int kAlphaTestReference = 0;

// The reference while Enhanced Mode FILTERING is on (SPEC.md 7.9): the PC's
// enhanced.frag cuts a filtered texel at HALF_COVER (`if (texel.a < 0.5)
// discard;`), i.e. it keeps alpha >= 0.5, which is a byte >= 128: GPU_GREATER
// against 127. Under bilinear filtering a cut-out edge's alpha runs between 0
// and 255 across a texel, and this keeps the silhouette where the PC's is
// instead of growing it by the half texel GREATER-0 would admit.
const int kAlphaTestHalfCover = 127;

// The reference the backend installs for one draw: the oracle's discard, or the
// PC's half cover while FILTERING is on -- EXCEPT for the canvas. enhanced.frag
// returns for its canvas modes (3, 4) BEFORE the HALF_COVER discard: the canvas
// blends by cover and is never cut. The hurt flash (alpha 0.4), the white flash,
// the inventory box (0.5) and the skull's fade are all canvas quads whose
// fragments sit below half cover; cutting them deletes them (review of the
// filtering batch, F1).
inline int alphaTestReferenceFor(bool filtering, Pass pass) {
    return (filtering && pass != Pass::Overlay) ? kAlphaTestHalfCover : kAlphaTestReference;
}

// The oracle's `if (index == 0) discard;` (shaders/palette.hlsl), as a
// predicate on the fragment's 8-bit alpha so a host test can hold it.
//
// WHY AN ALPHA TEST IS THE RIGHT EQUIVALENT, AND WHY IT IS NOT COSMETIC. The
// shader discards index 0, and this backend's palette expansion encodes index
// 0 as alpha 0 and every other index as alpha 255, so `alpha > 0` selects
// exactly the texels the shader keeps. That is the same predicate, but the
// point of the test is not the colour: a DISCARDED FRAGMENT DOES NOT WRITE
// DEPTH, while a fragment blended with alpha 0 does. Left as a blend, every
// transparent texel of an arena texture paints a hole in the depth buffer that
// hides whatever is behind it -- an invisible wall, drawn in the game's own
// colours, so the picture looks plausible right up to the place where the
// world stops. That is the failure this replaces.
bool alphaTestPasses(int alphaByte);

// The depth map the backend installs, as the three arguments of
// `C3D_DepthMap(kDepthMapIsZBuffer, kDepthMapScale, kDepthMapOffset)`, and the
// raw depth clear value for `C3D_RenderTargetClear`.
//
// WHY THESE NUMBERS. `eyeViewProjection` converts the engine's NDC z from [0,1]
// to [-1,0] (near = -1, far = 0). The PICA then stores `z*zScale + zOffset`, and
// the citro3d actually linked here applies NO hidden bias to those two
// arguments: `C3D_DepthMap` is `strb r0,[r4,#233]; bl f32tof24; str r0,[r4,#224];
// vmov.f32 s0,s16; bl f32tof24; str r0,[r4,#228]` and nothing else (measured on
// the linked archive: `arm-none-eabi-objdump -d $DEVKITPRO/libctru/lib/
// libcitro3dd.a`, section `.text.C3D_DepthMap`). So the stored depth here is
// `zNdc + 1`: near = 0, far = 1 — the SAME direction as the oracle's [0,1] with
// LESS_OR_EQUAL, which is why the test function is GPU_LEQUAL and not the
// GPU_GREATER that citro3d's own default state pairs with its own default map.
//
// The pairing is not invented here. Forsaken — the sibling port, same house,
// running on this hardware — installs `C3D_DepthMap(true, 1.0f, 1.0f)` with
// `C3D_DepthTest(true, GPU_LESS, GPU_WRITE_ALL)` and clears the depth to
// 0xFFFFFF (vault: 10-projects/forsaken/findings/
// citro3d-renderer-architecture-and-vertex-pipeline.md,
// citro3d-missile-chase-cam-per-eye-scissored-clear.md). citro3d's own default
// is the REVERSED range — `C3D_DepthMap(true, -1.0f, 0.0f)` with GPU_GREATER and
// a clear of 0 (vault: 60-references/3ds-platform/citro3d/
// 01-overview-and-init.md) — and that is the pairing the "the 3DS's depth
// convention is the opposite of the desktop's" note in this backend's history
// was describing. Both conventions are in the wild; these numbers pick the one
// that matches the oracle.
//
// AND THE CHOICE DOES NOT DEPEND ON THE ONE THING THAT CANNOT BE CHECKED HERE.
// Older citro3d releases folded a bias into these arguments (`z*0.5 + 0.5` for
// the z-buffer form). Whether the linked library does — measured above: it does
// not — or a future one does, `(scale 1, offset 1)` over the converted range
// maps [-1,0] monotonically INCREASING with distance into a subset of [0,1], and
// 0xFFFFFF is the buffer's maximum under either byte order of a
// DEPTH24_STENCIL8 word. Those are exactly the three properties the depth test
// needs: nearer fragments store less, so GPU_LEQUAL keeps them; nothing in the
// frustum falls outside the buffer; and the cleared buffer accepts everything.
// A SIGN error is not covered by that argument — and a sign error is what
// `storedDepth` against the real eye matrix pins in the test suite.
constexpr bool kDepthMapIsZBuffer = true;
constexpr float kDepthMapScale = 1.0f;
constexpr float kDepthMapOffset = 1.0f;
constexpr uint32_t kDepthClearValue = 0x00FFFFFFu;

// Convert the seam's clear colour to the word `C3D_RenderTargetClear` wants.
//
// THE TWO ENDS OF THIS SEAM PACK A COLOUR DIFFERENTLY, AND NOTHING IN THE
// SIGNATURE SAYS SO. `MdkFrameParams::clear_colour` is 0xAARRGGBB — that is the
// seam's documented convention, and the Vulkan backend unpacks it that way
// (`mdk_linux/src/render/vk/vk_context.cpp`: R is `(clearColour >> 16) & 0xFF`).
// The clear word `C3D_RenderTargetClear` consumes is **0xRRGGBBAA — red in the
// TOP byte** — which is measured, not read off a header. Two runs of one build on
// the pinned instrument (Mandarine r1.2.1, `DISPLAY=:99`) bracket the change:
//
//     word passed           panel pixel
//     0xFF102030 (raw)   ->  #ff1020     the alpha byte became red
//     0x102030FF (here)  ->  #102030     exactly what the frontend asked for
//
// So passing the seam value straight through paints the frame with the ALPHA
// byte as red. That is the whole of "the 3DS picture is a red clear": the clear
// was never the wrong *colour*, it was the wrong *byte order*, and everything
// drawn on top of it was being read against a background nobody intended.
//
// ASSERT THIS AS BIT FIELDS, NOT AS MEMORY BYTES. On a little-endian machine the
// byte at offset 0 is the LOW byte, which here is the alpha, so a byte-indexed
// expectation encodes the opposite of what the hardware sees even when the
// conversion is correct — the first draft of the host row did exactly that and
// went red over a fix that the emulator had already confirmed good.
//
// This lives here rather than inline in the backend for the reason the whole file
// exists: it is a conversion, it is host-testable, and a host test is the only
// place a byte order can be pinned without a console.
constexpr uint32_t clearColour(uint32_t argb) {
    return ((argb & 0x00FFFFFFu) << 8) | ((argb >> 24) & 0xFFu);
}

// Write one RGBA8 texel into the four bytes the PICA reads.
//
// THE SAME TRAP, ONE UNIT EARLIER IN THE PIPE. `clearColour` above fixed the
// colour that FILLS the frame; this fixes the colour that gets DRAWN on it, and
// the two ends had been disagreeing with the hardware in opposite directions.
//
// THE CONVENTION, AND WHERE IT COMES FROM. The PICA's RGBA8 is a 32-bit word
// read little-endian with **red in the TOP byte**, so the four bytes at a
// texel's address are `[A, B, G, R]` and the seam's RGBA-in-memory order (which
// the Vulkan backend consumes directly as `VK_FORMAT_R8G8B8A8_UNORM`, and which
// `render.h` documents) has to be rotated before it is handed over. That is not
// a guess and not a local invention; it is recorded in two places outside this
// repository, both read before this was written (review-6 F2 corrected an
// earlier claim here that no prior port had written the byte order down — it
// had, twice):
//
//   * Obsidian `GX-to-PICA200/22-textures.md:112`, in the encode block:
//     `GPU_RGBA8: bytes = {A, B, G, R}  // PICA RGBA8 is little-endian` — the
//     same note family AGENTS.md §12 names as required reading for the PICA200
//     pixel pipeline.
//   * `../perfect_dark/port/fast3d/gfx_citro3d.cpp:1708`, `rgba_to_abgr`, whose
//     own comment at :1735 says the GPU is handed "PICA-layout abgr words
//     (R[31:24] G[23:16] B[15:8] A[7:0])". That port hit the SAME defect on its
//     RGB565 pool path and recorded the same signature — "sampled red = 255,
//     green = source blue, blue = source green" (its §44.801az). Its distinction
//     that a BAKE path may hold true RGBA while a POOL path holds already-packed
//     words is the same seam question MDK answers by packing at exactly one
//     place: here.
//
// The `clearColour` agreement is a SECOND witness, not the reason: it is the
// same GPU's colour convention arriving at the other end of the pipe, and a
// framebuffer format on its own would be the weakest available evidence for a
// texture format.
//
// WHAT IT LOOKED LIKE WHEN IT WAS WRONG, which is the reason this is a rule and
// not a footnote: the expansions wrote `[R, G, B, 255]`, so the hardware read
// red from the byte the port had forced to 255. Every painted pixel on the
// console came back with R = 255 — measured on the pinned instrument, 96000 of
// 96000 pixels of `docs/evidence/2026-11-09-L7-shader-w-current.png`, which is
// why the 3DS picture has read as a red wash for this backend's whole history.
// Green and blue were the palette's blue and green, swapped, and the texel's
// ALPHA was the palette's red byte — so the index-0 discard still appeared to
// work, because a discarded texel is all four bytes zero and its alpha is zero
// under any byte order. The one defect the symptom hid.
//
// ASSERT IT AS BYTES, NOT AS A WORD. `clearColour` returns a word and is checked
// as bit fields; this one writes memory, and the whole point is which byte lands
// at which address. The two are checked in opposite ways on purpose — see the
// comment on the clear row, which is about the same trap from the other side.
// And the helper being right is not the shipped property: the two CALL SITES are
// pinned by `tests/scripts/c3d_frame_test.sh`, because this file cannot see them
// and a host row never sees a caller.
inline void texelBytes(uint8_t* dst, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    dst[0] = (uint8_t)a;
    dst[1] = (uint8_t)b;
    dst[2] = (uint8_t)g;
    dst[3] = (uint8_t)r;
}

// THE TEXEL'S SLOT IN THE PICA'S TILE ORDER, and the reason image data cannot be
// uploaded row by row.
//
// A PICA200 texture is NOT stored as rows of texels. Texels live in 8x8 tiles,
// the tiles themselves are stored row-major, and INSIDE a tile the texels
// follow the Z-order (Morton) curve: an 8x8 tile is four 4x4 quadrants ordered
// diagonally, each quadrant is four 2x2 cells, and each 2x2 cell is four texels
// ordered the same way. So a texel's slot is `(tileY*tilesPerRow + tileX)*64 +
// morton8(x&7, y&7)`, and the byte address is that slot times the format's
// bytes per texel.
//
// WHERE THE RULE COMES FROM, read rather than remembered: Obsidian
// `60-references/3ds-platform/pica200/05-textures-and-combiners.md:16`, "Texels
// stored in 8x8 tiles, within-tile order follows the Z-order (Morton) curve;
// tiles row-major", with the same nested-quadrant decomposition this function
// implements (3dbrew's internal-registers page is that note's source). The
// citro3d side agrees by construction: `C3D_TexLoadImage` in the linked
// `libcitro3dd.a` is a plain `memcpy` into the texture's buffer for a linear
// texture and never permutes anything (`arm-none-eabi-objdump -d texture.o`;
// the only non-`memcpy` path is the VRAM staging copy), and its own reference
// note says "`C3D_TexUpload` / `C3D_TexLoadImage` require data laid out in the
// PICA **tile format** ... raw RGBA uploads must be tiled manually"
// (`60-references/3ds-platform/citro3d/06-textures-texenv.md`). There is no
// stride field in `C3D_Tex` to get wrong: the GPU computes the tiled address
// from `dim` and `fmt`, which is exactly why a row-major buffer is accepted
// without complaint and sampled wrong.
//
// THE DEFECT THIS REPLACES, MEASURED AND REPRODUCED. The expansions wrote rows
// and the GPU read tiles, and the signature is the one recorded in
// `docs/OPEN_ITEMS.md` (batch 173): a period-32 vertical square wave came back
// as period-8 (four times too many bands), a left-white/right-black image came
// back as full-width horizontal bands (960 vertical colour transitions, zero
// horizontal -- no UV scale can turn a vertical split into horizontal bands),
// and a rect confined to one half came back as stripes inside one band. Those
// three are exactly what a Morton read of a linear buffer produces; the
// per-texel permutation below was checked against all three before this landed,
// and `tests/scripts/pica_matrix_tests.cpp` pins the period-32 case as the
// fixture. It also explains why most of the arena went MISSING rather than
// merely scrambled: a padded, power-of-two texture's unused region is
// transparent (alpha 0), the frame-wide alpha test discards alpha 0
// (`C3D_AlphaTest(true, GPU_GREATER, 0)` in `03_frame.inc`), and a scrambled
// address lands in that padding.
//
// COLD-PATH COST, STATED SO IT IS NOT A SURPRISE. Texture expansion runs at
// level load and again whenever an evicted texture is made again, so this is
// per-texel work in a path that can run inside a frame. It is integer only, has
// no division and no table, and does not change the texel count; R11 and the
// float rules do not apply to it.
inline uint32_t morton8InTile(uint32_t x, uint32_t y) {
    uint32_t m = 0;
    for (int b = 0; b < 3; b++) {
        m |= ((x >> b) & 1u) << (2 * b);
        m |= ((y >> b) & 1u) << (2 * b + 1);
    }
    return m;
}

// THE TEXTURE'S t AXIS RUNS FROM THE LAST MEMORY ROW. The PICA samples t = 0 at
// the LAST row of a texture's memory and t = 1 at the first, while the
// expansion writes image row 0 into the first rows (textureTexelIndex, y = 0
// first). A scene draw's oracle v (0 = image top) must therefore reach the GPU
// as t = 1 - (v * scaleY + offsetY), i.e. the uniform carries (-scaleY,
// 1 - offsetY). Measured 2026-11-17 in Mandarine: without it every scene
// texture was sampled upside down, and any texture whose height is not a power
// of two read its transparent padding instead of the image (a 306-row arena
// texture in a 512-row buffer sampled rows 511..206), so the arena on every
// level, and Kurt, went to alpha 0 / black. A power-of-two texture was merely
// upside down. THE CANVAS TAKES IT TOO. It was once exempt, on the argument that
// its projection's y inversion cancels this one; it does not (the projection
// places the quad, the uv picks the texel). Measured 2026-11-17, frame-exact
// against Linux on level 6: exempt, the health digits (not a power of two) read
// padding rows and the HUD region differed on 26% of pixels; flipped, 0.10%.
struct SceneUv {
    float scaleY;
    float offsetY;
};
inline SceneUv sceneUvFromTop(float scaleY, float offsetY) {
    SceneUv out;
    out.scaleY = -scaleY;
    out.offsetY = 1.0f - offsetY;
    return out;
}

inline size_t textureTexelIndex(uint32_t x, uint32_t y, uint32_t paddedWidth) {
    const size_t tileX = (size_t)(x >> 3);
    const size_t tileY = (size_t)(y >> 3);
    const size_t tilesPerRow = (size_t)(paddedWidth >> 3);
    return (tileY * tilesPerRow + tileX) * 64u + (size_t)morton8InTile(x & 7u, y & 7u);
}

// The depth the PICA stores for a converted NDC z — the model of what the two
// constants above do, kept next to them so the two cannot drift apart. The GPU
// does this arithmetic; the backend never calls this per fragment. It IS called
// once at init, to log the state the picture cannot show (AGENTS.md P6).
float storedDepth(float zNdc);

// True when a fragment whose converted NDC z is `zNdc` passes a buffer holding
// `stored`, under the oracle's LESS_OR_EQUAL and the pass's own `test` flag.
//
// The equality is load-bearing, not a rounding hedge: a fragment exactly on the
// far plane stores exactly the value the buffer was cleared to, so a strict
// LESS would reject the far plane and anything that touches it.
bool depthPasses(const DepthState& state, float zNdc, float stored);

// How many passes this frame draws, and the eye offset for each of them.
struct EyePlan {
    int passes;
    float offset[2];
};

// The frame's eye plan, from the physical 3D slider (0..1) and the hardware's
// maximum separation.
//
// THE CLAMP IS LOAD-BEARING. Mandarine's `osGet3DSliderState()` returns a
// garbage value (~1.34e8) when it does not model the slider, and a sibling port
// shipped exactly that as an eye separation of billions of units: the screen
// went black (../forsaken/3DS_PORT_NOTES.md "Bug 3"). Anything outside [0,1] is
// clamped, including a negative, so no caller can multiply garbage into a matrix.
//
// THE PASS COUNT IS THE OTHER HALF. With the slider closed the panel displays the
// LEFT buffer only, so a second pass into the right buffer is invisible work on a
// 268 MHz ARM11: one pass, offset zero. With it open, both eyes are drawn and the
// left takes the NEGATIVE offset, which is the convention this backend has always
// used and what `testTheEyeOffset` already pins for the matrix.
//
// It lives here rather than in the backend for the usual reason: the sign, the
// clamp and the skip are the three things that can silently black the screen, and
// renderer_c3d.cpp cannot be compiled on this host.
EyePlan eyePlan(float slider, float separation);

// ---------------------------------------------------------------------------
// THE HAZE (Enhanced Mode, SPEC.md 7.9 `enableHaze`): the PC's
//
//     fog = 1 - exp(-density * distance)     (enhanced.frag:187)
//
// computed by the VERTEX SHADER, not the PICA's fog unit. The fog unit indexes
// a 128-entry table by the STORED depth, and with this camera (near 0.5, far
// 20000, a z-buffer) every point past ~64 units lands between the table's first
// two entries: at 1000 units it would keep 0.08 of the colour where the PC keeps
// exp(-0.2) = 0.82. A w-buffer would fix the index but replaces the pinned depth
// map of the whole original look (kDepthMapIsZBuffer) for a cosmetic toggle.
//
// So the shader (mdk_c3d.v.pica) takes `kept = ex2(haze.x * clip.w)` per vertex
// -- clip.w is the eye depth -- and multiplies it into the primary colour's
// alpha; combiner stage 2 then blends toward the haze colour by it. All of it
// runs on the PICA; the CPU writes one uniform when it changes. Per vertex
// against the PC's per pixel, and eye depth against its Euclidean distance,
// are the two approximations.
// ---------------------------------------------------------------------------

// The shader's haze.x for a density: -density * log2(e), so ex2(x * depth) is
// exp(-density * depth). Zero (no haze) for a density that is not positive.
float hazeExponent(float density);

// Whether one draw takes the haze: the toggle is on, the rig is live with a
// density, and the draw is a surface the PC hazes (Lit or Sprite shading --
// the sky and the canvas are Original, and the PC's sky.frag and canvas
// modes take no haze). Overlay never does.
bool hazeApplies(bool toggleOn, const Lighting& lighting, Shading shading, Pass pass);

// The combiner constant's word for an opaque colour: 0xAABBGGRR (R in the low
// byte), the layout C3D_TexEnvColor takes.
uint32_t hazeColourWord(const Colour& colour);

// ---------------------------------------------------------------------------
// THE FILTERED EDGE (Enhanced Mode, SPEC.md 7.9 `enableFiltering`)
// ---------------------------------------------------------------------------

// Prepares an expanded, TILED texture (`textureTexelIndex` layout, texels as
// `texelBytes` [A, B, G, R]) for bilinear sampling. Cold: once per texture
// creation, never per frame.
//
//  1. THE COLOUR UNDER A TRANSPARENT TEXEL. Index 0 expands to all-zero, and a
//     bilinear tap that mixes it with an opaque neighbour pulls the colour
//     toward BLACK -- a dark fringe on every cut-out edge. The PC avoids it by
//     filtering premultiplied colour and dividing by alpha (enhanced.frag). The
//     PICA's combiner cannot divide, so the colour is put where the filter will
//     find it: every alpha-0 texel takes the mean colour of its opaque
//     8-neighbours. Its ALPHA STAYS 0, so a nearest sample of it is still
//     discarded and the original look does not change.
//  2. THE EDGE OF A PADDED AXIS. A texture whose real size is not a power of two
//     sits in a padded buffer, and a bilinear tap at its last real row/column
//     reaches into the padding: alpha 0, so an opaque wall's edge would fade and
//     the half-cover test would open a crack along it. The first padding row and
//     column take a copy of the last real ones (alpha included) -- the PC's
//     clamp, one texel wide, which is all a bilinear tap reaches.
void prepareForFiltering(uint8_t* tiled, int32_t width, int32_t height, int32_t paddedWidth,
                         int32_t paddedHeight);

// ---------------------------------------------------------------------------
// THE MIP CHAIN (Enhanced Mode, SPEC.md 7.9 `enableMipmaps`)
// ---------------------------------------------------------------------------
//
// The PICA samples a mip chain stored after level 0 (citro3d
// C3D_TexInitWithParams' maxLevel), each level tiled at its own size, and every
// level must be at least 8x8. The chain is BUILT ON THE CPU AT TEXTURE CREATION
// (cold, like the expansion it follows) and SAMPLED BY THE GPU: citro3d's
// C3D_TexGenerateMipmap runs a blocking display transfer per level, and a texture
// can be made again mid-frame after an eviction, where that is not safe. The HD
// texture packs (tex3ds .t3x) will carry their chains prebuilt, into the same
// slots.

// How many levels below level 0 a padded texture can carry (each >= 8x8, and the
// PICA's 4-bit field caps the count at 15; a 1024 axis gives at most 7).
int32_t mipLevelsFor(int32_t paddedWidth, int32_t paddedHeight);

// One level from the one above it: `src` is tiled at width x height (powers of
// two, >= 16), `dst` is tiled at width/2 x height/2. Each texel is the rounded
// mean of its 2x2 parents, channel by channel, alpha included -- the colour a
// transparent parent carries is prepareForFiltering's bled one, so the mean is
// not pulled toward black.
void downsampleTiled(const uint8_t* src, int32_t width, int32_t height, uint8_t* dst);

// THE TEXEL FORMATS this backend stores, and what each costs. RGBA8 is the
// original look's lossless expansion; ETC1 (opaque) and ETC1A4 (4-bit alpha) are
// the HD layer's, compressed on the HOST by tex3ds with their chains prebuilt
// (docs/HD_TEXTURES_3DS_SPEC.md) -- the 3DS never encodes them. Every byte count
// the backend budgets with (eviction, sprite residency) goes through
// textureBytes, so an imported ETC texture is costed by the same rule.
//
// Rgb565, Rgba5551 and Rgba4 are the 16-bit stores of an EXPANDED texture
// (EnhancedToggles::texture16, a 3DS store choice): half the bytes the PICA
// fetches per texel, chosen per texture by expandedTexelFormat below.
enum class TexelFormat { Rgba8, Etc1, Etc1A4, Rgb565, Rgba5551, Rgba4 };

// Bytes of a padded width x height texture of `format` with `levels` mip levels
// below level 0 (each level is a quarter of the one above; ETC formats are 4 bits
// and 8 bits per texel, in 4x4 blocks, so every level >= 8x8 is whole blocks; the
// 16-bit formats are 2 bytes a texel). The byte OFFSET of level k in a chain
// buffer is textureBytes(format, w, h, k - 1): the levels are consecutive.
size_t textureBytes(TexelFormat format, int32_t paddedWidth, int32_t paddedHeight,
                    int32_t levels);

// THE RECTANGLE A COMPRESSED KURT FRAME OCCUPIES in its .t3x
// (EnhancedToggles::kurtCompressed; mdk_3ds/tools/kurt_etc1_convert.py). The
// converter pads the ORIGINAL frame, unscaled, to a power of two with the image
// at the TOP-LEFT, so the frame is realWidth x realHeight of the
// textureWidth x textureHeight texture. (A frame past 256x256 -- the chute's --
// is first FITTED to 256x256; then realWidth x realHeight is that image's size,
// smaller than the frame, and the caller's quad, still the frame's size, maps
// its uv 0..1 onto it: the frame is drawn as large as before.) In tex3ds' own
// subtexture convention --
// the one the HD copies are drawn through (06_draw.inc: uv' = uv * (right - left,
// bottom - top) + (left, top)) -- that is left 0, top 1, right w/W, bottom 1 - h/H:
// exactly what tex3ds itself writes for an image it pads (measured on a 52x146
// Kurt frame: right 832/1024 = 52/64, bottom 440/1024 = 1 - 146/256).
struct CompressedRect {
    float left = 0.0f;
    float top = 1.0f;
    float right = 1.0f;
    float bottom = 0.0f;
};
CompressedRect compressedFrameRect(int32_t realWidth, int32_t realHeight, int32_t textureWidth,
                                   int32_t textureHeight);

// What one Kurt frame costs resident, by store (the sprite residency's bytes and
// the figures the A/B is reported in): the frame's padded size in RGBA8
// (texture16=0), RGBA5551 (texture16=1: an index texture's alpha is 0/255), or
// ETC1A4 (kurtCompressed=1: one byte a texel, `levels` mip levels below level 0
// when the set was converted with --mips; 0 by default).
size_t spriteFrameBytes(TexelFormat format, int32_t realWidth, int32_t realHeight, int32_t levels);

// ---------------------------------------------------------------------------
// THE 16-BIT STORE (EnhancedToggles::texture16)
// ---------------------------------------------------------------------------
//
// THE WORDS. Each 16-bit PICA texel is ONE LITTLE-ENDIAN u16, the high channel
// in the high bits (3dbrew GPU/Internal Registers, TEXUNITi_TYPE 2/3/4; the same
// packers in ../perfect_dark/port/fast3d/gfx_citro3d.cpp:1832-1855 run on this
// GPU; Obsidian GX-to-PICA200/22-textures.md:113 for RGB565):
//
//     RGB565    R[15:11] G[10:5]  B[4:0]            (sampled alpha is 1.0)
//     RGBA5551  R[15:11] G[10:6]  B[5:1]  A[0]
//     RGBA4     R[15:12] G[11:8]  B[7:4]  A[3:0]
//
// so memory holds the LOW byte first: `texel16Bytes`. This is the same
// convention as RGBA8's [A, B, G, R] (`texelBytes`) one size down -- the channel
// that is lowest in the word is first in memory -- and getting it backwards is
// the R = 255 class of defect again (alpha/blue read as red). The 8x8 Morton
// tile order is unchanged: a texel's SLOT is the same (`textureTexelIndex`),
// only its size is 2 bytes instead of 4.
//
// THE ROUNDING. A channel goes to n bits by round(v * (2^n - 1) / 255), the
// nearest representable level once the PICA widens it back by bit replication
// (5 bits: v << 3 | v >> 2, 6 bits: v << 2 | v >> 4, 4 bits: v * 17).
//
// THE ALPHA IS THE GAMEPLAY HALF, as on RGBA8: the alpha test keeps alpha > 0
// (the oracle's index-0 discard) or alpha > 127 (the filtered half cover,
// alphaTestReferenceFor). RGBA5551's one bit is set for alpha >= 128, which is
// the half cover exactly and, for an alpha that is 0 or 255 (every index
// texture's level 0), the > 0 test exactly too. RGBA4's four bits round to
// nearest, which keeps >= 128 <-> >= 8 (136) exactly, and a nonzero alpha
// never rounds to 0 (it takes 1, i.e. 17) so the > 0 test is exact as well.
inline uint32_t channelBits(uint32_t v, uint32_t maxLevel) {
    return (v * maxLevel + 127u) / 255u;
}
inline uint16_t rgb565Word(uint32_t r, uint32_t g, uint32_t b) {
    return (uint16_t)((channelBits(r, 31u) << 11) | (channelBits(g, 63u) << 5) |
                      channelBits(b, 31u));
}
inline uint16_t rgba5551Word(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return (uint16_t)((channelBits(r, 31u) << 11) | (channelBits(g, 31u) << 6) |
                      (channelBits(b, 31u) << 1) | (a >= 128u ? 1u : 0u));
}
inline uint16_t rgba4Word(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    uint32_t alpha = channelBits(a, 15u);
    if (a != 0u && alpha == 0u) {
        alpha = 1u;   // a texel the > 0 test keeps must not become one it discards
    }
    return (uint16_t)((channelBits(r, 15u) << 12) | (channelBits(g, 15u) << 8) |
                      (channelBits(b, 15u) << 4) | alpha);
}
inline void texel16Bytes(uint8_t* dst, uint16_t word) {
    dst[0] = (uint8_t)(word & 0xFFu);
    dst[1] = (uint8_t)(word >> 8);
}

// THE CHOICE, per texture, from its EXPANDED level 0 (`texelBytes` order, tiled,
// `level0Texels` = paddedWidth * paddedHeight, padding included):
//
//   * Rgba8 when the toggle is off, when the texture is pinned to RGBA8 (the
//     canvas: `keepRgba8`, see 06_draw.inc), or when the padded texture is at
//     most 8x8 (64 texels: a flat colour, the canvas white -- nothing to save
//     and a flat colour must stay the colour it was asked for);
//   * Rgb565 when every texel is opaque (alpha 255, padding included -- a
//     padded texture's padding is transparent, so only an unpadded hole-free
//     texture qualifies; the PICA samples RGB565 as alpha 1.0);
//   * Rgba5551 when every alpha is 0 or 255 -- every index texture: index 0 is
//     alpha 0 and the rest 255 (expandIndices). Level 0 is then stored EXACTLY in
//     alpha, and the mip levels' averaged alpha (64, 128, 191 at a cut-out's
//     edge) is cut at 128 -- the same answer the half-cover test gives at every
//     texel centre, which is the only test a mip level is ever sampled under
//     (06_draw.inc: the chain only with FILTERING, and FILTERING is the half
//     cover). What moves is the bilinear crossing BETWEEN two mip texels of a
//     cut-out's edge, by at most half a mip texel: sub-pixel at the distance a
//     level >= 1 is sampled. 5 bits of colour instead of RGBA4's 4 is the reason
//     to take that, because the near view (level 0) is where banding shows;
//   * Rgba4 otherwise -- a source with partial alpha (an RGBA texture), where one
//     bit of alpha would change level 0 itself (a canvas picture blends by its
//     alpha); four bits keep both alpha tests exact (rgba4Word).
TexelFormat expandedTexelFormat(bool texture16, bool keepRgba8, int32_t paddedWidth,
                                int32_t paddedHeight, const uint8_t* level0, size_t level0Texels);

// The widest store an INDEX texture of this padded size can take: Rgba5551 (2
// bytes, standing for Rgb565 too) under texture16 above the 8x8 floor, else
// Rgba8 -- an index texture's level 0 alpha is always 0 or 255, so it never
// takes Rgba4. The sprite residency's estimate (mdk_render_index_texture_bytes).
TexelFormat indexTexelFormatBound(bool texture16, int32_t paddedWidth, int32_t paddedHeight);

// Rewrites a chain buffer of `texels` texels from 4-byte `texelBytes` texels
// into 2-byte `format` texels IN PLACE (texel i's word at byte 2i; each read of
// texel i, at byte 4i, precedes the write, and no write reaches a texel not yet
// read). A no-op for Rgba8 (and for the ETC formats, which are never expanded).
void packTexels16(uint8_t* chain, size_t texels, TexelFormat format);

// The texels of level 0 plus `levels` levels below it, the size of one buffer
// holding the whole chain, levels consecutive (level k after level k-1).
size_t mipChainTexels(int32_t paddedWidth, int32_t paddedHeight, int32_t levels);

// Fills levels 1..levels of a chain buffer whose level 0 is already expanded
// and prepared: each level downsampled from the one above, then re-prepared
// (prepareForFiltering) at its own scaled real size -- (real + 1) / 2 per step --
// so the colour bleed and the one-texel clamp hold all the way down.
void buildMipChain(uint8_t* chain, int32_t paddedWidth, int32_t paddedHeight, int32_t realWidth,
                   int32_t realHeight, int32_t levels);

// ---------------------------------------------------------------------------
// THE HD RESIDENCY POLICY (docs/HD_TEXTURES_3DS_SPEC.md 4.2), pure so the host
// can run a camera's worth of frames through it: the backend's pump and its
// mid-frame allocation fallback call exactly these.
// ---------------------------------------------------------------------------

// One texture's HD copy as the policy sees it.
struct HdSlot {
    bool resident = false;
    uint32_t lastUsed = 0;   // the draw stamp of its last draw
    bool linked = true;      // some texture still links to it
};

// The copy to evict: among the resident ones NOT drawn at or after
// `protectFrom` (the first draw stamp of the frame whose commands may still be
// in flight, or of the one being recorded), an UNLINKED one first (nobody will
// draw it unless it is linked again), then the least recently drawn. With
// `unlinkedOnly`, only an unlinked one. -1 when there is none.
int32_t hdEvictionVictim(const HdSlot* slots, int32_t count, uint32_t protectFrom,
                         bool unlinkedOnly = false);

// May another SD read be handed out? Only when it could be imported: room for
// the largest pack texture for every read already in flight plus this one, or a
// copy that could be evicted. Otherwise a full budget would re-read files only
// to drop them.
bool hdMayDispatch(size_t residentBytes, size_t readsInFlight, size_t largestBytes,
                   size_t budgetBytes, bool evictable);

// ---------------------------------------------------------------------------
// THE SUN'S SHADOW (Enhanced Mode enableSkyShadows; the PC's sunlight()).
//
// The PC renders the frame's Lit triangles through `sunShadowMatrix` (render/
// sun_shadow.h: world -> x, y in [-1, 1] across the map, depth in [0, 1] along
// the light) into a 2048x2048 depth map and compares each lit pixel with it.
// The PICA does the same with its own shadow texture, in two passes
// (03_frame.inc castShadows, 06_draw.inc receiveShadows); these are the numbers
// and the two matrices both passes are made of, here so a host test can hold
// them to the PC's.
// ---------------------------------------------------------------------------

// The map's side in texels. The PC's is 2048; the PICA's largest texture is
// 1024 and its VRAM is 6 MB for both eyes' targets and the glow as well, so the
// map is 512: a megabyte (RGBA8), a texel 1.17 world units wide at the retail
// shadow distance of 300.
constexpr int32_t kSunShadowMapSize = 512;

// The VRAM a map of `mapSize` a side takes: its RGBA8 texels and the caster
// pass's 16-bit depth buffer.
constexpr size_t sunShadowVramBytes(int32_t mapSize) {
    return (size_t)mapSize * (size_t)mapSize * 6u;
}

// The VRAM the glow's textures take (06_draw.inc ensureGlow): the 256x512
// picture and its blur levels 1-5 and mix, RGBA8 -- 707,072 bytes -- rounded up
// to 768 KiB for the allocator's alignment. The sun shadow's map leaves this
// much for a glow that is wanted and not made yet.
constexpr size_t kGlowVramBytes = 768u * 1024u;

// THE DEPTH RAMP of the caster pass (mdk_caster.v.pica): a 512x256 RGBA8
// texture whose texel is the shadow map's encoding of a depth -- the map's
// word is (depth24 << 8) | 0xFF. Sampled NEAREST at s = depth and
// t = depth * 256 (repeating), the row is the depth's middle byte and the
// column gives its high byte; the low byte is 0, so a caster's stored depth is
// its depth rounded DOWN to 1/65536 of the range (0.12 world units of 8000).
//
// WHY 512 COLUMNS FOR A 256-VALUE BYTE. s and t are two coordinates the
// rasteriser interpolates separately, and at a depth just either side of a
// multiple of 1/256 they can land on different sides of it: the column says
// "the end of step k - 1" while the row says "just past the start of a step",
// or the reverse. Taken at face value that texel's depth is wrong by a whole
// step -- 31 world units -- a speck of false shadow or of leaked light. So the
// column carries one bit more than the byte: WHICH HALF of its step the depth
// is in. A row in the first quarter under a column in the second half can only
// be the next step's start, and a row in the last quarter under a column in
// the first half only the previous step's end; the texel holds the corrected
// byte. Anywhere else the two agree and the correction does nothing.
//
// `x`, `y` are the texel's column and its row IN MEMORY (row 0 first); the
// PICA's t runs from the LAST memory row (sceneUvFromTop), so the row that
// t = (j + 0.5) / 256 samples is memory row 255 - j.
constexpr int32_t kSunShadowRampWidth = 512;
constexpr int32_t kSunShadowRampHeight = 256;
// WHICH BYTE IS WHICH. A shadow map's word is (depth24 << 8) | softness, and a
// PICA RGBA8 texel is read as the word A:B:G:R. kShadowBytesReversed is what the
// shadow unit reads from that word: the depth's high, middle and low byte in A,
// B and G, the softness in R -- MEASURED in Mandarine r1.2.1 (2026-11-18) and,
// the emulator decoding `pixel >> 8` as the depth, the format's own layout --
// and the console's: the map it writes natively, read back, has the same bytes
// (03_frame.inc shadowByteOrder; 2026-11-18).
// kShadowBytesPica is the other order, R, G, B the depth's high, middle and low
// byte and A the softness: what the config's shadowMapFlipBytes writes.
enum ShadowByteOrder { kShadowBytesPica = 0, kShadowBytesReversed = 1 };
void sunShadowRampTexel(int32_t x, int32_t y, ShadowByteOrder order, uint8_t rgba[4]);
// The depth (0-1) a ramp sample at depth `depth` stores in the map: for the
// host suite, with the receive pass's comparison below.
float sunShadowStoredDepth(float depth);

// THE TWO GUARDS AGAINST A SURFACE SHADOWING ITSELF, the PC's scaled to the
// map's texel. The PC pushes the receiver 0.5 world units along its face normal
// (ShadowNormalOffset) and pulls its depth 0.5 units towards the sun
// (0.5 / (2 * reach) of the range: 10_enhanced.inc u.shadow[2], u.shadow[1]) --
// against a map whose texel is 600 / 2048 = 0.29 units, so the push is 1.7
// texels. A 512 map's texel is 1.17 units, and the PC's 0.5 is less than half
// of one: a surface at a grazing angle to the sun striped itself with shadow
// (measured, level 3's canyon wall, 2026-11-18). Both are therefore the PC's
// times the ratio of the two maps' sides, which keeps the push at the same 1.7
// texels: 2 world units at 512, 4 at 256. The cost is the usual one -- a shadow
// starts up to that far from the foot of what casts it.
constexpr int32_t kSunShadowPcMapSize = 2048;
float sunShadowNormalOffset(int32_t mapSize);
float sunShadowDepthBias(int32_t mapSize);

// World -> the CASTER pass's clip space: the sun matrix's x and y as they are
// (a texture target's clip x, y are its s, t: glowLevelCorner), and its depth
// d in [0, 1] as clip z = d - w, so the depth this backend's map stores
// (z + 1, kDepthMapScale / kDepthMapOffset) IS d: 0 at the sun's near plane.
void sunCasterMatrix(const float sun[16], float out[16]);

// World -> the shadow map's (u, v, depth, 1): u = x / 2 + 1/2, v = y / 2 + 1/2
// and the depth as it is -- the receive pass's texture coordinate, the same
// point the caster pass put in that texel.
void sunMapMatrix(const float sun[16], float out[16]);

// Does a texture's source have HOLES -- an index-0 texel (transparent whatever
// the palette says) or, for an RGBA source, an alpha below the half cover? The
// caster pass draws no texture of a caster's own (its one unit is the depth
// ramp), so it cannot cut a hole: a surface with holes is left out of the map
// rather than cast as if it were solid. A fence then casts NO shadow where the
// PC casts a fence's, which is the smaller wrong: level geometry carries whole
// polygons of transparent texels, and solid they shadowed half a canyon floor
// the PC leaves in the sun (measured, level 8, 2026-11-18).
bool textureSourceHasHoles(const uint8_t* source, size_t byteCount, bool indexed);

// Does a draw cast -- and so receive -- the sun's shadow? The PC's casters
// (11_shadow_post.inc drawShadowMap): triangles, Lit, in an opaque pass.
bool castsSunShadow(Pass pass, Shading shading, Primitive primitive);

// The receive pass's factor for a surface whose lit and shadowed lights are
// `lit` and `shadowed` (exposure * light, one channel): what the picture is
// multiplied by in the shadow, as mdk_shadow.g.pica computes it. For the host
// suite: the shader cannot be run there, its arithmetic can.
float sunShadowRatio(float lit, float shadowed);

// THE SHADOW TINT: the single-pass receiver's lookup (mdk_litshadow.g.pica,
// texture unit 2). What a Lit surface's light is in the sun's shadow depends on
// ONE number per triangle -- t = n.z * 0.5 + 0.5, the hemisphere's -- because
// sky, ground and exposure are the rig's: F_shadow(t) = to_srgb(exposure *
// mix(ground, sky, t)), halved and clamped at 1 like the lit program's F (its
// light stage scales x2). Along u: t from 0 (column 0) to 1 (the last column).
// Along v: the alpha is how much of the shadow survives the map's edge fade,
// 1 at t = 1 and 0 at t = 0 -- and the PICA samples t = 0 at the LAST memory
// row (sceneUvFromTop), so memory row y holds alpha (H - 1 - y) / (H - 1).
// RGBA8, made and refilled on the CPU when the rig changes (cold: a level's
// rig, 1024 texels, 384 powf -- one colour per column). 128 columns: the
// sampler interpolates linearly between them, and x^(1/2.2) is steep near
// black (review, 2026-11-19: 64 columns erred ~9/255 at a near-black ground).
constexpr int32_t kShadowTintWidth = 128;
constexpr int32_t kShadowTintHeight = 8;
void sunShadowTintTexel(int32_t x, int32_t y, const float sky[3], const float ground[3],
                        float exposure, uint8_t rgba[4]);
// The same, split for a fill: column x's colour (rgb) once, then row y's
// alpha. sunShadowTintTexel is the two together.
void sunShadowTintColumn(int32_t x, const float sky[3], const float ground[3], float exposure,
                         uint8_t rgb[3]);
uint8_t sunShadowTintAlpha(int32_t y);
// The geometry shader's lutMap uniform: u = t * out[0] + out[1] and
// v = survive * out[2] + out[3] land on the texel CENTRES of the end columns and
// rows, so t = 0 and t = 1 read exactly the first and last column.
void sunShadowTintMap(float out[4]);
// The edge fade's alpha with the shadowDarkness dial (EnhancedToggles, percent):
// how much of the shadow lands is the stage-0 product (1 - shadow) * alpha, so
// scaling the alpha scales the shadow's weight, mix(F_lit, F_shadow, A * d).
// 100 and above is sunShadowTintAlpha(y) itself, to the byte; 0 is no shadow.
uint8_t sunShadowTintAlphaDialled(int32_t y, int32_t darknessPercent);

// ---------------------------------------------------------------------------
// THE LOOK DIALS (EnhancedToggles hazeDensity / exposure / tintSaturation, in
// percent; the pause menu's sliders): the game's rig with each scaled, which
// the renderer makes once a frame and uploads as it always has (the haze
// exponent, the lit and shadow programs' uniforms, the shadow tint).
//
//   haze_density  x hazeDensity / 100
//   exposure      x exposure / 100
//   sun, sky, ground: each channel moved about the colour's Rec. 709 luminance
//                 L by tintSaturation / 100, max(L + (c - L) * s, 0). The rig's
//                 colours are skyTint(hue, k) scaled, and skyTint keeps the
//                 luminance (white and the hue are both 1), so this IS the rig
//                 with level_light's three k*Saturation constants scaled by s.
//
// A dial at 100 leaves its numbers untouched (not multiplied: the copy is the
// shipped look to the bit). Alpha channels are never touched.
// ---------------------------------------------------------------------------
void dialledLighting(const Lighting& rig, const EnhancedToggles& dials, Lighting* out);

} // namespace pica
} // namespace mdk
