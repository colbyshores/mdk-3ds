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
//   (B) culling: the PICA200 has NO CW-front mode. Through the oracle's
//       right-handed camera the meshes wind CCW on screen, so "cull the back
//       faces" is C3D_CullFace(GPU_CULL_BACK_CCW). (Under the older
//       left-handed camera build the picture was mirrored and they wound CW,
//       which is where the previous GPU_CULL_FRONT_CCW came from; the mirror
//       and the cull enum move together, and getting one of them right on its
//       own shows up as INVISIBLE GEOMETRY, not as a wrong picture.)
// ============================================================================

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <utility>
#include <vector>

#include "render/render.h"
#include "render/render_types.h"
#include "render/draw_queue.h"
#include "render/pica_matrix.h"
#include "mdk_c3d_shbin.h"
#include "platform/vfp_fpscr.h"
#include "platform/log_3ds.h"

namespace {

// The 3DS's framebuffer is 240x400 portrait. The game is landscape, and the
// projection carries the 90-degree rotation.
static constexpr int kScreenWidth = 240;
static constexpr int kScreenHeight = 400;

// THE DISPLAY TRANSFER FLAGS.
//
// These are NOT optional. The default transfer format is NOT RGBA8, and a
// transfer that reads the framebuffer with the wrong format produces a
// striped garbage image -- no error, no crash, just noise. The devkitPro
// example (graphics/gpu/geoshader/source/main.c) spells the same set out.
//
//   GX_TRANSFER_OUT_TILED(0)  the framebuffer is in 32-bit tiled layout
//   GX_TRANSFER_RAW_COPY(0)   copy, do not convert
//   GX_TRANSFER_IN_FORMAT     RGBA8 -- the render target's format
//   GX_TRANSFER_OUT_FORMAT    RGB8 -- what the display takes
//   GX_TRANSFER_SCALING       no scaling
#define MDK_DISPLAY_TRANSFER_FLAGS                                     \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |             \
     GX_TRANSFER_RAW_COPY(0) |                                        \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |                    \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |                   \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// The 3DS's VRAM is 6 MB on the original, and the linear heap starts there,
// so EVERY linear allocation is VRAM until the heap runs out.
//
// The scratch is the per-frame staging area. It is 512 KB for now, because
// the meshes carry their own buffers and the scratch is not used yet. It
// grows when the frame's staging needs it, and the budget is checked
// against the free VRAM.
static constexpr size_t kGpuScratchSize = 256u * 1024u;

// The next power of two at or above `value`, and at least 8.
//
// The PICA200's texture size register encodes log2 of the dimensions, so a
// texture's width and height must be powers of two. A dimension below 8 is
// not addressable either: the texture unit works in 8x8 blocks.
static int32_t nextPowerOfTwo(int32_t value) {
    int32_t result = 8;
    while (result < value && result < 1024) {
        result *= 2;
    }
    return result;
}

// The 36-byte all-float vertex the PICA200 wants. The engine's 24-byte
// vertex (position, uv, packed RGBA8 colour) is expanded into this.
struct GpuVertex {
    float pos[3];
    float colour[4];
    float texcoord[2];
};

// A GPU texture.
//
// THE 3DS HAS NO PALETTE TEXTURE FORMAT, so an index texture is EXPANDED
// on the CPU when it is created: the palette lookup happens here, once, and
// the GPU gets a plain RGBA8 image. The expansion's rule is the C#'s
// palette shader's rule exactly:
//
//     index == 0   -> TRANSPARENT (the shader discards it)
//     otherwise     -> the palette's RGB, and the alpha is FORCED to 255
//
// The palette's own alpha is NOT used: the C#'s shader takes
// `palette_texture.Load(index).rgb` and writes an alpha of 1.0, and the
// transparency comes from the index-0 discard.
//
// The data is LINEAR, not Morton-tiled: the PICA200's 32-bit texture
// formats are read linearly, and only the 16-bit and 8-bit formats go
// through the tiling transform. RGBA8 is also the only format that is
// LOSSLESS for MDK's palettes -- a measurement over the GOG data found that
// RGBA4 round-trips 0 of the 926 textures exactly, because the palettes
// are dithered ramps and 8->4 bits only round-trips multiples of 17. So
// the 3DS spends its VRAM on colour fidelity.
struct C3dTexture {
    C3D_Tex tex;
    bool valid = false;

    // The real size, and the size the texture was PADDED to. The
    // difference is what the UV transform has to undo.
    int32_t realWidth = 0;
    int32_t realHeight = 0;
    int32_t paddedWidth = 0;
    int32_t paddedHeight = 0;

    // realHeight / frameCount, computed once per texture on first draw.
    // The ARM11 has no integer divide (R11), so the per-draw site caches the
    // quotient here instead of dividing every batch every frame.
    int32_t frameHeight = 0;

    // When this texture was last drawn, for the eviction order.
    uint32_t lastUsed = 0;

    // THE SOURCE, kept so that an EVICTED texture can be made again.
    //
    // THE EVICTION HAS TO BE REVERSIBLE. The 3DS has six megabytes of VRAM
    // and the linear heap starts there, so a texture that does not fit is
    // evicted -- and an evicted texture whose source is gone can never come
    // back, because the caller holds an id and not the pixels. The symptom
    // is an arena surface that stops being textured for the rest of the
    // session: the id is still good, the GPU copy is not, and the draw is
    // skipped.
    //
    // The source is in SYSTEM memory, not the GPU's: the palette indices
    // are a quarter of the expanded RGBA8 and an eighth of the padded one.
    std::vector<uint8_t> source;
    // The 256x1 palette the indices expand through (index textures only).
    std::vector<uint8_t> palette;
    // True when `source` holds palette indices, false when it holds the
    // RGBA bytes of an RGBA texture.
    bool indexed = false;

    // The source changed while the old GPU copy may still be in flight. The
    // replacement waits for the next frame boundary (AGENTS.md P3).
    bool stale = false;

    // The caller released this texture while its GPU copy may still be in
    // flight. The copy waits for the next frame boundary; the id is already
    // invalid to the caller.
    bool releasePending = false;
};

// A mesh: the vertices, in LINEAR memory, where the GPU reads them.
struct C3dMesh {
    void* data = nullptr;
    int32_t capacity = 0;
    int32_t count = 0;
    bool valid = false;
    // The caller has given the mesh up, but the linear memory is still owed to
    // a frame that may be on the GPU. Freed at the next frame boundary, exactly
    // like a texture's releasePending (AGENTS.md P3).
    bool releasePending = false;
};

// The one frame whose geometry is censused (see `Census`). It has to be a frame
// after the level-7 landing (the golden lands at step 43) so the census measures
// the standing camera the golden pins, and it has to be well inside the 30 s the
// emulator harness runs, which reaches frame ~1700.
static constexpr int32_t kCensusFrame = 120;

// A one-shot census of ONE frame's triangles, for the named level-7 picture
// defect (docs/CONTINUATION.md "NEXT": "sample triangles near the spawn and
// compare the vertices/MVP/clip values with host-derived geometry; inspect the
// PICA winding and depth/cull behavior before modifying renderer state").
//
// Batch 77 traced the FIRST triangle of the first draw and that was not enough:
// it is a far-distance triangle at y=438 and it says nothing about the room the
// camera is standing in. This counts every triangle the frame queues, in the
// frame's real per-draw matrix, and sorts it by where it landed: behind the
// camera, outside the clip square, or on screen -- and for the on-screen ones,
// which way they wind. The winding is what decides the cull enum, and the
// on-screen count is what separates "the room is not queued" from "the room is
// queued and the GPU is throwing it away".
//
// It is COLD by construction: `active` is true for exactly one frame of the
// process, so the per-triangle loop runs once in a run and every other frame
// pays one integer compare.
struct Census {
    bool active = false;
    int32_t triangles = 0;   // queued triangles seen
    int32_t behind = 0;      // a vertex with clip w <= 0
    int32_t offscreen = 0;   // the whole bbox outside [-1,1] IN X/Y
    int32_t onscreen = 0;    // the bbox touches the clip square IN X/Y — see
                             // depthOut for the depth half, which this does NOT
                             // test: "inside the clip square" is an x/y statement
                             // and reading it as "the GPU will show it" over-reads
                             // it (review-1 F5).
    int32_t depthOut = 0;    // of those, the ones wholly outside [-1,1] in ndc z
    int32_t cw = 0;          // on-screen, negative signed area in NDC (y up)
    int32_t ccw = 0;         // on-screen, positive
    int32_t near60 = 0;      // on-screen AND within 60 units of the eye
    float areaSum = 0.0f;    // sum of |ndc signed area| over the on-screen ones
    float loX, hiX, loY, hiY;   // the ndc bbox of everything on screen
    float nLoX, nHiX, nLoY, nHiY;  // the same, for the near60 ones only
    float minW = 0.0f;       // nearest and farthest clip w seen
    float maxW = 0.0f;
    float bestW = -1.0f;     // the closest on-screen triangle's w, for the sample
    int32_t bestDraw = -1;
    int32_t bestFirst = -1;
    float bestV[9] = {0};    // its three world positions
    float bestArea = 0.0f;
    float bestNdc[6] = {0};  // its first two vertices' ndc x,y
};

struct RendererC3D {
    C3D_RenderTarget* targetLeft = nullptr;
    C3D_RenderTarget* targetRight = nullptr;
    C3D_RenderTarget* targetBottom = nullptr;

    // The camera, kept so each eye can be rendered with its own offset.
    float view[16] = {0};
    float projection[16] = {0};

    // The eyes' separation, in the game's units.
    float eyeSeparation = 0.0f;

    DVLB_s* dvlb = nullptr;
    shaderProgram_s program;

    // The uniforms' LOCATIONS, not their register numbers.
    //
    // THE PICA'S UNIFORM REGISTERS AND ITS CONSTANT REGISTERS ARE THE SAME
    // REGISTERS, and the constants come FIRST. A shader that declares
    // `.constf constants(...)` before its uniforms pushes the uniforms up
    // by however many registers the constants took -- so the matrix is NOT
    // at registers 0-3, and writing it there overwrites the constants.
    //
    // The symptom is a screen that shows only the clear colour: the
    // shader reads garbage for its matrix, every vertex lands outside the
    // clip volume, and nothing is rasterised. There is no error.
    //
    // The location is what the shader's own uniform table says, and it
    // accounts for the constants.
    int mvpLocation = 0;
    int uvLocation = 0;

    // The frame's per-eye view-projection, with both PICA conversions baked in,
    // for the target this frame's draws go to. A queued draw that carries a
    // world matrix composes it against THIS matrix (pica::drawMatrix), which is
    // what lets the 3DS place an object at all: with one frame-wide matrix and
    // no per-draw one, every object was drawn at the arena's own coordinates no
    // matter where its script put it.
    //
    // `installEye` writes it once per eye pass, immediately before that eye's
    // replay of the queue, and the draws read the matrix of the eye they are
    // in. There is one entry and not two because a frame draws an eye, then the
    // next: the queue is replayed per eye (batch 30), so the matrix that is
    // current is always the matrix of the eye being drawn -- and a draw that
    // carries its own world composes against it (`drawMatrix`, cached below).
    float eyeVp[16] = {0};

    // Whether the uniform currently holds `eyeVp` (the frame's matrix) or some
    // draw's own.
    //
    // C3D_FVUnifMtx4x4 dirties the uniform buffer every time it is called, so
    // the level geometry -- every draw of which carries an identity world --
    // must not pay for a write it does not need. This is the cache the cull mode
    // below uses, for the same reason: a per-draw C3D_* call is real ARM11 time,
    // and the vault's "redundant per-draw PICA200 state writes" note is that
    // lesson confirmed on two other projects. Anything that writes the matrix
    // behind this backend's back has to say so here.
    bool matrixIsFrame = true;

    // This frame's eye plan (how many passes, and each eye's offset), computed
    // once in mdk_render_begin_frame and consumed by mdk_render_end_frame.
    mdk::pica::EyePlan eyes;

    // The vertex scratch. Everything the CPU writes for the frame lives
    // here until it is uploaded.
    void* scratch = nullptr;
    size_t scratchSize = 0;

    // The GPU resources. The ids the seam hands out are indices into these.
    std::vector<C3dTexture> textures;

    // The draw counter, for the eviction order.
    uint32_t drawCounter = 0;
    // The draw counter's value when the current frame began. A texture
    // stamped at or after it has been used by THIS frame's commands, and
    // the GPU may still be reading it: it cannot be freed (AGENTS.md P3).
    uint32_t frameStartUse = 0;
    std::vector<C3dMesh> meshes;

    // The frame's queued draws, submitted in the oracle's pass order. The C#
    // collects every command of the frame and renders it one pass at a time
    // (Renderer.cs:1000-1004); recording draws as they arrive instead puts a
    // blended object in front of arena glass that had not been drawn yet, and
    // on THIS backend -- where the depth test is off and paint order is the
    // only occlusion there is -- that is the whole picture (draw_queue.h).
    mdk::DrawQueue queue;

    // The palettes, kept so a texture can be expanded later. The 3DS has
    // no palette texture, so the palette's only job is the expansion.
    std::vector<std::vector<uint8_t>> palettes;

    // The cull mode last written to the CITRO3D CONTEXT. citro3d keeps the
    // render state in ONE global context, not per render target --
    // C3D_FrameDrawOn does not restore it -- so anything that binds another
    // target mid-frame inherits this mode.
    //
    // C3D_CullFace always marks the effect block dirty, even when the value
    // is unchanged; without this cache every mesh draw would flush an
    // effect-block rebind at the next draw.
    GPU_CULLMODE cullMode = GPU_CULL_NONE;

