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

// The push constants. The layout is the GLSL push block's, in order:
// mat4 (64 bytes) + vec4 (16) + four ints (16) = 96 bytes, within the
// 128-byte minimum every implementation guarantees.
struct PushConstants {
    float mvp[16];
    float colour[4];
    int32_t textured;     // 0 flat, 1 index+palette, 2 RGBA
    int32_t frame_count;
    int32_t frame;
    int32_t unused_;
};

// The sky pipeline's push block (the same 96-byte range as PushConstants):
// the clip-to-direction matrix (column-vector form, the transpose of the
// row-vector view matrices) then the panorama uniforms.
struct SkyPush {
    float clip_to_direction[16];
    float wrap_width;
    float horizon_row;
    float offset;
    float height;
    int32_t top_colour;
    int32_t bottom_colour;
    float row_shift;
    int32_t sampling;
};

struct TextureVk {
    bool used = false;
    bool isIndex = false;
    int32_t width = 0;
    int32_t height = 0;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
    // The palette indices, kept for the enhanced look's colour expansion
    // (the C# keeps them in _indexPixels the same way). Bumped version on
    // every rewrite (bullet holes), so the colour cache can tell.
    std::vector<uint8_t> indices;
    uint32_t version = 0;
    // A 2D-array view of an RGBA texture's image, for the enhanced
    // sampler (which is an array sampler even for one-layer textures).
    VkImageView arrayView = VK_NULL_HANDLE;
};

struct PaletteVk {
    bool used = false;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    void* mapped = nullptr;
    // The palette's bytes, kept for the colour expansion.
    uint8_t rgba[1024] = {};
};

// One expanded colour texture (render/colour_mips): RGBA8, a mip chain,
// one array layer per frame. Built on demand for a (texture, palette,
// frames) triple and rebuilt when the index texture's version moves.
struct ColourVk {
    bool used = false;
    int32_t tex = -1;
    int32_t pal = -1;
    int32_t frames = 1;
    uint32_t version = 0;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

// A mod's whole colour image given instead of the palette expansion
// (mdk_render_replace_colour / _replace_image): premultiplied RGBA8, frames
// stacked downwards, kept until the index texture's version moves -- a
// changed texture shows its expansion again, exactly as the C#'s Changed
// drops _replaced. Canvas images (image=true) survive texture changes.
struct ReplacementVk {
    bool used = false;
    bool image = false;
    int32_t tex = -1;
    int32_t pal = -1;
    int32_t frames = 1;
    int32_t width = 0;
    int32_t height = 0;
    uint32_t textureVersion = 0;
    std::vector<uint8_t> rgba;
};

// THE VERTEX BUFFER SLOTS, and why a dynamic mesh needs more than one.
//
// A STATIC mesh is written once, when it is made, before any frame has drawn
// with it, so one buffer is enough.
//
// A DYNAMIC mesh is rewritten every frame -- the object stream, Kurt's sprite,
// the canvas -- and the frame being recorded is not the only one the GPU may be
// working on. beginFrame waits the fence of the slot it is about to record, not
// the fence of the slot the previous frame used, so with two frames in flight
// the previous submission can still be reading a vertex buffer while this
// frame's CPU code is writing it. AGENTS.md P3 puts the completion boundary
// BEFORE CPU reuse, so a dynamic mesh has one buffer per frame in flight and
// writes the slot its own frame will draw from: the slot whose fence beginFrame
// just waited, which is the frame two back and never the one in front.
struct MeshVk {
    bool used = false;
    bool dynamic = false;
    VkBuffer buffer[mdk::vk::kFramesInFlight] = {};
    VkDeviceMemory memory[mdk::vk::kFramesInFlight] = {};
    void* mapped[mdk::vk::kFramesInFlight] = {};
    VkDeviceSize capacity = 0;   // bytes, per slot
};

// Which slot of a mesh this frame writes and draws. A static mesh always uses
// slot 0, which no frame is rewriting.
int meshSlot(const MeshVk& mesh, const mdk::vk::Context* ctx) {
    return mesh.dynamic ? (int)ctx->currentFrame : 0;
}

// A buffer whose last GPU use may still be running. It is destroyed at the
// frame boundary that proves the reading frame has finished; see `retiring`.
struct RetiredBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t safeFrame = 0;
};

// A descriptor set the cache has forgotten but a recorded frame may still name.
// It goes back to the pool at the same boundary a retired buffer waits for;
// see `retiringSets`.
struct RetiredSet {
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint32_t safeFrame = 0;
};

// An image, its view and its memory that a recorded frame may still be sampling.
// A descriptor set names a VIEW, so deferring only the set is half a fix: the
// handle would wait two frames while the resource it names was destroyed on the
// spot, and AGENTS.md P3 puts the completion boundary on the RESOURCE, not on
// the handle that refers to it. Settled on the same boundary as `retiringSets`;
// see `retiringImages`.
struct RetiredImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t safeFrame = 0;
};

void destroyImage(mdk::vk::Context* ctx, VkImage* image, VkDeviceMemory* memory,
                  VkImageView* view, void** mapped);

void destroyBufferPair(VkDevice device, VkBuffer buffer, VkDeviceMemory memory) {
    if (buffer) vkDestroyBuffer(device, buffer, nullptr);
    if (memory) vkFreeMemory(device, memory, nullptr);
}

// A pipeline, keyed by (pass, blend, primitive).
struct PipelineVk {
    int key;
    VkPipeline pipeline;
};

// A descriptor set for one (texture, palette, wrap) triple.
struct DescPair {
    int32_t texture;
    int32_t palette;
    bool repeat;
    VkDescriptorSet set;
};

// Which resource a scope entry refers to, for mark/release.
struct ResRef {
    int kind;  // 0 texture, 1 palette, 2 mesh
    int32_t id;
};

struct RendererVk {
    mdk::vk::Context ctx;
    bool ready = false;
    bool inFrame = false;

    // The camera, kept so the draw path can use it.
    mdk::Camera camera;

    // The lighting rig. SHARED DATA: the Vulkan backend CONSUMES it (the
    // enhanced look); the 3DS backend ignores it (SPEC.md 5.3).
    mdk::Lighting lighting;
    // The Enhanced Mode feature toggles (SPEC.md 7.9): read once per
    // pass at setup (shadow pass, haze term, light buffer, post glow,
    // mirror draws), never per vertex or per pixel on the CPU.
    mdk::EnhancedToggles toggles;
    mdk::PointLight pointLights[64];
    int32_t pointLightCount = 0;

    mdk::Panorama panorama;
    float fade = 1.0f;
    float gamma = 1.0f;
    bool enhancedLook = false;

    // The gamma grade's objects (P6 post): a target the graded frame is
    // written into, sampled from the scene target, then presented instead
    // of it. Created on the first graded frame.
    VkImage gammaImage = VK_NULL_HANDLE;
    VkDeviceMemory gammaMemory = VK_NULL_HANDLE;
    VkImageView gammaView = VK_NULL_HANDLE;
    VkFramebuffer gammaFramebuffer = VK_NULL_HANDLE;
    uint32_t gammaWidth = 0;
    uint32_t gammaHeight = 0;
    VkShaderModule gammaVertModule = VK_NULL_HANDLE;
    VkShaderModule gammaFragModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout gammaSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout gammaPipeLayout = VK_NULL_HANDLE;
    VkPipeline gammaPipeline = VK_NULL_HANDLE;
    VkDescriptorPool gammaPool = VK_NULL_HANDLE;
    VkDescriptorSet gammaSets[mdk::vk::kFramesInFlight] = {};
    bool gammaReady = false;

    int32_t drawCalls = 0;
    int32_t triangles = 0;

    // The palette pipeline's objects.
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    // Descriptor sets that a failed `vkFreeDescriptorSets` lost for good (see
    // `settleRetired`). The handle is dropped rather than retried, so without a
    // count the only trace of one is a refusal that prints once per sentence.
    uint32_t lostDescriptorSets = 0;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipelineCache pipeCache = VK_NULL_HANDLE;
    // The C#'s two samplers (Renderer.cs:249-250): the REPEAT one for a
    // surface's index texture and the CLAMP one for the palette and the
    // sky's panorama.
    VkSampler sampler = VK_NULL_HANDLE;
    VkSampler samplerRepeat = VK_NULL_HANDLE;
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    VkShaderModule skyVertModule = VK_NULL_HANDLE;
    VkShaderModule skyFragModule = VK_NULL_HANDLE;
    VkPipeline skyPipeline = VK_NULL_HANDLE;

    // The flat-stereo state (P6). `stereo` is the requested mode from the
    // render description; `activeCamera` is the camera the draw path
    // consumes (the centre camera, or the eye being drawn); the composite
    // objects are created on the first stereo frame.
    mdk::StereoSettings stereo;
    mdk::Camera activeCamera;
    uint32_t clearColour = 0xFF000000u;
    bool replayingEyes = false;
    VkShaderModule stereoVertModule = VK_NULL_HANDLE;
    VkShaderModule stereoFragModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout stereoSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout stereoPipeLayout = VK_NULL_HANDLE;
    VkPipeline stereoPipeline = VK_NULL_HANDLE;
    VkDescriptorPool stereoPool = VK_NULL_HANDLE;
    VkDescriptorSet stereoSets[mdk::vk::kFramesInFlight] = {};
    VkSampler eyeSampler = VK_NULL_HANDLE;
    bool stereoReady = false;

    // The enhanced look's objects (P6 lit pipeline): the colour-texture
    // cache, the trilinear sampler, the per-frame descriptor pools and
    // uniform rings, and the pipelines keyed like the palette ones.
    std::vector<ColourVk> colours;
    std::vector<ReplacementVk> replacements;
    VkSampler colourSampler = VK_NULL_HANDLE;
    VkShaderModule enhancedVertModule = VK_NULL_HANDLE;
    VkShaderModule enhancedFragModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout enhancedSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout enhancedPipeLayout = VK_NULL_HANDLE;
    std::vector<PipelineVk> enhancedPipelines;
    VkDescriptorPool enhancedPools[mdk::vk::kFramesInFlight] = {};
    VkBuffer uniformRing[mdk::vk::kFramesInFlight] = {};
    VkDeviceMemory uniformRingMemory[mdk::vk::kFramesInFlight] = {};
    void* uniformRingMapped[mdk::vk::kFramesInFlight] = {};
    uint32_t uniformCursor = 0;
    VkBuffer lightBuffers[mdk::vk::kFramesInFlight] = {};
    VkDeviceMemory lightMemory[mdk::vk::kFramesInFlight] = {};
    void* lightMapped[mdk::vk::kFramesInFlight] = {};
    VkImage dummyColourImage = VK_NULL_HANDLE;
    VkDeviceMemory dummyColourMemory = VK_NULL_HANDLE;
    VkImageView dummyColourView = VK_NULL_HANDLE;
    bool enhancedReady = false;

    // The sun shadow (P6 shadow slice): a 2048x2048 depth target, its
    // depth-only pass, and the depth pipelines (Solid-culled and not).
    // The scene is deferred to end_frame under the enhanced look so the
    // shadow pass can read the whole queue before the scene renders.
    VkImage shadowImage = VK_NULL_HANDLE;
    VkDeviceMemory shadowMemory = VK_NULL_HANDLE;
    VkImageView shadowView = VK_NULL_HANDLE;
    VkRenderPass shadowPass = VK_NULL_HANDLE;
    VkFramebuffer shadowFramebuffer = VK_NULL_HANDLE;
    VkShaderModule depthFragModule = VK_NULL_HANDLE;
    VkPipeline depthPipelines[2] = {};
    bool shadowReady = false;
    float sunMatrix[16] = {};
    float postClipToView[16] = {};
    float postViewToClip[16] = {};
    bool deferScene = false;

    // The post pass (P6 occlusion/post slice): the camera's depth, the
    // enhanced scene with its mip chain, the occlusion target, and the
    // two screen pipelines (occlusion, post) over the composite pass.
    // The canvas then replays into the scene target through a LOAD pass,
    // over the post's output, as the oracle draws canvas after post.
    VkImage postSceneImage = VK_NULL_HANDLE;
    VkDeviceMemory postSceneMemory = VK_NULL_HANDLE;
    VkImageView postSceneView = VK_NULL_HANDLE;      // all mips, sampled
    VkImageView postSceneLevel0 = VK_NULL_HANDLE;    // mip 0, attached
    VkFramebuffer postSceneFramebuffer = VK_NULL_HANDLE;
    VkImage viewDepthImage = VK_NULL_HANDLE;
    VkDeviceMemory viewDepthMemory = VK_NULL_HANDLE;
    VkImageView viewDepthView = VK_NULL_HANDLE;
    VkFramebuffer viewDepthFramebuffer = VK_NULL_HANDLE;
    VkImage occlusionImage = VK_NULL_HANDLE;
    VkDeviceMemory occlusionMemory = VK_NULL_HANDLE;
    VkImageView occlusionView = VK_NULL_HANDLE;
    VkFramebuffer occlusionFramebuffer = VK_NULL_HANDLE;
    VkFramebuffer postFramebuffer = VK_NULL_HANDLE;  // over the scene view
    VkRenderPass canvasLoadPass = VK_NULL_HANDLE;
    VkFramebuffer canvasFramebuffer = VK_NULL_HANDLE;
    VkShaderModule postVertModule = VK_NULL_HANDLE;
    VkShaderModule occlusionFragModule = VK_NULL_HANDLE;
    VkShaderModule postFragModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout postSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout postPipeLayout = VK_NULL_HANDLE;
    VkPipeline occlusionPipeline = VK_NULL_HANDLE;
    VkPipeline postPipeline = VK_NULL_HANDLE;
    VkDescriptorPool postPool = VK_NULL_HANDLE;
    VkDescriptorSet postSets[mdk::vk::kFramesInFlight] = {};
    VkBuffer postUniformBuffers[mdk::vk::kFramesInFlight] = {};
    VkDeviceMemory postUniformMemory[mdk::vk::kFramesInFlight] = {};
    void* postUniformMapped[mdk::vk::kFramesInFlight] = {};
    VkSampler postMipSampler = VK_NULL_HANDLE;
    VkSampler postClampSampler = VK_NULL_HANDLE;
    uint32_t postWidth = 0;
    uint32_t postHeight = 0;
    uint32_t postMips = 0;
    bool postReady = false;

    // Canvas quads deferred under the enhanced look: the vertices are in
    // the canvas ring as usual; these say where and with what to draw.
    struct CanvasRecord {
        VkDeviceSize offset = 0;
        int32_t texture = -1;
        int32_t palette = -1;
        int32_t frameCount = 1;
        float colour[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    };
    std::vector<CanvasRecord> canvasRecords;
    std::vector<PipelineVk> pipelines;
    std::vector<DescPair> descs;

    // Resource pools. The id IS the index; a negative id is "none".
    std::vector<TextureVk> textures;
    std::vector<PaletteVk> palettes;
    std::vector<MeshVk> meshes;
    std::vector<ResRef> creationLog;

    // The buffers that were released while a frame that reads them may still be
    // running. Each one names the frame index at which its reading frame is
    // provably finished; `settleRetired` destroys them there, which is the
    // CPU/GPU completion boundary AGENTS.md P3 asks for.
    std::vector<RetiredBuffer> retiring;
    // The descriptor sets the cache dropped because the image view they name is
    // going away. Dropping the CACHE ENTRY is what makes a recreated id safe
    // (review-25); returning the SET is what keeps the pool alive, and the two
    // are different acts. Settled beside `retiring`, on the same boundary.
    std::vector<RetiredSet> retiringSets;
    // The images whose view a retired descriptor set names. Destroying the view
    // at the call that retired the set left a frame on the queue sampling a
    // destroyed view, which is the half of the resource AGENTS.md P3 asks about
    // and review-9 F2 named. Settled on the same boundary as `retiringSets`.
    std::vector<RetiredImage> retiringImages;
    // Counts the frames this backend has begun. The retire deadline is
    // `frameIndex + kFramesInFlight`; see `retireBuffer`.
    uint32_t frameIndex = 0;

    // The frame's queued draws, submitted in the oracle's pass order. The
    // caller queues a draw; the backend decides when the frame's 3D scene ends
    // and draws the buckets then (render.h's "the backend sorts and batches",
    // and draw_queue.h for the order).
    mdk::DrawQueue queue;

    // The 2D canvas: quads accumulated between begin/end, drawn in one
    // orthographic Overlay pass. A ring of dynamic vertex buffers -- one per
    // frame in flight, for the reason MeshVk spells out -- resized as needed.
    VkBuffer canvasBuffer[mdk::vk::kFramesInFlight] = {};
    VkDeviceMemory canvasMemory[mdk::vk::kFramesInFlight] = {};
    void* canvasMapped[mdk::vk::kFramesInFlight] = {};
    VkDeviceSize canvasCapacity = 0;
    // The write cursor into the canvas buffer. Each quad is uploaded at its own
    // offset and drawn from there, because the persistent map is written for
    // the whole frame at record time -- reusing one slot would make every
    // draw read the last quad's vertices.
    VkDeviceSize canvasOffset = 0;

    // The frame-capture path requested for the frame now being drawn. The
    // path is copied because MdkFrameParams owns only a borrowed C string.
    std::string screenshotPath;

    // The capture ACCOUNTING: how many frames asked for a picture, and how many
    // pictures reached the disk. Every failure below already returned a bool and
    // the frame seam threw it away, so the run exited 0 with no file and the
    // harness upstream read the BMP the previous run left there (review-2 F1).
    // `capturesWritten` is incremented ONLY on the path where the bytes landed.
    int32_t capturesRequested = 0;
    int32_t capturesWritten = 0;
};

// Defined with the drawing code below; the frame and the canvas need them and
// they come later in the file.
void flushDraws(RendererVk* r);

RendererVk* asVk(MdkRenderer r) {
    return static_cast<RendererVk*>(r);
}

// Defined below, with the rest of the refusal reporting. Declared here because
// `settleRetired` and the one-shot helpers are the first sites in the file that
// can be refused.
void refused(const char* fmt, ...);

// Stop using a buffer without destroying it yet.
//
// The caller has finished with it on the CPU, but a submission that reads it may
// still be running: a mesh released in frame N was drawn by frame N or earlier,
// and nothing has waited for that work. The deadline is two frames out, and the
// wait that makes it real is the fence beginFrame performs on the slot it is
// about to record. With two frames in flight the slot frame N used is next
// recorded at frame N+2, and the fence waited there is frame N's submission --
// so destroying at N+2 is after the GPU's last read. A buffer retired between
// frames (frameIndex has not advanced yet) is only more conservative.
void retireBuffer(RendererVk* r, VkBuffer buffer, VkDeviceMemory memory) {
    if (!buffer && !memory) {
        return;
    }
    RetiredBuffer retired;
    retired.buffer = buffer;
    retired.memory = memory;
    retired.safeFrame = r->frameIndex + mdk::vk::kFramesInFlight;
    r->retiring.push_back(retired);
}

// Destroy the retired buffers whose reading frame has finished. Called at the
// frame boundary AFTER beginFrame's fence wait, which is the GPU-completion
// boundary (AGENTS.md P3): a fence, not a lock and not a cache flush.
void settleRetired(RendererVk* r) {
    for (size_t i = 0; i < r->retiring.size();) {
        const RetiredBuffer retired = r->retiring[i];
        if (retired.safeFrame > r->frameIndex) {
            i++;
            continue;
        }
        destroyBufferPair(r->ctx.device, retired.buffer, retired.memory);
        r->retiring.erase(r->retiring.begin() + (long)i);
    }
    // The sets the cache forgot. A descriptor set is a slot in a pool with a
    // fixed `maxSets`, and nothing in this backend has ever given one back: no
    // `vkFreeDescriptorSets`, no `vkResetDescriptorPool`. So every texture or
    // palette released and re-made — and `mdk_render_update_texture` does that
    // on every same-size update — spent a set for the rest of the process, and
    // when the pool ran out every draw that needed a new pair was skipped.
    //
    // THE SET waits for the boundary the buffers wait for, for the set's own
    // reason: `vkFreeDescriptorSets` requires the set not be in use, and a frame
    // that recorded `vkCmdBindDescriptorSets` with it may still be on the queue.
    // Only the SET waits here for the SET's own reason. The image view it names
    // used to be destroyed at the drop site with no deadline at all; it now goes
    // through `retireImage` and settles in the loop below this one, on the same
    // boundary, so the pair is a completion boundary and not half of one.
    for (size_t i = 0; i < r->retiringSets.size();) {
        const RetiredSet retired = r->retiringSets[i];
        if (retired.safeFrame > r->frameIndex) {
            i++;
            continue;
        }
        VkDescriptorSet set = retired.set;
        const VkResult freed = vkFreeDescriptorSets(r->ctx.device, r->descPool, 1, &set);
        if (freed != VK_SUCCESS) {
            // The handle is dropped rather than retried, and two rules decide that.
            // They are stated as rules and not quoted (review-10 F3: an earlier draft
            // of this comment put one of them in quotation marks, and the vendored
            // header carries no spec prose to check a quotation against, so a quote
            // here is unfalsifiable and belongs in the spec, not here). First: the
            // out-of-memory result codes this call can return do not say whether the
            // sets were taken when one of them comes back. Second: a set handed to it
            // must be a valid handle this pool issued and not one already returned.
            // So a retry can be a second free of a handle the driver already took.
            // Losing one set is the smaller failure — but it is COUNTED, because the
            // refusal below prints once and a pool quietly holding N fewer sets than
            // its reader believes is the same silent-budget bug this batch closed.
            r->lostDescriptorSets++;
            refused("return a descriptor set to the pool: the set is lost and the "
                    "pool has one fewer to hand out; when it empties, every draw "
                    "that needs a new texture/palette pair is skipped");
        }
        r->retiringSets.erase(r->retiringSets.begin() + (long)i);
    }
    // The images those sets name. A descriptor set holds a VIEW, so retiring the
    // handle and destroying the resource in the same call defers half of a
    // lifetime: a frame still on the queue can be sampling a view that no longer
    // exists, which is AGENTS.md P3's "do not free an in-flight resource" read on
    // the resource rather than the handle (review-9 F2). Same boundary, same
    // reason, and the destroy order is `destroyImage`'s — view, then image, then
    // memory — because freeing memory bound to a live image is invalid use.
    for (size_t i = 0; i < r->retiringImages.size();) {
        const RetiredImage retired = r->retiringImages[i];
        if (retired.safeFrame > r->frameIndex) {
            i++;
            continue;
        }
        VkImage image = retired.image;
        VkDeviceMemory memory = retired.memory;
        VkImageView view = retired.view;
        destroyImage(&r->ctx, &image, &memory, &view, nullptr);
        r->retiringImages.erase(r->retiringImages.begin() + (long)i);
    }
}

// Stop using a descriptor set without returning it to the pool yet.
//
// The cache drops a set because the image view it names is about to be destroyed
// or replaced, so the entry must go NOW — but the set itself is named by whatever
// this frame has already recorded, and `vkFreeDescriptorSets` on a set an
// in-flight command buffer binds is invalid use. So the SET waits for the
// boundary `settleRetired` implements, which is the fence `vk::beginFrame` just
// waited (AGENTS.md P3), not a lock and not a cache flush.
//
// THE VIEW WAITS TOO. A retired set is a handle, and the handle is only half of
// what a queued frame touches: the other half is the image view the set names.
// `retireImage` below defers that pair to the same boundary, because deferring
// the handle and destroying the resource on the spot is the half-fix review-9 F2
// named — AGENTS.md P3 puts the boundary on the resource, not on the reference.
void retireDescriptorSet(RendererVk* r, VkDescriptorSet set) {
    if (!set) {
        return;
    }
    RetiredSet retired;
    retired.set = set;
    retired.safeFrame = r->frameIndex + mdk::vk::kFramesInFlight;
    r->retiringSets.push_back(retired);
}

// Stop using an image without destroying it yet, on the boundary
// `retireDescriptorSet` uses. Every caller of `forgetDescriptorSets` was
// destroying the image, its view and its memory in the same call that retired
// the set naming them, so the set waited two frames and the resource waited
// none. The destroy itself stays `destroyImage`'s, at the settle, so the view /
// image / memory order is one place and not two.
void retireImage(RendererVk* r, VkImage image, VkDeviceMemory memory, VkImageView view) {
    if (!image && !memory && !view) {
        return;
    }
    RetiredImage retired;
    retired.image = image;
    retired.memory = memory;
    retired.view = view;
    retired.safeFrame = r->frameIndex + mdk::vk::kFramesInFlight;
    r->retiringImages.push_back(retired);
}

// ---------------------------------------------------------------------------
// Vulkan helpers
// ---------------------------------------------------------------------------

uint32_t findMemoryType(mdk::vk::Context* ctx, uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps = {};
    vkGetPhysicalDeviceMemoryProperties(ctx->physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

// THE ONE-SHOT COMMAND BUFFER AND ITS POOL TRAVEL TOGETHER.
//
// They used to be paired by POSITION in a file-scope vector: `beginOneShotTracked`
// pushed the pool and `endOneShot` destroyed the front entry, which was sound only
// while every creation ran begin-use-end with no refusal in between. A refusal
// between the push and the return stranded an entry, and from then on EVERY
// `endOneShot` freed a foreign command buffer into a pool it had not come from and
// destroyed that pool one step early — `vkFreeCommandBuffers` requires the pool the
// handle was allocated from (review-6 F2). Handing the pool to the caller makes the
// pairing impossible to lose: there is no ledger to desynchronize, nothing left to
// drain at teardown, and a refusal has exactly one owner for the pool it made.
//
// THREE OUTCOMES, NOT TWO, AND THE THIRD ONE OWNS SOMETHING.
//
// This used to return a bool, which could only say "the upload did not happen" — and
// a bool has to be one of two things, so the case where the queue ACCEPTED the work
// and the wait then failed was reported as the case where the queue refused it. The
// two demand opposite things of the caller. A submit the queue refused never touched
// the resources its commands name, so they are still purely the CPU's to release. A
// submit the queue accepted and a wait that could not prove finished may STILL BE
// RUNNING, and releasing the memory it reads or writes is the use-after-free
// AGENTS.md P3 forbids in its own words: "do not overwrite, resize, or free an
// in-flight resource ... put the completion boundary BEFORE CPU reuse". A failed
// wait is a completion boundary that was NOT established, which is not the same fact
// as a completion (review-8 F1).
//
// The three VkResults here were discarded once, and the caller could not tell an
// upload that ran from one the driver refused: `createHostImage` returned true for an
// image whose layout transition and buffer->image copy lived in a command stream that
// was never submitted, which is review-5 F1's "an image that exists but is not the
// texture" re-opened one level down (review-7 F1). AGENTS.md P3 asks for the same
// distinction from the other side: CPU ownership, cache visibility and GPU
// completion are three obligations, and `vkQueueWaitIdle` is the completion boundary
// this file has for a one-shot queue.
//
// They are sequenced, not merely checked. A command buffer whose `vkEndCommandBuffer`
// failed is not finalised, and submitting an unfinalised command buffer is invalid
// use, so a failed end must not reach the submit; and there is no point waiting for a
// submission the queue refused.
enum class OneShot {
    done,             // the wait proved it: everything it named is safe to release
    neverSubmitted,   // the queue refused it: safe to release, and the resource is refused
    unfenced          // accepted, completion unknown: NOT this side of the boundary
};

OneShot endOneShot(mdk::vk::Context* ctx, VkCommandPool pool, VkCommandBuffer cb) {
    const bool ended = vkEndCommandBuffer(cb) == VK_SUCCESS;
    if (!ended) {
        refused("end of a one-shot command buffer: the recording is not finalised, so "
                "nothing it holds is submitted and its caller must not believe it was");
    }
    bool submitted = false;
    if (ended) {
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb;
        submitted = vkQueueSubmit(ctx->queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
        if (!submitted) {
            refused("submit of a one-shot upload: nothing was copied and nothing was "
                    "transitioned, so the resource this upload was made for is refused");
        }
    }
    OneShot result = OneShot::neverSubmitted;
    if (submitted) {
        // The queue has it. The retry at device scope is here because a device-wide
        // wait proves the same thing about this queue; it waits more than this queue
        // does — a stall, which is affordable on the one path where the narrow wait has
        // already failed — and a queue wait can fail where a device wait does not. If
        // both say no, the submission's completion is UNKNOWN, and unknown is not
        // finished: the caller must not release what the commands name.
        if (vkQueueWaitIdle(ctx->queue) == VK_SUCCESS ||
            vkDeviceWaitIdle(ctx->device) == VK_SUCCESS) {
            result = OneShot::done;
        } else {
            refused("wait for a one-shot upload: the copy is not known to have finished, "
                    "so the memory it reads or writes is NOT safe to release and its "
                    "caller must abandon it rather than free it");
            result = OneShot::unfenced;
        }
    }
    // AN UNFENCED SUBMISSION OWNS ITS OWN COMMAND STREAM. The queue has the copy, the
    // wait could not prove it finished, and the instructions of that copy live in `cb`,
    // allocated from `pool`. `vkFreeCommandBuffers` requires that every submitted
    // command in the buffer has completed (VUID-vkFreeCommandBuffers-pCommandBuffers-
    // 00047) and `vkDestroyCommandPool` requires the same of every command buffer made
    // from the pool (VUID-vkDestroyCommandPool-commandPool-00041). Batch 152 stopped the
    // CALLERS from releasing what an unfenced submission named, and this tail went on
    // freeing the stream those callers had just abandoned (review-1 F3).
    //
    // So the pair is abandoned on the callers' terms for the callers' reason: the
    // handles stop being ours instead of being destroyed. Two driver objects per failed
    // wait, next to the image and the staging memory the caller already abandoned above,
    // on a driver that fails a wait; a TRANSIENT pool is expected to be reclaimed with
    // the device. The guard returns, and everything below it is the proven-finished
    // path — that ordering is what B10 pins.
    if (result == OneShot::unfenced) {
        refused("one-shot command buffer and its pool are abandoned, not freed: the "
                "submission that may still be running is made of them");
        return result;
    }
    // A command buffer whose end failed is still in the RECORDING state, and
    // `vkFreeCommandBuffers` is not the way out of it: the reset that would be the
    // documented recovery needs VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    // which this pool deliberately does not ask for. Destroying the pool reclaims
    // every command buffer allocated from it, recording or not, which is what frees
    // this one (review-8 F6).
    if (ended) {
        vkFreeCommandBuffers(ctx->device, pool, 1, &cb);
    }
    vkDestroyCommandPool(ctx->device, pool, nullptr);
    return result;
}

// Returns the command buffer and, through `outPool`, the pool it came from; the
// caller hands both to `endOneShot`. When it returns VK_NULL_HANDLE it has already
// destroyed everything it made, so the caller has nothing left to undo.
VkCommandBuffer beginOneShotTracked(mdk::vk::Context* ctx, VkCommandPool* outPool) {
    *outPool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    alloc.queueFamilyIndex = ctx->queueFamily;
    alloc.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(ctx->device, &alloc, nullptr, &pool) != VK_SUCCESS) {
        refused("one-shot command pool: no texture, image or screenshot upload can be "
                "recorded while the driver keeps saying no");
        return VK_NULL_HANDLE;
    }
    // From here the pool is this function's to undo until the caller has a command
    // buffer to take away with it.
    VkCommandBufferAllocateInfo cbAlloc = {};
    cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAlloc.commandPool = pool;
    cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(ctx->device, &cbAlloc, &cb) != VK_SUCCESS) {
        refused("one-shot command buffer: the upload that asked for it copies into nothing "
                "and its caller refuses the resource instead");
        vkDestroyCommandPool(ctx->device, pool, nullptr);
        return VK_NULL_HANDLE;
    }
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &begin) != VK_SUCCESS) {
        refused("begin of a one-shot command buffer: the upload that asked for it has "
                "nowhere to be recorded");
        vkFreeCommandBuffers(ctx->device, pool, 1, &cb);
        vkDestroyCommandPool(ctx->device, pool, nullptr);
        return VK_NULL_HANDLE;
    }
    *outPool = pool;
    return cb;
}

