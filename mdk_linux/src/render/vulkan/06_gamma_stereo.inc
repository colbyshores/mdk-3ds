// 06_gamma_stereo.inc -- the gamma grade and the flat-stereo composite.
// Part of renderer_vk.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.


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