    // The depth state last written to the citro3d CONTEXT, for the same reason
    // the cull mode above is cached: C3D_DepthTest dirties the effect block on
    // every call, so an all-Solid frame must not re-install it per draw.
    //
    // Two bools and not one enum, because the oracle's rule has two independent
    // bits (Renderer.cs:606-607): the blended pass tests without writing, and
    // the exempt passes neither test nor write.
    bool depthTest = true;
    bool depthWrite = true;

    // The blend state last written to the citro3d CONTEXT, cached the same way.
    //
    // The pair it was derived from is cached rather than the factors, because
    // `pica::blendStateForPass` is a pure function of exactly these two and
    // two integer compares are cheaper than five. -1 is "nothing installed",
    // so the first draw of a frame always writes.
    int blendPass = -1;
    int blendMode = -1;

    int frame = 0;
    int32_t drawCount = 0;
    int32_t triangleCount = 0;
    Census census;
    bool diagnosticTriangle = true;
    bool ready = false;
    bool inFrame = false;

    // True after C3D_FrameEnd and before the next C3D_FrameBegin. The GPU
    // may still be reading the last frame's textures then. C3D_FrameBegin
    // waits for the previous queue, so it is the completion boundary.
    bool gpuPending = false;
};

RendererC3D* asC3d(MdkRenderer r) {
    return static_cast<RendererC3D*>(r);
}

// The MDK vertex, expanded into the PICA200's layout.
inline GpuVertex expand(const mdk::Vertex& v) {
    GpuVertex g;
    g.pos[0] = v.pos.x;
    g.pos[1] = v.pos.y;
    g.pos[2] = v.pos.z;
    // The engine's colour is RGBA8 with R in the low byte.
    g.colour[0] = (float)(v.colour & 0xFFu) / 255.0f;
    g.colour[1] = (float)((v.colour >> 8) & 0xFFu) / 255.0f;
    g.colour[2] = (float)((v.colour >> 16) & 0xFFu) / 255.0f;
    g.colour[3] = (float)((v.colour >> 24) & 0xFFu) / 255.0f;
    g.texcoord[0] = v.uv.x;
    g.texcoord[1] = v.uv.y;
    return g;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

 MdkRenderer mdk_render_create(const MdkRenderDesc* desc) {

    // R1: the FPSCR's FZ and DN bits MUST be set before any application float
    // work. This is the main thread, and main_3ds.cpp calls the helper
    // before gfxInitDefault; this is the defensive second call, and it is
    // cheap. The read-back is the point of the helper, so its result is not
    // discarded: a failure here means the FPSCR did not take the bits and
    // every float result in this process is suspect.
    if (!mdk::vfpSetFzDn()) {
        mdk::log3dsPrintf("FZ/DN read-back FAILED in renderer create");
    }

    RendererC3D* r = new RendererC3D();

    // The command buffer is 4x the default (256 KB -> 1 MB).
    //
    // IT IS LINEAR MEMORY, AND THE LINEAR HEAP STARTS IN THE VRAM. A 4 MB
    // command buffer -- which is what Forsaken uses, for the draw-call
    // spikes -- takes four of the 3DS's SIX megabytes of VRAM, and then the
    // render targets and the textures have nothing left. The symptom is
    // every texture failing to allocate and a screen that shows only the
    // clear colour.
    //
    // One megabyte is the compromise: enough for a busy frame's draw calls,
    // and it leaves the VRAM for the pictures.
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2)) {
        mdk::log3dsPrintf("C3D_Init FAILED");
        delete r;
        return nullptr;
    }

    // The top screen's eye pair. 240x400 portrait; the projection rotates it.
    r->targetLeft = C3D_RenderTargetCreate(kScreenWidth, kScreenHeight,
                                           GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!r->targetLeft) {
        mdk::log3dsPrintf("C3D_RenderTargetCreate(left) FAILED");
        C3D_Fini();
        delete r;
        return nullptr;
    }
    r->targetRight = C3D_RenderTargetCreate(kScreenWidth, kScreenHeight,
                                             GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!r->targetRight) {
        mdk::log3dsPrintf("C3D_RenderTargetCreate(right) FAILED");
        C3D_RenderTargetDelete(r->targetLeft);
        C3D_Fini();
        delete r;
        return nullptr;
    }
    C3D_RenderTargetSetOutput(r->targetLeft, GFX_TOP, GFX_LEFT,
                              MDK_DISPLAY_TRANSFER_FLAGS);
    C3D_RenderTargetSetOutput(r->targetRight, GFX_TOP, GFX_RIGHT,
                              MDK_DISPLAY_TRANSFER_FLAGS);

    // THE DEPTH STATE, IN THE LOG.
    //
    // WHY THIS IS HERE. The depth map, the test function and the clear value are
    // three numbers that have to agree with each other, and the failure mode of
    // their disagreeing is a screen showing only the clear colour — no error,
    // no crash, nothing to look at. This host has no 3DS emulator and no
    // Old-3DS, so the picture cannot be the evidence (AGENTS.md P6). The log
    // file can: these are the numbers the frame actually installs, read back
    // from the same constants the frame installs, so a run on hardware records
    // the state even when nobody can see the screen.
    mdk::log3dsPrintf("depth: map z*%.1f+%.1f near=%.4f far=%.4f clear=0x%06x test=LEQUAL",
                      (double)mdk::pica::kDepthMapScale,
                      (double)mdk::pica::kDepthMapOffset,
                      (double)mdk::pica::storedDepth(-1.0f),
                      (double)mdk::pica::storedDepth(0.0f),
                      (unsigned)mdk::pica::kDepthClearValue);

    // The shader.
    r->dvlb = DVLB_ParseFile((u32*)mdk_c3d_shbin, mdk_c3d_shbin_size);
    if (!r->dvlb) {
        mdk::log3dsPrintf("DVLB_ParseFile FAILED");
        C3D_RenderTargetDelete(r->targetLeft);
        C3D_RenderTargetDelete(r->targetRight);
        C3D_Fini();
        delete r;
        return nullptr;
    }
    const Result spInit = shaderProgramInit(&r->program);
    if (R_FAILED(spInit)) {
        mdk::log3dsPrintf("shaderProgramInit FAILED result=0x%08lx", (unsigned long)spInit);
        DVLB_Free(r->dvlb);
        C3D_RenderTargetDelete(r->targetLeft);
        C3D_RenderTargetDelete(r->targetRight);
        C3D_Fini();
        delete r;
        return nullptr;
    }
    // The DVLB's first DVLE is the vertex shader.
    mdk::log3dsPrintf("dvlb numDVLE=%d", (int)r->dvlb->numDVLE);
    const Result spVsh = (r->dvlb->numDVLE >= 1)
        ? shaderProgramSetVsh(&r->program, &r->dvlb->DVLE[0])
        : (Result)-1;
    if (R_FAILED(spVsh)) {
        mdk::log3dsPrintf("shaderProgramSetVsh FAILED result=0x%08lx", (unsigned long)spVsh);
        shaderProgramFree(&r->program);
        DVLB_Free(r->dvlb);
        C3D_RenderTargetDelete(r->targetLeft);
        C3D_RenderTargetDelete(r->targetRight);
        C3D_Fini();
        delete r;
        return nullptr;
    }
    // Logged on SUCCESS as well as failure, and that is the whole point of the
    // line: under Mandarine the guest stops between this and the next line, and
    // a log that only records failures cannot tell "hung in setVsh" apart from
    // "hung in configure". Measured 2026-11-05: `dvlb numDVLE=1` is the last
    // line the guest ever writes, so the pair below is what localises it.
    mdk::log3dsPrintf("shaderProgramSetVsh ok result=0x%08lx", (unsigned long)spVsh);

    // THERE IS NO shaderProgramConfigure HERE, ON PURPOSE.
    //
    // What the shipped binaries say (both read out of the devkitARM archives on
    // 2026-11-05 with objdump -dr, not remembered):
    //   * libctru `shaderProgramSetVsh` frees the old instance, mallocs a new
    //     one and tail-calls `shaderInstanceInit`. It uploads nothing.
    //   * citro3d `C3D_BindProgram` does not configure either: it marks the
    //     shader dirty in the context state — 0x1900 when no program was bound
    //     before, 0x900 / 0x1100 when the vertex or geometry DVLE differs — and
    //     `C3Di_UpdateContext` is what calls
    //     `shaderProgramConfigure(sp, state>>11 & 1, state>>12 & 1)`, from
    //     inside the frame.
    // So the bind in `drawFrame` already uploads the microcode exactly once, and
    // the create-time call that used to sit here was a SECOND send, issued
    // outside any `C3D_FrameBegin`, into whatever GPUCMD command buffer happened
    // to be current before the first frame existed. `shaderProgramConfigure` is
    // GPUCMD_Add from end to end — permutation/operand writes plus the
    // microcode-DEST sequence — so a send outside the frame contract is the
    // suspect for the stop, and it is the only call between the last line the
    // guest ever writes and the readiness token `backend:` that main prints.
    //
    // This is the library's own contract, not an invention: SPEC's create step is
    // "parse the DVLB, build the shader program" and the sibling citro3d port's
    // frame entry does `C3D_FrameBegin` then `C3D_BindProgram` with no configure
    // anywhere at create time. Verified by running it, not by inspection
    // (mdk_3ds/tools/emurun.sh, see docs/PROGRESS_LOG.md).

    // The uniform locations.
    r->mvpLocation = shaderInstanceGetUniformLocation(r->program.vertexShader, "mvp");
    r->uvLocation = shaderInstanceGetUniformLocation(r->program.vertexShader, "uvtransform");
    mdk::log3dsPrintf("uniforms: mvp=%d uvtransform=%d", (int)r->mvpLocation, (int)r->uvLocation);

    r->scratchSize = kGpuScratchSize;
    r->scratch = linearAlloc(r->scratchSize);
    if (!r->scratch) {
        mdk::log3dsPrintf("linearAlloc(%u) FAILED", (unsigned)r->scratchSize);
        shaderProgramFree(&r->program);
        DVLB_Free(r->dvlb);
        C3D_RenderTargetDelete(r->targetLeft);
        C3D_RenderTargetDelete(r->targetRight);
        C3D_Fini();
        delete r;
        return nullptr;
    }

    mdk::log3dsPrintf("renderer created: target=%p dvlb=%p numDVLE=%d scratch=%p",
                      (void*)r->targetLeft, (void*)r->dvlb, (int)r->dvlb->numDVLE, r->scratch);
    r->eyeSeparation = desc ? (float)desc->stereo.separation : 0.0f;

    r->ready = true;
    return r;
}

 void mdk_render_destroy(MdkRenderer rr) {
    RendererC3D* r = asC3d(rr);
    if (!r) return;

    if (r->scratch) {
        linearFree(r->scratch);
        r->scratch = nullptr;
    }
    // The textures and the meshes are NOT freed here, and that is
    // deliberate.
    //
    // Their memory comes from the linear heap (C3D_TexInit's linear path is
    // a linearAlloc, and so is a mesh's vertex array), and `C3D_Fini` below
    // is what waits for the render queue to drain -- it calls
    // gxCmdQueueWait with an infinite timeout before it stops the queue.
    // Freeing the textures before that wait would hand back memory the GPU
    // may still be reading (AGENTS.md P3); the heap itself goes when the
    // process exits, which is the only caller of this destructor.
    if (r->targetLeft) {
        C3D_RenderTargetDelete(r->targetLeft);
        r->targetLeft = nullptr;
    }
    if (r->targetRight) {
        C3D_RenderTargetDelete(r->targetRight);
        r->targetRight = nullptr;
    }
    if (r->dvlb) {
        shaderProgramFree(&r->program);
        DVLB_Free(r->dvlb);
        r->dvlb = nullptr;
    }
    C3D_Fini();
    delete r;
}

 const char* mdk_render_backend_name(MdkRenderer r) {
    (void)r;
    return "PICA200 Citro3D";
}

 mdk::MdkCapabilities mdk_render_capabilities(MdkRenderer r) {
    (void)r;
    mdk::MdkCapabilities caps;
    // THE 3DS GETS THE ORIGINAL LOOK. The enhanced look needs a programmable
    // fragment shader (for the palette lookup, the tone curve, the ambient
    // occlusion and the haze) and PICA200's fragment stage is fixed-function.
    // See SPEC.md 5.3 and 7.3.
    caps.enhanced_look = false;
    caps.msaa = false;              // the PICA200 has no multisampling
    caps.colour_mips = false;       // no mip chains on the original look
    caps.hardware_stereo = true;    // the top screen's parallax barrier
    caps.flat_stereo = false;       // SBS / interlaced composites are for flat displays
    caps.insets = true;             // the PICA viewport is a hardware clip rect
    caps.canvas_filtering = false;  // the palette lookup is fixed-function
    caps.panorama_linear = false;
    caps.max_texture_size = 1024;
    caps.max_canvas_quads = 512;

    // THE SPRITE RESIDENCY BUDGET.
    //
    // THE ARITHMETIC. The 3DS has six megabytes of VRAM and the linear heap
    // starts there, so everything the GPU reads comes out of it first:
    //
    //   the two eye targets      2 x (400x240 colour + 400x240 depth)  1.5 MiB
    //   the command buffer        C3D_DEFAULT_CMDBUF_SIZE x 2         0.5 MiB
    //   an arena's textures       the expanded, padded RGBA8          ~3.0 MiB
    //                                                            ----------
    //                                                            ~5.0 MiB
    //
    // which leaves about a megabyte for everything else -- and Kurt's
    // sprite frames are 96 KiB each as GPU textures (measured: 49.94 MiB for
    // all 360 frames of TRAVSPRT.BNI, 12.00 MiB for K_CHUTEC alone). So the
    // sprite cache keeps ONE megabyte resident and makes the rest again as
    // they are wanted.
    //
    // THE NUMBER IS AN ESTIMATE from the arithmetic above, not a
    // measurement: the Old-3DS timing and the real residency are still
    // UNMEASURED (AGENTS.md P6). What the bound buys is that the set can
    // never be the 49.94 MiB the eager upload asked for.
    caps.sprite_residency_bytes = 1024 * 1024;
    return caps;
}