// ---------------------------------------------------------------------------
// REFUSALS ARE PRINTED, NOT SWALLOWED.
//
// A refused resource is a missing picture: the frame still renders, the geometry
// is simply not in it, and every gate stays green because a frame with less in it
// is still a frame. The Citro3D backend has always said so — `renderer_c3d.cpp`
// prints `linearAlloc(N) FAILED for a dynamic mesh of %d corners` — and the
// Vulkan backend answered the same failures with a bare `return -1`, which is the
// half review-11 F4 recorded against `object_draw.h` and no batch fixed: "the
// Vulkan backend's mesh refusal prints nothing either, so a refused mesh on Linux
// is still a silent missing picture".
//
// Every refusal site below prints the request the driver was GIVEN, not a guess at
// it, for the reason review-11 F1 gives for the frontend's copy of this line: a
// number derived from something else (the frame's corner count, the previous
// mesh's size) disagrees with the backend's own byte count and sends the next
// reader after a refusal that never happened. Where the driver is asked for the
// same thing more than once — a dynamic mesh and the canvas ring are one buffer
// PER FRAME IN FLIGHT — the line prints the total as well as the per-slot figure,
// because "98304 bytes x 2 frames in flight" reads as a 96 KB budget when the
// driver was actually asked for 192 KB (review-5 F3).
//
// ONE LINE PER DISTINCT REQUEST, NOT ONE PER REFUSAL (review-5 F2). The Citro3D
// backend latches with a bool that a successful allocation clears
// (`renderer_c3d.cpp`'s `meshAllocFailureLogged`), and it does that because the
// line otherwise printed 1710 times in one run (review-10 F3, pinned by
// `c3d_frame_test.sh`). A bool is the right shape there and the wrong one here:
// the Vulkan sites repeat per DRAW as well as per frame — a descriptor set is
// allocated for every textured draw — so a bool would report the first refusal of
// a run and then hide every larger one after it, which is the review-11 F7 defect
// the frontend's re-arming latch exists to avoid. So this helper remembers every
// request it has already named and stays silent only for one of those: a sustained
// refusal of the same request is one line however many frames it lasts, and a
// request that changes — a mesh that grows, a ring that resizes, a second palette
// — prints. Measured, not assumed: with every descriptor allocation forced to
// fail, level 3 attempts 48 distinct (texture, palette, sampler) triples and
// draws 187 of them per frame, so printing per refusal is 22500 lines in a 120
// frame run and printing per distinct request is 48 (evidence:
// docs/evidence/2026-11-12-149-refusal-discovery.txt).
// ---------------------------------------------------------------------------
struct RefusalLine {
    char text[256];
};
// One entry per request already named. Refusals are rare and the table is bounded,
// so a linear scan is the right data structure. Past the cap everything prints:
// an unreported refusal is the bug this helper exists to fix, and a repeated line
// is only noise.
std::vector<RefusalLine> g_refusalLines;
const size_t kRefusalMemory = 256;

void refused(const char* fmt, ...) {
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    // THE KEY IS THE RENDERED REQUEST, not the call site. A source-site field used
    // to be part of this key on the theory that "the address of `fmt` IS the site".
    // That is not true: identical literals are merged at -O1 and above, so two
    // distinct sites can share one address, and a field that cannot tell them apart
    // is worse than no field, because the comment claimed it could (review-6 F4).
    // What is true is weaker and it is enough: a request is a rendered sentence, two
    // sites that render the same sentence are reporting the same fact, and it is the
    // SET that stops the flood. A per-draw site needs a set rather than a latch: the
    // descriptor refusal names 48 different requests in a level, and remembering
    // only the last one printed would re-print all 48 on every frame after that.
    //
    // THE MEMORY LASTS FOR THE LIFE OF THE PROCESS, deliberately, and that is an
    // asymmetry with the frontends rather than an oversight. The frontend latch
    // re-arms on every frame that did not refuse, because it is told once per frame
    // whether the pass refused. This helper is never told about a success, so "print
    // again when it comes back" needs a per-request success signal the backend does
    // not have, and clearing the table on a frame boundary restores the 22500-lines-
    // in-one-run flood the set exists to prevent (review-5 F2, review-10 F3). A
    // request that recovers and then recurs byte-identically is therefore silent the
    // second time (review-6 F5); one that recurs DIFFERENTLY — a bigger canvas, a
    // different texture — is a different request and does print.
    for (size_t i = 0; i < g_refusalLines.size(); i++) {
        if (std::strcmp(g_refusalLines[i].text, text) == 0) {
            return;  // this exact request was named already
        }
    }
    if (g_refusalLines.size() < kRefusalMemory) {
        RefusalLine line;
        std::snprintf(line.text, sizeof(line.text), "%s", text);
        g_refusalLines.push_back(line);
    }
    std::printf("[vk] REFUSED %s\n", text);
    std::fflush(stdout);
}

// Create a host-visible buffer and map it persistently.
bool createHostBuffer(mdk::vk::Context* ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                      VkBuffer* outBuffer, VkDeviceMemory* outMemory, void** outMapped) {
    VkBufferCreateInfo bufInfo = {};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = size;
    bufInfo.usage = usage;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(ctx->device, &bufInfo, nullptr, outBuffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req = {};
    vkGetBufferMemoryRequirements(ctx->device, *outBuffer, &req);
    const uint32_t type = findMemoryType(ctx, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == 0xFFFFFFFFu) {
        vkDestroyBuffer(ctx->device, *outBuffer, nullptr);
        *outBuffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(ctx->device, &alloc, nullptr, outMemory) != VK_SUCCESS) {
        vkDestroyBuffer(ctx->device, *outBuffer, nullptr);
        *outBuffer = VK_NULL_HANDLE;
        return false;
    }
    // A discarded VkResult here left a buffer bound to nothing, mapped, and handed
    // back as a success: every memcpy into it landed in the CPU's copy of an
    // unbound allocation and the frame drew whatever the buffer actually held.
    if (vkBindBufferMemory(ctx->device, *outBuffer, *outMemory, 0) != VK_SUCCESS) {
        refused("bind of a host buffer of %llu bytes: the buffer and its memory are "
                "destroyed, so the caller refuses the resource that asked for them",
                (unsigned long long)size);
        vkDestroyBuffer(ctx->device, *outBuffer, nullptr);
        *outBuffer = VK_NULL_HANDLE;
        vkFreeMemory(ctx->device, *outMemory, nullptr);
        *outMemory = VK_NULL_HANDLE;
        return false;
    }
    if (outMapped) {
        if (vkMapMemory(ctx->device, *outMemory, 0, VK_WHOLE_SIZE, 0, outMapped) != VK_SUCCESS) {
            // Destroy the object BEFORE releasing the memory bound to it. Freeing
            // first is invalid use, and the same order now holds in every teardown
            // block in this file (review-6 F6; `linux_refusal_report_test.sh` B9).
            vkDestroyBuffer(ctx->device, *outBuffer, nullptr);
            *outBuffer = VK_NULL_HANDLE;
            vkFreeMemory(ctx->device, *outMemory, nullptr);
            *outMemory = VK_NULL_HANDLE;
            *outMapped = nullptr;
            return false;
        }
    }
    return true;
}

// Create a host-visible 2D image, write `bytes` into it, and transition it
// to SHADER_READ_ONLY. `bytesPerTexel` is 1 (R8) or 4 (RGBA8).
bool createHostImage(mdk::vk::Context* ctx, int32_t width, int32_t height, VkFormat format,
                     uint32_t bytesPerTexel, const uint8_t* pixels, VkImage* outImage,
                     VkDeviceMemory* outMemory, VkImageView* outView, void** outMapped) {
    const VkDeviceSize size = (VkDeviceSize)width * (VkDeviceSize)height * bytesPerTexel;

    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = format;
    imgInfo.extent.width = (uint32_t)width;
    imgInfo.extent.height = (uint32_t)height;
    imgInfo.extent.depth = 1;
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx->device, &imgInfo, nullptr, outImage) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req = {};
    vkGetImageMemoryRequirements(ctx->device, *outImage, &req);
    const uint32_t type = findMemoryType(ctx, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == 0xFFFFFFFFu) {
        vkDestroyImage(ctx->device, *outImage, nullptr);
        *outImage = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(ctx->device, &alloc, nullptr, outMemory) != VK_SUCCESS) {
        vkDestroyImage(ctx->device, *outImage, nullptr);
        *outImage = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindImageMemory(ctx->device, *outImage, *outMemory, 0) != VK_SUCCESS) {
        refused("bind of a %dx%d image of %llu bytes: the image and its memory are "
                "destroyed, so the caller gets no texture rather than an unbound one",
                width, height, (unsigned long long)size);
        vkDestroyImage(ctx->device, *outImage, nullptr);
        *outImage = VK_NULL_HANDLE;
        vkFreeMemory(ctx->device, *outMemory, nullptr);
        *outMemory = VK_NULL_HANDLE;
        return false;
    }
    *outMapped = nullptr;

    // The pixels go through a staging buffer and a buffer->image copy.
    // Copying straight into a mapped OPTIMAL image is legal API but the
    // layout is implementation-defined -- a linear memcpy lands as garbage
    // on at least one driver (llvmpipe), and validation says nothing.
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    void* stagingMapped = nullptr;
    if (pixels &&
        !createHostBuffer(ctx, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging,
                          &stagingMemory, &stagingMapped)) {
        vkDestroyImage(ctx->device, *outImage, nullptr);
        *outImage = VK_NULL_HANDLE;
        vkFreeMemory(ctx->device, *outMemory, nullptr);
        *outMemory = VK_NULL_HANDLE;
        return false;
    }
    if (pixels) {
        memcpy(stagingMapped, pixels, (size_t)size);
        vkUnmapMemory(ctx->device, stagingMemory);
    }

    VkCommandPool oneShotPool = VK_NULL_HANDLE;
    VkCommandBuffer cb = beginOneShotTracked(ctx, &oneShotPool);
    OneShot upload = OneShot::neverSubmitted;
    if (cb) {
        VkImageMemoryBarrier toTransfer = {};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.srcAccessMask = 0;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = *outImage;
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.levelCount = 1;
        toTransfer.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &toTransfer);

        if (pixels) {
            VkBufferImageCopy region = {};
            region.bufferOffset = 0;
            region.bufferRowLength = 0;   // tightly packed
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = {0, 0, 0};
            region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
            vkCmdCopyBufferToImage(cb, staging, *outImage,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }

        VkImageMemoryBarrier toSample = toTransfer;
        toSample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toSample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toSample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toSample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toSample);
        upload = endOneShot(ctx, oneShotPool, cb);
    }
    if (upload == OneShot::unfenced) {
        // NOTHING IN THIS BLOCK MAY RELEASE ANYTHING. The queue accepted the copy and
        // the wait could not prove it finished, so it may still be reading the staging
        // buffer and writing this image. Freeing either here is exactly what the
        // refusal that has already printed said was not safe, and it is the
        // use-after-free AGENTS.md P3 puts a completion boundary in front of
        // (review-8 F1: the bool this function used to get back could only say "it did
        // not happen", so this case was reported as the refused-submit case and fell
        // straight into the destroy below).
        //
        // So ownership is abandoned, out loud. The handles are nulled because this
        // function is no longer their owner, NOT because they were destroyed, and the
        // caller restores the image it already had and never sees them. The leak is
        // bounded by the number of failed waits — zero on every driver measured here,
        // and a driver that fails a wait is a driver on its way to a lost device.
        // The counter this line does not carry is deliberate: the refusal set keys on
        // the rendered sentence, so a counter would make every leak a new sentence and
        // re-open the flood B7 exists to stop.
        refused("%dx%d image of %llu bytes and its staging buffer are abandoned, not "
                "destroyed: a submission that may still be running wrote into them",
                width, height, (unsigned long long)size);
        *outImage = VK_NULL_HANDLE;
        *outMemory = VK_NULL_HANDLE;
        *outView = VK_NULL_HANDLE;
        return false;
    }
    if (upload != OneShot::done) {
        // THE IMAGE IS NOT THE TEXTURE. It exists, but nothing was ever copied
        // into it and its layout was never set, so a `return true` here handed
        // the caller a texture that samples as noise and a log with no failure
        // in it (review-5 F1). Destroy it; the caller's own line names the
        // texture this one was meant to become.
        //
        // There are three ways to arrive here and they used to be one: no command
        // buffer at all, a command buffer whose submit the driver refused, and (until
        // review-8 F1) a submission whose completion was never proven. Whoever refused
        // it printed the cause; this line names which image is being destroyed instead
        // of returned, which is the half the caller needs and the half nobody else can
        // say. The unfenced case above is the one that may NOT be destroyed.
        refused("%dx%d image of %llu bytes is destroyed, not returned: its fill and "
                "layout transition were never executed",
                width, height, (unsigned long long)size);
        if (staging) {
            vkDestroyBuffer(ctx->device, staging, nullptr);
        }
        if (stagingMemory) {
            vkFreeMemory(ctx->device, stagingMemory, nullptr);
        }
        vkDestroyImage(ctx->device, *outImage, nullptr);
        *outImage = VK_NULL_HANDLE;
        vkFreeMemory(ctx->device, *outMemory, nullptr);
        *outMemory = VK_NULL_HANDLE;
        return false;
    }
    if (staging) {
        vkDestroyBuffer(ctx->device, staging, nullptr);
    }
    if (stagingMemory) {
        vkFreeMemory(ctx->device, stagingMemory, nullptr);
    }

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = *outImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(ctx->device, &viewInfo, nullptr, outView) != VK_SUCCESS) {
        // The image and its memory were this function's to keep until it handed
        // them over. Returning false with both still bound and still non-null
        // leaked them and left the caller holding a handle it had been told not
        // to use (review-6 open question 3, CONTINUATION item 1).
        refused("view of a %dx%d image: the image and its memory are destroyed, so the "
                "caller creates no texture rather than one nothing can sample",
                width, height);
        vkDestroyImage(ctx->device, *outImage, nullptr);
        *outImage = VK_NULL_HANDLE;
        vkFreeMemory(ctx->device, *outMemory, nullptr);
        *outMemory = VK_NULL_HANDLE;
        *outView = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

// Write an uncompressed, top-down 32-bit BMP. The pixels are BGRA, the
// same layout the C# oracle's Bmp.Write emits. Keeping the two files'
// containers identical makes the frame comparator work on the pixels, not
// on two subtly different image encoders.
//
// THE PICTURE IS PUBLISHED BY RENAME, NOT BY WRITING IN PLACE. A half-written
// BMP at the requested path is a file that exists, is non-empty, and is not a
// frame -- and "the file exists and is non-empty" is exactly the test a capture
// harness reaches for. So the bytes go to a sibling `<path>.part` and only a
// complete write is renamed onto the destination: a short write, a full disk or
// a failed close leaves the destination exactly as it found it and removes the
// partial file it made, which is a file this function created and therefore
// always safe to unlink. The frontend separately clears any previous picture at
// the destination (`clearCaptureDestination` in main.cpp) so a capture that
// never got here cannot leave the last run's frame either.
bool writeBmp(const char* path, uint32_t width, uint32_t height, const uint8_t* bgra) {
    if (!path || !bgra || width == 0 || height == 0) {
        return false;
    }
    const uint64_t pixelBytes = (uint64_t)width * (uint64_t)height * 4u;
    const uint64_t fileBytes = 54u + pixelBytes;
    if (fileBytes > UINT32_MAX) {
        return false;
    }

    const std::string temp = std::string(path) + ".part";
    FILE* file = std::fopen(temp.c_str(), "wb");
    if (!file) {
        std::printf("[vk] screenshot: could not open %s\n", path);
        return false;
    }

    uint8_t header[54] = {};
    header[0] = 'B';
    header[1] = 'M';
    const uint32_t fileSize = (uint32_t)fileBytes;
    header[2] = (uint8_t)(fileSize & 0xFFu);
    header[3] = (uint8_t)((fileSize >> 8) & 0xFFu);
    header[4] = (uint8_t)((fileSize >> 16) & 0xFFu);
    header[5] = (uint8_t)((fileSize >> 24) & 0xFFu);
    const uint32_t pixelOffset = 54u;
    header[10] = (uint8_t)(pixelOffset & 0xFFu);
    header[11] = (uint8_t)((pixelOffset >> 8) & 0xFFu);
    header[14] = 40u;
    header[18] = (uint8_t)(width & 0xFFu);
    header[19] = (uint8_t)((width >> 8) & 0xFFu);
    header[20] = (uint8_t)((width >> 16) & 0xFFu);
    header[21] = (uint8_t)((width >> 24) & 0xFFu);
    // A negative height stores the rows top-down.
    const uint32_t signedHeight = (uint32_t)-(int32_t)height;
    header[22] = (uint8_t)(signedHeight & 0xFFu);
    header[23] = (uint8_t)((signedHeight >> 8) & 0xFFu);
    header[24] = (uint8_t)((signedHeight >> 16) & 0xFFu);
    header[25] = (uint8_t)((signedHeight >> 24) & 0xFFu);
    header[26] = 1u;
    header[28] = 32u;

    bool ok = std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
    if (ok) {
        ok = std::fwrite(bgra, 1, (size_t)pixelBytes, file) == (size_t)pixelBytes;
    }
    if (std::fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        // The partial file is this function's own; the destination was never
        // opened, so whatever was there is still there and still complete.
        std::remove(temp.c_str());
        std::printf("[vk] screenshot: could not write %s\n", path);
        return false;
    }
    if (std::rename(temp.c_str(), path) != 0) {
        std::remove(temp.c_str());
        std::printf("[vk] screenshot: could not place the picture at %s (%s)\n", path,
                    std::strerror(errno));
        return false;
    }
    return true;
}

// Copy the swapchain image just presented into a host buffer and write it as
// a BMP. The copy is queued after the frame's present, so it is ordered after
// all of that frame's draws; its fence waits for the readback before the
// pixels are inspected.
bool captureSwapchain(RendererVk* r, const char* path) {
    if (!r || !path || r->ctx.extent.width == 0 || r->ctx.extent.height == 0) {
        return false;
    }
    const uint32_t width = r->ctx.extent.width;
    const uint32_t height = r->ctx.extent.height;
    const VkDeviceSize size = (VkDeviceSize)width * (VkDeviceSize)height * 4u;

    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    if (!createHostBuffer(&r->ctx, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          &buffer, &memory, &mapped)) {
        std::printf("[vk] screenshot: host buffer failed\n");
        return false;
    }

    const VkImage image = r->ctx.images[r->ctx.currentImage];
    VkCommandPool oneShotPool = VK_NULL_HANDLE;
    VkCommandBuffer cb = beginOneShotTracked(&r->ctx, &oneShotPool);
    if (!cb) {
        vkUnmapMemory(r->ctx.device, memory);
        vkDestroyBuffer(r->ctx.device, buffer, nullptr);
        vkFreeMemory(r->ctx.device, memory, nullptr);
        std::printf("[vk] screenshot: command buffer failed\n");
        return false;
    }

    VkImageMemoryBarrier toTransfer = {};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &toTransfer);

    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buffer, 1, &region);

    VkImageMemoryBarrier toPresent = toTransfer;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &toPresent);
    const OneShot shot = endOneShot(&r->ctx, oneShotPool, cb);
    if (shot == OneShot::unfenced) {
        // The copy may still be writing this buffer. Unmapping, destroying or freeing
        // it here is the use-after-free the refusal that has already printed said was
        // not safe (review-8 F1), so ownership is abandoned rather than released: a
        // screenshot is a debug artefact and a corrupted heap is not.
        refused("screenshot buffer of %llu bytes is abandoned, not freed: a submission "
                "that may still be running writes into it",
                (unsigned long long)size);
        return false;
    }
    if (shot != OneShot::done) {
        // `endOneShot` already printed why. A buffer nobody copied into is not a
        // screenshot: writing it out would publish a BMP of whatever the allocator
        // left in the mapping and print "wrote" over the top of it.
        vkUnmapMemory(r->ctx.device, memory);
        vkDestroyBuffer(r->ctx.device, buffer, nullptr);
        vkFreeMemory(r->ctx.device, memory, nullptr);
        return false;
    }

    const uint8_t* raw = static_cast<const uint8_t*>(mapped);
    std::vector<uint8_t> bgra((size_t)size);
    const bool rawIsBgra = r->ctx.swapchainFormat == VK_FORMAT_B8G8R8A8_UNORM ||
                           r->ctx.swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0; i < (size_t)size; i += 4) {
        if (rawIsBgra) {
            bgra[i] = raw[i];
            bgra[i + 1] = raw[i + 1];
            bgra[i + 2] = raw[i + 2];
        } else {
            bgra[i] = raw[i + 2];
            bgra[i + 1] = raw[i + 1];
            bgra[i + 2] = raw[i];
        }
        bgra[i + 3] = raw[i + 3];
    }

    const bool ok = writeBmp(path, width, height, bgra.data());
    vkUnmapMemory(r->ctx.device, memory);
    vkDestroyBuffer(r->ctx.device, buffer, nullptr);
    vkFreeMemory(r->ctx.device, memory, nullptr);
    if (ok) {
        std::printf("[vk] screenshot: wrote %s (%ux%u)\n", path, width, height);
    }
    return ok;
}

