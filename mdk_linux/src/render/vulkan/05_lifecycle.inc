// 05_lifecycle.inc -- resource slots, create/destroy, the backend name and capabilities.
// Part of renderer_vk.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

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
    r->stereo = desc->stereo;
    r->enhancedLook = desc->enhanced_look;
    r->toggles = desc->enhanced_toggles;
    r->activeCamera = r->camera;

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

void destroyGammaTarget(RendererVk* r);

void destroyPostTargets(RendererVk* r) {
    VkDevice device = r->ctx.device;
    if (r->postSceneFramebuffer) vkDestroyFramebuffer(device, r->postSceneFramebuffer, nullptr);
    if (r->viewDepthFramebuffer) vkDestroyFramebuffer(device, r->viewDepthFramebuffer, nullptr);
    if (r->occlusionFramebuffer) vkDestroyFramebuffer(device, r->occlusionFramebuffer, nullptr);
    if (r->postFramebuffer) vkDestroyFramebuffer(device, r->postFramebuffer, nullptr);
    if (r->canvasFramebuffer) vkDestroyFramebuffer(device, r->canvasFramebuffer, nullptr);
    r->postSceneFramebuffer = r->viewDepthFramebuffer = r->occlusionFramebuffer =
        r->postFramebuffer = r->canvasFramebuffer = VK_NULL_HANDLE;
    if (r->postSceneLevel0) vkDestroyImageView(device, r->postSceneLevel0, nullptr);
    if (r->postSceneView) vkDestroyImageView(device, r->postSceneView, nullptr);
    if (r->postSceneImage) vkDestroyImage(device, r->postSceneImage, nullptr);
    if (r->postSceneMemory) vkFreeMemory(device, r->postSceneMemory, nullptr);
    if (r->viewDepthView) vkDestroyImageView(device, r->viewDepthView, nullptr);
    if (r->viewDepthImage) vkDestroyImage(device, r->viewDepthImage, nullptr);
    if (r->viewDepthMemory) vkFreeMemory(device, r->viewDepthMemory, nullptr);
    if (r->occlusionView) vkDestroyImageView(device, r->occlusionView, nullptr);
    if (r->occlusionImage) vkDestroyImage(device, r->occlusionImage, nullptr);
    if (r->occlusionMemory) vkFreeMemory(device, r->occlusionMemory, nullptr);
    r->postSceneLevel0 = r->postSceneView = VK_NULL_HANDLE;
    r->postSceneImage = VK_NULL_HANDLE;
    r->postSceneMemory = VK_NULL_HANDLE;
    r->viewDepthView = VK_NULL_HANDLE;
    r->viewDepthImage = VK_NULL_HANDLE;
    r->viewDepthMemory = VK_NULL_HANDLE;
    r->occlusionView = VK_NULL_HANDLE;
    r->occlusionImage = VK_NULL_HANDLE;
    r->occlusionMemory = VK_NULL_HANDLE;
    r->postWidth = 0;
    r->postHeight = 0;
    r->postMips = 0;
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
    // The retired images, drained rather than settled: the device is idle here, so
    // their boundary has already passed. Draining them is not optional — an image
    // still in this vector at this point has no other owner, and the textures and
    // palettes loops above cannot see it because their fields were nulled at the
    // retire so that this drain would be the only one.
    for (size_t i = 0; i < r->retiringImages.size(); i++) {
        VkImage image = r->retiringImages[i].image;
        VkDeviceMemory memory = r->retiringImages[i].memory;
        VkImageView view = r->retiringImages[i].view;
        destroyImage(&r->ctx, &image, &memory, &view, nullptr);
    }
    r->retiringImages.clear();
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
    if (r->stereoPipeline) vkDestroyPipeline(r->ctx.device, r->stereoPipeline, nullptr);
    if (r->stereoPipeLayout) vkDestroyPipelineLayout(r->ctx.device, r->stereoPipeLayout, nullptr);
    if (r->stereoSetLayout) vkDestroyDescriptorSetLayout(r->ctx.device, r->stereoSetLayout, nullptr);
    if (r->stereoPool) vkDestroyDescriptorPool(r->ctx.device, r->stereoPool, nullptr);
    if (r->eyeSampler) vkDestroySampler(r->ctx.device, r->eyeSampler, nullptr);
    if (r->stereoVertModule) vkDestroyShaderModule(r->ctx.device, r->stereoVertModule, nullptr);
    if (r->stereoFragModule) vkDestroyShaderModule(r->ctx.device, r->stereoFragModule, nullptr);
    for (size_t i = 0; i < r->enhancedPipelines.size(); i++) {
        if (r->enhancedPipelines[i].pipeline) {
            vkDestroyPipeline(r->ctx.device, r->enhancedPipelines[i].pipeline, nullptr);
        }
    }
    for (size_t i = 0; i < r->colours.size(); i++) {
        retireImage(r, r->colours[i].image, r->colours[i].memory, r->colours[i].view);
    }
    if (r->dummyColourView) {
        retireImage(r, r->dummyColourImage, r->dummyColourMemory, r->dummyColourView);
    }
    if (r->enhancedPipeLayout) vkDestroyPipelineLayout(r->ctx.device, r->enhancedPipeLayout, nullptr);
    if (r->enhancedSetLayout) vkDestroyDescriptorSetLayout(r->ctx.device, r->enhancedSetLayout, nullptr);
    if (r->colourSampler) vkDestroySampler(r->ctx.device, r->colourSampler, nullptr);
    if (r->enhancedVertModule) vkDestroyShaderModule(r->ctx.device, r->enhancedVertModule, nullptr);
    if (r->enhancedFragModule) vkDestroyShaderModule(r->ctx.device, r->enhancedFragModule, nullptr);
    for (uint32_t i = 0; i < mdk::vk::kFramesInFlight; i++) {
        if (r->enhancedPools[i]) vkDestroyDescriptorPool(r->ctx.device, r->enhancedPools[i], nullptr);
        destroyBufferPair(r->ctx.device, r->uniformRing[i], r->uniformRingMemory[i]);
        destroyBufferPair(r->ctx.device, r->lightBuffers[i], r->lightMemory[i]);
    }
    if (r->depthPipelines[0]) vkDestroyPipeline(r->ctx.device, r->depthPipelines[0], nullptr);
    if (r->depthPipelines[1]) vkDestroyPipeline(r->ctx.device, r->depthPipelines[1], nullptr);
    if (r->depthFragModule) vkDestroyShaderModule(r->ctx.device, r->depthFragModule, nullptr);
    if (r->shadowFramebuffer) vkDestroyFramebuffer(r->ctx.device, r->shadowFramebuffer, nullptr);
    if (r->shadowView) vkDestroyImageView(r->ctx.device, r->shadowView, nullptr);
    if (r->shadowImage) vkDestroyImage(r->ctx.device, r->shadowImage, nullptr);
    if (r->shadowMemory) vkFreeMemory(r->ctx.device, r->shadowMemory, nullptr);
    if (r->shadowPass) vkDestroyRenderPass(r->ctx.device, r->shadowPass, nullptr);
    destroyPostTargets(r);
    if (r->canvasLoadPass) vkDestroyRenderPass(r->ctx.device, r->canvasLoadPass, nullptr);
    if (r->occlusionPipeline) vkDestroyPipeline(r->ctx.device, r->occlusionPipeline, nullptr);
    if (r->postPipeline) vkDestroyPipeline(r->ctx.device, r->postPipeline, nullptr);
    if (r->postPipeLayout) vkDestroyPipelineLayout(r->ctx.device, r->postPipeLayout, nullptr);
    if (r->postSetLayout) vkDestroyDescriptorSetLayout(r->ctx.device, r->postSetLayout, nullptr);
    if (r->postPool) vkDestroyDescriptorPool(r->ctx.device, r->postPool, nullptr);
    if (r->postMipSampler) vkDestroySampler(r->ctx.device, r->postMipSampler, nullptr);
    if (r->postClampSampler) vkDestroySampler(r->ctx.device, r->postClampSampler, nullptr);
    if (r->postVertModule) vkDestroyShaderModule(r->ctx.device, r->postVertModule, nullptr);
    if (r->occlusionFragModule) vkDestroyShaderModule(r->ctx.device, r->occlusionFragModule, nullptr);
    if (r->postFragModule) vkDestroyShaderModule(r->ctx.device, r->postFragModule, nullptr);
    for (uint32_t i = 0; i < mdk::vk::kFramesInFlight; i++) {
        destroyBufferPair(r->ctx.device, r->postUniformBuffers[i], r->postUniformMemory[i]);
    }
    destroyGammaTarget(r);
    if (r->gammaPipeline) vkDestroyPipeline(r->ctx.device, r->gammaPipeline, nullptr);
    if (r->gammaPipeLayout) vkDestroyPipelineLayout(r->ctx.device, r->gammaPipeLayout, nullptr);
    if (r->gammaSetLayout) vkDestroyDescriptorSetLayout(r->ctx.device, r->gammaSetLayout, nullptr);
    if (r->gammaPool) vkDestroyDescriptorPool(r->ctx.device, r->gammaPool, nullptr);
    if (r->gammaVertModule) vkDestroyShaderModule(r->ctx.device, r->gammaVertModule, nullptr);
    if (r->gammaFragModule) vkDestroyShaderModule(r->ctx.device, r->gammaFragModule, nullptr);
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
