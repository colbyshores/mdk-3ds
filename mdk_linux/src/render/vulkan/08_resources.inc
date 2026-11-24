// 08_resources.inc -- textures, palettes, the mod replacements, meshes, scopes.
// Part of renderer_vk.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

MdkTextureId mdk_render_create_index_texture(MdkRenderer r_, int32_t w, int32_t h,
                                             const uint8_t* px, const uint8_t* paletteRgba) {
    // The Vulkan backend IGNORES the palette bytes: it samples the index
    // texture and then the palette in the shader, exactly as the C# does.
    // The palette reaches the shader through the material's palette id.
    (void)paletteRgba;
    RendererVk* r = asVk(r_);
    if (!r || !r->ready || !px || w <= 0 || h <= 0) {
        return -1;
    }
    const int32_t id = acquireSlot(r->textures);
    TextureVk& t = r->textures[id];
    if (!createHostImage(&r->ctx, w, h, VK_FORMAT_R8_UNORM, 1, px, &t.image, &t.memory,
                         &t.view, &t.mapped)) {
        refused("index texture %dx%d (%llu bytes): the surface it belongs to draws untextured",
                w, h, (unsigned long long)((VkDeviceSize)w * (VkDeviceSize)h));
        t.used = false;
        return -1;
    }
    t.isIndex = true;
    t.width = w;
    t.height = h;
    t.size = (VkDeviceSize)w * (VkDeviceSize)h;
    t.indices.assign(px, px + (size_t)w * (size_t)h);
    t.version = 1;
    ResRef ref;
    ref.kind = 0;
    ref.id = id;
    r->creationLog.push_back(ref);
    return id;
}

int32_t mdk_render_index_texture_bytes(MdkRenderer r_, int32_t w, int32_t h) {
    (void)r_;
    if (w <= 0 || h <= 0) {
        return 0;
    }
    // WHAT THE DESKTOP SPENDS ON AN INDEX TEXTURE. The indices are kept as
    // they are -- an R8_UNORM image, one byte a pixel -- and the palette
    // lookup happens in the shader. So the texture is exactly its own
    // pixels, and the 3DS's expanded, padded RGBA8 is eight times that.
    return w * h;
}

// THE DESKTOP HAS NO COMPRESSED TEXTURES: it keeps the palette indices (one
// byte a pixel, the lookup in the shader), so there is nothing a compressed
// frame would save. The answer is "none", and the caller makes the index texture
// as it always did (render.h).
MdkTextureId mdk_render_create_compressed_texture(MdkRenderer, int32_t, int32_t, const uint8_t*,
                                                  int32_t) {
    return -1;
}

// Same answer from a file (render.h): the desktop makes the index texture.
MdkTextureId mdk_render_create_compressed_file(MdkRenderer, int32_t, int32_t, const char*,
                                               const uint8_t*) {
    return -1;
}

// No texture here is made from a file, so none ever wants its pixels (render.h).
bool mdk_render_texture_needs_pixels(MdkRenderer, MdkTextureId) {
    return false;
}

// NO STREAMED PACK READS ON THE DESKTOP: it never loads Kurt's compressed pack
// (it has no compressed textures), so there is nothing to stream (render.h).
bool mdk_render_read_range_async(MdkRenderer, int32_t, const char*, uint32_t, uint32_t) {
    return false;
}

bool mdk_render_poll_range_read(MdkRenderer, int32_t*, uint8_t**, uint32_t*, bool*) {
    return false;
}

// NO LENT FILE READER ON THE DESKTOP: its menu screens read at once, as the C#'s
// (render.h).
bool mdk_render_has_file_reader(MdkRenderer) {
    return false;
}

bool mdk_render_read_file_async(MdkRenderer, int32_t, const char*, MdkFileWork) {
    return false;
}

bool mdk_render_poll_file_read(MdkRenderer, int32_t, uint8_t**, uint32_t*, bool*) {
    return false;
}

void mdk_render_cancel_file_read(MdkRenderer, int32_t) {}

// NOR FOR THE MUSIC PREFETCH: the desktop's bank reads its steps on the frame
// (render.h).
bool mdk_render_read_music_async(MdkRenderer, int32_t, const char*, uint32_t, uint32_t) {
    return false;
}

bool mdk_render_poll_music_read(MdkRenderer, int32_t, uint8_t**, uint32_t*, bool*) {
    return false;
}

void mdk_render_cancel_music_read(MdkRenderer, int32_t) {}

