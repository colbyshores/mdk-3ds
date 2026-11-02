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
// `Renderer.cs:606-607` builds every pipeline's depth state from the pass:
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

} // namespace pica
} // namespace mdk
