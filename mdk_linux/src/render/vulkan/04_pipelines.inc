// 04_pipelines.inc -- matrix helpers, the palette and sky pipelines, descriptor sets, the sky.
// Part of renderer_vk.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

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
//
// `cover`: the camera arena's depth over the other arena's (render/draw_queue.h
// step 2): no colour, the depth written where it is FARTHER than what is there
// (GREATER_OR_EQUAL; drawBuckets narrows the depth range to the far plane, so
// that is wherever the draw lies), so the other arena's depth under the camera's
// arena gives way to it.
// KURT'S PASSES (render_types.h KurtPass; docs/DIVERGENCES.md): the depth and
// stencil state of a Mark, Tested or Over draw, over the pass's own. `kurt` is a
// KurtPass value; None leaves the state alone. Used by the palette pipelines and
// the enhanced ones alike, so the two looks mark and draw Kurt the same way.
void kurtDepthStencil(int kurt, VkPipelineDepthStencilStateCreateInfo* ds,
                      VkPipelineColorBlendAttachmentState* cb) {
    if (kurt == (int)mdk::KurtPass::None) {
        return;
    }
    const uint32_t mark = mdk::kKurtStencilBit;
    const uint32_t glass = mdk::kKurtGlassBit;
    const uint32_t drawn = mdk::kKurtDrawnBit;
    VkStencilOpState op = {};
    op.failOp = VK_STENCIL_OP_KEEP;
    op.depthFailOp = VK_STENCIL_OP_KEEP;
    op.passOp = VK_STENCIL_OP_KEEP;
    op.compareOp = VK_COMPARE_OP_ALWAYS;
    op.compareMask = 0;
    op.writeMask = 0;
    op.reference = 0;
    if (kurt == (int)mdk::KurtPass::Mark || kurt == (int)mdk::KurtPass::MarkGlass) {
        // No colour, no depth: its bit, where the surface is in front of
        // everything drawn so far.
        ds->depthTestEnable = VK_TRUE;
        ds->depthWriteEnable = VK_FALSE;
        ds->depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        cb->blendEnable = VK_FALSE;
        cb->colorWriteMask = 0;
        op.passOp = VK_STENCIL_OP_REPLACE;
        op.writeMask = kurt == (int)mdk::KurtPass::Mark ? mark : glass;
        op.reference = op.writeMask;
    } else if (kurt == (int)mdk::KurtPass::Tested) {
        // Where the mark is clear: the pass's own depth state; "drawn" set.
        op.compareOp = VK_COMPARE_OP_EQUAL;
        op.compareMask = mark;
        op.reference = drawn;   // its mark bit is 0
        op.passOp = VK_STENCIL_OP_REPLACE;
        op.writeMask = drawn;
    } else if (kurt == (int)mdk::KurtPass::Over) {
        // Where the mark is set: no depth test (still written); the mark
        // cleared and "drawn" set in one replace.
        ds->depthTestEnable = VK_TRUE;
        ds->depthWriteEnable = VK_TRUE;
        ds->depthCompareOp = VK_COMPARE_OP_ALWAYS;
        op.compareOp = VK_COMPARE_OP_NOT_EQUAL;
        op.compareMask = mark;
        op.reference = drawn;   // its mark bit is 0
        op.passOp = VK_STENCIL_OP_REPLACE;
        op.writeMask = mark | drawn;
    } else {
        // Glass: not where Kurt was drawn over a glass surface of his own space.
        op.compareOp = VK_COMPARE_OP_NOT_EQUAL;
        op.compareMask = glass | drawn;
        op.reference = glass | drawn;
    }
    ds->stencilTestEnable = VK_TRUE;
    ds->front = op;
    ds->back = op;
}

// The Kurt pass a draw's pipeline is made for (04_pipelines.inc kurtDepthStencil):
// its own, or None without a stencil (a Tested draw is then the plain draw).
int kurtPipelinePass(const RendererVk* r, const mdk::MeshDraw* draw) {
    return r->ctx.depthHasStencil ? (int)draw->kurt : (int)mdk::KurtPass::None;
}