// ---------------------------------------------------------------------------
// The MVP for one eye lives in render/pica_matrix.cpp.
//
// It moved there so a host test can pin the three PICA conversions (the free
// transpose, the [0,1]->[-1,0] depth remap, the 90-degree screen rotation) and
// the SIDE a per-draw world matrix composes on -- none of which can be checked
// from a file that includes <3ds.h>, and all of which fail as INVISIBLE
// GEOMETRY rather than as a wrong picture. The conventions, and the identity
// `W*(V*P*E*R) == (W*V*P)*E*R` that makes a per-draw matrix legal at all, are
// written out in that header.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

static bool makeTexture(RendererC3D* renderer, C3dTexture& texture, bool diagnostics);

// Settle the texture changes that could not be made while the GPU might
// still be reading the old copy.
//
// C3D_FrameBegin has waited for the previous queue by the time this runs,
// so deleting and replacing these copies is safe (AGENTS.md P3). A release
// wins over an update: the id is already invalid and its source is gone.
static void settleDeferredTextures(RendererC3D* renderer) {
    if (!renderer) {
        return;
    }
    // Releases come first: they return memory without needing an
    // allocation, and a stale replacement below may need that memory.
    for (size_t i = 0; i < renderer->textures.size(); i++) {
        C3dTexture& texture = renderer->textures[i];
        if (!texture.releasePending) {
            continue;
        }
        if (texture.valid) {
            C3D_TexDelete(&texture.tex);
            texture.valid = false;
        }
        texture.releasePending = false;
        texture.stale = false;
        texture.source.clear();
        texture.palette.clear();
    }
    for (size_t i = 0; i < renderer->textures.size(); i++) {
        C3dTexture& texture = renderer->textures[i];
        if (!texture.stale) {
            continue;
        }
        texture.stale = false;
        if (texture.valid) {
            C3D_TexDelete(&texture.tex);
            texture.valid = false;
        }
        if (!texture.source.empty() && makeTexture(renderer, texture, false)) {
            // The replacement is protected until it is first drawn.
            texture.lastUsed = ++renderer->drawCounter;
        }
    }
}

// Defined with the resource code below; the frame boundary needs them and it
// comes first in the file.
static void settleDeferredMeshes(RendererC3D* renderer);
static void replayQueue(RendererC3D* r);
static void drawDiagnostic(RendererC3D* r);
static void installEye(RendererC3D* r, C3D_RenderTarget* target, float offset);

// Install one pass's depth state on the citro3d context and record it in the
// cache.
//
// THE WRITE MASK IS NOT THE TEST. The mask is the PICA's combined colour+depth
// write mask, so "test, but do not write depth" is GPU_WRITE_COLOR — write the
// colour, not the depth — and NEVER 0: a mask of 0 asks the hardware to write
// nothing at all, which is a draw that happens and is invisible. This frame used
// to install `(GPU_WRITEMASK)0` along with a disabled test; whether that mask
// gates colour is exactly the sort of thing this host cannot check, so nothing
// here relies on the answer.
//
// GPU_LEQUAL and not GPU_LESS: a fragment exactly on the far plane stores
// exactly the value the buffer was cleared to, so a strict LESS would reject the
// far plane itself. The oracle says LESS_OR_EQUAL (Renderer.cs:605), and
// `pica::depthPasses` pins that tie.
static void installDepthState(RendererC3D* r, mdk::pica::DepthState state) {
    C3D_DepthTest(state.test, state.test ? GPU_LEQUAL : GPU_ALWAYS,
                  state.write ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
    r->depthTest = state.test;
    r->depthWrite = state.write;
}

// The host-readable factor to the GPU's. The switch is exhaustive over the
// four the oracle's rule can name, and the default is unreachable: a new
// factor in `pica_matrix.h` without a line here lands on ONE, which is the
// replace, i.e. the draw paints what it was given -- the least surprising
// failure for a factor this backend was never told about.
static GPU_BLENDFACTOR gpuBlendFactor(mdk::pica::BlendFactor factor) {
    switch (factor) {
    case mdk::pica::kBlendZero: return GPU_ZERO;
    case mdk::pica::kBlendOne: return GPU_ONE;
    case mdk::pica::kBlendSrcAlpha: return GPU_SRC_ALPHA;
    case mdk::pica::kBlendOneMinusSrcAlpha: return GPU_ONE_MINUS_SRC_ALPHA;
    }
    return GPU_ONE;
}

// Install one pass's blend state on the citro3d context and record what it was
// derived from.
//
// WHY THIS IS WRITTEN AT ALL, WHEN THE PICTURE ALREADY LOOKS BLENDED. citro3d
// does not leave this register alone: C3D_Init calls
// `C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA,
// GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA)` -- measured
// from the linked libcitro3dd.a, not from a doc -- so blending has been ON for
// every pass of every frame since the port's first build, with the oracle's
// COLOUR pair and not its alpha pair. That coincidence is what made glass look
// right, and it is a coincidence in three separate ways: the four opaque passes
// blend when the oracle says they must not (so a texel of alpha 128 is
// translucent here and opaque in the game), the alpha channel's source factor
// is SRC_ALPHA where the oracle writes ONE, and `Blend::Add` has no
// representation anywhere. AGENTS.md P2 is the rule that settles it: all GPU
// state is set explicitly every frame, and a register whose value comes from a
// library's initialiser is not set by this backend.
//
// THE DISABLE IS A PAIR, NOT A FLAG. The PICA has no "blend off"; the oracle's
// `enable_blend = false` is (ONE, ZERO) on both channels. That is only safe
// because of the alpha test installed next to it -- see
// `pica::alphaTestPasses` -- since an index-0 texel that is BLENDED with alpha 0
// still writes depth, and one that is DISCARDED does not.
static void installBlendState(RendererC3D* r, mdk::Pass pass, mdk::Blend blend) {
    const mdk::pica::BlendState s = mdk::pica::blendStateForPass(pass, blend);
    // `enable` is read here, not only asserted by the host suite: a state the
    // oracle calls disabled installs as the replace pair whatever factors it
    // carries, so the struct cannot claim "off" and mix anyway (review-4 F6).
    // This changes no pixel -- the four opaque states already carry that pair
    // (pica::kBlendReplace) -- it removes a way of being wrong that nothing
    // downstream could check. One call site, because the frame gate counts them
    // (`alpha-blend-single-writer`) and a second writer would write behind the
    // blend cache's back.
    const mdk::pica::BlendState eff = s.enable ? s : mdk::pica::kBlendReplace;
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   gpuBlendFactor(eff.srcColour), gpuBlendFactor(eff.dstColour),
                   gpuBlendFactor(eff.srcAlpha), gpuBlendFactor(eff.dstAlpha));
    r->blendPass = static_cast<int>(pass);
    r->blendMode = static_cast<int>(blend);
}

