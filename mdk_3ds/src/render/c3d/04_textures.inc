// 04_textures.inc -- texture memory: eviction, slots, the palette expansion.
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

 // Free the LEAST RECENTLY USED texture, so that a new one can be made.
//
// THE EVICTION IS WHAT MAKES AN ARENA FIT. The 3DS has six megabytes of
// VRAM, the render targets take one and a half, and an arena's expanded
// textures come to around three more -- so the whole set does not fit at
// once with everything else. The textures that are actually being drawn
// stay; the rest go, and are made again when they are next needed.
//
// ONLY A TEXTURE THAT CANNOT BE IN FLIGHT MAY GO. A texture drawn by this
// frame is read by this frame's commands, and a texture submitted by the
// previous frame is still in flight until the next C3D_FrameBegin waits for
// that queue (AGENTS.md P3). Outside a frame after a frame has been
// submitted, nothing may be evicted.
//
// The evicted texture KEEPS ITS SOURCE, so it can be made again: the
// eviction gives the VRAM back without making the id a dead end.
//
// Returns false when there is no texture that may be freed.
static bool evictTextures(RendererC3D* renderer, size_t needed) {
    (void)needed;
    if (!renderer || renderer->gpuPending) {
        return false;
    }
    // The least recently used texture this frame has not used.
    int oldest = -1;
    uint32_t oldestUse = 0xFFFFFFFFu;
    for (size_t i = 0; i < renderer->textures.size(); i++) {
        const C3dTexture& texture = renderer->textures[i];
        if (!texture.valid || texture.releasePending) {
            continue;
        }
        // A chunked expansion in flight is never a victim: its source and
        // palette are what its next slice reads. (Pending textures are not
        // valid, so this is belt on that braces -- stated, not inferred.)
        if (texture.expandPending || texture.expandPixels || texture.expandWanted) {
            continue;
        }
        if (renderer->inFrame && texture.lastUsed >= renderer->frameStartUse) {
            continue;
        }
        if (texture.lastUsed < oldestUse) {
            oldestUse = texture.lastUsed;
            oldest = (int)i;
        }
    }
    if (oldest < 0) {
        return false;
    }
    C3dTexture& victim = renderer->textures[(size_t)oldest];
    C3D_TexDelete(&victim.tex);
    victim.valid = false;
    renderer->texturesEvicted++;
    return true;
}

// A free slot in the texture table, or a new one.
//
// The ids the seam hands out are indices into the table, and a caller that
// releases a texture and makes it again -- the sprite cache does exactly
// that when its residency budget says a frame has to go -- must not grow
// the table for ever. A slot with a source is NOT free: it is an evicted
// texture that can be made again, and its source is the only copy there is.
static int32_t acquireTextureSlot(RendererC3D* renderer) {
    for (size_t i = 0; i < renderer->textures.size(); i++) {
        const C3dTexture& texture = renderer->textures[i];
        // A pending chunked expansion owns its slot and its buffer until it
        // finishes or is dropped: never hand the slot to a new texture.
        if (!texture.valid && !texture.releasePending && texture.source.empty() &&
            !texture.expandPending && !texture.expandPixels && !texture.expandWanted) {
            return (int32_t)i;
        }
    }
    renderer->textures.push_back(C3dTexture());
    return (int32_t)renderer->textures.size() - 1;
}

// The expanded RGBA8 image of an index texture: the indices through the
// palette, and the padding transparent. The caller frees it.
//
// THE RULE IS THE C#'s PALETTE SHADER'S RULE EXACTLY:
//
//     index == 0   -> TRANSPARENT (the shader discards it)
//     otherwise    -> the palette's RGB, and the alpha is FORCED to 255
//
// The palette's own alpha is NOT used: the C#'s shader takes the palette's
// RGB and writes an alpha of 1.0, and the transparency comes from the
// index-0 discard.
//
// THE FOUR BYTES GO IN THE PICA'S ORDER, NOT THE PALETTE'S. The palette is
// RGBA in memory, the hardware reads `[A, B, G, R]`, and writing the palette's
// order straight through is what pinned the console's red channel to 255 on
// every painted pixel. See `pica::texelBytes`.
// Rows [fromY, toY) of the expansion below, into an already-zeroed chain
// buffer; returns whether any index-0 texel was seen. Split out so a
// chunked expansion can do a slice of rows a frame; the whole-range call
// is exactly the old loop.
static bool expandIndexRows(const uint8_t* indices, int32_t width, int32_t fromY, int32_t toY,
                            const uint8_t* paletteRgba, int32_t paddedWidth, uint8_t* pixels) {
    bool sawTransparent = false;
    for (int32_t y = fromY; y < toY; y++) {
        const uint8_t* source = indices + (size_t)y * (size_t)width;
        for (int32_t x = 0; x < width; x++) {
            const uint8_t index = source[x];
            if (index == 0) {
                sawTransparent = true;
                continue;
            }
            const size_t slot =
                mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth);
            mdk::pica::texelBytes(pixels + slot * 4u, paletteRgba[index * 4 + 0],
                                  paletteRgba[index * 4 + 1], paletteRgba[index * 4 + 2], 255u);
        }
    }
    return sawTransparent;
}

static uint8_t* expandIndices(const uint8_t* indices, int32_t width, int32_t height,
                              const uint8_t* paletteRgba, int32_t paddedWidth,
                              int32_t paddedHeight, int32_t mipLevels) {
    // Level 0 and, under Enhanced Mode enableMipmaps, the chain after it in the
    // same buffer (pica::buildMipChain, levels consecutive).
    const size_t texels = mdk::pica::mipChainTexels(paddedWidth, paddedHeight, mipLevels);
    uint8_t* pixels = static_cast<uint8_t*>(malloc(texels * 4));
    if (!pixels) {
        return nullptr;
    }
    // The padding is transparent. Each source row is read at its own stride:
    // the source is width pixels wide, but the padded texture is paddedWidth
    // pixels wide, so treating the source as one contiguous run would shift
    // every row after the first.
    //
    // THE DESTINATION IS A TILE SLOT, NOT `y*paddedWidth + x`. The PICA reads
    // its texels in 8x8 Morton tiles (see `pica::textureTexelIndex`), so a
    // row-major write is sampled scrambled -- measured, and the reason the
    // arena went invisible through the alpha test rather than merely looking
    // wrong.
    memset(pixels, 0, texels * 4);
#ifdef MDK_HW_PROFILE
    const uint64_t expT0 = svcGetSystemTick();
#endif
    // Transparent texels need no write (the memset left the slot all-zero,
    // and black-with-alpha-255 is what the palette would wrongly give).
    const bool sawTransparent =
        expandIndexRows(indices, width, 0, height, paletteRgba, paddedWidth, pixels);
#ifdef MDK_HW_PROFILE
    const uint64_t expT1 = svcGetSystemTick();
#endif
    // Ready for Enhanced Mode filtering whichever way its toggle stands now: the
    // toggle is live, the texture is made once (pica::prepareForFiltering; a
    // nearest sample of any texel it touches is unchanged in colour or
    // discarded as before, except at an exact padded edge, which now clamps).
    mdk::pica::prepareForFiltering(pixels, width, height, paddedWidth, paddedHeight,
                                   sawTransparent);
#ifdef MDK_HW_PROFILE
    const uint64_t expT2 = svcGetSystemTick();
#endif
    mdk::pica::buildMipChain(pixels, paddedWidth, paddedHeight, width, height, mipLevels);
#ifdef MDK_HW_PROFILE
    const uint64_t expT3 = svcGetSystemTick();
    const double msPer = 1000.0 / (double)SYSCLOCK_ARM11;
    const double expMs = (double)(expT3 - expT0) * msPer;
    if (expMs > 50.0) {
        mdk::log3dsPrintf("expandstage: %dx%d main %.1f filter %.1f mip %.1f ms%s", (int)width,
                          (int)height, (double)(expT1 - expT0) * msPer,
                          (double)(expT2 - expT1) * msPer, (double)(expT3 - expT2) * msPer,
                          sawTransparent ? "" : " (opaque)");
    }
#endif
    return pixels;
}

