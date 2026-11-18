// ============================================================================
// vk_context.cpp — the Vulkan device, swapchain and per-frame resources.
//
// See vk_context.h for why the headers are vendored and the entry points
// come through volk.
// ============================================================================

#include "render/vk/vk_context.h"

#include <volk.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace mdk {
namespace vk {

namespace {

// A FAILURE THAT SPEAKS.
//
// Every result-bearing Vulkan call in this file has its result read, and every
// init-path one that can fail says so: a device that refuses to build a swapchain
// and prints nothing is a black window with a green build, which is the same
// invisible-refusal defect `renderer_vk.cpp`'s `refused()` exists for.
//
// WHY THIS IS NOT THAT HELPER. `refused()` remembers every distinct request so a
// per-draw refusal prints once; this file has no such memory and must not
// duplicate it. That is fine because the split is by how often a site can repeat:
// everything here runs once per process (instance, device, swapchain, depth,
// frame resources), so a plain line cannot flood. The FRAME path is the one that
// repeats, and it is reported by the seam instead — `beginFrame`/`endFrame`
// return false quietly and `mdk_render_begin_frame`/`mdk_render_end_frame` print
// through `refused()`, which is the layer that owns refusal reporting.
//
// It flushes for the reason the seam's helper flushes: a refusal is exactly the
// moment the process may die with the line that explains it still in the buffer.
void vkFailed(const char* what, VkResult result) {
    std::printf("[vk] %s failed (VkResult %d)\n", what, (int)result);
    std::fflush(stdout);
}

// A refusal with no VkResult to report: the call succeeded and the ANSWER is the
// problem (a swapchain that reports zero images, say).
void vkRefused(const char* what) {
    std::printf("[vk] REFUSED %s\n", what);
    std::fflush(stdout);
}

// CONSUMING AN ACQUIRE THAT NO FRAME WILL USE.
//
// `presentWait` is a BINARY semaphore. It is created with no type info, so it has two
// states — signalled and unsignalled — and no count. A successful acquire posts it and
// the only thing that clears it is a submit that waits on it. So a frame that acquired
// an image and then refused left the semaphore signalled, and the NEXT frame's acquire
// would pass an already-signalled binary semaphore to `vkAcquireNextImageKHR`, which is
// invalid use (VUID-vkAcquireNextImageKHR-semaphore-01779) with no specified answer.
//
// The comment this replaces called the leftover "a stale +1 on a semaphore, not a leak
// and not a hang". That reasoned about an arithmetic the object cannot represent: a
// binary semaphore is never +1 (review-1 F4). It is also not unfixable. Vulkan cannot
// WITHDRAW a signal from the CPU, but it can CONSUME one: an empty submit — no command
// buffers, no signal, just this wait — retires the signal as soon as the queue reaches
// it, and the queue reaches it at once because the signal is already posted.
//
// Call this ONLY where the acquire SUCCEEDED. On a failed acquire the semaphore's state
// is not known (the spec does not say it is untouched), and waiting on a signal that may
// never arrive would put the queue to sleep instead of repairing anything.
void drainPresentWait(Context* ctx, FrameContext& frame) {
    VkSubmitInfo drain = {};
    drain.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    drain.waitSemaphoreCount = 1;
    drain.pWaitSemaphores = &frame.presentWait;
    // A stage mask is required whenever there is a wait semaphore
    // (VUID-VkSubmitInfo-pWaitDstStageMask-parameter). TOP_OF_PIPE asks the least of
    // anything: the wait is already satisfied, and no stage has to be flushed for it.
    const VkPipelineStageFlags top = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    drain.pWaitDstStageMask = &top;
    const VkResult drained = vkQueueSubmit(ctx->queue, 1, &drain, VK_NULL_HANDLE);
    if (drained != VK_SUCCESS) {
        vkFailed("vkQueueSubmit(consuming the image acquire of a refused frame)", drained);
    }
}

// The validation layer, when it is asked for.
static const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

bool hasValidationLayer() {
    // BOTH CALLS, BOTH RESULTS. The count call and the fill call can each fail,
    // and the fill call may legitimately answer VK_INCOMPLETE (a layer appeared
    // between the two). A discarded result here makes "I could not read the list"
    // and "there is no validation layer" the same return value, and the one thing
    // that turns validation off does so in silence.
    uint32_t count = 0;
    VkResult result = vkEnumerateInstanceLayerProperties(&count, nullptr);
    if (result != VK_SUCCESS) {
        vkFailed("vkEnumerateInstanceLayerProperties (the count; validation stays off "
                 "because the list could not be read)", result);
        return false;
    }
    std::vector<VkLayerProperties> layers(count);
    if (count > 0) {
        result = vkEnumerateInstanceLayerProperties(&count, layers.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            vkFailed("vkEnumerateInstanceLayerProperties (the list; validation stays off)",
                     result);
            return false;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(layers[i].layerName, kValidationLayer) == 0) {
            return true;
        }
    }
    return false;
}

// Pick the swapchain format. The surface's preferred format is used when it
// is one we can render to; otherwise B8G8R8A8_UNORM is the fallback that
// virtually every platform supports.
VkSurfaceFormatKHR chooseSurfaceFormat(VkPhysicalDevice device, VkSurfaceKHR surface) {
    // The queries are read, and a failure is PRINTED rather than swallowed: the
    // B8G8R8A8/FIFO fallback below is deliberate and stays, but a surface whose
    // formats could not be read is a fact the operator needs, because the
    // fallback is a guess about someone else's display.
    uint32_t count = 0;
    VkResult result = vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, nullptr);
    if (result != VK_SUCCESS) {
        vkFailed("vkGetPhysicalDeviceSurfaceFormatsKHR (the count)", result);
        count = 0;  // the out-parameter is not trustworthy when the call failed
    }
    std::vector<VkSurfaceFormatKHR> formats(count);
    if (count > 0) {
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, formats.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            vkFailed("vkGetPhysicalDeviceSurfaceFormatsKHR (the list)", result);
            count = 0;
            formats.clear();
        }
    }
    for (const VkSurfaceFormatKHR& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    for (const VkSurfaceFormatKHR& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM) {
            return format;
        }
    }
    if (count > 0) {
        return formats[0];
    }
    VkSurfaceFormatKHR fallback = {};
    fallback.format = VK_FORMAT_B8G8R8A8_UNORM;
    fallback.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    return fallback;
}

VkPresentModeKHR choosePresentMode(VkPhysicalDevice device, VkSurfaceKHR surface, bool vsync) {
    uint32_t count = 0;
    VkResult result = vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, nullptr);
    if (result != VK_SUCCESS) {
        vkFailed("vkGetPhysicalDeviceSurfacePresentModesKHR (the count)", result);
        count = 0;
    }
    std::vector<VkPresentModeKHR> modes(count);
    if (count > 0) {
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count,
                                                           modes.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            vkFailed("vkGetPhysicalDeviceSurfacePresentModesKHR (the list)", result);
            count = 0;
            modes.clear();
        }
    }
    // FIFO is ALWAYS supported and is the one that tears nothing. It is the
    // fallback for every other case, including when vsync is off: the
    // immediate mode is a screen-tear, and the mailbox mode is not universal.
    VkPresentModeKHR chosen = VK_PRESENT_MODE_FIFO_KHR;
    if (vsync) {
        return chosen;
    }
    for (VkPresentModeKHR mode : modes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return mode;
        }
    }
    for (VkPresentModeKHR mode : modes) {
        if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            return mode;
        }
    }
    return chosen;
}