// No per-texture answer here: callers fall back to mdk_render_index_texture_bytes,
// which is the desktop's exact cost of an index texture (render.h).
int32_t mdk_render_texture_bytes(MdkRenderer, MdkTextureId) {
    return 0;
}

MdkTextureId mdk_render_create_rgba_texture(MdkRenderer r_, int32_t w, int32_t h,
                                            const uint8_t* px) {
    RendererVk* r = asVk(r_);
    if (!r || !r->ready || !px || w <= 0 || h <= 0) {
        return -1;
    }
    const int32_t id = acquireSlot(r->textures);
    TextureVk& t = r->textures[id];
    if (!createHostImage(&r->ctx, w, h, VK_FORMAT_R8G8B8A8_UNORM, 4, px, &t.image, &t.memory,
                         &t.view, &t.mapped)) {
        refused("RGBA texture %dx%d (%llu bytes): whatever asked for it draws without it",
                w, h, (unsigned long long)((VkDeviceSize)w * (VkDeviceSize)h * 4));
        t.used = false;
        return -1;
    }
    t.isIndex = false;
    t.width = w;
    t.height = h;
    t.size = (VkDeviceSize)w * (VkDeviceSize)h * 4;
    ResRef ref;
    ref.kind = 0;
    ref.id = id;
    r->creationLog.push_back(ref);
    return id;
}

MdkPaletteId mdk_render_create_palette(MdkRenderer r_, const uint8_t* rgba) {
    RendererVk* r = asVk(r_);
    if (!r || !r->ready || !rgba) {
        return -1;
    }
    const int32_t id = acquireSlot(r->palettes);
    PaletteVk& p = r->palettes[id];
    memcpy(p.rgba, rgba, sizeof(p.rgba));
    if (!createHostImage(&r->ctx, 256, 1, VK_FORMAT_R8G8B8A8_UNORM, 4, rgba, &p.image,
                         &p.memory, &p.view, &p.mapped)) {
        refused("palette (256x1 RGBA, 1024 bytes): every surface that names palette id %d "
                "draws without a palette", id);
        p.used = false;
        return -1;
    }
    ResRef ref;
    ref.kind = 1;
    ref.id = id;
    r->creationLog.push_back(ref);
    return id;
}

void mdk_render_update_texture(MdkRenderer r_, MdkTextureId t_, int32_t w, int32_t h,
                               const uint8_t* px) {
    RendererVk* r = asVk(r_);
    if (!r || t_ < 0 || t_ >= (int32_t)r->textures.size() || !r->textures[t_].used || !px) {
        return;
    }
    TextureVk& t = r->textures[t_];
    if (t.width != w || t.height != h) {
        return;  // the contract is same-size updates only
    }
    // The image is OPTIMAL-tiled with no persistent mapping (see
    // createHostImage); an update re-creates the image through the staging
    // path. Updates are rare (animated textures), not per-frame-per-texture.
    // The cached descriptor sets name the old image view, so they must go
    // before that view is destroyed.
    forgetDescriptorSets(r, t_, -1);
    if (t.arrayView) {
        vkDestroyImageView(r->ctx.device, t.arrayView, nullptr);
        t.arrayView = VK_NULL_HANDLE;
    }
    VkImage oldImage = t.image;
    VkDeviceMemory oldMemory = t.memory;
    VkImageView oldView = t.view;
    t.image = VK_NULL_HANDLE;
    t.memory = VK_NULL_HANDLE;
    t.view = VK_NULL_HANDLE;
    if (createHostImage(&r->ctx, w, h, t.isIndex ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM,
                        t.isIndex ? 1u : 4u, px, &t.image, &t.memory, &t.view, &t.mapped)) {
        if (t.isIndex) {
            t.indices.assign(px, px + (size_t)w * (size_t)h);
            t.version++;
            // A changed texture shows its expansion again: its material
            // replacements (not its canvas images) are stale, as in the
            // C#'s Changed. Their colour textures rebuild on next draw
            // through the version check in getColourView.
            for (size_t i = 0; i < r->replacements.size();) {
                const ReplacementVk& rep = r->replacements[i];
                if (rep.used && !rep.image && rep.tex == t_) {
                    r->replacements.erase(r->replacements.begin() + (int)i);
                } else {
                    i++;
                }
            }
        }
        // Retired, not destroyed: a frame already on the queue may still be
        // sampling this view through a set that was retired a few lines above.
        retireImage(r, oldImage, oldMemory, oldView);
    } else {
        refused("re-made texture %d at %dx%d: keeping the image it had, so an animated "
                "surface freezes on its last frame instead of disappearing", t_, w, h);
        t.image = oldImage;
        t.memory = oldMemory;
        t.view = oldView;
    }
}