// Install ONE EYE: its target, its matrix, and every piece of fixed state the
// draws need. Called from mdk_render_end_frame once per eye pass, immediately
// before that eye's replay of the queue -- C3D_FrameDrawOn selects the target
// for the draws that FOLLOW it, so an install that is not immediately followed
// by its own draws is an install that gets overwritten.
//
// Most of this state is context-global in citro3d (C3D_FrameDrawOn does not
// restore it -- see the cull-mode cache above), so re-installing it per eye is
// belt and braces rather than the mechanism. It is written every frame anyway,
// because an unwritten PICA register is the invisible-geometry failure mode and
// not an error (AGENTS.md P2).
static void installEye(RendererC3D* r, C3D_RenderTarget* target, float offset) {
    C3D_FrameDrawOn(target);

    // The MVP for this eye: the view-projection with both PICA conversions
    // baked in. It is KEPT on the renderer, because a draw that carries a
    // world matrix composes it against this matrix instead of rebuilding the
    // whole chain per draw.
    mdk::pica::eyeViewProjection(r->view, r->projection, offset, r->eyeVp);
    // THE STRIDED READ -- see mdk::pica::registers: register i must hold
    // COLUMN i of the row-major matrix, or a rotation comes out inverted.
    //
    // ALL UNIFORM REGISTERS MUST BE WRITTEN EVERY FRAME. An unwritten
    // uniform holds the previous frame's value or garbage. The shader
    // reads registers 0-3 (the mvp), so exactly those are written. A frame
    // whose draws all carry an identity world is placed by this one write
    // and nothing more; drawRecorded adds one per draw that does not.
    C3D_Mtx gpuMtx;
    mdk::pica::registers(r->eyeVp, gpuMtx.m);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->mvpLocation, &gpuMtx);
    r->matrixIsFrame = true;

    C3D_BindProgram(&r->program);

    // EVERY GPU STATE, EVERY FRAME. Uninitialised registers give
    // invisible geometry, not an error.
    C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
    AttrInfo_Init(attrInfo);
    // THE ATTRIBUTES MUST MATCH THE VERTEX LAYOUT EXACTLY. The mesh's
    // vertex is FIVE floats -- the position and the texture coordinate
    // -- and the attribute loaders say so here. Declaring a third
    // loader (a colour) that the vertex does not have makes the
    // attribute unit read past the end of every vertex, and the
    // geometry vanishes with no error at all.
    AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);   // position
    AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2);   // texcoord

    // The combiner. The original look is an unlit, untextured surface for
    // now; the textured path replaces this when the palette expansion
    // lands.
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

    // THE DEPTH TEST IS ON, with the oracle's LESS_OR_EQUAL and a write
    // mask, for the frame's default pass. The ORIGINAL look still draws
    // back to front — the queue's pass order is the oracle's and the
    // painter's order still holds — but the C# does not rely on that: it
    // builds every mesh pipeline with `compare_op = LESS_OR_EQUAL`, depth
    // test enabled and depth write enabled except on the blended and exempt
    // passes (Renderer.cs:603-607). Without a test, an arena batch that the
    // queue order happens to place late can paint over something nearer,
    // and the depth pull (`render/depth_pull.cpp`, the oracle's DepthPull
    // for a poster over its wall) has nothing to bias against.
    //
    // The map this pairs with, and the evidence for the pairing, are in
    // pica_matrix.h next to the constants; the ordering they imply is pinned
    // by `PASS PicaMatrix` because no picture on this host can show it.
    installDepthState(r, mdk::pica::depthStateForPass(mdk::Pass::Solid));

    // THE ALPHA TEST IS THE ORACLE'S DISCARD, and it is frame state: the
    // palette shader discards index 0 on every surface it draws, so the test is
    // installed once per eye and no draw is allowed to turn it off. The
    // reference is `pica::kAlphaTestReference` and the function is GPU_GREATER,
    // so the predicate the hardware applies is the one `pica::alphaTestPasses`
    // states and the host suite pins.
    //
    // citro3d's initialiser leaves this DISABLED
    // (`C3D_AlphaTest(false, GPU_ALWAYS, 0)`, measured from the same object
    // file as the blend call above), so before this line the port had no
    // discard at all: index-0 texels were made invisible by blending with alpha
    // 0 and went on writing depth, hiding the world behind every transparent
    // texel of every arena texture.
    //
    // THREE CORNERS ARE NOT THE ORACLE'S, and they are recorded rather than
    // hidden. (1) The discard is `palette.hlsl`'s and it sits inside its
    // TEXTURED branch, so a FLAT colour of alpha 0 on an unblended pass is
    // painted by the oracle and discarded here. No shipped material does that
    // -- the flat colours are glass (alpha from the DTI, on a blended pass) and
    // the mirrors (alpha 1) -- and the alternative is a per-draw test keyed on
    // "is this a palette texture", for a case the data does not contain.
    // (2) The oracle's OTHER fragment programs do not discard at all: both
    // `mirror.hlsl:36` and `sky.hlsl:63,67` return `float4(..., 1.0)` with no
    // `discard`, so a mirror or sky repaint of palette index 0 paints
    // palette[0].rgb in the oracle and is discarded by this frame-wide test.
    // Neither pass is reachable on this backend yet -- there is no mirror
    // panorama and no sky pass -- and both already diverge in colour for other
    // reasons, so the frame-wide test stays and the corner is written down
    // where the next mirror batch will find it (review-3 F5).
    // (3) `enhanced.hlsl:199-201`, the MODE_CANVAS and MODE_IMAGE branches,
    // return the texel's OWN alpha (`float4(..., texel.a) * colour`) with no
    // discard, so a canvas or image texel of alpha 0 is painted by the oracle
    // and discarded by this frame-wide test. RECORDED, NOT ESTABLISHED
    // (review-5 F5 -- the earlier version of this note claimed a divergence and
    // handed the canvas batch a work item, and neither holds). Every canvas call
    // site is `Pass.Overlay` in the oracle (`Renderer.cs:320,325,329`, and
    // `Renderer.Insets.cs:127` draws the whole canvas in that pass), and
    // Overlay is blended SRC_ALPHA / ONE_MINUS_SRC_ALPHA (`Renderer.cs:565,
    // 571-573`), where a fragment of alpha 0 leaves the target bit-for-bit as a
    // discard does; `Blend::Add` (SRC_ALPHA / ONE) adds zero, the same. And a
    // PARTIAL alpha is above the `> 0` reference, so it survives this test rather
    // than being cut -- the palette expansion's fractional EDGE_CLEAR coverage is
    // not at risk here. The corner has TWO INDEPENDENT TRIGGERS and either one is
    // enough (review-6 F3 -- an earlier draft of this line made them one
    // conjunction, which is the narrower and wrong reading): an UNBLENDED pass
    // moves COLOUR, because a fragment with blend off replaces the target, so the
    // oracle paints palette[0].rgb exactly where this discard writes nothing, and
    // that is true with depth test and write both off; a DEPTH-WRITING pass moves
    // DEPTH, because the oracle records the fragment's depth and a discard records
    // none, so a later depth-tested draw can win or lose differently even when the
    // colour works out. Neither needs the other. Nothing satisfies either today:
    // Overlay is blended and exempt from depth test and write alike
    // (`Renderer.cs:606-607`).
    // The canvas batch (CONTINUATION item 5b) should re-read this when it picks the
    // canvas's pass, and must NOT spend itself scoping the alpha test per pass
    // before that choice is made (review-4 F9 for the claim, review-5 F5 for the
    // correction).
    C3D_AlphaTest(true, GPU_GREATER, mdk::pica::kAlphaTestReference);

    // The frame's default blend state: the pass the queue starts with. Written
    // every eye so the register is never left to citro3d's initialiser, and
    // the cache is told what was written (the same discipline as the cull mode
    // below -- the cache must describe the context, not intend to).
    installBlendState(r, mdk::Pass::Solid, mdk::Blend::Alpha);

    // (B) THE CULL ENUM. The game wants back-face culling, and through the
    // oracle's right-handed camera the meshes wind CCW on screen. The
    // PICA200 has no CW-front mode, so "cull the back faces of CCW-front
    // meshes" is GPU_CULL_BACK_CCW. See the file header: this moved with
    // the camera's handedness, and it needs a hardware look (AGENTS.md P6)
    // before it can be called verified.
    C3D_CullFace(GPU_CULL_BACK_CCW);
    // The cache describes the state actually installed above. The mode
    // is GLOBAL to the context, so the first eye's install already
    // covers the second; re-installing it per eye is belt and braces,
    // not the mechanism.
    r->cullMode = GPU_CULL_BACK_CCW;
    C3D_DepthMap(mdk::pica::kDepthMapIsZBuffer, mdk::pica::kDepthMapScale,
                 mdk::pica::kDepthMapOffset);

    // The viewport is a HARDWARE CLIP RECTANGLE. A zero-area viewport
    // SILENTLY DROPS the draw -- the "invisible 3D trap". The full target
    // is the safe default; the insets set their own.
    C3D_SetViewport(0, 0, kScreenWidth, kScreenHeight);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, kScreenWidth, kScreenHeight);
}

 void mdk_render_begin_frame(MdkRenderer rr, const MdkFrameParams* params) {
    RendererC3D* r = asC3d(rr);
    if (!r || !r->ready) return;
    if (r->inFrame) return;   // a begin without an end is a bug, not a re-entry

    // C3D_FrameBegin returns FALSE when it could not begin (with
    // C3D_FRAME_NONBLOCK). A failed begin does NOT authorise drawing.
    if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) {
        return;
    }
    r->inFrame = true;
    r->frame++;
    r->drawCount = 0;
    r->triangleCount = 0;
    // The one frame that gets censused (see `Census`). Everything else pays the
    // compare and nothing more.
    r->census.active = (r->frame == kCensusFrame);
    r->census.triangles = 0;
    r->census.behind = 0;
    r->census.offscreen = 0;
    r->census.onscreen = 0;
    r->census.depthOut = 0;
    r->census.cw = 0;
    r->census.ccw = 0;
    r->census.near60 = 0;
    r->census.areaSum = 0.0f;
    r->census.loX = r->census.nLoX = 1e30f;
    r->census.hiX = r->census.nHiX = -1e30f;
    r->census.loY = r->census.nLoY = 1e30f;
    r->census.hiY = r->census.nHiY = -1e30f;
    r->census.bestW = -1.0f;
    r->census.bestDraw = -1;
    // C3D_FrameBegin waited for the previous queue above. The old frame's
    // GPU reads are complete, so deferred releases and replacements can be
    // made now (AGENTS.md P3).
    r->gpuPending = false;
    // THE FRAME'S BOUNDARY FOR THE EVICTION. A texture stamped at or after
    // this has been used by THIS frame's commands, and the GPU may still be
    // reading it, so it cannot be freed (AGENTS.md P3).
    r->frameStartUse = r->drawCounter + 1;
    settleDeferredTextures(r);
    settleDeferredMeshes(r);
    r->queue.reset();

    // THE CLEAR COLOUR IS CONVERTED, NOT PASSED THROUGH. The seam carries
    // 0xAARRGGBB (what the Vulkan backend unpacks, and what both frontends
    // therefore write); the PICA's fill pattern is 0xRRGGBBAA. Handing the seam
    // value to C3D_RenderTargetClear directly painted the panel with the alpha
    // byte as red — measured, 0xFF102030 became #ff1020. See pica::clearColour.
    const uint32_t clear =
        mdk::pica::clearColour(params ? params->clear_colour : 0xFF000000u);

    // THE EYE PLAN, read once, here, and stored on the renderer.
    //
    // The eye passes happen at the END of the frame -- see mdk_render_end_frame:
    // the draws are queued between begin and end, so an eye installed here is
    // installed before there is anything to draw into it, and every draw then
    // lands in whichever eye was installed last. Installing both here, as this
    // function used to, meant they all landed in the RIGHT target while the top
    // screen shows the LEFT one: a frame of nothing but the clear colour, on a
    // backend nobody can look at from this host.
    //
    // The slider must still be read here and not at the end, so that the two
    // eyes cannot disagree if the user moves it mid-frame.
    //
    // THE SLIDER MUST BE CLAMPED. Mandarine returns a garbage value (~134
    // million) when it does not model the slider, and Forsaken shipped a bug
    // from exactly that: an unclamped depth became an eye separation of
    // billions of units and the screen went black. The clamp, the sign of each
    // eye and the mono skip are pica::eyePlan, which a host test pins.
    r->eyes = mdk::pica::eyePlan((float)osGet3DSliderState(), r->eyeSeparation);

    // Both eye buffers are cleared here, whether or not both get drawn: a buffer
    // that is not cleared keeps the previous frame's picture, and the right
    // buffer starts being displayed the moment the slider goes up.
    //
    // The depth is cleared to the BUFFER'S MAXIMUM.
    //
    // The frame's depth map stores 0 at the near plane and 1 at the far, so
    // the value that accepts every fragment before one is drawn is the top
    // of the buffer, not the bottom. The devkitPro examples clear to 0 and
    // that is correct for them: citro3d's own default map is the reversed
    // range (see mdk::pica::kDepthMapScale for both pairings). Clearing to
    // 0 with this map rejects everything, which is a screen showing only the
    // clear colour and no error.
    //
    // The value is the raw fill pattern GX_MemoryFill writes into the
    // DEPTH24_STENCIL8 buffer (measured: C3D_FrameBufClear passes
    // `clearDepth` straight to GX_MemoryFill as the fill value), and
    // 0x00FFFFFF is the 24-bit maximum under either byte order of the word.
    C3D_RenderTargetClear(r->targetLeft, C3D_CLEAR_ALL, clear,
                          mdk::pica::kDepthClearValue);
    C3D_RenderTargetClear(r->targetRight, C3D_CLEAR_ALL, clear,
                          mdk::pica::kDepthClearValue);
}

// Report one censused frame. Cold: called once per process, from the end of the
// first eye's replay of frame `kCensusFrame`.
static void logCensus(RendererC3D* r) {
    const Census& c = r->census;
    mdk::log3dsPrintf("census frame=%d draws=%d tris=%d behind=%d offscreen=%d onscreen=%d depthout=%d near60=%d cw=%d ccw=%d w=[%.1f,%.1f] areaset=%.4f",
                      r->frame, (int)r->drawCount, c.triangles, c.behind,
                      c.offscreen, c.onscreen, c.depthOut, c.near60, c.cw, c.ccw, c.minW,
                      c.maxW, c.areaSum);
    if (c.bestDraw >= 0) {
        mdk::log3dsPrintf("census nearest draw=%d first=%d w=%.1f area=%+.8f v0=(%.3f,%.3f,%.3f) v1=(%.3f,%.3f,%.3f) v2=(%.3f,%.3f,%.3f)",
                          c.bestDraw, c.bestFirst, c.bestW, c.bestArea,
                          c.bestV[0], c.bestV[1], c.bestV[2], c.bestV[3],
                          c.bestV[4], c.bestV[5], c.bestV[6], c.bestV[7],
                          c.bestV[8]);
        mdk::log3dsPrintf("census nearest ndc v0=(%.5f,%.5f) v1=(%.5f,%.5f)",
                          c.bestNdc[0], c.bestNdc[1], c.bestNdc[2], c.bestNdc[3]);
    }
    {
        // The ndc bbox is the mapping probe: the panel's painted rectangle has to
        // be this box, or the framebuffer is not showing what the matrix produced.
        //
        // IT PRINTS THE PIXEL BOX NOW, BECAUSE THE TEST WAS NEVER APPLIED (review-1
        // F2). Batch 78 logged the ndc box, said "or the framebuffer is not
        // showing what the matrix produced", and then moved on without applying it.
        // Applied, it FAILS: the box maps outside the target on three sides, so it
        // covers the whole panel, while the panel was 61% clear colour. That is not
        // a contradiction — the box is the UNION of every triangle's bbox with no
        // per-pixel coverage and no depth test in it, so "the union covers the
        // panel" and "the panel is mostly clear" are both true and neither says
        // where the geometry is. The probe is kept because a box that landed wholly
        // INSIDE the panel while the panel stayed clear would be a real finding; a
        // box that overflows it proves only that the union is large.
        //
        // THE SPACE IS STATED, because a number without one is how batch 78 ended
        // up reporting a "407x9 strip" in a 400-pixel-wide panel (review-1 F6).
        // The target is 240x400 PORTRAIT; the capture's 400x240 panel is that
        // transposed, so a pixel box here is not directly a pixel box there.
        const Census& c = r->census;
        const float pxLo = (c.loX + 1.0f) * 0.5f * (float)kScreenWidth;
        const float pxHi = (c.hiX + 1.0f) * 0.5f * (float)kScreenWidth;
        const float pyLo = (1.0f - c.hiY) * 0.5f * (float)kScreenHeight;
        const float pyHi = (1.0f - c.loY) * 0.5f * (float)kScreenHeight;
        mdk::log3dsPrintf("census ndc box=[%.3f,%.3f]x[%.3f,%.3f] near60 box=[%.3f,%.3f]x[%.3f,%.3f]",
                          c.loX, c.hiX, c.loY, c.hiY, c.nLoX, c.nHiX,
                          c.nLoY, c.nHiY);
        mdk::log3dsPrintf("census mapping: target px x=[%.1f,%.1f] y=[%.1f,%.1f] of %dx%d portrait"
                          " (panel capture is this transposed); covers=%s",
                          pxLo, pxHi, pyLo, pyHi, kScreenWidth, kScreenHeight,
                          (pxLo < 0.0f && pxHi > (float)kScreenWidth &&
                           pyLo < 0.0f && pyHi > (float)kScreenHeight)
                              ? "whole target"
                              : "part of the target");
    }
}

 void mdk_render_end_frame(MdkRenderer rr) {
    RendererC3D* r = asC3d(rr);
    if (!r || !r->inFrame) return;
    // THE PER-EYE REPLAY, and it has to happen HERE rather than in
    // mdk_render_begin_frame: the draws are queued between the two, so this is
    // the first moment there is anything to draw, and the last one before the
    // queue is submitted.
    //
    // One pass per eye, each with its own target and matrix installed
    // immediately before its own replay. eyePlan decides how many there are:
    // ONE when the 3D slider is closed, because the panel then shows the left
    // buffer and drawing the right one is invisible work on a 268 MHz ARM11 --
    // Forsaken measures the same cost on the same hardware ("two full render
    // passes ~ 2x CPU+GPU cost ... Slider = 0 disables stereo and restores full
    // framerate", 3DS_PORT_NOTES.md).
    //
    // The queue is reset after the LAST eye, not inside the loop: the second eye
    // replays the same records.
    // The diagnostic is drawn INSIDE the loop, once per eye, so each eye gets
    // its own copy in its own target: drawn once after the loop it would be a
    // triangle in one eye and a hole in the other.
    for (int eye = 0; eye < r->eyes.passes; eye++) {
        installEye(r, eye == 0 ? r->targetLeft : r->targetRight, r->eyes.offset[eye]);
        replayQueue(r);
        // The census is read after the FIRST eye's replay: the second eye draws
        // the same records and would double every count.
        if (r->census.active) {
            logCensus(r);
            r->census.active = false;
        }
        drawDiagnostic(r);
    }
    r->queue.reset();
    // C3D_FrameEnd is the ONLY present path. It triggers the auto display
    // transfer set up by C3D_RenderTargetSetOutput.
    C3D_FrameEnd(0);
    r->inFrame = false;
    // C3D_FrameEnd submits the queue; it does not wait for it. Until the
    // next C3D_FrameBegin, the GPU may still be reading this frame's
    // textures, so they cannot be freed or replaced (AGENTS.md P3).
    r->gpuPending = true;
}

 void mdk_render_present(MdkRenderer rr) {
    // The present IS the frame end on this backend. Kept as a separate seam
    // entry so the Linux backend can differ, but there is nothing to do.
    (void)rr;
}

 void mdk_render_set_camera(MdkRenderer rr, const mdk::Camera* cam) {
    RendererC3D* r = asC3d(rr);
    if (!r || !cam) return;

    // The camera is KEPT, not baked: each eye of the 3DS's stereo pair needs
    // its own offset, and the offset is applied to the view matrix.
    memcpy(r->view, cam->view, sizeof(r->view));
    memcpy(r->projection, cam->projection, sizeof(r->projection));
}

 void mdk_render_set_lighting(MdkRenderer rr, const mdk::Lighting* light) {
    // The 3DS draws the ORIGINAL look. The lighting rig is shared DATA
    // (SPEC.md 5.3) and the 3DS simply does not consume it.
    (void)rr;
    (void)light;
}

 void mdk_render_set_point_lights(MdkRenderer rr, const mdk::PointLight* lights, int32_t count) {
    (void)rr;
    (void)lights;
    (void)count;
}

 void mdk_render_set_panorama(MdkRenderer rr, const mdk::Panorama* p) {
    (void)rr;
    (void)p;
}

 void mdk_render_set_fade(MdkRenderer rr, float level) {
    (void)rr;
    (void)level;
}

 void mdk_render_set_gamma(MdkRenderer rr, float gamma) {
    (void)rr;
    (void)gamma;
}