// Find a queue family that can both present and do graphics.
bool findQueueFamily(VkPhysicalDevice device, VkSurfaceKHR surface, uint32_t* outFamily) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (uint32_t i = 0; i < count; i++) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            VkBool32 presentSupported = VK_FALSE;
            const VkResult supported =
                vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupported);
            if (supported != VK_SUCCESS) {
                // A failed query leaves the out-parameter undefined, so the
                // VK_FALSE this initialises to is not evidence that the family
                // cannot present — it is evidence that nobody asked. Say so and
                // move on: a family whose support is unknown is not one to build
                // the present queue on.
                vkFailed("vkGetPhysicalDeviceSurfaceSupportKHR (queue family skipped)",
                         supported);
                continue;
            }
            if (presentSupported) {
                *outFamily = i;
                return true;
            }
        }
    }
    return false;
}

bool createSwapchain(Context* ctx, uint32_t width, uint32_t height, bool vsync) {
    VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(ctx->physicalDevice, ctx->surface);
    VkPresentModeKHR presentMode = choosePresentMode(ctx->physicalDevice, ctx->surface, vsync);

    // A FAILED CAPABILITIES QUERY IS NOT AN EMPTY ONE. `capabilities` is
    // zero-initialised, and a zero `currentExtent` here is indistinguishable from a
    // surface with no preferred size, so the clamp below hands {0,0} to
    // vkCreateSwapchainKHR — which fails, and the line printed blames the create
    // and not the query that caused it. Zero-initialised is not "undefined"; it is
    // a defined value that happens to be a lie, which is worse, because it looks
    // like an answer.
    VkSurfaceCapabilitiesKHR capabilities = {};
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->physicalDevice,
                                                                ctx->surface, &capabilities);
    if (result != VK_SUCCESS) {
        vkFailed("vkGetPhysicalDeviceSurfaceCapabilitiesKHR (no swapchain can be sized "
                 "without it)", result);
        return false;
    }

    // The extent is clamped to what the surface supports: a window that is
    // momentarily larger than the display must not make the swapchain
    // creation fail.
    VkExtent2D extent = {width, height};
    if (capabilities.currentExtent.width != 0xFFFFFFFFu) {
        extent = capabilities.currentExtent;
    } else {
        extent.width = (extent.width < capabilities.minImageExtent.width) ? capabilities.minImageExtent.width : extent.width;
        extent.width = (extent.width > capabilities.maxImageExtent.width) ? capabilities.maxImageExtent.width : extent.width;
        extent.height = (extent.height < capabilities.minImageExtent.height) ? capabilities.minImageExtent.height : extent.height;
        extent.height = (extent.height > capabilities.maxImageExtent.height) ? capabilities.maxImageExtent.height : extent.height;
    }
    ctx->extent = extent;
    ctx->swapchainFormat = surfaceFormat.format;
    ctx->colorSpace = surfaceFormat.colorSpace;

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = ctx->surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    // The 3DS and the PC are the same picture, so the colour space is NOT
    // sRGB-encoded by the surface: the game's own gamma pass does that, and
    // doing it twice changes the picture.
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = ctx->swapchain;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    result = vkCreateSwapchainKHR(ctx->device, &createInfo, nullptr, &swapchain);
    if (result != VK_SUCCESS) {
        std::printf("[vk] vkCreateSwapchainKHR failed (%d)\n", (int)result);
        return false;
    }
    ctx->swapchain = swapchain;

    // A SWAPCHAIN WITH NO IMAGES IS A RENDERER WITH NO FRAMEBUFFER. `ctx->imageCount`
    // drives createFramebuffers and every loop that walks the images; a zero left
    // here means beginFrame records a frame with no framebuffer in it and the
    // picture is black with every create reported as a success. VK_INCOMPLETE is a
    // legitimate answer from the second call (the image set changed between the
    // two); a zero count is never one. The count the FILL reports is the one kept,
    // because that is the number of entries it actually wrote.
    uint32_t count = 0;
    result = vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &count, nullptr);
    if (result != VK_SUCCESS) {
        vkFailed("vkGetSwapchainImagesKHR (the count)", result);
        return false;
    }
    if (count == 0) {
        vkRefused("the swapchain: it reported zero images");
        return false;
    }
    if (count > 16) {
        count = 16;
    }
    std::vector<VkImage> images(count);
    result = vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &count, images.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        vkFailed("vkGetSwapchainImagesKHR (the images)", result);
        return false;
    }
    if (count == 0) {
        vkRefused("the swapchain: its image list came back empty");
        return false;
    }
    ctx->imageCount = count;
    for (uint32_t i = 0; i < count; i++) {
        ctx->images[i] = images[i];
    }
    return true;
}

bool createRenderPass(Context* ctx) {
    VkAttachmentDescription attachments[2] = {};

    attachments[0].format = ctx->swapchainFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // The render pass targets the swapchain image directly, so it must end
    // in PRESENT_SRC. (A TRANSFER_SRC final layout here silently presents
    // black -- it is what the skeleton's offscreen design left behind.)
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
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    VkRenderPassCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    createInfo.attachmentCount = 2;
    createInfo.pAttachments = attachments;
    createInfo.subpassCount = 1;
    createInfo.pSubpasses = &subpass;
    createInfo.dependencyCount = 2;
    createInfo.pDependencies = dependencies;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkResult result = vkCreateRenderPass(ctx->device, &createInfo, nullptr, &renderPass);
    if (result != VK_SUCCESS) {
        std::printf("[vk] vkCreateRenderPass failed (%d)\n", (int)result);
        return false;
    }
    ctx->renderPass = renderPass;
    return true;
}

bool createDepthImage(Context* ctx) {
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_D32_SFLOAT;
    imageInfo.extent.width = ctx->extent.width;
    imageInfo.extent.height = ctx->extent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult result = vkCreateImage(ctx->device, &imageInfo, nullptr, &ctx->depthImage);
    if (result != VK_SUCCESS) {
        std::printf("[vk] vkCreateImage(depth) failed (%d)\n", (int)result);
        return false;
    }

    VkMemoryRequirements requirements = {};
    vkGetImageMemoryRequirements(ctx->device, ctx->depthImage, &requirements);
    VkMemoryAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = 0;

    // Find a memory type that is DEVICE LOCAL.
    VkPhysicalDeviceMemoryProperties properties = {};
    vkGetPhysicalDeviceMemoryProperties(ctx->physicalDevice, &properties);
    bool found = false;
    for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            allocateInfo.memoryTypeIndex = i;
            found = true;
            break;
        }
    }
    if (!found) {
        std::printf("[vk] no device-local memory type for the depth buffer\n");
        return false;
    }

    result = vkAllocateMemory(ctx->device, &allocateInfo, nullptr, &ctx->depthMemory);
    if (result != VK_SUCCESS) {
        std::printf("[vk] vkAllocateMemory(depth) failed (%d)\n", (int)result);
        return false;
    }
    // An image bound to nothing is a depth attachment that reads whatever the
    // allocation happened to hold, and every depth test in the frame is then a
    // coin flip. Destroy the object before releasing the memory bound to it.
    result = vkBindImageMemory(ctx->device, ctx->depthImage, ctx->depthMemory, 0);
    if (result != VK_SUCCESS) {
        vkFailed("vkBindImageMemory(depth)", result);
        vkDestroyImage(ctx->device, ctx->depthImage, nullptr);
        ctx->depthImage = VK_NULL_HANDLE;
        vkFreeMemory(ctx->device, ctx->depthMemory, nullptr);
        ctx->depthMemory = VK_NULL_HANDLE;
        return false;
    }

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = ctx->depthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_D32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    result = vkCreateImageView(ctx->device, &viewInfo, nullptr, &ctx->depthView);
    if (result != VK_SUCCESS) {
        std::printf("[vk] vkCreateImageView(depth) failed (%d)\n", (int)result);
        return false;
    }
    return true;
}