// The expanded RGBA8 image of an RGBA texture: the real pixels, with the
// POT padding transparent. The PICA200's texture-size register requires
// power-of-two dimensions of at least 8, just as it does for an index
// texture. The draw path's UV transform maps the real rectangle into the
// padded one.
//
// NOT A memcpy, AND THAT IS THE POINT. The source is the seam's RGBA-in-memory
// order; the PICA wants `[A, B, G, R]` (`pica::texelBytes`), so every texel has
// to be rotated on the way in. Row-wise memcpy was the first version and it is
// what painted every flat-coloured surface with its own alpha as red.
static uint8_t* expandRgba(const uint8_t* source, int32_t width, int32_t height,
                           int32_t paddedWidth, int32_t paddedHeight, int32_t mipLevels) {
    const size_t texels = mdk::pica::mipChainTexels(paddedWidth, paddedHeight, mipLevels);
    uint8_t* pixels = static_cast<uint8_t*>(malloc(texels * 4));
    if (!pixels) {
        return nullptr;
    }
    memset(pixels, 0, texels * 4);
    bool sawTransparent = false;
    for (int32_t y = 0; y < height; y++) {
        const uint8_t* row = source + (size_t)y * (size_t)width * 4;
        for (int32_t x = 0; x < width; x++) {
            if (row[(size_t)x * 4 + 3] == 0) {
                sawTransparent = true;
            }
            const size_t slot =
                mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth);
            mdk::pica::texelBytes(pixels + slot * 4u, row[(size_t)x * 4 + 0],
                                  row[(size_t)x * 4 + 1], row[(size_t)x * 4 + 2],
                                  row[(size_t)x * 4 + 3]);
        }
    }
    mdk::pica::prepareForFiltering(pixels, width, height, paddedWidth, paddedHeight,
                                   sawTransparent);
    mdk::pica::buildMipChain(pixels, paddedWidth, paddedHeight, width, height, mipLevels);
    return pixels;
}

// WHAT IS LEFT, when a texture does not fit. The probe is the only
// diagnostic the 3DS gives: an allocation that fails says nothing at all,
// and that silence is how a whole arena's textures once refused to allocate
// one after another while the screen showed only the clear colour.
static void probeTextureMemory(int32_t width, int32_t height, size_t bytes) {
    void* probe = linearAlloc(1024u * 1024u);
    mdk::log3dsPrintf("texture %dx%d (%d bytes) failed; linear 1MB: %s",
                      (int)width, (int)height, (int)bytes, probe ? "ok" : "FULL");
    if (probe) {
        linearFree(probe);
    }
    C3dTexture small;
    if (C3D_TexInit(&small.tex, 64, 64, GPU_RGBA8)) {
        mdk::log3dsPrintf("  64x64 linear: yes");
        C3D_TexDelete(&small.tex);
    } else {
        mdk::log3dsPrintf("  64x64 linear: no");
    }
    C3dTexture smallVram;
    if (C3D_TexInitVRAM(&smallVram.tex, 64, 64, GPU_RGBA8)) {
        mdk::log3dsPrintf("  64x64 vram: yes");
        C3D_TexDelete(&smallVram.tex);
    } else {
        mdk::log3dsPrintf("  64x64 vram: no");
    }
    // THE BOUNDARY. Is the limit a dimension, or a total size?
    static const int probes[][2] = {
        {64, 64}, {64, 128}, {128, 64}, {128, 128},
        {64, 256}, {256, 64}, {128, 176}, {32, 512}, {512, 32},
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        C3dTexture probeTex;
        const bool ok = C3D_TexInit(&probeTex.tex, (u16)probes[i][0], (u16)probes[i][1],
                                    GPU_RGBA8);
        mdk::log3dsPrintf("  %dx%d: %s", probes[i][0], probes[i][1], ok ? "yes" : "NO");
        if (ok) {
            C3D_TexDelete(&probeTex.tex);
        }
    }
    // And the same boundary in RGBA4, which is what the working port uses.
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        C3dTexture probeTex;
        const bool ok = C3D_TexInit(&probeTex.tex, (u16)probes[i][0], (u16)probes[i][1],
                                    GPU_RGBA4);
        mdk::log3dsPrintf("  rgba4 %dx%d: %s", probes[i][0], probes[i][1], ok ? "yes" : "NO");
        if (ok) {
            C3D_TexDelete(&probeTex.tex);
        }
    }
}

// The PICA texel format of a store (C3D_TexInitWithParams' format), and its
// slot in RendererC3D::texturesMade. The ETC formats are the HD layer's, which
// tex3ds imports itself; an expanded texture is never one, so they read RGBA8.
static GPU_TEXCOLOR picaTexelFormat(mdk::pica::TexelFormat format) {
    switch (format) {
        case mdk::pica::TexelFormat::Rgb565: return GPU_RGB565;
        case mdk::pica::TexelFormat::Rgba5551: return GPU_RGBA5551;
        case mdk::pica::TexelFormat::Rgba4: return GPU_RGBA4;
        default: return GPU_RGBA8;
    }
}

static int32_t texelStoreIndex(mdk::pica::TexelFormat format) {
    switch (format) {
        case mdk::pica::TexelFormat::Rgb565: return 0;
        case mdk::pica::TexelFormat::Rgba5551: return 1;
        case mdk::pica::TexelFormat::Rgba4: return 2;
        default: return 3;
    }
}

