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
//       Once. Not twice.
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

    int frame = 0;
    int32_t drawCount = 0;
    int32_t triangleCount = 0;
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
    const Result spCfg = shaderProgramConfigure(&r->program, true, false);
    if (R_FAILED(spCfg)) {
        mdk::log3dsPrintf("shaderProgramConfigure FAILED result=0x%08lx", (unsigned long)spCfg);
        shaderProgramFree(&r->program);
        DVLB_Free(r->dvlb);
        C3D_RenderTargetDelete(r->targetLeft);
        C3D_RenderTargetDelete(r->targetRight);
        C3D_Fini();
        delete r;
        return nullptr;
    }

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
// The MVP for one eye.
//
// THE MATRIX CONVENTIONS, which are the whole difficulty here:
//
//   The engine's matrices are ROW-MAJOR with a ROW-VECTOR convention: the
//   point is a row and the transform is `p_clip = p_world * M`.
//
//   The PICA200's vertex shader is COLUMN-VECTOR: the transform is
//   `p_clip = M_pica * p_world`, and uniform register i holds COLUMN i of
//   M_pica.
//
//   For the two to describe the SAME transform we need M_pica = M^T, and
//   the column-major register layout means that M^T's column i is M's ROW i.
//   So register i gets our row i, verbatim, and the transpose is FREE --
//   exactly the "implicit transpose" the D3D->GL conversion already relies
//   on. (Uploading our memory verbatim would give M^T's ROW i, which is M's
//   column i, i.e. the transpose of what is wanted: a rotation would come
//   out INVERTED. The two are one transpose apart, and only one of them is
//   right.)
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
// THE 90-DEGREE ROTATION is applied here too, and it is a PROPER ROTATION
// (determinant +1): the new row 0 is the old row 1 and the new row 1 is the
// NEGATED old row 0. The upstream picaGL version used a reflection
// (determinant -1), which flips the winding a second time and undoes the
// cull inversion.
//
// THE EYE OFFSET moves the camera along its own right axis by `offset`.
// For a row-vector view matrix V = [R | T], the camera's world position is
// C = -R^T T, and moving C by d along the camera's right axis r = row 0 of R
// gives T' = T - d * (R^T r), and R^T r is the first COLUMN of R. So the
// translation's x, y and z each lose d times the first column's x, y, z.
// ---------------------------------------------------------------------------
namespace {

void buildEyeMvp(const RendererC3D* r, float eyeOffset, float out[16]) {
    // The view matrix, with the eye's offset applied.
    //
    // The view is a row-major, row-vector look-at matrix: its translation row
    // is (-dot(right, eye), -dot(trueUp, eye), -dot(forward, eye)), and its
    // COLUMNS are the camera axes. Moving the eye by d along its own right
    // axis therefore changes ONLY the first translation component, by -d:
    // the other two axes are perpendicular to `right`, so their dot products
    // are unchanged. This is the C# oracle's View.Eye (Renderer.cs:92), which
    // rebuilds CreateLookAt at the shifted position; patching the row is the
    // algebraically identical shortcut. A former version scaled all three
    // translation components by the right axis, which moved the eyes along a
    // skewed direction for any camera that was not axis-aligned.
    float view[16];
    memcpy(view, r->view, sizeof(view));
    if (eyeOffset != 0.0f) {
        view[12] -= eyeOffset;
    }

    // The combined matrix: view * projection, in the row-vector convention.
    // A row vector p transforms as p' = p * V * P, so the combined matrix is
    // V * P with view as the LEFT operand -- the C# oracle's `view *
    // projection` (Renderer.cs:115, FollowCamera.cs, FallView.cs). The
    // product below is the ordinary row-times-column product of that.
    float m[16];
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += view[row * 4 + k] * r->projection[k * 4 + col];
            }
            m[row * 4 + col] = sum;
        }
    }

    // (A) The depth remap: D3D [0,1] -> PICA [-1,0].
    //
    // In REGISTER space this is Forsaken's `c3d_vp.r[2] -= c3d_vp.r[3]`
    // (render_c3d.c:1122), i.e. clip.z -= clip.w. Register i holds COLUMN i
    // of the engine's row-vector matrix (see the upload below), so the
    // register-row operation is a COLUMN operation here: column 2 -= column 3.
    // A row operation would perturb clip.x/.y/.w and leave z/w untouched.
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

} // namespace

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
    // C3D_FrameBegin waited for the previous queue above. The old frame's
    // GPU reads are complete, so deferred releases and replacements can be
    // made now (AGENTS.md P3).
    r->gpuPending = false;
    // THE FRAME'S BOUNDARY FOR THE EVICTION. A texture stamped at or after
    // this has been used by THIS frame's commands, and the GPU may still be
    // reading it, so it cannot be freed (AGENTS.md P3).
    r->frameStartUse = r->drawCounter + 1;
    settleDeferredTextures(r);

    const uint32_t clear = params ? params->clear_colour : 0xFF000000u;

    // The 3DS's top screen is an AUTOSTEREOSCOPIC PARALLAX BARRIER: it has
    // two eye buffers and the hardware shows them from slightly different
    // angles. The slider's position says how deep the barrier is.
    //
    // THE SLIDER MUST BE CLAMPED. Mandarine returns a garbage value (~134
    // million) when it does not model the slider, and Forsaken shipped a bug
    // from exactly that: an unclamped depth became an eye separation of
    // billions of units and the screen went black.
    float slider = (float)osGet3DSliderState();
    if (slider < 0.0f) slider = 0.0f;
    if (slider > 1.0f) slider = 1.0f;
    const float eyeOffset = slider * r->eyeSeparation * 0.5f;

    // Each eye is a full pass into its own target.
    for (int eye = 0; eye < 2; eye++) {
        C3D_RenderTarget* target = (eye == 0) ? r->targetLeft : r->targetRight;
        const float offset = (eye == 0) ? -eyeOffset : eyeOffset;

        // The depth is cleared to 0, like every devkitPro example.
        C3D_RenderTargetClear(target, C3D_CLEAR_ALL, clear, 0);
        C3D_FrameDrawOn(target);

        // The MVP for this eye.
        float mvp[16];
        buildEyeMvp(r, offset, mvp);
        // THE STRIDED READ. The shader consumes register i as the vector
        // dotted with the position (dp4 outpos.x, mvp[0], r0), so register i
        // is ROW i of the column-vector matrix M_pica, and M_pica = M^T for
        // our row-vector M. Therefore register i must hold COLUMN i of our
        // row-major matrix -- the strided read below, exactly Forsaken's
        // matrix_to_c3d (render_c3d.c:1077). A verbatim row copy uploads the
        // transpose: rotations come out inverted and the picture is mirrored.
        C3D_Mtx gpuMtx;
        for (int i = 0; i < 4; i++) {
            gpuMtx.r[i].x = mvp[0 + i];
            gpuMtx.r[i].y = mvp[4 + i];
            gpuMtx.r[i].z = mvp[8 + i];
            gpuMtx.r[i].w = mvp[12 + i];
        }
        // ALL UNIFORM REGISTERS MUST BE WRITTEN EVERY FRAME. An unwritten
        // uniform holds the previous frame's value or garbage. The shader
        // reads registers 0-3 (the mvp), so exactly those are written.
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->mvpLocation, &gpuMtx);

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

        // THE DEPTH TEST IS OFF.
        //
        // The ORIGINAL look has no depth buffer: the game draws its
        // surfaces back to front, and the C#'s original-look path does the
        // same. The 3DS's depth convention is also the opposite of the
        // desktop's, so a desktop-style test with a desktop-style clear
        // rejects EVERY fragment -- and the screen shows nothing, with no
        // error at all.
        //
        // The depth BUFFER is still cleared, to 0, because that is what the
        // devkitPro examples do and the hardware expects.
        C3D_DepthTest(false, GPU_ALWAYS, (GPU_WRITEMASK)0);
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
        C3D_DepthMap(true, 1.0f, 1.0f);

        // The viewport is a HARDWARE CLIP RECTANGLE. A zero-area viewport
        // SILENTLY DROPS the draw -- the "invisible 3D trap". The full target
        // is the safe default; the insets set their own.
        C3D_SetViewport(0, 0, kScreenWidth, kScreenHeight);
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, kScreenWidth, kScreenHeight);
    }
}

 void mdk_render_end_frame(MdkRenderer rr) {
    RendererC3D* r = asC3d(rr);
    if (!r || !r->inFrame) return;
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
    if (mesh.data) {
        linearFree(mesh.data);
        mesh.data = nullptr;
    }
    mesh.valid = false;
}