void destroyImage(mdk::vk::Context* ctx, VkImage* image, VkDeviceMemory* memory,
                  VkImageView* view, void** mapped) {
    if (*view) { vkDestroyImageView(ctx->device, *view, nullptr); *view = VK_NULL_HANDLE; }
    if (*image) { vkDestroyImage(ctx->device, *image, nullptr); *image = VK_NULL_HANDLE; }
    if (*memory) { vkFreeMemory(ctx->device, *memory, nullptr); *memory = VK_NULL_HANDLE; }
    (void)mapped;  // unmapped implicitly when the memory is freed
}

// ---------------------------------------------------------------------------
// Row-vector matrix product: result = a * b with p' = p * M.
// ---------------------------------------------------------------------------

void mulMatrix(const float a[16], const float b[16], float out[16]) {
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

// Flip clip.y, in the engine's row-major row-vector layout.
//
// The engine's projection is the C# oracle's, which is written for a clip
// space whose y runs UP. Vulkan's framebuffer runs y DOWN, so the C# gets an
// upright picture from SDL_GPU because SDL_GPU's Vulkan backend draws through a
// NEGATIVE viewport height. This backend cannot copy that: a negative viewport
// needs VK_EXT_negative_viewport_height, and the software driver this project
// validates against (lavapipe) does not advertise it. The two are the same
// framebuffer map, so the flip is folded into the matrix instead: right-multiply
// the row-vector matrix by diag(1,-1,1,1), which negates its column 1.
//
// The same map reverses the triangle winding, which is why the solid pipeline
// names CLOCKWISE as its front face (createPipeline).
void flipClipY(float m[16]) {
    for (int row = 0; row < 4; row++) {
        m[row * 4 + 1] = -m[row * 4 + 1];
    }
}

// The same flip applied from the LEFT (diag(1,-1,1,1) * m), which is what the
// sky's clip-to-direction needs: it is the INVERSE of the projection the clip
// points came from, so the flip that was on the right of the forward transform
// lands on the left of the inverse. In the row-major layout that negates row 1.
void flipClipYLeft(float m[16]) {
    for (int col = 0; col < 4; col++) {
        m[1 * 4 + col] = -m[1 * 4 + col];
    }
}

// ---------------------------------------------------------------------------
// Pipeline construction
// ---------------------------------------------------------------------------

VkShaderModule createModule(mdk::vk::Context* ctx, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytes;
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    // The result is checked because the caller's guard is `if (!module)`, and a
    // failed create leaves an out-parameter undefined rather than null.
    if (vkCreateShaderModule(ctx->device, &info, nullptr, &module) != VK_SUCCESS) {
        refused("shader module: the driver would not take these bytes; the caller names "
                "the stage and refuses to start the renderer");
        return VK_NULL_HANDLE;
    }
    return module;
}

// The C# CreatePipeline states, translated. key = pass*16 + blend*4 + prim.
VkPipeline createPipeline(RendererVk* r, int pass, int blend, int primitive) {
    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkVertexInputBindingDescription binding = {};
    binding.binding = 0;
    binding.stride = sizeof(mdk::Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[3] = {};
    attrs[0].location = 0; attrs[0].binding = 0; attrs[0].offset = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].location = 1; attrs[1].binding = 0; attrs[1].offset = 12;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[2].location = 2; attrs[2].binding = 0; attrs[2].offset = 20;
    attrs[2].format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = primitive ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = {};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    // The C#: cull BACK on Solid only; front faces wind clockwise ON SCREEN,
    // which in Vulkan's Y-up framebuffer is counter-clockwise.
    rs.cullMode = (pass == (int)mdk::Pass::Solid) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    // CLOCKWISE: the clip.y flip folded into the projection matrix (flipClipY)
    // reverses the winding the same way a negative viewport height would, and
    // the C# oracle's front faces wind clockwise on screen.
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms = {};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = {};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = (pass != (int)mdk::Pass::Overlay && pass != (int)mdk::Pass::OnTop)
                             ? VK_TRUE
                             : VK_FALSE;
    ds.depthWriteEnable = (pass != (int)mdk::Pass::Blended && pass != (int)mdk::Pass::Overlay &&
                           pass != (int)mdk::Pass::OnTop) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    const bool blended = (pass == (int)mdk::Pass::Blended || pass == (int)mdk::Pass::Overlay);
    VkPipelineColorBlendAttachmentState cb = {};
    cb.blendEnable = blended ? VK_TRUE : VK_FALSE;
    cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cb.dstColorBlendFactor = (blend == (int)mdk::Blend::Add) ? VK_BLEND_FACTOR_ONE
                                                             : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cb.colorBlendOp = VK_BLEND_OP_ADD;
    cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cb.alphaBlendOp = VK_BLEND_OP_ADD;
    cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbs = {};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;

    VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamics;

    VkGraphicsPipelineCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = r->vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = r->fragModule;
    stages[1].pName = "main";
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &cbs;
    info.pDynamicState = &dyn;
    info.layout = r->pipeLayout;
    info.renderPass = r->ctx.renderPass;
    info.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(r->ctx.device, r->pipeCache, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
        refused("pipeline for pass %d blend %d prim %d: every draw that needs it is "
                "dropped, so a whole material can vanish from the frame",
                pass, blend, primitive);
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

VkPipeline getPipeline(RendererVk* r, int pass, int blend, int primitive) {
    const int key = pass * 16 + blend * 4 + primitive;
    for (size_t i = 0; i < r->pipelines.size(); i++) {
        if (r->pipelines[i].key == key) {
            return r->pipelines[i].pipeline;
        }
    }
    VkPipeline p = createPipeline(r, pass, blend, primitive);
    PipelineVk entry;
    entry.key = key;
    entry.pipeline = p;
    r->pipelines.push_back(entry);
    return p;
}

// The sky pipeline: one screen-filling triangle, no vertex input, no cull, no
// depth (the C#'s "sky" pipeline: DoubleSided, Geometry.Screen).
VkPipeline createSkyPipeline(RendererVk* r) {
    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = {};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms = {};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = {};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;

    VkPipelineColorBlendAttachmentState cb = {};
    cb.blendEnable = VK_FALSE;
    cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbs = {};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;

    VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamics;

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = r->skyVertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = r->skyFragModule;
    stages[1].pName = "main";

    VkGraphicsPipelineCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &cbs;
    info.pDynamicState = &dyn;
    info.layout = r->pipeLayout;
    info.renderPass = r->ctx.renderPass;
    info.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(r->ctx.device, r->pipeCache, 1, &info, nullptr,
                                  &pipeline) != VK_SUCCESS) {
        refused("sky pipeline: the panorama is not drawn at all this run");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

// The descriptor set for one (texture, palette) pair, created on demand.
//
// THE SAMPLER IS PART OF THE PAIR, because the C# binds two different
// samplers (Renderer.cs:1098-1099): a surface's index texture is sampled
// with the REPEAT sampler and the palette with the CLAMP one. A surface's
// texture coordinates run past the texture's edge -- the arena's UVs are
// in texels, so a wall that shows the texture twice has a coordinate
// above 1 -- and the original TILES the texture there. Clamping instead
// stretches the edge texel over the rest of the surface, which is what
// the C++ used to do: it turned whole walls into flat brown.
//
// The sky is the exception: the C# binds the CLAMP sampler for the
// panorama as well (Renderer.cs:1029-1030), so the sky's set is made with
// repeat = false.
VkDescriptorSet getDescriptorSet(RendererVk* r, int32_t texture, int32_t palette, bool repeat) {
    for (size_t i = 0; i < r->descs.size(); i++) {
        if (r->descs[i].texture == texture && r->descs[i].palette == palette &&
            r->descs[i].repeat == repeat) {
            return r->descs[i].set;
        }
    }
    VkDescriptorSetAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = r->descPool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &r->setLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(r->ctx.device, &alloc, &set) != VK_SUCCESS) {
        // The cache returns sets to this pool (`forgetDescriptorSets` retires,
        // `settleRetired` frees), so a dry pool is no longer a leak — but it is
        // NOT only `descs.size()` that spends the 4096: a retired set holds its
        // slot until the frame boundary frees it, so the churn that dries the
        // pool is live sets PLUS the ones awaiting that boundary (review-9 F3).
        // The callers do NOT draw without a set: a draw with the previous draw's
        // set bound is a WRONG picture, which is worse than the missing one every
        // other refusal here produces.
        refused("descriptor set for texture %d palette %d (%s sampler): the draw that "
                "asked for it is skipped rather than sampled with the previous set",
                (int)texture, (int)palette, repeat ? "REPEAT" : "CLAMP");
        return VK_NULL_HANDLE;
    }

    VkDescriptorImageInfo texInfo = {};
    texInfo.sampler = repeat ? r->samplerRepeat : r->sampler;
    texInfo.imageView = (texture >= 0 && texture < (int32_t)r->textures.size())
                            ? r->textures[texture].view : VK_NULL_HANDLE;
    texInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo palInfo = {};
    // The palette is always clamped: the C# binds the CLAMP sampler with
    // it, and it is fetched by texel, so its address mode is moot.
    palInfo.sampler = r->sampler;
    palInfo.imageView = (palette >= 0 && palette < (int32_t)r->palettes.size())
                            ? r->palettes[palette].view : VK_NULL_HANDLE;
    palInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &texInfo;
    writes[1] = writes[0];
    writes[1].dstBinding = 1;
    writes[1].pImageInfo = &palInfo;
    vkUpdateDescriptorSets(r->ctx.device, 2, writes, 0, nullptr);

    DescPair pair;
    pair.texture = texture;
    pair.palette = palette;
    pair.repeat = repeat;
    pair.set = set;
    r->descs.push_back(pair);
    return set;
}

// Forget the descriptor sets that name a texture or palette whose image
// view is about to be replaced or destroyed.
//
// A descriptor set holds the image view, not the texture id. If the view is
// destroyed and the id is then reused, a cached set keyed by the id would
// still name the destroyed view. The cache must therefore lose every set
// that names the changed resource before the change happens.
void forgetDescriptorSets(RendererVk* r, int32_t texture, int32_t palette) {
    size_t i = 0;
    while (i < r->descs.size()) {
        if ((texture >= 0 && r->descs[i].texture == texture) ||
            (palette >= 0 && r->descs[i].palette == palette)) {
            // Retire it, do not just forget it. The entry has to leave the cache
            // here — that is review-25's fix, and a recreated id must not inherit
            // a set naming the dead view — but the SET is still allocated, and
            // until this batch nothing ever handed it back, so the cache was
            // correct and the pool was leaking in proportion to the churn.
            retireDescriptorSet(r, r->descs[i].set);
            r->descs[i] = r->descs.back();
            r->descs.pop_back();
            continue;
        }
        i++;
    }
}

// The sky: one screen-filling triangle, drawn first, before the level. The
// clip-to-direction matrix is the inverse of (rotation * projection) in the
// row-vector convention, uploaded verbatim; the sky shader multiplies it on
// the left of the clip point, which GLSL's column-major mat4 turns back into
// the row-vector product (the C#'s mul(clip_to_direction, v)).
void drawSky(RendererVk* r) {
    if (r->panorama.sky < 0 || r->panorama.palette < 0) {
        return;
    }
    if (!r->skyPipeline) {
        r->skyPipeline = createSkyPipeline(r);
        if (!r->skyPipeline) {
            return;
        }
    }

    // The C#'s sky rotation is CreateLookAt(Zero, camera.Forward, camera.Up):
    // the exact camera axes, NOT the view matrix's. The view matrix recomputes
    // `position + forward - position`, and the position's float cancellation
    // changes the forward ULP. Use the camera's stored exact axes.
    const mdk::Vec3 forward = r->camera.forward;
    const mdk::Vec3 up = r->camera.up;
    // System.Numerics normalizes EVERY axis it derives, including zaxis =
    // normalize(eye - target). `eye` is Zero here so the subtraction is exact,
    // but `forward` is itself the result of a normalize and its length is not
    // exactly one, so the division is a real last-bit step and not a no-op.
    // Normalize divides by the length; it does not multiply by a reciprocal.
    mdk::Vec3 zaxis{-forward.x, -forward.y, -forward.z};
    const float zLen = sqrtf(zaxis.x * zaxis.x + zaxis.y * zaxis.y + zaxis.z * zaxis.z);
    if (zLen <= 0.0f) {
        return;
    }
    zaxis.x /= zLen;
    zaxis.y /= zLen;
    zaxis.z /= zLen;
    mdk::Vec3 right{up.y * zaxis.z - up.z * zaxis.y,
                    up.z * zaxis.x - up.x * zaxis.z,
                    up.x * zaxis.y - up.y * zaxis.x};
    const float rightLen = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
    if (rightLen <= 0.0f) {
        return;
    }
    right.x /= rightLen;
    right.y /= rightLen;
    right.z /= rightLen;
    const mdk::Vec3 trueUp{zaxis.y * right.z - zaxis.z * right.y,
                           zaxis.z * right.x - zaxis.x * right.z,
                           zaxis.x * right.y - zaxis.y * right.x};
    float rotation[16];
    rotation[0] = right.x;   rotation[1] = trueUp.x;  rotation[2] = zaxis.x;  rotation[3] = 0.0f;
    rotation[4] = right.y;   rotation[5] = trueUp.y;  rotation[6] = zaxis.y;  rotation[7] = 0.0f;
    rotation[8] = right.z;   rotation[9] = trueUp.z;  rotation[10] = zaxis.z; rotation[11] = 0.0f;
    rotation[12] = 0.0f;     rotation[13] = 0.0f;     rotation[14] = 0.0f;    rotation[15] = 1.0f;
    float rp[16];
    mulMatrix(rotation, r->activeCamera.projection, rp);
    float c2d[16];
    if (!mdk::invertSystemNumericsMatrix4(rp, c2d)) {
        return;
    }
    // The clip points this matrix turns into rays went through the flipped
    // projection (flipClipY), so the inverse carries the flip on its other
    // side.
    flipClipYLeft(c2d);

    SkyPush sp = {};
    // Uploaded verbatim, exactly as the C# pushes its clip_to_direction: the
    // shader's column-major mat4 is this row-major matrix transposed, and the
    // shader's `M * v` is the row-vector product the C#'s mul(M, v) computes.
    memcpy(sp.clip_to_direction, c2d, sizeof(c2d));
    sp.wrap_width = r->panorama.wrap_width;
    sp.horizon_row = r->panorama.horizon_row;
    sp.offset = r->panorama.offset;
    sp.height = r->panorama.height;
    sp.top_colour = (int32_t)r->panorama.top_colour;
    sp.bottom_colour = (int32_t)r->panorama.bottom_colour;
    sp.row_shift = 0.0f;
    sp.sampling = r->panorama.linear ? 1 : 0;

    VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->skyPipeline);
    vkCmdPushConstants(cb, r->pipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(SkyPush), &sp);
    // The sky's panorama is clamped, like the C#'s (Renderer.cs:1029).
    VkDescriptorSet set = getDescriptorSet(r, r->panorama.sky, r->panorama.palette, false);
    if (!set) {
        return;  // no set means no draw, not a stale one: see drawRecorded
    }
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeLayout, 0, 1, &set, 0,
                            nullptr);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

// ---------------------------------------------------------------------------
// Resource pool helpers
// ---------------------------------------------------------------------------

template <typename T>
int32_t acquireSlot(std::vector<T>& pool) {
    for (size_t i = 0; i < pool.size(); i++) {
        if (!pool[i].used) {
            pool[i] = T{};
            pool[i].used = true;
            return (int32_t)i;
        }
    }
    pool.push_back(T{});
    pool.back().used = true;
    return (int32_t)pool.size() - 1;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

MdkRenderer mdk_render_create(const MdkRenderDesc* desc) {
    if (!desc) {
        return nullptr;
    }

    // volk loads the Vulkan entry points through the loader. Doing this
    // before anything else means a missing loader is a message, not a
    // crash inside a dispatch.
    if (volkInitialize() != VK_SUCCESS) {
        std::printf("[vk] volkInitialize failed: the Vulkan loader could not be loaded.\n");
        return nullptr;
    }

    RendererVk* r = new RendererVk();
    r->stereo = desc->stereo;
    r->enhancedLook = desc->enhanced_look;
    r->toggles = desc->enhanced_toggles;
    r->activeCamera = r->camera;

    // The surface creator. It is a function pointer so that
    // vk_context.cpp -- which owns the device, the swapchain and the
    // per-frame resources -- does not have to know about SDL.
    if (!desc->headless) {
        r->ctx.extensionProvider = [](const char* const** outNames) -> uint32_t {
            uint32_t count = 0;
            const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
            if (outNames) {
                *outNames = names;
            }
            return count;
        };
    }

    r->ctx.surfaceCreator = [](VkInstance instance, void* window, VkSurfaceKHR* outSurface) -> bool {
        if (!window) {
            return false;
        }
        return SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(window), instance, nullptr, outSurface);
    };

    if (!mdk::vk::createInstance(&r->ctx, desc->validation)) {
        delete r;
        return nullptr;
    }

    const bool surfaceOk = desc->headless
        ? mdk::vk::createHeadlessSurface(&r->ctx)
        : mdk::vk::createSurface(&r->ctx, desc->surface);
    if (!surfaceOk) {
        mdk::vk::destroyContext(&r->ctx);
        delete r;
        return nullptr;
    }

    int width = desc->width;
    int height = desc->height;
    if (desc->headless) {
        width = desc->hidden_width;
        height = desc->hidden_height;
    } else if (desc->surface) {
        SDL_GetWindowSize(static_cast<SDL_Window*>(desc->surface), &width, &height);
    }
    if (!mdk::vk::createDevice(&r->ctx, (uint32_t)width, (uint32_t)height, desc->vsync)) {
        mdk::vk::destroyContext(&r->ctx);
        delete r;
        return nullptr;
    }

    // ---- the palette pipeline's objects -----------------------------------

    // The descriptor set layout: binding 0 the surface texture, binding 1
    // the palette. The C# binds them in space2 at t0/t1; the GLSL port uses
    // plain bindings 0 and 1.
    {
        VkDescriptorSetLayoutBinding bindings[2] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(r->ctx.device, &layoutInfo, nullptr, &r->setLayout) != VK_SUCCESS) {
            mdk::vk::destroyContext(&r->ctx);
            delete r;
            return nullptr;
        }
    }

    {
        VkDescriptorPoolSize sizes[1] = {};
        sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[0].descriptorCount = 8192;
        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        // The cache RETURNS sets to this pool now (see `settleRetired`), and
        // `vkFreeDescriptorSets` is invalid use on a pool that did not ask for
        // this bit. The alternative was `vkResetDescriptorPool`, which takes
        // every set in the pool with it — including the ones a frame that is
        // still on the queue has bound — so there is no safe point for it in a
        // frame loop.
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 4096;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(r->ctx.device, &poolInfo, nullptr, &r->descPool) != VK_SUCCESS) {
            mdk::vk::destroyContext(&r->ctx);
            delete r;
            return nullptr;
        }
    }

    {
        VkPushConstantRange range = {};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        range.offset = 0;
        range.size = sizeof(PushConstants);
        VkPipelineLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &r->setLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &range;
        if (vkCreatePipelineLayout(r->ctx.device, &layoutInfo, nullptr, &r->pipeLayout) != VK_SUCCESS) {
            mdk::vk::destroyContext(&r->ctx);
            delete r;
            return nullptr;
        }
    }

    {
        VkPipelineCacheCreateInfo cacheInfo = {};
        cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        // A pipeline cache is an optimisation, so refusing it is not fatal — but the
        // handle must not stay undefined and reach vkCreateGraphicsPipelines, which is
        // the undefined-handle-into-the-next-call shape `vkAllocateCommandBuffers` had
        // (review-5 F1). A null cache is the spec's "no cache", so that is what a
        // failure leaves behind, and it is said out loud rather than absorbed.
        if (vkCreatePipelineCache(r->ctx.device, &cacheInfo, nullptr, &r->pipeCache) !=
            VK_SUCCESS) {
            r->pipeCache = VK_NULL_HANDLE;
            refused("pipeline cache: every pipeline this run builds is compiled from "
                    "scratch instead of reused, which is slower and nothing else");
        }
    }

    // The original look samples with NEAREST, and the C# has TWO samplers
    // (Renderer.cs:249-250): the REPEAT one for a surface's index texture
    // and the CLAMP one for the palette and the sky's panorama. The
    // sampler's address mode is the difference: a surface's texture
    // coordinates run past the texture's edge (the arena's UVs are in
    // texels), and the original TILES the texture there.
    {
        VkSamplerCreateInfo samplerInfo = {};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 0.0f;
        if (vkCreateSampler(r->ctx.device, &samplerInfo, nullptr, &r->sampler) != VK_SUCCESS) {
            refused("CLAMP sampler: every palette, sky and canvas sample in the run is "
                    "bound to a null sampler");
        }
    }
    {
        VkSamplerCreateInfo samplerInfo = {};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        // W repeats too: the C#'s CreateSampler sets all three axes to the
        // same mode (Renderer.cs:506-508). It is moot for a 2D view, and it
        // is written the same way so the two samplers differ ONLY in the
        // address mode the C# differs in.
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.maxLod = 0.0f;
        if (vkCreateSampler(r->ctx.device, &samplerInfo, nullptr, &r->samplerRepeat) !=
            VK_SUCCESS) {
            refused("REPEAT sampler: every surface in the run samples with a null sampler, "
                    "which is the tiled-edge look lost everywhere at once");
        }
    }

    r->vertModule = createModule(&r->ctx, spv_palette_vert, spv_palette_vert_size);
    r->fragModule = createModule(&r->ctx, spv_palette_frag, spv_palette_frag_size);
    if (!r->vertModule || !r->fragModule) {
        std::printf("[vk] shader modules failed -- were the shaders compiled?\n");
        mdk::vk::destroyContext(&r->ctx);
        delete r;
        return nullptr;
    }
    r->skyVertModule = createModule(&r->ctx, spv_sky_vert, spv_sky_vert_size);
    r->skyFragModule = createModule(&r->ctx, spv_sky_frag, spv_sky_frag_size);
    if (!r->skyVertModule || !r->skyFragModule) {
        std::printf("[vk] sky shader modules failed -- were the shaders compiled?\n");
        mdk::vk::destroyContext(&r->ctx);
        delete r;
        return nullptr;
    }

    r->ready = true;
    return r;
}

void destroyGammaTarget(RendererVk* r);

void destroyPostTargets(RendererVk* r) {
    VkDevice device = r->ctx.device;
    if (r->postSceneFramebuffer) vkDestroyFramebuffer(device, r->postSceneFramebuffer, nullptr);
    if (r->viewDepthFramebuffer) vkDestroyFramebuffer(device, r->viewDepthFramebuffer, nullptr);
    if (r->occlusionFramebuffer) vkDestroyFramebuffer(device, r->occlusionFramebuffer, nullptr);
    if (r->postFramebuffer) vkDestroyFramebuffer(device, r->postFramebuffer, nullptr);
    if (r->canvasFramebuffer) vkDestroyFramebuffer(device, r->canvasFramebuffer, nullptr);
    r->postSceneFramebuffer = r->viewDepthFramebuffer = r->occlusionFramebuffer =
        r->postFramebuffer = r->canvasFramebuffer = VK_NULL_HANDLE;
    if (r->postSceneLevel0) vkDestroyImageView(device, r->postSceneLevel0, nullptr);
    if (r->postSceneView) vkDestroyImageView(device, r->postSceneView, nullptr);
    if (r->postSceneImage) vkDestroyImage(device, r->postSceneImage, nullptr);
    if (r->postSceneMemory) vkFreeMemory(device, r->postSceneMemory, nullptr);
    if (r->viewDepthView) vkDestroyImageView(device, r->viewDepthView, nullptr);
    if (r->viewDepthImage) vkDestroyImage(device, r->viewDepthImage, nullptr);
    if (r->viewDepthMemory) vkFreeMemory(device, r->viewDepthMemory, nullptr);
    if (r->occlusionView) vkDestroyImageView(device, r->occlusionView, nullptr);
    if (r->occlusionImage) vkDestroyImage(device, r->occlusionImage, nullptr);
    if (r->occlusionMemory) vkFreeMemory(device, r->occlusionMemory, nullptr);
    r->postSceneLevel0 = r->postSceneView = VK_NULL_HANDLE;
    r->postSceneImage = VK_NULL_HANDLE;
    r->postSceneMemory = VK_NULL_HANDLE;
    r->viewDepthView = VK_NULL_HANDLE;
    r->viewDepthImage = VK_NULL_HANDLE;
    r->viewDepthMemory = VK_NULL_HANDLE;
    r->occlusionView = VK_NULL_HANDLE;
    r->occlusionImage = VK_NULL_HANDLE;
    r->occlusionMemory = VK_NULL_HANDLE;
    r->postWidth = 0;
    r->postHeight = 0;
    r->postMips = 0;
}


