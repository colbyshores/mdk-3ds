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
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "render/depth_pull.h"
#include "render/draw_queue.h"
#include "render/render.h"
#include "render/render_types.h"
#include "render/system_numerics_inverse.h"
#include "render/vk/vk_context.h"

#include "shader_palette_vert.h"
#include "shader_palette_frag.h"
#include "shader_sky_vert.h"
#include "shader_sky_frag.h"

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
};

struct PaletteVk {
    bool used = false;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    void* mapped = nullptr;
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
    mdk::PointLight pointLights[64];
    int32_t pointLightCount = 0;

    mdk::Panorama panorama;
    float fade = 1.0f;
    float gamma = 1.0f;

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
    // Only the SET. The image view it names is destroyed at the drop site, in the
    // same call that retired this one, and has no deadline of its own — see
    // `retireDescriptorSet`, which says so, and the open item that is the real
    // fix. Do not read this paragraph as a completion boundary for the pair.
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
// THE VIEW DOES NOT WAIT, and that is a gap rather than a design. Every caller
// destroys the image, its view and its memory in the same call that retires this
// set, so a frame still on the queue can be sampling a destroyed view while only
// the handle has been deferred. AGENTS.md P3 asks for the boundary on the whole
// resource, not on half of it; giving the image its own deadline is a separate
// slice (a `RetiredImage` beside this struct, settled here) and is recorded as an
// open item instead of being implied by the paragraph above.
void retireDescriptorSet(RendererVk* r, VkDescriptorSet set) {
    if (!set) {
        return;
    }
    RetiredSet retired;
    retired.set = set;
    retired.safeFrame = r->frameIndex + mdk::vk::kFramesInFlight;
    r->retiringSets.push_back(retired);
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
bool writeBmp(const char* path, uint32_t width, uint32_t height, const uint8_t* bgra) {
    if (!path || !bgra || width == 0 || height == 0) {
        return false;
    }
    const uint64_t pixelBytes = (uint64_t)width * (uint64_t)height * 4u;
    const uint64_t fileBytes = 54u + pixelBytes;
    if (fileBytes > UINT32_MAX) {
        return false;
    }

    FILE* file = std::fopen(path, "wb");
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
        std::printf("[vk] screenshot: could not write %s\n", path);
    }
    return ok;
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
    mulMatrix(rotation, r->camera.projection, rp);
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

    // The sky is drawn first, before the level (SPEC.md 6.3 step 2).
    drawSky(r);
}

void mdk_render_end_frame(MdkRenderer rr) {
    RendererVk* r = asVk(rr);
    if (!r || !r->inFrame) {
        return;
    }
    // The frame's queued draws go in now, in the oracle's pass order, while the
    // render pass is still open (draw_queue.h). A frontend that drew its HUD
    // already flushed at canvas_begin; this is the catch-all for a frame with no
    // canvas, and it must come before endFrame closes the render pass.
    flushDraws(r);
    // THE FRAME'S OWN RESULT WAS DISCARDED ENTIRELY. `endFrame` returns false when
    // the fence reset, the submit or the present failed; until now the frame was
    // reported as rendered either way, so a pinned frame whose commands never ran
    // would be measured, published and committed as if it were the picture.
    const bool submitted = mdk::vk::endFrame(&r->ctx);
    r->inFrame = false;
    if (!submitted) {
        refused("end the frame: its commands were not submitted or not presented, so this "
                "frame did not reach the display");
    }
    if (!r->screenshotPath.empty()) {
        const std::string path = r->screenshotPath;
        r->screenshotPath.clear();
        if (!submitted) {
            // The readback would succeed and print its usual "wrote N" line about an
            // image this frame never drew into. A frame record that cannot be trusted
            // is worse than no frame record, because a frame record gets committed.
            refused("capture the frame to %s: the frame it was asked to capture was never "
                    "submitted, so the image would not be this frame's",
                    path.c_str());
        } else {
            captureSwapchain(r, path.c_str());
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
    VkImage oldImage = t.image;
    VkDeviceMemory oldMemory = t.memory;
    VkImageView oldView = t.view;
    t.image = VK_NULL_HANDLE;
    t.memory = VK_NULL_HANDLE;
    t.view = VK_NULL_HANDLE;
    if (createHostImage(&r->ctx, w, h, t.isIndex ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM,
                        t.isIndex ? 1u : 4u, px, &t.image, &t.memory, &t.view, &t.mapped)) {
        destroyImage(&r->ctx, &oldImage, &oldMemory, &oldView, nullptr);
    } else {
        refused("re-made texture %d at %dx%d: keeping the image it had, so an animated "
                "surface freezes on its last frame instead of disappearing", t_, w, h);
        t.image = oldImage;
        t.memory = oldMemory;
        t.view = oldView;
    }
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
    destroyImage(&r->ctx, &r->textures[t_].image, &r->textures[t_].memory,
                 &r->textures[t_].view, &r->textures[t_].mapped);
    r->textures[t_].used = false;
}

void mdk_render_release_palette(MdkRenderer r_, MdkPaletteId p_) {
    RendererVk* r = asVk(r_);
    if (!r || p_ < 0 || p_ >= (int32_t)r->palettes.size() || !r->palettes[p_].used) {
        return;
    }
    forgetDescriptorSets(r, -1, p_);
    destroyImage(&r->ctx, &r->palettes[p_].image, &r->palettes[p_].memory,
                 &r->palettes[p_].view, &r->palettes[p_].mapped);
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
        mdk::buildDepthPullMatrix(r->camera.position, draw->primitive,
                                  draw->material.depth_layer, pulled);
        mulMatrix(draw->world, pulled, world);
    } else {
        memcpy(world, draw->world, sizeof(world));
    }
    float wv[16];
    mulMatrix(world, r->camera.view, wv);

    PushConstants pc;
    mulMatrix(wv, r->camera.projection, pc.mvp);
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
    for (int32_t pass = 0; pass < mdk::kDrawQueuePassCount; pass++) {
        const std::vector<mdk::MeshDraw>& bucket = r->queue.bucket(pass);
        for (size_t i = 0; i < bucket.size(); i++) {
            drawRecorded(r, &bucket[i]);
        }
    }
    r->queue.reset();
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
    }
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
