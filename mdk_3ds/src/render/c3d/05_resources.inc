// 05_resources.inc -- the resource seam: textures, palettes, meshes, scopes.
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

// Records a resource the seam made, for mdk_render_release (RendererC3D::creationLog).
static void logCreation(RendererC3D* renderer, uint8_t kind, int32_t id) {
    RendererC3D::Created created;
    created.kind = kind;
    created.id = id;
    renderer->creationLog.push_back(created);
}

MdkTextureId mdk_render_create_index_texture(MdkRenderer r, int32_t w, int32_t h, const uint8_t* px,
                                              const uint8_t* paletteRgba) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !renderer->ready || w <= 0 || h <= 0 || w > 1024 || h > 1024 || !px) {
        return -1;
    }
#ifdef MDK_HW_PROFILE
    // WHO MADE IT (profile builds only): a large index texture's creator, by
    // return address -- `addr2line -f -e mdk_3ds/mdk.elf <caller>` names the
    // function. The menu's transition frames create pictures whose owners the
    // texchunk lines alone cannot name.
    if ((size_t)w * (size_t)h >= kChunkMinTexels) {
        mdk::log3dsPrintf("texcreate %dx%d frame %d caller %p", (int)w, (int)h,
                          (int)renderer->frame, __builtin_return_address(0));
    }
#endif
    if (!paletteRgba) {
        // Without a palette there is nothing to expand through. The Vulkan
        // backend binds the palette as a texture of its own and does not
        // need it here, but the 3DS does.
        return -1;
    }

    // THE EXPANSION. The palette lookup happens on the CPU because the
    // PICA200 has no palette texture format and no programmable fragment
    // shader (SPEC.md 7.3).
    //
    // The rule is the C#'s palette shader's rule exactly: index 0 is
    // transparent, and the palette's alpha is NOT used -- the shader takes
    // the palette's RGB and writes an alpha of 1.
    // THE DIMENSIONS GO UP TO A POWER OF TWO, and to at least 8.
    //
    // The PICA200's texture size register encodes log2(width) and
    // log2(height), so a texture's dimensions MUST be powers of two. A
    // 128x176 texture is not one the hardware can hold, and the allocation
    // fails with NO DIAGNOSTIC AT ALL -- which is how this was found: a
    // whole arena's textures silently refused to allocate, one after
    // another, and the screen showed only the clear colour.
    //
    // MDK's textures are 256x400, 128x176, 256x320 and so on, so EVERY one
    // of them has to be padded. The cost is VRAM: 256x400 becomes 256x512,
    // a third more. The price of the hardware's rule.
    C3dTexture texture;
    texture.realWidth = w;
    texture.realHeight = h;
    texture.paddedWidth = nextPowerOfTwo(w);
    texture.paddedHeight = nextPowerOfTwo(h);
    texture.indexed = true;
    // THE SOURCE IS KEPT. An evicted texture is made again from this, and
    // this is the only copy: the caller holds an id, not the pixels.
    texture.source.assign(px, px + (size_t)w * (size_t)h);
    texture.hasHoles = mdk::pica::textureSourceHasHoles(texture.source.data(),
                                                        texture.source.size(), true);
    texture.palette.assign(paletteRgba, paletteRgba + 256 * 4);
    texture.lastUsed = ++renderer->drawCounter;
    if (renderDebugFlags() & kRenderDebugTexLog) {
        // rdebug `texlog`: what the expansion will be given, for the first 24.
        static int32_t logged = 0;
        if (logged < 24) {
            logged++;
            int32_t colourful = 0, opaque = 0;
            for (int32_t e = 0; e < 256; e++) {
                const uint8_t* c = paletteRgba + e * 4;
                if (c[0] | c[1] | c[2]) colourful++;
                if (c[3]) opaque++;
            }
            size_t nonzero = 0;
            const size_t n = (size_t)w * (size_t)h;
            for (size_t i = 0; i < n; i++) {
                if (px[i]) nonzero++;
            }
            mdk::log3dsPrintf("texlog: index %ldx%ld palette=%p entries non-black=%ld alpha>0=%ld "
                              "entry1=(%u,%u,%u,%u) texels nonzero=%lu/%lu",
                              (long)w, (long)h, (const void*)paletteRgba, (long)colourful, (long)opaque,
                              paletteRgba[4], paletteRgba[5], paletteRgba[6], paletteRgba[7],
                              (unsigned long)nonzero, (unsigned long)n);
        }
    }

    const int32_t id = acquireTextureSlot(renderer);
    C3dTexture& slot = renderer->textures[(size_t)id];
    slot = std::move(texture);
    // A large indexed texture expands in slices across frames instead of
    // in one make (04_textures.inc, THE CHUNKED EXPANSION); the id is good
    // now, the GPU copy lands at a frame boundary or on first draw.
    if ((size_t)w * (size_t)h >= kChunkMinTexels) {
        const int32_t started = startExpansion(renderer, slot, false);
        if (started > 0) {
            logCreation(renderer, 0, id);
            return id;
        }
        if (started < 0) {
            // Deferred by the pending cap: the frame pump starts it when a
            // slot frees, or the first draw finishes it.
            slot.expandWanted = true;
            logCreation(renderer, 0, id);
            return id;
        }
    }
    if (!makeTexture(renderer, slot, true)) {
        // The failed slot goes back to being free: no GPU copy, no source.
        dropExpansion(slot);
        slot = C3dTexture();
        return -1;
    }
    logCreation(renderer, 0, id);
    return id;
}

