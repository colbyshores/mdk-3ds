// 10_enhanced.inc -- the enhanced pipelines and draws, the enhanced canvas.
// Part of renderer_vk.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

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

