// 11_shadow_post.inc -- the sun shadow, the post targets, depth, mips, the post draw.
// Part of renderer_vk.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

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
    // [0-1]: back-face culled (Solid) and not; [2-3]: the same, keeping the
    // FARTHER depth (GREATER_OR_EQUAL) -- the camera arena's cover step in the
    // camera's depth (drawCameraDepth; render/draw_queue.h).
    for (int variant = 0; variant < 4; variant++) {
        const int cull = variant % 2;
        const bool cover = variant >= 2;
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
        ds.depthCompareOp = cover ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
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
                                      &r->depthPipelines[variant]) != VK_SUCCESS) {
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

    // Every caster, the other arena's `back` draws too (render/draw_queue.h): the
    // sun's map has no camera, so their order does not matter here.
    for (int32_t slot = 0; slot < 2 * mdk::kDrawQueuePassCount; slot++) {
        const std::vector<mdk::MeshDraw>& bucket =
            slot < mdk::kDrawQueuePassCount
                ? r->queue.backBucket(slot)
                : r->queue.bucket(slot - mdk::kDrawQueuePassCount);
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
    // The scene's three steps (render/draw_queue.h): the other arena's `back`
    // draws, the camera arena's `cover` draws' depth over them (GREATER_OR_EQUAL,
    // depthPipelines[2-3]), every draw. Without back draws only the last runs.
    for (int32_t slot = 0; slot < 3 * mdk::kDrawQueuePassCount; slot++) {
        const int32_t step = slot / mdk::kDrawQueuePassCount;
        const int32_t bucketIdx = slot % mdk::kDrawQueuePassCount;
        if (step < 2 && !r->queue.hasBack()) {
            continue;
        }
        const std::vector<mdk::MeshDraw>& bucket =
            step == 0 ? r->queue.backBucket(bucketIdx) : r->queue.bucket(bucketIdx);
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
            if (step == 1 && (!draw.cover || draw.material.pass == mdk::Pass::Mirror)) {
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

            VkPipeline pipeline = r->depthPipelines[(draw.material.pass == mdk::Pass::Solid ? 0 : 1) +
                                                    (step == 1 ? 2 : 0)];
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