void mdk_render_prepare_texture(MdkRenderer r, MdkTextureId t) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !renderer->ready || t < 0 || (size_t)t >= renderer->textures.size()) {
        return;
    }
    C3dTexture& texture = renderer->textures[(size_t)t];
    // A PACKED ORIGINAL (packPath) with no GPU copy -- not made yet, or evicted --
    // has its file asked for: the boundary pump reads and imports it before a
    // draw needs it. This is how an evicted one comes back (the pump's surface
    // scan prepares the resident views' textures).
    if (!texture.packPath.empty()) {
        if (!texture.valid && !texture.releasePending) {
            queuePackRead(renderer, t);
        }
        return;
    }
    // An evicted large indexed texture is re-expanded in slices from here
    // (the frame pump advances it); anything else is either resident,
    // already in flight, or cheap enough to make at its draw as before.
    // (Eviction is exactly this state: valid=false, source kept -- stale
    // and restamped are the canvas/bullet-hole remakes, settled elsewhere.)
    if (texture.valid || texture.expandPending || texture.expandPixels || texture.expandWanted ||
        texture.source.empty() || !texture.indexed || texture.compressed || texture.streamed ||
        texture.stale || texture.restamped || texture.releasePending ||
        texture.realWidth <= 0 || texture.realHeight <= 0) {
        return;
    }
    if ((size_t)texture.realWidth * (size_t)texture.realHeight < kChunkMinTexels) {
        return;
    }
    if (startExpansion(renderer, texture, false) < 0) {
        texture.expandWanted = true;
    }
}

MdkTextureId mdk_render_create_rgba_texture(MdkRenderer r, int32_t w, int32_t h, const uint8_t* px) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !renderer->ready || w <= 0 || h <= 0 || w > 1024 || h > 1024 || !px) {
        return -1;
    }
    C3dTexture texture;
    texture.realWidth = w;
    texture.realHeight = h;
    // The PICA's texture-size register wants powers of two, and at least 8,
    // for RGBA textures as well as index textures. The real rectangle is
    // mapped into the padded one by the draw path's UV transform.
    texture.paddedWidth = nextPowerOfTwo(w);
    texture.paddedHeight = nextPowerOfTwo(h);
    texture.indexed = false;
    // The source is kept, so an evicted texture can be made again.
    texture.source.assign(px, px + (size_t)w * (size_t)h * 4);
    texture.hasHoles = mdk::pica::textureSourceHasHoles(texture.source.data(),
                                                        texture.source.size(), false);
    texture.lastUsed = ++renderer->drawCounter;

    const int32_t id = acquireTextureSlot(renderer);
    C3dTexture& slot = renderer->textures[(size_t)id];
    slot = std::move(texture);
    // The linear heap, not the texture VRAM -- see the index-texture
    // creation for why.
    if (!makeTexture(renderer, slot, true)) {
        slot = C3dTexture();
        return -1;
    }
    logCreation(renderer, 0, id);
    return id;
}

int32_t mdk_render_index_texture_bytes(MdkRenderer r, int32_t width, int32_t height) {
    if (width <= 0 || height <= 0 || width > 1024 || height > 1024) {
        return 0;
    }
    // WHAT THE 3DS SPENDS ON AN INDEX TEXTURE. The indices are expanded
    // through the palette into RGBA8, and the dimensions are padded up to
    // powers of two, so a 95x168 frame costs 128x256x4 = 131072 bytes --
    // eight times the 15960 bytes of the indices themselves. AND ITS MIP CHAIN
    // when Enhanced Mode enableMipmaps is on (makeTexture builds one then): a
    // third more. Kurt's sprite residency budgets against this number, so it is
    // the same pica::textureBytes the allocation is costed with. AND ITS STORE
    // under texture16: an index texture's alpha is 0 or 255, so it is made
    // RGB565 or RGBA5551 -- two bytes a texel, half the RGBA8 figure -- above
    // the 8x8 floor (pica::indexTexelFormatBound). A canvas texture is kept
    // RGBA8 (keepRgba8), but the canvas is not what this number budgets.
    RendererC3D* renderer = asC3d(r);
    const int32_t paddedW = nextPowerOfTwo(width);
    const int32_t paddedH = nextPowerOfTwo(height);
    const int32_t levels = (renderer && renderer->toggles.enableMipmaps)
                               ? mdk::pica::mipLevelsFor(paddedW, paddedH)
                               : 0;
    const mdk::pica::TexelFormat format = mdk::pica::indexTexelFormatBound(
        renderer ? renderer->toggles.texture16 : false, paddedW, paddedH);
    return (int32_t)mdk::pica::textureBytes(format, paddedW, paddedH, levels);
}

// A COMPRESSED texture from a .t3x's bytes (render.h; EnhancedToggles::
// kurtCompressed): imported now -- a decompress and a copy, no expansion -- and
// kept as bytes so an eviction is undone by importing again (makeTexture). The
// image is width x height at the texture's top-left (kurt_etc1_convert.py pads,
// fitting a frame past 256x256 into 256x256 first), drawn through pica::compressedFrameRect.
MdkTextureId mdk_render_create_compressed_texture(MdkRenderer r, int32_t w, int32_t h,
                                                  const uint8_t* t3x, int32_t size) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !renderer->ready || w <= 0 || h <= 0 || w > 1024 || h > 1024 || !t3x ||
        size <= 0) {
        return -1;
    }
    C3dTexture texture;
    texture.realWidth = w;
    texture.realHeight = h;
    texture.compressed = true;
    texture.indexed = false;
    texture.source.assign(t3x, t3x + size);
    // A sprite frame: the shadow pass never sees it (Pass::DoubleSided Sprite
    // shading, pica::castsSunShadow), so "holes" is only the honest answer.
    texture.hasHoles = true;
    texture.lastUsed = ++renderer->drawCounter;
    const int32_t id = acquireTextureSlot(renderer);
    C3dTexture& slot = renderer->textures[(size_t)id];
    slot = std::move(texture);
    if (!makeTexture(renderer, slot, false)) {
        slot = C3dTexture();
        return -1;
    }
    // A texture whose image is bigger than the file's texture is not this set's:
    // refused rather than drawn through a rectangle past the texture's edge.
    if (slot.realWidth > slot.paddedWidth || slot.realHeight > slot.paddedHeight) {
        mdk::log3dsPrintf("texture %dx%d: compressed file is %dx%d, refused", (int)w, (int)h,
                          (int)slot.paddedWidth, (int)slot.paddedHeight);
        C3D_TexDelete(&slot.tex);
        slot = C3dTexture();
        return -1;
    }
    logCreation(renderer, 0, id);
    return id;
}

