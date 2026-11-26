// 09_colour.inc -- the enhanced colour cache: expansions, replacements, mip chains.
// Part of renderer_vk.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

struct EnhancedUniforms {
    float colour[4];
    int32_t textured;
    int32_t frameCount;
    int32_t frame;
    int32_t mode;
    float sunMatrix[16];
    float sun[4];
    float sunColour[4];
    float sky[4];
    float ground[4];
    float camera[4];
    float haze[4];
    float shadow[4];
};

// A colour texture's GPU image from its expanded chain: RGBA8, a mip
// chain, one array layer per frame. Uploaded through a staging buffer in
// one one-shot submission, like every other upload here.
bool buildColourImage(RendererVk* r, int32_t w, int32_t h, int32_t frames,
                      const std::vector<uint8_t>& chain, ColourVk* out) {
    const int32_t levels = mdk::colourLevels(w, h);
    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent.width = (uint32_t)w;
    imgInfo.extent.height = (uint32_t)h;
    imgInfo.extent.depth = 1;
    imgInfo.mipLevels = (uint32_t)levels;
    imgInfo.arrayLayers = (uint32_t)frames;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(r->ctx.device, &imgInfo, nullptr, &out->image) != VK_SUCCESS) {
        refused("colour texture image %dx%d x%d frames: the surface draws "
                "through the original pipeline instead", w, h, frames);
        return false;
    }
    VkMemoryRequirements req = {};
    vkGetImageMemoryRequirements(r->ctx.device, out->image, &req);
    const uint32_t type = findMemoryType(&r->ctx, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == 0xFFFFFFFFu) {
        refused("colour texture memory type: the surface draws through the "
                "original pipeline instead");
        vkDestroyImage(r->ctx.device, out->image, nullptr);
        out->image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(r->ctx.device, &alloc, nullptr, &out->memory) != VK_SUCCESS ||
        vkBindImageMemory(r->ctx.device, out->image, out->memory, 0) != VK_SUCCESS) {
        refused("colour texture memory: the surface draws through the original "
                "pipeline instead");
        vkDestroyImage(r->ctx.device, out->image, nullptr);
        out->image = VK_NULL_HANDLE;
        if (out->memory != VK_NULL_HANDLE) {
            vkFreeMemory(r->ctx.device, out->memory, nullptr);
            out->memory = VK_NULL_HANDLE;
        }
        return false;
    }

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = out->image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = (uint32_t)levels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = (uint32_t)frames;
    if (vkCreateImageView(r->ctx.device, &viewInfo, nullptr, &out->view) != VK_SUCCESS) {
        refused("colour texture view: the surface draws through the original "
                "pipeline instead");
        vkDestroyImage(r->ctx.device, out->image, nullptr);
        out->image = VK_NULL_HANDLE;
        vkFreeMemory(r->ctx.device, out->memory, nullptr);
        out->memory = VK_NULL_HANDLE;
        return false;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    void* stagingMapped = nullptr;
    if (!createHostBuffer(&r->ctx, (VkDeviceSize)chain.size(),
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &stagingMemory,
                          &stagingMapped)) {
        refused("colour texture staging: the surface draws through the original "
                "pipeline instead");
        return false;
    }
    memcpy(stagingMapped, chain.data(), chain.size());

    VkCommandPool oneShotPool = VK_NULL_HANDLE;
    VkCommandBuffer cb = beginOneShotTracked(&r->ctx, &oneShotPool);
    if (!cb) {
        refused("colour texture upload command: the surface draws through the "
                "original pipeline instead");
        destroyBufferPair(r->ctx.device, staging, stagingMemory);
        return false;
    }
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = out->image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = (uint32_t)levels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = (uint32_t)frames;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);

    const int32_t chainSize = mdk::colourChainSize(w, h);
    std::vector<VkBufferImageCopy> regions;
    for (int32_t f = 0; f < frames; f++) {
        int32_t lw = w;
        int32_t lh = h;
        VkDeviceSize offset = (VkDeviceSize)f * (VkDeviceSize)chainSize;
        for (int32_t level = 0; level < levels; level++) {
            VkBufferImageCopy region = {};
            region.bufferOffset = offset;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = (uint32_t)level;
            region.imageSubresource.baseArrayLayer = (uint32_t)f;
            region.imageSubresource.layerCount = 1;
            region.imageExtent.width = (uint32_t)lw;
            region.imageExtent.height = (uint32_t)lh;
            region.imageExtent.depth = 1;
            regions.push_back(region);
            offset += (VkDeviceSize)lw * (VkDeviceSize)lh * 4;
            lw = lw / 2 > 1 ? lw / 2 : 1;
            lh = lh / 2 > 1 ? lh / 2 : 1;
        }
    }
    vkCmdCopyBufferToImage(cb, staging, out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           (uint32_t)regions.size(), regions.data());

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &barrier);
    const OneShot done = endOneShot(&r->ctx, oneShotPool, cb);
    destroyBufferPair(r->ctx.device, staging, stagingMemory);
    if (done != OneShot::done) {
        refused("colour texture upload: the copy is not known to have finished, so "
                "the surface draws through the original pipeline instead");
        return false;
    }
    return true;
}

