// 04_make.inc -- the orig pack's reads, the HD rule, and the synchronous make (makeTextureNow).
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

// ---------------------------------------------------------------------------
// THE ORIG PACK'S READS (C3dTexture::packPath). A packed original is created
// with no I/O (mdk_render_create_compressed_file): it is QUEUED, its file read
// on the reader thread (kChannelOrig), and the bytes imported at a frame
// boundary under kPackImportsPerFrame (pumpPackOriginals, in
// 04_streams.inc). Only a draw that cannot wait reads on this thread (finishPackNow).
// ---------------------------------------------------------------------------

// The whole of a file, on the calling thread (05_resources.inc): the last
// resort's read below, and nothing else.
static bool readWholeFile(const char* path, std::vector<uint8_t>* out);

// THE HD RULE (docs/HD_TEXTURES_3DS_SPEC.md 3.1): whether this texture is drawn
// from its HD copy and nothing else right now -- made from the packs' files
// (C3dTexture::hdOnly), the toggle on, and the copy not given up on (a file
// that failed for good, or past its retries, leaves the texture to its packed
// original: an unusable HD file must not leave a surface blank for the level).
static bool drawsHdOnly(const RendererC3D* renderer, const C3dTexture& texture) {
    if (!texture.hdOnly || !renderer->toggles.enableHdTextures || texture.hdCopy < 0 ||
        (size_t)texture.hdCopy >= renderer->hdCopies.size()) {
        return false;
    }
    const HdCopy& copy = renderer->hdCopies[(size_t)texture.hdCopy];
    // With the level's set resident (spec 3.2) only a copy of the set counts.
    if (renderer->hdPinnedMode && !copy.pinned) {
        return false;
    }
    return !(copy.state == HdCopy::Failed && (copy.broken || copy.failures >= kHdMaxFailures));
}

// Asks for an HD copy: queued for the boundary pump (pumpHdTextures) -- once
// per wait, and a failed one again only after its pause (review F4).
static void requestHdCopy(RendererC3D* renderer, int32_t index) {
    if (index < 0 || (size_t)index >= renderer->hdCopies.size()) {
        return;
    }
    HdCopy& copy = renderer->hdCopies[(size_t)index];
    if (copy.state == HdCopy::None ||
        (copy.state == HdCopy::Failed && copy.failures < kHdMaxFailures &&
         renderer->frame >= copy.retryFrame)) {
        copy.state = HdCopy::Queued;
        renderer->hdQueue.push_back(index);
    }
}

// Asks for a packed original's file: queued for the boundary pump. Only an
// idle one -- made textures are idle too, so the caller checks `valid` -- and
// once per wait (a queued or reading one is already asked for, a failed one
// waits for its index pixels).
//
// AN HD-ONLY TEXTURE ASKS FOR ITS HD COPY INSTEAD (drawsHdOnly): every ask --
// the creation's, a draw's, the pump's re-ask of an evicted one, a hint's --
// comes through here, so none of them reads the original while the HD copy is
// the texture.
static void queuePackRead(RendererC3D* renderer, int32_t id) {
    C3dTexture& texture = renderer->textures[(size_t)id];
    if (drawsHdOnly(renderer, texture)) {
        requestHdCopy(renderer, texture.hdCopy);
        return;
    }
    if (texture.packPath.empty() || texture.packState != C3dTexture::PackIdle) {
        return;
    }
    texture.packState = C3dTexture::PackQueued;
    renderer->packQueue.push_back(id);
}

// A packed original that cannot be made from its file: unreadable, or not this
// set's .t3x. Not retried -- a property of the file -- and the core is told
// (mdk_render_texture_needs_pixels) so it hands the index pixels over and the
// texture draws the ordinary way. Counted and logged, once per texture.
static void markPackFailed(RendererC3D* renderer, C3dTexture& texture, const char* why) {
    texture.packState = C3dTexture::PackFailed;
    renderer->packFailed++;
    mdk::log3dsPrintf("pack: %s: %s (%d failed)", why, texture.packPath.c_str(),
                      (int)renderer->packFailed);
}

// Whether a draw may make this packed original now (drawRecorded's revive,
// finishPackNow): not failed, and not already found without room this frame --
// a later record or the other eye must not read the same file again.
static bool packMakeableNow(const RendererC3D* renderer, const C3dTexture& texture) {
    return !texture.packPath.empty() && texture.packState != C3dTexture::PackFailed &&
           texture.packNoRoomFrame != renderer->frame;
}