// Read a whole file into a vector, on the calling thread: ONLY the packed
// original's last resort (04_textures.inc finishPackNow -- a draw in the eye's
// frustum whose file the reader thread has not delivered). Every other read of
// the orig pack is the reader thread's. False when it cannot be opened or read;
// an empty file is not an error here, the import rejects it.
static bool readWholeFile(const char* path, std::vector<uint8_t>* out) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    const long size = ftell(file);
    // A bound on ftell, not a pack limit: the largest .t3x is 699008 bytes, so
    // anything past 4 MiB is a damaged entry, and a huge resize under
    // -fno-exceptions aborts instead of falling back to the expansion.
    if (size < 0 || (unsigned long)size > 4u * 1024u * 1024u) {
        fclose(file);
        return false;
    }
    rewind(file);
    out->resize((size_t)size);
    const size_t got = out->empty() ? 0 : fread(out->data(), 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        out->clear();
        return false;
    }
    return true;
}

// A PACKED ORIGINAL from its FILE (render.h): created with NO I/O and no
// citro3d call -- safe mid-frame, where LevelView::draw asks for it -- as a
// pending id (C3dTexture::packPath, PackQueued): the boundary pump has the
// reader thread read the file and imports it (04_textures.inc
// pumpPackOriginals), and a draw that cannot wait reads it itself, after the
// eye's frustum cull (finishPackNow). The size is the image's; the import
// sets the texture's. `paletteRgba` is the stamp the index fallback expands
// through (mdk_render_update_texture's rule for a stamped compressed texture):
// the bytes are the palette the texture stands in for, so the fallback is
// bit-exact. -1 only when the renderer is not ready or the path is empty or
// longer than the reader's slots hold (the same rule as recordHdPath's).
MdkTextureId mdk_render_create_compressed_file(MdkRenderer r, int32_t w, int32_t h,
                                               const char* path, const uint8_t* paletteRgba) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !renderer->ready || w <= 0 || h <= 0 || w > 1024 || h > 1024 || !path ||
        !path[0] || strlen(path) >= mdk::AsyncFileReader::kPathMax) {
        return -1;
    }
    C3dTexture texture;
    texture.realWidth = w;
    texture.realHeight = h;
    texture.compressed = true;
    texture.indexed = false;
    // As Kurt's compressed frames (mdk_render_create_compressed_texture): the
    // sun's shadow pass never takes a pack-made surface.
    texture.hasHoles = true;
    texture.packPath = path;
    if (paletteRgba) {
        texture.palette.assign(paletteRgba, paletteRgba + 256 * 4);
    }
    const int32_t id = acquireTextureSlot(renderer);
    renderer->textures[(size_t)id] = std::move(texture);
    queuePackRead(renderer, id);
    logCreation(renderer, 0, id);
    return id;
}

// Whether a packed original could not be made from its file (render.h): the
// core then hands its index pixels over (mdk_render_update_texture), which
// turns it into an ordinary index texture.
bool mdk_render_texture_needs_pixels(MdkRenderer r, MdkTextureId t) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || t < 0 || (size_t)t >= renderer->textures.size()) {
        return false;
    }
    const C3dTexture& texture = renderer->textures[(size_t)t];
    return !texture.releasePending && !texture.packPath.empty() &&
           texture.packState == C3dTexture::PackFailed;
}

int32_t mdk_render_texture_bytes(MdkRenderer r, MdkTextureId t) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || t < 0 || (size_t)t >= renderer->textures.size()) {
        return 0;
    }
    const C3dTexture& texture = renderer->textures[(size_t)t];
    if (!texture.valid || texture.releasePending) {
        return 0;
    }
    if (texture.compressed) {
        return (int32_t)texture.gpuBytes;
    }
    return (int32_t)mdk::pica::textureBytes(texture.format, texture.paddedWidth,
                                            texture.paddedHeight, texture.mipLevels);
}

MdkPaletteId mdk_render_create_palette(MdkRenderer r, const uint8_t* rgba) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !renderer->ready || !rgba) {
        return -1;
    }
    // The 3DS keeps the palette only to expand the textures through it. A released
    // palette's slot (empty: a live one always holds 256 entries) is taken again, so
    // a level-to-level run does not grow the table.
    MdkPaletteId id = -1;
    for (size_t i = 0; i < renderer->palettes.size(); i++) {
        if (renderer->palettes[i].empty()) {
            id = (MdkPaletteId)i;
            break;
        }
    }
    if (id < 0) {
        renderer->palettes.push_back(std::vector<uint8_t>());
        id = (MdkPaletteId)(renderer->palettes.size() - 1);
    }
    renderer->palettes[(size_t)id].assign(rgba, rgba + 256 * 4);
    logCreation(renderer, 1, id);
    return id;
}