// ---------------------------------------------------------------------------
// Resources — the P1 scaffold allocates nothing yet. The texture path lands
// with the palette expansion (SPEC.md 7.3).
// ---------------------------------------------------------------------------

 // Free the LEAST RECENTLY USED texture, so that a new one can be made.
//
// THE EVICTION IS WHAT MAKES AN ARENA FIT. The 3DS has six megabytes of
// VRAM, the render targets take one and a half, and an arena's expanded
// textures come to around three more -- so the whole set does not fit at
// once with everything else. The textures that are actually being drawn
// stay; the rest go, and are made again when they are next needed.
//
// ONLY A TEXTURE THAT CANNOT BE IN FLIGHT MAY GO. A texture drawn by this
// frame is read by this frame's commands, and a texture submitted by the
// previous frame is still in flight until the next C3D_FrameBegin waits for
// that queue (AGENTS.md P3). Outside a frame after a frame has been
// submitted, nothing may be evicted.
//
// The evicted texture KEEPS ITS SOURCE, so it can be made again: the
// eviction gives the VRAM back without making the id a dead end.
//
// Returns false when there is no texture that may be freed.
static bool evictTextures(RendererC3D* renderer, size_t needed) {
    (void)needed;
    if (!renderer || renderer->gpuPending) {
        return false;
    }
    // The least recently used texture this frame has not used.
    int oldest = -1;
    uint32_t oldestUse = 0xFFFFFFFFu;
    for (size_t i = 0; i < renderer->textures.size(); i++) {
        const C3dTexture& texture = renderer->textures[i];
        if (!texture.valid || texture.releasePending) {
            continue;
        }
        if (renderer->inFrame && texture.lastUsed >= renderer->frameStartUse) {
            continue;
        }
        if (texture.lastUsed < oldestUse) {
            oldestUse = texture.lastUsed;
            oldest = (int)i;
        }
    }
    if (oldest < 0) {
        return false;
    }
    C3dTexture& victim = renderer->textures[(size_t)oldest];
    C3D_TexDelete(&victim.tex);
    victim.valid = false;
    return true;
}

// A free slot in the texture table, or a new one.
//
// The ids the seam hands out are indices into the table, and a caller that
// releases a texture and makes it again -- the sprite cache does exactly
// that when its residency budget says a frame has to go -- must not grow
// the table for ever. A slot with a source is NOT free: it is an evicted
// texture that can be made again, and its source is the only copy there is.
static int32_t acquireTextureSlot(RendererC3D* renderer) {
    for (size_t i = 0; i < renderer->textures.size(); i++) {
        const C3dTexture& texture = renderer->textures[i];
        if (!texture.valid && !texture.releasePending && texture.source.empty()) {
            return (int32_t)i;
        }
    }
    renderer->textures.push_back(C3dTexture());
    return (int32_t)renderer->textures.size() - 1;
}

// The expanded RGBA8 image of an index texture: the indices through the
// palette, and the padding transparent. The caller frees it.
//
// THE RULE IS THE C#'s PALETTE SHADER'S RULE EXACTLY:
//
//     index == 0   -> TRANSPARENT (the shader discards it)
//     otherwise    -> the palette's RGB, and the alpha is FORCED to 255
//
// The palette's own alpha is NOT used: the C#'s shader takes the palette's
// RGB and writes an alpha of 1.0, and the transparency comes from the
// index-0 discard.
static uint8_t* expandIndices(const uint8_t* indices, int32_t width, int32_t height,
                              const uint8_t* paletteRgba, int32_t paddedWidth,
                              int32_t paddedHeight) {
    const size_t texels = (size_t)paddedWidth * (size_t)paddedHeight;
    uint8_t* pixels = static_cast<uint8_t*>(malloc(texels * 4));
    if (!pixels) {
        return nullptr;
    }
    // The padding is transparent. Each source row is copied into its own
    // padded row: the source is width pixels wide, but the GPU texture is
    // paddedWidth pixels wide, so copying the source as one contiguous run
    // would shift every row after the first.
    memset(pixels, 0, texels * 4);
    for (int32_t y = 0; y < height; y++) {
        const uint8_t* source = indices + (size_t)y * (size_t)width;
        uint8_t* out = pixels + (size_t)y * (size_t)paddedWidth * 4;
        for (int32_t x = 0; x < width; x++) {
            const uint8_t index = source[x];
            uint8_t* pixel = out + (size_t)x * 4;
            if (index == 0) {
                // Transparent: the colour is irrelevant, but it must not be
                // black-with-alpha-255, which is what the palette says.
                continue;
            }
            pixel[0] = paletteRgba[index * 4 + 0];
            pixel[1] = paletteRgba[index * 4 + 1];
            pixel[2] = paletteRgba[index * 4 + 2];
            pixel[3] = 255;
        }
    }
    return pixels;
}

// The expanded RGBA8 image of an RGBA texture: the real pixels, with the
// POT padding transparent. The PICA200's texture-size register requires
// power-of-two dimensions of at least 8, just as it does for an index
// texture. The draw path's UV transform maps the real rectangle into the
// padded one.
static uint8_t* expandRgba(const uint8_t* source, int32_t width, int32_t height,
                           int32_t paddedWidth, int32_t paddedHeight) {
    const size_t texels = (size_t)paddedWidth * (size_t)paddedHeight;
    uint8_t* pixels = static_cast<uint8_t*>(malloc(texels * 4));
    if (!pixels) {
        return nullptr;
    }
    memset(pixels, 0, texels * 4);
    const size_t rowBytes = (size_t)width * 4;
    for (int32_t y = 0; y < height; y++) {
        memcpy(pixels + (size_t)y * (size_t)paddedWidth * 4,
               source + (size_t)y * rowBytes, rowBytes);
    }
    return pixels;
}

// WHAT IS LEFT, when a texture does not fit. The probe is the only
// diagnostic the 3DS gives: an allocation that fails says nothing at all,
// and that silence is how a whole arena's textures once refused to allocate
// one after another while the screen showed only the clear colour.
static void probeTextureMemory(int32_t width, int32_t height, size_t bytes) {
    void* probe = linearAlloc(1024u * 1024u);
    mdk::log3dsPrintf("texture %dx%d (%d bytes) failed; linear 1MB: %s",
                      (int)width, (int)height, (int)bytes, probe ? "ok" : "FULL");
    if (probe) {
        linearFree(probe);
    }
    C3dTexture small;
    if (C3D_TexInit(&small.tex, 64, 64, GPU_RGBA8)) {
        mdk::log3dsPrintf("  64x64 linear: yes");
        C3D_TexDelete(&small.tex);
    } else {
        mdk::log3dsPrintf("  64x64 linear: no");
    }
    C3dTexture smallVram;
    if (C3D_TexInitVRAM(&smallVram.tex, 64, 64, GPU_RGBA8)) {
        mdk::log3dsPrintf("  64x64 vram: yes");
        C3D_TexDelete(&smallVram.tex);
    } else {
        mdk::log3dsPrintf("  64x64 vram: no");
    }
    // THE BOUNDARY. Is the limit a dimension, or a total size?
    static const int probes[][2] = {
        {64, 64}, {64, 128}, {128, 64}, {128, 128},
        {64, 256}, {256, 64}, {128, 176}, {32, 512}, {512, 32},
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        C3dTexture probeTex;
        const bool ok = C3D_TexInit(&probeTex.tex, (u16)probes[i][0], (u16)probes[i][1],
                                    GPU_RGBA8);
        mdk::log3dsPrintf("  %dx%d: %s", probes[i][0], probes[i][1], ok ? "yes" : "NO");
        if (ok) {
            C3D_TexDelete(&probeTex.tex);
        }
    }
    // And the same boundary in RGBA4, which is what the working port uses.
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        C3dTexture probeTex;
        const bool ok = C3D_TexInit(&probeTex.tex, (u16)probes[i][0], (u16)probes[i][1],
                                    GPU_RGBA4);
        mdk::log3dsPrintf("  rgba4 %dx%d: %s", probes[i][0], probes[i][1], ok ? "yes" : "NO");
        if (ok) {
            C3D_TexDelete(&probeTex.tex);
        }
    }
}

// Makes the GPU copy of a texture from the source the backend kept, and
// fills it.
//
// The allocation can fail because the VRAM is full, and the VRAM IS
// routinely full: the render targets take one and a half megabytes, the
// command buffer half a megabyte, and an arena's expanded textures come to
// around three more, against six megabytes.
//
// So the allocation RETRIES, each time evicting the least recently used
// texture. The textures that are being drawn stay; the ones that are not
// are made again when they are next needed.
static bool makeTexture(RendererC3D* renderer, C3dTexture& texture, bool diagnostics) {
    if (texture.valid) {
        return true;
    }
    const size_t gpuBytes = (size_t)texture.paddedWidth * (size_t)texture.paddedHeight * 4;
    const size_t realBytes = (size_t)texture.realWidth * (size_t)texture.realHeight *
                             (texture.indexed ? 1u : 4u);
    if (texture.source.size() != realBytes ||
        (texture.indexed && texture.palette.size() < 256 * 4)) {
        mdk::log3dsPrintf("texture %dx%d source is invalid",
                          (int)texture.realWidth, (int)texture.realHeight);
        return false;
    }
    bool allocated = false;
    bool diagnosticsRun = false;
    for (;;) {
        // C3D_TexInit, NOT C3D_TexInitVRAM.
        //
        // The VRAM-resident variant allocates from the 3DS's separate
        // texture VRAM, and it FAILS on this build for a texture of any
        // size -- even ninety kilobytes, with megabytes free. The
        // linear-heap variant is what the working port (../forsaken) uses,
        // and it works: the PICA200 samples textures out of the linear heap
        // as well as out of the texture VRAM.
        //
        // RGBA8 is read LINEARLY by the PICA200; only the 16-bit and 8-bit
        // formats go through the Morton tiling.
        if (C3D_TexInit(&texture.tex, (u16)texture.paddedWidth, (u16)texture.paddedHeight,
                        GPU_RGBA8)) {
            allocated = true;
            break;
        }
        if (!diagnosticsRun && diagnostics) {
            diagnosticsRun = true;
            probeTextureMemory(texture.realWidth, texture.realHeight, gpuBytes);
        }
        if (!evictTextures(renderer, gpuBytes)) {
            break;
        }
    }
    if (!allocated) {
        mdk::log3dsPrintf("texture %dx%d FAILED: no texture memory",
                          (int)texture.realWidth, (int)texture.realHeight);
        return false;
    }

    // THE PIXELS. A PLAIN CPU BUFFER, NOT linearAlloc.
    //
    // The linear heap STARTS IN THE VRAM: its first megabytes are texture
    // memory, and only then does it extend into system RAM. So a
    // temporary buffer taken from the linear heap PERMANENTLY EATS VRAM --
    // and this one is a temporary: it is copied into the texture and thrown
    // away. Every expansion was therefore costing the 3DS its texture
    // memory, and the arena's textures stopped allocating one after
    // another.
    uint8_t* pixels = nullptr;
    if (texture.indexed) {
        pixels = expandIndices(texture.source.data(), texture.realWidth, texture.realHeight,
                               texture.palette.data(), texture.paddedWidth, texture.paddedHeight);
    } else {
        pixels = expandRgba(texture.source.data(), texture.realWidth, texture.realHeight,
                            texture.paddedWidth, texture.paddedHeight);
    }
    if (!pixels) {
        C3D_TexDelete(&texture.tex);
        return false;
    }
    C3D_TexLoadImage(&texture.tex, pixels, GPU_TEXFACE_2D, 0);
    free(pixels);
    texture.valid = true;
    return true;
}