MdkScope mdk_render_mark(MdkRenderer r) { (void)r; return MdkScope{}; }
void mdk_render_release(MdkRenderer r, MdkScope s) { (void)r; (void)s; }

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

 void mdk_render_draw_mesh(MdkRenderer rr, const mdk::MeshDraw* draw) {
    RendererC3D* r = asC3d(rr);
    if (!r || !draw || !r->inFrame) {
        return;
    }
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
        const float* vertex = static_cast<const float*>(mesh.data);
        mdk::log3dsPrintf("draw %d: mesh=%d first=%d count=%d tex=%d  v0=(%.1f,%.1f,%.1f) uv=(%.3f,%.3f)",
                          (int)r->drawCount, (int)draw->mesh, (int)draw->first_vertex,
                          (int)draw->vertex_count, (int)draw->material.texture,
                          vertex[0], vertex[1], vertex[2], vertex[3], vertex[4]);
    }
    // THE DIAGNOSTIC TRIANGLE. Clip-space, with the identity matrix, and no
    // texture: if THIS does not appear, the problem is in the frame or the
    // program, not in the mesh, the transform or the texture.
    if (r->diagnosticTriangle) {
        static float triangle[15] = {
            -0.8f, -0.8f, 0.0f, 0.0f, 0.0f,
             0.8f, -0.8f, 0.0f, 0.0f, 0.0f,
             0.0f,  0.8f, 0.0f, 0.0f, 0.0f,
        };
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
        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    }

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
    C3D_DrawArrays(GPU_TRIANGLES, first, count);
    // Restore the frame's default for the next draw, so every draw starts
    // from the state begin_frame installed. The transition back is cached in
    // the same way; repeated Solid draws do not dirty the effect block.
    if (r->cullMode != GPU_CULL_BACK_CCW) {
        C3D_CullFace(GPU_CULL_BACK_CCW);
        r->cullMode = GPU_CULL_BACK_CCW;
    }
}

// ---------------------------------------------------------------------------
// The 2D canvas
// ---------------------------------------------------------------------------

 void mdk_render_canvas_begin(MdkRenderer r) { (void)r; }
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

 void mdk_render_begin_inset(MdkRenderer r, const mdk::Inset* i) { (void)r; (void)i; }
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