void mdk_render_update_texture(MdkRenderer r, MdkTextureId t, int32_t w, int32_t h, const uint8_t* px) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || t < 0 || (size_t)t >= renderer->textures.size() || !px) {
        return;
    }
    C3dTexture& texture = renderer->textures[(size_t)t];
    // A packed original has no source -- its file is -- and is updated too.
    if ((texture.source.empty() && texture.packPath.empty()) || texture.releasePending) {
        return;
    }
    if (w != texture.realWidth || h != texture.realHeight) {
        return;  // the contract is same-size updates only
    }
    // A COMPRESSED texture's source is a .t3x's bytes, not indices to replace,
    // and the caller hands the format's INDEX bytes (SurfaceCache::refresh):
    // the ordinary path below would size them as a store image (w*h*4 for a
    // non-indexed texture), four times the caller's buffer, and write garbage
    // over the source. The safe recreation is the INDEX one: the update's
    // pixels ARE the texture's indices, so take them, clear the compressed
    // flag, and let the restamp below rebuild the GPU copy through the palette
    // -- the texture falls back to the index path for that key for good,
    // exactly as an HD/orig-backed texture gives up its copy below. Without a
    // palette there is nothing to expand through (a .t3x made by
    // mdk_render_create_compressed_texture never carries one): the stamp is
    // dropped, as it was when this guard refused every compressed texture --
    // a stale copy beats an invisible surface.
    //
    // A PACKED ORIGINAL converts the same way -- a bullet hole, or the core's
    // answer to mdk_render_texture_needs_pixels (a file that could not be made)
    // -- and stops being one: its read is dropped (a late one must not land on
    // the index texture) and its path cleared BEFORE the index texture's first
    // prepare, which may start a chunked expansion.
    if (texture.compressed) {
        if (texture.palette.size() < 256 * 4) {
            return;
        }
        if (!texture.packPath.empty()) {
            renderer->hdReader.discard(mdk::AsyncFileReader::kChannelOrig, t);
            std::string().swap(texture.packPath);
            texture.packState = C3dTexture::PackIdle;
        }
        texture.compressed = false;
        texture.indexed = true;
        texture.paddedWidth = nextPowerOfTwo(texture.realWidth);
        texture.paddedHeight = nextPowerOfTwo(texture.realHeight);
        texture.mipLevels = 0;  // the make below sets the chain it actually builds
    }
    // A bullet hole's texture changes (scripts/bullet_holes.h). The 3DS's texture
    // is EXPANDED, so the update has to go through the palette again -- and the
    // palette the texture was expanded through is kept with the texture, so it can.
    //
    // THE SOURCE IS TAKEN NOW: it is the system-memory copy an eviction is undone
    // from, so a texture evicted before its turn below comes back WITH the hole.
    const size_t bytes = (size_t)w * (size_t)h * (texture.indexed ? 1u : 4u);
    texture.source.assign(px, px + bytes);
    // A chunked expansion in flight is built from the OLD source: drop it,
    // and the next make expands the new source instead.
    dropExpansion(texture);
    texture.hasHoles = mdk::pica::textureSourceHasHoles(texture.source.data(),
                                                        texture.source.size(), texture.indexed);

    // THE PACK COPY GOES (a world texture's, from either pack; a canvas picture
    // keeps its own): the pack's image is the texture WITHOUT the hole, and the
    // PC drops a changed texture's replacement the same way (the C#'s
    // Renderer.Changed: "an HD texture no longer shows the texture: expanded
    // again from now on"). Unlinked only: the copies table owns it and its
    // budget frees it (mdk_render_release_texture's rule). THE FALLBACK IS
    // PERMANENT for this key: the restamp below rebuilds the index expansion
    // (bit-exact -- the same expandIndices through the kept palette as a
    // texture that never had a copy), nothing re-links the copy, and a later
    // re-look cannot either -- the stamped texture's hdKeyOf has changed, so
    // neither pack holds its key any more.
    if (!texture.hdCanvas && texture.hdCopy >= 0 &&
        (size_t)texture.hdCopy < renderer->hdCopies.size()) {
        HdCopy& copy = renderer->hdCopies[(size_t)texture.hdCopy];
        if (--copy.links == 0) {
            copy.unlinkedSince = renderer->frame;
        }
        texture.hdCopy = -1;
    }

    if (!texture.valid) {
        return;  // the next draw makes it from the new source
    }
    // NEVER MADE HERE. The expansion is the expensive half (a whole texture through
    // the palette, Morton-tiled, its mip chain), and two rounds can stamp in one
    // step: the frame boundary makes ONE restamped texture a frame, after
    // C3D_FrameBegin's wait, so the old copy is not freed under the GPU either
    // (AGENTS.md P3). The old copy draws until then.
    texture.restamped = true;
}

// A STREAMED picture (render.h; package #20c): the indices and/or the palette
// taken now into the texture's system-memory source (so an eviction comes back
// with the newest picture), the GPU copy rewritten at the next frame boundary
// (settleDeferredTextures, one streamed texture a boundary). The first update
// turns the texture into a streamed one (C3dTexture::streamed). The palette id's
// bytes follow too, for any texture made from it later.
void mdk_render_update_paletted(MdkRenderer r, MdkTextureId t, MdkPaletteId p,
                                const uint8_t* indices, const uint8_t* rgba256) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer) {
        return;
    }
    if (rgba256 && p >= 0 && (size_t)p < renderer->palettes.size() &&
        !renderer->palettes[(size_t)p].empty()) {
        memcpy(renderer->palettes[(size_t)p].data(), rgba256, 256 * 4);
    }
    if (t < 0 || (size_t)t >= renderer->textures.size()) {
        return;
    }
    C3dTexture& texture = renderer->textures[(size_t)t];
    if (!texture.indexed || texture.source.empty() || texture.releasePending ||
        texture.compressed || (!indices && !rgba256)) {
        return;
    }
    if (indices) {
        memcpy(texture.source.data(), indices, texture.source.size());
    }
    if (rgba256) {
        texture.palette.assign(rgba256, rgba256 + 256 * 4);
    }
    if (!texture.streamed) {
        texture.streamed = true;
        texture.restamped = false;
        // A STREAMED texture is made by makeStreamed (RGB565, no mips), never
        // through the chunked expansion -- but create_index_texture has already
        // started one for a picture this large (movie_view.cpp's makeImage
        // creates the 600x360 video frame and turns it streamed in the next
        // call). Left pending, the first draw finished it synchronously and
        // threw it away: measured on the Old 3DS, the menu's 554.8 ms frame 1
        // and 392.5 ms reopen (texchunk 600x360 start -> ready one frame later).
        // Dropped here, with its RGBA8 chain buffer.
        dropExpansion(texture);
        // The HD copy is of the first picture only (mdk_render_update_texture's rule).
        if (texture.hdCopy >= 0 && (size_t)texture.hdCopy < renderer->hdCopies.size()) {
            HdCopy& copy = renderer->hdCopies[(size_t)texture.hdCopy];
            if (--copy.links == 0) {
                copy.unlinkedSince = renderer->frame;
            }
            texture.hdCopy = -1;
        }
        texture.hdCanvas = false;
    }
    texture.streamDirty = true;
}