MdkTextureId mdk_render_create_index_texture(MdkRenderer r, int32_t w, int32_t h, const uint8_t* px,
                                              const uint8_t* paletteRgba) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !renderer->ready || w <= 0 || h <= 0 || w > 1024 || h > 1024 || !px) {
        return -1;
    }
    if (!paletteRgba) {
        // Without a palette there is nothing to expand through. The Vulkan
        // backend binds the palette as a texture of its own and does not
        // need it here, but the 3DS does.
        return -1;
    }

    // THE EXPANSION. The palette lookup happens on the CPU because the
    // PICA200 has no palette texture format and no programmable fragment
    // shader (SPEC.md 7.3).
    //
    // The rule is the C#'s palette shader's rule exactly: index 0 is
    // transparent, and the palette's alpha is NOT used -- the shader takes
    // the palette's RGB and writes an alpha of 1.
    // THE DIMENSIONS GO UP TO A POWER OF TWO, and to at least 8.
    //
    // The PICA200's texture size register encodes log2(width) and
    // log2(height), so a texture's dimensions MUST be powers of two. A
    // 128x176 texture is not one the hardware can hold, and the allocation
    // fails with NO DIAGNOSTIC AT ALL -- which is how this was found: a
    // whole arena's textures silently refused to allocate, one after
    // another, and the screen showed only the clear colour.
    //
    // MDK's textures are 256x400, 128x176, 256x320 and so on, so EVERY one
    // of them has to be padded. The cost is VRAM: 256x400 becomes 256x512,
    // a third more. The price of the hardware's rule.
    C3dTexture texture;
    texture.realWidth = w;
    texture.realHeight = h;
    texture.paddedWidth = nextPowerOfTwo(w);
    texture.paddedHeight = nextPowerOfTwo(h);
    texture.indexed = true;
    // THE SOURCE IS KEPT. An evicted texture is made again from this, and
    // this is the only copy: the caller holds an id, not the pixels.
    texture.source.assign(px, px + (size_t)w * (size_t)h);
    texture.palette.assign(paletteRgba, paletteRgba + 256 * 4);
    texture.lastUsed = ++renderer->drawCounter;

    const int32_t id = acquireTextureSlot(renderer);
    C3dTexture& slot = renderer->textures[(size_t)id];
    slot = std::move(texture);
    if (!makeTexture(renderer, slot, true)) {
        // The failed slot goes back to being free: no GPU copy, no source.
        slot = C3dTexture();
        return -1;
    }
    return id;
}

MdkTextureId mdk_render_create_rgba_texture(MdkRenderer r, int32_t w, int32_t h, const uint8_t* px) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !renderer->ready || w <= 0 || h <= 0 || w > 1024 || h > 1024 || !px) {
        return -1;
    }
    C3dTexture texture;
    texture.realWidth = w;
    texture.realHeight = h;
    // The PICA's texture-size register wants powers of two, and at least 8,
    // for RGBA textures as well as index textures. The real rectangle is
    // mapped into the padded one by the draw path's UV transform.
    texture.paddedWidth = nextPowerOfTwo(w);
    texture.paddedHeight = nextPowerOfTwo(h);
    texture.indexed = false;
    // The source is kept, so an evicted texture can be made again.
    texture.source.assign(px, px + (size_t)w * (size_t)h * 4);
    texture.lastUsed = ++renderer->drawCounter;

    const int32_t id = acquireTextureSlot(renderer);
    C3dTexture& slot = renderer->textures[(size_t)id];
    slot = std::move(texture);
    // The linear heap, not the texture VRAM -- see the index-texture
    // creation for why.
    if (!makeTexture(renderer, slot, true)) {
        slot = C3dTexture();
        return -1;
    }
    return id;
}

int32_t mdk_render_index_texture_bytes(MdkRenderer r, int32_t width, int32_t height) {
    (void)r;
    if (width <= 0 || height <= 0 || width > 1024 || height > 1024) {
        return 0;
    }
    // WHAT THE 3DS SPENDS ON AN INDEX TEXTURE. The indices are expanded
    // through the palette into RGBA8, and the dimensions are padded up to
    // powers of two, so a 95x168 frame costs 128x256x4 = 131072 bytes --
    // eight times the 15960 bytes of the indices themselves.
    return nextPowerOfTwo(width) * nextPowerOfTwo(height) * 4;
}

MdkPaletteId mdk_render_create_palette(MdkRenderer r, const uint8_t* rgba) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !renderer->ready || !rgba) {
        return -1;
    }
    // The 3DS keeps the palette only to expand the textures through it.
    renderer->palettes.push_back(std::vector<uint8_t>(rgba, rgba + 256 * 4));
    return (MdkPaletteId)(renderer->palettes.size() - 1);
}

void mdk_render_update_texture(MdkRenderer r, MdkTextureId t, int32_t w, int32_t h, const uint8_t* px) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || t < 0 || (size_t)t >= renderer->textures.size() || !px) {
        return;
    }
    C3dTexture& texture = renderer->textures[(size_t)t];
    if (texture.source.empty() || texture.releasePending) {
        return;
    }
    if (w != texture.realWidth || h != texture.realHeight) {
        return;  // the contract is same-size updates only
    }
    // A bullet hole's texture changes. The 3DS's texture is EXPANDED, so
    // the update has to go through the palette again -- and the palette the
    // texture was expanded through is kept with the texture, so it can.
    const size_t bytes = (size_t)w * (size_t)h * (texture.indexed ? 1u : 4u);
    texture.source.assign(px, px + bytes);

    // If the old GPU copy may still be in flight, keep it and replace it at
    // the next frame boundary. Otherwise the replacement can be made now.
    const bool inFlight = renderer->gpuPending ||
                          (renderer->inFrame && texture.lastUsed >= renderer->frameStartUse);
    if (!texture.valid) {
        return;  // the next draw makes the new source
    }
    if (inFlight) {
        texture.stale = true;
        return;
    }
    texture.stale = false;
    C3D_TexDelete(&texture.tex);
    texture.valid = false;
    if (!makeTexture(renderer, texture, false)) {
        return;
    }
    texture.lastUsed = ++renderer->drawCounter;
}

void mdk_render_release_texture(MdkRenderer r, MdkTextureId t) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || t < 0 || (size_t)t >= renderer->textures.size()) {
        return;
    }
    C3dTexture& texture = renderer->textures[(size_t)t];
    const bool inFlight = renderer->gpuPending ||
                          (renderer->inFrame && texture.lastUsed >= renderer->frameStartUse);
    if (texture.valid && !inFlight) {
        C3D_TexDelete(&texture.tex);
        texture.valid = false;
    } else if (texture.valid) {
        // The id is invalid to the caller now, but the GPU may still be
        // reading the old copy. The next frame boundary frees it.
        texture.releasePending = true;
    }
    // A RELEASE is final: the source goes with the GPU copy, so the slot is
    // free again once the deferred deletion settles. (An EVICTION is not a
    // release: it keeps the source, and the texture is made again when it is
    // next drawn.)
    texture.source.clear();
    texture.palette.clear();
    texture.stale = false;
}

void mdk_render_release_palette(MdkRenderer r, MdkPaletteId p) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || p < 0 || (size_t)p >= renderer->palettes.size()) {
        return;
    }
    renderer->palettes[(size_t)p].clear();
}

MdkMeshId mdk_render_create_mesh(MdkRenderer r, const mdk::Vertex* v, int32_t n) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !renderer->ready || n <= 0 || !v) {
        return -1;
    }
    C3dMesh mesh;
    mesh.capacity = n;
    // The 3DS's vertex: the position and the texture coordinate, five
    // floats. The packed colour is NOT uploaded: the original look's world
    // surfaces are unlit, and the flat-coloured surfaces are drawn with a
    // one-pixel texture of that colour instead.
    mesh.data = linearAlloc((size_t)n * 5 * sizeof(float));
    if (!mesh.data) {
        return -1;
    }
    mesh.valid = true;
    // THE MESH IS PUSHED BEFORE IT IS FILLED.
    //
    // The update takes the mesh's ID and looks the mesh up by it, so a mesh
    // that is not in the table yet cannot be updated: the update finds
    // nothing, the vertex count stays ZERO, and every draw of that mesh is
    // then skipped -- silently, because a draw with no vertices is a draw
    // that is simply not made.
    //
    // The symptom was a screen that showed only the clear colour while the
    // log said the arena had loaded with 154 batches and 1197 vertices.
    renderer->meshes.push_back(mesh);
    const MdkMeshId id = (MdkMeshId)(renderer->meshes.size() - 1);
    mdk_render_update_mesh(r, id, v, n);
    return id;
}

MdkMeshId mdk_render_create_dynamic_mesh(MdkRenderer r, int32_t cap) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !renderer->ready || cap <= 0) {
        return -1;
    }
    C3dMesh mesh;
    mesh.capacity = cap;
    mesh.data = linearAlloc((size_t)cap * 5 * sizeof(float));
    if (!mesh.data) {
        return -1;
    }
    mesh.valid = true;
    renderer->meshes.push_back(mesh);
    return (MdkMeshId)(renderer->meshes.size() - 1);
}

void mdk_render_update_mesh(MdkRenderer r, MdkMeshId m, const mdk::Vertex* v, int32_t n) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || m < 0 || (size_t)m >= renderer->meshes.size() || !v || n <= 0) {
        return;
    }
    C3dMesh& mesh = renderer->meshes[(size_t)m];
    if (!mesh.valid || n > mesh.capacity) {
        return;
    }
    // SAFE INSIDE THE FRAME IT BELONGS TO, which is the seam's contract
    // (render.h): C3D_FrameBegin waited for the previous queue, so nothing is
    // reading this buffer when the frame's own draws write it. An update outside a
    // frame would be an in-flight write; no caller does one.
    float* out = static_cast<float*>(mesh.data);
    for (int32_t i = 0; i < n; i++) {
        out[i * 5 + 0] = v[i].pos.x;
        out[i * 5 + 1] = v[i].pos.y;
        out[i * 5 + 2] = v[i].pos.z;
        out[i * 5 + 3] = v[i].uv.x;
        out[i * 5 + 4] = v[i].uv.y;
    }
    mesh.count = n;
}

void mdk_render_release_mesh(MdkRenderer r, MdkMeshId m) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || m < 0 || (size_t)m >= renderer->meshes.size()) {
        return;
    }
    C3dMesh& mesh = renderer->meshes[(size_t)m];
    // DEFERRED, LIKE A TEXTURE. The mesh may have been drawn by a frame whose
    // queue has not been waited yet -- C3D_DrawArrays only records the command,
    // and the queue is submitted at C3D_FrameEnd -- so the linear memory is
    // still an in-flight resource. C3D_FrameBegin waits the previous queue, so
    // the next frame boundary is the completion boundary (AGENTS.md P3).
    //
    // The cost is that a mesh that grows (the object stream's, which doubles
    // when a frame needs more) holds its old buffer for one more frame. That is
    // a bounded transient, and it is what P3 asks for.
    mesh.valid = false;
    mesh.count = 0;
    if (mesh.data) {
        mesh.releasePending = true;
    }
}

// Free the meshes whose frame has finished. Called at the frame boundary, after
// C3D_FrameBegin waited for the previous queue (AGENTS.md P3).
static void settleDeferredMeshes(RendererC3D* renderer) {
    if (!renderer) {
        return;
    }
    for (size_t i = 0; i < renderer->meshes.size(); i++) {
        C3dMesh& mesh = renderer->meshes[i];
        if (!mesh.releasePending) {
            continue;
        }
        if (mesh.data) {
            linearFree(mesh.data);
            mesh.data = nullptr;
        }
        mesh.releasePending = false;
        mesh.capacity = 0;
    }
}