// A streamed picture (render.h; package #20c's movies and splash): the C#'s
// PalettedImage.SetIndices and SetPalette, each the image re-made through the
// staging path as update_texture does. A palette that did not change is not
// re-made (the menu's last video frame stays for minutes).
void mdk_render_update_paletted(MdkRenderer r_, MdkTextureId t_, MdkPaletteId p_,
                                const uint8_t* indices, const uint8_t* rgba256) {
    RendererVk* r = asVk(r_);
    if (!r) {
        return;
    }
    const bool hasTexture =
        t_ >= 0 && t_ < (int32_t)r->textures.size() && r->textures[t_].used;
    if (indices && hasTexture) {
        mdk_render_update_texture(r_, t_, r->textures[t_].width, r->textures[t_].height,
                                  indices);
    }
    if (!rgba256 || p_ < 0 || p_ >= (int32_t)r->palettes.size() || !r->palettes[p_].used) {
        return;
    }
    PaletteVk& p = r->palettes[p_];
    if (memcmp(p.rgba, rgba256, sizeof(p.rgba)) == 0) {
        return;
    }
    // The cached sets name the old view (update_texture's order).
    forgetDescriptorSets(r, -1, p_);
    VkImage oldImage = p.image;
    VkDeviceMemory oldMemory = p.memory;
    VkImageView oldView = p.view;
    p.image = VK_NULL_HANDLE;
    p.memory = VK_NULL_HANDLE;
    p.view = VK_NULL_HANDLE;
    if (createHostImage(&r->ctx, 256, 1, VK_FORMAT_R8G8B8A8_UNORM, 4, rgba256, &p.image,
                        &p.memory, &p.view, &p.mapped)) {
        memcpy(p.rgba, rgba256, sizeof(p.rgba));
        // The colour expansions keyed on this palette rebuild on the texture's
        // version (getColourView), as a changed index texture's do.
        if (hasTexture) {
            r->textures[t_].version++;
        }
        retireImage(r, oldImage, oldMemory, oldView);
    } else {
        refused("re-made palette %d: keeping the colours it had", p_);
        p.image = oldImage;
        p.memory = oldMemory;
        p.view = oldView;
    }
}

// The 3DS HD pack's compressed replacements: this backend decodes the PC mods
// through mdk_render_replace_colour, and no shared code hands it a .t3x.
void mdk_render_replace_compressed(MdkRenderer, MdkTextureId, const char*) {}
void mdk_render_replace_image_compressed(MdkRenderer, MdkTextureId, const char*) {}

void mdk_render_replace_colour(MdkRenderer r_, MdkTextureId texture, MdkPaletteId palette,
                               int32_t frames, int32_t width, int32_t height,
                               const uint8_t* rgba) {
    registerReplacement(asVk(r_), texture, palette, frames, width, height, rgba, false);
}

void mdk_render_replace_image(MdkRenderer r_, MdkTextureId texture, MdkPaletteId palette,
                              int32_t width, int32_t height, const uint8_t* rgba) {
    registerReplacement(asVk(r_), texture, palette, 1, width, height, rgba, true);
}

// Textures are resident from creation on this backend: nothing to prepare.
void mdk_render_prepare_texture(MdkRenderer, MdkTextureId) {}