void mdk_render_destroy(MdkRenderer rr) {
    RendererVk* r = asVk(rr);
    if (!r) {
        return;
    }
    if (vkDeviceWaitIdle(r->ctx.device) != VK_SUCCESS) {
        // There is nothing to do with the information except record it: the
        // alternative to destroying these resources is leaking them, and the
        // process is going away either way. What must not happen is a teardown
        // that could not fence its own destroys and says nothing about it (P3).
        refused("teardown wait for the device failed: the destroys that follow are not "
                "fenced against a frame that may still be running");
    }
    for (size_t i = 0; i < r->pipelines.size(); i++) {
        vkDestroyPipeline(r->ctx.device, r->pipelines[i].pipeline, nullptr);
    }
    for (size_t i = 0; i < r->textures.size(); i++) {
        if (r->textures[i].used) {
            destroyImage(&r->ctx, &r->textures[i].image, &r->textures[i].memory,
                         &r->textures[i].view, &r->textures[i].mapped);
        }
    }
    for (size_t i = 0; i < r->palettes.size(); i++) {
        if (r->palettes[i].used) {
            destroyImage(&r->ctx, &r->palettes[i].image, &r->palettes[i].memory,
                         &r->palettes[i].view, &r->palettes[i].mapped);
        }
    }
    for (size_t i = 0; i < r->meshes.size(); i++) {
        if (r->meshes[i].used) {
            for (uint32_t slot = 0; slot < mdk::vk::kFramesInFlight; slot++) {
                destroyBufferPair(r->ctx.device, r->meshes[i].buffer[slot],
                                  r->meshes[i].memory[slot]);
            }
        }
    }
    for (size_t i = 0; i < r->retiring.size(); i++) {
        destroyBufferPair(r->ctx.device, r->retiring[i].buffer, r->retiring[i].memory);
    }
    r->retiring.clear();
    // The retired images, drained rather than settled: the device is idle here, so
    // their boundary has already passed. Draining them is not optional — an image
    // still in this vector at this point has no other owner, and the textures and
    // palettes loops above cannot see it because their fields were nulled at the
    // retire so that this drain would be the only one.
    for (size_t i = 0; i < r->retiringImages.size(); i++) {
        VkImage image = r->retiringImages[i].image;
        VkDeviceMemory memory = r->retiringImages[i].memory;
        VkImageView view = r->retiringImages[i].view;
        destroyImage(&r->ctx, &image, &memory, &view, nullptr);
    }
    r->retiringImages.clear();
    // The canvas ring. The device is idle here, so these are destroyed and not
    // retired. (They were never freed at all before; the leak was invisible
    // because it happened once, at exit.)
    for (uint32_t slot = 0; slot < mdk::vk::kFramesInFlight; slot++) {
        destroyBufferPair(r->ctx.device, r->canvasBuffer[slot], r->canvasMemory[slot]);
    }
    if (r->vertModule) vkDestroyShaderModule(r->ctx.device, r->vertModule, nullptr);
    if (r->fragModule) vkDestroyShaderModule(r->ctx.device, r->fragModule, nullptr);
    if (r->skyPipeline) vkDestroyPipeline(r->ctx.device, r->skyPipeline, nullptr);
    if (r->skyVertModule) vkDestroyShaderModule(r->ctx.device, r->skyVertModule, nullptr);
    if (r->skyFragModule) vkDestroyShaderModule(r->ctx.device, r->skyFragModule, nullptr);
    if (r->stereoPipeline) vkDestroyPipeline(r->ctx.device, r->stereoPipeline, nullptr);
    if (r->stereoPipeLayout) vkDestroyPipelineLayout(r->ctx.device, r->stereoPipeLayout, nullptr);
    if (r->stereoSetLayout) vkDestroyDescriptorSetLayout(r->ctx.device, r->stereoSetLayout, nullptr);
    if (r->stereoPool) vkDestroyDescriptorPool(r->ctx.device, r->stereoPool, nullptr);
    if (r->eyeSampler) vkDestroySampler(r->ctx.device, r->eyeSampler, nullptr);
    if (r->stereoVertModule) vkDestroyShaderModule(r->ctx.device, r->stereoVertModule, nullptr);
    if (r->stereoFragModule) vkDestroyShaderModule(r->ctx.device, r->stereoFragModule, nullptr);
    for (size_t i = 0; i < r->enhancedPipelines.size(); i++) {
        if (r->enhancedPipelines[i].pipeline) {
            vkDestroyPipeline(r->ctx.device, r->enhancedPipelines[i].pipeline, nullptr);
        }
    }
    for (size_t i = 0; i < r->colours.size(); i++) {
        retireImage(r, r->colours[i].image, r->colours[i].memory, r->colours[i].view);
    }
    if (r->dummyColourView) {
        retireImage(r, r->dummyColourImage, r->dummyColourMemory, r->dummyColourView);
    }
    if (r->enhancedPipeLayout) vkDestroyPipelineLayout(r->ctx.device, r->enhancedPipeLayout, nullptr);
    if (r->enhancedSetLayout) vkDestroyDescriptorSetLayout(r->ctx.device, r->enhancedSetLayout, nullptr);
    if (r->colourSampler) vkDestroySampler(r->ctx.device, r->colourSampler, nullptr);
    if (r->enhancedVertModule) vkDestroyShaderModule(r->ctx.device, r->enhancedVertModule, nullptr);
    if (r->enhancedFragModule) vkDestroyShaderModule(r->ctx.device, r->enhancedFragModule, nullptr);
    for (uint32_t i = 0; i < mdk::vk::kFramesInFlight; i++) {
        if (r->enhancedPools[i]) vkDestroyDescriptorPool(r->ctx.device, r->enhancedPools[i], nullptr);
        destroyBufferPair(r->ctx.device, r->uniformRing[i], r->uniformRingMemory[i]);
        destroyBufferPair(r->ctx.device, r->lightBuffers[i], r->lightMemory[i]);
    }
    if (r->depthPipelines[0]) vkDestroyPipeline(r->ctx.device, r->depthPipelines[0], nullptr);
    if (r->depthPipelines[1]) vkDestroyPipeline(r->ctx.device, r->depthPipelines[1], nullptr);
    if (r->depthFragModule) vkDestroyShaderModule(r->ctx.device, r->depthFragModule, nullptr);
    if (r->shadowFramebuffer) vkDestroyFramebuffer(r->ctx.device, r->shadowFramebuffer, nullptr);
    if (r->shadowView) vkDestroyImageView(r->ctx.device, r->shadowView, nullptr);
    if (r->shadowImage) vkDestroyImage(r->ctx.device, r->shadowImage, nullptr);
    if (r->shadowMemory) vkFreeMemory(r->ctx.device, r->shadowMemory, nullptr);
    if (r->shadowPass) vkDestroyRenderPass(r->ctx.device, r->shadowPass, nullptr);
    destroyPostTargets(r);
    if (r->canvasLoadPass) vkDestroyRenderPass(r->ctx.device, r->canvasLoadPass, nullptr);
    if (r->occlusionPipeline) vkDestroyPipeline(r->ctx.device, r->occlusionPipeline, nullptr);
    if (r->postPipeline) vkDestroyPipeline(r->ctx.device, r->postPipeline, nullptr);
    if (r->postPipeLayout) vkDestroyPipelineLayout(r->ctx.device, r->postPipeLayout, nullptr);
    if (r->postSetLayout) vkDestroyDescriptorSetLayout(r->ctx.device, r->postSetLayout, nullptr);
    if (r->postPool) vkDestroyDescriptorPool(r->ctx.device, r->postPool, nullptr);
    if (r->postMipSampler) vkDestroySampler(r->ctx.device, r->postMipSampler, nullptr);
    if (r->postClampSampler) vkDestroySampler(r->ctx.device, r->postClampSampler, nullptr);
    if (r->postVertModule) vkDestroyShaderModule(r->ctx.device, r->postVertModule, nullptr);
    if (r->occlusionFragModule) vkDestroyShaderModule(r->ctx.device, r->occlusionFragModule, nullptr);
    if (r->postFragModule) vkDestroyShaderModule(r->ctx.device, r->postFragModule, nullptr);
    for (uint32_t i = 0; i < mdk::vk::kFramesInFlight; i++) {
        destroyBufferPair(r->ctx.device, r->postUniformBuffers[i], r->postUniformMemory[i]);
    }
    destroyGammaTarget(r);
    if (r->gammaPipeline) vkDestroyPipeline(r->ctx.device, r->gammaPipeline, nullptr);
    if (r->gammaPipeLayout) vkDestroyPipelineLayout(r->ctx.device, r->gammaPipeLayout, nullptr);
    if (r->gammaSetLayout) vkDestroyDescriptorSetLayout(r->ctx.device, r->gammaSetLayout, nullptr);
    if (r->gammaPool) vkDestroyDescriptorPool(r->ctx.device, r->gammaPool, nullptr);
    if (r->gammaVertModule) vkDestroyShaderModule(r->ctx.device, r->gammaVertModule, nullptr);
    if (r->gammaFragModule) vkDestroyShaderModule(r->ctx.device, r->gammaFragModule, nullptr);
    if (r->sampler) vkDestroySampler(r->ctx.device, r->sampler, nullptr);
    if (r->samplerRepeat) vkDestroySampler(r->ctx.device, r->samplerRepeat, nullptr);
    if (r->pipeCache) vkDestroyPipelineCache(r->ctx.device, r->pipeCache, nullptr);
    if (r->pipeLayout) vkDestroyPipelineLayout(r->ctx.device, r->pipeLayout, nullptr);
    if (r->descPool) vkDestroyDescriptorPool(r->ctx.device, r->descPool, nullptr);
    if (r->lostDescriptorSets) {
        // Printed here, once, rather than inside `settleRetired`'s refusal: that
        // helper dedups on the rendered sentence, so a count inside it is a new
        // sentence per frame and the flood B7 exists to prevent (review-9 F3/F4).
        // Teardown is the one place a running total cannot repeat.
        std::printf("[vk] %u descriptor set(s) were lost to failed frees and are "
                    "gone from the pool for good\n", (unsigned)r->lostDescriptorSets);
    }
    // No per-set free here, and none for `retiringSets`: destroying a descriptor
    // pool returns every set allocated from it, retired ones included, so a
    // second release of the same handle would be the double free this file has
    // been chasing one level down all along.
    if (r->setLayout) vkDestroyDescriptorSetLayout(r->ctx.device, r->setLayout, nullptr);
    mdk::vk::destroyContext(&r->ctx);
    delete r;
}

const char* mdk_render_backend_name(MdkRenderer r) {
    (void)r;
    return "Vulkan 1.3";
}

mdk::MdkCapabilities mdk_render_capabilities(MdkRenderer r) {
    (void)r;
    mdk::MdkCapabilities caps;
    // The Linux backend does EVERYTHING: the enhanced look, MSAA, mipmapped
    // colour textures, the flat stereo composites, the filtered canvas.
    caps.enhanced_look = true;
    caps.msaa = true;
    caps.colour_mips = true;
    caps.hardware_stereo = false;   // a flat display has no parallax barrier
    caps.flat_stereo = true;        // SBS / interlaced / TAB composites
    caps.insets = true;
    caps.canvas_filtering = true;
    caps.panorama_linear = true;
    caps.max_texture_size = 16384;
    caps.max_canvas_quads = 4096;
    // NO SPRITE RESIDENCY BOUND. The desktop has the memory for every frame
    // the C# keeps -- its texture cache is keyed by (name, frame) and
    // nothing ever evicts from it -- so the answer is 0, "no bound", and the
    // sprite cache keeps the C#'s behaviour. (The 3DS names a budget; see
    // renderer_c3d.cpp.)
    caps.sprite_residency_bytes = 0;
    return caps;
}

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

// The gamma grade's target, at the current extent (see the gamma fields).
void destroyGammaTarget(RendererVk* r) {
    VkDevice device = r->ctx.device;
    if (r->gammaFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, r->gammaFramebuffer, nullptr);
        r->gammaFramebuffer = VK_NULL_HANDLE;
    }
    if (r->gammaView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, r->gammaView, nullptr);
        r->gammaView = VK_NULL_HANDLE;
    }
    if (r->gammaImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, r->gammaImage, nullptr);
        r->gammaImage = VK_NULL_HANDLE;
    }
    if (r->gammaMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, r->gammaMemory, nullptr);
        r->gammaMemory = VK_NULL_HANDLE;
    }
    r->gammaWidth = 0;
    r->gammaHeight = 0;
}

// The gamma pipeline and its target, created on the first graded frame.
// The pipeline runs over the context's composite pass (colour only), so
// the eye targets (which own that pass) are ensured first, and the eye
// sampler is shared.
bool ensureGamma(RendererVk* r) {
    VkDevice device = r->ctx.device;
    if (!mdk::vk::ensureEyeTargets(&r->ctx)) {
        refused("gamma: the composite pass could not be created, so the frame "
                "is presented ungraded");
        return false;
    }
    if (!r->eyeSampler) {
        VkSamplerCreateInfo samplerInfo = {};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(device, &samplerInfo, nullptr, &r->eyeSampler) != VK_SUCCESS) {
            refused("gamma sampler: the frame is presented ungraded");
            return false;
        }
    }
    if (!r->gammaReady) {
        r->gammaVertModule = createModule(&r->ctx, spv_stereo_vert, spv_stereo_vert_size);
        r->gammaFragModule = createModule(&r->ctx, spv_gamma_frag, spv_gamma_frag_size);
        if (!r->gammaVertModule || !r->gammaFragModule) {
            refused("gamma shader modules: the frame is presented ungraded");
            return false;
        }

        VkDescriptorSetLayoutBinding binding = {};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &r->gammaSetLayout) !=
            VK_SUCCESS) {
            refused("gamma descriptor layout: the frame is presented ungraded");
            return false;
        }

        VkPushConstantRange range = {};
        range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        range.offset = 0;
        range.size = 16;
        VkPipelineLayoutCreateInfo pipeLayoutInfo = {};
        pipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeLayoutInfo.setLayoutCount = 1;
        pipeLayoutInfo.pSetLayouts = &r->gammaSetLayout;
        pipeLayoutInfo.pushConstantRangeCount = 1;
        pipeLayoutInfo.pPushConstantRanges = &range;
        if (vkCreatePipelineLayout(device, &pipeLayoutInfo, nullptr, &r->gammaPipeLayout) !=
            VK_SUCCESS) {
            refused("gamma pipeline layout: the frame is presented ungraded");
            return false;
        }

        VkDescriptorPoolSize poolSize = {};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = mdk::vk::kFramesInFlight;
        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = mdk::vk::kFramesInFlight;
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &r->gammaPool) != VK_SUCCESS) {
            refused("gamma descriptor pool: the frame is presented ungraded");
            return false;
        }
        for (uint32_t i = 0; i < mdk::vk::kFramesInFlight; i++) {
            VkDescriptorSetAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool = r->gammaPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts = &r->gammaSetLayout;
            if (vkAllocateDescriptorSets(device, &allocInfo, &r->gammaSets[i]) != VK_SUCCESS) {
                refused("gamma descriptor sets: the frame is presented ungraded");
                return false;
            }
        }

        VkPipelineVertexInputStateCreateInfo vi = {};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia = {};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp = {};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = {};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1.0f;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        VkPipelineMultisampleStateCreateInfo ms = {};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds = {};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_FALSE;
        ds.depthWriteEnable = VK_FALSE;
        ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendAttachmentState cb = {};
        cb.blendEnable = VK_FALSE;
        cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cbs = {};
        cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cbs.attachmentCount = 1;
        cbs.pAttachments = &cb;
        VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn = {};
        dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dynamics;
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = r->gammaVertModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = r->gammaFragModule;
        stages[1].pName = "main";
        VkGraphicsPipelineCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vi;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &vp;
        info.pRasterizationState = &rs;
        info.pMultisampleState = &ms;
        info.pDepthStencilState = &ds;
        info.pColorBlendState = &cbs;
        info.pDynamicState = &dyn;
        info.layout = r->gammaPipeLayout;
        info.renderPass = r->ctx.compositePass;
        info.subpass = 0;
        if (vkCreateGraphicsPipelines(device, r->pipeCache, 1, &info, nullptr,
                                      &r->gammaPipeline) != VK_SUCCESS) {
            refused("gamma pipeline: the frame is presented ungraded");
            return false;
        }
        r->gammaReady = true;
    }

    if (r->gammaWidth != r->ctx.extent.width || r->gammaHeight != r->ctx.extent.height) {
        destroyGammaTarget(r);
        VkImageCreateInfo imageInfo = {};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = r->ctx.swapchainFormat;
        imageInfo.extent.width = r->ctx.extent.width;
        imageInfo.extent.height = r->ctx.extent.height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &imageInfo, nullptr, &r->gammaImage) != VK_SUCCESS) {
            refused("gamma target image: the frame is presented ungraded");
            return false;
        }
        VkMemoryRequirements requirements = {};
        vkGetImageMemoryRequirements(device, r->gammaImage, &requirements);
        VkMemoryAllocateInfo allocateInfo = {};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = 0;
        VkPhysicalDeviceMemoryProperties properties = {};
        vkGetPhysicalDeviceMemoryProperties(r->ctx.physicalDevice, &properties);
        bool found = false;
        for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                allocateInfo.memoryTypeIndex = i;
                found = true;
                break;
            }
        }
        if (!found || vkAllocateMemory(device, &allocateInfo, nullptr, &r->gammaMemory) !=
                          VK_SUCCESS ||
            vkBindImageMemory(device, r->gammaImage, r->gammaMemory, 0) != VK_SUCCESS) {
            refused("gamma target memory: the frame is presented ungraded");
            destroyGammaTarget(r);
            return false;
        }
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = r->gammaImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = r->ctx.swapchainFormat;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &viewInfo, nullptr, &r->gammaView) != VK_SUCCESS) {
            refused("gamma target view: the frame is presented ungraded");
            destroyGammaTarget(r);
            return false;
        }
        VkImageView attachments[1] = {r->gammaView};
        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = r->ctx.compositePass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = attachments;
        fbInfo.width = r->ctx.extent.width;
        fbInfo.height = r->ctx.extent.height;
        fbInfo.layers = 1;
        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &r->gammaFramebuffer) != VK_SUCCESS) {
            refused("gamma framebuffer: the frame is presented ungraded");
            destroyGammaTarget(r);
            return false;
        }
        r->gammaWidth = r->ctx.extent.width;
        r->gammaHeight = r->ctx.extent.height;
    }
    return true;
}

// The gamma grade: sample the finished scene, write colour^(1/gamma) into
// the gamma target, and present that instead (Renderer.Gamma.cs). Runs
// with no render pass open; closes its own.
void drawGamma(RendererVk* r) {
    VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = r->ctx.sceneImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    VkDescriptorSet set = r->gammaSets[r->ctx.currentFrame];
    VkDescriptorImageInfo image = {};
    image.sampler = r->eyeSampler;
    image.imageView = r->ctx.sceneView;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(r->ctx.device, 1, &write, 0, nullptr);

    mdk::vk::beginPass(&r->ctx, r->gammaFramebuffer, r->ctx.compositePass, 0);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->gammaPipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->gammaPipeLayout,
                            0, 1, &set, 0, nullptr);
    const float uniforms[4] = {1.0f / r->gamma, 0.0f, 0.0f, 0.0f};
    vkCmdPushConstants(cb, r->gammaPipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(uniforms), uniforms);
    vkCmdDraw(cb, 3, 1, 0, 0);
    mdk::vk::endPass(&r->ctx);
    mdk::vk::setPresentImage(&r->ctx, r->gammaImage);
}

// One eye's camera, the C#'s View.Eye (Renderer.cs:92): the eyes are
// `separation` apart, parallel, and their images converge at `convergence`
// (<= 0: at infinity), which shifts the frustum sideways by
// -side * separation * near / (2 * convergence) in projection row 3.
// Full-side-by-side, crossview and VR eyes see half the frame's aspect, so
// their x scale doubles (StereoModes.EyeAspect).
mdk::Camera eyeCamera(RendererVk* r, float side) {
    mdk::Camera eye = r->camera;
    const mdk::Vec3 forward = r->camera.forward;
    const mdk::Vec3 up = r->camera.up;
    const float nearPlane = r->camera.near_plane;

    mdk::Vec3 right{forward.y * up.z - forward.z * up.y,
                    forward.z * up.x - forward.x * up.z,
                    forward.x * up.y - forward.y * up.x};
    const float rightLen = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
    if (rightLen <= 0.0f) {
        return eye;
    }
    right.x /= rightLen;
    right.y /= rightLen;
    right.z /= rightLen;

    const float offset = side * r->stereo.separation * 0.5f;
    const mdk::Vec3 position{r->camera.position.x + right.x * offset,
                             r->camera.position.y + right.y * offset,
                             r->camera.position.z + right.z * offset};

    // CreateLookAt(position, position + forward, up), spelt as the core
    // camera builder spells it (the axes are the view matrix's columns).
    mdk::Vec3 zaxis{-forward.x, -forward.y, -forward.z};
    const float zLen = sqrtf(zaxis.x * zaxis.x + zaxis.y * zaxis.y + zaxis.z * zaxis.z);
    if (zLen <= 0.0f) {
        return eye;
    }
    zaxis.x /= zLen;
    zaxis.y /= zLen;
    zaxis.z /= zLen;
    mdk::Vec3 xaxis{up.y * zaxis.z - up.z * zaxis.y,
                    up.z * zaxis.x - up.x * zaxis.z,
                    up.x * zaxis.y - up.y * zaxis.x};
    const float xLen = sqrtf(xaxis.x * xaxis.x + xaxis.y * xaxis.y + xaxis.z * xaxis.z);
    if (xLen <= 0.0f) {
        return eye;
    }
    xaxis.x /= xLen;
    xaxis.y /= xLen;
    xaxis.z /= xLen;
    const mdk::Vec3 yaxis{zaxis.y * xaxis.z - zaxis.z * xaxis.y,
                          zaxis.z * xaxis.x - zaxis.x * xaxis.z,
                          zaxis.x * xaxis.y - zaxis.y * xaxis.x};

    float* view = eye.view;
    view[0] = xaxis.x;  view[1] = yaxis.x;  view[2] = zaxis.x;  view[3] = 0.0f;
    view[4] = xaxis.y;  view[5] = yaxis.y;  view[6] = zaxis.y;  view[7] = 0.0f;
    view[8] = xaxis.z;  view[9] = yaxis.z;  view[10] = zaxis.z; view[11] = 0.0f;
    view[12] = -(xaxis.x * position.x + xaxis.y * position.y + xaxis.z * position.z);
    view[13] = -(yaxis.x * position.x + yaxis.y * position.y + yaxis.z * position.z);
    view[14] = -(zaxis.x * position.x + zaxis.y * position.y + zaxis.z * position.z);
    view[15] = 1.0f;
    eye.position = position;

    if (r->stereo.mode == mdk::StereoMode::SbsFull ||
        r->stereo.mode == mdk::StereoMode::CrossView ||
        r->stereo.mode == mdk::StereoMode::Vr) {
        eye.projection[0] *= 2.0f;
    }
    if (r->stereo.convergence > 0.0f && nearPlane > 0.0f) {
        const float shift = -side * r->stereo.separation * nearPlane /
                            (2.0f * r->stereo.convergence);
        eye.projection[8] += shift * eye.projection[0] / nearPlane;
    }
    return eye;
}

// The stereo composite pipeline: one screen-filling triangle sampling the
// two eye targets into the scene target (the C#'s "stereo" pipeline).
VkPipeline createStereoPipeline(RendererVk* r) {
    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = {};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms = {};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = {};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;

    VkPipelineColorBlendAttachmentState cb = {};
    cb.blendEnable = VK_FALSE;
    cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbs = {};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;

    VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamics;

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = r->stereoVertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = r->stereoFragModule;
    stages[1].pName = "main";

    VkGraphicsPipelineCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &cbs;
    info.pDynamicState = &dyn;
    info.layout = r->stereoPipeLayout;
    info.renderPass = r->ctx.compositePass;
    info.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(r->ctx.device, r->pipeCache, 1, &info, nullptr,
                                  &pipeline) != VK_SUCCESS) {
        refused("stereo composite pipeline: the frame is presented without its "
                "second eye composited");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

// The composite's Vulkan objects, created on the first stereo frame. The
// eye targets themselves live in the context (ensureEyeTargets).
bool ensureStereo(RendererVk* r) {
    if (r->stereoReady) {
        return true;
    }
    VkDevice device = r->ctx.device;

    r->stereoVertModule = createModule(&r->ctx, spv_stereo_vert, spv_stereo_vert_size);
    r->stereoFragModule = createModule(&r->ctx, spv_stereo_frag, spv_stereo_frag_size);
    if (!r->stereoVertModule || !r->stereoFragModule) {
        refused("stereo shader modules: the frame is presented without stereo");
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[2] = {};
    for (int i = 0; i < 2; i++) {
        bindings[i].binding = (uint32_t)i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &r->stereoSetLayout) !=
        VK_SUCCESS) {
        refused("stereo descriptor layout: the frame is presented without stereo");
        return false;
    }

    VkPushConstantRange range = {};
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.offset = 0;
    range.size = 16;
    VkPipelineLayoutCreateInfo pipeLayoutInfo = {};
    pipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeLayoutInfo.setLayoutCount = 1;
    pipeLayoutInfo.pSetLayouts = &r->stereoSetLayout;
    pipeLayoutInfo.pushConstantRangeCount = 1;
    pipeLayoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device, &pipeLayoutInfo, nullptr, &r->stereoPipeLayout) !=
        VK_SUCCESS) {
        refused("stereo pipeline layout: the frame is presented without stereo");
        return false;
    }

    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &r->eyeSampler) != VK_SUCCESS) {
        refused("stereo eye sampler: the frame is presented without stereo");
        return false;
    }

    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 2 * mdk::vk::kFramesInFlight;
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = mdk::vk::kFramesInFlight;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &r->stereoPool) != VK_SUCCESS) {
        refused("stereo descriptor pool: the frame is presented without stereo");
        return false;
    }
    for (uint32_t i = 0; i < mdk::vk::kFramesInFlight; i++) {
        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = r->stereoPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &r->stereoSetLayout;
        if (vkAllocateDescriptorSets(device, &allocInfo, &r->stereoSets[i]) != VK_SUCCESS) {
            refused("stereo descriptor sets: the frame is presented without stereo");
            return false;
        }
    }

    r->stereoPipeline = createStereoPipeline(r);
    if (!r->stereoPipeline) {
        return false;
    }
    r->stereoReady = true;
    return true;
}

// Bind this frame slot's composite set to the context's current eye views.
// Rewritten every frame: the eye targets are recreated on a swapchain
// rebuild, and a set naming a destroyed view is the stale-descriptor defect.
void updateStereoSet(RendererVk* r) {
    VkDescriptorSet set = r->stereoSets[r->ctx.currentFrame];
    VkDescriptorImageInfo images[2] = {};
    VkWriteDescriptorSet writes[2] = {};
    for (int eye = 0; eye < 2; eye++) {
        images[eye].sampler = r->eyeSampler;
        images[eye].imageView = r->ctx.eyeView[eye];
        images[eye].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[eye].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[eye].dstSet = set;
        writes[eye].dstBinding = (uint32_t)eye;
        writes[eye].descriptorCount = 1;
        writes[eye].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[eye].pImageInfo = &images[eye];
    }
    vkUpdateDescriptorSets(r->ctx.device, 2, writes, 0, nullptr);
}