VkPipeline createPipeline(RendererVk* r, int pass, int blend, int primitive, bool cover,
                          int kurt) {
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
    if (cover) {
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    }

    const bool blended = !cover &&
                         (pass == (int)mdk::Pass::Blended || pass == (int)mdk::Pass::Overlay);
    VkPipelineColorBlendAttachmentState cb = {};
    cb.blendEnable = blended ? VK_TRUE : VK_FALSE;
    cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cb.dstColorBlendFactor = (blend == (int)mdk::Blend::Add) ? VK_BLEND_FACTOR_ONE
                                                             : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cb.colorBlendOp = VK_BLEND_OP_ADD;
    cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cb.alphaBlendOp = VK_BLEND_OP_ADD;
    cb.colorWriteMask = cover ? 0
                              : (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
    kurtDepthStencil(kurt, &ds, &cb);
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
    // The mirrors have their own program (the oracle's "mirror", Renderer.cs:1064)
    // under the same pass state: tested, written, both faces, opaque.
    const bool mirror = !cover && pass == (int)mdk::Pass::Mirror;
    stages[0].module = mirror ? r->mirrorVertModule : r->vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = mirror ? r->mirrorFragModule : r->fragModule;
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

VkPipeline getPipeline(RendererVk* r, int pass, int blend, int primitive, int kurt) {
    // A Kurt pass's pipeline under keys 256.. (no pass/blend/primitive key
    // reaches them). Kurt's draws are DoubleSided, so Over's 1024 + 16.. stays
    // clear of the cover keys 1024 + Solid/DoubleSided... -- of 1024 + 1 only by
    // that; a Kurt pass on another pass would need its own key range.
    const int key = pass * 16 + blend * 4 + primitive + kurt * 256;
    for (size_t i = 0; i < r->pipelines.size(); i++) {
        if (r->pipelines[i].key == key) {
            return r->pipelines[i].pipeline;
        }
    }
    VkPipeline p = createPipeline(r, pass, blend, primitive, false, kurt);
    PipelineVk entry;
    entry.key = key;
    entry.pipeline = p;
    r->pipelines.push_back(entry);
    return p;
}

// The cover pipeline of a Solid or DoubleSided draw (createPipeline's `cover`),
// kept with the others under a key no pass/blend/primitive key reaches.
VkPipeline getCoverPipeline(RendererVk* r, int pass) {
    const int key = 1024 + pass;
    for (size_t i = 0; i < r->pipelines.size(); i++) {
        if (r->pipelines[i].key == key) {
            return r->pipelines[i].pipeline;
        }
    }
    VkPipeline p = createPipeline(r, pass, 0, 0, true, 0);
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

// A stereo eye's clip-to-direction: the inverse of (rotation * projection),
// which keeps the eye's shifted frustum (upstream 5f9ea5f2 keeps it for eyes).
bool skyInverse(RendererVk* r, float c2d[16]) {
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
        return false;
    }
    zaxis.x /= zLen;
    zaxis.y /= zLen;
    zaxis.z /= zLen;
    mdk::Vec3 right{up.y * zaxis.z - up.z * zaxis.y,
                    up.z * zaxis.x - up.x * zaxis.z,
                    up.x * zaxis.y - up.y * zaxis.x};
    const float rightLen = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
    if (rightLen <= 0.0f) {
        return false;
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
    return mdk::invertSystemNumericsMatrix4(rp, c2d);
}

// The centre camera's clip-to-direction, built from its axes and projection
// (upstream 5f9ea5f2, ViewCamera.SkyDirections): (x, y, 1, 1) goes to
// forward + right*x' + up*y', all on one plane. The shader blends the screen
// triangle's corner directions across the pixels, which is right only for
// corners in one plane; inverting rotation * projection in float (near 0.5,
// far 20000) put them off it at clip depth 1 by an error that changed with
// the camera's angle, and the sky shook by pixels when turning.
bool skyDirections(const mdk::Camera& camera, float m[16]) {
    const mdk::Vec3 f = camera.forward;
    const float fLen = sqrtf(f.x * f.x + f.y * f.y + f.z * f.z);
    if (fLen <= 0.0f) {
        return false;
    }
    const mdk::Vec3 forward{f.x / fLen, f.y / fLen, f.z / fLen};
    const mdk::Vec3 up0 = camera.up;
    mdk::Vec3 right{forward.y * up0.z - forward.z * up0.y,
                    forward.z * up0.x - forward.x * up0.z,
                    forward.x * up0.y - forward.y * up0.x};
    const float rLen = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
    if (rLen <= 0.0f) {
        return false;
    }
    right.x /= rLen;
    right.y /= rLen;
    right.z /= rLen;
    const mdk::Vec3 up{right.y * forward.z - right.z * forward.y,
                       right.z * forward.x - right.x * forward.z,
                       right.x * forward.y - right.y * forward.x};
    const float* p = camera.projection;  // row-major: M11 p[0], M22 p[5], M31 p[8], M32 p[9]
    const float sx = p[8] / p[0];
    const float sy = p[9] / p[5];
    m[0] = right.x / p[0];  m[1] = right.y / p[0];  m[2] = right.z / p[0];  m[3] = 0.0f;
    m[4] = up.x / p[5];     m[5] = up.y / p[5];     m[6] = up.z / p[5];     m[7] = 0.0f;
    m[8] = forward.x + right.x * sx + up.x * sy;
    m[9] = forward.y + right.y * sx + up.y * sy;
    m[10] = forward.z + right.z * sx + up.z * sy;
    m[11] = 0.0f;
    m[12] = 0.0f;           m[13] = 0.0f;           m[14] = 0.0f;           m[15] = 1.0f;
    return true;
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
    // C# Renderer: `if (Panorama != null && Backdrop == Backdrop.Sky) DrawSky(...)` --
    // the scripts' sky mode 1 (black) and -1 (the last frame) draw none (package #15).
    // The C# draws Keep by LOADING the scene target instead of clearing it; this
    // backend's passes always clear, so Keep shows the clear colour where nothing is
    // drawn (in the retail arenas that set it, closed rooms, nothing is uncovered).
    if (r->backdrop != mdk::Backdrop::Sky) {
        return;
    }
    if (!r->skyPipeline) {
        r->skyPipeline = createSkyPipeline(r);
        if (!r->skyPipeline) {
            return;
        }
    }

    float c2d[16];
    const bool built = r->eyeSide == 0.0f ? skyDirections(r->activeCamera, c2d)
                                          : skyInverse(r, c2d);
    if (!built) {
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