MdkScope mdk_render_mark(MdkRenderer r) { (void)r; return MdkScope{}; }
void mdk_render_release(MdkRenderer r, MdkScope s) { (void)r; (void)s; }

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// Record one queued draw. This is the oracle's `DrawOne`: the texture, the UV
// transform for its animation frame, the combiner, the vertex buffer, one
// C3D_DrawArrays -- issued at FLUSH time, in the frame's pass order.
static void drawRecorded(RendererC3D* r, const mdk::MeshDraw* draw) {
    if (draw->mesh < 0 || (size_t)draw->mesh >= r->meshes.size()) {
        return;
    }
    const C3dMesh& mesh = r->meshes[(size_t)draw->mesh];
    if (!mesh.valid || mesh.count <= 0) {
        return;
    }
    if (draw->first_vertex < 0 || draw->vertex_count <= 0 ||
        draw->first_vertex + draw->vertex_count > mesh.count) {
        return;
    }

    // The texture. A surface with no texture is a flat colour, and it is
    // drawn with a ONE-PIXEL texture of that colour: the PICA200's
    // combiner has no constant-colour source for a vertex shader's output,
    // so a colour has to come from a texture.
    C3D_Tex* texture = nullptr;
    if (draw->material.texture >= 0 && (size_t)draw->material.texture < r->textures.size()) {
        C3dTexture& resource = r->textures[(size_t)draw->material.texture];
        // A released texture whose GPU copy is waiting for the next frame
        // boundary must not be drawn again: the caller has given the id up.
        if (!resource.releasePending) {
            if (!resource.valid && !resource.source.empty()) {
                // THE EVICTION IS REVERSIBLE. The texture was made again from
                // the source the backend kept: the caller's id is still good,
                // and the picture comes back with it.
                makeTexture(r, resource, false);
            }
            if (resource.valid) {
                texture = &resource.tex;
                // THE EVICTION ORDER. The texture is stamped every time it is
                // drawn, so the least recently used one is the one that goes
                // when the VRAM is full.
                resource.lastUsed = ++r->drawCounter;
            }
        }
    }
    if (!texture) {
        return;
    }
    C3D_TexBind(0, texture);

    // THE UV TRANSFORM, in uniform register 4.
    //
    //     uv' = uv * scale + offset
    //
    // The scale undoes the padding: the real width over the padded width.
    // The offset picks the frame of an animated texture.
    //
    // The frames are stacked VERTICALLY, and the original's shader picks
    // one with `uv.y = (frac(uv.y) + frame) / frame_count`, so the frame's
    // strip is `frameHeight` rows tall out of the padded height, and the
    // frame's origin is `frame * frameHeight` rows down.
    const C3dTexture& resource = r->textures[(size_t)draw->material.texture];
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    if (resource.paddedWidth > 0 && resource.paddedHeight > 0) {
        // R11: the quotient is cached on the texture. frameCount is a
        // property of the texture itself (material_resolver sets it from
        // texture->frameCount()), so one cached value is exact for every
        // batch that uses this texture.
        C3dTexture& mutableResource = r->textures[(size_t)draw->material.texture];
        if (mutableResource.frameHeight <= 0) {
            const int32_t frameCount = draw->material.frame_count > 0 ? draw->material.frame_count : 1;
            /* @vfp-exempt R11 — quotient cached on the texture: this divide runs at most once per texture lifetime, not per draw; the per-frame path below reads the cached int. measured one __aeabi_idivmod per texture. Verified 2026-10-21. */
            mutableResource.frameHeight = mutableResource.realHeight > 0 ? mutableResource.realHeight / frameCount : 1;
        }
        const int32_t frameHeight = mutableResource.frameHeight;
        scaleX = (float)resource.realWidth / (float)resource.paddedWidth;
        scaleY = (float)frameHeight / (float)resource.paddedHeight;
        offsetY = (float)(draw->frame * frameHeight) / (float)resource.paddedHeight;
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, r->uvLocation, scaleX, scaleY, offsetX, offsetY);

    // The combiner: the texture's colour, with its own alpha (so index 0
    // is transparent), replacing the primary colour.
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

    // The vertex data. The buffer info names the linear memory the GPU
    // reads. Two attributes (position, texture coordinate), so the
    // permutation is 0x10 -- the same shape the devkitPro examples use.
    C3D_BufInfo* bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
    BufInfo_Add(bufInfo, mesh.data, 5 * sizeof(float), 2, 0x10);

    r->drawCount++;
    r->triangleCount += draw->vertex_count / 3;
    if (r->drawCount <= 3) {
        // THE FIRST DRAWS, in full. A draw that never happens and a draw
        // that happens with the wrong data look the same on the screen, and
        // this is the difference between them.
        // THE SAMPLE IS THE DRAW'S OWN FIRST VERTEX, not the mesh's. Printing
        // `mesh.data[0..2]` made every draw of a mesh log the same vertex, and
        // that is what sent batch 77 looking for a fan in a triangle its draw did
        // not even contain.
        //
        // AND IT IS BOUNDED BY THE MESH (review-1 F5). A draw whose first_vertex
        // sits at or past the mesh's own vertex count — an empty tail draw is
        // enough, and a mesh updated with fewer vertices than its capacity is
        // another — would otherwise read 20 bytes past the end of the mesh's
        // linear buffer and print them as a vertex, which is the wrong-vertex
        // attribution this sample was rewritten to stop. A diagnostic must not
        // read outside the resource it is inspecting.
        const int32_t first = draw->first_vertex >= 0 ? draw->first_vertex : 0;
        const bool sampleable = mesh.count > 0 && first + 1 <= mesh.count;
        float v[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        if (sampleable) {
            memcpy(v, static_cast<const float*>(mesh.data) + (size_t)first * 5u, sizeof(v));
        }
        mdk::log3dsPrintf("draw %d: mesh=%d first=%d count=%d tex=%d pass=%d v0=(%.1f,%.1f,%.1f) uv=(%.3f,%.3f)%s",
                          (int)r->drawCount, (int)draw->mesh, (int)draw->first_vertex,
                          (int)draw->vertex_count, (int)draw->material.texture,
                          (int)draw->material.pass,
                          v[0], v[1], v[2], v[3], v[4],
                          sampleable ? "" : " [first is past the mesh's end]");
    }
    // THE DIAGNOSTIC TRIANGLE IS DRAWN LAST, AFTER THE REAL DRAW -- see below.

    // The draw.
    //
    // THE PICA200 HAS NO LINE PRIMITIVE. Its only primitives are triangles,
    // a triangle strip and a triangle fan, so the original's outline edges
    // (the game's "glass frames") cannot be drawn as lines here. They would
    // have to be thin quads, which is a rendering decision and not a port
    // of the original's draw call. For v1 the 3DS draws the surfaces and
    // not the outlines; the picture differs from the original in that one
    // respect, and it is recorded rather than hidden.
    const int32_t first = draw->first_vertex;
    const int32_t count = draw->vertex_count;

    // THE PER-DRAW MATRIX.
    //
    // `MeshDraw::world` is the object's transform, and the seam's contract is
    // that the backend places the draw with it: the Vulkan backend multiplies it
    // into the same chain in its own drawRecorded. Ignoring it here meant every
    // queued draw landed at the arena's own coordinates, so the 3DS could not
    // place an object however the script moved it (docs/OPEN_ITEMS.md, the
    // `MeshDraw::world` item).
    //
    // The composition is ONE 4x4 multiply, and only for the draws that need it.
    // The arena batches and Kurt's sprite both carry an identity world, and for
    // those the frame's matrix is already installed, so a frame of level
    // geometry costs no multiply and no uniform write; an object draw pays one
    // multiply and one write. That is the same caching the cull mode below uses,
    // for the same reason -- a per-draw C3D_* call is real ARM11 time and this
    // one dirties the uniform buffer.
    //
    // The two PICA conversions are already inside r->eyeVp and they are
    // right-multiplications by fixed matrices, so composing the world against
    // the baked matrix is the same product as rebuilding it (see pica_matrix.h).
    const bool identityWorld = mdk::pica::isIdentity(draw->world);
    if (!identityWorld || !r->matrixIsFrame) {
        float m[16];
        if (identityWorld) {
            // Restoring the frame's matrix after a draw that installed its own.
            memcpy(m, r->eyeVp, sizeof(m));
        } else {
            mdk::pica::drawMatrix(draw->world, r->eyeVp, m);
        }
        C3D_Mtx gpuMtx;
        mdk::pica::registers(m, gpuMtx.m);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->mvpLocation, &gpuMtx);
        r->matrixIsFrame = identityWorld;
    }

    // THE PASS'S CULLING, PER DRAW.
    //
    // The Vulkan backend culls only Pass::Solid (renderer_vk.cpp's pipeline
    // builder) and leaves every other pass -- DoubleSided, Mirror, Blended,
    // OnTop and Overlay -- uncullable. The oracle additionally requires a
    // mesh and a non-shadow output for back-face culling; this seam only
    // receives mesh surfaces, so those conjuncts cannot select another path
    // here. This backend has one pipeline, so the state is selected for the
    // draw that asks for it.
    //
    // Pass::DoubleSided is LOAD-BEARING for Kurt's sprite: the quad mirrors
    // `at.right` for a left-facing view, and that mirror reverses the
    // winding. With the frame's BACK_CCW default the mirrored sprite would
    // be a back face and vanish. The oracle's material says DoubleSided for
    // exactly this reason; do not "fix" it by emitting both windings, which
    // would double the sprite's vertex count.
    //
    // C3D_CullFace does not compare the requested mode with the current one:
    // it always dirties the effect block, which is flushed at the next draw.
    // Cache the installed mode and change it only on a pass transition. An
    // all-Solid frame then performs no per-draw cull calls at all.
    const bool solid = draw->material.pass == mdk::Pass::Solid;
    const GPU_CULLMODE wantedCull = solid ? GPU_CULL_BACK_CCW : GPU_CULL_NONE;
    if (r->cullMode != wantedCull) {
        C3D_CullFace(wantedCull);
        r->cullMode = wantedCull;
    }

    // THE PASS'S DEPTH STATE, PER DRAW.
    //
    // The oracle derives it from the pass, per pipeline (Renderer.cs:606-607);
    // this backend has one program and one pipeline, so the state is selected
    // for the draw that asks for it — the same shape as the cull mode just
    // above, and the same reason: C3D_DepthTest dirties the effect block on
    // every call, so it is written only on a pass transition and an all-Solid
    // frame writes it once, at the frame's install.
    //
    // What changes in practice: `Pass::Blended` glass is tested against the
    // scene but does not write depth, so it can be seen through, and the
    // `OnTop`/`Overlay` exemptions exist. The rule itself is
    // `pica::depthStateForPass`, which the host suite holds to the oracle.
    const mdk::pica::DepthState wantedDepth =
        mdk::pica::depthStateForPass(draw->material.pass);
    if (r->depthTest != wantedDepth.test || r->depthWrite != wantedDepth.write) {
        installDepthState(r, wantedDepth);
    }

    // THE PASS'S BLEND STATE, PER DRAW -- same shape and same reason as the two
    // above: `C3D_AlphaBlend` dirties the effect block on every call, so it is
    // written on a pass transition and on nothing else. An all-Solid frame
    // writes it once, at the eye's install, and the glass that follows costs one
    // transition in and one back.
    //
    // `material.blend` is read here and not folded into the pass, because the
    // oracle keeps them independent: the pass decides WHETHER the draw mixes and
    // the blend decides HOW (Renderer.cs:565,572). A Blended pass with
    // Blend::Add is a different register pair from a Blended pass with Alpha,
    // and the queue's pass order cannot tell them apart.
    const int wantedBlendPass = static_cast<int>(draw->material.pass);
    const int wantedBlendMode = static_cast<int>(draw->material.blend);
    if (r->blendPass != wantedBlendPass || r->blendMode != wantedBlendMode) {
        installBlendState(r, draw->material.pass, draw->material.blend);
    }

    // THE ONE-FRAME GEOMETRY CENSUS (see the `Census` struct). Cold by
    // construction: `census.active` is true for exactly one frame of the
    // process, so every other frame pays one integer compare and this loop runs
    // once in a run.
    //
    // It measures, in the SAME matrix the GPU is about to receive, where each of
    // this draw's triangles lands and which way it winds. Batch 77's single
    // triangle could not separate "the room is not queued" from "the room is
    // queued and the GPU discards it", and could not say what the room's winding
    // is, which is what the cull enum turns on.
    if (r->census.active && count >= 3) {
        float cm[16];
        if (identityWorld) {
            memcpy(cm, r->eyeVp, sizeof(cm));
        } else {
            mdk::pica::drawMatrix(draw->world, r->eyeVp, cm);
        }
        const float* cv = static_cast<const float*>(mesh.data);
        const int32_t tris = count / 3;
        for (int32_t t = 0; t < tris; t++) {
            float nx[3];
            float ny[3];
            float nz[3];
            float w[3];
            bool behind = false;
            for (int32_t v = 0; v < 3; v++) {
                const int32_t at = (first + t * 3 + v) * 5;
                const float x = cv[at];
                const float y = cv[at + 1];
                const float z = cv[at + 2];
                const float cx = x * cm[0] + y * cm[4] + z * cm[8] + cm[12];
                const float cy = x * cm[1] + y * cm[5] + z * cm[9] + cm[13];
                const float cz = x * cm[2] + y * cm[6] + z * cm[10] + cm[14];
                const float cw = x * cm[3] + y * cm[7] + z * cm[11] + cm[15];
                if (cw <= 0.0f) {
                    behind = true;
                }
                w[v] = cw;
                const float inv = cw != 0.0f ? 1.0f / cw : 0.0f;
                nx[v] = cx * inv;
                ny[v] = cy * inv;
                nz[v] = cz * inv;
            }
            Census& c = r->census;
            c.triangles++;
            float wmin = w[0];
            float wmax = w[0];
            float wsum = 0.0f;
            for (int32_t v = 0; v < 3; v++) {
                if (w[v] < wmin) {
                    wmin = w[v];
                }
                if (w[v] > wmax) {
                    wmax = w[v];
                }
                wsum += w[v];
            }
            if (c.triangles == 1 || wmin < c.minW) {
                c.minW = wmin;
            }
            if (c.triangles == 1 || wmax > c.maxW) {
                c.maxW = wmax;
            }
            if (behind) {
                c.behind++;
                continue;
            }
            float lox = nx[0];
            float hix = nx[0];
            float loy = ny[0];
            float hiy = ny[0];
            for (int32_t v = 1; v < 3; v++) {
                if (nx[v] < lox) {
                    lox = nx[v];
                }
                if (nx[v] > hix) {
                    hix = nx[v];
                }
                if (ny[v] < loy) {
                    loy = ny[v];
                }
                if (ny[v] > hiy) {
                    hiy = ny[v];
                }
            }
            if (hix < -1.0f || lox > 1.0f || hiy < -1.0f || loy > 1.0f) {
                c.offscreen++;
                continue;
            }
            // The depth half, counted separately rather than folded into onscreen:
            // a triangle inside the clip SQUARE in x/y but entirely past the far
            // plane (or in front of the near one) is not something the GPU will
            // show, and the census's on-screen count was read that way (review-1
            // F5). The PICA's depth map decides which side of [-1,1] is "in", so
            // this is reported as a range and not as a verdict.
            float loz = nz[0];
            float hiz = nz[0];
            for (int32_t v = 1; v < 3; v++) {
                if (nz[v] < loz) {
                    loz = nz[v];
                }
                if (nz[v] > hiz) {
                    hiz = nz[v];
                }
            }
            if (hiz < -1.0f || loz > 1.0f) {
                c.depthOut++;
            }
            const float area = (nx[1] - nx[0]) * (ny[2] - ny[0]) -
                               (ny[1] - ny[0]) * (nx[2] - nx[0]);
            c.onscreen++;
            if (area < 0.0f) {
                c.cw++;
            } else {
                c.ccw++;
            }
            c.areaSum += area < 0.0f ? -area : area;
            if (lox < c.loX) { c.loX = lox; }
            if (hix > c.hiX) { c.hiX = hix; }
            if (loy < c.loY) { c.loY = loy; }
            if (hiy > c.hiY) { c.hiY = hiy; }
            const float wc = wsum / 3.0f;
            if (wc < 60.0f) {
                c.near60++;
                if (lox < c.nLoX) { c.nLoX = lox; }
                if (hix > c.nHiX) { c.nHiX = hix; }
                if (loy < c.nLoY) { c.nLoY = loy; }
                if (hiy > c.nHiY) { c.nHiY = hiy; }
            }
            if (c.bestW < 0.0f || wc < c.bestW) {
                c.bestW = wc;
                c.bestDraw = (int32_t)r->drawCount;
                c.bestFirst = first + t * 3;
                c.bestArea = area;
                for (int32_t v = 0; v < 3; v++) {
                    const int32_t at = (c.bestFirst + v) * 5;
                    c.bestV[v * 3 + 0] = cv[at];
                    c.bestV[v * 3 + 1] = cv[at + 1];
                    c.bestV[v * 3 + 2] = cv[at + 2];
                }
                c.bestNdc[0] = nx[0];
                c.bestNdc[1] = ny[0];
                c.bestNdc[2] = nx[1];
                c.bestNdc[3] = ny[1];
            }
        }
    }

    C3D_DrawArrays(GPU_TRIANGLES, first, count);
    // Restore the frame's default for the next draw, so every draw starts
    // from the state begin_frame installed. The transition back is cached in
    // the same way; repeated Solid draws do not dirty the effect block.
    if (r->cullMode != GPU_CULL_BACK_CCW) {
        C3D_CullFace(GPU_CULL_BACK_CCW);
        r->cullMode = GPU_CULL_BACK_CCW;
    }

    // THE DIAGNOSTIC TRIANGLE IS NOT DRAWN HERE. It used to be, immediately
    // after the real draw, and that cost the frame: the triangle needs the depth
    // test OFF, so it wrote the state behind the per-draw cache's back and the
    // NEXT draw had to install its pass's state again -- two effect-block-
    // dirtying C3D_DepthTest calls per draw and one extra draw per real draw,
    // all of it multiplied again by the eye count (review-1 F3). It now runs
    // once per eye from mdk_render_end_frame; see drawDiagnostic.
}

