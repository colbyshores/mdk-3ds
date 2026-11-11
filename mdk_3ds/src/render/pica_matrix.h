#pragma once

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

// The uniform layout: register i holds COLUMN i of `m`, i.e. register i gets
// out[i*4+0..3] = m[0*4+i], m[1*4+i], m[2*4+i], m[3*4+i].
//
// THE STRIDED READ. The shader consumes register i as the vector dotted with the
// position (`dp4 outpos.x, mvp[0], r0`), so register i is ROW i of the
// column-vector matrix M_pica, and M_pica = M^T for our row-vector M. Therefore
// register i must hold COLUMN i of our row-major matrix — exactly Forsaken's
// matrix_to_c3d (render_c3d.c:1077). A verbatim row copy uploads the transpose:
// rotations come out inverted and the picture is mirrored.
//
// `out` may be the `m` member of a C3D_Mtx (that union's raw view of the same 16
// floats), which is why this writes floats instead of a citro3d type.
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
// THE y AXIS IS DELIBERATELY NOT THE C#'s, AND SAYING IT WRONG HERE WOULD BE THE
// WAY TO GET IT WRONG. System.Numerics expands the oracle's call as
// `M22 = 2/(top-bottom) = +2/H`, `M42 = (bottom+top)/(bottom-top) = -1`: the
// arguments are NOT swapped (`top = CanvasHeight`, `bottom = 0`), so the oracle
// puts the canvas top (y = 0) at clip MINUS 1. The Vulkan backend is a bit-for-bit
// copy of that (`renderer_vk.cpp`'s `canvasOrtho`: `out[5] = +2/H`, `out[13] = -1`)
// because a Vulkan framebuffer is y-DOWN and wants exactly that.
//
// This target is the opposite. The engine's landscape clip is y-UP -- that is what
// `eyeViewProjection` consumes and what the scene is drawn through -- so the canvas
// has to agree with the scene it is drawn over, and the y row is NEGATED against
// the oracle: `m[5] = -2/H`, `m[13] = +1`. Getting it the oracle's way paints the
// HUD upside down and mirrored, which is at least visible.
//
// `tests/scripts/pica_matrix_tests.cpp`'s canvas row starts from the oracle's
// expansion and applies that negation as a named step, so the difference is a
// decision the test states instead of a convention it silently copies.
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
};

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

} // namespace pica
} // namespace mdk