// Release the scene target (see Context::sceneImage). Safe on null handles.
void destroySceneTarget(Context* ctx) {
    if (ctx->sceneFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(ctx->device, ctx->sceneFramebuffer, nullptr);
        ctx->sceneFramebuffer = VK_NULL_HANDLE;
    }
    if (ctx->sceneView != VK_NULL_HANDLE) {
        vkDestroyImageView(ctx->device, ctx->sceneView, nullptr);
        ctx->sceneView = VK_NULL_HANDLE;
    }
    if (ctx->sceneImage != VK_NULL_HANDLE) {
        vkDestroyImage(ctx->device, ctx->sceneImage, nullptr);
        ctx->sceneImage = VK_NULL_HANDLE;
    }
    if (ctx->sceneMemory != VK_NULL_HANDLE) {
        vkFreeMemory(ctx->device, ctx->sceneMemory, nullptr);
        ctx->sceneMemory = VK_NULL_HANDLE;
    }
}

// Create the scene target at the current extent: a swapchain-format colour
// image the scene renders into, plus a framebuffer over the shared render
// pass and depth view. Mirrors createDepthImage's memory discipline.
bool createSceneTarget(Context* ctx) {
    destroySceneTarget(ctx);

    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = ctx->swapchainFormat;
    imageInfo.extent.width = ctx->extent.width;
    imageInfo.extent.height = ctx->extent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult result = vkCreateImage(ctx->device, &imageInfo, nullptr, &ctx->sceneImage);
    if (result != VK_SUCCESS) {
        std::printf("[vk] vkCreateImage(scene) failed (%d)\n", (int)result);
        return false;
    }

    VkMemoryRequirements requirements = {};
    vkGetImageMemoryRequirements(ctx->device, ctx->sceneImage, &requirements);
    VkMemoryAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = 0;

    VkPhysicalDeviceMemoryProperties properties = {};
    vkGetPhysicalDeviceMemoryProperties(ctx->physicalDevice, &properties);
    bool found = false;
    for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            allocateInfo.memoryTypeIndex = i;
            found = true;
            break;
        }
    }
    if (!found) {
        std::printf("[vk] no device-local memory type for the scene target\n");
        destroySceneTarget(ctx);
        return false;
    }

    result = vkAllocateMemory(ctx->device, &allocateInfo, nullptr, &ctx->sceneMemory);
    if (result != VK_SUCCESS) {
        std::printf("[vk] vkAllocateMemory(scene) failed (%d)\n", (int)result);
        destroySceneTarget(ctx);
        return false;
    }
    result = vkBindImageMemory(ctx->device, ctx->sceneImage, ctx->sceneMemory, 0);
    if (result != VK_SUCCESS) {
        vkFailed("vkBindImageMemory(scene)", result);
        destroySceneTarget(ctx);
        return false;
    }

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = ctx->sceneImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = ctx->swapchainFormat;
    viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    result = vkCreateImageView(ctx->device, &viewInfo, nullptr, &ctx->sceneView);
    if (result != VK_SUCCESS) {
        std::printf("[vk] vkCreateImageView(scene) failed (%d)\n", (int)result);
        destroySceneTarget(ctx);
        return false;
    }

    VkImageView attachments[2] = {ctx->sceneView, ctx->depthView};
    VkFramebufferCreateInfo fbInfo = {};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = ctx->renderPass;
    fbInfo.attachmentCount = 2;
    fbInfo.pAttachments = attachments;
    fbInfo.width = ctx->extent.width;
    fbInfo.height = ctx->extent.height;
    fbInfo.layers = 1;

    result = vkCreateFramebuffer(ctx->device, &fbInfo, nullptr, &ctx->sceneFramebuffer);
    if (result != VK_SUCCESS) {
        std::printf("[vk] vkCreateFramebuffer(scene) failed (%d)\n", (int)result);
        destroySceneTarget(ctx);
        return false;
    }
    return true;
}

bool createFramebuffers(Context* ctx) {
    for (uint32_t i = 0; i < ctx->imageCount; i++) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = ctx->images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = ctx->swapchainFormat;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        VkImageView view = VK_NULL_HANDLE;
        VkResult result = vkCreateImageView(ctx->device, &viewInfo, nullptr, &view);
        if (result != VK_SUCCESS) {
            std::printf("[vk] vkCreateImageView failed (%d)\n", (int)result);
            return false;
        }
        ctx->imageViews[i] = view;

        // BOTH attachments, in the render pass's order: colour then depth.
        VkImageView attachments[2] = {view, ctx->depthView};

        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = ctx->renderPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = attachments;
        fbInfo.width = ctx->extent.width;
        fbInfo.height = ctx->extent.height;
        fbInfo.layers = 1;

        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        result = vkCreateFramebuffer(ctx->device, &fbInfo, nullptr, &framebuffer);
        if (result != VK_SUCCESS) {
            std::printf("[vk] vkCreateFramebuffer failed (%d)\n", (int)result);
            return false;
        }
        ctx->framebuffers[i] = framebuffer;
    }
    // The scene target rides the same rebuild: same extent, same pass.
    return createSceneTarget(ctx);
}

bool createFrameResources(Context* ctx) {
    for (uint32_t i = 0; i < kFramesInFlight; i++) {
        FrameContext& frame = ctx->frames[i];

        VkCommandPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = ctx->queueFamily;
        VkResult result = vkCreateCommandPool(ctx->device, &poolInfo, nullptr, &frame.commandPool);
        if (result != VK_SUCCESS) {
            std::printf("[vk] vkCreateCommandPool failed (%d)\n", (int)result);
            return false;
        }

        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = frame.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(ctx->device, &allocInfo, &frame.commandBuffer);
        if (result != VK_SUCCESS) {
            std::printf("[vk] vkAllocateCommandBuffers failed (%d)\n", (int)result);
            return false;
        }

        VkSemaphoreCreateInfo semInfo = {};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        // A NULL SEMAPHORE IS NOT "NO SEMAPHORE" TO A SUBMIT — it is a wait that
        // is silently absent. `presentWait` missing means the present does not
        // wait for the render (a torn frame); `renderFinished` missing means the
        // present waits on nothing at all. Both are worse than a failed init, so
        // the create is read and the context refuses.
        result = vkCreateSemaphore(ctx->device, &semInfo, nullptr, &frame.renderFinished);
        if (result != VK_SUCCESS) {
            vkFailed("vkCreateSemaphore(renderFinished)", result);
            return false;
        }
        result = vkCreateSemaphore(ctx->device, &semInfo, nullptr, &frame.presentWait);
        if (result != VK_SUCCESS) {
            vkFailed("vkCreateSemaphore(presentWait)", result);
            return false;
        }

        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        // The fence starts SIGNALED so the first frame does not wait on a
        // fence that nobody will ever signal.
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        // A fence that does not exist is a frame that never completes: beginFrame
        // would wait on a null handle, which is not a wait at all.
        result = vkCreateFence(ctx->device, &fenceInfo, nullptr, &frame.inFlight);
        if (result != VK_SUCCESS) {
            vkFailed("vkCreateFence(inFlight)", result);
            return false;
        }
    }
    return true;
}

} // namespace