// THE COMPRESSED TEXTURE'S GPU COPY (EnhancedToggles::kurtCompressed): tex3ds'
// import of the .t3x bytes kept in `source` -- an LZ decompress and a copy into
// the linear heap, the ETC blocks already tiled by tex3ds on the host. Nothing is
// expanded, which is the point: a Kurt frame made from its palette indices was a
// 256 KB RGBA8 expansion of several ms on the ARM11, re-made nearly every frame
// once the animations outran the residency (measured on the Old 3DS: clusters of
// 13-16 frames with a 20-40 ms draw build).
//
// THE LOOK. ETC1 codes each 4x4 block as one colour pair and a modifier table,
// so a sharp colour edge inside a block softens (the 4x4 block look); the owner
// ruled performance over accuracy on the 3DS (2026-11-18) and saw little
// difference against RGBA4 (2026-11-19). The ALPHA is exact: ETC1A4 has four
// explicit bits a texel and Kurt's alpha is a colour key (0 or 255 -> 0 or 15),
// so both alpha-test references cut him texel for texel as the original does
// (kurt_etc1_convert.py --check proves it per frame).
//
// The same eviction ladder as makeTexture: a nobody's HD copy, then the least
// recently used original, then any HD copy this frame has not drawn.
static bool importCompressed(RendererC3D* renderer, C3dTexture& texture) {
    if (texture.source.empty()) {
        return false;
    }
    // THE HEADER FIRST, so a file that is not this set's never reaches the
    // eviction ladder below: an import that fails for a broken file would
    // otherwise evict every texture it could before giving up. tex3ds' header:
    // u16 subtextures, u8 log2(width) - 3 | (log2(height) - 3) << 3, u8 format.
    // Only ETC1/ETC1A4 (GPU_ETC1 = 12, GPU_ETC1A4 = 13): the compressed seam's
    // contract, and the formats `format` below can name.
    const uint8_t* header = texture.source.data();
    const bool headerOk =
        texture.source.size() > 6 && (header[0] | (header[1] << 8)) >= 1 &&
        (header[3] == (uint8_t)GPU_ETC1 || header[3] == (uint8_t)GPU_ETC1A4) &&
        (8 << (header[2] & 7)) >= texture.realWidth &&
        (8 << ((header[2] >> 3) & 7)) >= texture.realHeight;
    if (!headerOk) {
        mdk::log3dsPrintf("texture %dx%d: not an ETC .t3x of that size (%u bytes), refused",
                          (int)texture.realWidth, (int)texture.realHeight,
                          (unsigned)texture.source.size());
        return false;
    }
    Tex3DS_Texture t3x = nullptr;
    for (;;) {
        t3x = Tex3DS_TextureImport(texture.source.data(), texture.source.size(), &texture.tex,
                                   nullptr, false);
        if (t3x) {
            break;
        }
        if (evictHd(renderer, renderer->frameStartUse, true) ||
            evictTextures(renderer, texture.source.size() * 4u) ||
            evictHd(renderer, renderer->frameStartUse, false)) {
            continue;
        }
        mdk::log3dsPrintf("texture %dx%d: compressed import FAILED (%u bytes)",
                          (int)texture.realWidth, (int)texture.realHeight,
                          (unsigned)texture.source.size());
        return false;
    }
    Tex3DS_TextureFree(t3x);
    texture.paddedWidth = texture.tex.width;
    texture.paddedHeight = texture.tex.height;
    texture.rect = mdk::pica::compressedFrameRect(texture.realWidth, texture.realHeight,
                                                  texture.paddedWidth, texture.paddedHeight);
    texture.mipLevels = texture.tex.maxLevel;
    texture.gpuBytes = (uint32_t)C3D_TexCalcTotalSize(texture.tex.size, texture.tex.maxLevel);
    texture.format = texture.tex.fmt == GPU_ETC1 ? mdk::pica::TexelFormat::Etc1
                                                 : mdk::pica::TexelFormat::Etc1A4;
    // NEAREST at creation (the original look); the draw rewrites the filter per
    // draw (06_draw.inc). REPEAT only on an axis the image fills: on a padded one
    // a wrap would sample the padding (the HD copies' rule).
    C3D_TexSetFilter(&texture.tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&texture.tex,
                   texture.realWidth == texture.paddedWidth ? GPU_REPEAT : GPU_CLAMP_TO_EDGE,
                   texture.realHeight == texture.paddedHeight ? GPU_REPEAT : GPU_CLAMP_TO_EDGE);
    texture.valid = true;
    renderer->compressedMade++;
    renderer->compressedBytesSaved +=
        mdk::pica::textureBytes(mdk::pica::TexelFormat::Rgba8, texture.paddedWidth,
                                texture.paddedHeight, 0) -
        mdk::pica::textureBytes(texture.format, texture.paddedWidth, texture.paddedHeight, 0);
    return true;
}

// A STREAMED picture's store (C3dTexture::streamed, package #20c): RGB565, no
// mip chain, the padding cleared once, the picture written straight into the
// texture's own linear buffer (pica::streamTexelsRgb565) -- no expansion
// buffer, no 16-bit pack. Wrap and filter as makeTexture's (NEAREST, CLAMP on a
// padded axis). A full heap evicts as makeTexture does, once.
static bool makeStreamed(RendererC3D* renderer, C3dTexture& texture) {
    const size_t bytes = (size_t)texture.paddedWidth * (size_t)texture.paddedHeight * 2u;
    C3D_TexInitParams params = {};
    params.width = (u16)texture.paddedWidth;
    params.height = (u16)texture.paddedHeight;
    params.maxLevel = 0;
    params.format = GPU_RGB565;
    params.type = GPU_TEX_2D;
    params.onVram = false;
    if (!C3D_TexInitWithParams(&texture.tex, nullptr, params)) {
        if (!evictTextures(renderer, bytes) ||
            !C3D_TexInitWithParams(&texture.tex, nullptr, params)) {
            mdk::log3dsPrintf("streamed texture %dx%d FAILED: no texture memory",
                              (int)texture.realWidth, (int)texture.realHeight);
            return false;
        }
    }
    C3D_TexSetFilter(&texture.tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&texture.tex,
                   texture.realWidth == texture.paddedWidth ? GPU_REPEAT : GPU_CLAMP_TO_EDGE,
                   texture.realHeight == texture.paddedHeight ? GPU_REPEAT : GPU_CLAMP_TO_EDGE);
    memset(texture.tex.data, 0, bytes);
    mdk::pica::streamTexelsRgb565(texture.source.data(), texture.realWidth, texture.realHeight,
                                  texture.palette.data(), texture.paddedWidth,
                                  (uint8_t*)texture.tex.data);
    C3D_TexFlush(&texture.tex);
    texture.valid = true;
    texture.format = mdk::pica::TexelFormat::Rgb565;
    texture.mipLevels = 0;
    texture.hasHoles = false;
    renderer->texturesMade[texelStoreIndex(texture.format)]++;
    return true;
}

// Makes the GPU copy of a texture from the source the backend kept, and
// fills it.
//
// The allocation can fail because the VRAM is full, and the VRAM IS
// routinely full: the render targets take one and a half megabytes, the
// command buffer half a megabyte, and an arena's expanded textures come to
// around three more, against six megabytes.
//
// So the allocation RETRIES, each time evicting the least recently used
// texture. The textures that are being drawn stay; the ones that are not
// are made again when they are next needed.
static bool makeTextureNow(RendererC3D* renderer, C3dTexture& texture, bool diagnostics);

// ---------------------------------------------------------------------------
// THE CHUNKED EXPANSION (HANDOFF_MUSE.md task 2). A large indexed texture's
// make measured 64x704: expand 41.9 ms + pack 18.4 ms + alloc/load ~5 ms in
// one frame (frame 795's 77 ms texmake). The pump cannot reach that batch in
// the pair window, so the make itself is sliced: rows in slices, then the
// filter and mip passes as single steps, then the 16-bit pack in texel
// slices, then the ordinary upload (makeTextureNow consuming the finished
// buffer as a preset). The frame-boundary pump advances every pending
// expansion ONE step a frame; a draw that needs the texture first finishes
// it synchronously, so no frame ever binds a half-made texture and the
// picture is unchanged -- only its timing is.
// ---------------------------------------------------------------------------
static const size_t kChunkMinTexels = 32768u;   // below this a make is < ~20 ms: synchronous
static const int32_t kChunkRowTexels = 16384;   // one row step's texel budget (64 wide: 256 rows)
static const size_t kChunkPackTexels = 16384u;  // 64x1024 chain: 4 pack steps
static const int32_t kPumpStepsPerFrame = 3;    // total steps the frame pump does, all textures

static void dropExpansion(C3dTexture& texture) {
    free(texture.expandPixels);
    texture.expandPixels = nullptr;
    texture.expandPending = false;
    texture.expandWanted = false;
    texture.expandRow = 0;
    texture.expandStage = 0;
    texture.expandPackAt = 0;
    texture.expandSawTransparent = false;
    texture.expandLevels = 0;
    texture.expandFormat = mdk::pica::TexelFormat::Rgba8;
}

