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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "render/render.h"
#include "render/render_types.h"
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

struct MeshVk {
    bool used = false;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize capacity = 0;
};

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

    // The 2D canvas: quads accumulated between begin/end, drawn in one
    // orthographic Overlay pass. A single dynamic vertex buffer is resized as
    // needed.
    VkBuffer canvasBuffer = VK_NULL_HANDLE;
    VkDeviceMemory canvasMemory = VK_NULL_HANDLE;
    void* canvasMapped = nullptr;
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

RendererVk* asVk(MdkRenderer r) {
    return static_cast<RendererVk*>(r);
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

// The one-shot pools are few (one per resource creation at load time); keep
// them in a small vector so endOneShot can destroy them. Creations are
// sequential (begin, use, end), so the front of the vector is always the
// pool the command buffer came from.
std::vector<VkCommandPool> g_oneShotPools;

void endOneShot(mdk::vk::Context* ctx, VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    vkQueueSubmit(ctx->queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx->queue);
    // Find and destroy the pool this command buffer came from.
    for (size_t i = 0; i < g_oneShotPools.size(); i++) {
        // vkFreeCommandBuffers with the right pool; the pools are used in
        // order, so pop the front.
        vkFreeCommandBuffers(ctx->device, g_oneShotPools[i], 1, &cb);
        vkDestroyCommandPool(ctx->device, g_oneShotPools[i], nullptr);
        g_oneShotPools.erase(g_oneShotPools.begin() + (long)i);
        break;
    }
}

VkCommandBuffer beginOneShotTracked(mdk::vk::Context* ctx) {
    VkCommandPoolCreateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    alloc.queueFamilyIndex = ctx->queueFamily;
    alloc.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(ctx->device, &alloc, nullptr, &pool) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    g_oneShotPools.push_back(pool);
    VkCommandBufferAllocateInfo cbAlloc = {};
    cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAlloc.commandPool = pool;
    cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(ctx->device, &cbAlloc, &cb);
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);
    return cb;
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
    vkBindBufferMemory(ctx->device, *outBuffer, *outMemory, 0);
    if (outMapped) {
        if (vkMapMemory(ctx->device, *outMemory, 0, VK_WHOLE_SIZE, 0, outMapped) != VK_SUCCESS) {
            vkFreeMemory(ctx->device, *outMemory, nullptr);
            vkDestroyBuffer(ctx->device, *outBuffer, nullptr);
            *outBuffer = VK_NULL_HANDLE;
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
    vkBindImageMemory(ctx->device, *outImage, *outMemory, 0);
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
        vkFreeMemory(ctx->device, *outMemory, nullptr);
        *outMemory = VK_NULL_HANDLE;
        vkDestroyImage(ctx->device, *outImage, nullptr);
        *outImage = VK_NULL_HANDLE;
        return false;
    }
    if (pixels) {
        memcpy(stagingMapped, pixels, (size_t)size);
        vkUnmapMemory(ctx->device, stagingMemory);
    }

    VkCommandBuffer cb = beginOneShotTracked(ctx);
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
        endOneShot(ctx, cb);
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
    VkCommandBuffer cb = beginOneShotTracked(&r->ctx);
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
    endOneShot(&r->ctx, cb);

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
    vkCreateShaderModule(ctx->device, &info, nullptr, &module);
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
        std::printf("[vk] pipeline create failed (pass %d blend %d prim %d)\n", pass, blend, primitive);
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
        std::printf("[vk] sky pipeline create failed\n");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

// A general 4x4 inverse, row-major in and out (the engine's convention).
// Returns false for a singular matrix.
bool invertMatrix4(const float m[16], float out[16]) {
    float inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] +
             m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] -
             m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] +
             m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] -
              m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] -
             m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] +
             m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] -
             m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] +
              m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] +
             m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] -
             m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] +
              m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] -
              m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] -
             m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] +
             m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] -
              m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] +
              m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det == 0.0f) {
        return false;
    }
    const float k = 1.0f / det;
    for (int i = 0; i < 16; i++) {
        out[i] = inv[i] * k;
    }
    return true;
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

    float rotation[16];
    memcpy(rotation, r->camera.view, 12 * sizeof(float));
    rotation[12] = rotation[13] = rotation[14] = 0.0f;
    rotation[15] = 1.0f;
    float rp[16];
    mulMatrix(rotation, r->camera.projection, rp);
    float c2d[16];
    if (!invertMatrix4(rp, c2d)) {
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
    if (set) {
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeLayout, 0, 1, &set, 0,
                                nullptr);
    }
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
        vkCreatePipelineCache(r->ctx.device, &cacheInfo, nullptr, &r->pipeCache);
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
        vkCreateSampler(r->ctx.device, &samplerInfo, nullptr, &r->sampler);
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
        vkCreateSampler(r->ctx.device, &samplerInfo, nullptr, &r->samplerRepeat);
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
    vkDeviceWaitIdle(r->ctx.device);
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
            if (r->meshes[i].buffer) vkDestroyBuffer(r->ctx.device, r->meshes[i].buffer, nullptr);
            if (r->meshes[i].memory) vkFreeMemory(r->ctx.device, r->meshes[i].memory, nullptr);
        }
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
    if (!mdk::vk::beginFrame(&r->ctx, w, h, true, clear)) {
        return;
    }
    r->inFrame = true;
    r->drawCalls = 0;
    r->triangles = 0;
    r->screenshotPath = params && params->screenshot_path ? params->screenshot_path : "";

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
    mdk::vk::endFrame(&r->ctx);
    r->inFrame = false;
    if (!r->screenshotPath.empty()) {
        const std::string path = r->screenshotPath;
        r->screenshotPath.clear();
        captureSwapchain(r, path.c_str());
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
    destroyImage(&r->ctx, &r->textures[t_].image, &r->textures[t_].memory,
                 &r->textures[t_].view, &r->textures[t_].mapped);
    r->textures[t_].used = false;
}