// Links a texture to the HD copy of `path` (RendererC3D::hdCopies, one per
// file). Recorded only: the draw that first wants it queues it, and the
// frame-boundary pump imports it under the HD budget (04_textures.inc, THE HD
// STREAM).
static void recordHdPath(MdkRenderer r, MdkTextureId t, const char* path, bool canvas) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || t < 0 || (size_t)t >= renderer->textures.size() || !path || !*path) {
        return;
    }
    if (strlen(path) >= mdk::AsyncFileReader::kPathMax) {
        // The reader's slots hold kPathMax bytes; a longer path could never be
        // read, and left queued it would stop every read behind it (review F2).
        mdk::log3dsPrintf("hd: path too long, ignored: %s", path);
        return;
    }
    // The copy for this FILE, made on first sight and shared from then on.
    int32_t index = -1;
    const std::map<std::string, int32_t>::const_iterator found = renderer->hdByPath.find(path);
    if (found != renderer->hdByPath.end()) {
        index = found->second;
    } else {
        index = (int32_t)renderer->hdCopies.size();
        HdCopy copy;
        copy.path = path;
        renderer->hdCopies.push_back(copy);
        renderer->hdByPath[path] = index;
    }
    C3dTexture& texture = renderer->textures[(size_t)t];
    if (texture.hdCopy != index) {
        if (texture.hdCopy >= 0 && (size_t)texture.hdCopy < renderer->hdCopies.size()) {
            HdCopy& old = renderer->hdCopies[(size_t)texture.hdCopy];
            if (--old.links == 0) {
                old.unlinkedSince = renderer->frame;
            }
        }
        renderer->hdCopies[(size_t)index].links++;
    }
    texture.hdCopy = index;
    texture.hdCanvas = canvas;
    // A fresh link is a fresh chance for a copy that failed for a passing reason
    // (a linear-heap shortage): a re-made Kurt frame must not stay on its
    // original for the session (review F3). A broken file is not retried.
    HdCopy& copy = renderer->hdCopies[(size_t)index];
    if (copy.state == HdCopy::Failed && !copy.broken) {
        copy.state = HdCopy::None;
        copy.failures = 0;
    }
}

// KURT'S PACK STREAM through the HD reader's thread (render.h): its own channel,
// one read in flight, so the HD stream keeps the other slots and never sees a
// pack read. Only the main thread calls these (the reader's slots are locked).
bool mdk_render_read_range_async(MdkRenderer r, int32_t tag, const char* path, uint32_t offset,
                                 uint32_t size) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !path || size == 0 ||
        renderer->hdReader.busy(mdk::AsyncFileReader::kChannelPack) > 0) {
        return false;
    }
    return renderer->hdReader.requestRange(mdk::AsyncFileReader::kChannelPack, tag, path,
                                           offset, size);
}

bool mdk_render_poll_range_read(MdkRenderer r, int32_t* tag, uint8_t** bytes, uint32_t* size,
                                bool* ok) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !tag || !bytes || !size || !ok) {
        return false;
    }
    char path[mdk::AsyncFileReader::kPathMax];
    size_t length = 0;
    if (!renderer->hdReader.poll(mdk::AsyncFileReader::kChannelPack, tag, path, bytes, &length,
                                 ok)) {
        return false;
    }
    *size = (uint32_t)length;
    if (!*ok) {
        free(*bytes);
        *bytes = nullptr;
        *size = 0;
    }
    return true;
}

// THE MENUS' WHOLE-FILE READS through the same thread (render.h): kChannelFile,
// taken by tag. Only the main thread calls these (the reader's slots are locked).
bool mdk_render_has_file_reader(MdkRenderer r) {
    RendererC3D* renderer = asC3d(r);
    return renderer != nullptr && renderer->hdReader.running();
}

bool mdk_render_read_file_async(MdkRenderer r, int32_t tag, const char* path, MdkFileWork work) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !path || !*path) {
        return false;
    }
    return renderer->hdReader.requestFile(mdk::AsyncFileReader::kChannelFile, tag, path, work);
}

bool mdk_render_poll_file_read(MdkRenderer r, int32_t tag, uint8_t** bytes, uint32_t* size,
                               bool* ok) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !bytes || !size || !ok) {
        return false;
    }
    size_t length = 0;
    if (!renderer->hdReader.pollTag(mdk::AsyncFileReader::kChannelFile, tag, bytes, &length,
                                    ok)) {
        return false;
    }
    *size = (uint32_t)length;
    if (!*ok) {
        free(*bytes);
        *bytes = nullptr;
        *size = 0;
    }
    return true;
}

void mdk_render_cancel_file_read(MdkRenderer r, int32_t tag) {
    RendererC3D* renderer = asC3d(r);
    if (renderer) {
        renderer->hdReader.discard(mdk::AsyncFileReader::kChannelFile, tag);
    }
}

// THE MUSIC PREFETCH'S RANGE READS through the same thread (render.h):
// kChannelMusic, one in flight (a cancelled read still being read counts until
// the worker ends it), taken by tag. Only the main thread calls these.
bool mdk_render_read_music_async(MdkRenderer r, int32_t tag, const char* path, uint32_t offset,
                                 uint32_t size) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !path || !*path || size == 0 ||
        renderer->hdReader.busy(mdk::AsyncFileReader::kChannelMusic) > 0) {
        return false;
    }
    return renderer->hdReader.requestRange(mdk::AsyncFileReader::kChannelMusic, tag, path,
                                           offset, size);
}

bool mdk_render_poll_music_read(MdkRenderer r, int32_t tag, uint8_t** bytes, uint32_t* size,
                                bool* ok) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !bytes || !size || !ok) {
        return false;
    }
    size_t length = 0;
    if (!renderer->hdReader.pollTag(mdk::AsyncFileReader::kChannelMusic, tag, bytes, &length,
                                    ok)) {
        return false;
    }
    *size = (uint32_t)length;
    if (!*ok) {
        free(*bytes);
        *bytes = nullptr;
        *size = 0;
    }
    return true;
}

