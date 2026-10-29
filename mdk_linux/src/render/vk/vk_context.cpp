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

// The validation layer, when it is asked for.
static const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

bool hasValidationLayer() {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    if (count > 0) {
        vkEnumerateInstanceLayerProperties(&count, layers.data());
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
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    if (count > 0) {
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, formats.data());
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
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    if (count > 0) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, modes.data());
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
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupported);
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

    VkSurfaceCapabilitiesKHR capabilities = {};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->physicalDevice, ctx->surface, &capabilities);

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
    VkResult result = vkCreateSwapchainKHR(ctx->device, &createInfo, nullptr, &swapchain);
    if (result != VK_SUCCESS) {
        std::printf("[vk] vkCreateSwapchainKHR failed (%d)\n", (int)result);
        return false;
    }
    ctx->swapchain = swapchain;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &count, nullptr);
    if (count > 16) {
        count = 16;
    }
    ctx->imageCount = count;
    std::vector<VkImage> images(count);
    vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &count, images.data());
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
    vkBindImageMemory(ctx->device, ctx->depthImage, ctx->depthMemory, 0);

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
    return true;
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
        vkCreateSemaphore(ctx->device, &semInfo, nullptr, &frame.renderFinished);
        vkCreateSemaphore(ctx->device, &semInfo, nullptr, &frame.presentWait);

        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        // The fence starts SIGNALED so the first frame does not wait on a
        // fence that nobody will ever signal.
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(ctx->device, &fenceInfo, nullptr, &frame.inFlight);
    }
    return true;
}

} // namespace

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
    vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, nullptr);
    std::vector<VkExtensionProperties> available(availableCount);
    if (availableCount > 0) {
        vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, available.data());
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

    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &ctx->instance);
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
    vkEnumeratePhysicalDevices(ctx->instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        std::printf("[vk] no Vulkan physical device. (Is an ICD installed? "
                    "On a headless box, try "
                    "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json)\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(ctx->instance, &deviceCount, devices.data());

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

    VkPhysicalDeviceFeatures features = {};
    // MDK's original look needs nothing exotic. Anisotropic sampling and
    // samplerAnisotropy belong to the ENHANCED look, and are requested when
    // that look is enabled.
    features.fillModeNonSolid = VK_TRUE;

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
    deviceInfo.pEnabledFeatures = &features;

    VkResult result = vkCreateDevice(ctx->physicalDevice, &deviceInfo, nullptr, &ctx->device);
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
        vkDeviceWaitIdle(ctx->device);

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
    vkDeviceWaitIdle(ctx->device);

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

bool beginFrame(Context* ctx, uint32_t width, uint32_t height, bool vsync, uint32_t clearColour) {
    if (ctx->extent.width != width || ctx->extent.height != height) {
        if (!recreateSwapchain(ctx, width, height, vsync)) {
            return false;
        }
    }

    // Wait for this frame's fence: the GPU must be done with the resources
    // before the CPU touches them again. This is the GPU-completion
    // boundary, and it is a fence, not a lock (AGENTS.md P3).
    FrameContext& frame = ctx->frames[ctx->currentFrame];
    vkWaitForFences(ctx->device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult result = vkAcquireNextImageKHR(ctx->device, ctx->swapchain, UINT64_MAX,
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

    vkResetCommandBuffer(frame.commandBuffer, 0);
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);

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

    VkRenderPassBeginInfo passInfo = {};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = ctx->renderPass;
    passInfo.framebuffer = ctx->framebuffers[ctx->currentImage];
    passInfo.renderArea.extent = ctx->extent;
    passInfo.clearValueCount = 2;
    passInfo.pClearValues = clears;

    vkCmdBeginRenderPass(frame.commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

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
    vkCmdEndRenderPass(ctx->frames[ctx->currentFrame].commandBuffer);

    FrameContext& frame = ctx->frames[ctx->currentFrame];

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

    vkResetFences(ctx->device, 1, &frame.inFlight);
    VkResult result = vkQueueSubmit(ctx->queue, 1, &submitInfo, frame.inFlight);
    if (result != VK_SUCCESS) {
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
