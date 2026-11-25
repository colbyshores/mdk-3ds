// 03_shadow_probe.inc -- the sun shadow's one-time measurement: the GPU read-back, the ramp and native-write tests.
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

// A GPU BUFFER READ BACK INTO LINEAR MEMORY, OUTSIDE A FRAME, AND KNOWN DONE.
// C3D_SyncTextureCopy out of a frame ends in gspWaitForEvent(PPF, false), which
// does not clear the event first: a display transfer of the frame just ended
// that completed unobserved leaves it set, and the wait then returns before
// THIS copy has run (review, 2026-11-18; the emulator completes transfers at
// once and cannot show it). So the destination is filled with `fill` (the
// answer that means "nothing seen") and a second, 64-byte copy follows into a
// renderer-owned drain: the transfer engine runs in order, and the PPF event
// the second wait sees is at the earliest the first copy's own. The drain is
// freed only after C3D_Fini; a destination freed after this cannot still be
// the target of a transfer. False when the drain cannot be had (nothing read).
static bool readBack(RendererC3D* r, void* source, u32 rowDim, u32* dest, size_t bytes, u8 fill) {
    if (!r->copyDrain) {
        r->copyDrain = static_cast<u32*>(linearAlloc(64));
        if (!r->copyDrain) {
            return false;
        }
    }
    memset(dest, fill, bytes);
    GSPGPU_FlushDataCache(dest, bytes);
    C3D_SyncTextureCopy(static_cast<u32*>(source), rowDim, dest, rowDim, (u32)bytes,
                        GX_TRANSFER_RAW_COPY(1));
    const u32 drainDim = GX_BUFFER_DIM(64u >> 4, 0);
    C3D_SyncTextureCopy(static_cast<u32*>(source), drainDim, r->copyDrain, drainDim, 64,
                        GX_TRANSFER_RAW_COPY(1));
    GSPGPU_InvalidateDataCache(dest, bytes);
    return true;
}

// The sampler's half of testNativeShadowWrites: the receive program over the
// test's quad (the bound vertex buffer) into `probe`, every receiver at depth
// 0.75 at the map's centre (sunMtx rows (0,0,0,c), no normal offset, no bias),
// the combiner the sampler's answer alone: black where the map holds a nearer
// depth. A geometry program; the caller keeps bindProgram's rule.
static void testSampleShadowMap(RendererC3D* r, C3D_Tex* tex, C3D_RenderTarget* probe,
                                const C3D_Mtx* identity) {
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_FrameBufClear(&probe->frameBuf, C3D_CLEAR_COLOR, 0xFFFFFFFFu, 0);
    C3D_FrameDrawOn(probe);
    C3D_SetViewport(0, 0, 8, 8);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 8, 8);
    bindProgram(r, &r->programShadow);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->mvpLocation, identity);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->worldLocation, identity);
    r->worldIsIdentity = true;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, r->uvLocation, 1.0f, 1.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, r->tintLocation, 1.0f, 1.0f, 1.0f, 1.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, r->hazeLocation, 0.0f, 0.0f, 0.0f, 0.0f);
    r->tintIsWhite = true;
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowSunDirLocation, 0.0f, 0.0f, -1.0f, 0.0f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowSunColourLocation, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowSkyLocation, 1.0f, 1.0f, 1.0f, 0.0f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowGroundLocation, 1.0f, 1.0f, 1.0f, 0.0f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowCameraLocation, 0.0f, 0.0f, 1.0f, 1.0f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowParamsLocation, 1.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowSunMtxLocation + 0, 0.0f, 0.0f, 0.0f, 0.5f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowSunMtxLocation + 1, 0.0f, 0.0f, 0.0f, 0.5f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowSunMtxLocation + 2, 0.0f, 0.0f, 0.0f, 0.75f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowSunMtxLocation + 3, 0.0f, 0.0f, 0.0f, 1.0f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowLutMapLocation, 0.0f, 0.0f, 0.0f, 0.0f);
    bindTexUnit(r, 0, tex);
    C3D_TexShadowParams(false, 0.0f);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, 0xFFFFFFFFu);
    C3D_DrawArrays(GPU_GEOMETRY_PRIM, 0, 6);
}