// Drop the built colour texture for a (texture, palette, frames) triple,
// retiring its image for the frames that may still sample it.
void dropColour(RendererVk* r, int32_t tex, int32_t pal, int32_t frames) {
    for (size_t i = 0; i < r->colours.size(); i++) {
        ColourVk& c = r->colours[i];
        if (c.used && c.tex == tex && c.pal == pal && c.frames == frames) {
            retireImage(r, c.image, c.memory, c.view);
            r->colours.erase(r->colours.begin() + (int)i);
            return;
        }
    }
}

// The replacement registered for a triple, or null. A replacement whose
// index texture has changed is stale (the C#'s Changed): it and its built
// colour texture go, and the expansion returns. Canvas images stay.
const ReplacementVk* findReplacement(RendererVk* r, int32_t tex, int32_t pal, int32_t frames) {
    for (size_t i = 0; i < r->replacements.size(); i++) {
        ReplacementVk& rep = r->replacements[i];
        if (!rep.used || rep.tex != tex || rep.pal != pal || rep.frames != frames) {
            continue;
        }
        if (!rep.image && tex >= 0 && tex < (int32_t)r->textures.size() &&
            r->textures[tex].version != rep.textureVersion) {
            dropColour(r, tex, pal, frames);
            r->replacements.erase(r->replacements.begin() + (int)i);
            return nullptr;
        }
        return &rep;
    }
    return nullptr;
}

// Whether a canvas image replacement (mdk_render_replace_image) is
// registered for a (texture, palette) pair: the enhanced canvas draws the
// image (mode 4) instead of filtering the index texture (mode 3).
bool hasImageReplacement(RendererVk* r, int32_t tex, int32_t pal) {
    for (size_t i = 0; i < r->replacements.size(); i++) {
        const ReplacementVk& rep = r->replacements[i];
        if (rep.used && rep.image && rep.tex == tex && rep.pal == pal) {
            return true;
        }
    }
    return false;
}

// A replacement's mip chain: each frame's RGBA into its chain level 0,
// then the shared box filter (render/colour_mips) builds the rest.
std::vector<uint8_t> replacementChain(const ReplacementVk& rep) {
    const int32_t frameBytes = mdk::colourChainSize(rep.width, rep.height);
    std::vector<uint8_t> chain((size_t)frameBytes * (size_t)rep.frames);
    const size_t framePixels = (size_t)rep.width * (size_t)rep.height * 4;
    for (int32_t f = 0; f < rep.frames; f++) {
        uint8_t* level0 = chain.data() + (size_t)frameBytes * (size_t)f;
        memcpy(level0, rep.rgba.data() + framePixels * (size_t)f, framePixels);
        mdk::colourFillChain(level0, rep.width, rep.height);
    }
    return chain;
}

