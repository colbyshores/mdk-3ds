// ============================================================================
// renderer_c3d.cpp — the 3DS backend behind the render.h seam.
//
// This file implements the SAME seam the Vulkan backend implements
// (mdk_linux/src/render/renderer_vk.cpp). The backend is chosen at BUILD
// TIME by which tree is compiled; there is no virtual interface, no vtable,
// and no #ifdef inside the shared code.
//
// THE FRAME LIFECYCLE (AGENTS.md P2, and ../forsaken/render_c3d.c):
//
//   create   -> C3D_Init(16x default command buffer)
//              C3D_RenderTargetCreate for the top (and the bottom)
//              parse the shader DVLB, build the shader program
//   begin    -> C3D_FrameBegin(C3D_FRAME_SYNCDRAW)
//              C3D_RenderTargetClear + C3D_FrameDrawOn(target)
//              C3D_BindProgram
//              EVERY GPU STATE EXPLICITLY: C3D_AttrInfo, C3D_TexEnv,
//              C3D_DepthTest, C3D_CullFace, C3D_DepthMap
//   draw     -> upload the vertices, C3D_DrawArrays
//   end      -> C3D_FrameEnd(0)
//
// ALL GPU STATE MUST BE SET EXPLICITLY EVERY FRAME. Uninitialised PICA200
// registers produce INVISIBLE GEOMETRY -- no crash, no error, no output.
// That is the single most common way a first frame comes up black.
//
// C3D_FrameEnd IS THE ONLY PRESENT PATH. Do not add a second display
// transfer: Forsaken's black screen was picaGL's GX_DisplayTransfer
// conflicting with citro3d's auto-transfer from C3D_FrameEnd.
//
// THE TWO D3D CONVERSIONS (../forsaken/3DS_PORT_NOTES.md, SPEC.md 7.2):
//   (A) depth: the engine's projection puts NDC z in [0,1] (the C# oracle's
//       System.Numerics perspective, right-handed); PICA's is [-1,0], and
//       `row[2] -= row[3]` on the combined matrix converts one to the other.
//       Once. Not twice. The depth test is then ON, with the oracle's
//       LESS_OR_EQUAL, because the map this backend installs stores 0 at the
//       near plane and 1 at the far — see mdk::pica::kDepthMapScale and the
//       evidence recorded there.
//   (B) culling: kSolidCull = GPU_CULL_FRONT_CCW, MEASURED 2026-11-17 in the
//       emulator: visible faces arrive clockwise on screen, so back-face
//       culling culls the CCW ones. The reasoned GPU_CULL_BACK_CCW that stood
//       here culled the front faces instead, hiding the arena interior and the
//       floor on every level -- this note's own warning: a wrong cull enum
//       shows up as INVISIBLE GEOMETRY, not as a wrong picture. The picture is
//       not mirrored (it matches the Linux build's orientation), so the enum,
//       not a mirror, was what was wrong. Hardware look still owed.
// ============================================================================

#include <3ds.h>
#include <citro3d.h>
#include <tex3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "render/render.h"
#include "render/render_c3d_hd_set.h"
#include "render/render_types.h"
#include "render/draw_queue.h"
#include "render/pica_matrix.h"
#include "util/matrix.h"
#include "render/sun_shadow.h"
#include "mdk_c3d_shbin.h"
#include "platform/vfp_fpscr.h"
#include "platform/log_3ds.h"
#include "platform/async_file_reader.h"

// THIS FILE IS A SINGLE TRANSLATION UNIT ASSEMBLED FROM c3d/*.inc
// (the ../t2k split of the same name, adopted 2026-11-15). The
// single-file form was 2,973 lines; split by concern, a grep hit names
// the section it is in. The .inc files are NOT headers: no guards, no
// includes of their own, and they depend on the include order below,
// which is the original file order and therefore load-bearing. The
// split is provably free: the preprocessor emits the same token stream,
// and the object code stayed byte-identical (disassembly md5).

namespace {
#include "c3d/01_types.inc"
} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

#include "c3d/02_lifecycle.inc"
#include "c3d/03_frame_state.inc"
#include "c3d/03_eye.inc"
#include "c3d/03_shadow_probe.inc"
#include "c3d/03_frame.inc"
#include "c3d/03_shadows.inc"
#include "c3d/03_toggles.inc"
// ---------------------------------------------------------------------------
// Resources — the P1 scaffold allocates nothing yet. The texture path lands
// with the palette expansion (SPEC.md 7.3).
// ---------------------------------------------------------------------------

#include "c3d/04_textures.inc"
#include "c3d/04_expansion.inc"
#include "c3d/04_make.inc"
#include "c3d/04_hd_stream.inc"
#include "c3d/04_streams.inc"
#include "c3d/05_resources.inc"
// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// Record one queued draw. This is the oracle's `DrawOne`: the texture, the UV
// transform for its animation frame, the combiner, the vertex buffer, one
// C3D_DrawArrays -- issued at FLUSH time, in the frame's pass order.
#include "c3d/06_draw.inc"
#include "c3d/06_draw_submit.inc"
#include "c3d/06_glow.inc"
#include "c3d/06_queue.inc"
#include "c3d/06_glow_billboards.inc"
// ---------------------------------------------------------------------------
// The 2D canvas
// ---------------------------------------------------------------------------

#include "c3d/07_canvas.inc"