// ============================================================================
// THE PICA'S OWN SHADOW WRITES, MEASURED (2026-11-18). The caster pass has two
// ways to make the map:
//   * NATIVE: GPU_FRAGOPMODE_SHADOW, the mode the PICA has for it -- every
//     caster fragment's 24-bit depth goes into the map (the nearer one kept).
//   * THE RAMP: an ordinary draw whose texture coordinates look the depth's
//     bytes up in a ramp texture (s = depth, t = 256 * depth, REPEAT).
// Each fails on one instrument, and both failures were measured:
//   * on the Old 3DS the ramp's t is not repeated that far: the map the console
//     wrote, read back byte for byte against the emulator's (level 8 frame 60),
//     has the depth's middle byte 0 in every texel, so its depth is good to
//     1/256 of the range (31 world units) and the sunlit floor shadowed itself;
//     native writes land and the shadow sampler reads them.
//   * in the emulator (Mandarine's OpenGL renderer on llvmpipe) the ramp is
//     exact, and native writes reach memory and even an 8x8 test's sampler,
//     yet the game's own frame showed no shadow from them at all.
// So the renderer measures both, once, before the first frame that wants a sun
// shadow, in a frame of its own, waited for:
//   * THE RAMP: one quad at a depth whose middle byte is 0x37 drawn through it,
//     read back -- the byte is there, or it is 0 (the console);
//   * NATIVE: an 8x8 map SAMPLED (testSampleShadowMap, still clear), WRITTEN
//     natively at depth 0.5, and SAMPLED AGAIN with every receiver behind it.
// The ramp where it is exact; native writes where it is not and they work
// (the console); the ramp otherwise, logged as neither. end_frame then reads
// the real pass's map back once more in native mode.
static void testNativeShadowWrites(RendererC3D* r) {
    // A test that could not RUN (no VRAM or linear memory for it this time) is
    // tried again, up to three times, before the ramp is taken for the session:
    // on the console the ramp is the path that cannot work.
    r->shadowNativeTestRuns++;
    r->shadowNativeTested = r->shadowNativeTestRuns >= 3;
    r->shadowNative = false;
    C3D_Tex tex;
    if (!C3D_TexInitShadow(&tex, 8, 8)) {
        mdk::log3dsPrintf("shadows: native-write test could not make its texture (try %d)",
                          (int)r->shadowNativeTestRuns);
        return;
    }
    C3D_RenderTarget* target = C3D_RenderTargetCreateFromTex(&tex, GPU_TEXFACE_2D, 0, -1);
    // ... and the RECEIVER: an 8x8 colour target the shadow sampler draws into.
    C3D_RenderTarget* probe = C3D_RenderTargetCreate(8, 8, GPU_RB_RGBA8, -1);
    // ... and the RAMP's: an 8x8 colour target one ramp quad is drawn into.
    C3D_RenderTarget* rampProbe = C3D_RenderTargetCreate(8, 8, GPU_RB_RGBA8, -1);
    float* quad = static_cast<float*>(linearAlloc(12 * 5 * sizeof(float)));
    u32* back = static_cast<u32*>(linearAlloc(8 * 8 * 4));
    u32* seen = static_cast<u32*>(linearAlloc(8 * 8 * 4));
    u32* rampSeen = static_cast<u32*>(linearAlloc(8 * 8 * 4));
    if (target && probe && rampProbe && quad && back && seen && rampSeen &&
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) {
        // Clip space through the identity: the whole target, z -0.5 -> depth 0.5
        // (pica::kDepthMapScale z + kDepthMapOffset).
        static const int kTestCorners = 6;
        static const int kRampTestMiddle = 0x37;
        static const float kCorners[kTestCorners][2] = {
            {-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
        // The second quad, for the ramp: a depth whose middle byte is 0x37 and
        // whose high byte is 0x80, half a step into its 1/65536 (no boundary near).
        const float rampDepth = (float)(0x80 * 256 + kRampTestMiddle) / 65536.0f + 0.5f / 65536.0f;
        for (int q = 0; q < 2; q++) {
            for (int c = 0; c < kTestCorners; c++) {
                float* corner = quad + (q * kTestCorners + c) * 5;
                corner[0] = kCorners[c][0];
                corner[1] = kCorners[c][1];
                corner[2] = q == 0 ? -0.5f : rampDepth - 1.0f;
                corner[3] = 0.0f;
                corner[4] = 0.0f;
            }
        }
        GSPGPU_FlushDataCache(quad, 12 * 5 * sizeof(float));
        C3D_FrameBufClear(&target->frameBuf, C3D_CLEAR_COLOR, 0xFFFFFFFFu, 0);
        C3D_Mtx identity;
        Mtx_Identity(&identity);
        C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
        AttrInfo_Init(attrInfo);
        AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);
        AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2);
        C3D_BufInfo* bufInfo = C3D_GetBufInfo();
        BufInfo_Init(bufInfo);
        BufInfo_Add(bufInfo, quad, 5 * sizeof(float), 2, 0x10);
        C3D_TexEnv* env = C3D_GetTexEnv(0);
        for (int stage = 1; stage <= 5; stage++) {
            C3D_TexEnvInit(C3D_GetTexEnv(stage));
        }
        C3D_TexEnvBufUpdate(C3D_Both, 0);
        r->hazeOn = false;
        r->lightStageOn = false;
        C3D_DepthMap(mdk::pica::kDepthMapIsZBuffer, mdk::pica::kDepthMapScale,
                     mdk::pica::kDepthMapOffset);
        installDepthState(r, mdk::pica::DepthState{false, false});
        installAlphaTest(r, -1, true);
        writeBlendState(r, mdk::pica::kBlendReplace, -3, -3);   // castShadows' own
        C3D_CullFace(GPU_CULL_NONE);
        r->cullMode = GPU_CULL_NONE;
        // THE RAMP, MEASURED: one quad through the caster program and the ramp,
        // as castShadows draws the map in ramp mode. Where t = 256 * depth is
        // repeated (the emulator) the texel's middle byte is 0x37; where it is
        // not (the Old 3DS: 0) the ramp cannot make a map.
        const bool rampMade = makeShadowRamp(r);
        if (rampMade) {
            C3D_FrameBufClear(&rampProbe->frameBuf, C3D_CLEAR_COLOR, 0, 0);
            C3D_FrameDrawOn(rampProbe);
            C3D_SetViewport(0, 0, 8, 8);
            C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 8, 8);
            bindProgram(r, &r->programCaster);
            r->litBound = false;
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->casterMvpLocation, &identity);
            bindTexUnit(r, 0, &r->shadowRamp);
            C3D_TexEnv* rampEnv = C3D_GetTexEnv(0);
            C3D_TexEnvInit(rampEnv);
            C3D_TexEnvSrc(rampEnv, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
            C3D_TexEnvFunc(rampEnv, C3D_Both, GPU_REPLACE);
            C3D_DrawArrays(GPU_TRIANGLES, kTestCorners, kTestCorners);
        }
        // FIRST, THE MAP IS SAMPLED while it still holds its clear, as the game's
        // map has been sampled by every frame before the one that writes it: an
        // instrument that caches a sampled texture and does not see later native
        // writes into it (the emulator: its own 8x8 test passed without this and
        // the game's map then showed no shadow) must fail HERE.
        testSampleShadowMap(r, &tex, probe, &identity);
        // THEN THE NATIVE WRITE (the caster program is plain: bindProgram splits
        // the list after the sampler's geometry program, the console's rule).
        C3D_FrameDrawOn(target);
        C3D_SetViewport(0, 0, 8, 8);
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 8, 8);
        bindProgram(r, &r->programCaster);
        r->litBound = false;
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->casterMvpLocation, &identity);
        // C3D_GetTexEnv again: it is what marks the stage dirty, and the sampler's
        // draw has uploaded (and cleaned) it since -- edits through the old
        // pointer would never reach the GPU, and the caster would run with the
        // sampler's combiner (measured on the console: green non-zero, the
        // penumbra path, softness 0xFE and no depth).
        env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_TexEnvColor(env, 0x00000000u);
        C3D_FragOpMode(GPU_FRAGOPMODE_SHADOW);
        C3D_FragOpShadow(0.0f, 1.0f);
        C3D_DrawArrays(GPU_TRIANGLES, 0, kTestCorners);
        C3D_FragOpMode(GPU_FRAGOPMODE_GL);
        // THE SAMPLER'S SIDE, in the same command list, as the game reads the map:
        // the receive program over the same quad, every receiver at depth 0.75 at
        // the map's centre (sunMtx rows (0,0,0,c), no normal offset, no bias), the
        // combiner the sampler's answer alone. Black = behind the 0.5 just written
        // = the sampler saw the native write. (The emulator writes natively, its
        // readback sees it -- and its sampler, in the same list, does not: level
        // 8 frame 60 then showed no shadow at all.) A geometry program is the last
        // thing in this list, so nothing plain follows it (bindProgram's rule).
        // AND THE SAMPLER AGAIN: now it must see the write.
        testSampleShadowMap(r, &tex, probe, &identity);
        C3D_FrameEnd(0);
        // The copies wait for the queue (the test's draws) to drain first.
        // Each pre-filled with the answer that means "did not work" (readBack):
        // unwritten (FF), not dark (FF), no middle byte (00).
        const u32 rowDim = GX_BUFFER_DIM((8u * 8u * 4u) >> 4, 0);
        const bool read = readBack(r, tex.data, rowDim, back, 8 * 8 * 4, 0xFF) &&
                          readBack(r, probe->frameBuf.colorBuf, rowDim, seen, 8 * 8 * 4, 0xFF) &&
                          readBack(r, rampProbe->frameBuf.colorBuf, rowDim, rampSeen, 8 * 8 * 4, 0x00);
        // The ramp texel's middle byte: memory byte 1 in the reversed order (the
        // map's [high, mid, low, softness]), byte 2 in the PICA's.
        const int midByte = shadowByteOrder(r) == mdk::pica::kShadowBytesReversed ? 1 : 2;
        int rampExact = 0;
        for (int i = 0; i < 64 && rampMade && read; i++) {
            const uint8_t* texel = reinterpret_cast<const uint8_t*>(&rampSeen[i]);
            if (texel[midByte] == kRampTestMiddle) {
                rampExact++;
            }
        }
        int moved = 0;
        int dark = 0;
        for (int i = 0; i < 64 && read; i++) {
            if (back[i] != 0xFFFFFFFFu) {
                moved++;
            }
            // RGBA8 in memory is [A, B, G, R]: the colour is the word's top 24 bits.
            if ((seen[i] & 0xFFFFFF00u) == 0) {
                dark++;
            }
        }
        r->shadowNativeTested = true;   // it ran: its answer stands
        // THE DECISION: the ramp where it is exact (its depths are the same 16
        // bits everywhere it works, and its own sampler reads them); native
        // writes where the ramp is not and they work; else the ramp, said so.
        const bool rampWorks = rampExact == 64;
        const bool nativeWorks = moved == 64 && dark == 64;
        r->shadowNative = !rampWorks && nativeWorks;
        mdk::log3dsPrintf("shadows: the ramp %s (%d of 64 texels carry the middle byte, first "
                          "%08lX); native writes %s (%d of 64 written, first %08lX; %d of 64 "
                          "receivers behind them dark, first %08lX); the caster pass uses %s",
                          rampWorks ? "WORKS" : "does not work", rampExact,
                          (unsigned long)rampSeen[0], nativeWorks ? "WORK" : "do not work", moved,
                          (unsigned long)back[0], dark, (unsigned long)seen[0],
                          r->shadowNative ? "native writes"
                                          : (rampWorks ? "the ramp"
                                                       : "the ramp (NEITHER path measured right)"));
        // A native session never draws through the ramp: its half megabyte back.
        // And a map an earlier, could-not-run try left behind was made for the
        // ramp (with a depth buffer the native writes were never measured with):
        // it goes too, and the next frame makes the native one (review,
        // 2026-11-18). Outside a frame here, as releaseShadowMap needs.
        if (r->shadowNative) {
            releaseShadowMap(r);
        }
        r->matrixIsFrame = false;
        r->hazeOn = false;
    } else {
        mdk::log3dsPrintf("shadows: native-write test could not run (try %d)",
                          (int)r->shadowNativeTestRuns);
    }
    if (rampSeen) {
        linearFree(rampSeen);
    }
    if (seen) {
        linearFree(seen);
    }
    if (back) {
        linearFree(back);
    }
    if (quad) {
        linearFree(quad);
    }
    if (rampProbe) {
        C3D_RenderTargetDelete(rampProbe);
    }
    if (probe) {
        C3D_RenderTargetDelete(probe);
    }
    if (target) {
        C3D_RenderTargetDelete(target);
    }
    // citro3d keeps the pointer it was bound with: unit 0 is unbound before the
    // test's texture (on this stack) goes, so nothing re-dirtied later (an APT
    // resume re-sends every unit) can read a dead C3D_Tex (review, 2026-11-18).
    // Every draw that samples unit 0 binds its own texture first.
    bindTexUnit(r, 0, nullptr);
    C3D_TexDelete(&tex);
}