// THE LAST RESORT (makeTextureNow, from drawRecorded's revive): a packed
// original whose batch is IN THIS EYE'S FRUSTUM and whose GPU copy the pump has
// not made. A read that has landed is taken by its tag; otherwise the read is
// dropped -- so it can never land on the copy made here -- and the file is read
// HERE, on the main thread, inside the frame. Never capped: a cap turns a slow
// surface into a missing one, and a texture behind the camera never gets here
// (the cull runs first), which already makes this cheaper than the old
// make-everything-at-prepare. The import keeps this frame's draws
// (frameStartUse), as every mid-frame make's evictions do. A failure draws
// nothing this frame, and nothing later this frame reads the file again.
static bool finishPackNow(RendererC3D* renderer, C3dTexture& texture) {
    // Never for a texture its HD copy stands for (the HD rule): no draw path
    // asks, and this is the one place a read would happen on the main thread.
    if (!packMakeableNow(renderer, texture) || drawsHdOnly(renderer, texture)) {
        return false;
    }
    const int32_t id = (int32_t)(&texture - renderer->textures.data());
    uint8_t* landedBytes = nullptr;
    size_t landedSize = 0;
    bool ok = false;
    const bool landed = texture.packState == C3dTexture::PackReading &&
                        renderer->hdReader.pollTag(mdk::AsyncFileReader::kChannelOrig, id,
                                                   &landedBytes, &landedSize, &ok);
    std::vector<uint8_t> read;
    if (!landed) {
        renderer->hdReader.discard(mdk::AsyncFileReader::kChannelOrig, id);
        const u64 t0 = svcGetSystemTick();
        ok = readWholeFile(texture.packPath.c_str(), &read);
        const double ms = (double)(svcGetSystemTick() - t0) / (double)(SYSCLOCK_ARM11 / 1000ull);
        renderer->packMainReads++;
        mdk::log3dsPrintf("pack: MAIN-THREAD read %s (%u bytes, %.1f ms, %s; %d this session)",
                          texture.packPath.c_str(), (unsigned)read.size(), ms,
                          texture.packState == C3dTexture::PackReading ? "its read in flight"
                                                                       : "not yet asked for",
                          (int)renderer->packMainReads);
    }
    bool made = false;
    bool refused = false;
    if (ok) {
        const uint8_t* bytes = landed ? landedBytes : read.data();
        const size_t size = landed ? landedSize : read.size();
        made = importCompressed(renderer, texture, bytes, size, renderer->frameStartUse, true,
                                &refused);
    }
    free(landedBytes);
    if (made) {
        texture.packState = C3dTexture::PackIdle;
    } else if (!ok || refused) {
        markPackFailed(renderer, texture, ok ? "not an ETC .t3x of that size" : "cannot read");
    } else {
        // No room even after the evictions: idle, asked for again by the next
        // frame's draw (prepareDraw queues it) -- not by this frame's.
        texture.packState = C3dTexture::PackIdle;
        texture.packNoRoomFrame = renderer->frame;
    }
    return made;
}

static bool makeTextureNow(RendererC3D* renderer, C3dTexture& texture, bool diagnostics) {
    // Texel memory is about to be (re)filled, possibly at an address a bound
    // texture had: no unit's bind may be skipped on the strength of the old
    // contents (03_frame_state.inc, THE DEDUPE).
    forgetTextureBinds(renderer);
    if (texture.compressed) {
        // Kurt's frames import the bytes they keep; a packed original has only
        // its file, and a draw that reaches here cannot wait for the reader.
        return texture.packPath.empty()
                   ? importCompressed(renderer, texture, texture.source.data(),
                                      texture.source.size(), renderer->frameStartUse, true,
                                      nullptr)
                   : finishPackNow(renderer, texture);
    }
    if (texture.streamed) {
        return makeStreamed(renderer, texture);
    }
    // THE MIP CHAIN (SPEC.md 7.9 enableMipmaps): made with the texture when the
    // toggle is on at that moment, so turning it on costs its memory -- a third
    // more per texture -- from the next texture made (the next level load, or an
    // evicted texture coming back); the sampler's use of it is live (06_draw.inc).
    int32_t levels = renderer->toggles.enableMipmaps && !texture.noMips
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
        if (!evictTextures(renderer, renderer->frameStartUse)) {
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
    // and this renderer calls `C3D_FrameEnd(0)`, which is exactly AGENTS.md P3's
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
    texture.format = format;
    renderer->texturesMade[texelStoreIndex(format)]++;
    renderer->textureBytesSaved +=
        mdk::pica::textureBytes(mdk::pica::TexelFormat::Rgba8, texture.paddedWidth,
                                texture.paddedHeight, levels) -
        gpuBytes;
    return true;
}