// The composite draw: the two eyes into the scene target, the C#'s
// CompositeStereo uniforms (Renderer.Stereo.cs): x the layout (0 side by
// side, 1 interlaced, 2 top and bottom, 3 VR), y the eyes exchanged,
// z the frame aspect; w (this port's addition) the frame height, for the
// interlaced row parity the HLSL takes from SV_Position.
void drawComposite(RendererVk* r) {
    updateStereoSet(r);
    VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->stereoPipeline);
    VkDescriptorSet set = r->stereoSets[r->ctx.currentFrame];
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->stereoPipeLayout,
                            0, 1, &set, 0, nullptr);

    float layout = 0.0f;
    bool swapped = false;
    switch (r->stereo.mode) {
        case mdk::StereoMode::Interlaced: layout = 1.0f; break;
        case mdk::StereoMode::InterlacedReversed: layout = 1.0f; swapped = true; break;
        case mdk::StereoMode::Tab: layout = 2.0f; break;
        case mdk::StereoMode::TabReversed: layout = 2.0f; swapped = true; break;
        case mdk::StereoMode::Vr: layout = 3.0f; break;
        case mdk::StereoMode::CrossView: swapped = true; break;
        default: break;
    }
    const float aspect = r->ctx.extent.height != 0
                             ? (float)r->ctx.extent.width / (float)r->ctx.extent.height
                             : 4.0f / 3.0f;
    const float mode[4] = {layout, swapped ? 1.0f : 0.0f, aspect,
                           (float)r->ctx.extent.height};
    vkCmdPushConstants(cb, r->stereoPipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(mode), mode);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

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

void mdk_render_begin_frame(MdkRenderer rr, const MdkFrameParams* params) {
    RendererVk* r = asVk(rr);
    if (!r || !r->ready) {
        return;
    }
    if (r->inFrame) {
        return;
    }

    uint32_t w = r->ctx.extent.width;
    uint32_t h = r->ctx.extent.height;

    const uint32_t clear = params ? params->clear_colour : 0xFF000000u;
    r->clearColour = clear;

    // The frame's first pass targets the scene target, or the left eye's
    // when stereo is on; the active camera follows the same choice. A
    // stereo resource failure falls back to the mono frame with a refusal,
    // never to a half-drawn pair of eyes.
    const bool stereoOn = r->stereo.mode != mdk::StereoMode::Off && ensureStereo(r) &&
                          mdk::vk::ensureEyeTargets(&r->ctx);
    if (stereoOn) {
        mdk::vk::setSceneOverride(&r->ctx, r->ctx.eyeFramebuffer[0]);
        r->activeCamera = eyeCamera(r, -1.0f);
    } else {
        if (r->stereo.mode != mdk::StereoMode::Off) {
            refused("stereo frame: the eye targets or composite could not be "
                    "created, so this frame is presented mono");
        }
        mdk::vk::setSceneOverride(&r->ctx, VK_NULL_HANDLE);
        r->activeCamera = r->camera;
    }
    mdk::vk::setPresentImage(&r->ctx, VK_NULL_HANDLE);
    if (r->enhancedLook && ensureEnhanced(r)) {
        // This frame slot's enhanced sets and uniform ring start empty.
        if (vkResetDescriptorPool(r->ctx.device, r->enhancedPools[r->ctx.currentFrame],
                                  0) != VK_SUCCESS) {
            refused("enhanced descriptor pool reset: this frame's lit draws "
                    "may be dropped at allocation");
        }
        r->uniformCursor = 0;
    }
    if (r->enhancedLook && !r->enhancedReady) {
        // Stated once per distinct request (refused dedups): without the
        // lit pipeline an enhanced frame falls back to the original look.
        // Never a silent substitution.
        refused("enhanced look: the lit pipeline could not be created, so "
                "this frame renders the original look");
    }
    // A FRAME THAT DID NOT BEGIN IS NOT A FRAME. `beginFrame` returns false when the
    // fence wait, the image acquire, the command-buffer reset or the buffer's begin
    // failed — every one of which was read by nobody until this batch — and the
    // behaviour here was a bare `return`: the frame is black, the build is green,
    // and nothing anywhere says why. This is the seam, so this is where it prints,
    // through the helper that remembers each distinct request so a device that
    // refuses every frame costs one line and not one per frame.
    if (!mdk::vk::beginFrame(&r->ctx, w, h, true, clear)) {
        refused("begin the frame at %ux%u: nothing was recorded and nothing will be "
                "presented this frame",
                w, h);
        return;
    }
    r->inFrame = true;
    r->drawCalls = 0;
    r->triangles = 0;
    r->screenshotPath = params && params->screenshot_path ? params->screenshot_path : "";

    // The frame's own number, then the buffers the previous frames left behind.
    // The order matters: `settleRetired` reads frameIndex, and the fence that
    // makes those buffers safe is the one vk::beginFrame just waited.
    r->frameIndex++;
    settleRetired(r);
    r->queue.reset();

    // The viewport and scissor are dynamic state; set them once per frame so
    // every pipeline shares them.
    VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)r->ctx.extent.width;
    viewport.height = (float)r->ctx.extent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor = {{0, 0}, {r->ctx.extent.width, r->ctx.extent.height}};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &scissor);

    // The scene is deferred under the enhanced look without stereo: the
    // shadow pass at end_frame reads the whole queue, so nothing draws
    // into this first pass and the sky waits for the replay.
    r->deferScene = r->enhancedLook && r->stereo.mode == mdk::StereoMode::Off;

    // The sky is drawn first, before the level (SPEC.md 6.3 step 2).
    if (!r->deferScene) {
        drawSky(r);
    }
}

void mdk_render_end_frame(MdkRenderer rr) {
    RendererVk* r = asVk(rr);
    if (!r || !r->inFrame) {
        return;
    }
    // The deferred enhanced frame: the first pass (begun by begin_frame)
    // holds nothing; close it, render the sun's depth from the whole
    // queue, then replay sky, scene and canvas into the scene target.
    bool deferredFrame = false;
    if (r->deferScene && r->enhancedReady) {
        r->replayingEyes = true;
        mdk::vk::endPass(&r->ctx);
        if (r->lighting.shadows && r->toggles.enableSkyShadows && ensureShadow(r)) {
            drawShadowMap(r);
        }
        // The post pass, when its targets and pipelines exist and the
        // camera's clip matrices invert: the scene then renders into
        // the mipmapped scene target and is post-processed into the
        // scene target, with the canvas over it (the oracle's order).
        // Without it the frame takes the pre-post path below, and the
        // refusal has already been printed by ensurePost.
        bool post = ensurePost(r);
        if (post && !mdk::postClipMatrices(r->activeCamera.forward, r->activeCamera.up,
                                           r->activeCamera.projection, r->postClipToView,
                                           r->postViewToClip)) {
            refused("post pass: the camera's clip matrices do not invert, so the "
                    "frame renders without occlusion and glow");
            post = false;
        }
        if (post) {
            drawCameraDepth(r);
        }
        mdk::vk::beginPass(&r->ctx,
                           post ? r->postSceneFramebuffer : r->ctx.sceneFramebuffer,
                           r->ctx.renderPass, r->clearColour);
        {
            VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
            VkViewport viewport = {};
            viewport.x = 0.0f;
            viewport.y = 0.0f;
            viewport.width = (float)r->ctx.extent.width;
            viewport.height = (float)r->ctx.extent.height;
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            VkRect2D scissor = {{0, 0}, r->ctx.extent};
            vkCmdSetViewport(cb, 0, 1, &viewport);
            vkCmdSetScissor(cb, 0, 1, &scissor);
        }
        drawSky(r);
        flushDraws(r);
        r->queue.reset();
        if (!post) {
            replayCanvas(r);
        }
        mdk::vk::endPass(&r->ctx);
        if (post) {
            generateSceneMips(r);
            drawPost(r);
            mdk::vk::beginPass(&r->ctx, r->canvasFramebuffer, r->canvasLoadPass, 0);
            replayCanvas(r);
            mdk::vk::endPass(&r->ctx);
        }
        r->replayingEyes = false;
        deferredFrame = true;
    }
    bool stereoFrame = false;
    if (r->stereo.mode != mdk::StereoMode::Off && r->ctx.eyesReady && r->stereoReady) {
        // THE STEREO FRAME: replay the queued draws through the right eye,
        // then composite the two eyes into the scene target, which the
        // shared submit blits to present. The left eye's pass (begun by
        // begin_frame, sky included) is still open here.
        r->replayingEyes = true;
        flushDraws(r);
        mdk::vk::endPass(&r->ctx);

        r->activeCamera = eyeCamera(r, 1.0f);
        mdk::vk::beginPass(&r->ctx, r->ctx.eyeFramebuffer[1], r->ctx.renderPass,
                           r->clearColour);
        drawSky(r);
        flushDraws(r);
        r->queue.reset();
        mdk::vk::endPass(&r->ctx);

        // Both eyes now hold finished frames in the render pass's final
        // layout; the composite samples them.
        VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
        VkImageMemoryBarrier eyeBarriers[2] = {};
        for (int eye = 0; eye < 2; eye++) {
            eyeBarriers[eye].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            eyeBarriers[eye].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            eyeBarriers[eye].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            eyeBarriers[eye].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            eyeBarriers[eye].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            eyeBarriers[eye].image = r->ctx.eyeImage[eye];
            eyeBarriers[eye].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            eyeBarriers[eye].subresourceRange.levelCount = 1;
            eyeBarriers[eye].subresourceRange.layerCount = 1;
            eyeBarriers[eye].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            eyeBarriers[eye].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        }
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 2, eyeBarriers);

        mdk::vk::beginPass(&r->ctx, r->ctx.compositeFramebuffer, r->ctx.compositePass, 0);
        drawComposite(r);
        mdk::vk::endPass(&r->ctx);
        r->replayingEyes = false;
        r->activeCamera = r->camera;

        stereoFrame = true;
    }
    // The gamma grade, when the display's gamma is not 1: the finished
    // scene (or the stereo composite) is graded into its own target and
    // that is presented instead.
    if ((stereoFrame || deferredFrame) && r->gamma != 1.0f && ensureGamma(r)) {
        drawGamma(r);
    }
    bool submitted = false;
    if (stereoFrame || deferredFrame) {
        submitted = mdk::vk::submitFrame(&r->ctx);
    }
    if (!stereoFrame && !deferredFrame && r->gamma != 1.0f && ensureGamma(r)) {
        flushDraws(r);
        mdk::vk::endPass(&r->ctx);
        drawGamma(r);
        submitted = mdk::vk::submitFrame(&r->ctx);
    } else if (!stereoFrame && !deferredFrame) {
        // The frame's queued draws go in now, in the oracle's pass order, while the
        // render pass is still open (draw_queue.h). A frontend that drew its HUD
        // already flushed at canvas_begin; this is the catch-all for a frame with no
        // canvas, and it must come before endFrame closes the render pass.
        flushDraws(r);
        // THE FRAME'S OWN RESULT WAS DISCARDED ENTIRELY. `endFrame` returns false when
        // the fence reset, the submit or the present failed; until now the frame was
        // reported as rendered either way, so a pinned frame whose commands never ran
        // would be measured, published and committed as if it were the picture.
        submitted = mdk::vk::endFrame(&r->ctx);
    }
    r->inFrame = false;
    if (!submitted) {
        refused("end the frame: its commands were not submitted or not presented, so this "
                "frame did not reach the display");
    }
    if (!r->screenshotPath.empty()) {
        const std::string path = r->screenshotPath;
        r->screenshotPath.clear();
        r->capturesRequested++;
        if (!submitted) {
            // The readback would succeed and print its usual "wrote N" line about an
            // image this frame never drew into. A frame record that cannot be trusted
            // is worse than no frame record, because a frame record gets committed.
            refused("capture the frame to %s: the frame it was asked to capture was never "
                    "submitted, so the image would not be this frame's",
                    path.c_str());
        } else if (captureSwapchain(r, path.c_str())) {
            // The one place this counter may rise: `captureSwapchain` returns false
            // for a failed open, a short write, a failed readback and a mapping it
            // could not make, and every one of those is a picture that does not exist.
            r->capturesWritten++;
        }
    }
}

void mdk_render_present(MdkRenderer rr) {
    // The present IS the frame end on this backend too.
    (void)rr;
}

void mdk_render_set_camera(MdkRenderer rr, const mdk::Camera* cam) {
    RendererVk* r = asVk(rr);
    if (!r || !cam) {
        return;
    }
    r->camera = *cam;
}

void mdk_render_set_lighting(MdkRenderer rr, const mdk::Lighting* light) {
    RendererVk* r = asVk(rr);
    if (!r || !light) {
        return;
    }
    r->lighting = *light;
}

void mdk_render_set_enhanced_toggles(MdkRenderer rr, const mdk::EnhancedToggles* toggles) {
    RendererVk* r = asVk(rr);
    if (!r || !toggles) {
        return;
    }
    r->toggles = *toggles;
}

void mdk_render_set_point_lights(MdkRenderer rr, const mdk::PointLight* lights, int32_t count) {
    RendererVk* r = asVk(rr);
    if (!r || !lights || count <= 0) {
        if (r) {
            r->pointLightCount = 0;
        }
        return;
    }
    const int32_t capped = (count < 64) ? count : 64;
    memcpy(r->pointLights, lights, sizeof(mdk::PointLight) * (size_t)capped);
    r->pointLightCount = capped;
}

void mdk_render_set_panorama(MdkRenderer rr, const mdk::Panorama* p) {
    RendererVk* r = asVk(rr);
    if (!r || !p) {
        return;
    }
    r->panorama = *p;
}

void mdk_render_set_fade(MdkRenderer rr, float level) {
    RendererVk* r = asVk(rr);
    if (r) {
        r->fade = level;
    }
}

void mdk_render_set_gamma(MdkRenderer rr, float gamma) {
    RendererVk* r = asVk(rr);
    if (r) {
        r->gamma = gamma;
    }
}

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

MdkTextureId mdk_render_create_index_texture(MdkRenderer r_, int32_t w, int32_t h,
                                             const uint8_t* px, const uint8_t* paletteRgba) {
    // The Vulkan backend IGNORES the palette bytes: it samples the index
    // texture and then the palette in the shader, exactly as the C# does.
    // The palette reaches the shader through the material's palette id.
    (void)paletteRgba;
    RendererVk* r = asVk(r_);
    if (!r || !r->ready || !px || w <= 0 || h <= 0) {
        return -1;
    }
    const int32_t id = acquireSlot(r->textures);
    TextureVk& t = r->textures[id];
    if (!createHostImage(&r->ctx, w, h, VK_FORMAT_R8_UNORM, 1, px, &t.image, &t.memory,
                         &t.view, &t.mapped)) {
        refused("index texture %dx%d (%llu bytes): the surface it belongs to draws untextured",
                w, h, (unsigned long long)((VkDeviceSize)w * (VkDeviceSize)h));
        t.used = false;
        return -1;
    }
    t.isIndex = true;
    t.width = w;
    t.height = h;
    t.size = (VkDeviceSize)w * (VkDeviceSize)h;
    t.indices.assign(px, px + (size_t)w * (size_t)h);
    t.version = 1;
    ResRef ref;
    ref.kind = 0;
    ref.id = id;
    r->creationLog.push_back(ref);
    return id;
}

int32_t mdk_render_index_texture_bytes(MdkRenderer r_, int32_t w, int32_t h) {
    (void)r_;
    if (w <= 0 || h <= 0) {
        return 0;
    }
    // WHAT THE DESKTOP SPENDS ON AN INDEX TEXTURE. The indices are kept as
    // they are -- an R8_UNORM image, one byte a pixel -- and the palette
    // lookup happens in the shader. So the texture is exactly its own
    // pixels, and the 3DS's expanded, padded RGBA8 is eight times that.
    return w * h;
}

MdkTextureId mdk_render_create_rgba_texture(MdkRenderer r_, int32_t w, int32_t h,
                                            const uint8_t* px) {
    RendererVk* r = asVk(r_);
    if (!r || !r->ready || !px || w <= 0 || h <= 0) {
        return -1;
    }
    const int32_t id = acquireSlot(r->textures);
    TextureVk& t = r->textures[id];
    if (!createHostImage(&r->ctx, w, h, VK_FORMAT_R8G8B8A8_UNORM, 4, px, &t.image, &t.memory,
                         &t.view, &t.mapped)) {
        refused("RGBA texture %dx%d (%llu bytes): whatever asked for it draws without it",
                w, h, (unsigned long long)((VkDeviceSize)w * (VkDeviceSize)h * 4));
        t.used = false;
        return -1;
    }
    t.isIndex = false;
    t.width = w;
    t.height = h;
    t.size = (VkDeviceSize)w * (VkDeviceSize)h * 4;
    ResRef ref;
    ref.kind = 0;
    ref.id = id;
    r->creationLog.push_back(ref);
    return id;
}

MdkPaletteId mdk_render_create_palette(MdkRenderer r_, const uint8_t* rgba) {
    RendererVk* r = asVk(r_);
    if (!r || !r->ready || !rgba) {
        return -1;
    }
    const int32_t id = acquireSlot(r->palettes);
    PaletteVk& p = r->palettes[id];
    memcpy(p.rgba, rgba, sizeof(p.rgba));
    if (!createHostImage(&r->ctx, 256, 1, VK_FORMAT_R8G8B8A8_UNORM, 4, rgba, &p.image,
                         &p.memory, &p.view, &p.mapped)) {
        refused("palette (256x1 RGBA, 1024 bytes): every surface that names palette id %d "
                "draws without a palette", id);
        p.used = false;
        return -1;
    }
    ResRef ref;
    ref.kind = 1;
    ref.id = id;
    r->creationLog.push_back(ref);
    return id;
}

void mdk_render_update_texture(MdkRenderer r_, MdkTextureId t_, int32_t w, int32_t h,
                               const uint8_t* px) {
    RendererVk* r = asVk(r_);
    if (!r || t_ < 0 || t_ >= (int32_t)r->textures.size() || !r->textures[t_].used || !px) {
        return;
    }
    TextureVk& t = r->textures[t_];
    if (t.width != w || t.height != h) {
        return;  // the contract is same-size updates only
    }
    // The image is OPTIMAL-tiled with no persistent mapping (see
    // createHostImage); an update re-creates the image through the staging
    // path. Updates are rare (animated textures), not per-frame-per-texture.
    // The cached descriptor sets name the old image view, so they must go
    // before that view is destroyed.
    forgetDescriptorSets(r, t_, -1);
    if (t.arrayView) {
        vkDestroyImageView(r->ctx.device, t.arrayView, nullptr);
        t.arrayView = VK_NULL_HANDLE;
    }
    VkImage oldImage = t.image;
    VkDeviceMemory oldMemory = t.memory;
    VkImageView oldView = t.view;
    t.image = VK_NULL_HANDLE;
    t.memory = VK_NULL_HANDLE;
    t.view = VK_NULL_HANDLE;
    if (createHostImage(&r->ctx, w, h, t.isIndex ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM,
                        t.isIndex ? 1u : 4u, px, &t.image, &t.memory, &t.view, &t.mapped)) {
        if (t.isIndex) {
            t.indices.assign(px, px + (size_t)w * (size_t)h);
            t.version++;
            // A changed texture shows its expansion again: its material
            // replacements (not its canvas images) are stale, as in the
            // C#'s Changed. Their colour textures rebuild on next draw
            // through the version check in getColourView.
            for (size_t i = 0; i < r->replacements.size();) {
                const ReplacementVk& rep = r->replacements[i];
                if (rep.used && !rep.image && rep.tex == t_) {
                    r->replacements.erase(r->replacements.begin() + (int)i);
                } else {
                    i++;
                }
            }
        }
        // Retired, not destroyed: a frame already on the queue may still be
        // sampling this view through a set that was retired a few lines above.
        retireImage(r, oldImage, oldMemory, oldView);
    } else {
        refused("re-made texture %d at %dx%d: keeping the image it had, so an animated "
                "surface freezes on its last frame instead of disappearing", t_, w, h);
        t.image = oldImage;
        t.memory = oldMemory;
        t.view = oldView;
    }
}

void mdk_render_replace_colour(MdkRenderer r_, MdkTextureId texture, MdkPaletteId palette,
                               int32_t frames, int32_t width, int32_t height,
                               const uint8_t* rgba) {
    registerReplacement(asVk(r_), texture, palette, frames, width, height, rgba, false);
}

void mdk_render_replace_image(MdkRenderer r_, MdkTextureId texture, MdkPaletteId palette,
                              int32_t width, int32_t height, const uint8_t* rgba) {
    registerReplacement(asVk(r_), texture, palette, 1, width, height, rgba, true);
}

void mdk_render_release_texture(MdkRenderer r_, MdkTextureId t_) {
    RendererVk* r = asVk(r_);
    if (!r || t_ < 0 || t_ >= (int32_t)r->textures.size() || !r->textures[t_].used) {
        return;
    }
    // The cached descriptor sets name the image view that is about to be
    // destroyed. A released id may be reused, so those sets must not
    // survive the release.
    forgetDescriptorSets(r, t_, -1);
    // Its replacements and built colour textures name it; a reused id must
    // not inherit them.
    for (size_t i = 0; i < r->replacements.size();) {
        if (r->replacements[i].used && r->replacements[i].tex == t_) {
            dropColour(r, t_, r->replacements[i].pal, r->replacements[i].frames);
            r->replacements.erase(r->replacements.begin() + (int)i);
        } else {
            i++;
        }
    }
    if (r->textures[t_].arrayView) {
        vkDestroyImageView(r->ctx.device, r->textures[t_].arrayView, nullptr);
        r->textures[t_].arrayView = VK_NULL_HANDLE;
    }
    // Retire the resource beside the sets that name it. Nulling the fields is
    // what makes the retire exclusive: leaving them set would have teardown
    // destroy the same image a second time.
    retireImage(r, r->textures[t_].image, r->textures[t_].memory, r->textures[t_].view);
    r->textures[t_].image = VK_NULL_HANDLE;
    r->textures[t_].memory = VK_NULL_HANDLE;
    r->textures[t_].view = VK_NULL_HANDLE;
    r->textures[t_].used = false;
}

void mdk_render_release_palette(MdkRenderer r_, MdkPaletteId p_) {
    RendererVk* r = asVk(r_);
    if (!r || p_ < 0 || p_ >= (int32_t)r->palettes.size() || !r->palettes[p_].used) {
        return;
    }
    forgetDescriptorSets(r, -1, p_);
    // Its replacements and built colour textures name it, as for a texture.
    for (size_t i = 0; i < r->replacements.size();) {
        if (r->replacements[i].used && r->replacements[i].pal == p_) {
            dropColour(r, r->replacements[i].tex, p_, r->replacements[i].frames);
            r->replacements.erase(r->replacements.begin() + (int)i);
        } else {
            i++;
        }
    }
    retireImage(r, r->palettes[p_].image, r->palettes[p_].memory, r->palettes[p_].view);
    r->palettes[p_].image = VK_NULL_HANDLE;
    r->palettes[p_].memory = VK_NULL_HANDLE;
    r->palettes[p_].view = VK_NULL_HANDLE;
    r->palettes[p_].used = false;
}

MdkMeshId mdk_render_create_mesh(MdkRenderer r_, const mdk::Vertex* v, int32_t n) {
    RendererVk* r = asVk(r_);
    if (!r || !r->ready || !v || n <= 0) {
        return -1;
    }
    const int32_t id = acquireSlot(r->meshes);
    MeshVk& m = r->meshes[id];
    const VkDeviceSize size = sizeof(mdk::Vertex) * (VkDeviceSize)n;
    if (!createHostBuffer(&r->ctx, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &m.buffer[0],
                          &m.memory[0], &m.mapped[0])) {
        refused("static mesh of %d corners (%llu bytes): the caller gets no mesh id and "
                "draws nothing", n, (unsigned long long)size);
        m.used = false;
        return -1;
    }
    memcpy(m.mapped[0], v, (size_t)size);
    m.capacity = size;
    ResRef ref;
    ref.kind = 2;
    ref.id = id;
    r->creationLog.push_back(ref);
    return id;
}

MdkMeshId mdk_render_create_dynamic_mesh(MdkRenderer r_, int32_t cap) {
    RendererVk* r = asVk(r_);
    if (!r || !r->ready || cap <= 0) {
        return -1;
    }
    const int32_t id = acquireSlot(r->meshes);
    MeshVk& m = r->meshes[id];
    const VkDeviceSize size = sizeof(mdk::Vertex) * (VkDeviceSize)cap;
    // ONE BUFFER PER FRAME IN FLIGHT. The frame writes its whole stream into
    // the slot it will draw from; the previous frame's submission may still be
    // reading the other one (MeshVk, AGENTS.md P3). Two slots is the whole
    // cost: the object stream is a few hundred kilobytes at its largest
    // measured frame, and the alternative -- waiting for the previous frame
    // before writing -- is a stall in the frame's critical path.
    for (uint32_t slot = 0; slot < mdk::vk::kFramesInFlight; slot++) {
        if (!createHostBuffer(&r->ctx, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &m.buffer[slot],
                              &m.memory[slot], &m.mapped[slot])) {
            for (uint32_t done = 0; done < slot; done++) {
                destroyBufferPair(r->ctx.device, m.buffer[done], m.memory[done]);
                m.buffer[done] = VK_NULL_HANDLE;
                m.memory[done] = VK_NULL_HANDLE;
                m.mapped[done] = nullptr;
            }
            refused("dynamic mesh of %d corners x %u frames in flight (%llu bytes total, "
                    "%llu per slot): the object pass has nowhere to upload and draws no "
                    "objects", cap, (unsigned)mdk::vk::kFramesInFlight,
                    (unsigned long long)(size * mdk::vk::kFramesInFlight),
                    (unsigned long long)size);
            m.used = false;
            return -1;
        }
    }
    m.dynamic = true;
    m.capacity = size;
    ResRef ref;
    ref.kind = 2;
    ref.id = id;
    r->creationLog.push_back(ref);
    return id;
}