void mdk_render_release_texture(MdkRenderer r_, MdkTextureId t_) {
    RendererVk* r = asVk(r_);
    if (!r || t_ < 0 || t_ >= (int32_t)r->textures.size() || !r->textures[t_].used) {
        return;
    }
    // The cached descriptor sets name the image view that is about to be
    // destroyed. A released id may be reused, so those sets must not
    // survive the release.
    forgetDescriptorSets(r, t_, -1);
    // Its replacements and built colour textures name it; a reused id must
    // not inherit them.
    for (size_t i = 0; i < r->replacements.size();) {
        if (r->replacements[i].used && r->replacements[i].tex == t_) {
            dropColour(r, t_, r->replacements[i].pal, r->replacements[i].frames);
            r->replacements.erase(r->replacements.begin() + (int)i);
        } else {
            i++;
        }
    }
    if (r->textures[t_].arrayView) {
        vkDestroyImageView(r->ctx.device, r->textures[t_].arrayView, nullptr);
        r->textures[t_].arrayView = VK_NULL_HANDLE;
    }
    // Retire the resource beside the sets that name it. Nulling the fields is
    // what makes the retire exclusive: leaving them set would have teardown
    // destroy the same image a second time.
    retireImage(r, r->textures[t_].image, r->textures[t_].memory, r->textures[t_].view);
    r->textures[t_].image = VK_NULL_HANDLE;
    r->textures[t_].memory = VK_NULL_HANDLE;
    r->textures[t_].view = VK_NULL_HANDLE;
    r->textures[t_].used = false;
    // The index copy goes with the texture (swap, not clear: clear keeps the
    // capacity). A released slot used to keep its pixels until the slot was reused,
    // so a finished level's textures stayed on the heap into the next level
    // (package 2's teardown measured it; the 3DS backend already swaps).
    std::vector<uint8_t>().swap(r->textures[t_].indices);
}

void mdk_render_release_palette(MdkRenderer r_, MdkPaletteId p_) {
    RendererVk* r = asVk(r_);
    if (!r || p_ < 0 || p_ >= (int32_t)r->palettes.size() || !r->palettes[p_].used) {
        return;
    }
    forgetDescriptorSets(r, -1, p_);
    // Its replacements and built colour textures name it, as for a texture.
    for (size_t i = 0; i < r->replacements.size();) {
        if (r->replacements[i].used && r->replacements[i].pal == p_) {
            dropColour(r, r->replacements[i].tex, p_, r->replacements[i].frames);
            r->replacements.erase(r->replacements.begin() + (int)i);
        } else {
            i++;
        }
    }
    retireImage(r, r->palettes[p_].image, r->palettes[p_].memory, r->palettes[p_].view);
    r->palettes[p_].image = VK_NULL_HANDLE;
    r->palettes[p_].memory = VK_NULL_HANDLE;
    r->palettes[p_].view = VK_NULL_HANDLE;
    r->palettes[p_].used = false;
}

MdkMeshId mdk_render_create_mesh(MdkRenderer r_, const mdk::Vertex* v, int32_t n) {
    RendererVk* r = asVk(r_);
    if (!r || !r->ready || !v || n <= 0) {
        return -1;
    }
    const int32_t id = acquireSlot(r->meshes);
    MeshVk& m = r->meshes[id];
    const VkDeviceSize size = sizeof(mdk::Vertex) * (VkDeviceSize)n;
    if (!createHostBuffer(&r->ctx, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &m.buffer[0],
                          &m.memory[0], &m.mapped[0])) {
        refused("static mesh of %d corners (%llu bytes): the caller gets no mesh id and "
                "draws nothing", n, (unsigned long long)size);
        m.used = false;
        return -1;
    }
    memcpy(m.mapped[0], v, (size_t)size);
    m.capacity = size;
    ResRef ref;
    ref.kind = 2;
    ref.id = id;
    r->creationLog.push_back(ref);
    return id;
}

MdkMeshId mdk_render_create_dynamic_mesh(MdkRenderer r_, int32_t cap) {
    RendererVk* r = asVk(r_);
    if (!r || !r->ready || cap <= 0) {
        return -1;
    }
    const int32_t id = acquireSlot(r->meshes);
    MeshVk& m = r->meshes[id];
    const VkDeviceSize size = sizeof(mdk::Vertex) * (VkDeviceSize)cap;
    // ONE BUFFER PER FRAME IN FLIGHT. The frame writes its whole stream into
    // the slot it will draw from; the previous frame's submission may still be
    // reading the other one (MeshVk, AGENTS.md P3). Two slots is the whole
    // cost: the object stream is a few hundred kilobytes at its largest
    // measured frame, and the alternative -- waiting for the previous frame
    // before writing -- is a stall in the frame's critical path.
    for (uint32_t slot = 0; slot < mdk::vk::kFramesInFlight; slot++) {
        if (!createHostBuffer(&r->ctx, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &m.buffer[slot],
                              &m.memory[slot], &m.mapped[slot])) {
            for (uint32_t done = 0; done < slot; done++) {
                destroyBufferPair(r->ctx.device, m.buffer[done], m.memory[done]);
                m.buffer[done] = VK_NULL_HANDLE;
                m.memory[done] = VK_NULL_HANDLE;
                m.mapped[done] = nullptr;
            }
            refused("dynamic mesh of %d corners x %u frames in flight (%llu bytes total, "
                    "%llu per slot): the object pass has nowhere to upload and draws no "
                    "objects", cap, (unsigned)mdk::vk::kFramesInFlight,
                    (unsigned long long)(size * mdk::vk::kFramesInFlight),
                    (unsigned long long)size);
            m.used = false;
            return -1;
        }
    }
    m.dynamic = true;
    m.capacity = size;
    ResRef ref;
    ref.kind = 2;
    ref.id = id;
    r->creationLog.push_back(ref);
    return id;
}

