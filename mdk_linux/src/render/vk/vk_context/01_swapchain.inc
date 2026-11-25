// vk_context/01_swapchain.inc -- the swapchain, the render passes, the depth image, the scene target, the framebuffers and the per-frame resources.
// Part of vk_context.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.
// Pasted INSIDE vk_context.cpp's anonymous namespace: these stay file-local.

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