void mdk_render_update_mesh(MdkRenderer r_, MdkMeshId m_, const mdk::Vertex* v, int32_t n) {
    RendererVk* r = asVk(r_);
    if (!r || m_ < 0 || m_ >= (int32_t)r->meshes.size() || !r->meshes[m_].used || !v || n <= 0) {
        return;
    }
    MeshVk& m = r->meshes[m_];
    const VkDeviceSize size = sizeof(mdk::Vertex) * (VkDeviceSize)n;
    if (size > m.capacity) {
        refused("update of mesh %d with %d corners (%llu bytes) against a capacity of %llu "
                "bytes: the mesh keeps the vertices it already had", m_, n,
                (unsigned long long)size, (unsigned long long)m.capacity);
        return;
    }
    // The slot this frame draws from, which is the one whose fence has been
    // waited. A static mesh is slot 0 and is not rewritten after it is made.
    //
    // BETWEEN FRAMES THERE IS NO SUCH SLOT. Outside a recorded frame the fence
    // that was waited belongs to the slot the NEXT frame will use, and the frame
    // before that one may still be running, so the only safe thing is to wait for
    // the device. The seam's contract is that a dynamic mesh is updated while the
    // frame it belongs to is being recorded (render.h), and every caller does that
    // -- this is a guard, not a path.
    if (!r->inFrame) {
        if (vkDeviceWaitIdle(r->ctx.device) != VK_SUCCESS) {
            // The whole point of the wait is that the previous frame may still be
            // reading this slot. A wait that failed does not establish that, and
            // memcpy-ing under a running frame is the CPU reuse P3 puts the
            // completion boundary before.
            refused("update of mesh %d outside a recorded frame could not wait for the "
                    "device: the update is skipped rather than written under a frame "
                    "that may still be reading this slot", m_);
            return;
        }
    }
    const int slot = meshSlot(m, &r->ctx);
    void* mapped = m.mapped[slot];
    if (!mapped) {
        refused("update of mesh %d found no mapped slot at slot %d: the frame draws the "
                "mesh's previous contents", m_, slot);
        return;
    }
    memcpy(mapped, v, (size_t)size);
}

void mdk_render_release_mesh(MdkRenderer r_, MdkMeshId m_) {
    RendererVk* r = asVk(r_);
    if (!r || m_ < 0 || m_ >= (int32_t)r->meshes.size() || !r->meshes[m_].used) {
        return;
    }
    MeshVk& m = r->meshes[m_];
    // RETIRED, NOT DESTROYED. The mesh may have been drawn by a frame that is
    // still on the GPU; destroying its buffer there is the use-after-free the
    // loader would be within its rights to make visible.
    for (uint32_t slot = 0; slot < mdk::vk::kFramesInFlight; slot++) {
        retireBuffer(r, m.buffer[slot], m.memory[slot]);
        m.buffer[slot] = VK_NULL_HANDLE;
        m.memory[slot] = VK_NULL_HANDLE;
        m.mapped[slot] = nullptr;
    }
    m.used = false;
}

MdkScope mdk_render_mark(MdkRenderer r_) {
    RendererVk* r = asVk(r_);
    MdkScope scope;
    scope.textures = r ? (int32_t)r->creationLog.size() : 0;
    scope.buffers = 0;
    return scope;
}

void mdk_render_release(MdkRenderer r_, MdkScope s) {
    RendererVk* r = asVk(r_);
    if (!r) {
        return;
    }
    while ((int32_t)r->creationLog.size() > s.textures) {
        const ResRef ref = r->creationLog.back();
        r->creationLog.pop_back();
        if (ref.kind == 0) {
            mdk_render_release_texture(r_, ref.id);
        } else if (ref.kind == 1) {
            mdk_render_release_palette(r_, ref.id);
        } else {
            mdk_render_release_mesh(r_, ref.id);
        }
    }
}

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

struct EnhancedUniforms {
    float colour[4];
    int32_t textured;
    int32_t frameCount;
    int32_t frame;
    int32_t mode;
    float sunMatrix[16];
    float sun[4];
    float sunColour[4];
    float sky[4];
    float ground[4];
    float camera[4];
    float haze[4];
    float shadow[4];
};

// A colour texture's GPU image from its expanded chain: RGBA8, a mip
// chain, one array layer per frame. Uploaded through a staging buffer in
// one one-shot submission, like every other upload here.
bool buildColourImage(RendererVk* r, int32_t w, int32_t h, int32_t frames,
                      const std::vector<uint8_t>& chain, ColourVk* out) {
    const int32_t levels = mdk::colourLevels(w, h);
    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent.width = (uint32_t)w;
    imgInfo.extent.height = (uint32_t)h;
    imgInfo.extent.depth = 1;
    imgInfo.mipLevels = (uint32_t)levels;
    imgInfo.arrayLayers = (uint32_t)frames;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(r->ctx.device, &imgInfo, nullptr, &out->image) != VK_SUCCESS) {
        refused("colour texture image %dx%d x%d frames: the surface draws "
                "through the original pipeline instead", w, h, frames);
        return false;
    }
    VkMemoryRequirements req = {};
    vkGetImageMemoryRequirements(r->ctx.device, out->image, &req);
    const uint32_t type = findMemoryType(&r->ctx, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == 0xFFFFFFFFu) {
        refused("colour texture memory type: the surface draws through the "
                "original pipeline instead");
        vkDestroyImage(r->ctx.device, out->image, nullptr);
        out->image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(r->ctx.device, &alloc, nullptr, &out->memory) != VK_SUCCESS ||
        vkBindImageMemory(r->ctx.device, out->image, out->memory, 0) != VK_SUCCESS) {
        refused("colour texture memory: the surface draws through the original "
                "pipeline instead");
        vkDestroyImage(r->ctx.device, out->image, nullptr);
        out->image = VK_NULL_HANDLE;
        if (out->memory != VK_NULL_HANDLE) {
            vkFreeMemory(r->ctx.device, out->memory, nullptr);
            out->memory = VK_NULL_HANDLE;
        }
        return false;
    }

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = out->image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = (uint32_t)levels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = (uint32_t)frames;
    if (vkCreateImageView(r->ctx.device, &viewInfo, nullptr, &out->view) != VK_SUCCESS) {
        refused("colour texture view: the surface draws through the original "
                "pipeline instead");
        vkDestroyImage(r->ctx.device, out->image, nullptr);
        out->image = VK_NULL_HANDLE;
        vkFreeMemory(r->ctx.device, out->memory, nullptr);
        out->memory = VK_NULL_HANDLE;
        return false;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    void* stagingMapped = nullptr;
    if (!createHostBuffer(&r->ctx, (VkDeviceSize)chain.size(),
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &stagingMemory,
                          &stagingMapped)) {
        refused("colour texture staging: the surface draws through the original "
                "pipeline instead");
        return false;
    }
    memcpy(stagingMapped, chain.data(), chain.size());

    VkCommandPool oneShotPool = VK_NULL_HANDLE;
    VkCommandBuffer cb = beginOneShotTracked(&r->ctx, &oneShotPool);
    if (!cb) {
        refused("colour texture upload command: the surface draws through the "
                "original pipeline instead");
        destroyBufferPair(r->ctx.device, staging, stagingMemory);
        return false;
    }
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = out->image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = (uint32_t)levels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = (uint32_t)frames;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);

    const int32_t chainSize = mdk::colourChainSize(w, h);
    std::vector<VkBufferImageCopy> regions;
    for (int32_t f = 0; f < frames; f++) {
        int32_t lw = w;
        int32_t lh = h;
        VkDeviceSize offset = (VkDeviceSize)f * (VkDeviceSize)chainSize;
        for (int32_t level = 0; level < levels; level++) {
            VkBufferImageCopy region = {};
            region.bufferOffset = offset;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = (uint32_t)level;
            region.imageSubresource.baseArrayLayer = (uint32_t)f;
            region.imageSubresource.layerCount = 1;
            region.imageExtent.width = (uint32_t)lw;
            region.imageExtent.height = (uint32_t)lh;
            region.imageExtent.depth = 1;
            regions.push_back(region);
            offset += (VkDeviceSize)lw * (VkDeviceSize)lh * 4;
            lw = lw / 2 > 1 ? lw / 2 : 1;
            lh = lh / 2 > 1 ? lh / 2 : 1;
        }
    }
    vkCmdCopyBufferToImage(cb, staging, out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           (uint32_t)regions.size(), regions.data());

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &barrier);
    const OneShot done = endOneShot(&r->ctx, oneShotPool, cb);
    destroyBufferPair(r->ctx.device, staging, stagingMemory);
    if (done != OneShot::done) {
        refused("colour texture upload: the copy is not known to have finished, so "
                "the surface draws through the original pipeline instead");
        return false;
    }
    return true;
}

// Drop the built colour texture for a (texture, palette, frames) triple,
// retiring its image for the frames that may still sample it.
void dropColour(RendererVk* r, int32_t tex, int32_t pal, int32_t frames) {
    for (size_t i = 0; i < r->colours.size(); i++) {
        ColourVk& c = r->colours[i];
        if (c.used && c.tex == tex && c.pal == pal && c.frames == frames) {
            retireImage(r, c.image, c.memory, c.view);
            r->colours.erase(r->colours.begin() + (int)i);
            return;
        }
    }
}

// The replacement registered for a triple, or null. A replacement whose
// index texture has changed is stale (the C#'s Changed): it and its built
// colour texture go, and the expansion returns. Canvas images stay.
const ReplacementVk* findReplacement(RendererVk* r, int32_t tex, int32_t pal, int32_t frames) {
    for (size_t i = 0; i < r->replacements.size(); i++) {
        ReplacementVk& rep = r->replacements[i];
        if (!rep.used || rep.tex != tex || rep.pal != pal || rep.frames != frames) {
            continue;
        }
        if (!rep.image && tex >= 0 && tex < (int32_t)r->textures.size() &&
            r->textures[tex].version != rep.textureVersion) {
            dropColour(r, tex, pal, frames);
            r->replacements.erase(r->replacements.begin() + (int)i);
            return nullptr;
        }
        return &rep;
    }
    return nullptr;
}

// Whether a canvas image replacement (mdk_render_replace_image) is
// registered for a (texture, palette) pair: the enhanced canvas draws the
// image (mode 4) instead of filtering the index texture (mode 3).
bool hasImageReplacement(RendererVk* r, int32_t tex, int32_t pal) {
    for (size_t i = 0; i < r->replacements.size(); i++) {
        const ReplacementVk& rep = r->replacements[i];
        if (rep.used && rep.image && rep.tex == tex && rep.pal == pal) {
            return true;
        }
    }
    return false;
}

// A replacement's mip chain: each frame's RGBA into its chain level 0,
// then the shared box filter (render/colour_mips) builds the rest.
std::vector<uint8_t> replacementChain(const ReplacementVk& rep) {
    const int32_t frameBytes = mdk::colourChainSize(rep.width, rep.height);
    std::vector<uint8_t> chain((size_t)frameBytes * (size_t)rep.frames);
    const size_t framePixels = (size_t)rep.width * (size_t)rep.height * 4;
    for (int32_t f = 0; f < rep.frames; f++) {
        uint8_t* level0 = chain.data() + (size_t)frameBytes * (size_t)f;
        memcpy(level0, rep.rgba.data() + framePixels * (size_t)f, framePixels);
        mdk::colourFillChain(level0, rep.width, rep.height);
    }
    return chain;
}

// Register (or re-register) a mod's colour image for a triple. The built
// colour texture for the triple goes, so the next draw builds this.
void registerReplacement(RendererVk* r, int32_t tex, int32_t pal, int32_t frames, int32_t width,
                         int32_t height, const uint8_t* rgba, bool image) {
    if (!r || !r->ready || !rgba || width <= 0 || height <= 0 || frames <= 0 || tex < 0 ||
        tex >= (int32_t)r->textures.size() || !r->textures[tex].used || pal < 0 ||
        pal >= (int32_t)r->palettes.size() || !r->palettes[pal].used) {
        return;
    }
    ReplacementVk rep;
    rep.used = true;
    rep.image = image;
    rep.tex = tex;
    rep.pal = pal;
    rep.frames = frames;
    rep.width = width;
    rep.height = height;
    rep.textureVersion = r->textures[tex].version;
    rep.rgba.assign(rgba, rgba + (size_t)width * (size_t)height * (size_t)frames * 4);
    for (size_t i = 0; i < r->replacements.size(); i++) {
        ReplacementVk& old = r->replacements[i];
        if (old.used && old.tex == tex && old.pal == pal && old.frames == frames) {
            // A canvas image keeps its image flag when a material
            // replacement lands on the same triple (the C# keeps _images).
            rep.image = rep.image || old.image;
            r->replacements.erase(r->replacements.begin() + (int)i);
            break;
        }
    }
    r->replacements.push_back(rep);
    dropColour(r, tex, pal, frames);
}

// The colour texture for a (texture, palette, frames) triple, built or
// rebuilt when the index texture's version has moved (bullet holes).
VkImageView getColourView(RendererVk* r, int32_t tex, int32_t pal, int32_t frames) {
    if (tex < 0 || tex >= (int32_t)r->textures.size() || !r->textures[tex].used ||
        !r->textures[tex].isIndex || r->textures[tex].indices.empty()) {
        refused("colour texture for texture %d: it is not a live index texture, so "
                "the surface draws through the original pipeline instead", tex);
        return VK_NULL_HANDLE;
    }
    if (pal < 0 || pal >= (int32_t)r->palettes.size() || !r->palettes[pal].used) {
        refused("colour texture for palette %d: it is not a live palette, so the "
                "surface draws through the original pipeline instead", pal);
        return VK_NULL_HANDLE;
    }
    const TextureVk& t = r->textures[tex];
    // A mod's replacement image, when one is registered for this triple,
    // is the colour texture instead of the expansion (a stale one has
    // already gone in findReplacement).
    const ReplacementVk* rep = findReplacement(r, tex, pal, frames);
    for (size_t i = 0; i < r->colours.size(); i++) {
        ColourVk& c = r->colours[i];
        if (c.used && c.tex == tex && c.pal == pal && c.frames == frames) {
            if (c.version == t.version) {
                return c.view;
            }
            retireImage(r, c.image, c.memory, c.view);
            c.image = VK_NULL_HANDLE;
            c.memory = VK_NULL_HANDLE;
            c.view = VK_NULL_HANDLE;
            const std::vector<uint8_t> chain =
                rep ? replacementChain(*rep)
                    : mdk::colourChain(t.indices.data(), t.width, t.height, frames,
                                       r->palettes[pal].rgba, 1024);
            if (!buildColourImage(r, rep ? rep->width : t.width, rep ? rep->height : t.height,
                                  frames, chain, &c)) {
                refused("colour texture rebuild for texture %d: the surface draws "
                        "through the original pipeline instead", tex);
                c.used = false;
                return VK_NULL_HANDLE;
            }
            c.version = t.version;
            return c.view;
        }
    }
    ColourVk c;
    c.used = true;
    c.tex = tex;
    c.pal = pal;
    c.frames = frames;
    c.version = t.version;
    const std::vector<uint8_t> chain =
        rep ? replacementChain(*rep)
            : mdk::colourChain(t.indices.data(), t.width, t.height, frames,
                               r->palettes[pal].rgba, 1024);
    if (!buildColourImage(r, rep ? rep->width : t.width, rep ? rep->height : t.height,
                          frames, chain, &c)) {
        refused("colour texture build for texture %d: the surface draws through "
                "the original pipeline instead", tex);
        return VK_NULL_HANDLE;
    }
    r->colours.push_back(c);
    return c.view;
}

// The enhanced pipeline for a pass/blend/primitive triple: the palette
// pipeline's states with the enhanced shaders and layout.
VkPipeline createEnhancedPipeline(RendererVk* r, int pass, int blend, int primitive) {
    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkVertexInputBindingDescription binding = {};
    binding.binding = 0;
    binding.stride = sizeof(mdk::Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[3] = {};
    attrs[0].location = 0; attrs[0].binding = 0; attrs[0].offset = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].location = 1; attrs[1].binding = 0; attrs[1].offset = 12;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[2].location = 2; attrs[2].binding = 0; attrs[2].offset = 20;
    attrs[2].format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = primitive ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = {};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    rs.cullMode = (pass == (int)mdk::Pass::Solid) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms = {};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = {};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = (pass != (int)mdk::Pass::Overlay && pass != (int)mdk::Pass::OnTop)
                             ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = (pass != (int)mdk::Pass::Blended && pass != (int)mdk::Pass::Overlay &&
                           pass != (int)mdk::Pass::OnTop) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    const bool blended = (pass == (int)mdk::Pass::Blended || pass == (int)mdk::Pass::Overlay);
    VkPipelineColorBlendAttachmentState cb = {};
    cb.blendEnable = blended ? VK_TRUE : VK_FALSE;
    cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cb.dstColorBlendFactor = (blend == (int)mdk::Blend::Add) ? VK_BLEND_FACTOR_ONE
                                                             : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cb.colorBlendOp = VK_BLEND_OP_ADD;
    cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cb.alphaBlendOp = VK_BLEND_OP_ADD;
    cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbs = {};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;

    VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamics;

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = r->enhancedVertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = r->enhancedFragModule;
    stages[1].pName = "main";

    VkGraphicsPipelineCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &cbs;
    info.pDynamicState = &dyn;
    info.layout = r->enhancedPipeLayout;
    info.renderPass = r->ctx.renderPass;
    info.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(r->ctx.device, r->pipeCache, 1, &info, nullptr,
                                  &pipeline) != VK_SUCCESS) {
        refused("enhanced pipeline for pass %d blend %d prim %d: the draw is dropped",
                pass, blend, primitive);
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

VkPipeline getEnhancedPipeline(RendererVk* r, int pass, int blend, int primitive) {
    const int key = pass * 16 + blend * 4 + primitive;
    for (size_t i = 0; i < r->enhancedPipelines.size(); i++) {
        if (r->enhancedPipelines[i].key == key) {
            return r->enhancedPipelines[i].pipeline;
        }
    }
    VkPipeline p = createEnhancedPipeline(r, pass, blend, primitive);
    PipelineVk entry;
    entry.key = key;
    entry.pipeline = p;
    r->enhancedPipelines.push_back(entry);
    return p;
}

// The enhanced objects, created on the first enhanced frame.
bool ensureEnhanced(RendererVk* r) {
    if (r->enhancedReady) {
        return true;
    }
    VkDevice device = r->ctx.device;
    r->enhancedVertModule = createModule(&r->ctx, spv_enhanced_vert, spv_enhanced_vert_size);
    r->enhancedFragModule = createModule(&r->ctx, spv_enhanced_frag, spv_enhanced_frag_size);
    if (!r->enhancedVertModule || !r->enhancedFragModule) {
        refused("enhanced shader modules: the frame renders the original look");
        return false;
    }

    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = 1000.0f;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &r->colourSampler) != VK_SUCCESS) {
        refused("colour sampler: the frame renders the original look");
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[6] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // The canvas modes' index and palette (enhanced.frag bindings 4/5):
    // written for every enhanced set, dummy views on the Lit/Sprite path.
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 6;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &r->enhancedSetLayout) !=
        VK_SUCCESS) {
        refused("enhanced descriptor layout: the frame renders the original look");
        return false;
    }

    VkPushConstantRange range = {};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    range.offset = 0;
    range.size = 128;
    VkPipelineLayoutCreateInfo pipeLayoutInfo = {};
    pipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeLayoutInfo.setLayoutCount = 1;
    pipeLayoutInfo.pSetLayouts = &r->enhancedSetLayout;
    pipeLayoutInfo.pushConstantRangeCount = 1;
    pipeLayoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device, &pipeLayoutInfo, nullptr, &r->enhancedPipeLayout) !=
        VK_SUCCESS) {
        refused("enhanced pipeline layout: the frame renders the original look");
        return false;
    }

    for (uint32_t i = 0; i < mdk::vk::kFramesInFlight; i++) {
        VkDescriptorPoolSize poolSizes[2] = {};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[0].descriptorCount = 4096;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[1].descriptorCount = 2048;
        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        poolInfo.maxSets = 1024;
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &r->enhancedPools[i]) !=
            VK_SUCCESS) {
            refused("enhanced descriptor pool: the frame renders the original look");
            return false;
        }
        if (!createHostBuffer(&r->ctx, 64 * 1024, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              &r->uniformRing[i], &r->uniformRingMemory[i],
                              &r->uniformRingMapped[i]) ||
            !createHostBuffer(&r->ctx, 528, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              &r->lightBuffers[i], &r->lightMemory[i], &r->lightMapped[i])) {
            refused("enhanced uniform buffers: the frame renders the original look");
            return false;
        }
    }

    // A 1x1 white colour texture for untextured enhanced draws.
    {
        ColourVk dummy;
        dummy.used = true;
        dummy.tex = -1;
        const uint8_t whitePalette[4] = {255, 255, 255, 255};
        const uint8_t whiteIndex[1] = {0};
        // Index 0 is clear; use a direct RGBA build instead.
        std::vector<uint8_t> chain(4, 255);
        (void)whitePalette;
        (void)whiteIndex;
        if (!buildColourImage(r, 1, 1, 1, chain, &dummy)) {
            refused("enhanced dummy texture: the frame renders the original look");
            return false;
        }
        r->dummyColourImage = dummy.image;
        r->dummyColourMemory = dummy.memory;
        r->dummyColourView = dummy.view;
    }

    r->enhancedReady = true;
    return true;
}

// The frame's point lights into this slot's light buffer (called on the
// first enhanced draw of the frame; the buffer is per frame-in-flight).
void writePointLights(RendererVk* r) {
    const uint32_t slot = r->ctx.currentFrame;
    if (!r->lightMapped[slot]) {
        return;
    }
    float* out = static_cast<float*>(r->lightMapped[slot]);
    // enableFragmentLights off: the light buffer reports no lights, so
    // the GPU skips every point-light term (SPEC.md 7.9).
    const int32_t count = r->toggles.enableFragmentLights
                              ? (r->pointLightCount < 16 ? r->pointLightCount : 16)
                              : 0;
    out[0] = (float)count;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 0.0f;
    for (int32_t i = 0; i < count; i++) {
        const mdk::PointLight& light = r->pointLights[i];
        float* at = out + 4 + (size_t)i * 8;
        at[0] = light.position.x;
        at[1] = light.position.y;
        at[2] = light.position.z;
        at[3] = light.radius;
        at[4] = light.colour.r;
        at[5] = light.colour.g;
        at[6] = light.colour.b;
        at[7] = 0.0f;
    }
}

// A 2D-array view of an RGBA texture's image, for the enhanced shader's
// colour_texture (the C#'s Copy path for colour textures). Created once.
VkImageView getRgbaArrayView(RendererVk* r, int32_t tex) {
    TextureVk& t = r->textures[tex];
    if (!t.arrayView) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = t.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(r->ctx.device, &viewInfo, nullptr, &t.arrayView) !=
            VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
    }
    return t.arrayView;
}

// One enhanced descriptor set for this frame slot: the colour array, the
// uniform ring entry, the point lights, the shadow map, and the canvas
// modes' index/palette (dummy views where a mode does not read them).
// False leaves *out null; the caller names the dropped draw.
bool allocEnhancedSet(RendererVk* r, VkImageView colourView, VkImageView indexView,
                      VkImageView paletteView, VkDeviceSize uniformOffset,
                      VkDescriptorSet* out) {
    const uint32_t slot = r->ctx.currentFrame;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = r->enhancedPools[slot];
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &r->enhancedSetLayout;
    if (vkAllocateDescriptorSets(r->ctx.device, &allocInfo, &set) != VK_SUCCESS) {
        return false;
    }
    VkDescriptorImageInfo imageInfo = {};
    imageInfo.sampler = r->colourSampler;
    imageInfo.imageView = colourView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorBufferInfo uniformInfo = {};
    uniformInfo.buffer = r->uniformRing[slot];
    uniformInfo.offset = uniformOffset;
    uniformInfo.range = sizeof(EnhancedUniforms);
    VkDescriptorBufferInfo lightInfo = {};
    lightInfo.buffer = r->lightBuffers[slot];
    lightInfo.offset = 0;
    lightInfo.range = 528;
    VkDescriptorImageInfo shadowInfo = {};
    shadowInfo.sampler = r->colourSampler;
    shadowInfo.imageView = r->shadowReady ? r->shadowView : r->dummyColourView;
    shadowInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo indexInfo = {};
    indexInfo.sampler = r->sampler;
    indexInfo.imageView = indexView;
    indexInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo paletteInfo = {};
    paletteInfo.sampler = r->sampler;
    paletteInfo.imageView = paletteView;
    paletteInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet writes[6] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &imageInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &uniformInfo;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = set;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].pBufferInfo = &lightInfo;
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = set;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].pImageInfo = &shadowInfo;
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = set;
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[4].pImageInfo = &indexInfo;
    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = set;
    writes[5].dstBinding = 5;
    writes[5].descriptorCount = 1;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[5].pImageInfo = &paletteInfo;
    vkUpdateDescriptorSets(r->ctx.device, 6, writes, 0, nullptr);
    *out = set;
    return true;
}