// Simultaneous chunked expansions each hold a full RGBA8 chain in the
// system heap; the cap keeps the large-first pump from holding an arena's
// worth at once (the emulator's bigger heap would hide that, AGENTS.md P8).
static const int32_t kMaxPendingExpansions = 4;

static int32_t pendingExpansions(RendererC3D* renderer) {
    int32_t count = 0;
    for (size_t i = 0; i < renderer->textures.size(); i++) {
        if (renderer->textures[i].expandPending) {
            count++;
        }
    }
    return count;
}

// 1 = started (or already in flight), 0 = not a chunked candidate or the
// source is invalid, -1 = deferred by the pending cap (expandWanted is the
// caller's to set; the frame pump starts it when a slot frees). A draw's
// finish (force) ignores the cap: the texture is needed now.
static int32_t startExpansion(RendererC3D* renderer, C3dTexture& texture, bool force) {
    if (texture.expandPending || texture.expandPixels) {
        return 1;
    }
    if (!texture.indexed || texture.compressed || texture.streamed) {
        return 0;
    }
    if (!force && pendingExpansions(renderer) >= kMaxPendingExpansions) {
        return -1;
    }
    const size_t realBytes = (size_t)texture.realWidth * (size_t)texture.realHeight;
    if (texture.source.size() != realBytes || texture.palette.size() < 256 * 4) {
        mdk::log3dsPrintf("texture %dx%d source is invalid", (int)texture.realWidth,
                          (int)texture.realHeight);
        return 0;
    }
    texture.expandLevels = renderer->toggles.enableMipmaps
                               ? mdk::pica::mipLevelsFor(texture.paddedWidth,
                                                         texture.paddedHeight)
                               : 0;
    const size_t texels = mdk::pica::mipChainTexels(texture.paddedWidth, texture.paddedHeight,
                                                    texture.expandLevels);
    texture.expandPixels = static_cast<uint8_t*>(malloc(texels * 4u));
    if (!texture.expandPixels) {
        return 0;
    }
    memset(texture.expandPixels, 0, texels * 4u);
    texture.expandRow = 0;
    texture.expandStage = 0;
    texture.expandSawTransparent = false;
    texture.expandPackAt = 0;
    texture.expandPending = true;
#ifdef MDK_HW_PROFILE
    mdk::log3dsPrintf("texchunk: %dx%d start frame %d", (int)texture.realWidth,
                      (int)texture.realHeight, (int)renderer->frame);
#endif
    return 1;
}

// One step (or all of them, when a draw needs the texture now). Returns
// false only when the finishing upload fails; the state is then clean and
// a later make retries synchronously from the kept source. The levels are
// frozen at the start (the chain buffer is sized by them) and the store is
// chosen at stage 3, so an enableMipmaps flip mid-expansion reaches this
// texture only at its next making; the texture16 store follows the flip.
// PROFILE builds charge the step to textureMakeTicks, so the pump's
// work stays visible in the arena table instead of vanishing from texMs.
static bool advanceExpansionSteps(RendererC3D* renderer, C3dTexture& texture, bool finishAll) {
    if (!texture.expandPending) {
        return texture.valid;
    }
    for (;;) {
        switch (texture.expandStage) {
            case 0: {
                int32_t stepRows = kChunkRowTexels / (texture.realWidth > 0 ? texture.realWidth : 1);
                if (stepRows < 1) {
                    stepRows = 1;
                }
                int32_t to = finishAll ? texture.realHeight : texture.expandRow + stepRows;
                if (to > texture.realHeight) {
                    to = texture.realHeight;
                }
                if (expandIndexRows(texture.source.data(), texture.realWidth, texture.expandRow,
                                    to, texture.palette.data(), texture.paddedWidth,
                                    texture.expandPixels)) {
                    texture.expandSawTransparent = true;
                }
                texture.expandRow = to;
                if (to >= texture.realHeight) {
                    texture.expandStage = 1;
                }
                break;
            }
            case 1:
                mdk::pica::prepareForFiltering(texture.expandPixels, texture.realWidth,
                                               texture.realHeight, texture.paddedWidth,
                                               texture.paddedHeight,
                                               texture.expandSawTransparent);
                texture.expandStage = 2;
                break;
            case 2:
                mdk::pica::buildMipChain(texture.expandPixels, texture.paddedWidth,
                                         texture.paddedHeight, texture.realWidth,
                                         texture.realHeight, texture.expandLevels);
                texture.expandStage = 3;
                break;
            case 3: {
                if (texture.expandPackAt == 0) {
                    texture.expandFormat = mdk::pica::expandedTexelFormat(
                        renderer->toggles.texture16, texture.keepRgba8, texture.paddedWidth,
                        texture.paddedHeight, texture.expandPixels,
                        (size_t)texture.paddedWidth * (size_t)texture.paddedHeight);
                }
                const size_t chain = mdk::pica::mipChainTexels(
                    texture.paddedWidth, texture.paddedHeight, texture.expandLevels);
                if (texture.expandFormat == mdk::pica::TexelFormat::Rgba8) {
                    // No pack to slice: the chain is already in its store.
                    texture.expandPackAt = chain;
                    texture.expandStage = 4;
                    break;
                }
                size_t count = chain - texture.expandPackAt;
                if (!finishAll && count > kChunkPackTexels) {
                    count = kChunkPackTexels;
                }
                mdk::pica::packTexels16Range(texture.expandPixels, texture.expandPackAt, count,
                                             texture.expandFormat);
                texture.expandPackAt += count;
                if (texture.expandPackAt >= chain) {
                    texture.expandStage = 4;
                }
                break;
            }
            default: {
                // The finished buffer stays on the texture as a preset;
                // makeTextureNow consumes (and frees) it in its upload.
                texture.expandPending = false;
                const bool made = makeTextureNow(renderer, texture, false);
                if (!made) {
                    dropExpansion(texture);
                }
#ifdef MDK_HW_PROFILE
                if (made) {
                    mdk::log3dsPrintf("texchunk: %dx%d ready frame %d", (int)texture.realWidth,
                                      (int)texture.realHeight, (int)renderer->frame);
                }
#endif
                return made;
            }
        }
        if (!finishAll) {
            return true;
        }
    }
}

static bool advanceExpansion(RendererC3D* renderer, C3dTexture& texture, bool finishAll) {
#ifdef MDK_HW_PROFILE
    const uint64_t stepStart = svcGetSystemTick();
    const bool stepped = advanceExpansionSteps(renderer, texture, finishAll);
    renderer->textureMakeTicks += svcGetSystemTick() - stepStart;
    return stepped;
#else
    return advanceExpansionSteps(renderer, texture, finishAll);
#endif
}

// The frame-boundary pump: one step per pending expansion per frame, after
// C3D_FrameBegin's wait (03_frame.inc), so an upload's evictions are legal.
static void pumpExpansions(RendererC3D* renderer) {
    // Starts deferred by the pending cap go first, as slots free.
    int32_t pending = pendingExpansions(renderer);
    for (size_t i = 0; i < renderer->textures.size() && pending < kMaxPendingExpansions; i++) {
        C3dTexture& texture = renderer->textures[i];
        if (texture.expandWanted && !texture.expandPending) {
            const int32_t started = startExpansion(renderer, texture, false);
            if (started > 0) {
                texture.expandWanted = false;
                pending++;
            } else if (started == 0) {
                texture.expandWanted = false;   // a definitive failure is not retried
            }
        }
    }
    // One step a texture, oldest first, up to the frame's step budget: the
    // pump's work in a frame is bounded however many expansions are pending.
    int32_t steps = 0;
    for (size_t i = 0; i < renderer->textures.size() && steps < kPumpStepsPerFrame; i++) {
        if (renderer->textures[i].expandPending) {
            advanceExpansion(renderer, renderer->textures[i], false);
            steps++;
        }
    }
}

