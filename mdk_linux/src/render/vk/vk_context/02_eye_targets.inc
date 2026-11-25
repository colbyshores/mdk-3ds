// vk_context/02_eye_targets.inc -- the stereo eye targets: destroyEyeTargets and ensureEyeTargets.
// Part of vk_context.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

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