// Register (or re-register) a mod's colour image for a triple. The built
// colour texture for the triple goes, so the next draw builds this.
void registerReplacement(RendererVk* r, int32_t tex, int32_t pal, int32_t frames, int32_t width,
                         int32_t height, const uint8_t* rgba, bool image) {
    if (!r || !r->ready || !rgba || width <= 0 || height <= 0 || frames <= 0 || tex < 0 ||
        tex >= (int32_t)r->textures.size() || !r->textures[tex].used || pal < 0 ||
        pal >= (int32_t)r->palettes.size() || !r->palettes[pal].used) {
        return;
    }
    ReplacementVk rep;
    rep.used = true;
    rep.image = image;
    rep.tex = tex;
    rep.pal = pal;
    rep.frames = frames;
    rep.width = width;
    rep.height = height;
    rep.textureVersion = r->textures[tex].version;
    rep.rgba.assign(rgba, rgba + (size_t)width * (size_t)height * (size_t)frames * 4);
    for (size_t i = 0; i < r->replacements.size(); i++) {
        ReplacementVk& old = r->replacements[i];
        if (old.used && old.tex == tex && old.pal == pal && old.frames == frames) {
            // A canvas image keeps its image flag when a material
            // replacement lands on the same triple (the C# keeps _images).
            rep.image = rep.image || old.image;
            r->replacements.erase(r->replacements.begin() + (int)i);
            break;
        }
    }
    r->replacements.push_back(rep);
    dropColour(r, tex, pal, frames);
}

// The colour texture for a (texture, palette, frames) triple, built or
// rebuilt when the index texture's version has moved (bullet holes).
VkImageView getColourView(RendererVk* r, int32_t tex, int32_t pal, int32_t frames) {
    if (tex < 0 || tex >= (int32_t)r->textures.size() || !r->textures[tex].used ||
        !r->textures[tex].isIndex || r->textures[tex].indices.empty()) {
        refused("colour texture for texture %d: it is not a live index texture, so "
                "the surface draws through the original pipeline instead", tex);
        return VK_NULL_HANDLE;
    }
    if (pal < 0 || pal >= (int32_t)r->palettes.size() || !r->palettes[pal].used) {
        refused("colour texture for palette %d: it is not a live palette, so the "
                "surface draws through the original pipeline instead", pal);
        return VK_NULL_HANDLE;
    }
    const TextureVk& t = r->textures[tex];
    // A layer is ONE FRAME: the index texture stacks its frames downwards, so a
    // frame is height / frames rows (the C#'s SizeOf, Renderer.Colours.cs), and
    // indices of whole frames are the C#'s precondition (CanExpand). The full
    // height here read `frames` times past the indices (EXPLODE 128x3328 x 26:
    // a segfault in colourChain) and gave every animated texture wrong layers.
    if (frames <= 0 || t.height % frames != 0) {
        refused("colour texture for texture %d: %d rows are not %d whole frames, so the "
                "surface draws through the original pipeline instead", tex, t.height, frames);
        return VK_NULL_HANDLE;
    }
    const int32_t frameHeight = t.height / frames;
    // A mod's replacement image, when one is registered for this triple,
    // is the colour texture instead of the expansion (a stale one has
    // already gone in findReplacement).
    const ReplacementVk* rep = findReplacement(r, tex, pal, frames);
    for (size_t i = 0; i < r->colours.size(); i++) {
        ColourVk& c = r->colours[i];
        if (c.used && c.tex == tex && c.pal == pal && c.frames == frames) {
            if (c.version == t.version) {
                return c.view;
            }
            retireImage(r, c.image, c.memory, c.view);
            c.image = VK_NULL_HANDLE;
            c.memory = VK_NULL_HANDLE;
            c.view = VK_NULL_HANDLE;
            const std::vector<uint8_t> chain =
                rep ? replacementChain(*rep)
                    : mdk::colourChain(t.indices.data(), t.width, frameHeight, frames,
                                       r->palettes[pal].rgba, 1024);
            if (!buildColourImage(r, rep ? rep->width : t.width, rep ? rep->height : frameHeight,
                                  frames, chain, &c)) {
                refused("colour texture rebuild for texture %d: the surface draws "
                        "through the original pipeline instead", tex);
                c.used = false;
                return VK_NULL_HANDLE;
            }
            c.version = t.version;
            return c.view;
        }
    }
    ColourVk c;
    c.used = true;
    c.tex = tex;
    c.pal = pal;
    c.frames = frames;
    c.version = t.version;
    const std::vector<uint8_t> chain =
        rep ? replacementChain(*rep)
            : mdk::colourChain(t.indices.data(), t.width, frameHeight, frames,
                               r->palettes[pal].rgba, 1024);
    if (!buildColourImage(r, rep ? rep->width : t.width, rep ? rep->height : frameHeight,
                          frames, chain, &c)) {
        refused("colour texture build for texture %d: the surface draws through "
                "the original pipeline instead", tex);
        return VK_NULL_HANDLE;
    }
    r->colours.push_back(c);
    return c.view;
}