// THE DIAGNOSTIC TRIANGLE, once per eye, after that eye's replay. Clip-space,
// with the identity matrix, no texture and no depth test: if THIS does not
// appear on a screen, the problem is in the frame or the program, not in the
// mesh, the transform or the texture.
//
// WHY IT MOVED OUT OF THE PER-DRAW PATH (review-1 F3). Per-draw it cost two
// C3D_DepthTest calls and one extra draw for every real draw, and the per-eye
// replay multiplies that by the number of eyes: on the pinned scene's frame 1
// (154 Solid + 1 DoubleSided, docs/OPEN_ITEMS.md) that was ~310 depth writes
// and 155 diagnostic draws per eye, where one eye now pays 2 and 1. Moving it
// also makes it a better instrument, not merely a cheaper one:
//
//   * It now runs on a frame that queued NOTHING. Inside the per-draw path it
//     could not -- no draw, no diagnostic -- and an empty frame is exactly where
//     "is the frame and the program alive at all" is the only question left.
//   * Nothing real is drawn after it inside an eye, so the state it leaves
//     behind (the context's single BufInfo and TexEnv, the identity uniform, the
//     test off) cannot damage a draw of this frame. That was not theory: the
//     vault's hardware-CONFIRMED pattern c3d-interleaved-draw-leaves-vertex-
//     uniforms-stale.md is this hazard class -- an interleaved draw that switches
//     vertex streams leaves the VERTEX UNIFORMS stale, and the victim is a
//     later, unrelated draw that lands off-screen while every state probe reads
//     correct. The next installEye cleans up after the diagnostic, and
//     installEye writes every uniform and every piece of fixed state anyway
//     (AGENTS.md P2).
//
// IT STILL MUST COME AFTER THE REAL DRAWS, NOT BEFORE THEM. citro3d hands out
// the CONTEXT's single BufInfo and TexEnv rather than a copy -- C3D_GetBufInfo()
// is called here and never handed back with C3D_SetBufInfo, so the two calls
// return the SAME struct. Drawn first, the diagnostic replaced the buffer
// description the real draw had just installed and the real draw read the
// triangle's 15 floats at the mesh's offsets -- invisible geometry, with the
// diagnostic itself on screen to say the frame was fine (batch 28's defect).
static void drawDiagnostic(RendererC3D* r) {
    if (!r->diagnosticTriangle || !r->scratch) {
        return;
    }
    // THE VERTICES MUST BE IN LINEAR MEMORY, AND THEY WERE NOT. They used to be
    // a `static float triangle[15]`: a .bss address in the application heap.
    // citro3d hands the BufInfo address to the GPU, and only the linear heap is
    // the GPU's address space; nothing flushes the CPU cache for a static array
    // either. The renderer's linear scratch is the right home: already
    // `linearAlloc`'d, already GPU-addressable, and nothing else writes it.
    //
    // IT STILL DOES NOT APPEAR, AND THAT IS MEASURED, NOT ASSUMED. Batch 78
    // replaced these vertices with a triangle covering the whole clip square,
    // ran it with the cull mode set to GPU_CULL_NONE and the depth test off, and
    // not one pixel of the panel changed: the panel is 50% white when the SCENE
    // is drawn with the primary colour and 50% clear colour, and the extra
    // 50% this triangle would have painted is still the clear colour. So the
    // linear-memory fix is correct on its own terms -- a .bss pointer is not a
    // GPU address -- but it is not the whole reason, and the remaining cause is
    // open. What has been measured and ruled out for it: the cull mode (NONE
    // changes the frame by 69 px of 96000), the depth test (off changes it by
    // 201 px), and the winding (the culled set is the one that looks
    // counter-clockwise on the panel, and the triangle was tried both ways).
    // The 13/58 this comment used to carry were batch 78's first reading and are
    // superseded by the re-measurement recorded in PROGRESS_LOG Iteration 108
    // (review-4 F7); the probes are MORE informative than the old numbers said.
    float* triangle = static_cast<float*>(r->scratch);
    triangle[0] = -0.8f; triangle[1] = -0.8f; triangle[2] = 0.0f; triangle[3] = 0.0f; triangle[4] = 0.0f;
    triangle[5] = 0.8f;  triangle[6] = -0.8f; triangle[7] = 0.0f; triangle[8] = 0.0f; triangle[9] = 0.0f;
    triangle[10] = 0.0f; triangle[11] = 0.8f;  triangle[12] = 0.0f; triangle[13] = 0.0f; triangle[14] = 0.0f;
    C3D_Mtx identity;
    for (int i = 0; i < 16; i++) {
        identity.m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->mvpLocation, &identity);
    C3D_BufInfo* info = C3D_GetBufInfo();
    BufInfo_Init(info);
    BufInfo_Add(info, triangle, 5 * sizeof(float), 2, 0x10);
    C3D_TexEnv* plain = C3D_GetTexEnv(0);
    C3D_TexEnvInit(plain);
    C3D_TexEnvSrc(plain, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(plain, C3D_Both, GPU_REPLACE);
    // THE DIAGNOSTIC IS NOT DEPTH-TESTED, and that is load-bearing now that
    // the scene is. Its three vertices sit at clip z = 0 through an identity
    // matrix, which is the FAR plane — exactly what the buffer was cleared
    // to — so with the test installed it would fail wherever any real
    // geometry is nearer, and the one draw whose whole job is to say "the
    // frame and the program are fine" would go missing in exactly the busy
    // scenes it is needed for. The cache is told, because this writes the
    // state behind the per-draw check's back.
    installDepthState(r, mdk::pica::DepthState{false, false});
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    // The uniform now holds the identity, not the frame's matrix. The next
    // installEye -- the other eye, or this one's next frame -- is what puts it
    // back, and it writes the uniform unconditionally.
    r->matrixIsFrame = false;
}

// One replay of everything the frame has queued, in the oracle's pass order
// (draw_queue.h), into whatever target is current. It is separate from the
// frame end because a stereo frame replays the SAME queue once per eye: the
// queue already IS the recorded draw list, so the second eye is a replay with a
// different matrix and target, not a second walk of the scene.
//
// THE ONLY REPLAY POINT IS mdk_render_end_frame, once per eye. A mid-frame
// replay is not a harmless early flush: it emits the frame's records an extra
// time into whatever target installEye last selected, and a flushDraws-shaped
// one (this replay plus a reset) consumes the records as well, so the eye loop
// that follows would replay an empty queue. That is why the canvas and inset
// seams below do not flush (review-1 F4) -- and, since review-3 F1, why none of
// them may draw by itself either: an immediate C3D_DrawArrays in a seam is
// outside the per-eye replay, so it lands in one target or in none.
static void replayQueue(RendererC3D* r) {
    if (!r || r->queue.empty() || !r->inFrame) {
        return;
    }
    for (int32_t pass = 0; pass < mdk::kDrawQueuePassCount; pass++) {
        const std::vector<mdk::MeshDraw>& bucket = r->queue.bucket(pass);
        for (size_t i = 0; i < bucket.size(); i++) {
            drawRecorded(r, &bucket[i]);
        }
    }
}

 void mdk_render_draw_mesh(MdkRenderer rr, const mdk::MeshDraw* draw) {
    RendererC3D* r = asC3d(rr);
    if (!r || !draw || !r->inFrame) {
        return;
    }
    // Queued, not recorded: the oracle renders the frame's commands one pass at
    // a time, and render.h gives that ordering job to the backend. With the
    // depth test off on this target, the order IS the occlusion, so this is not
    // a tidiness change.
    r->queue.add(*draw);
}

// ---------------------------------------------------------------------------
// The 2D canvas
// ---------------------------------------------------------------------------

 void mdk_render_canvas_begin(MdkRenderer r) {
    // NO FLUSH HERE, and that is a contract rather than an omission (review-1
    // F4). With the per-eye replay at the frame end, a mid-frame flush emits
    // THIS frame's queued records into whatever target installEye last
    // selected -- the previous frame's last eye, or no target at all on frame 1
    // -- and a flushDraws-shaped one empties the queue too, so the eye loop
    // replays nothing: a scene that vanished behind a correct clear colour.
    //
    // Nothing calls this on the 3DS today (canvas_begin outside the backends is
    // an empty grep) and mdk_render_canvas_quad is a no-op on this backend, so
    // the flush was never load-bearing -- it was a latent black screen. When the
    // HUD lands it must be a RECORD IN THE QUEUE, drawn inside the eye loop once
    // per eye; docs/OPEN_ITEMS.md carries the requirement, and the gate that
    // refuses a draw here covers canvas_quad, the seam hud_view.cpp actually
    // calls (review-3 F1).
    (void)r;
}
 void mdk_render_canvas_quad(MdkRenderer r, const mdk::CanvasQuad* q) { (void)r; (void)q; }
 void mdk_render_canvas_end(MdkRenderer r) { (void)r; }
 float mdk_render_canvas_width(MdkRenderer r) {
    (void)r;
    // The canvas is 360 units high and its width follows the view's aspect,
    // like C# Renderer.CanvasWidth. The ratio looks INVERTED and is not:
    // kScreenWidth/kScreenHeight name the ROTATED FRAMEBUFFER axes (240x400
    // portrait, renderer_c3d.cpp:62-63), not the landscape 400x240 the game
    // draws into, so the landscape aspect is height/width = 400/240 and the
    // canvas is 600 units wide. Renaming those constants to the landscape
    // orientation would silently turn this into 216.
    return mdk::kCanvasHeight * (float)kScreenHeight / (float)kScreenWidth;
}
 int32_t mdk_render_canvas_capacity(MdkRenderer r) {
    (void)r;
    return 512;
}

// ---------------------------------------------------------------------------
// Insets
// ---------------------------------------------------------------------------

 void mdk_render_begin_inset(MdkRenderer r, const mdk::Inset* i) {
    // The same contract as mdk_render_canvas_begin: an inset is a viewport
    // change INSIDE one eye's replay, not a flush point, so nothing is emitted
    // here. Wiring the sniper insets means recording the viewport with the
    // draw, not flushing the frame at its boundary.
    (void)r;
    (void)i;
}
 void mdk_render_end_inset(MdkRenderer r) { (void)r; }

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

 void mdk_render_stats(MdkRenderer r, int32_t* draw_calls, int32_t* triangles) {
    (void)r;
    if (draw_calls) *draw_calls = 0;
    if (triangles) *triangles = 0;
}

 float mdk_render_gpu_wait_ms(MdkRenderer r) { (void)r; return 0.0f; }
