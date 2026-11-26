// 04_textures.inc -- texture memory: eviction, slots, the palette expansion helpers, the compressed and streamed imports.
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
// eviction gives the VRAM back without making the id a dead end. (A packed
// original keeps its FILE -- packPath, PackIdle -- and is read again.)
//
// `protectFrom`: inside a frame, a texture stamped at or after it is kept. The
// makes pass the frame's own first stamp (frameStartUse: this frame's commands
// read it); the orig pack's boundary import passes the PREVIOUS frame's, so a
// texture a draw asked for never evicts what the player just saw (and one no
// draw asked for evicts nothing at all, pumpPackOriginals).
//
// Returns false when there is no texture that may be freed.
static bool evictTextures(RendererC3D* renderer, uint32_t protectFrom) {
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
        if (renderer->inFrame && texture.lastUsed >= protectFrom) {
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
    return true;
}

// A free slot in the texture table, or a new one.
//
// The ids the seam hands out are indices into the table, and a caller that
// releases a texture and makes it again -- the sprite cache does exactly
// that when its residency budget says a frame has to go -- must not grow
// the table for ever. A slot with a source is NOT free: it is an evicted
// texture that can be made again, and its source is the only copy there is.
// Nor is a slot with a PACK PATH: a packed original keeps no source, and one
// evicted or still being read is exactly `!valid` with an empty source -- the
// caller holds its id, and handing the slot out again would give one id two
// textures.
static int32_t acquireTextureSlot(RendererC3D* renderer) {
    for (size_t i = 0; i < renderer->textures.size(); i++) {
        const C3dTexture& texture = renderer->textures[i];
        // A pending chunked expansion owns its slot and its buffer until it
        // finishes or is dropped: never hand the slot to a new texture.
        if (!texture.valid && !texture.releasePending && texture.source.empty() &&
            texture.packPath.empty() && texture.pages.empty() &&
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
    // Transparent texels need no write (the memset left the slot all-zero,
    // and black-with-alpha-255 is what the palette would wrongly give).
    const bool sawTransparent =
        expandIndexRows(indices, width, 0, height, paletteRgba, paddedWidth, pixels);
    // Ready for Enhanced Mode filtering whichever way its toggle stands now: the
    // toggle is live, the texture is made once (pica::prepareForFiltering; a
    // nearest sample of any texel it touches is unchanged in colour or
    // discarded as before, except at an exact padded edge, which now clamps).
    mdk::pica::prepareForFiltering(pixels, width, height, paddedWidth, paddedHeight,
                                   sawTransparent);
    mdk::pica::buildMipChain(pixels, paddedWidth, paddedHeight, width, height, mipLevels);
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
// import of a .t3x's bytes -- Kurt's kept in `source`, a packed original's just
// read from its file -- an LZ decompress (none for the uncompressed orig pack)
// and a copy into the linear heap, the ETC blocks already tiled by tex3ds on
// the host. Nothing is expanded, which is the point: a Kurt frame made from its palette indices was a
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
// recently used original, then any HD copy -- each kept from `protectFrom` on
// (evictTextures) -- unless `mayEvict` is false (a packed original no draw asked
// for: free memory or nothing). `refused` (may be null) is set when the bytes
// are not this set's .t3x: a property of the file, where a false without it is a
// full heap.
static bool importCompressed(RendererC3D* renderer, C3dTexture& texture, const uint8_t* bytes,
                             size_t size, uint32_t protectFrom, bool mayEvict, bool* refused) {
    if (refused) {
        *refused = false;
    }
    // THE HEADER FIRST, so a file that is not this set's never reaches the
    // eviction ladder below: an import that fails for a broken file would
    // otherwise evict every texture it could before giving up. tex3ds' header:
    // u16 subtextures, u8 log2(width) - 3 | (log2(height) - 3) << 3, u8 format.
    // Only ETC1/ETC1A4 (GPU_ETC1 = 12, GPU_ETC1A4 = 13): the compressed seam's
    // contract, and the formats `format` below can name.
    const uint8_t* header = bytes;
    const bool headerOk =
        bytes && size > 6 && (header[0] | (header[1] << 8)) >= 1 &&
        (header[3] == (uint8_t)GPU_ETC1 || header[3] == (uint8_t)GPU_ETC1A4) &&
        (8 << (header[2] & 7)) >= texture.realWidth &&
        (8 << ((header[2] >> 3) & 7)) >= texture.realHeight;
    if (!headerOk) {
        mdk::log3dsPrintf("texture %dx%d: not an ETC .t3x of that size (%u bytes), refused",
                          (int)texture.realWidth, (int)texture.realHeight, (unsigned)size);
        if (refused) {
            *refused = true;
        }
        return false;
    }
    Tex3DS_Texture t3x = nullptr;
    for (;;) {
        t3x = Tex3DS_TextureImport(bytes, size, &texture.tex, nullptr, false);
        if (t3x) {
            break;
        }
        if (mayEvict && (evictHd(renderer, protectFrom, true) ||
                         evictTextures(renderer, protectFrom) ||
                         evictHd(renderer, protectFrom, false))) {
            continue;
        }
        // A hint's failure is not logged: it is an expected "no room" in a heap
        // the free-space check called big enough (a total, not a contiguous
        // block), and the line is an SD write at the boundary, again every time
        // the hint is re-asked. The pump leaves it idle; nothing is counted.
        if (mayEvict) {
            mdk::log3dsPrintf("texture %dx%d: compressed import FAILED (%u bytes)",
                              (int)texture.realWidth, (int)texture.realHeight, (unsigned)size);
        }
        return false;
    }
    Tex3DS_TextureFree(t3x);
    // THE IMPORT'S CACHE CLEAN, WHOLE CHAIN. Tex3DS decompresses a linear-heap
    // texture straight into tex->data and cleans nothing (the linked tex3ds.o:
    // its one GSPGPU_FlushDataCache is the VRAM path's staging buffer), so the
    // chain is in the CPU's write-back cache until C3D_FrameEnd's heap-wide flush.
    // Cleaned here by range, as makeTextureNow does for an expanded texture, while
    // maxLevel is still the import's (the draw rewrites it). See
    // docs/WORK_LEDGER.md, the mip-chain entry: the candidate for the New 3DS's
    // distant-surface glitch, unproven until the owner's A/B.
    C3D_TexFlush(&texture.tex);
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
        if (!evictTextures(renderer, renderer->frameStartUse) ||
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

