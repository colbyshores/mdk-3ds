// 06_glow.inc -- the glow bloom pass (enableGlowBillboards with glowBloom): its quad, its state, the pass.
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

// The glow quad: two triangles, the whole canvas. The level-pass quad: the same
// two triangles over a whole texture level (pica::glowLevelCorner).
static constexpr int kGlowQuadCorners = 6;

// THE GLOW'S MEMORY, made on first use (linear heap; the VRAM is the eye
// targets'). False when any part could not be made: the glow is then off for the
// session, with one refusal line.
static bool ensureGlow(RendererC3D* r) {
    // The refusal first: a failed set-up leaves its textures for releaseGlow
    // (which runs outside a frame), so glowReady may be true with nothing usable.
    if (r->glowRefused) {
        return false;
    }
    if (r->glowReady) {
        return true;
    }
    const size_t quadBytes = kGlowQuadCorners * 5 * sizeof(float);
    if (!r->glowQuad) {
        r->glowQuad = static_cast<float*>(linearAlloc(quadBytes));
        r->glowQuadWidth = 0.0f;
    }
    if (!r->glowLevelQuad) {
        r->glowLevelQuad = static_cast<float*>(linearAlloc(quadBytes));
    }
    C3D_TexInitParams params;
    params.width = (u16)mdk::pica::kGlowTexWidth;
    params.height = (u16)mdk::pica::kGlowTexHeight;
    params.maxLevel = 0;
    params.format = GPU_RGBA8;
    params.type = GPU_TEX_2D;
    // IN VRAM: citro3d renders only into VRAM (C3D_RenderTargetCreateFromTex
    // refuses a texture outside it -- measured: a linear-heap texture's targets
    // came back null), and every blur level is rendered into.
    params.onVram = true;
    bool ok = r->glowQuad && r->glowLevelQuad &&
              C3D_TexInitWithParams(&r->glowTex, nullptr, params);
    r->glowReady = ok;   // from here releaseGlow frees what was made
    for (int i = 0; ok && i < mdk::pica::kGlowSmallCount; i++) {
        const int level = (i == mdk::pica::kGlowSmallMix) ? mdk::pica::kGlowMidLevel : i + 1;
        C3D_TexInitParams small = params;
        small.width = (u16)(mdk::pica::kGlowTexWidth >> level);
        small.height = (u16)(mdk::pica::kGlowTexHeight >> level);
        r->glowSmallMade[i] = C3D_TexInitWithParams(&r->glowSmall[i], nullptr, small);
        ok = r->glowSmallMade[i];
        if (ok) {
            C3D_TexSetFilter(&r->glowSmall[i], GPU_LINEAR, GPU_LINEAR);
            C3D_TexSetWrap(&r->glowSmall[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            r->glowTarget[i] = C3D_RenderTargetCreateFromTex(&r->glowSmall[i], GPU_TEXFACE_2D, 0, -1);
            ok = r->glowTarget[i] != nullptr;
        }
    }
    if (!ok) {
        // Inside a frame, where C3D_RenderTargetDelete breaks: what was made waits
        // for releaseGlow (the next eye-target rebuild, or the destructor).
        mdk::log3dsPrintf("glow: texture/target/quad allocation FAILED; glow off (vram free %u)",
                          (unsigned)vramSpaceFree());
        r->glowRefused = true;
        return false;
    }
    // Black outside the picture: the copy fills only the 240x400 corner of the
    // picture texture, and the averaging carries the rest in (the glow only ever
    // adds, so the edges lose a little shine and gain nothing). A GPU fill,
    // queued ahead of this frame's first copy -- the CPU does not write VRAM.
    u32* picture = static_cast<u32*>(r->glowTex.data);
    GX_MemoryFill(picture, 0, picture + r->glowTex.size / 4,
                  GX_FILL_32BIT_DEPTH | GX_FILL_TRIGGER, nullptr, 0, nullptr, 0);
    C3D_TexSetFilter(&r->glowTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&r->glowTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    // The small-pass quad: the identity over a whole target.
    const float corner[kGlowQuadCorners][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
    for (int i = 0; i < kGlowQuadCorners; i++) {
        float* v = r->glowLevelQuad + i * 5;
        mdk::pica::glowLevelCorner(corner[i][0], corner[i][1], &v[0], &v[1]);
        v[2] = -0.5f;   // inside this GPU's [-w, 0] clip z (pica::canvasMatrix)
        v[3] = corner[i][0];
        v[4] = corner[i][1];
    }
    GSPGPU_FlushDataCache(r->glowLevelQuad, quadBytes);
    mdk::log3dsPrintf("glow: picture 256x512 + blur levels 1-5 + mix (VRAM, %u free)",
                      (unsigned)vramSpaceFree());
    return true;
}

// The state every glow draw shares: the scene program unlit, texture unit 0 on
// `view`, stage 0 = that texture (alpha 1, so the alpha test passes), the depth
// test off, no cull. Stages 1-5 are left to the caller.
static void glowDrawState(RendererC3D* r, C3D_Tex* view) {
    bindProgram(r, &r->program);
    r->litBound = false;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, r->uvLocation, 1.0f, 1.0f, 0.0f, 0.0f);
    bindTexUnit(r, 0, view);
    C3D_TexEnv* tex = C3D_GetTexEnv(0);
    C3D_TexEnvInit(tex);
    C3D_TexEnvSrc(tex, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(tex, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvSrc(tex, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(tex, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(tex, 0xFFFFFFFFu);
    installAlphaTest(r, 0, false);
    installDepthState(r, mdk::pica::DepthState{false, false});
    C3D_CullFace(GPU_CULL_NONE);
    r->cullMode = GPU_CULL_NONE;
}

// THE GLOW (Enhanced Mode enableGlowBillboards): the PC's post.frag shine, on the
// PICA200, between the scene and the canvas -- once per eye, from that eye's own
// picture. NO CPU TOUCHES A PIXEL, and that is a measured statement, not a hope:
// the first version called C3D_TexGenerateMipmap, which in the linked
// libcitro3d.a is a CPU box filter for a linear-heap texture (review,
// 2026-11-18: no GX call in its disassembly; it also read the previous frame's
// copy, since the GX queue is stopped until C3D_FrameEnd). The levels are made
// here by the GPU:
//
//   1. C3D_FrameSplit: the eye's scene so far goes to the GX queue, so what
//      follows reads it finished.
//   2. At edge-smoothing scale 2, the 480x800 target is brought to 240x400
//      first: a 2x2-averaging transfer TILED TO LINEAR (the screen output's own
//      form), then an unscaled LINEAR TO TILED one (the texture upload's form).
//      Not one tiled-to-tiled transfer (flag bit 5): on the console ANY display
//      transfer with bit 5 set stalls the transfer engine for good, scaled or
//      not, with or without OUT_TILED (measured 2026-11-18 on the Old 3DS
//      appliance, mdk_3ds/tools/gstest stages T20/T23/T27; the two-transfer
//      chain and the texture copy below ran 90 frames each, T22/T24-T26). The
//      emulator accepts bit 5, which is how this pass hung the first console
//      frame it ran on. Two GX queue entries per eye here, not one.
//   3. GX_TextureCopy, tile row by tile row (30 tiles = 7680 bytes of a 240-wide
//      framebuffer row into the 8192 bytes of a 256-wide texture row): the
//      picture becomes the 256x512 picture texture, unscaled (a raw copy).
//   4. Blur levels 1-5, each its own small texture: a quad into each one's
//      render target, sampling the level above bilinearly at texel corners (an
//      exact 2x2 average). Neither the transfer engine's tiled-to-tiled scaling
//      (citro3d never uses it, so nothing shows the console does it at these
//      widths) nor the picture texture's own mip levels (rendering into those
//      never reached the sampler in the emulator).
//   5. The mix texture := level 3, then += level 5 with a constant-alpha blend
//      (pica::kBlendGlowMix): (level 3 + level 5) / 2, the PC's two blur levels
//      folded into one, so ONE texture unit reads them. (Texture units 1 and 2
//      are not an option: citro3d cannot unbind them -- C3D_TexBind(1, NULL)
//      reads through the null pointer -- and a bound unit is sampled on every
//      later fragment of every frame.)
//   6. One full-panel quad into the eye, screen-blended (pica::kBlendScreen),
//      whose combiner is the shine: the threshold subtracted (SUBTRACT clamps at
//      0: the PC's max(.., 0)) and the fitted curve (pica_matrix.h, THE GLOW).
//
// GX QUEUE BUDGET: citro3d's queue holds 32 entries and a full one is svcBreak.
// This pass adds 2 per eye (the split, the copy) and 2 more at scale 2 (the
// shrink and the re-tile); the blur passes flush inside the command list instead
// of splitting. Measured on the console, stereo at scale 2, before the re-tile:
// 14 entries a frame.
// Everything this pass changes is restored, or its cache invalidated, before the
// canvas draws.
static void drawGlowBillboards(RendererC3D* r);

static void drawGlow(RendererC3D* r) {
    if (r->billboardsLive) {
        drawGlowBillboards(r);   // the 3DS default (config glowBloom = 0)
        return;
    }
    if (!r->glowLive || !r->eyeTarget || !ensureGlow(r)) {
        return;
    }
    C3D_RenderTarget* eye = r->eyeTarget;
    leaveShadowLitStages(r);
    frameSplit(r);
    u32* source = static_cast<u32*>(eye->frameBuf.colorBuf);
    if (r->targetScale > 1) {
        const size_t scratchBytes = (size_t)kScreenWidth * kScreenHeight * 4;
        if (!r->glowScratch) {
            r->glowScratch = static_cast<u32*>(linearAlloc(scratchBytes));
        }
        if (!r->glowScratchTiled) {
            r->glowScratchTiled = static_cast<u32*>(linearAlloc(scratchBytes));
        }
        if (!r->glowScratch || !r->glowScratchTiled) {
            mdk::log3dsPrintf("glow: smoothing scratch allocation FAILED; glow off this session");
            r->glowRefused = true;
            r->glowLive = false;
            return;
        }
        const u32 rgba = GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                         GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8);
        // Step 2 (above): NEVER flag bit 5 (tiled to tiled) -- it hangs the
        // console's transfer engine. Tiled to linear with the display's own
        // averaging (2x2, or 2x1: transferScaling), both
        // dimensions the input's (the engine halves the output itself: see
        // writeScreenshotBmp) ...
        const u32 full = GX_BUFFER_DIM(r->targetWidth, r->targetHeight);
        GX_DisplayTransfer(source, full, r->glowScratch, full,
                           rgba | transferScaling(r->targetScaleX, r->targetScaleY));
        // ... then linear back to tiled, unscaled, for the tile-row copy below.
        const u32 half = GX_BUFFER_DIM(kScreenWidth, kScreenHeight);
        GX_DisplayTransfer(r->glowScratch, half, r->glowScratchTiled, half,
                           rgba | GX_TRANSFER_OUT_TILED(1));
        source = r->glowScratchTiled;
    }
    const u32 rowBytes = (u32)kScreenWidth * 8 * 4;                       // 7680
    const u32 texRowBytes = (u32)mdk::pica::kGlowTexWidth * 8 * 4;        // 8192
    const u32 rows = (u32)kScreenHeight / 8;                              // 50
    GX_TextureCopy(source, GX_BUFFER_DIM(rowBytes >> 4, 0),
                   static_cast<u32*>(C3D_Tex2DGetImagePtr(&r->glowTex, 0, nullptr)),
                   GX_BUFFER_DIM(rowBytes >> 4, (texRowBytes - rowBytes) >> 4),
                   rowBytes * rows, GX_TRANSFER_RAW_COPY(1));
    // THE BLUR (steps 4-6), through the identity matrix: seven small
    // render-to-texture passes. Each ends in a framebuffer FLUSH and INVALIDATE
    // written into the command list, so the next pass samples what this one
    // wrote (the texture cache is cleared by the rebind each pass makes:
    // C3Di_UpdateContext sets TEXUNIT_CONFIG bit 16 on a bound-texture change).
    // Not a C3D_FrameSplit per pass: each split is a GX queue entry, citro3d's
    // queue holds 32 and a full queue is svcBreak, and a stereo frame at scale 2
    // already needs ~14 (review, 2026-11-18).
    C3D_Mtx identity;
    Mtx_Identity(&identity);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->mvpLocation, &identity);
    r->matrixIsFrame = false;
    C3D_BufInfo* info = C3D_GetBufInfo();
    BufInfo_Init(info);
    BufInfo_Add(info, r->glowLevelQuad, 5 * sizeof(float), 2, 0x10);
    for (int stage = 1; stage <= 5; stage++) {
        C3D_TexEnvInit(C3D_GetTexEnv(stage));
    }
    r->lightStageOn = false;
    r->hazeOn = false;
    struct SmallPass {
        int into;       // a glowSmall index
        C3D_Tex* from;
        bool mix;
    };
    const int mix = mdk::pica::kGlowSmallMix;
    const SmallPass passes[7] = {
        {0, &r->glowTex, false},          // level 1 from the picture
        {1, &r->glowSmall[0], false},     // 2 from 1
        {2, &r->glowSmall[1], false},     // 3 from 2
        {3, &r->glowSmall[2], false},     // 4 from 3
        {4, &r->glowSmall[3], false},     // 5 from 4
        {mix, &r->glowSmall[2], false},   // mix := 3
        {mix, &r->glowSmall[4], true},    // mix += 5, at alpha 1/2
    };
    for (int i = 0; i < 7; i++) {
        C3D_FrameDrawOn(r->glowTarget[passes[i].into]);
        C3D_SetViewport(0, 0, r->glowSmall[passes[i].into].width,
                        r->glowSmall[passes[i].into].height);
        glowDrawState(r, passes[i].from);
        if (passes[i].mix) {
            C3D_BlendingColor(0x80000000u);   // alpha 128/255 (bits 24-31)
            writeBlendState(r, mdk::pica::kBlendGlowMix, -2, -2);
        } else {
            writeBlendState(r, mdk::pica::kBlendReplace, -2, -2);
        }
        C3D_DrawArrays(GPU_TRIANGLES, 0, kGlowQuadCorners);
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
    }

    // BACK TO THE EYE (step 7), with installEye's viewport (the whole target); the
    // scissor is still the one installEye disabled.
    C3D_FrameDrawOn(eye);
    C3D_SetViewport(0, 0, (u32)r->targetWidth, (u32)r->targetHeight);
    const float width = mdk_render_canvas_width(reinterpret_cast<MdkRenderer>(r));
    if (width != r->glowQuadWidth) {
        const float corner[kGlowQuadCorners][2] = {{0, 0}, {1, 0}, {1, 1},
                                                   {0, 0}, {1, 1}, {0, 1}};
        for (int i = 0; i < kGlowQuadCorners; i++) {
            float* v = r->glowQuad + i * 5;
            v[0] = corner[i][0] * width;
            v[1] = corner[i][1] * mdk::kCanvasHeight;
            v[2] = 0.0f;
            mdk::pica::glowTexCoord(corner[i][0], corner[i][1], &v[3], &v[4]);
        }
        GSPGPU_FlushDataCache(r->glowQuad, kGlowQuadCorners * 5 * sizeof(float));
        r->glowQuadWidth = width;
    }
    float screen[16];
    mdk::pica::canvasMatrix(screen, width);
    C3D_Mtx gpuMtx;
    mdk::pica::registers(screen, gpuMtx.m);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->mvpLocation, &gpuMtx);
    info = C3D_GetBufInfo();
    BufInfo_Init(info);
    BufInfo_Add(info, r->glowQuad, 5 * sizeof(float), 2, 0x10);
    glowDrawState(r, &r->glowSmall[mdk::pica::kGlowSmallMix]);

    // THE SHINE, s*(u) = P * min(1, u / u0) + Q * u (pica_matrix.h, THE GLOW).
    // Each constant is a byte, each scale 1, 2 or 4.
    //   0: b, the blur                           (glowDrawState: texture, alpha 1)
    //   1: 4 (b - t)          = 4 * band * u    SUBTRACT clamps at 0: max(.., 0)
    //   2: Q u                                  -> the combiner buffer
    //   3: 4 Q u                                (clamped at 1)
    //   4: 16 Q u = u / u0                      (clamped at 1: the min)
    //   5: P * prev + buffer                    MULTIPLY_ADD
    auto byteOf = [](float v) -> u32 {
        const float b = v * 255.0f + 0.5f;
        return b <= 0.0f ? 0u : (b >= 255.0f ? 255u : (u32)b);
    };
    auto grey = [](u32 b) -> u32 { return 0xFF000000u | (b << 16) | (b << 8) | b; };
    C3D_TexEnv* excess = C3D_GetTexEnv(1);
    C3D_TexEnvInit(excess);
    C3D_TexEnvSrc(excess, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(excess, C3D_RGB, GPU_SUBTRACT);
    C3D_TexEnvScale(excess, C3D_RGB, GPU_TEVSCALE_4);
    C3D_TexEnvColor(excess, grey(byteOf(mdk::pica::kGlowThresholdSrgb)));
    C3D_TexEnv* linear = C3D_GetTexEnv(2);
    C3D_TexEnvInit(linear);
    C3D_TexEnvSrc(linear, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(linear, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvColor(linear, grey(byteOf(mdk::pica::kGlowCurveQ / (4.0f * mdk::pica::kGlowBand))));
    C3D_TexEnvBufUpdate(C3D_RGB, 1 << 2);   // stage 2's output is the buffer from stage 3
    for (int stage = 3; stage <= 4; stage++) {
        C3D_TexEnv* steep = C3D_GetTexEnv(stage);
        C3D_TexEnvInit(steep);
        C3D_TexEnvScale(steep, C3D_RGB, GPU_TEVSCALE_4);   // REPLACE previous, x4
    }
    C3D_TexEnv* sum = C3D_GetTexEnv(5);
    C3D_TexEnvInit(sum);
    C3D_TexEnvSrc(sum, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PREVIOUS_BUFFER);
    C3D_TexEnvFunc(sum, C3D_RGB, GPU_MULTIPLY_ADD);
    C3D_TexEnvColor(sum, grey(byteOf(mdk::pica::kGlowCurveP)));
    writeBlendState(r, mdk::pica::kBlendScreen, -2, -2);
    C3D_DrawArrays(GPU_TRIANGLES, 0, kGlowQuadCorners);
    r->glowPasses++;

    // Back to the frame's state for the canvas: stages 1-5 passing through (the
    // light and haze caches say so) and the combiner buffer off. Stage 0, the
    // texture on unit 0, the matrix and the blend are written by the next draw
    // itself; the program is the scene's (glowDrawState left it unlit).
    for (int stage = 1; stage <= 5; stage++) {
        C3D_TexEnvInit(C3D_GetTexEnv(stage));
    }
    C3D_TexEnvBufUpdate(C3D_Both, 0);
    r->lightStageOn = false;
    r->hazeOn = false;
}