void mdk_render_cancel_music_read(MdkRenderer r, int32_t tag) {
    RendererC3D* renderer = asC3d(r);
    if (renderer) {
        renderer->hdReader.discard(mdk::AsyncFileReader::kChannelMusic, tag);
    }
}

// The HD pack's compressed replacement for an index texture: recorded only.
// The draw that first wants it queues it, and the frame-boundary pump imports
// it under the HD budget (04_textures.inc, THE HD STREAM).
void mdk_render_replace_compressed(MdkRenderer r, MdkTextureId t, const char* path) {
    recordHdPath(r, t, path, false);
}

// The same for a canvas picture (HUD, menus): drawn on the canvas's own
// Original-shaded pass.
// THE CANVAS PIN, SAID UP FRONT (render.h): the first Overlay draw pins a
// texture RGBA8 (06_draw.inc) and re-makes a copy already made 16-bit -- for the
// menu's font atlas and MDKOPT background that was a second whole expansion on
// the splash-to-menu frame (measured on the Old 3DS: front frames 556-557,
// 85-97 ms). Pinned at creation, an expansion still in flight picks the RGBA8
// store at its pack stage, so the texture is expanded once; a copy already made
// 16-bit is re-made exactly as the draw-time pin does.
void mdk_render_mark_canvas(MdkRenderer r, MdkTextureId t) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || t < 0 || (size_t)t >= renderer->textures.size()) {
        return;
    }
    C3dTexture& texture = renderer->textures[(size_t)t];
    if (texture.keepRgba8 || texture.compressed || texture.streamed) {
        return;
    }
    texture.keepRgba8 = true;
    if (texture.valid && texture.format != mdk::pica::TexelFormat::Rgba8) {
        texture.stale = true;
    }
}

void mdk_render_replace_image_compressed(MdkRenderer r, MdkTextureId t, const char* path) {
    recordHdPath(r, t, path, true);
}

// The mod system's DECODED colour-image replacements: the 3DS takes the HD
// pack's compressed .t3x instead (mdk_render_replace_compressed above) and never
// decodes a PNG, so these stay deliberate no-ops.
void mdk_render_replace_colour(MdkRenderer, MdkTextureId, MdkPaletteId, int32_t, int32_t,
                               int32_t, const uint8_t*) {}
void mdk_render_replace_image(MdkRenderer, MdkTextureId, MdkPaletteId, int32_t, int32_t,
                              const uint8_t*) {}

void mdk_render_release_texture(MdkRenderer r, MdkTextureId t) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || t < 0 || (size_t)t >= renderer->textures.size()) {
        return;
    }
    C3dTexture& texture = renderer->textures[(size_t)t];
    const bool inFlight = renderer->gpuPending ||
                          (renderer->inFrame && texture.lastUsed >= renderer->frameStartUse);
    // The HD copy is the copies table's, not this texture's: unlinked only. It
    // stays resident (a re-made texture of the same file relinks to it) until the
    // budget evicts it, and eviction never takes a copy drawn by a frame the GPU
    // may still be reading.
    if (texture.hdCopy >= 0 && (size_t)texture.hdCopy < renderer->hdCopies.size()) {
        HdCopy& copy = renderer->hdCopies[(size_t)texture.hdCopy];
        if (--copy.links == 0) {
            copy.unlinkedSince = renderer->frame;
        }
    }
    texture.hdCopy = -1;
    texture.hdCanvas = false;
    // A PACKED ORIGINAL's read is the reader's, not this texture's: dropped (a
    // finished one freed now, one in progress when it ends), and the path goes,
    // which is what frees the slot (acquireTextureSlot). A queue entry for it is
    // dropped by the pump's next scan.
    if (!texture.packPath.empty()) {
        renderer->hdReader.discard(mdk::AsyncFileReader::kChannelOrig, t);
        std::string().swap(texture.packPath);
        texture.packState = C3dTexture::PackIdle;
    }
    if (texture.valid && !inFlight) {
        C3D_TexDelete(&texture.tex);
        texture.valid = false;
    } else if (texture.valid) {
        // The id is invalid to the caller now, but the GPU may still be
        // reading the old copy. The next frame boundary frees it.
        texture.releasePending = true;
    }
    // A RELEASE is final: the source goes with the GPU copy, so the slot is
    // free again once the deferred deletion settles. (An EVICTION is not a
    // release: it keeps the source, and the texture is made again when it is
    // next drawn.)
    // swap, not clear(): clear keeps the capacity, and an arena-sized release
    // (game/arena_views.h) must hand the heap back for the next arena's stream.
    std::vector<uint8_t>().swap(texture.source);
    std::vector<uint8_t>().swap(texture.palette);
    dropExpansion(texture);
    texture.stale = false;
    texture.restamped = false;
    texture.streamed = false;
    texture.streamDirty = false;
}

void mdk_render_release_palette(MdkRenderer r, MdkPaletteId p) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || p < 0 || (size_t)p >= renderer->palettes.size()) {
        return;
    }
    // swap, not clear: clear keeps the kilobyte (and the slot is reused by the next
    // create_palette, which tells a released slot by its being empty).
    std::vector<uint8_t>().swap(renderer->palettes[(size_t)p]);
}

// A mesh slot: a released one whose linear memory the frame boundary has already
// freed (not valid, nothing pending, no data), else a new one -- so a level-to-level
// run reuses the table instead of growing it.
static MdkMeshId acquireMeshSlot(RendererC3D* renderer) {
    for (size_t i = 0; i < renderer->meshes.size(); i++) {
        const C3dMesh& mesh = renderer->meshes[i];
        if (!mesh.valid && !mesh.releasePending && !mesh.data) {
            return (MdkMeshId)i;
        }
    }
    renderer->meshes.push_back(C3dMesh());
    return (MdkMeshId)(renderer->meshes.size() - 1);
}