static bool makeTexture(RendererC3D* renderer, C3dTexture& texture, bool diagnostics) {
    if (texture.valid) {
        return true;
    }
    if (texture.expandWanted && !texture.expandPending) {
        // Started or definitively failed: wanted is spent either way, and a
        // failure falls through to the synchronous make below.
        texture.expandWanted = startExpansion(renderer, texture, true) < 0;
    }
    if (texture.expandPending) {
        return advanceExpansion(renderer, texture, true);
    }
#ifdef MDK_HW_PROFILE
    // Timed for the arena-entry table (textureMakeTicks): one tick read a
    // making. Profile builds only — an SVC per make does not belong in the
    // shipped path, least of all in the stutter being measured.
    const uint64_t start = svcGetSystemTick();
    const bool made = makeTextureNow(renderer, texture, diagnostics);
    const uint64_t mk = svcGetSystemTick() - start;
    renderer->textureMakeTicks += mk;
    // WHICH make was the expensive one: the entry table says how many and how
    // long; this names each by size and path (the oversized refused strips are
    // identifiable by dimension alone: sl_big 64x1920, fire 64x2628,
    // explode 128x3328). Profile builds only, like the tick read above.
    mdk::log3dsPrintf("texmake %dx%d %s %.1f ms", (int)texture.realWidth, (int)texture.realHeight,
                      texture.compressed ? "pack" : (texture.streamed ? "stream" : "expand"),
                      (double)mk * 1000.0 / (double)SYSCLOCK_ARM11);
    return made;
#else
    return makeTextureNow(renderer, texture, diagnostics);
#endif
}