// Record one enhanced draw: the Lit/Sprite path of drawRecorded.
// `world` and `wv` are drawRecorded's own matrices for this draw.
void drawEnhanced(RendererVk* r, const mdk::MeshDraw* draw, const float world[16],
                  const float wv[16]) {
    const int pass = (int)draw->material.pass;
    const int blend = (int)draw->material.blend;
    const int primitive = (draw->primitive == mdk::Primitive::Lines) ? 1 : 0;
    VkPipeline pipeline = getEnhancedPipeline(r, pass, blend, primitive);
    if (!pipeline) {
        return;
    }

    const bool textured = draw->material.texture >= 0 &&
                          draw->material.texture < (int32_t)r->textures.size() &&
                          r->textures[draw->material.texture].used;
    VkImageView colourView = r->dummyColourView;
    if (textured && r->textures[draw->material.texture].isIndex) {
        const int32_t frames = draw->material.frame_count > 0 ? draw->material.frame_count : 1;
        colourView = getColourView(r, draw->material.texture, draw->material.palette, frames);
        if (!colourView) {
            return;
        }
    } else if (textured) {
        // An RGBA texture given whole (the C#'s Copy path for colour
        // textures): sample it through an array view of the same image.
        colourView = getRgbaArrayView(r, draw->material.texture);
        if (!colourView) {
            return;
        }
    }

    const uint32_t slot = r->ctx.currentFrame;
    if (r->uniformCursor == 0) {
        writePointLights(r);
    }
    const uint32_t stride = 256;
    if ((r->uniformCursor + 1) * stride > 64 * 1024) {
        refused("enhanced uniform ring full: the rest of the frame's lit draws "
                "are dropped");
        return;
    }
    EnhancedUniforms u = {};
    u.colour[0] = draw->material.colour.r;
    u.colour[1] = draw->material.colour.g;
    u.colour[2] = draw->material.colour.b;
    u.colour[3] = draw->material.colour.a;
    u.textured = textured ? 1 : 0;
    u.frameCount = draw->material.frame_count > 0 ? draw->material.frame_count : 1;
    u.frame = draw->frame;
    u.mode = draw->material.shading == mdk::Shading::Sprite ? 2 : 1;
    memcpy(u.sunMatrix, r->sunMatrix, sizeof(u.sunMatrix));
    u.sun[0] = r->lighting.sun_direction.x;
    u.sun[1] = r->lighting.sun_direction.y;
    u.sun[2] = r->lighting.sun_direction.z;
    u.sun[3] = r->lighting.exposure;
    u.sunColour[0] = r->lighting.sun.r;
    u.sunColour[1] = r->lighting.sun.g;
    u.sunColour[2] = r->lighting.sun.b;
    u.sunColour[3] = 1.0f;
    u.sky[0] = r->lighting.sky.r;
    u.sky[1] = r->lighting.sky.g;
    u.sky[2] = r->lighting.sky.b;
    u.sky[3] = 0.6f;  // Tonemap.Knee
    u.ground[0] = r->lighting.ground.r;
    u.ground[1] = r->lighting.ground.g;
    u.ground[2] = r->lighting.ground.b;
    u.ground[3] = 1.0f;
    u.camera[0] = r->activeCamera.position.x;
    u.camera[1] = r->activeCamera.position.y;
    u.camera[2] = r->activeCamera.position.z;
    u.camera[3] = 1.0f;
    u.haze[0] = r->lighting.haze_colour.r;
    u.haze[1] = r->lighting.haze_colour.g;
    u.haze[2] = r->lighting.haze_colour.b;
    u.haze[3] = r->toggles.enableHaze ? r->lighting.haze_density : 0.0f;
    u.shadow[0] = 2048.0f;
    u.shadow[1] = 0.5f / (2.0f * mdk::kSunShadowReach);
    u.shadow[2] = 0.5f;  // ShadowNormalOffset
    u.shadow[3] = (r->lighting.shadows && r->toggles.enableSkyShadows && r->shadowReady)
                      ? 1.0f
                      : 0.0f;

    const VkDeviceSize uniformOffset = (VkDeviceSize)r->uniformCursor * stride;
    memcpy(static_cast<uint8_t*>(r->uniformRingMapped[slot]) + uniformOffset, &u, sizeof(u));
    r->uniformCursor++;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!allocEnhancedSet(r, colourView, r->dummyColourView, r->dummyColourView,
                          uniformOffset, &set)) {
        refused("enhanced descriptor set: the lit draw is dropped");
        return;
    }

    float push[32];
    float mvp[16];
    mulMatrix(wv, r->activeCamera.projection, mvp);
    flipClipY(mvp);
    memcpy(push, mvp, sizeof(mvp));
    memcpy(push + 16, world, sizeof(float) * 16);

    VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->enhancedPipeLayout,
                            0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cb, r->enhancedPipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(push), push);

    const MeshVk& mesh = r->meshes[draw->mesh];
    const VkBuffer buffer = mesh.buffer[meshSlot(mesh, &r->ctx)];
    if (!buffer) {
        return;
    }
    const VkDeviceSize offset = sizeof(mdk::Vertex) * (VkDeviceSize)draw->first_vertex;
    vkCmdBindVertexBuffers(cb, 0, 1, &buffer, &offset);
    vkCmdDraw(cb, (uint32_t)draw->vertex_count, 1, 0, 0);

    r->drawCalls++;
    if (!primitive) {
        r->triangles += draw->vertex_count / 3;
    }
}


// Defined with the canvas code below; the enhanced canvas draw needs them.
float canvasWidth(RendererVk* r);
void canvasOrtho(float out[16], float width);

// One canvas quad through the enhanced shader (Renderer.Enhanced.cs's
// ModeOf for the Overlay pass under CanvasSampling.Linear): mode 3 filters
// the index texture through its palette, mode 4 samples a colour texture
// (a mod's replacement image); both blend their edges by coverage instead
// of cutting them. The vertices are already in the canvas ring; the world
// matrix is identity and the transform is the canvas ortho, exactly the
// palette path's geometry with the enhanced shader over it.
void drawEnhancedCanvas(RendererVk* r, int32_t texture, int32_t palette, int32_t frameCount,
                        const float colour[4], VkDeviceSize bufferOffset) {
    VkPipeline pipeline = getEnhancedPipeline(r, (int)mdk::Pass::Overlay,
                                              (int)mdk::Blend::Alpha, 0);
    if (!pipeline) {
        return;
    }
    const bool textured = texture >= 0 && texture < (int32_t)r->textures.size() &&
                          r->textures[texture].used;
    // A canvas image given whole by a mod (ReplaceImage) draws as mode 4
    // through its colour texture; an RGBA texture is mode 4 as before.
    const bool imageReplacement =
        textured && palette >= 0 && hasImageReplacement(r, texture, palette);
    const int32_t mode = (textured && (palette < 0 || imageReplacement)) ? 4 : 3;
    VkImageView colourView = r->dummyColourView;
    VkImageView indexView = r->dummyColourView;
    VkImageView paletteView = r->dummyColourView;
    if (textured && mode == 4 && imageReplacement) {
        colourView = getColourView(r, texture, palette, 1);
        if (!colourView) {
            return;
        }
    } else if (textured && mode == 4) {
        colourView = getRgbaArrayView(r, texture);
        if (!colourView) {
            return;
        }
    }
    if (textured && mode == 3) {
        if (palette < 0 || palette >= (int32_t)r->palettes.size() ||
            !r->palettes[palette].used) {
            return;
        }
        indexView = r->textures[texture].view;
        paletteView = r->palettes[palette].view;
    }

    const uint32_t slot = r->ctx.currentFrame;
    if (r->uniformCursor == 0) {
        writePointLights(r);
    }
    const uint32_t stride = 256;
    if ((r->uniformCursor + 1) * stride > 64 * 1024) {
        refused("enhanced uniform ring full: the rest of the frame's canvas "
                "quads are dropped");
        return;
    }
    EnhancedUniforms u = {};
    u.colour[0] = colour[0];
    u.colour[1] = colour[1];
    u.colour[2] = colour[2];
    u.colour[3] = colour[3];
    u.textured = textured ? 1 : 0;
    u.frameCount = frameCount > 0 ? frameCount : 1;
    u.frame = 0;
    u.mode = mode;
    const VkDeviceSize uniformOffset = (VkDeviceSize)r->uniformCursor * stride;
    memcpy(static_cast<uint8_t*>(r->uniformRingMapped[slot]) + uniformOffset, &u, sizeof(u));
    r->uniformCursor++;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!allocEnhancedSet(r, colourView, indexView, paletteView, uniformOffset, &set)) {
        refused("enhanced descriptor set: the canvas quad is dropped");
        return;
    }

    float push[32];
    float mvp[16];
    canvasOrtho(mvp, canvasWidth(r));
    memcpy(push, mvp, sizeof(mvp));
    for (int i = 16; i < 32; i++) {
        push[i] = 0.0f;
    }
    push[16] = 1.0f;
    push[21] = 1.0f;
    push[26] = 1.0f;
    push[31] = 1.0f;

    VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->enhancedPipeLayout,
                            0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cb, r->enhancedPipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(push), push);
    VkDeviceSize offset = bufferOffset;
    vkCmdBindVertexBuffers(cb, 0, 1, &r->canvasBuffer[slot], &offset);
    vkCmdDraw(cb, 6, 1, 0, 0);
    r->drawCalls++;
}

// The sun shadow's objects: a 2048x2048 depth target and its depth-only
// pass. The depth pipelines reuse the palette layout and sets (the depth
// fragment discards palette index 0 exactly as depth.hlsl does).
bool ensureShadow(RendererVk* r) {
    if (r->shadowReady) {
        return true;
    }
    VkDevice device = r->ctx.device;

    VkAttachmentDescription attachment = {};
    attachment.format = VK_FORMAT_D32_SFLOAT;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef = {};
    depthRef.attachment = 0;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = 0;
    dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo passInfo = {};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    passInfo.attachmentCount = 1;
    passInfo.pAttachments = &attachment;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = 2;
    passInfo.pDependencies = dependencies;
    if (vkCreateRenderPass(device, &passInfo, nullptr, &r->shadowPass) != VK_SUCCESS) {
        refused("shadow render pass: the frame renders without sun shadows");
        return false;
    }

    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_D32_SFLOAT;
    imgInfo.extent.width = 2048;
    imgInfo.extent.height = 2048;
    imgInfo.extent.depth = 1;
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imgInfo, nullptr, &r->shadowImage) != VK_SUCCESS) {
        refused("shadow map image: the frame renders without sun shadows");
        return false;
    }
    VkMemoryRequirements req = {};
    vkGetImageMemoryRequirements(device, r->shadowImage, &req);
    const uint32_t type = findMemoryType(&r->ctx, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == 0xFFFFFFFFu) {
        refused("shadow map memory type: the frame renders without sun shadows");
        vkDestroyImage(device, r->shadowImage, nullptr);
        r->shadowImage = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &alloc, nullptr, &r->shadowMemory) != VK_SUCCESS ||
        vkBindImageMemory(device, r->shadowImage, r->shadowMemory, 0) != VK_SUCCESS) {
        refused("shadow map memory: the frame renders without sun shadows");
        vkDestroyImage(device, r->shadowImage, nullptr);
        r->shadowImage = VK_NULL_HANDLE;
        if (r->shadowMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device, r->shadowMemory, nullptr);
            r->shadowMemory = VK_NULL_HANDLE;
        }
        return false;
    }
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = r->shadowImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_D32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &viewInfo, nullptr, &r->shadowView) != VK_SUCCESS) {
        refused("shadow map view: the frame renders without sun shadows");
        return false;
    }
    VkImageView attachments[1] = {r->shadowView};
    VkFramebufferCreateInfo fbInfo = {};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = r->shadowPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = attachments;
    fbInfo.width = 2048;
    fbInfo.height = 2048;
    fbInfo.layers = 1;
    if (vkCreateFramebuffer(device, &fbInfo, nullptr, &r->shadowFramebuffer) != VK_SUCCESS) {
        refused("shadow framebuffer: the frame renders without sun shadows");
        return false;
    }

    r->depthFragModule = createModule(&r->ctx, spv_depth_frag, spv_depth_frag_size);
    if (!r->depthFragModule) {
        refused("depth shader module: the frame renders without sun shadows");
        return false;
    }
    for (int cull = 0; cull < 2; cull++) {
        VkPipelineVertexInputStateCreateInfo vi = {};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkVertexInputBindingDescription binding = {};
        binding.binding = 0;
        binding.stride = sizeof(mdk::Vertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attrs[3] = {};
        attrs[0].location = 0; attrs[0].binding = 0; attrs[0].offset = 0;
        attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[1].location = 1; attrs[1].binding = 0; attrs[1].offset = 12;
        attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
        attrs[2].location = 2; attrs[2].binding = 0; attrs[2].offset = 20;
        attrs[2].format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = 3;
        vi.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo ia = {};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp = {};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = {};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1.0f;
        rs.cullMode = cull == 0 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        VkPipelineMultisampleStateCreateInfo ms = {};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds = {};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendStateCreateInfo cbs = {};
        cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cbs.attachmentCount = 0;
        VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn = {};
        dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dynamics;
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = r->vertModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = r->depthFragModule;
        stages[1].pName = "main";
        VkGraphicsPipelineCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vi;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &vp;
        info.pRasterizationState = &rs;
        info.pMultisampleState = &ms;
        info.pDepthStencilState = &ds;
        info.pColorBlendState = &cbs;
        info.pDynamicState = &dyn;
        info.layout = r->pipeLayout;
        info.renderPass = r->shadowPass;
        info.subpass = 0;
        if (vkCreateGraphicsPipelines(device, r->pipeCache, 1, &info, nullptr,
                                      &r->depthPipelines[cull]) != VK_SUCCESS) {
            refused("shadow depth pipeline: the frame renders without sun shadows");
            return false;
        }
    }

    r->shadowReady = true;
    return true;
}

// The sun's depth: the frame's shadow casters (opaque Lit triangles)
// through the sun's matrix, into the shadow map. Records into the open
// command buffer with no pass open; closes its own pass and leaves the
// map readable by the fragment shader.
void drawShadowMap(RendererVk* r) {
    mdk::sunShadowMatrix(r->lighting.sun_direction, r->activeCamera.position,
                         r->lighting.shadow_distance, 2048, r->sunMatrix);

    VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = 2048.0f;
    viewport.height = 2048.0f;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor = {{0, 0}, {2048, 2048}};

    mdk::vk::beginDepthPass(&r->ctx, r->shadowFramebuffer, r->shadowPass, 2048, 2048);
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &scissor);

    for (int32_t bucketIdx = 0; bucketIdx < mdk::kDrawQueuePassCount; bucketIdx++) {
        const std::vector<mdk::MeshDraw>& bucket = r->queue.bucket(bucketIdx);
        for (size_t i = 0; i < bucket.size(); i++) {
            const mdk::MeshDraw& draw = bucket[i];
            if (draw.primitive != mdk::Primitive::Triangles) {
                continue;
            }
            if (draw.material.pass != mdk::Pass::Solid &&
                draw.material.pass != mdk::Pass::DoubleSided) {
                continue;
            }
            if (draw.material.shading != mdk::Shading::Lit) {
                continue;
            }
            if (draw.mesh < 0 || draw.mesh >= (int32_t)r->meshes.size() ||
                !r->meshes[draw.mesh].used || draw.vertex_count <= 0) {
                continue;
            }
            const MeshVk& mesh = r->meshes[draw.mesh];
            const VkBuffer buffer = mesh.buffer[meshSlot(mesh, &r->ctx)];
            if (!buffer) {
                continue;
            }

            float world[16];
            if (draw.material.depth_layer != 0) {
                float pulled[16];
                mdk::buildDepthPullMatrix(r->activeCamera.position, draw.primitive,
                                          draw.material.depth_layer, pulled);
                mulMatrix(draw.world, pulled, world);
            } else {
                memcpy(world, draw.world, sizeof(world));
            }
            PushConstants pc = {};
            float ws[16];
            mulMatrix(world, r->sunMatrix, ws);
            flipClipY(ws);
            memcpy(pc.mvp, ws, sizeof(ws));
            const bool textured = draw.material.texture >= 0 &&
                                  draw.material.texture < (int32_t)r->textures.size() &&
                                  r->textures[draw.material.texture].used &&
                                  r->textures[draw.material.texture].isIndex;
            pc.textured = textured ? 1 : 0;
            pc.frame_count = draw.material.frame_count > 0 ? draw.material.frame_count : 1;
            pc.frame = draw.frame;

            VkPipeline pipeline =
                r->depthPipelines[draw.material.pass == mdk::Pass::Solid ? 0 : 1];
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdPushConstants(cb, r->pipeLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(PushConstants), &pc);
            if (textured) {
                VkDescriptorSet set =
                    getDescriptorSet(r, draw.material.texture, draw.material.palette, true);
                if (!set) {
                    continue;
                }
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeLayout,
                                        0, 1, &set, 0, nullptr);
            }
            const VkDeviceSize offset = sizeof(mdk::Vertex) * (VkDeviceSize)draw.first_vertex;
            vkCmdBindVertexBuffers(cb, 0, 1, &buffer, &offset);
            vkCmdDraw(cb, (uint32_t)draw.vertex_count, 1, 0, 0);
        }
    }
    mdk::vk::endPass(&r->ctx);

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = r->shadowImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &barrier);
}

// ---------------------------------------------------------------------------
// The post pass (P6 occlusion/post slice): the camera's depth from the
// opaque queue, the Alchemy occlusion into its own target, then the
// scene (with mips) through occlusion + glow into the scene target.
// Mirrors the C#'s RenderEnhanced order: shadow, camera depth, scene,
// mips, post, canvas.
// ---------------------------------------------------------------------------

struct PostUniforms {
    float clipToView[16];
    float viewToClip[16];
    float screen[4];
    float occlusion[4];
    float glow[4];
};

// One post target image + memory + a view over all its mips.
bool createPostTarget(RendererVk* r, VkFormat format, VkImageUsageFlags usage, uint32_t w,
                      uint32_t h, uint32_t mips, VkImageAspectFlags aspect, VkImage* image,
                      VkDeviceMemory* memory, VkImageView* view) {
    VkDevice device = r->ctx.device;
    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = format;
    imgInfo.extent.width = w;
    imgInfo.extent.height = h;
    imgInfo.extent.depth = 1;
    imgInfo.mipLevels = mips;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = usage;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imgInfo, nullptr, image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req = {};
    vkGetImageMemoryRequirements(device, *image, &req);
    const uint32_t type = findMemoryType(&r->ctx, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (type == 0xFFFFFFFFu || vkAllocateMemory(device, &alloc, nullptr, memory) != VK_SUCCESS ||
        vkBindImageMemory(device, *image, *memory, 0) != VK_SUCCESS) {
        vkDestroyImage(device, *image, nullptr);
        *image = VK_NULL_HANDLE;
        if (*memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, *memory, nullptr);
            *memory = VK_NULL_HANDLE;
        }
        return false;
    }
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = *image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspect;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mips;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &viewInfo, nullptr, view) != VK_SUCCESS) {
        vkDestroyImage(device, *image, nullptr);
        *image = VK_NULL_HANDLE;
        vkFreeMemory(device, *memory, nullptr);
        *memory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

// A screen-triangle pipeline over the composite pass (the gamma
// pipeline's states, a different fragment shader).
bool createScreenPipeline(RendererVk* r, VkShaderModule fragModule, VkPipeline* out) {
    VkDevice device = r->ctx.device;
    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    VkPipelineMultisampleStateCreateInfo ms = {};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    VkPipelineColorBlendAttachmentState blend = {};
    blend.blendEnable = VK_FALSE;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbs = {};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1;
    cbs.pAttachments = &blend;
    VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamics;
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = r->postVertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";
    VkGraphicsPipelineCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &cbs;
    info.pDynamicState = &dyn;
    info.layout = r->postPipeLayout;
    info.renderPass = r->ctx.compositePass;
    info.subpass = 0;
    return vkCreateGraphicsPipelines(device, r->pipeCache, 1, &info, nullptr, out) == VK_SUCCESS;
}

// The post pass's objects. The pipelines, layout, samplers and uniform
// buffers are extent-independent; the targets follow the extent (and
// the scene target's identity, which the framebuffers name).
bool ensurePost(RendererVk* r) {
    VkDevice device = r->ctx.device;
    const uint32_t w = r->ctx.extent.width;
    const uint32_t h = r->ctx.extent.height;
    if (r->postReady && r->postWidth == w && r->postHeight == h) {
        return true;
    }
    // The depth pipelines + depth-only pass, and the composite pass.
    if (!ensureShadow(r) || !mdk::vk::ensureEyeTargets(&r->ctx)) {
        refused("post pass: the depth or composite pass could not be created, so "
                "the frame renders without occlusion and glow");
        return false;
    }
    if (!r->postVertModule) {
        r->postVertModule = createModule(&r->ctx, spv_stereo_vert, spv_stereo_vert_size);
        if (!r->postVertModule) {
            refused("post pass: the screen vertex module could not be created");
            return false;
        }
    }
    if (!r->postPipeLayout) {
        r->occlusionFragModule = createModule(&r->ctx, spv_occlusion_frag, spv_occlusion_frag_size);
        r->postFragModule = createModule(&r->ctx, spv_post_frag, spv_post_frag_size);
        if (!r->occlusionFragModule || !r->postFragModule) {
            refused("post pass: a screen shader module could not be created, so "
                    "the frame renders without occlusion and glow");
            return false;
        }
        VkDescriptorSetLayoutBinding bindings[4] = {};
        for (int i = 0; i < 3; i++) {
            bindings[i].binding = (uint32_t)i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        bindings[3].binding = 3;
        bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[3].descriptorCount = 1;
        bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 4;
        layoutInfo.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &r->postSetLayout) !=
            VK_SUCCESS) {
            refused("post pass: the descriptor layout could not be created");
            return false;
        }
        VkPipelineLayoutCreateInfo pipeLayoutInfo = {};
        pipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeLayoutInfo.setLayoutCount = 1;
        pipeLayoutInfo.pSetLayouts = &r->postSetLayout;
        if (vkCreatePipelineLayout(device, &pipeLayoutInfo, nullptr, &r->postPipeLayout) !=
            VK_SUCCESS) {
            refused("post pass: the pipeline layout could not be created");
            return false;
        }
        VkDescriptorPoolSize poolSizes[2] = {};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[0].descriptorCount = 3 * mdk::vk::kFramesInFlight;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[1].descriptorCount = mdk::vk::kFramesInFlight;
        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        poolInfo.maxSets = mdk::vk::kFramesInFlight;
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &r->postPool) != VK_SUCCESS) {
            refused("post pass: the descriptor pool could not be created");
            return false;
        }
        for (uint32_t i = 0; i < mdk::vk::kFramesInFlight; i++) {
            VkDescriptorSetAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool = r->postPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts = &r->postSetLayout;
            if (vkAllocateDescriptorSets(device, &allocInfo, &r->postSets[i]) != VK_SUCCESS) {
                refused("post pass: the descriptor sets could not be allocated");
                return false;
            }
            if (!createHostBuffer(&r->ctx, sizeof(PostUniforms),
                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &r->postUniformBuffers[i],
                                  &r->postUniformMemory[i], &r->postUniformMapped[i])) {
                refused("post pass: the uniform buffers could not be created");
                return false;
            }
        }
        // The oracle's _mipSampler (linear, trilinear, clamped) for the
        // scene, and _clampSampler (nearest, clamped) for depth/occlusion.
        VkSamplerCreateInfo samplerInfo = {};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 1000.0f;
        if (vkCreateSampler(device, &samplerInfo, nullptr, &r->postMipSampler) != VK_SUCCESS) {
            refused("post pass: the mip sampler could not be created");
            return false;
        }
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.maxLod = 0.0f;
        if (vkCreateSampler(device, &samplerInfo, nullptr, &r->postClampSampler) != VK_SUCCESS) {
            refused("post pass: the clamp sampler could not be created");
            return false;
        }
        if (!createScreenPipeline(r, r->occlusionFragModule, &r->occlusionPipeline) ||
            !createScreenPipeline(r, r->postFragModule, &r->postPipeline)) {
            refused("post pass: a screen pipeline could not be created, so the "
                    "frame renders without occlusion and glow");
            return false;
        }
        // The canvas-over-post pass: colour LOAD (the post's output is
        // kept), depth cleared, over the scene target. Its pipelines are
        // the scene pass's own -- render-pass compatible (same formats,
        // load ops are not part of compatibility).
        if (!r->canvasLoadPass) {
            VkAttachmentDescription attachments[2] = {};
            attachments[0].format = r->ctx.swapchainFormat;
            attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[0].initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            attachments[1].format = VK_FORMAT_D32_SFLOAT;
            attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            VkAttachmentReference colorRef = {};
            colorRef.attachment = 0;
            colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkAttachmentReference depthRef = {};
            depthRef.attachment = 1;
            depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            VkSubpassDescription subpass = {};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &colorRef;
            subpass.pDepthStencilAttachment = &depthRef;
            VkSubpassDependency dependencies[2] = {};
            dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
            dependencies[0].dstSubpass = 0;
            dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dependencies[0].srcAccessMask = 0;
            dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            dependencies[1].srcSubpass = 0;
            dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
            dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            VkRenderPassCreateInfo passInfo = {};
            passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            passInfo.attachmentCount = 2;
            passInfo.pAttachments = attachments;
            passInfo.subpassCount = 1;
            passInfo.pSubpasses = &subpass;
            passInfo.dependencyCount = 2;
            passInfo.pDependencies = dependencies;
            if (vkCreateRenderPass(device, &passInfo, nullptr, &r->canvasLoadPass) != VK_SUCCESS) {
                refused("post pass: the canvas load pass could not be created");
                return false;
            }
        }
    }

    destroyPostTargets(r);
    // Mips down to a pixel at most (the oracle's SceneMips rule:
    // min(SceneMips, log2(min(w, h)) + 1)).
    uint32_t mips = 1;
    {
        uint32_t smallest = w < h ? w : h;
        while (mips < mdk::kSceneMips && (smallest >> mips) >= 1) {
            mips++;
        }
    }
    const VkFormat colourFormat = r->ctx.swapchainFormat;
    if (!createPostTarget(r, colourFormat,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          w, h, mips, VK_IMAGE_ASPECT_COLOR_BIT, &r->postSceneImage,
                          &r->postSceneMemory, &r->postSceneView) ||
        !createPostTarget(r, VK_FORMAT_D32_SFLOAT,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          w, h, 1, VK_IMAGE_ASPECT_DEPTH_BIT, &r->viewDepthImage,
                          &r->viewDepthMemory, &r->viewDepthView) ||
        !createPostTarget(r, colourFormat,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, w, h,
                          1, VK_IMAGE_ASPECT_COLOR_BIT, &r->occlusionImage, &r->occlusionMemory,
                          &r->occlusionView)) {
        refused("post pass: a target could not be created, so the frame renders "
                "without occlusion and glow");
        destroyPostTargets(r);
        return false;
    }
    {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = r->postSceneImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = colourFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &viewInfo, nullptr, &r->postSceneLevel0) != VK_SUCCESS) {
            refused("post pass: the scene level-0 view could not be created");
            destroyPostTargets(r);
            return false;
        }
    }
    VkFramebufferCreateInfo fbInfo = {};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.width = w;
    fbInfo.height = h;
    fbInfo.layers = 1;
    {
        VkImageView attachments[2] = {r->postSceneLevel0, r->ctx.depthView};
        fbInfo.renderPass = r->ctx.renderPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = attachments;
        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &r->postSceneFramebuffer) != VK_SUCCESS) {
            refused("post pass: the scene framebuffer could not be created");
            destroyPostTargets(r);
            return false;
        }
    }
    {
        VkImageView attachments[1] = {r->viewDepthView};
        fbInfo.renderPass = r->shadowPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = attachments;
        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &r->viewDepthFramebuffer) != VK_SUCCESS) {
            refused("post pass: the depth framebuffer could not be created");
            destroyPostTargets(r);
            return false;
        }
    }
    {
        VkImageView attachments[1] = {r->occlusionView};
        fbInfo.renderPass = r->ctx.compositePass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = attachments;
        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &r->occlusionFramebuffer) != VK_SUCCESS) {
            refused("post pass: the occlusion framebuffer could not be created");
            destroyPostTargets(r);
            return false;
        }
    }
    {
        VkImageView attachments[1] = {r->ctx.sceneView};
        fbInfo.renderPass = r->ctx.compositePass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = attachments;
        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &r->postFramebuffer) != VK_SUCCESS) {
            refused("post pass: the frame framebuffer could not be created");
            destroyPostTargets(r);
            return false;
        }
    }
    {
        VkImageView attachments[2] = {r->ctx.sceneView, r->ctx.depthView};
        fbInfo.renderPass = r->canvasLoadPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = attachments;
        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &r->canvasFramebuffer) != VK_SUCCESS) {
            refused("post pass: the canvas framebuffer could not be created");
            destroyPostTargets(r);
            return false;
        }
    }
    r->postWidth = w;
    r->postHeight = h;
    r->postMips = mips;
    r->postReady = true;
    return true;
}

