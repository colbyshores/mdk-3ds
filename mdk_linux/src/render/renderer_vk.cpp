// ============================================================================
// renderer_vk.cpp — the Linux backend behind the render.h seam.
//
// This file implements the SAME seam the 3DS backend implements
// (mdk_3ds/src/render/renderer_c3d.cpp). The backend is chosen at BUILD
// TIME by which tree is compiled; there is no virtual interface, no vtable,
// and no #ifdef inside the shared code.
//
// The Linux backend has a programmable fragment shader, so it can do the
// palette lookup in the shader exactly as the C# port does -- the 3DS
// cannot, and expands the palette on the CPU instead (SPEC.md 7.3). The two
// backends differ in HOW they answer the palette, not in WHAT they draw.
//
// The pipeline states (cull, winding, depth, blend) are the C# oracle's,
// read from src/Mdk.Engine/Render/Renderer.cs CreatePipeline: front faces
// wind CLOCKWISE ON SCREEN (SDL_GPU's convention; Vulkan's framebuffer Y is
// up, so that is VK_FRONT_FACE_COUNTER_CLOCKWISE here), depth is
// LESS_OR_EQUAL, culling is BACK on the Solid pass only, blending is on for
// Blended and Overlay only.
//
// MEMORY: Stage 1 uses host-visible|host-coherent memory for every buffer
// and texture and maps it persistently. That is correct on every desktop
// driver and removes a whole class of staging/barrier bugs from the bring-up
// phase. Device-local memory with staging uploads is the upgrade, and it is
// an upgrade, not a fix.
//
// THIS FILE IS A SINGLE TRANSLATION UNIT ASSEMBLED FROM vulkan/*.inc
// (the ../t2k c3d/*.inc split, adopted 2026-11-15). The single-file form
// was 6,046 lines; split by concern, a grep hit names the section it is
// in. The .inc files are NOT headers: no guards, no includes of their
// own, and they depend on the include order below, which is the original
// file order and therefore load-bearing. The split is provably free: the
// preprocessor emits the same token stream, and the object code stayed
// byte-identical (disassembly md5, the bar ../t2k held its split to).
// ============================================================================

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <volk.h>

#include <cmath>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "render/depth_pull.h"
#include "render/colour_mips.h"
#include "render/post_params.h"
#include "render/sun_shadow.h"
#include "render/draw_queue.h"
#include "render/render.h"
#include "render/render_types.h"
#include "render/system_numerics_inverse.h"
#include "render/vk/vk_context.h"

#include "shader_palette_vert.h"
#include "shader_palette_frag.h"
#include "shader_sky_vert.h"
#include "shader_sky_frag.h"
#include "shader_stereo_vert.h"
#include "shader_stereo_frag.h"
#include "shader_gamma_frag.h"
#include "shader_enhanced_vert.h"
#include "shader_enhanced_frag.h"
#include "shader_depth_frag.h"
#include "shader_occlusion_frag.h"
#include "shader_post_frag.h"

namespace {
#include "vulkan/01_types.inc"
#include "vulkan/02_helpers.inc"
#include "vulkan/03_vulkan_helpers.inc"
#include "vulkan/04_pipelines.inc"
#include "vulkan/05_lifecycle.inc"
#include "vulkan/06_gamma_stereo.inc"

namespace {
bool ensureEnhanced(RendererVk* r);
bool ensureShadow(RendererVk* r);
void drawShadowMap(RendererVk* r);
bool ensurePost(RendererVk* r);
void drawCameraDepth(RendererVk* r);
void generateSceneMips(RendererVk* r);
void drawPost(RendererVk* r);
} // namespace

void replayCanvas(RendererVk* r);

#include "vulkan/07_frame.inc"
// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

// Defined with the enhanced look's colour cache below; the seam's
// replacement entry points sit with the other resource functions.
namespace {
void registerReplacement(RendererVk* r, int32_t tex, int32_t pal, int32_t frames, int32_t width,
                         int32_t height, const uint8_t* rgba, bool image);
void dropColour(RendererVk* r, int32_t tex, int32_t pal, int32_t frames);
} // namespace

#include "vulkan/08_resources.inc"
// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

namespace {

// ---------------------------------------------------------------------------
// The enhanced look's backend half (P6 lit pipeline slice): expanded
// colour textures on demand, a trilinear sampler, per-frame descriptor
// pools with a uniform ring, and the Lit/Sprite pipelines. The fragment
// uniforms mirror shaders/enhanced.hlsl's cbuffers, offsets included.
// ---------------------------------------------------------------------------

#include "vulkan/09_colour.inc"
#include "vulkan/10_enhanced.inc"
#include "vulkan/11_shadow_post.inc"
#include "vulkan/12_draw.inc"
} // namespace

void mdk_render_draw_mesh(MdkRenderer rr, const mdk::MeshDraw* draw) {
    RendererVk* r = asVk(rr);
    if (!r || !draw || !r->inFrame) {
        return;
    }
    // Queued, not recorded: the oracle renders the frame's commands bucketed by
    // pass, and the pass a batch belongs to is not the order it was found in.
    // render.h gives this job to the backend ("the material carries the pass,
    // so the backend sorts and batches; the caller does not").
    r->queue.add(*draw);
}

// ---------------------------------------------------------------------------
// The 2D canvas
// ---------------------------------------------------------------------------
//
// Quads are drawn immediately in the Overlay pass (blended, no depth) through
// an orthographic projection of the canvas's 360-high logical space onto the
// full viewport. The canvas width follows the frame's aspect, exactly like the
// C# port's Renderer.CanvasWidth. The C# batches quads by texture; this draws
// one draw call per quad, which is fine for a HUD and keeps the path simple.
// The vertex format is the mesh format, so the palette pipeline draws it
// unchanged.

namespace {

#include "vulkan/13_canvas.inc"
} // namespace

#include "vulkan/14_canvas_seam.inc"