static bool makeTextureNow(RendererC3D* renderer, C3dTexture& texture, bool diagnostics) {
    // Texel memory is about to be (re)filled, possibly at an address a bound
    // texture had: no unit's bind may be skipped on the strength of the old
    // contents (03_frame.inc, THE DEDUPE).
    forgetTextureBinds(renderer);
    if (texture.compressed) {
        return importCompressed(renderer, texture);
    }
    if (texture.streamed) {
        return makeStreamed(renderer, texture);
    }
    // THE MIP CHAIN (SPEC.md 7.9 enableMipmaps): made with the texture when the
    // toggle is on at that moment, so turning it on costs its memory -- a third
    // more per texture -- from the next texture made (the next level load, or an
    // evicted texture coming back); the sampler's use of it is live (06_draw.inc).
    int32_t levels = renderer->toggles.enableMipmaps
                         ? mdk::pica::mipLevelsFor(texture.paddedWidth, texture.paddedHeight)
                         : 0;
    const size_t realBytes = (size_t)texture.realWidth * (size_t)texture.realHeight *
                             (texture.indexed ? 1u : 4u);
    if (texture.source.size() != realBytes ||
        (texture.indexed && texture.palette.size() < 256 * 4)) {
        mdk::log3dsPrintf("texture %dx%d source is invalid",
                          (int)texture.realWidth, (int)texture.realHeight);
        return false;
    }

    // THE PIXELS, BEFORE THE ALLOCATION: the store (texture16) is chosen from the
    // expanded texels' alpha, and the allocation's size is the store's.
    //
    // A PLAIN CPU BUFFER, NOT linearAlloc.
    //
    // The linear heap STARTS IN THE VRAM: its first megabytes are texture
    // memory, and only then does it extend into system RAM. So a
    // temporary buffer taken from the linear heap PERMANENTLY EATS VRAM --
    // and this one is a temporary: it is copied into the texture and thrown
    // away. Every expansion was therefore costing the 3DS its texture
    // memory, and the arena's textures stopped allocating one after
    // another.
    uint8_t* pixels = nullptr;
#ifdef MDK_HW_PROFILE
    // THE STAGES OF A SLOW MAKE (profile builds only -- 42ee7a22's lesson: an
    // SVC read per texture make does not belong in the shipped path). The
    // arena-entry stalls are 53-163 ms a texture on OG; this says which stage
    // eats it: expand (palette), alloc (C3D_TexInit + eviction), pack (16-bit
    // rewrite), load (memcpy), flush (GSPGPU_FlushDataCache).
    const uint64_t texT0 = svcGetSystemTick();
#endif
    // A chunked expansion's finished buffer arrives as a preset: already
    // expanded, filtered, mip-built and packed in expandFormat/expandLevels,
    // so neither the expansion nor the pack below runs again.
    bool prepacked = false;
    if (texture.expandPixels) {
        pixels = texture.expandPixels;
        texture.expandPixels = nullptr;
        prepacked = true;
        levels = texture.expandLevels;
    } else if (texture.indexed) {
        pixels = expandIndices(texture.source.data(), texture.realWidth, texture.realHeight,
                               texture.palette.data(), texture.paddedWidth, texture.paddedHeight,
                               levels);
    } else {
        pixels = expandRgba(texture.source.data(), texture.realWidth, texture.realHeight,
                            texture.paddedWidth, texture.paddedHeight, levels);
    }
    if (!pixels) {
        return false;
    }
#ifdef MDK_HW_PROFILE
    const uint64_t texT1 = svcGetSystemTick();
#endif
    // THE STORE (EnhancedToggles::texture16): RGBA8, or 16 bits a texel -- RGB565
    // with no transparent texel, RGBA5551 with only 0/255 alpha (every index
    // texture), RGBA4 with partial alpha. pica::expandedTexelFormat carries the
    // reasons (the alpha test's two references survive each). Decided here, when
    // the texture is made: the toggle reaches a texture at its next making.
    const mdk::pica::TexelFormat format =
        prepacked ? texture.expandFormat
                  : mdk::pica::expandedTexelFormat(
                        renderer->toggles.texture16, texture.keepRgba8, texture.paddedWidth,
                        texture.paddedHeight, pixels,
                        (size_t)texture.paddedWidth * (size_t)texture.paddedHeight);
    size_t gpuBytes = mdk::pica::textureBytes(format, texture.paddedWidth, texture.paddedHeight,
                                              levels);
    bool allocated = false;
    bool diagnosticsRun = false;
    for (;;) {
        // C3D_TexInit, NOT C3D_TexInitVRAM.
        //
        // The VRAM-resident variant allocates from the 3DS's separate
        // texture VRAM, and it FAILS on this build for a texture of any
        // size -- even ninety kilobytes, with megabytes free. The
        // linear-heap variant is what the working port (../forsaken) uses,
        // and it works: the PICA200 samples textures out of the linear heap
        // as well as out of the texture VRAM.
        //
        // RGBA8 IS TILED TOO, AND THE OLD LINE HERE SAID OTHERWISE. It read
        // "RGBA8 is read LINEARLY by the PICA200; only the 16-bit and 8-bit
        // formats go through the Morton tiling", which is not true of this GPU
        // and was the reason the expansions wrote rows the hardware never read
        // as rows. The 8x8 Morton tile order applies to the 32-bit formats as
        // it does to the rest (`pica::textureTexelIndex` carries the citation,
        // the disassembly of `C3D_TexLoadImage`, and the reproduced symptom);
        // what C3D_TexInit's format argument selects is the TEXEL FORMAT, not
        // the layout. Nothing about the allocation changes here -- only where
        // the expansion writes its texels.
        //
        // THE FORMAT IS THE STORE'S, the same `format` the texels are packed in
        // below: a 16-bit buffer in an RGBA8 texture is half a texture of
        // garbage, an RGBA8 buffer in a 16-bit one is the top half twice over.
        C3D_TexInitParams params = {};
        params.width = (u16)texture.paddedWidth;
        params.height = (u16)texture.paddedHeight;
        params.maxLevel = (u8)levels;
        params.format = picaTexelFormat(format);
        params.type = GPU_TEX_2D;
        params.onVram = false;
        if (C3D_TexInitWithParams(&texture.tex, nullptr, params)) {
            allocated = true;
            break;
        }
        if (!diagnosticsRun && diagnostics) {
            diagnosticsRun = true;
            probeTextureMemory(texture.realWidth, texture.realHeight, gpuBytes);
        }
        // A nobody's HD copy (unlinked: a finished level's, a trimmed frame's)
        // goes before a live original is evicted.
        if (evictHd(renderer, renderer->frameStartUse, true)) {
            continue;
        }
        if (!evictTextures(renderer, gpuBytes)) {
            // The HD copies share this heap: one not drawn this frame goes before
            // an original fails to be made (review F1). Mid-frame, so only copies
            // older than this frame's first draw (the GPU finished the last one
            // at C3D_FrameBegin; this frame's commands are not submitted yet).
            if (evictHd(renderer, renderer->frameStartUse, false)) {
                continue;
            }
            // A CHAIN IS AN ENHANCEMENT, THE TEXTURE IS NOT: with nothing left to
            // evict, try once more without the chain before the surface goes
            // missing (review of the mipmap batch, F2). The expanded buffer still
            // holds level 0 at its start (the levels are consecutive), and the
            // store was chosen from level 0 alone, so neither is made again.
            if (levels > 0) {
                mdk::log3dsPrintf("texture %dx%d: no room for its mip chain; made without",
                                  (int)texture.realWidth, (int)texture.realHeight);
                levels = 0;
                gpuBytes = mdk::pica::textureBytes(format, texture.paddedWidth,
                                                   texture.paddedHeight, 0);
                continue;
            }
            break;
        }
    }
    if (!allocated) {
        free(pixels);
        mdk::log3dsPrintf("texture %dx%d FAILED: no texture memory",
                          (int)texture.realWidth, (int)texture.realHeight);
        return false;
    }
#ifdef MDK_HW_PROFILE
    const uint64_t texT2 = svcGetSystemTick();
#endif

    // SAMPLING, set explicitly rather than inherited from C3D_TexInit.
    //   Filter: NEAREST both ways at creation -- the ORIGINAL look. The oracle's
    //   palette shader looks up ONE index per texel (an index cannot be
    //   filtered), so the original look is point-sampled. Enhanced Mode
    //   FILTERING rewrites this per draw (06_draw.inc), live; the texture is
    //   prepared for it either way (pica::prepareForFiltering).
    //   Wrap: REPEAT on an axis whose real size IS the padded size, CLAMP
    //   otherwise. The world's tiled surfaces carry uv well outside [0,1]
    //   (2026-11-17, rdebug `uvlog`: all 45 tiling draws of level 3's view are
    //   256x256), and the oracle's sampler wraps them; with the default clamp
    //   they smeared into one edge texel (level 3's ground). A power-of-two axis
    //   has no padding, so REPEAT over the buffer is exactly REPEAT over the
    //   image. A padded axis cannot wrap exactly in hardware (it would wrap
    //   through the padding), so it keeps the clamp; no shipped tiling draw
    //   measured so far uses one.
    //   (Enhanced Mode FILTERING switches this per draw, live: 06_draw.inc.)
    C3D_TexSetFilter(&texture.tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&texture.tex,
                   texture.realWidth == texture.paddedWidth ? GPU_REPEAT : GPU_CLAMP_TO_EDGE,
                   texture.realHeight == texture.paddedHeight ? GPU_REPEAT : GPU_CLAMP_TO_EDGE);

    // THE 16-BIT STORE: the chain's texels rewritten in place, 4 bytes to 2 in
    // `format`'s word (pica::packTexels16, host-tested: the PICA's little-endian
    // u16, the same tile slots). Every texel up to the last level uploaded, and
    // nothing for RGBA8. After the expansion (which builds the chain in RGBA8:
    // the filter's colour bleed and the mip averaging stay one implementation),
    // before the first upload.
    // (Skipped for a chunked expansion's preset buffer: it arrives packed.)
    if (!prepacked)
    mdk::pica::packTexels16(pixels,
                            mdk::pica::mipChainTexels(texture.paddedWidth, texture.paddedHeight,
                                                      levels),
                            format);
#ifdef MDK_HW_PROFILE
    const uint64_t texT3 = svcGetSystemTick();
#endif
    C3D_TexLoadImage(&texture.tex, pixels, GPU_TEXFACE_2D, 0);
    // The chain, already in the same buffer after level 0 (the expansion built
    // it: pica::buildMipChain). Each level is handed to the sink from its offset:
    // the bytes of the levels above it IN THE STORE (2 a texel for a 16-bit one).
    int32_t made = 0;
    for (int32_t level = 1; level <= levels; level++) {
        const size_t offset = mdk::pica::textureBytes(format, texture.paddedWidth,
                                                      texture.paddedHeight, level - 1);
        C3D_TexLoadImage(&texture.tex, pixels + offset, GPU_TEXFACE_2D, level);
        made = level;
    }
#ifdef MDK_HW_PROFILE
    const uint64_t texT4 = svcGetSystemTick();
#endif
    texture.mipLevels = made;
    // C3D_TexFlush flushes level 0 plus `maxLevel` levels; the draw path rewrites
    // maxLevel per draw, so put back what the allocation holds first.
    texture.tex.maxLevel = (u8)levels;
    // THE PAIRED FLUSH. `C3D_TexLoadImage` copies the pixels into the texture's
    // own linear buffer and does NOT make them visible to the GPU. citro3d's
    // flush is a separate call, `C3D_TexFlush`, and that is measured rather than
    // remembered: in the linked `libcitro3dd.a`, `GSPGPU_FlushDataCache` is
    // referenced by exactly three objects (`renderqueue.o`, `texture.o`,
    // `tex3ds.o`), and the ONE call site in `texture.o` is inside
    // `C3D_TexFlush` - `C3D_TexLoadImage` has none (`arm-none-eabi-readelf -r
    // texture.o`: one `R_ARM_CALL GSPGPU_FlushDataCache`, at 0x40, in
    // `.rel.text.C3D_TexFlush`; `C3D_TexUpload` is an inline that calls
    // `C3D_TexLoadImage` and does not flush either).
    //
    // IT IS NOT WHAT MAKES P3 MET, AND THE OLD COMMENT SAID IT WAS (review-1
    // F1). `C3D_FrameEnd` in this citro3d flushes the WHOLE linear heap --
    // `tst r4,#2` then `ldr r0,__ctru_linear_heap; ldr r1,__ctru_linear_heap_size;
    // bl GSPGPU_FlushDataCache` (`arm-none-eabi-objdump -dr renderqueue.o`) --
    // and this file calls `C3D_FrameEnd(0)`, which is exactly AGENTS.md P3's
    // "flushes linear memory before queue execution". So the obligation is met
    // by the frame end whether or not this line is here. `C3D_TexFlush` stays
    // for the narrower and better reason: it is citro3d's OWN pairing for
    // `C3D_TexLoadImage`, the library's documented contract for a non-VRAM
    // texture, and using a library the way it says to be used is not redundant
    // work.
    //
    // IT IS NOT THE CANVAS BUG, AND THE CLAIM THAT IT WAS IS MEASURED DEAD
    // (2026-11-11). This flush was landed on the theory that the 8x8 white
    // texture - 256 bytes, which fits entirely in the 8 KB write-back D-cache and
    // never leaves it - was never reaching the GPU, so every fragment sampling it
    // hit alpha 0 and the frame's alpha test discarded it. Two measurements kill
    // that for every capture this loop has: Mandarine's PICA renderer reads the
    // same host memory the emulated CPU wrote, so there is no incoherence to
    // flush away, and the white texture demonstrably samples opaque - the level
    // redrawn with `Pass::Overlay`, this white texture and a green tint paints
    // 644 pixels of exactly (0,255,0) on the panel WITH THIS LINE ALREADY IN
    // PLACE. The canvas is still absent. What the white texture and the canvas
    // actually have in common turned out to be nothing: the earlier "everything
    // that failed sampled the white texture" generalisation was an artefact of
    // probes that each had a different second defect (quads built at clip z = 0,
    // which is this GPU's far plane, and so were clipped before rasterisation).
    // See docs/OPEN_ITEMS.md, "The 3DS canvas draws no pixels".
    C3D_TexFlush(&texture.tex);
#ifdef MDK_HW_PROFILE
    {
        const uint64_t texT5 = svcGetSystemTick();
        const double msTotal = (double)(texT5 - texT0) * 1000.0 / (double)SYSCLOCK_ARM11;
        if (msTotal > 50.0) {
            const double msPer = 1000.0 / (double)SYSCLOCK_ARM11;
            mdk::log3dsPrintf("texperf: %dx%d %s expand %.1f alloc %.1f pack %.1f load %.1f "
                              "flush %.1f ms",
                              (int)texture.realWidth, (int)texture.realHeight,
                              texture.indexed ? "indexed" : "rgba",
                              (double)(texT1 - texT0) * msPer, (double)(texT2 - texT1) * msPer,
                              (double)(texT3 - texT2) * msPer, (double)(texT4 - texT3) * msPer,
                              (double)(texT5 - texT4) * msPer);
        }
    }
#endif
    free(pixels);
    texture.valid = true;
    texture.format = format;
    renderer->texturesMade[texelStoreIndex(format)]++;
    renderer->textureBytesSaved +=
        mdk::pica::textureBytes(mdk::pica::TexelFormat::Rgba8, texture.paddedWidth,
                                texture.paddedHeight, levels) -
        gpuBytes;
    return true;
}