void mdk_render_release_palette(MdkRenderer r_, MdkPaletteId p_) {
    RendererVk* r = asVk(r_);
    if (!r || p_ < 0 || p_ >= (int32_t)r->palettes.size() || !r->palettes[p_].used) {
        return;
    }
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
    if (!createHostBuffer(&r->ctx, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &m.buffer,
                          &m.memory, &m.mapped)) {
        m.used = false;
        return -1;
    }
    memcpy(m.mapped, v, (size_t)size);
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
    if (!createHostBuffer(&r->ctx, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &m.buffer,
                          &m.memory, &m.mapped)) {
        m.used = false;
        return -1;
    }
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
        return;
    }
    memcpy(m.mapped, v, (size_t)size);
}

void mdk_render_release_mesh(MdkRenderer r_, MdkMeshId m_) {
    RendererVk* r = asVk(r_);
    if (!r || m_ < 0 || m_ >= (int32_t)r->meshes.size() || !r->meshes[m_].used) {
        return;
    }
    MeshVk& m = r->meshes[m_];
    if (m.buffer) vkDestroyBuffer(r->ctx.device, m.buffer, nullptr);
    if (m.memory) vkFreeMemory(r->ctx.device, m.memory, nullptr);
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

void mdk_render_draw_mesh(MdkRenderer rr, const mdk::MeshDraw* draw) {
    RendererVk* r = asVk(rr);
    if (!r || !draw || !r->inFrame) {
        return;
    }
    if (draw->mesh < 0 || draw->mesh >= (int32_t)r->meshes.size() || !r->meshes[draw->mesh].used) {
        return;
    }
    if (draw->vertex_count <= 0) {
        return;
    }

    // clip = p * world * view * projection -- the row-vector chain, the
    // same order the C# uses (world first, then view, then projection).
    float wv[16];
    mulMatrix(draw->world, r->camera.view, wv);

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
        if (set) {
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeLayout, 0, 1,
                                    &set, 0, nullptr);
        }
    }

    const VkDeviceSize offset = sizeof(mdk::Vertex) * (VkDeviceSize)draw->first_vertex;
    vkCmdBindVertexBuffers(cb, 0, 1, &r->meshes[draw->mesh].buffer, &offset);
    vkCmdDraw(cb, (uint32_t)draw->vertex_count, 1, 0, 0);

    r->drawCalls++;
    if (!primitive) {
        r->triangles += draw->vertex_count / 3;
    }
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
    if (r->canvasBuffer && r->canvasCapacity >= size) {
        return true;
    }
    if (r->canvasBuffer) {
        vkDestroyBuffer(r->ctx.device, r->canvasBuffer, nullptr);
        vkFreeMemory(r->ctx.device, r->canvasMemory, nullptr);
        r->canvasBuffer = VK_NULL_HANDLE;
        r->canvasMemory = VK_NULL_HANDLE;
        r->canvasMapped = nullptr;
    }
    VkDeviceSize cap = size < (64 * 1024) ? (64 * 1024) : size;
    if (!createHostBuffer(&r->ctx, cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &r->canvasBuffer,
                          &r->canvasMemory, &r->canvasMapped)) {
        r->canvasCapacity = 0;
        return false;
    }
    r->canvasCapacity = cap;
    return true;
}

} // namespace

void mdk_render_canvas_begin(MdkRenderer r) {
    RendererVk* vk = asVk(r);
    if (vk) {
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
    memcpy((char*)r->canvasMapped + r->canvasOffset, verts, bytes);

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
        if (set) {
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeLayout, 0, 1, &set,
                                    0, nullptr);
        }
    }
    VkDeviceSize offset = r->canvasOffset;
    vkCmdBindVertexBuffers(cb, 0, 1, &r->canvasBuffer, &offset);
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

void mdk_render_begin_inset(MdkRenderer r, const mdk::Inset* i) { (void)r; (void)i; }
void mdk_render_end_inset(MdkRenderer r) { (void)r; }

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