MdkMeshId mdk_render_create_mesh(MdkRenderer r, const mdk::Vertex* v, int32_t n) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !renderer->ready || n <= 0 || !v) {
        return -1;
    }
    C3dMesh mesh;
    mesh.capacity = n;
    // The 3DS's vertex: the position and the texture coordinate, five
    // floats. The packed colour is NOT uploaded: the original look's world
    // surfaces are unlit, and the flat-coloured surfaces are drawn with a
    // one-pixel texture of that colour instead.
    mesh.data = linearAlloc((size_t)n * 5 * sizeof(float));
    // The HD copies share the linear heap: unlinked ones (a finished level's) go
    // before a mesh fails to be made, then any copy this frame did not draw.
    while (!mesh.data && (evictHd(renderer, renderer->frameStartUse, true) ||
                          evictHd(renderer, renderer->frameStartUse, false))) {
        mesh.data = linearAlloc((size_t)n * 5 * sizeof(float));
    }
    if (!mesh.data) {
        return -1;
    }
    mesh.valid = true;
    // THE MESH IS PUSHED BEFORE IT IS FILLED.
    //
    // The update takes the mesh's ID and looks the mesh up by it, so a mesh
    // that is not in the table yet cannot be updated: the update finds
    // nothing, the vertex count stays ZERO, and every draw of that mesh is
    // then skipped -- silently, because a draw with no vertices is a draw
    // that is simply not made.
    //
    // The symptom was a screen that showed only the clear colour while the
    // log said the arena had loaded with 154 batches and 1197 vertices.
    const MdkMeshId id = acquireMeshSlot(renderer);
    renderer->meshes[(size_t)id] = mesh;
    logCreation(renderer, 2, id);
    mdk_render_update_mesh(r, id, v, n);
    return id;
}

MdkMeshId mdk_render_create_dynamic_mesh(MdkRenderer r, int32_t cap) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !renderer->ready || cap <= 0) {
        return -1;
    }
    C3dMesh mesh;
    mesh.capacity = cap;
    mesh.data = linearAlloc((size_t)cap * 5 * sizeof(float));
    while (!mesh.data && (evictHd(renderer, renderer->frameStartUse, true) ||
                          evictHd(renderer, renderer->frameStartUse, false))) {
        mesh.data = linearAlloc((size_t)cap * 5 * sizeof(float));
    }
    if (!mesh.data) {
        // Printed, like every other allocation this backend refuses (the scratch
        // buffer, the textures). A mesh that is not there is a draw that is simply
        // not made — no fault, no warning, no picture — and the emulator's bigger
        // heap cannot show it, so the guest log is the only place it can be seen
        // (AGENTS P3/P6, review-10 F3).
        if (!renderer->meshAllocFailureLogged) {
            renderer->meshAllocFailureLogged = true;
            mdk::log3dsPrintf("linearAlloc(%u) FAILED for a dynamic mesh of %d corners",
                              (unsigned)((size_t)cap * 5 * sizeof(float)), (int)cap);
        }
        return -1;
    }
    renderer->meshAllocFailureLogged = false;
    mesh.valid = true;
    const MdkMeshId id = acquireMeshSlot(renderer);
    renderer->meshes[(size_t)id] = mesh;
    logCreation(renderer, 2, id);
    return id;
}

void mdk_render_update_mesh(MdkRenderer r, MdkMeshId m, const mdk::Vertex* v, int32_t n) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || m < 0 || (size_t)m >= renderer->meshes.size() || !v || n <= 0) {
        return;
    }
    C3dMesh& mesh = renderer->meshes[(size_t)m];
    if (!mesh.valid || n > mesh.capacity) {
        return;
    }
    // SAFE INSIDE THE FRAME IT BELONGS TO, which is the seam's contract
    // (render.h): C3D_FrameBegin waited for the previous queue, so nothing is
    // reading this buffer when the frame's own draws write it. An update outside a
    // frame would be an in-flight write; no caller does one.
    float* out = static_cast<float*>(mesh.data);
#ifdef MDK_HW_PROFILE
    const uint64_t updStart = svcGetSystemTick();
#endif
    for (int32_t i = 0; i < n; i++) {
        out[i * 5 + 0] = v[i].pos.x;
        out[i * 5 + 1] = v[i].pos.y;
        out[i * 5 + 2] = v[i].pos.z;
        out[i * 5 + 3] = v[i].uv.x;
        out[i * 5 + 4] = v[i].uv.y;
    }
#ifdef MDK_HW_PROFILE
    {
        static uint64_t copySum = 0;
        static int32_t copyCalls = 0;
        copySum += svcGetSystemTick() - updStart;
        copyCalls++;
        if ((renderer->frame % 300) == 0 && copyCalls > 0) {
            mdk::log3dsPrintf("meshupd: frame %d mean copy %.3f ms over %d calls",
                              (int)renderer->frame,
                              (double)copySum / (double)copyCalls * 1000.0 /
                                  (double)SYSCLOCK_ARM11,
                              (int)copyCalls);
            copySum = 0;
            copyCalls = 0;
        }
    }