void destroyEyeTargets(Context* ctx) {
    for (int eye = 0; eye < 2; eye++) {
        if (ctx->eyeFramebuffer[eye] != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(ctx->device, ctx->eyeFramebuffer[eye], nullptr);
            ctx->eyeFramebuffer[eye] = VK_NULL_HANDLE;
        }
        if (ctx->eyeView[eye] != VK_NULL_HANDLE) {
            vkDestroyImageView(ctx->device, ctx->eyeView[eye], nullptr);
            ctx->eyeView[eye] = VK_NULL_HANDLE;
        }
        if (ctx->eyeImage[eye] != VK_NULL_HANDLE) {
            vkDestroyImage(ctx->device, ctx->eyeImage[eye], nullptr);
            ctx->eyeImage[eye] = VK_NULL_HANDLE;
        }
        if (ctx->eyeMemory[eye] != VK_NULL_HANDLE) {
            vkFreeMemory(ctx->device, ctx->eyeMemory[eye], nullptr);
            ctx->eyeMemory[eye] = VK_NULL_HANDLE;
        }
    }
    if (ctx->compositeFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(ctx->device, ctx->compositeFramebuffer, nullptr);
        ctx->compositeFramebuffer = VK_NULL_HANDLE;
    }
    if (ctx->compositePass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(ctx->device, ctx->compositePass, nullptr);
        ctx->compositePass = VK_NULL_HANDLE;
    }
    ctx->eyesReady = false;
}

bool ensureEyeTargets(Context* ctx) {
    if (ctx->eyesReady) {
        return true;
    }

    // The composite pass: colour only, over the scene view, its contents
    // discarded on entry (the composite triangle covers every pixel) and
    // stored for the present blit.
    {
        VkAttachmentDescription attachment = {};
        attachment.format = ctx->swapchainFormat;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorRef = {};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        VkSubpassDependency dependencies[2] = {};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].srcAccessMask = 0;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        VkRenderPassCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        createInfo.attachmentCount = 1;
        createInfo.pAttachments = &attachment;
        createInfo.subpassCount = 1;
        createInfo.pSubpasses = &subpass;
        createInfo.dependencyCount = 2;
        createInfo.pDependencies = dependencies;

        VkResult result = vkCreateRenderPass(ctx->device, &createInfo, nullptr,
                                             &ctx->compositePass);
        if (result != VK_SUCCESS) {
            std::printf("[vk] vkCreateRenderPass(composite) failed (%d)\n", (int)result);
            destroyEyeTargets(ctx);
            return false;
        }
    }

    for (int eye = 0; eye < 2; eye++) {
        VkImageCreateInfo imageInfo = {};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = ctx->swapchainFormat;
        imageInfo.extent.width = ctx->extent.width;
        imageInfo.extent.height = ctx->extent.height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkResult result = vkCreateImage(ctx->device, &imageInfo, nullptr,
                                        &ctx->eyeImage[eye]);
        if (result != VK_SUCCESS) {
            std::printf("[vk] vkCreateImage(eye) failed (%d)\n", (int)result);
            destroyEyeTargets(ctx);
            return false;
        }

        VkMemoryRequirements requirements = {};
        vkGetImageMemoryRequirements(ctx->device, ctx->eyeImage[eye], &requirements);
        VkMemoryAllocateInfo allocateInfo = {};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = 0;

        VkPhysicalDeviceMemoryProperties properties = {};
        vkGetPhysicalDeviceMemoryProperties(ctx->physicalDevice, &properties);
        bool found = false;
        for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                allocateInfo.memoryTypeIndex = i;
                found = true;
                break;
            }
        }
        if (!found) {
            std::printf("[vk] no device-local memory type for an eye target\n");
            destroyEyeTargets(ctx);
            return false;
        }

        result = vkAllocateMemory(ctx->device, &allocateInfo, nullptr,
                                  &ctx->eyeMemory[eye]);
        if (result != VK_SUCCESS) {
            std::printf("[vk] vkAllocateMemory(eye) failed (%d)\n", (int)result);
            destroyEyeTargets(ctx);
            return false;
        }
        result = vkBindImageMemory(ctx->device, ctx->eyeImage[eye],
                                   ctx->eyeMemory[eye], 0);
        if (result != VK_SUCCESS) {
            vkFailed("vkBindImageMemory(eye)", result);
            destroyEyeTargets(ctx);
            return false;
        }

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = ctx->eyeImage[eye];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = ctx->swapchainFormat;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        result = vkCreateImageView(ctx->device, &viewInfo, nullptr, &ctx->eyeView[eye]);
        if (result != VK_SUCCESS) {
            std::printf("[vk] vkCreateImageView(eye) failed (%d)\n", (int)result);
            destroyEyeTargets(ctx);
            return false;
        }

        VkImageView attachments[2] = {ctx->eyeView[eye], ctx->depthView};
        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = ctx->renderPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = attachments;
        fbInfo.width = ctx->extent.width;
        fbInfo.height = ctx->extent.height;
        fbInfo.layers = 1;

        result = vkCreateFramebuffer(ctx->device, &fbInfo, nullptr,
                                     &ctx->eyeFramebuffer[eye]);
        if (result != VK_SUCCESS) {
            std::printf("[vk] vkCreateFramebuffer(eye) failed (%d)\n", (int)result);
            destroyEyeTargets(ctx);
            return false;
        }
    }

    {
        VkImageView attachments[1] = {ctx->sceneView};
        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = ctx->compositePass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = attachments;
        fbInfo.width = ctx->extent.width;
        fbInfo.height = ctx->extent.height;
        fbInfo.layers = 1;

        VkResult result = vkCreateFramebuffer(ctx->device, &fbInfo, nullptr,
                                              &ctx->compositeFramebuffer);
        if (result != VK_SUCCESS) {
            std::printf("[vk] vkCreateFramebuffer(composite) failed (%d)\n", (int)result);
            destroyEyeTargets(ctx);
            return false;
        }
    }

    ctx->eyesReady = true;
    return true;
}


bool createInstance(Context* ctx, bool validation) {
    ctx->validation = validation && hasValidationLayer();

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "MDK";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "MDK";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    const char* layers[1] = {kValidationLayer};

    // The instance extensions. The base list is VK_KHR_surface; the windowing
    // system adds the platform's own (xcb, xlib, wayland), because THAT is
    // the extension its surface creation needs and it MUST be enabled in the
    // instance.
    //
    // The list is then FILTERED against the loader's available extensions.
    // Enabling an extension the loader does not have makes
    // vkCreateInstance fail outright, and which platform extensions exist
    // depends on how the loader was started -- a loader in single-ICD
    // "direct" mode exposes none of them, for instance.
    std::vector<const char*> wanted;
    wanted.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    // The headless surface. It is OPTIONAL: the list is filtered against
    // the loader's available extensions, so a loader without it still
    // works -- it just cannot run headless.
    wanted.push_back("VK_EXT_headless_surface");
    if (ctx->extensionProvider) {
        uint32_t platformCount = 0;
        const char* const* platformNames = nullptr;
        platformCount = ctx->extensionProvider(&platformNames);
        for (uint32_t i = 0; i < platformCount && platformNames != nullptr; i++) {
            wanted.push_back(platformNames[i]);
        }
    }

    uint32_t availableCount = 0;
    // Read, and a failure leaves the list EMPTY on purpose: the check below then
    // refuses the instance with its own line. What must not happen is a failed
    // enumeration that looks like "the loader has no extensions".
    VkResult result =
        vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, nullptr);
    if (result != VK_SUCCESS) {
        vkFailed("vkEnumerateInstanceExtensionProperties (the count)", result);
        availableCount = 0;
    }
    std::vector<VkExtensionProperties> available(availableCount);
    if (availableCount > 0) {
        result =
            vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, available.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            vkFailed("vkEnumerateInstanceExtensionProperties (the list)", result);
            availableCount = 0;
            available.clear();
        }
    }
    auto isAvailable = [&](const char* name) -> bool {
        for (const VkExtensionProperties& property : available) {
            if (strcmp(property.extensionName, name) == 0) {
                return true;
            }
        }
        return false;
    };

    std::vector<const char*> extensions;
    for (const char* name : wanted) {
        if (isAvailable(name)) {
            extensions.push_back(name);
        }
    }
    if (extensions.empty()) {
        std::printf("[vk] no usable instance extensions (the loader exposed none)\n");
        return false;
    }

    VkInstanceCreateInfo instanceInfo = {};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = (uint32_t)extensions.size();
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    if (ctx->validation) {
        instanceInfo.enabledLayerCount = 1;
        instanceInfo.ppEnabledLayerNames = layers;
    }

    result = vkCreateInstance(&instanceInfo, nullptr, &ctx->instance);
    if (result != VK_SUCCESS) {
        std::printf("[vk] vkCreateInstance failed (%d)\n", (int)result);
        return false;
    }

    // LOAD THE INSTANCE-LEVEL ENTRY POINTS. volk loads only the GLOBAL
    // functions in volkInitialize(); every instance-level function (the
    // surface queries, vkCreateDevice, vkDestroySurfaceKHR) is a NULL
    // pointer until volkLoadInstance is called. Calling one before this is
    // a jump to address zero, which is exactly what it looks like.
    volkLoadInstance(ctx->instance);
    return true;
}