// The camera's depth: the opaque triangles through the view-projection,
// into the view depth the post pass samples. The oracle's RenderDepth
// with Output.Depth (no depth-layer pull: DrawDepth transforms by
// command.World * viewProjection, nothing else).
void drawCameraDepth(RendererVk* r) {
    VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
    const uint32_t w = r->ctx.extent.width;
    const uint32_t h = r->ctx.extent.height;
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)w;
    viewport.height = (float)h;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor = {{0, 0}, {w, h}};

    float vp[16];
    mulMatrix(r->activeCamera.view, r->activeCamera.projection, vp);
    flipClipY(vp);

    mdk::vk::beginDepthPass(&r->ctx, r->viewDepthFramebuffer, r->shadowPass, w, h);
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    for (int32_t bucketIdx = 0; bucketIdx < mdk::kDrawQueuePassCount; bucketIdx++) {
        const std::vector<mdk::MeshDraw>& bucket = r->queue.bucket(bucketIdx);
        for (size_t i = 0; i < bucket.size(); i++) {
            const mdk::MeshDraw& draw = bucket[i];
            if (draw.primitive != mdk::Primitive::Triangles) {
                continue;
            }
            if (draw.material.pass != mdk::Pass::Solid &&
                draw.material.pass != mdk::Pass::DoubleSided &&
                draw.material.pass != mdk::Pass::Mirror) {
                continue;
            }
            if (draw.mesh < 0 || draw.mesh >= (int32_t)r->meshes.size() ||
                !r->meshes[draw.mesh].used || draw.vertex_count <= 0) {
                continue;
            }
            const MeshVk& mesh = r->meshes[draw.mesh];
            const VkBuffer buffer = mesh.buffer[meshSlot(mesh, &r->ctx)];
            if (!buffer) {
                continue;
            }
            PushConstants pc = {};
            mulMatrix(draw.world, vp, pc.mvp);
            const bool textured = draw.material.pass != mdk::Pass::Mirror &&
                                  draw.material.texture >= 0 &&
                                  draw.material.texture < (int32_t)r->textures.size() &&
                                  r->textures[draw.material.texture].used &&
                                  r->textures[draw.material.texture].isIndex;
            pc.textured = textured ? 1 : 0;
            pc.frame_count = draw.material.frame_count > 0 ? draw.material.frame_count : 1;
            pc.frame = draw.frame;

            VkPipeline pipeline = r->depthPipelines[draw.material.pass == mdk::Pass::Solid ? 0 : 1];
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdPushConstants(cb, r->pipeLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(PushConstants), &pc);
            if (textured) {
                VkDescriptorSet set =
                    getDescriptorSet(r, draw.material.texture, draw.material.palette, true);
                if (!set) {
                    continue;
                }
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeLayout, 0, 1,
                                        &set, 0, nullptr);
            }
            const VkDeviceSize offset = sizeof(mdk::Vertex) * (VkDeviceSize)draw.first_vertex;
            vkCmdBindVertexBuffers(cb, 0, 1, &buffer, &offset);
            vkCmdDraw(cb, (uint32_t)draw.vertex_count, 1, 0, 0);
        }
    }
    mdk::vk::endPass(&r->ctx);

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = r->viewDepthImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

// The scene's mip chain, blitted level by level (the oracle's
// SDL_GenerateMipmapsForGPUTexture), then readable by the post pass.
void generateSceneMips(RendererVk* r) {
    VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = r->postSceneImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.layerCount = 1;
    barrier.subresourceRange.levelCount = 1;

    int32_t w = (int32_t)r->postWidth;
    int32_t h = (int32_t)r->postHeight;
    for (uint32_t level = 1; level < r->postMips; level++) {
        barrier.subresourceRange.baseMipLevel = level - 1;
        barrier.oldLayout = level == 1 ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                       : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = level == 1 ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                           : VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);

        const int32_t nw = w > 1 ? w / 2 : 1;
        const int32_t nh = h > 1 ? h / 2 : 1;
        VkImageBlit blit = {};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = level - 1;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1] = {w, h, 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = level;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[1] = {nw, nh, 1};
        vkCmdBlitImage(cb, r->postSceneImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       r->postSceneImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);

        barrier.subresourceRange.baseMipLevel = level - 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
        w = nw;
        h = nh;
    }
    if (r->postMips > 1) {
        barrier.subresourceRange.baseMipLevel = r->postMips - 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
    } else {
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
    }
}

// Occlusion into its target, then the post into the scene target. The
// uniforms for this frame's slot are written first (its fence was
// waited at begin_frame, so the slot is free to rewrite).
void drawPost(RendererVk* r) {
    const uint32_t slot = r->ctx.currentFrame;
    PostUniforms uniforms = {};
    memcpy(uniforms.clipToView, r->postClipToView, sizeof(uniforms.clipToView));
    memcpy(uniforms.viewToClip, r->postViewToClip, sizeof(uniforms.viewToClip));
    uniforms.screen[0] = (float)r->postWidth;
    uniforms.screen[1] = (float)r->postHeight;
    uniforms.screen[2] = 1.0f / (float)r->postWidth;
    uniforms.screen[3] = 1.0f / (float)r->postHeight;
    uniforms.occlusion[0] = mdk::kOcclusionRadius;
    uniforms.occlusion[1] = mdk::kOcclusionStrength;
    uniforms.occlusion[2] = r->lighting.haze_density;
    uniforms.occlusion[3] = 0.0f;
    uniforms.glow[0] = r->lighting.glow;
    uniforms.glow[1] = mdk::kGlowThreshold;
    // enableGlowBillboards off: zero intensity kills the whole shine
    // term in the shader (SPEC.md 7.9); the pass still runs for the
    // occlusion, which is not a toggle of its own.
    uniforms.glow[2] = r->toggles.enableGlowBillboards ? mdk::kGlowIntensity : 0.0f;
    uniforms.glow[3] = mdk::kGlowMip;
    memcpy(r->postUniformMapped[slot], &uniforms, sizeof(uniforms));

    VkDescriptorImageInfo imageInfos[3] = {};
    imageInfos[0].sampler = r->postMipSampler;
    imageInfos[0].imageView = r->postSceneView;
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[1].sampler = r->postClampSampler;
    imageInfos[1].imageView = r->viewDepthView;
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[2].sampler = r->postClampSampler;
    imageInfos[2].imageView = r->occlusionView;
    imageInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorBufferInfo bufferInfo = {};
    bufferInfo.buffer = r->postUniformBuffers[slot];
    bufferInfo.offset = 0;
    bufferInfo.range = sizeof(PostUniforms);
    VkWriteDescriptorSet writes[4] = {};
    for (int i = 0; i < 3; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = r->postSets[slot];
        writes[i].dstBinding = (uint32_t)i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imageInfos[i];
    }
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = r->postSets[slot];
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[3].pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(r->ctx.device, 4, writes, 0, nullptr);

    VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
    mdk::vk::beginPass(&r->ctx, r->occlusionFramebuffer, r->ctx.compositePass, 0);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->occlusionPipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->postPipeLayout, 0, 1,
                            &r->postSets[slot], 0, nullptr);
    vkCmdDraw(cb, 3, 1, 0, 0);
    mdk::vk::endPass(&r->ctx);

    // The occlusion target leaves the composite pass in PRESENT_SRC;
    // the post pass samples it, so it changes layout and hands its
    // writes over explicitly (the pass's own dependency names the
    // present blit as the reader, not this shader).
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = r->occlusionImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);

    mdk::vk::beginPass(&r->ctx, r->postFramebuffer, r->ctx.compositePass, 0);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->postPipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->postPipeLayout, 0, 1,
                            &r->postSets[slot], 0, nullptr);
    vkCmdDraw(cb, 3, 1, 0, 0);
    mdk::vk::endPass(&r->ctx);
}

// Record one queued draw into this frame's command buffer. This is what the
// oracle's `DrawOne` is (Renderer.cs:1060-1110): a pipeline for the pass, the
// matrices, the texture pair, one draw.
//
// It runs at FLUSH time, not at queue time, so the frame-wide state it reads --
// the camera, the viewport, the render pass -- is the state the frame is drawn
// with, exactly as the C# reads `view.ViewProjection` when it renders the
// command rather than when the view queued it.
void drawRecorded(RendererVk* r, const mdk::MeshDraw* draw) {
    if (draw->mesh < 0 || draw->mesh >= (int32_t)r->meshes.size() || !r->meshes[draw->mesh].used) {
        return;
    }
    if (draw->vertex_count <= 0) {
        return;
    }

    // clip = p * world * depth_pull * view * projection -- the row-vector
    // chain, the same order the C# uses (Renderer.cs:1072). The depth pull
    // keeps a detail coplanar with the surface under it in the same place on
    // screen, but nearer in the depth buffer.
    float world[16];
    if (draw->material.depth_layer != 0 || draw->primitive == mdk::Primitive::Lines) {
        float pulled[16];
        mdk::buildDepthPullMatrix(r->activeCamera.position, draw->primitive,
                                  draw->material.depth_layer, pulled);
        mulMatrix(draw->world, pulled, world);
    } else {
        memcpy(world, draw->world, sizeof(world));
    }
    float wv[16];
    mulMatrix(world, r->activeCamera.view, wv);

    // The enhanced look's Lit/Sprite draws go through the lit pipeline;
    // everything else (Original shading, mirrors, the canvas's own draws)
    // keeps the palette path below.
    if (r->enhancedLook && r->enhancedReady &&
        (draw->material.shading == mdk::Shading::Lit ||
         draw->material.shading == mdk::Shading::Sprite)) {
        drawEnhanced(r, draw, world, wv);
        return;
    }

    PushConstants pc;
    mulMatrix(wv, r->activeCamera.projection, pc.mvp);
    // Vulkan's framebuffer runs y down; see flipClipY.
    flipClipY(pc.mvp);
    pc.colour[0] = draw->material.colour.r;
    pc.colour[1] = draw->material.colour.g;
    pc.colour[2] = draw->material.colour.b;
    pc.colour[3] = draw->material.colour.a;
    pc.textured = 0;
    if (draw->material.texture >= 0 &&
        draw->material.texture < (int32_t)r->textures.size() &&
        r->textures[draw->material.texture].used) {
        pc.textured = r->textures[draw->material.texture].isIndex ? 1 : 2;
    }
    pc.frame_count = draw->material.frame_count > 0 ? draw->material.frame_count : 1;
    pc.frame = draw->frame;
    pc.unused_ = 0;

    const int pass = (int)draw->material.pass;
    const int blend = (int)draw->material.blend;
    const int primitive = (draw->primitive == mdk::Primitive::Lines) ? 1 : 0;
    VkPipeline pipeline = getPipeline(r, pass, blend, primitive);
    if (!pipeline) {
        return;
    }

    VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdPushConstants(cb, r->pipeLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &pc);

    if (pc.textured != 0) {
        // A surface's texture REPEATS, like the C#'s (Renderer.cs:1098).
        VkDescriptorSet set = getDescriptorSet(r, draw->material.texture, draw->material.palette, true);
        if (!set) {
            // SKIPPED, NOT DRAWN BLIND. A textured draw that does not bind a set
            // samples whatever the PREVIOUS draw bound: a wall wearing another
            // surface's pixels, with nothing in the log to say so. That is a
            // wrong picture, which is worse than the missing one every other
            // refusal here produces (review-5 F1).
            return;
        }
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeLayout, 0, 1,
                                &set, 0, nullptr);
    }

    // THE SLOT, not "the" buffer: a dynamic mesh is drawn from the slot this
    // frame wrote, which is the one whose fence beginFrame waited.
    const MeshVk& mesh = r->meshes[draw->mesh];
    const VkBuffer buffer = mesh.buffer[meshSlot(mesh, &r->ctx)];
    if (!buffer) {
        return;
    }
    const VkDeviceSize offset = sizeof(mdk::Vertex) * (VkDeviceSize)draw->first_vertex;
    vkCmdBindVertexBuffers(cb, 0, 1, &buffer, &offset);
    vkCmdDraw(cb, (uint32_t)draw->vertex_count, 1, 0, 0);

    r->drawCalls++;
    if (!primitive) {
        r->triangles += draw->vertex_count / 3;
    }
}

// Draw everything the frame has queued, in the oracle's pass order: every
// `Solid` batch, then `DoubleSided`, `Mirror`, `Blended`, `OnTop` and
// `Overlay`, each bucket in the order it was queued (draw_queue.h).
//
// The flush point is the end of the frame's 3D scene, which is where the oracle
// closes its render pass: before the 2D canvas, before an inset changes the
// viewport, and at the frame's end. A frontend that drew a mesh AFTER its
// canvas would get a second, later group -- the canvas is Overlay and the
// oracle draws the canvas last, so nothing correct does that.
void flushDraws(RendererVk* r) {
    if (!r || r->queue.empty() || !r->inFrame) {
        return;
    }
    // Under stereo the queue is replayed once per eye at end_frame, so a
    // mid-frame flush (canvas_begin, an inset boundary) must not consume
    // it: the draws wait and are recorded twice, once through each eye's
    // camera. The canvas's own immediate quads are not queued and still
    // draw into the left eye's pass only -- the seam hands the backend no
    // way to replay a frontend's canvas calls, so the HUD appears in the
    // left half of a stereo frame. Stated here because the composite
    // cannot say it.
    if (r->stereo.mode != mdk::StereoMode::Off && !r->replayingEyes) {
        return;
    }
    // Under the enhanced look the scene is deferred to end_frame (the
    // shadow pass reads the whole queue first), so a mid-frame flush
    // waits for the replay there too.
    if (r->enhancedLook && !r->replayingEyes) {
        return;
    }
    for (int32_t pass = 0; pass < mdk::kDrawQueuePassCount; pass++) {
        const std::vector<mdk::MeshDraw>& bucket = r->queue.bucket(pass);
        for (size_t i = 0; i < bucket.size(); i++) {
            // enableMirrors off: mirror draws are skipped whole, at
            // queue setup, not shaded around (SPEC.md 7.9).
            if (!r->toggles.enableMirrors && bucket[i].material.pass == mdk::Pass::Mirror) {
                continue;
            }
            drawRecorded(r, &bucket[i]);
        }
    }
    if (!r->replayingEyes) {
        r->queue.reset();
    }
}

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

// The C# Renderer.CanvasWidth: 360 units high, as wide as the current target.
float canvasWidth(RendererVk* r) {
    const uint32_t w = r ? r->ctx.extent.width : 0;
    const uint32_t h = r ? r->ctx.extent.height : 0;
    const float aspect = h != 0 ? (float)w / (float)h : 4.0f / 3.0f;
    return mdk::kCanvasHeight * aspect;
}

// The orthographic matrix mapping canvas pixels (x right, y DOWN) to clip
// space over the full viewport. The canvas is screen space, so it is written
// straight for the framebuffer: canvas y 0 is the top row, which is clip y -1
// with Vulkan's y-down framebuffer. That is the C#'s
// CreateOrthographicOffCenter(..., CanvasHeight + margin, -margin, ...) with
// its y arguments swapped, expressed in the engine's row-major row-vector
// layout (the translation lives in row 3, indices 12..14).
void canvasOrtho(float out[16], float width) {
    for (int i = 0; i < 16; i++) {
        out[i] = 0.0f;
    }
    out[0] = 2.0f / width;      // x -> [-1,1]
    out[5] = 2.0f / mdk::kCanvasHeight; // y (down) -> clip y down, the framebuffer's own sense
    out[10] = 1.0f;
    out[12] = -1.0f;
    out[13] = -1.0f;
    out[15] = 1.0f;
}

bool ensureCanvasBuffer(RendererVk* r, VkDeviceSize size) {
    const uint32_t slot = r->ctx.currentFrame;
    if (r->canvasBuffer[slot] && r->canvasCapacity >= size) {
        return true;
    }
    // The ring is resized as a unit, and the slot that is NOT this frame's may
    // belong to a submission that is still running, so nothing here is
    // destroyed: it is retired and freed at the frame boundary that proves the
    // GPU has finished with it (see MeshVk).
    for (uint32_t i = 0; i < mdk::vk::kFramesInFlight; i++) {
        retireBuffer(r, r->canvasBuffer[i], r->canvasMemory[i]);
        r->canvasBuffer[i] = VK_NULL_HANDLE;
        r->canvasMemory[i] = VK_NULL_HANDLE;
        r->canvasMapped[i] = nullptr;
    }
    r->canvasCapacity = 0;
    const VkDeviceSize cap = size < (64 * 1024) ? (64 * 1024) : size;
    for (uint32_t i = 0; i < mdk::vk::kFramesInFlight; i++) {
        if (!createHostBuffer(&r->ctx, cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              &r->canvasBuffer[i], &r->canvasMemory[i], &r->canvasMapped[i])) {
            for (uint32_t done = 0; done < i; done++) {
                destroyBufferPair(r->ctx.device, r->canvasBuffer[done], r->canvasMemory[done]);
                r->canvasBuffer[done] = VK_NULL_HANDLE;
                r->canvasMemory[done] = VK_NULL_HANDLE;
                r->canvasMapped[done] = nullptr;
            }
            refused("canvas ring of %llu bytes total, %llu per slot, x %u frames in "
                    "flight: the HUD and every other canvas quad from this frame is "
                    "dropped",
                    (unsigned long long)(cap * mdk::vk::kFramesInFlight),
                    (unsigned long long)cap, (unsigned)mdk::vk::kFramesInFlight);
            return false;
        }
    }
    r->canvasCapacity = cap;
    return true;
}

} // namespace

void mdk_render_canvas_begin(MdkRenderer r) {
    RendererVk* vk = asVk(r);
    if (vk) {
        // The scene ends here: the queued draws go in BEFORE any canvas quad,
        // because the canvas is the Overlay pass drawn over the scene and the
        // oracle draws its Overlay bucket last (draw_queue.h).
        flushDraws(vk);
        vk->canvasOffset = 0;
        vk->canvasRecords.clear();
    }
}

// The deferred canvas replay: the quads a deferred (enhanced) frame
// recorded, drawn into the replayed scene pass in order.
void replayCanvas(RendererVk* r) {
    const uint32_t slot = r->ctx.currentFrame;
    for (size_t i = 0; i < r->canvasRecords.size(); i++) {
        const RendererVk::CanvasRecord& rec = r->canvasRecords[i];
        // The enhanced look filters the canvas (its ModeOf routes the
        // Overlay pass to the enhanced shader); the original look replays
        // through the palette pipeline below.
        if (r->enhancedLook && r->enhancedReady) {
            drawEnhancedCanvas(r, rec.texture, rec.palette, rec.frameCount, rec.colour,
                               rec.offset);
            continue;
        }
        PushConstants pc = {};
        canvasOrtho(pc.mvp, canvasWidth(r));
        pc.colour[0] = rec.colour[0];
        pc.colour[1] = rec.colour[1];
        pc.colour[2] = rec.colour[2];
        pc.colour[3] = rec.colour[3];
        pc.textured = (rec.texture >= 0) ? ((rec.palette >= 0) ? 1 : 2) : 0;
        pc.frame_count = rec.frameCount;
        pc.frame = 0;
        pc.unused_ = 0;

        VkPipeline pipeline = getPipeline(r, (int)mdk::Pass::Overlay, (int)mdk::Blend::Alpha, 0);
        if (!pipeline) {
            continue;
        }
        VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdPushConstants(cb, r->pipeLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(PushConstants), &pc);
        if (pc.textured != 0) {
            VkDescriptorSet set = getDescriptorSet(r, rec.texture, rec.palette, true);
            if (!set) {
                continue;
            }
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeLayout, 0, 1,
                                    &set, 0, nullptr);
        }
        VkDeviceSize offset = rec.offset;
        vkCmdBindVertexBuffers(cb, 0, 1, &r->canvasBuffer[slot], &offset);
        vkCmdDraw(cb, 6, 1, 0, 0);
        r->drawCalls++;
    }
    r->canvasRecords.clear();
}

void mdk_render_canvas_quad(MdkRenderer rr, const mdk::CanvasQuad* q) {
    RendererVk* r = asVk(rr);
    if (!r || !r->ready || !r->inFrame || !q) {
        return;
    }

    const float xs[4] = {q->x0, q->x1, q->x2, q->x3};
    const float ys[4] = {q->y0, q->y1, q->y2, q->y3};
    const float us[4] = {q->u0, q->u1, q->u2, q->u3};
    const float vs[4] = {q->v0, q->v1, q->v2, q->v3};
    const int order[6] = {0, 1, 2, 0, 2, 3};

    mdk::Vertex verts[6];
    auto pack = [](float c) -> uint32_t {
        int v = (int)(c * 255.0f + 0.5f);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        return (uint32_t)v;
    };
    const uint32_t packed = pack(q->colour.r) | (pack(q->colour.g) << 8) |
                            (pack(q->colour.b) << 16) | (pack(q->colour.a) << 24);
    for (int i = 0; i < 6; i++) {
        const int c = order[i];
        verts[i].pos = mdk::Vec3{xs[c], ys[c], 0.0f};
        verts[i].uv = mdk::Vec2{us[c], vs[c]};
        verts[i].colour = packed;
    }

    const VkDeviceSize bytes = sizeof(verts);
    if (!ensureCanvasBuffer(r, r->canvasOffset + bytes)) {
        return;
    }
    const uint32_t slot = r->ctx.currentFrame;
    if (!r->canvasMapped[slot]) {
        refused("canvas slot %u has no mapping after a resize: this quad is not drawn", slot);
        return;
    }
    memcpy((char*)r->canvasMapped[slot] + r->canvasOffset, verts, bytes);

    PushConstants pc = {};
    canvasOrtho(pc.mvp, canvasWidth(r));
    pc.colour[0] = q->colour.r;
    pc.colour[1] = q->colour.g;
    pc.colour[2] = q->colour.b;
    pc.colour[3] = q->colour.a;
    pc.textured = (q->texture >= 0) ? ((q->palette >= 0) ? 1 : 2) : 0;
    pc.frame_count = q->frame_count > 0 ? q->frame_count : 1;
    pc.frame = 0;
    pc.unused_ = 0;

    // Under a deferred (enhanced) frame the quad is recorded, not drawn:
    // the scene pass it belongs to is replayed at end_frame, and the
    // canvas is drawn there, over the scene, in order (replayCanvas).
    if (r->deferScene) {
        RendererVk::CanvasRecord rec;
        rec.offset = r->canvasOffset;
        rec.texture = q->texture;
        rec.palette = q->palette;
        rec.frameCount = pc.frame_count;
        rec.colour[0] = q->colour.r;
        rec.colour[1] = q->colour.g;
        rec.colour[2] = q->colour.b;
        rec.colour[3] = q->colour.a;
        r->canvasRecords.push_back(rec);
        r->canvasOffset += bytes;
        return;
    }

    // The enhanced look filters the canvas even when the frame is not
    // deferred (stereo eye passes draw the canvas immediately).
    if (r->enhancedLook && r->enhancedReady) {
        drawEnhancedCanvas(r, q->texture, q->palette, pc.frame_count, pc.colour,
                           r->canvasOffset);
        r->canvasOffset += bytes;
        return;
    }

    VkPipeline pipeline = getPipeline(r, (int)mdk::Pass::Overlay, (int)mdk::Blend::Alpha, 0);
    if (!pipeline) {
        return;
    }
    VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdPushConstants(cb, r->pipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &pc);
    if (pc.textured != 0) {
        // The canvas's image repeats too: the C# draws it as an Overlay
        // material, and the Overlay path binds the REPEAT sampler.
        VkDescriptorSet set = getDescriptorSet(r, q->texture, q->palette, true);
        if (!set) {
            return;  // this quad is not drawn; see drawRecorded (review-5 F1)
        }
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeLayout, 0, 1, &set,
                                0, nullptr);
    }
    VkDeviceSize offset = r->canvasOffset;
    vkCmdBindVertexBuffers(cb, 0, 1, &r->canvasBuffer[slot], &offset);
    vkCmdDraw(cb, 6, 1, 0, 0);
    r->canvasOffset += bytes;
    r->drawCalls++;
}

void mdk_render_canvas_end(MdkRenderer r) { (void)r; }
float mdk_render_canvas_width(MdkRenderer r) {
    return canvasWidth(asVk(r));
}
int32_t mdk_render_canvas_capacity(MdkRenderer r) {
    (void)r;
    return 4096;
}

// ---------------------------------------------------------------------------
// Insets
// ---------------------------------------------------------------------------

// An inset is a 3D view inside a canvas rectangle, so its draws are their own
// scene group: the queued draws go in before the viewport changes and before it
// is restored, in the inset's own rectangle. This backend has no inset support
// yet (the rectangle is ignored), but the queue boundary is still the right one
// for the day it has.
void mdk_render_begin_inset(MdkRenderer r, const mdk::Inset* i) {
    (void)i;
    flushDraws(asVk(r));
}
void mdk_render_end_inset(MdkRenderer r) { flushDraws(asVk(r)); }

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

void mdk_render_stats(MdkRenderer r, int32_t* draw_calls, int32_t* triangles) {
    RendererVk* vr = asVk(r);
    if (draw_calls) {
        *draw_calls = vr ? vr->drawCalls : 0;
    }
    if (triangles) {
        *triangles = vr ? vr->triangles : 0;
    }
}

float mdk_render_gpu_wait_ms(MdkRenderer r) {
    (void)r;
    return 0.0f;
}

void mdk_render_capture_stats(MdkRenderer r, int32_t* requested, int32_t* written) {
    RendererVk* vr = asVk(r);
    if (requested) {
        *requested = vr ? vr->capturesRequested : 0;
    }
    if (written) {
        *written = vr ? vr->capturesWritten : 0;
    }
}