// ---------------------------------------------------------------------------
// THE HD STREAM (RendererC3D::hdCopies, docs/HD_TEXTURES_3DS_SPEC.md 4.2)
// ---------------------------------------------------------------------------

// Frees one HD copy's texture. Only where the GPU cannot be reading it (P3): the
// pump at the frame boundary (after C3D_FrameBegin's wait), or makeTexture's
// fallback for a copy older than this frame's first draw (this frame's commands
// are not submitted yet, and the last frame's finished at C3D_FrameBegin).
static void dropHd(RendererC3D* renderer, HdCopy& copy) {
    if (copy.state == HdCopy::Resident) {
        C3D_TexDelete(&copy.tex);
        renderer->hdResidentBytes -= copy.bytes;
    }
    copy.state = HdCopy::None;
    copy.bytes = 0;
    copy.levels = 0;
}

// The policy's view of the copies (pica::HdSlot), in a buffer reused across
// calls.
static void hdSlots(RendererC3D* renderer) {
    std::vector<mdk::pica::HdSlot>& slots = renderer->hdSlotScratch;
    slots.resize(renderer->hdCopies.size());
    for (size_t i = 0; i < renderer->hdCopies.size(); i++) {
        slots[i].resident = renderer->hdCopies[i].state == HdCopy::Resident;
        slots[i].lastUsed = renderer->hdCopies[i].lastUsed;
        slots[i].linked = renderer->hdCopies[i].links > 0;
    }
}

// Evicts the copy pica::hdEvictionVictim names (host-tested), or returns false.
static bool evictHd(RendererC3D* renderer, uint32_t protectFrom, bool unlinkedOnly) {
    hdSlots(renderer);
    const int32_t victim = mdk::pica::hdEvictionVictim(
        renderer->hdSlotScratch.data(), (int32_t)renderer->hdSlotScratch.size(), protectFrom,
        unlinkedOnly);
    if (victim < 0) {
        return false;
    }
    dropHd(renderer, renderer->hdCopies[(size_t)victim]);
    renderer->hdEvicted++;
    return true;
}

static bool hdHasEvictable(RendererC3D* renderer, uint32_t protectFrom) {
    hdSlots(renderer);
    return mdk::pica::hdEvictionVictim(renderer->hdSlotScratch.data(),
                                       (int32_t)renderer->hdSlotScratch.size(),
                                       protectFrom) >= 0;
}

// A read or import that failed: counted, logged, and retried after
// kHdRetryFrames up to kHdMaxFailures times (the draw re-queues it, F4).
static void hdFailed(RendererC3D* renderer, HdCopy& copy, const char* why) {
    copy.state = HdCopy::Failed;
    copy.failures++;
    copy.retryFrame = renderer->frame + kHdRetryFrames;
    renderer->hdFailedCount++;
    mdk::log3dsPrintf("hd: %s: %s (failure %d; %u resident, %u linear free)", why,
                      copy.path.c_str(), (int)copy.failures, (unsigned)renderer->hdResidentBytes,
                      (unsigned)linearSpaceFree());
}