bool createHeadlessSurface(Context* ctx) {
    // VK_EXT_headless_surface. A surface with no window behind it, so the
    // renderer can draw into an image and read it back -- exactly what the
    // golden-frame harness needs, and the only way to run on a machine with
    // no display server (or a software X server without DRI3, which the
    // lavapipe WSI needs for presentation).
    PFN_vkCreateHeadlessSurfaceEXT create =
        (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(ctx->instance, "vkCreateHeadlessSurfaceEXT");
    if (!create) {
        std::printf("[vk] VK_EXT_headless_surface is not available\n");
        return false;
    }
    VkHeadlessSurfaceCreateInfoEXT info = {};
    info.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
    VkResult result = create(ctx->instance, &info, nullptr, &ctx->surface);
    if (result != VK_SUCCESS) {
        std::printf("[vk] vkCreateHeadlessSurfaceEXT failed (%d)\n", (int)result);
        return false;
    }
    return true;
}

bool createSurface(Context* ctx, void* window) {
    // The surface is platform-specific, so the call goes through a function
    // pointer the platform layer sets. That keeps this file free of SDL
    // (and of any other windowing library) while still letting the seam's
    // single mdk_render_create entry point own the whole sequence.
    if (!ctx->surfaceCreator) {
        std::printf("[vk] no surface creator was set\n");
        return false;
    }
    return ctx->surfaceCreator(ctx->instance, window, &ctx->surface);
}

bool createDevice(Context* ctx, uint32_t width, uint32_t height, bool vsync) {
    uint32_t deviceCount = 0;
    // The count query and the fill are both read. A failed count is NOT "zero
    // devices": the message below about a missing ICD is the right answer for a
    // successful query that found nothing, and the wrong one for a loader that
    // could not be asked.
    VkResult result = vkEnumeratePhysicalDevices(ctx->instance, &deviceCount, nullptr);
    if (result != VK_SUCCESS) {
        vkFailed("vkEnumeratePhysicalDevices (the count)", result);
        return false;
    }
    if (deviceCount == 0) {
        std::printf("[vk] no Vulkan physical device. (Is an ICD installed? "
                    "On a headless box, try "
                    "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json)\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    result = vkEnumeratePhysicalDevices(ctx->instance, &deviceCount, devices.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        vkFailed("vkEnumeratePhysicalDevices (the list)", result);
        return false;
    }
    if (deviceCount == 0) {
        vkRefused("the device: the physical device list came back empty");
        return false;
    }

    bool found = false;
    for (VkPhysicalDevice candidate : devices) {
        uint32_t family = 0;
        if (!findQueueFamily(candidate, ctx->surface, &family)) {
            continue;
        }
        ctx->physicalDevice = candidate;
        ctx->queueFamily = family;
        found = true;
        break;
    }
    if (!found) {
        std::printf("[vk] no physical device with a presentable graphics queue\n");
        return false;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = ctx->queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    // THE FEATURES, asked for only where the device has them. The original look
    // needs fillModeNonSolid alone. The enhanced look's colour sampler turns on
    // anisotropy when the device supports it (10_enhanced.inc), which is only
    // legal if the DEVICE enabled samplerAnisotropy; and glslang targeting
    // vulkan1.3 compiles `discard` to OpDemoteToHelperInvocation, which needs
    // shaderDemoteToHelperInvocation. Both were missing (validation errors
    // VUID-VkSamplerCreateInfo-anisotropyEnable-01070 and
    // VUID-VkShaderModuleCreateInfo-pCode-08740 on every --enhanced run).
    VkPhysicalDeviceVulkan13Features have13 = {};
    have13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceFeatures2 have = {};
    have.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    have.pNext = &have13;
    vkGetPhysicalDeviceFeatures2(ctx->physicalDevice, &have);

    VkPhysicalDeviceVulkan13Features want13 = {};
    want13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    want13.shaderDemoteToHelperInvocation = have13.shaderDemoteToHelperInvocation;
    VkPhysicalDeviceFeatures2 want = {};
    want.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    want.pNext = &want13;
    VkPhysicalDeviceFeatures& features = want.features;
    features.fillModeNonSolid = VK_TRUE;
    features.samplerAnisotropy = have.features.samplerAnisotropy;

    // THE DEVICE EXTENSIONS. VK_KHR_swapchain is NOT optional: without it
    // vkGetDeviceProcAddr returns a NULL pointer for vkCreateSwapchainKHR,
    // and calling it jumps to address zero. (In Vulkan 1.1 and later the
    // swapchain is core, but the EXTENSION still has to be enabled for the
    // entry points to be present.)
    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo deviceInfo = {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = (uint32_t)(sizeof(deviceExtensions) / sizeof(deviceExtensions[0]));
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    deviceInfo.pNext = &want;   // the features travel in the chain, so pEnabledFeatures stays null

    result = vkCreateDevice(ctx->physicalDevice, &deviceInfo, nullptr, &ctx->device);
    if (result != VK_SUCCESS) {
        std::printf("[vk] vkCreateDevice failed (%d)\n", (int)result);
        return false;
    }
    // The device-level entry points, likewise.
    volkLoadDevice(ctx->device);
    vkGetDeviceQueue(ctx->device, ctx->queueFamily, 0, &ctx->queue);

    if (!createSwapchain(ctx, width, height, vsync)) {
        return false;
    }
    if (!createRenderPass(ctx)) {
        return false;
    }
    if (!createDepthImage(ctx) || !createFramebuffers(ctx) || !createFrameResources(ctx)) {
        return false;
    }
    return true;
}

void destroyContext(Context* ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->device != VK_NULL_HANDLE) {
        // Teardown cannot DO anything with a failed wait except say so: the destroys
        // below have to happen or the device itself cannot be released. What the
        // line buys is the record that the resources were released without a proven
        // completion boundary (AGENTS.md P3) — the difference between a clean exit
        // and one whose last frame was still in flight when the device went away.
        const VkResult idle = vkDeviceWaitIdle(ctx->device);
        if (idle != VK_SUCCESS) {
            vkFailed("vkDeviceWaitIdle(teardown; the resources below are released without "
                     "a proven completion)", idle);
        }

        destroyEyeTargets(ctx);
        destroySceneTarget(ctx);
        for (uint32_t i = 0; i < ctx->imageCount; i++) {
            if (ctx->framebuffers[i] != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(ctx->device, ctx->framebuffers[i], nullptr);
            }
            if (ctx->imageViews[i] != VK_NULL_HANDLE) {
                vkDestroyImageView(ctx->device, ctx->imageViews[i], nullptr);
            }
        }
        if (ctx->depthView != VK_NULL_HANDLE) {
            vkDestroyImageView(ctx->device, ctx->depthView, nullptr);
            ctx->depthView = VK_NULL_HANDLE;
        }
        if (ctx->depthImage != VK_NULL_HANDLE) {
            vkDestroyImage(ctx->device, ctx->depthImage, nullptr);
            ctx->depthImage = VK_NULL_HANDLE;
        }
        if (ctx->depthMemory != VK_NULL_HANDLE) {
            vkFreeMemory(ctx->device, ctx->depthMemory, nullptr);
            ctx->depthMemory = VK_NULL_HANDLE;
        }
        for (uint32_t i = 0; i < kFramesInFlight; i++) {
            FrameContext& frame = ctx->frames[i];
            if (frame.inFlight != VK_NULL_HANDLE) {
                vkDestroyFence(ctx->device, frame.inFlight, nullptr);
            }
            if (frame.renderFinished != VK_NULL_HANDLE) {
                vkDestroySemaphore(ctx->device, frame.renderFinished, nullptr);
            }
            if (frame.presentWait != VK_NULL_HANDLE) {
                vkDestroySemaphore(ctx->device, frame.presentWait, nullptr);
            }
            if (frame.commandPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(ctx->device, frame.commandPool, nullptr);
            }
        }
        if (ctx->renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(ctx->device, ctx->renderPass, nullptr);
        }
        if (ctx->swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(ctx->device, ctx->swapchain, nullptr);
        }
        vkDestroyDevice(ctx->device, nullptr);
        ctx->device = VK_NULL_HANDLE;
    }
    if (ctx->instance != VK_NULL_HANDLE) {
        if (ctx->surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(ctx->instance, ctx->surface, nullptr);
            ctx->surface = VK_NULL_HANDLE;
        }
        vkDestroyInstance(ctx->instance, nullptr);
        ctx->instance = VK_NULL_HANDLE;
    }
}

bool recreateSwapchain(Context* ctx, uint32_t width, uint32_t height, bool vsync) {
    // The wait is the ONLY thing that makes the destroys below safe: a framebuffer
    // a submission is still reading must not be destroyed, and AGENTS.md P3 puts the
    // completion boundary before the release. A failed wait is a boundary that was
    // NOT established, so this refuses before it touches anything: the old swapchain
    // stays intact and this frame is simply dropped.
    const VkResult idle = vkDeviceWaitIdle(ctx->device);
    if (idle != VK_SUCCESS) {
        vkFailed("vkDeviceWaitIdle(before the swapchain rebuild; nothing was destroyed)",
                 idle);
        return false;
    }

    destroyEyeTargets(ctx);
    destroySceneTarget(ctx);
    for (uint32_t i = 0; i < ctx->imageCount; i++) {
        if (ctx->framebuffers[i] != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(ctx->device, ctx->framebuffers[i], nullptr);
            ctx->framebuffers[i] = VK_NULL_HANDLE;
        }
        if (ctx->imageViews[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(ctx->device, ctx->imageViews[i], nullptr);
            ctx->imageViews[i] = VK_NULL_HANDLE;
        }
    }
    if (ctx->renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(ctx->device, ctx->renderPass, nullptr);
        ctx->renderPass = VK_NULL_HANDLE;
    }
    if (ctx->swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(ctx->device, ctx->swapchain, nullptr);
        ctx->swapchain = VK_NULL_HANDLE;
    }

    if (!createSwapchain(ctx, width, height, vsync)) {
        return false;
    }
    if (!createRenderPass(ctx)) {
        return false;
    }
    if (ctx->depthImage != VK_NULL_HANDLE) {
        vkDestroyImageView(ctx->device, ctx->depthView, nullptr);
        vkDestroyImage(ctx->device, ctx->depthImage, nullptr);
        vkFreeMemory(ctx->device, ctx->depthMemory, nullptr);
        ctx->depthImage = VK_NULL_HANDLE;
        ctx->depthView = VK_NULL_HANDLE;
        ctx->depthMemory = VK_NULL_HANDLE;
    }
    if (!createDepthImage(ctx) || !createFramebuffers(ctx)) {
        return false;
    }
    return true;
}

void setSceneOverride(Context* ctx, VkFramebuffer framebuffer) {
    ctx->sceneOverride = framebuffer;
}

void setPresentImage(Context* ctx, VkImage image) {
    ctx->presentImage = image;
}

void endPass(Context* ctx) {
    FrameContext& frame = ctx->frames[ctx->currentFrame];
    vkCmdEndRenderPass(frame.commandBuffer);
}

bool beginPass(Context* ctx, VkFramebuffer framebuffer, VkRenderPass pass,
               uint32_t clearColour) {
    FrameContext& frame = ctx->frames[ctx->currentFrame];
    // The clear colour is 0xAARRGGBB (the seam's convention); the swapchain
    // is B8G8R8A8, so the bytes land reversed. UNORM, not SRGB: the C#
    // clears with raw bytes and does its own gamma at the end.
    VkClearValue clears[2] = {};
    clears[0].color = {{
        (float)((clearColour >> 16) & 0xFFu) / 255.0f,
        (float)((clearColour >> 8) & 0xFFu) / 255.0f,
        (float)(clearColour & 0xFFu) / 255.0f,
        (float)(clearColour >> 24) / 255.0f,
    }};
    clears[1].depthStencil = {1.0f, 0};

    const bool scenePass = pass == ctx->renderPass;
    VkRenderPassBeginInfo passInfo = {};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = pass;
    passInfo.framebuffer = framebuffer;
    passInfo.renderArea.extent = ctx->extent;
    passInfo.clearValueCount = scenePass ? 2 : 0;
    passInfo.pClearValues = scenePass ? clears : nullptr;

    vkCmdBeginRenderPass(frame.commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
    return true;
}

bool beginDepthPass(Context* ctx, VkFramebuffer framebuffer, VkRenderPass pass,
                    uint32_t width, uint32_t height) {
    FrameContext& frame = ctx->frames[ctx->currentFrame];
    VkClearValue clear = {};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo passInfo = {};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = pass;
    passInfo.framebuffer = framebuffer;
    passInfo.renderArea.offset = {0, 0};
    passInfo.renderArea.extent = {width, height};
    passInfo.clearValueCount = 1;
    passInfo.pClearValues = &clear;
    vkCmdBeginRenderPass(frame.commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
    return true;
}

bool beginFrame(Context* ctx, uint32_t width, uint32_t height, bool vsync, uint32_t clearColour) {
    if (ctx->extent.width != width || ctx->extent.height != height) {
        if (!recreateSwapchain(ctx, width, height, vsync)) {
            return false;
        }
    }

    // THE FENCE WAIT IS THE COMPLETION BOUNDARY, so its result is the answer to "is
    // it safe to touch this frame's resources". Discarding it means a failed wait —
    // or a lost device — goes on to reset, begin and record a command buffer on a
    // device that will not run it. It returns false quietly on purpose: the seam
    // prints, once per distinct request, because a frame-boundary failure repeats
    // every frame and this file has no refusal memory of its own.
    // WHY A REFUSED BEGIN NEEDS NO FENCE RECOVERY, AND WHY A REFUSED SUBMIT DOES.
    // `vkWaitForFences` does not RESET a fence: a successful wait leaves it signalled,
    // and only `vkResetFences` — which this function never calls, and which C3 of
    // linux_refusal_report_test.sh keeps it that way — takes it back. So every
    // `return false` below leaves this slot's fence SIGNALLED, which is the state the
    // slot is supposed to be in when it is free, and the next `beginFrame` waits on a
    // signalled fence and returns immediately. The slot is reused; nothing hangs.
    // endFrame's failed submit is the opposite case: there the fence HAD been reset
    // for a submission that never happened, so nothing would ever signal it, and that
    // one does carry the signalled replacement it has.
    FrameContext& frame = ctx->frames[ctx->currentFrame];
    VkResult result = vkWaitForFences(ctx->device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        return false;
    }

    uint32_t imageIndex = 0;
    result = vkAcquireNextImageKHR(ctx->device, ctx->swapchain, UINT64_MAX,
                                            frame.presentWait, VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        if (!recreateSwapchain(ctx, width, height, vsync)) {
            return false;
        }
        result = vkAcquireNextImageKHR(ctx->device, ctx->swapchain, UINT64_MAX,
                                       frame.presentWait, VK_NULL_HANDLE, &imageIndex);
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return false;
    }
    ctx->currentImage = imageIndex;

    // A command buffer that failed to RESET is still recording, and beginning a
    // recording command buffer is invalid use. A BEGIN that failed leaves every
    // vkCmd* the renderer then issues invalid, so the frame would report success
    // with nothing in it — the invisible empty picture, at the widest possible
    // scale. Both are refused where they become known.
    //
    // WHAT THESE TWO EXITS COST, and what is done about it: the acquire has already
    // posted `presentWait`, and a binary semaphore that stays posted is not a harmless
    // leftover — the next frame's acquire would hand a signalled one to
    // `vkAcquireNextImageKHR`, which is invalid use. So the signal is consumed here,
    // by the empty submit `drainPresentWait` explains. What is NOT repaired is the
    // OUT_OF_DATE retry above: a failed acquire's semaphore is not known to be
    // untouched, and waiting on a signal that may never arrive is a queue stall, not a
    // fix. The obvious reordering — reset and begin BEFORE the acquire — was considered
    // and rejected: a later acquire failure would then leave the command buffer
    // recording, and `vkResetCommandBuffer` on a recording buffer is invalid use.
    result = vkResetCommandBuffer(frame.commandBuffer, 0);
    if (result != VK_SUCCESS) {
        drainPresentWait(ctx, frame);
        return false;
    }
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
        drainPresentWait(ctx, frame);
        return false;
    }

    // The scene renders into the scene target (or, while the renderer is
    // drawing a stereo eye, into that eye's target): see beginPass below.
    beginPass(ctx,
              ctx->sceneOverride != VK_NULL_HANDLE ? ctx->sceneOverride
                                                   : ctx->sceneFramebuffer,
              ctx->renderPass, clearColour);

    // The viewport and scissor are dynamic state and MUST be set every frame;
    // without them nothing rasterizes. POSITIVE height: the y flip that puts
    // the picture upright is folded into the projection matrix
    // (renderer_vk.cpp flipClipY), not into the viewport. A negative height
    // would flip it a second time and show the world upside down.
    //
    // The renderer sets its own viewport again right after this one, so this
    // value is not the one that rasterizes anything; it is here so that a frame
    // with a begin and no further state still has defined viewport state. It
    // is kept consistent with the renderer on purpose -- two comments in this
    // tree used to claim the opposite ("the negative viewport height restores
    // the oracle's orientation") and a reader could not tell which one was
    // live.
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)ctx->extent.width;
    viewport.height = (float)ctx->extent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    VkRect2D scissor = {{0, 0}, ctx->extent};
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
    return true;
}

bool endFrame(Context* ctx) {
    FrameContext& frame = ctx->frames[ctx->currentFrame];
    vkCmdEndRenderPass(frame.commandBuffer);

    // PRESENT THE SCENE TARGET. The render pass left the scene image in its
    // colour attachment's final layout (PRESENT_SRC_KHR); move it to a blit
    // source, blit it whole into the acquired swapchain image, and leave that
    // image in PRESENT_SRC_KHR for vkQueuePresentKHR. Same format and extent
    // on both sides, so NEAREST is an exact copy: the presented bytes are the
    // bytes the scene pass produced, unchanged.
    {
        VkImage swapImage = ctx->images[ctx->currentImage];
        VkImageMemoryBarrier barriers[2] = {};
        barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].image = ctx->presentImage != VK_NULL_HANDLE ? ctx->presentImage
                                                                 : ctx->sceneImage;
        barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[0].subresourceRange.levelCount = 1;
        barriers[0].subresourceRange.layerCount = 1;
        barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].image = swapImage;
        barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[1].subresourceRange.levelCount = 1;
        barriers[1].subresourceRange.layerCount = 1;
        barriers[1].srcAccessMask = 0;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(frame.commandBuffer,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             2, barriers);

        VkImageBlit blit = {};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        blit.srcOffsets[1].x = (int32_t)ctx->extent.width;
        blit.srcOffsets[1].y = (int32_t)ctx->extent.height;
        blit.srcOffsets[1].z = 1;
        blit.dstOffsets[1].x = (int32_t)ctx->extent.width;
        blit.dstOffsets[1].y = (int32_t)ctx->extent.height;
        blit.dstOffsets[1].z = 1;
        vkCmdBlitImage(frame.commandBuffer,
                       ctx->presentImage != VK_NULL_HANDLE ? ctx->presentImage
                                                           : ctx->sceneImage,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_NEAREST);

        VkImageMemoryBarrier present = {};
        present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        present.image = swapImage;
        present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        present.subresourceRange.levelCount = 1;
        present.subresourceRange.layerCount = 1;
        present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        present.dstAccessMask = 0;
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &present);
    }

    // THE COMMAND STREAM HAS TO BE CLOSED BEFORE IT IS SUBMITTED. Only a pending or an
    // executable command buffer may be named by `VkSubmitInfo::pCommandBuffers`
    // (VUID-vkSubmitInfo-pCommandBuffers-00075), and a buffer that was begun and never
    // ended is still RECORDING. This file submitted one every frame. That it drew 138
    // committed frames is evidence that lavapipe does not enforce the state machine and
    // no evidence at all that the submission was legal (review-1 F1); the one-shot path
    // in renderer_vk.cpp already refused to submit an unfinalised buffer, and the frame
    // path is the same rule at the frame's scale.
    //
    // The end sits BEFORE the fence reset deliberately: a refused frame has to leave its
    // fence signalled (see the note in beginFrame), and the reset is the one call here
    // that would take it away. A failed end does leave the buffer recording — nothing
    // un-records a buffer — so the next frame's reset is then invalid use too; that is
    // a driver that has already refused an end, and it is printed rather than hidden.
    const VkResult ended = vkEndCommandBuffer(frame.commandBuffer);
    if (ended != VK_SUCCESS) {
        drainPresentWait(ctx, frame);
        vkFailed("vkEndCommandBuffer(the frame's command buffer; nothing was submitted)",
                 ended);
        return false;
    }

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &frame.presentWait;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &frame.renderFinished;

    // The reset is read too: if it failed the fence's state is unknown, and a
    // submit that relies on that fence is a guess. Refuse BEFORE the submit, so
    // nothing is queued behind a fence nobody can wait on.
    VkResult result = vkResetFences(ctx->device, 1, &frame.inFlight);
    if (result != VK_SUCCESS) {
        drainPresentWait(ctx, frame);
        return false;
    }
    result = vkQueueSubmit(ctx->queue, 1, &submitInfo, frame.inFlight);
    if (result != VK_SUCCESS) {
        // The queue refused the frame, so nothing waited on `presentWait` and its
        // signal is still posted. Consume it before the fence is touched: the next
        // frame's acquire is invalid use until somebody does.
        drainPresentWait(ctx, frame);
        // The fence was reset and nothing is going to signal it, so the NEXT
        // beginFrame would wait UINT64_MAX on a fence no submission will ever
        // signal: one refused submit becomes a hang two frames later. The fence is
        // not associated with any queue (the submit failed), so the way back to the
        // invariant "this slot's fence is signalled when the slot is free" is a
        // fresh signalled one. It is CREATED BEFORE the old one is destroyed, so a
        // failed recovery changes nothing and leaves the hazard printed rather than
        // swapping a hang for a null fence. (`vkSignalFence` is the direct way back
        // and is not declared by the vendored header this build compiles against,
        // so it is not available here.)
        VkFence replacement = VK_NULL_HANDLE;
        VkFenceCreateInfo recover = {};
        recover.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        recover.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        const VkResult refence = vkCreateFence(ctx->device, &recover, nullptr, &replacement);
        if (refence == VK_SUCCESS) {
            vkDestroyFence(ctx->device, frame.inFlight, nullptr);
            frame.inFlight = replacement;
        } else {
            vkFailed("vkCreateFence(replacing the fence of a refused submit; the next "
                     "frame's fence wait may not return)", refence);
        }
        return false;
    }

    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &frame.renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &ctx->swapchain;
    presentInfo.pImageIndices = &ctx->currentImage;
    result = vkQueuePresentKHR(ctx->queue, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        // The window was resized. The swapchain is rebuilt on the next
        // frame; this frame is simply dropped.
        return true;
    }
    if (result != VK_SUCCESS) {
        return false;
    }

    ctx->currentFrame = (ctx->currentFrame + 1) % kFramesInFlight;
    return true;
}


bool submitFrame(Context* ctx) {
    FrameContext& frame = ctx->frames[ctx->currentFrame];

    // PRESENT THE SCENE TARGET. The render pass left the scene image in its
    // colour attachment's final layout (PRESENT_SRC_KHR); move it to a blit
    // source, blit it whole into the acquired swapchain image, and leave that
    // image in PRESENT_SRC_KHR for vkQueuePresentKHR. Same format and extent
    // on both sides, so NEAREST is an exact copy: the presented bytes are the
    // bytes the scene pass produced, unchanged.
    {
        VkImage swapImage = ctx->images[ctx->currentImage];
        VkImageMemoryBarrier barriers[2] = {};
        barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].image = ctx->presentImage != VK_NULL_HANDLE ? ctx->presentImage
                                                                 : ctx->sceneImage;
        barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[0].subresourceRange.levelCount = 1;
        barriers[0].subresourceRange.layerCount = 1;
        barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].image = swapImage;
        barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[1].subresourceRange.levelCount = 1;
        barriers[1].subresourceRange.layerCount = 1;
        barriers[1].srcAccessMask = 0;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(frame.commandBuffer,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             2, barriers);

        VkImageBlit blit = {};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        blit.srcOffsets[1].x = (int32_t)ctx->extent.width;
        blit.srcOffsets[1].y = (int32_t)ctx->extent.height;
        blit.srcOffsets[1].z = 1;
        blit.dstOffsets[1].x = (int32_t)ctx->extent.width;
        blit.dstOffsets[1].y = (int32_t)ctx->extent.height;
        blit.dstOffsets[1].z = 1;
        vkCmdBlitImage(frame.commandBuffer,
                       ctx->presentImage != VK_NULL_HANDLE ? ctx->presentImage
                                                           : ctx->sceneImage,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_NEAREST);

        VkImageMemoryBarrier present = {};
        present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        present.image = swapImage;
        present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        present.subresourceRange.levelCount = 1;
        present.subresourceRange.layerCount = 1;
        present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        present.dstAccessMask = 0;
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &present);
    }

    // THE COMMAND STREAM HAS TO BE CLOSED BEFORE IT IS SUBMITTED. Only a pending or an
    // executable command buffer may be named by `VkSubmitInfo::pCommandBuffers`
    // (VUID-vkSubmitInfo-pCommandBuffers-00075), and a buffer that was begun and never
    // ended is still RECORDING. This file submitted one every frame. That it drew 138
    // committed frames is evidence that lavapipe does not enforce the state machine and
    // no evidence at all that the submission was legal (review-1 F1); the one-shot path
    // in renderer_vk.cpp already refused to submit an unfinalised buffer, and the frame
    // path is the same rule at the frame's scale.
    //
    // The end sits BEFORE the fence reset deliberately: a refused frame has to leave its
    // fence signalled (see the note in beginFrame), and the reset is the one call here
    // that would take it away. A failed end does leave the buffer recording — nothing
    // un-records a buffer — so the next frame's reset is then invalid use too; that is
    // a driver that has already refused an end, and it is printed rather than hidden.
    const VkResult ended = vkEndCommandBuffer(frame.commandBuffer);
    if (ended != VK_SUCCESS) {
        drainPresentWait(ctx, frame);
        vkFailed("vkEndCommandBuffer(the frame's command buffer; nothing was submitted)",
                 ended);
        return false;
    }

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &frame.presentWait;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &frame.renderFinished;

    // The reset is read too: if it failed the fence's state is unknown, and a
    // submit that relies on that fence is a guess. Refuse BEFORE the submit, so
    // nothing is queued behind a fence nobody can wait on.
    VkResult result = vkResetFences(ctx->device, 1, &frame.inFlight);
    if (result != VK_SUCCESS) {
        drainPresentWait(ctx, frame);
        return false;
    }
    result = vkQueueSubmit(ctx->queue, 1, &submitInfo, frame.inFlight);
    if (result != VK_SUCCESS) {
        // The queue refused the frame, so nothing waited on `presentWait` and its
        // signal is still posted. Consume it before the fence is touched: the next
        // frame's acquire is invalid use until somebody does.
        drainPresentWait(ctx, frame);
        // The fence was reset and nothing is going to signal it, so the NEXT
        // beginFrame would wait UINT64_MAX on a fence no submission will ever
        // signal: one refused submit becomes a hang two frames later. The fence is
        // not associated with any queue (the submit failed), so the way back to the
        // invariant "this slot's fence is signalled when the slot is free" is a
        // fresh signalled one. It is CREATED BEFORE the old one is destroyed, so a
        // failed recovery changes nothing and leaves the hazard printed rather than
        // swapping a hang for a null fence. (`vkSignalFence` is the direct way back
        // and is not declared by the vendored header this build compiles against,
        // so it is not available here.)
        VkFence replacement = VK_NULL_HANDLE;
        VkFenceCreateInfo recover = {};
        recover.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        recover.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        const VkResult refence = vkCreateFence(ctx->device, &recover, nullptr, &replacement);
        if (refence == VK_SUCCESS) {
            vkDestroyFence(ctx->device, frame.inFlight, nullptr);
            frame.inFlight = replacement;
        } else {
            vkFailed("vkCreateFence(replacing the fence of a refused submit; the next "
                     "frame's fence wait may not return)", refence);
        }
        return false;
    }

    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &frame.renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &ctx->swapchain;
    presentInfo.pImageIndices = &ctx->currentImage;
    result = vkQueuePresentKHR(ctx->queue, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        // The window was resized. The swapchain is rebuilt on the next
        // frame; this frame is simply dropped.
        return true;
    }
    if (result != VK_SUCCESS) {
        return false;
    }

    ctx->currentFrame = (ctx->currentFrame + 1) % kFramesInFlight;
    return true;
}

VkCommandBuffer commandBuffer(Context* ctx) {
    return ctx->frames[ctx->currentFrame].commandBuffer;
}

} // namespace vk
} // namespace mdk