void mdk_render_update_mesh(MdkRenderer r_, MdkMeshId m_, const mdk::Vertex* v, int32_t n) {
    RendererVk* r = asVk(r_);
    if (!r || m_ < 0 || m_ >= (int32_t)r->meshes.size() || !r->meshes[m_].used || !v || n <= 0) {
        return;
    }
    MeshVk& m = r->meshes[m_];
    const VkDeviceSize size = sizeof(mdk::Vertex) * (VkDeviceSize)n;
    if (size > m.capacity) {
        refused("update of mesh %d with %d corners (%llu bytes) against a capacity of %llu "
                "bytes: the mesh keeps the vertices it already had", m_, n,
                (unsigned long long)size, (unsigned long long)m.capacity);
        return;
    }
    // The slot this frame draws from, which is the one whose fence has been
    // waited. A static mesh is slot 0 and is not rewritten after it is made.
    //
    // BETWEEN FRAMES THERE IS NO SUCH SLOT. Outside a recorded frame the fence
    // that was waited belongs to the slot the NEXT frame will use, and the frame
    // before that one may still be running, so the only safe thing is to wait for
    // the device. The seam's contract is that a dynamic mesh is updated while the
    // frame it belongs to is being recorded (render.h), and every caller does that
    // -- this is a guard, not a path.
    if (!r->inFrame) {
        if (vkDeviceWaitIdle(r->ctx.device) != VK_SUCCESS) {
            // The whole point of the wait is that the previous frame may still be
            // reading this slot. A wait that failed does not establish that, and
            // memcpy-ing under a running frame is the CPU reuse P3 puts the
            // completion boundary before.
            refused("update of mesh %d outside a recorded frame could not wait for the "
                    "device: the update is skipped rather than written under a frame "
                    "that may still be reading this slot", m_);
            return;
        }
    }
    const int slot = meshSlot(m, &r->ctx);
    void* mapped = m.mapped[slot];
    if (!mapped) {
        refused("update of mesh %d found no mapped slot at slot %d: the frame draws the "
                "mesh's previous contents", m_, slot);
        return;
    }
    memcpy(mapped, v, (size_t)size);
}

void mdk_render_release_mesh(MdkRenderer r_, MdkMeshId m_) {
    RendererVk* r = asVk(r_);
    if (!r || m_ < 0 || m_ >= (int32_t)r->meshes.size() || !r->meshes[m_].used) {
        return;
    }
    MeshVk& m = r->meshes[m_];
    // RETIRED, NOT DESTROYED. The mesh may have been drawn by a frame that is
    // still on the GPU; destroying its buffer there is the use-after-free the
    // loader would be within its rights to make visible.
    for (uint32_t slot = 0; slot < mdk::vk::kFramesInFlight; slot++) {
        retireBuffer(r, m.buffer[slot], m.memory[slot]);
        m.buffer[slot] = VK_NULL_HANDLE;
        m.memory[slot] = VK_NULL_HANDLE;
        m.mapped[slot] = nullptr;
    }
    m.used = false;
}

MdkScope mdk_render_mark(MdkRenderer r_) {
    RendererVk* r = asVk(r_);
    MdkScope scope;
    scope.textures = r ? (int32_t)r->creationLog.size() : 0;
    scope.buffers = 0;
    return scope;
}

void mdk_render_release(MdkRenderer r_, MdkScope s) {
    RendererVk* r = asVk(r_);
    if (!r) {
        return;
    }
    while ((int32_t)r->creationLog.size() > s.textures) {
        const ResRef ref = r->creationLog.back();
        r->creationLog.pop_back();
        if (ref.kind == 0) {
            mdk_render_release_texture(r_, ref.id);
        } else if (ref.kind == 1) {
            mdk_render_release_palette(r_, ref.id);
        } else {
            mdk_render_release_mesh(r_, ref.id);
        }
    }
}