// THE PUMP, at the frame boundary -- after C3D_FrameBegin has waited for the
// previous frame, so an eviction never frees what the GPU is reading (P3).
// Two halves: queued copies are handed to the reader thread (their SD reads
// happen there, off this thread); finished reads are imported, at most
// kHdImportsPerFrame per frame, within the HD budget. `protectFrom` is the first
// draw stamp of the frame that just ended: what it drew is not evicted.
static void pumpHdTextures(RendererC3D* renderer, uint32_t protectFrom) {
    // THE TOGGLE OFF: no reads, no imports, and the resident copies are freed
    // (this is a frame boundary, so none is in flight) -- off means the memory is
    // the original look's again (review F5). Turned back on, draws ask anew.
    if (!renderer->toggles.enableHdTextures) {
        if (renderer->hdResidentBytes > 0) {
            for (size_t i = 0; i < renderer->hdCopies.size(); i++) {
                dropHd(renderer, renderer->hdCopies[i]);
            }
        }
        return;
    }
    // (0) NOBODY'S COPIES. A copy no texture links to any more (a level ended, a
    // sprite frame was trimmed) is cancelled if it was only asked for (a read in
    // flight is dropped when it lands) and freed once it has stayed unlinked for
    // kHdUnlinkedFrames -- this is the frame boundary, and the frame that drew it
    // last has finished (C3D_FrameBegin waited). Under memory pressure it goes
    // first (evictHd's unlinked preference, makeTexture and the mesh allocator).
    for (size_t i = 0; i < renderer->hdCopies.size(); i++) {
        HdCopy& copy = renderer->hdCopies[i];
        if (copy.links > 0) {
            continue;
        }
        if (copy.state == HdCopy::Queued ||
            (copy.state == HdCopy::Resident &&
             renderer->frame - copy.unlinkedSince >= kHdUnlinkedFrames)) {
            dropHd(renderer, copy);
        }
    }
    // (1) Hand out reads while the reader has free slots -- and only while each
    // could be imported (pica::hdMayDispatch, host-tested). Otherwise the queue
    // waits (the view's working set fills the budget; the originals draw
    // meanwhile), and nothing is read from SD only to be thrown away (F3). The
    // evictable scan only runs when something waits.
    const bool evictable = !renderer->hdQueue.empty() && hdHasEvictable(renderer, protectFrom);
    size_t next = 0;
    for (; next < renderer->hdQueue.size(); next++) {
        const size_t reading =
            (size_t)renderer->hdReader.busy(mdk::AsyncFileReader::kChannelHd);
        if (!mdk::pica::hdMayDispatch(renderer->hdResidentBytes, reading, kHdLargestBytes,
                                      kHdBudgetBytes, evictable)) {
            break;
        }
        const int32_t id = renderer->hdQueue[next];
        if (id < 0 || (size_t)id >= renderer->hdCopies.size()) {
            continue;
        }
        HdCopy& copy = renderer->hdCopies[(size_t)id];
        if (copy.state != HdCopy::Queued) {
            continue;
        }
        if (!renderer->hdReader.request(id, copy.path.c_str())) {
            break;   // every slot is busy: this one waits at the queue's head
        }
        copy.state = HdCopy::Reading;
    }
    renderer->hdQueue.erase(renderer->hdQueue.begin(),
                            renderer->hdQueue.begin() + (long)next);

    // (2) Import finished reads.
    int32_t imported = 0;
    int32_t id = -1;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    bool ok = false;
    char path[mdk::AsyncFileReader::kPathMax];
    while (imported < kHdImportsPerFrame &&
           renderer->hdReader.poll(mdk::AsyncFileReader::kChannelHd, &id, path, &bytes, &size,
                                   &ok)) {
        if (id < 0 || (size_t)id >= renderer->hdCopies.size()) {
            free(bytes);
            continue;
        }
        HdCopy& copy = renderer->hdCopies[(size_t)id];
        if (copy.state != HdCopy::Reading || copy.path != path || copy.links <= 0) {
            // Dropped (the toggle went off) or nobody's (unlinked) while it was
            // being read: not imported.
            if (copy.state == HdCopy::Reading) {
                copy.state = HdCopy::None;
            }
            free(bytes);
            continue;
        }
        if (!ok) {
            hdFailed(renderer, copy, "cannot read");
            continue;
        }
        // The file is the chain plus a small header, so its size is the budget's
        // measure before the import; the exact bytes are booked after it.
        bool room = true;
        while (renderer->hdResidentBytes + size > kHdBudgetBytes) {
            if (!evictHd(renderer, protectFrom, false)) {
                room = false;
                break;
            }
        }
        if (!room) {
            // Everything resident was drawn last frame: the budget is the view's
            // working set. Back to the queue; it is asked for again later.
            free(bytes);
            copy.state = HdCopy::Queued;
            renderer->hdQueue.push_back(id);
            continue;
        }
        // THE IMPORT: an LZ decompress and a copy into the linear heap, the ETC
        // blocks already tiled on the host -- at the frame boundary, after
        // C3D_FrameBegin's wait (P3), at most kHdImportsPerFrame a frame. This
        // is the HD pack's copy landing on its expansion (the profile timing
        // below, hdImportTicks, counts it; the file read ran on the reader
        // thread, off the frame). The ORIG pack's textures do NOT come through
        // here: they are made from their .t3x at creation
        // (mdk_render_create_compressed_file), and that import runs through
        // importCompressed below, timed as texture-make work. No sampler state
        // is set here: 06_draw.inc rewrites filter and maxLevel per draw, so a
        // full mip chain holds at NEAREST level 0 in the original look; only
        // the wrap, which the draw does not rewrite, is set below from the
        // image's own rectangle.
        forgetTextureBinds(renderer);   // texel memory refilled (THE DEDUPE)
#ifdef MDK_HW_PROFILE
        const uint64_t importStart = svcGetSystemTick();
        Tex3DS_Texture t3x = Tex3DS_TextureImport(bytes, size, &copy.tex, nullptr, false);
        if (!t3x && evictHd(renderer, protectFrom, false)) {
            t3x = Tex3DS_TextureImport(bytes, size, &copy.tex, nullptr, false);
        }
        renderer->hdImportTicks += svcGetSystemTick() - importStart;
#else
        Tex3DS_Texture t3x = Tex3DS_TextureImport(bytes, size, &copy.tex, nullptr, false);
        if (!t3x && evictHd(renderer, protectFrom, false)) {
            t3x = Tex3DS_TextureImport(bytes, size, &copy.tex, nullptr, false);
        }
#endif
        free(bytes);
        if (!t3x) {
            hdFailed(renderer, copy, "import FAILED");
            continue;
        }
        // The image's rectangle. hd_pack_convert.py makes one unrotated image per
        // file; a rotated one (top < bottom) would need its axes swapped in the
        // shader's uv transform, so it is refused rather than drawn sideways (F7).
        const Tex3DS_SubTexture* sub = Tex3DS_GetSubTexture(t3x, 0);
        if (!sub || Tex3DS_SubTextureRotated(sub)) {
            Tex3DS_TextureFree(t3x);
            C3D_TexDelete(&copy.tex);
            copy.failures = kHdMaxFailures;   // a property of the file: no retry
            copy.broken = true;
            hdFailed(renderer, copy, sub ? "rotated subtexture" : "no subtexture");
            continue;
        }
        copy.left = sub->left;
        copy.top = sub->top;
        copy.right = sub->right;
        copy.bottom = sub->bottom;
        Tex3DS_TextureFree(t3x);
        copy.levels = copy.tex.maxLevel;
        copy.bytes = (uint32_t)C3D_TexCalcTotalSize(copy.tex.size, copy.tex.maxLevel);
        // REPEAT on an axis the image fills (a power-of-two image: exact), CLAMP
        // on one tex3ds padded (an image under 8 texels sits in an 8-texel
        // texture, and REPEAT would wrap through the padding) (F7).
        const bool fullU = copy.left == 0.0f && copy.right == 1.0f;
        const bool fullV = copy.top == 1.0f && copy.bottom == 0.0f;
        C3D_TexSetWrap(&copy.tex, fullU ? GPU_REPEAT : GPU_CLAMP_TO_EDGE,
                       fullV ? GPU_REPEAT : GPU_CLAMP_TO_EDGE);
        renderer->hdResidentBytes += copy.bytes;
        copy.state = HdCopy::Resident;
        copy.lastUsed = protectFrom;   // fresh: not the next eviction's victim
        renderer->hdImported++;
        imported++;
        if (renderer->hdImported <= 3 || (renderer->hdImported % 50) == 0) {
            mdk::log3dsPrintf("hd: imported %s %ux%u levels=%d %u bytes (uv %.2f,%.2f-%.2f,%.2f); "
                              "%d imported, %u bytes resident, %d evicted, %d failed",
                              copy.path.c_str(), (unsigned)copy.tex.width,
                              (unsigned)copy.tex.height, (int)copy.levels, (unsigned)copy.bytes,
                              copy.left, copy.top, copy.right, copy.bottom,
                              (int)renderer->hdImported, (unsigned)renderer->hdResidentBytes,
                              (int)renderer->hdEvicted, (int)renderer->hdFailedCount);
        }
    }
}
