// 04_expansion.inc -- the chunked palette expansion: a large texture's make, one step a frame.
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

// ---------------------------------------------------------------------------
// THE CHUNKED EXPANSION (HANDOFF_MUSE.md task 2). A large indexed texture's
// make measured 64x704: expand 41.9 ms + pack 18.4 ms + alloc/load ~5 ms in
// one frame (frame 795's 77 ms texmake). The pump cannot reach that batch in
// the pair window, so the make itself is sliced: rows in slices, then the
// filter and mip passes in rows of tiles (pica::prepareForFilteringStep,
// buildMipChainStep), then the 16-bit pack in texel slices, then the ordinary
// upload (makeTextureNow consuming the finished buffer as a preset). The filter
// and mip passes were single steps until 2026-11-24, and on the Old 3DS each was
// a 100-220 ms frame by itself: level start (a level-7 run's `begin` span, which
// holds this pump, up to 203 ms), every main-menu open, and the 64x704 corridor
// strip made again mid-play. The frame-boundary pump advances every pending
// expansion ONE step a frame; a draw that needs the texture first finishes
// it synchronously, so no frame ever binds a half-made texture and the
// picture is unchanged -- only its timing is.
// ---------------------------------------------------------------------------
static const size_t kChunkMinTexels = 32768u;   // below this a make is < ~20 ms: synchronous
static const int32_t kChunkRowTexels = 16384;   // one row step's texel budget (64 wide: 256 rows)
static const size_t kChunkPackTexels = 16384u;  // 64x1024 chain: 4 pack steps
// One filter or mip step's work, in texels: the row step's budget. A bleed row
// of tiles costs its real width times 8 (a 1024-wide sky: two rows a step), a
// downsample row of tiles the texels it writes, the clamp its width plus height.
static const size_t kChunkFilterTexels = 16384u;
static const int32_t kPumpStepsPerFrame = 3;    // total steps the frame pump does, all textures

static void dropExpansion(C3dTexture& texture) {
    free(texture.expandPixels);
    texture.expandPixels = nullptr;
    texture.expandPending = false;
    texture.expandWanted = false;
    texture.expandRow = 0;
    texture.expandStage = 0;
    texture.expandFilter = mdk::pica::FilterCursor();
    texture.expandPackAt = 0;
    texture.expandSawTransparent = false;
    texture.expandLevels = 0;
    texture.expandFormat = mdk::pica::TexelFormat::Rgba8;
    texture.stalePending = false;
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
    texture.expandLevels = renderer->toggles.enableMipmaps && !texture.noMips
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
    texture.expandFilter = mdk::pica::FilterCursor();
    texture.expandSawTransparent = false;
    texture.expandPackAt = 0;
    texture.expandPending = true;
    return 1;
}

// One step (or all of them, when a draw needs the texture now). Returns
// false only when the finishing upload fails; the state is then clean and
// a later make retries synchronously from the kept source. The levels are
// frozen at the start (the chain buffer is sized by them) and the store is
// chosen at stage 3, so an enableMipmaps flip mid-expansion reaches this
// texture only at its next making; the texture16 store follows the flip.
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
            case 1: {
                const size_t budget = finishAll ? SIZE_MAX : kChunkFilterTexels;
                if (mdk::pica::prepareForFilteringStep(
                        texture.expandPixels, texture.realWidth, texture.realHeight,
                        texture.paddedWidth, texture.paddedHeight, texture.expandSawTransparent,
                        texture.expandFilter, budget)) {
                    texture.expandFilter = mdk::pica::FilterCursor();
                    texture.expandStage = 2;
                }
                break;
            }
            case 2: {
                const size_t budget = finishAll ? SIZE_MAX : kChunkFilterTexels;
                if (mdk::pica::buildMipChainStep(texture.expandPixels, texture.paddedWidth,
                                                 texture.paddedHeight, texture.realWidth,
                                                 texture.realHeight, texture.expandLevels,
                                                 texture.expandFilter, budget)) {
                    texture.expandFilter = mdk::pica::FilterCursor();
                    texture.expandStage = 3;
                }
                break;
            }
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
                if (texture.stalePending) {
                    // The old copy drew until now (a frame boundary, so
                    // the GPU is done with it): swap it for the new one.
                    texture.stalePending = false;
                    if (texture.valid) {
                        C3D_TexDelete(&texture.tex);
                        texture.valid = false;
                    }
                }
                const bool made = makeTextureNow(renderer, texture, false);
                if (!made) {
                    dropExpansion(texture);
                }
                return made;
            }
        }
        if (!finishAll) {
            return true;
        }
    }
}

static bool advanceExpansion(RendererC3D* renderer, C3dTexture& texture, bool finishAll) {
    return advanceExpansionSteps(renderer, texture, finishAll);
}

// The stale re-make's chunked start (03_frame_state.inc's settle): same
// candidate rule as a first make, forced past the pending cap -- the old
// copy is already deleted, so deferring the start would leave the surface
// untextured until a draw finishes it whole anyway.
static bool startStaleExpansion(RendererC3D* renderer, C3dTexture& texture) {
    if (!texture.indexed || texture.compressed || texture.streamed ||
        texture.realWidth <= 0 || texture.realHeight <= 0 ||
        (size_t)texture.realWidth * (size_t)texture.realHeight < kChunkMinTexels) {
        return false;
    }
    return startExpansion(renderer, texture, true) > 0;
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
    return makeTextureNow(renderer, texture, diagnostics);
}