#endif
    // THE CPU/GPU CACHE-VISIBILITY OBLIGATION (AGENTS.md P3) -- AND THIS CALL IS
    // NOT WHAT MEETS IT.
    //
    // WHAT IS MEASURED (review-1 F1, re-measured 2026-11-12 against the linked
    // `/opt/devkitpro/libctru/lib/libcitro3dd.a`):
    //   * `C3D_FrameEnd` flushes the ENTIRE linear heap: `tst r4,#2`, then
    //     `ldr r0, __ctru_linear_heap`; `ldr r1, __ctru_linear_heap_size`;
    //     `bl GSPGPU_FlushDataCache` (`arm-none-eabi-objdump -dr renderqueue.o`,
    //     relocs 0x168/0x16c). `mdk_render_end_frame`'s caller reaches
    //     `C3D_FrameEnd(0)`.
    //   * citro3d submits the command queue at that same frame end, so the flush
    //     precedes the GPU ever reading the vertex data. That is AGENTS.md P3's
    //     own sentence: the default `C3D_FrameEnd(0)` path "flushes linear memory
    //     before queue execution".
    //   * `GSPGPU_FlushDataCache` in this libctru is a SYNCHRONOUS service call:
    //     it ends in `bl svcSendSyncRequest` (`libctru.a:gspgpu.o`). So this line
    //     costs one GSP round-trip per mesh upload per frame, and the object
    //     stream, Kurt, the canvas and the diagnostic all upload every frame.
    //
    // THE ORDERING ARGUMENT, WRITTEN DOWN RATHER THAN IMPLIED. There is none that
    // favours keeping it. The mesh bytes are written here, the draws are recorded
    // later in the same frame, and the queue is not handed to the GPU until
    // `C3D_FrameEnd(0)` -- which flushes first. No path lets the GPU read these
    // bytes before that flush, so the frame-end flush is sufficient and this call
    // is redundant. The old comment claimed the opposite ("P3 asks for it and real
    // silicon needs it"); that claim is withdrawn.
    //
    // IT STAYS ANYWAY, FOR ONE HONEST REASON: the only target this loop can
    // measure on is Mandarine, whose PICA renderer reads the same host memory the
    // emulated CPU wrote, so NO EMULATOR RUN CAN DISTINGUISH this call from its
    // absence (measured 2026-11-11: the canvas is absent both with this flush and
    // with the D-cache deliberately thrashed by 512 KB of unrelated linear writes
    // before the draw). Removing a cache clean on the strength of an instrument
    // that cannot see it would be the same mistake as adding one. Dropping it is a
    // named follow-up in docs/OPEN_ITEMS.md, gated on an original-3DS capture --
    // and it is a PERFORMANCE win on the 268 MHz target, not a correctness fix.
    //
    // AND IT WAS NEVER THE CANVAS BUG. The theory that a mesh written during the
    // frame was still dirty when the replay handed its address to
    // `C3D_DrawArrays` is refuted; see docs/OPEN_ITEMS.md, "The 3DS canvas draws
    // no pixels", for what the measurements now do say.
    //
    // THE RANGE IS THE WRITTEN PREFIX, NOT THE BUFFER. The clean is per line and
    // the cost is proportional to the range; a mesh of 4096 quads is 245760
    // bytes and flushing all of it to publish 24 corners would be paying for a
    // capacity the frame did not use.
    //
    // REMOVED 2026-11-23, on the original-3DS evidence the follow-up above
    // asked for: hw_cycle --profile on old3ds-lan167 attributes ~4.6 ms of
    // a corridor frame's draw span to the object pass whose only heavy
    // call was this flush (3417 corners, 38 draws), and C3D_FrameEnd's
    // whole-linear-heap flush precedes queue execution (the disassembly
    // argument above), so no GPU read can see unflushed bytes. The A/B
    // run and a CAPTURE=800 readback proving objects still draw are the
    // evidence (docs/HANDOFF_MUSE.md, campaign update).
    mesh.count = n;
}

void mdk_render_release_mesh(MdkRenderer r, MdkMeshId m) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || m < 0 || (size_t)m >= renderer->meshes.size()) {
        return;
    }
    C3dMesh& mesh = renderer->meshes[(size_t)m];
    // DEFERRED, LIKE A TEXTURE. The mesh may have been drawn by a frame whose
    // queue has not been waited yet -- C3D_DrawArrays only records the command,
    // and the queue is submitted at C3D_FrameEnd -- so the linear memory is
    // still an in-flight resource. C3D_FrameBegin waits the previous queue, so
    // the next frame boundary is the completion boundary (AGENTS.md P3).
    //
    // The cost is that a mesh that grows (the object stream's, which doubles
    // when a frame needs more) holds its old buffer for one more frame. That is
    // a bounded transient, and it is what P3 asks for.
    mesh.valid = false;
    mesh.count = 0;
    if (mesh.data) {
        mesh.releasePending = true;
    }
}

// Free the meshes whose frame has finished. Called at the frame boundary, after
// C3D_FrameBegin waited for the previous queue (AGENTS.md P3).
static void settleDeferredMeshes(RendererC3D* renderer) {
    if (!renderer) {
        return;
    }
    for (size_t i = 0; i < renderer->meshes.size(); i++) {
        C3dMesh& mesh = renderer->meshes[i];
        if (!mesh.releasePending) {
            continue;
        }
        if (mesh.data) {
            linearFree(mesh.data);
            mesh.data = nullptr;
        }
        mesh.releasePending = false;
        mesh.capacity = 0;
    }
}

// THE SCOPE (the C#'s Renderer.Mark / Release, and the Vulkan backend's
// creationLog): the mark is the log's length, and a release pops the log back to it,
// releasing each resource newest first through the ordinary deferred release -- so
// nothing the GPU may still read is freed before the next frame boundary (P3). A
// resource released earlier by its owner is released again harmlessly: a released
// texture has no GPU copy, a released mesh no data, a released palette no bytes.
// The renderer's OWN resources are skipped: the canvas mesh is made on the first
// frame that draws the HUD, so inside whatever scope is open then, and the renderer
// keeps it across screens (07_canvas.inc).
MdkScope mdk_render_mark(MdkRenderer r) {
    RendererC3D* renderer = asC3d(r);
    MdkScope scope;
    scope.textures = renderer ? (int32_t)renderer->creationLog.size() : 0;
    scope.buffers = 0;
    return scope;
}

void mdk_render_release(MdkRenderer r, MdkScope s) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || s.textures < 0) {
        return;
    }
    while ((int32_t)renderer->creationLog.size() > s.textures) {
        const RendererC3D::Created created = renderer->creationLog.back();
        renderer->creationLog.pop_back();
        if (created.kind == 0) {
            if (created.id != renderer->canvasWhite) {
                mdk_render_release_texture(r, created.id);
            }
        } else if (created.kind == 1) {
            mdk_render_release_palette(r, created.id);
        } else if (created.id != renderer->canvasMesh) {
            mdk_render_release_mesh(r, created.id);
        }
    }
}

