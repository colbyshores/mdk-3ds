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
        if (!texture.valid && !texture.releasePending && texture.source.empty()) {
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
    for (int32_t y = 0; y < height; y++) {
        const uint8_t* source = indices + (size_t)y * (size_t)width;
        for (int32_t x = 0; x < width; x++) {
            const uint8_t index = source[x];
            const size_t slot =
                mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth);
            uint8_t* pixel = pixels + slot * 4u;
            if (index == 0) {
                // Transparent: the colour is irrelevant, but it must not be
                // black-with-alpha-255, which is what the palette says.
                continue;
            }
            mdk::pica::texelBytes(pixel, paletteRgba[index * 4 + 0],
                                  paletteRgba[index * 4 + 1], paletteRgba[index * 4 + 2], 255u);
        }
    }
    // Ready for Enhanced Mode filtering whichever way its toggle stands now: the
    // toggle is live, the texture is made once (pica::prepareForFiltering; a
    // nearest sample of any texel it touches is unchanged in colour or
    // discarded as before, except at an exact padded edge, which now clamps).
    mdk::pica::prepareForFiltering(pixels, width, height, paddedWidth, paddedHeight);
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
    for (int32_t y = 0; y < height; y++) {
        const uint8_t* row = source + (size_t)y * (size_t)width * 4;
        for (int32_t x = 0; x < width; x++) {
            const size_t slot =
                mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth);
            mdk::pica::texelBytes(pixels + slot * 4u, row[(size_t)x * 4 + 0],
                                  row[(size_t)x * 4 + 1], row[(size_t)x * 4 + 2],
                                  row[(size_t)x * 4 + 3]);
        }
    }
    mdk::pica::prepareForFiltering(pixels, width, height, paddedWidth, paddedHeight);
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
static bool makeTexture(RendererC3D* renderer, C3dTexture& texture, bool diagnostics) {
    if (texture.valid) {
        return true;
    }
    // THE MIP CHAIN (SPEC.md 7.9 enableMipmaps): made with the texture when the
    // toggle is on at that moment, so turning it on costs its memory -- a third
    // more per texture -- from the next texture made (the next level load, or an
    // evicted texture coming back); the sampler's use of it is live (06_draw.inc).
    int32_t levels = renderer->toggles.enableMipmaps
                         ? mdk::pica::mipLevelsFor(texture.paddedWidth, texture.paddedHeight)
                         : 0;
    size_t gpuBytes = mdk::pica::textureBytes(mdk::pica::TexelFormat::Rgba8,
                                                    texture.paddedWidth, texture.paddedHeight,
                                                    levels);
    const size_t realBytes = (size_t)texture.realWidth * (size_t)texture.realHeight *
                             (texture.indexed ? 1u : 4u);
    if (texture.source.size() != realBytes ||
        (texture.indexed && texture.palette.size() < 256 * 4)) {
        mdk::log3dsPrintf("texture %dx%d source is invalid",
                          (int)texture.realWidth, (int)texture.realHeight);
        return false;
    }
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
        C3D_TexInitParams params = {};
        params.width = (u16)texture.paddedWidth;
        params.height = (u16)texture.paddedHeight;
        params.maxLevel = (u8)levels;
        params.format = GPU_RGBA8;
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
        if (!evictTextures(renderer, gpuBytes)) {
            // A CHAIN IS AN ENHANCEMENT, THE TEXTURE IS NOT: with nothing left to
            // evict, try once more without the chain before the surface goes
            // missing (review of the mipmap batch, F2).
            if (levels > 0) {
                mdk::log3dsPrintf("texture %dx%d: no room for its mip chain; made without",
                                  (int)texture.realWidth, (int)texture.realHeight);
                levels = 0;
                gpuBytes = mdk::pica::textureBytes(mdk::pica::TexelFormat::Rgba8,
                                                   texture.paddedWidth, texture.paddedHeight, 0);
                continue;
            }
            break;
        }
    }
    if (!allocated) {
        mdk::log3dsPrintf("texture %dx%d FAILED: no texture memory",
                          (int)texture.realWidth, (int)texture.realHeight);
        return false;
    }

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

    // THE PIXELS. A PLAIN CPU BUFFER, NOT linearAlloc.
    //
    // The linear heap STARTS IN THE VRAM: its first megabytes are texture
    // memory, and only then does it extend into system RAM. So a
    // temporary buffer taken from the linear heap PERMANENTLY EATS VRAM --
    // and this one is a temporary: it is copied into the texture and thrown
    // away. Every expansion was therefore costing the 3DS its texture
    // memory, and the arena's textures stopped allocating one after
    // another.
    uint8_t* pixels = nullptr;
    if (texture.indexed) {
        pixels = expandIndices(texture.source.data(), texture.realWidth, texture.realHeight,
                               texture.palette.data(), texture.paddedWidth, texture.paddedHeight,
                               levels);
    } else {
        pixels = expandRgba(texture.source.data(), texture.realWidth, texture.realHeight,
                            texture.paddedWidth, texture.paddedHeight, levels);
    }
    if (!pixels) {
        C3D_TexDelete(&texture.tex);
        return false;
    }
    C3D_TexLoadImage(&texture.tex, pixels, GPU_TEXFACE_2D, 0);
    // The chain, already in the same buffer after level 0 (the expansion built
    // it: pica::buildMipChain). Each level is handed to the sink from its offset.
    size_t offset = (size_t)texture.paddedWidth * (size_t)texture.paddedHeight * 4u;
    int32_t made = 0;
    for (int32_t level = 1; level <= levels; level++) {
        C3D_TexLoadImage(&texture.tex, pixels + offset, GPU_TEXFACE_2D, level);
        offset += ((size_t)texture.paddedWidth >> level) * ((size_t)texture.paddedHeight >> level) * 4u;
        made = level;
    }
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
    free(pixels);
    texture.valid = true;
    return true;
}

