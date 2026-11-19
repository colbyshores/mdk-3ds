// 02_lifecycle.inc -- create/destroy, the backend name and capabilities.
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

// THE EYE TARGETS, AT A SCALE.
//
// Scale 1 is the panel's own 240x400 (portrait; the projection rotates it).
// Scale 2 is EDGE SMOOTHING (enableEdgeSmoothing, SPEC.md 7.9): each eye draws
// into a 480x800 target and the display transfer that already copies the target
// to the panel averages every 2x2 block into one pixel (GX_TRANSFER_SCALE_XY).
// That is supersampling done by the PICA's own transfer engine -- the 3DS's
// counterpart of the PC's MSAA, with no CPU work and no extra pass.
//
// VRAM IS SIX MEGABYTES. A scale-2 eye is 1.5 MB of RGBA8 colour plus 1.5 MB of
// DEPTH24_STENCIL8, so two full eyes would be all of it. The eyes are drawn one
// after the other, so at scale 2 the right eye BORROWS the left eye's depth
// buffer (4.5 MB in all) and mdk_render_end_frame clears it between the passes.
// A target created without depth does not own (or free) the buffer it borrows:
// citro3d frees only what it allocated (C3Di_RenderTargetDestroy's ownsDepth).
static u32 displayTransferFlags(int scale) {
    return (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
            GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
            GX_TRANSFER_SCALING(scale > 1 ? GX_TRANSFER_SCALE_XY : GX_TRANSFER_SCALE_NO));
}

// Outside a frame only: C3D_RenderTargetDelete waits for the GPU queue (and
// breaks if called inside a frame), so nothing in flight still reads them.
static void releaseEyeTargets(RendererC3D* r) {
    if (r->targetRight) {
        C3D_RenderTargetDelete(r->targetRight);
        r->targetRight = nullptr;
    }
    if (r->targetLeft) {
        C3D_RenderTargetDelete(r->targetLeft);
        r->targetLeft = nullptr;
    }
    r->targetsShareDepth = false;
}

// The glow's VRAM (06_draw.inc ensureGlow), outside a frame only, like the eye
// targets: the first C3D_RenderTargetDelete waits for the GPU queue, so nothing
// still reads the textures freed after it. Released whenever the eye targets are
// rebuilt and remade on the glow's next use, so the glow's 0.7 MB never sits
// between the eye buffers a rebuild needs contiguous (review, 2026-11-18: glow on
// first, then edge smoothing, could otherwise fragment the VRAM into a refusal).
// A refusal is forgotten too: it may have been the old layout's.
// The sun shadow's map and its target, with the glow's for the glow's reason
// (they sit in the same VRAM the eye buffers need contiguous), and a refusal is
// forgotten with them. Outside a frame only: C3D_RenderTargetDelete waits on
// the queue.
static void releaseShadowMap(RendererC3D* r) {
    if (r->shadowTarget) {
        C3D_RenderTargetDelete(r->shadowTarget);
        r->shadowTarget = nullptr;
    }
    if (r->shadowMapReady) {
        C3D_TexDelete(&r->shadowTex);
        r->shadowMapReady = false;
    }
    if (r->shadowRampReady) {
        C3D_TexDelete(&r->shadowRamp);
        r->shadowRampReady = false;
    }
    r->shadowRefused = false;
}

static void releaseGlow(RendererC3D* r) {
    for (int i = 0; i < mdk::pica::kGlowSmallCount; i++) {
        if (r->glowTarget[i]) {
            C3D_RenderTargetDelete(r->glowTarget[i]);
            r->glowTarget[i] = nullptr;
        }
    }
    for (int i = 0; i < mdk::pica::kGlowSmallCount; i++) {
        if (r->glowSmallMade[i]) {
            C3D_TexDelete(&r->glowSmall[i]);
            r->glowSmallMade[i] = false;
        }
    }
    if (r->glowReady) {
        C3D_TexDelete(&r->glowTex);
        r->glowReady = false;
    }
    r->glowRefused = false;
}

// Both eyes at `scale`, or neither (false, nothing held). The caller has released
// the previous pair: the VRAM is not big enough for both generations at once.
static bool buildEyeTargets(RendererC3D* r, int scale) {
    const int w = kScreenWidth * scale;
    const int h = kScreenHeight * scale;
    C3D_RenderTarget* left = C3D_RenderTargetCreate(w, h, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!left) {
        mdk::log3dsPrintf("C3D_RenderTargetCreate(left %dx%d) FAILED (vram free %u)", w, h,
                          (unsigned)vramSpaceFree());
        return false;
    }
    const bool share = scale > 1;
    C3D_RenderTarget* right = share
        ? C3D_RenderTargetCreate(w, h, GPU_RB_RGBA8, -1)
        : C3D_RenderTargetCreate(w, h, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!right) {
        mdk::log3dsPrintf("C3D_RenderTargetCreate(right %dx%d) FAILED (vram free %u)", w, h,
                          (unsigned)vramSpaceFree());
        C3D_RenderTargetDelete(left);
        return false;
    }
    if (share) {
        C3D_FrameBufDepth(&right->frameBuf, left->frameBuf.depthBuf, GPU_RB_DEPTH24_STENCIL8);
    }
    C3D_RenderTargetSetOutput(left, GFX_TOP, GFX_LEFT, displayTransferFlags(scale));
    C3D_RenderTargetSetOutput(right, GFX_TOP, GFX_RIGHT, displayTransferFlags(scale));
    r->targetLeft = left;
    r->targetRight = right;
    r->targetScale = scale;
    r->targetWidth = w;
    r->targetHeight = h;
    r->targetsShareDepth = share;
    mdk::log3dsPrintf("targets: %dx%d per eye, scale %d%s (vram free %u)", w, h, scale,
                      share ? ", right eye borrows the left depth" : "",
                      (unsigned)vramSpaceFree());
    return true;
}

 MdkRenderer mdk_render_create(const MdkRenderDesc* desc) {

    // R1: the FPSCR's FZ and DN bits MUST be set before any application float
    // work. This is the main thread, and main_3ds.cpp calls the helper
    // before gfxInitDefault; this is the defensive second call, and it is
    // cheap. The read-back is the point of the helper, so its result is not
    // discarded: a failure here means the FPSCR did not take the bits and
    // every float result in this process is suspect.
    if (!mdk::vfpSetFzDn()) {
        mdk::log3dsPrintf("FZ/DN read-back FAILED in renderer create");
    }

    RendererC3D* r = new RendererC3D();

    // The command buffer is 4x the default (256 KB -> 1 MB).
    //
    // IT IS LINEAR MEMORY, AND THE LINEAR HEAP STARTS IN THE VRAM. A 4 MB
    // command buffer -- which is what Forsaken uses, for the draw-call
    // spikes -- takes four of the 3DS's SIX megabytes of VRAM, and then the
    // render targets and the textures have nothing left. The symptom is
    // every texture failing to allocate and a screen that shows only the
    // clear colour.
    //
    // One megabyte is the compromise: enough for a busy frame's draw calls,
    // and it leaves the VRAM for the pictures.
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2)) {
        mdk::log3dsPrintf("C3D_Init FAILED");
        delete r;
        return nullptr;
    }

    // The top screen's eye pair, at scale 1 (the panel's own 240x400 portrait;
    // the projection rotates it). The toggles arrive after create, and the first
    // begin_frame rebuilds them at scale 2 when enableEdgeSmoothing is on.
    if (!buildEyeTargets(r, 1)) {
        C3D_Fini();
        delete r;
        return nullptr;
    }

    // THE DEPTH STATE, IN THE LOG.
    //
    // WHY THIS IS HERE. The depth map, the test function and the clear value are
    // three numbers that have to agree with each other, and the failure mode of
    // their disagreeing is a screen showing only the clear colour — no error,
    // no crash, nothing to look at. This host has no 3DS emulator and no
    // Old-3DS, so the picture cannot be the evidence (AGENTS.md P6). The log
    // file can: these are the numbers the frame actually installs, read back
    // from the same constants the frame installs, so a run on hardware records
    // the state even when nobody can see the screen.
    mdk::log3dsPrintf("depth: map z*%.1f+%.1f near=%.4f far=%.4f clear=0x%06x test=LEQUAL",
                      (double)mdk::pica::kDepthMapScale,
                      (double)mdk::pica::kDepthMapOffset,
                      (double)mdk::pica::storedDepth(-1.0f),
                      (double)mdk::pica::storedDepth(0.0f),
                      (unsigned)mdk::pica::kDepthClearValue);

    // The shader.
    r->dvlb = DVLB_ParseFile((u32*)mdk_c3d_shbin, mdk_c3d_shbin_size);
    if (!r->dvlb) {
        mdk::log3dsPrintf("DVLB_ParseFile FAILED");
        releaseEyeTargets(r);
        C3D_Fini();
        delete r;
        return nullptr;
    }
    const Result spInit = shaderProgramInit(&r->program);
    if (R_FAILED(spInit)) {
        mdk::log3dsPrintf("shaderProgramInit FAILED result=0x%08lx", (unsigned long)spInit);
        DVLB_Free(r->dvlb);
        releaseEyeTargets(r);
        C3D_Fini();
        delete r;
        return nullptr;
    }
    // The DVLB's first DVLE is the vertex shader.
    mdk::log3dsPrintf("dvlb numDVLE=%d", (int)r->dvlb->numDVLE);
    const Result spVsh = (r->dvlb->numDVLE >= 1)
        ? shaderProgramSetVsh(&r->program, &r->dvlb->DVLE[0])
        : (Result)-1;
    if (R_FAILED(spVsh)) {
        mdk::log3dsPrintf("shaderProgramSetVsh FAILED result=0x%08lx", (unsigned long)spVsh);
        shaderProgramFree(&r->program);
        DVLB_Free(r->dvlb);
        releaseEyeTargets(r);
        C3D_Fini();
        delete r;
        return nullptr;
    }
    // Logged on SUCCESS as well as failure, and that is the whole point of the
    // line: under Mandarine the guest stops between this and the next line, and
    // a log that only records failures cannot tell "hung in setVsh" apart from
    // "hung in configure". Measured 2026-11-05: `dvlb numDVLE=1` is the last
    // line the guest ever writes, so the pair below is what localises it.
    mdk::log3dsPrintf("shaderProgramSetVsh ok result=0x%08lx", (unsigned long)spVsh);

    // THERE IS NO shaderProgramConfigure HERE, ON PURPOSE.
    //
    // What the shipped binaries say (both read out of the devkitARM archives on
    // 2026-11-05 with objdump -dr, not remembered):
    //   * libctru `shaderProgramSetVsh` frees the old instance, mallocs a new
    //     one and tail-calls `shaderInstanceInit`. It uploads nothing.
    //   * citro3d `C3D_BindProgram` does not configure either: it marks the
    //     shader dirty in the context state — 0x1900 when no program was bound
    //     before, 0x900 / 0x1100 when the vertex or geometry DVLE differs — and
    //     `C3Di_UpdateContext` is what calls
    //     `shaderProgramConfigure(sp, state>>11 & 1, state>>12 & 1)`, from
    //     inside the frame.
    // So the bind in `drawFrame` already uploads the microcode exactly once, and
    // the create-time call that used to sit here was a SECOND send, issued
    // outside any `C3D_FrameBegin`, into whatever GPUCMD command buffer happened
    // to be current before the first frame existed. `shaderProgramConfigure` is
    // GPUCMD_Add from end to end — permutation/operand writes plus the
    // microcode-DEST sequence — so a send outside the frame contract is the
    // suspect for the stop, and it is the only call between the last line the
    // guest ever writes and the readiness token `backend:` that main prints.
    //
    // This is the library's own contract, not an invention: SPEC's create step is
    // "parse the DVLB, build the shader program" and the sibling citro3d port's
    // frame entry does `C3D_FrameBegin` then `C3D_BindProgram` with no configure
    // anywhere at create time. Verified by running it, not by inspection
    // (mdk_3ds/tools/emurun.sh, see docs/PROGRESS_LOG.md).

    // The uniform locations.
    r->mvpLocation = shaderInstanceGetUniformLocation(r->program.vertexShader, "mvp");
    r->uvLocation = shaderInstanceGetUniformLocation(r->program.vertexShader, "uvtransform");
    r->tintLocation = shaderInstanceGetUniformLocation(r->program.vertexShader, "tint");
    r->hazeLocation = shaderInstanceGetUniformLocation(r->program.vertexShader, "haze");
    r->worldLocation = shaderInstanceGetUniformLocation(r->program.vertexShader, "world");
    // THE LIT PROGRAM: the same vertex shader, plus the geometry shader (DVLE[1]),
    // whose input stride is 12 registers: three vertices of four outputs each
    // (position, colour, texcoord, world). Optional -- a failure is logged and
    // lighting is simply unavailable.
    if (r->dvlb->numDVLE >= 2 && R_SUCCEEDED(shaderProgramInit(&r->programLit))) {
        const Result litVsh = shaderProgramSetVsh(&r->programLit, &r->dvlb->DVLE[0]);
        const Result litGsh = shaderProgramSetGsh(&r->programLit, &r->dvlb->DVLE[1], 12);
        if (R_SUCCEEDED(litVsh) && R_SUCCEEDED(litGsh)) {
            shaderInstance_s* gs = r->programLit.geometryShader;
            r->sunDirLocation = shaderInstanceGetUniformLocation(gs, "sunDir");
            r->sunColourLocation = shaderInstanceGetUniformLocation(gs, "sunColour");
            r->skyLocation = shaderInstanceGetUniformLocation(gs, "skyColour");
            r->groundLocation = shaderInstanceGetUniformLocation(gs, "groundColour");
            r->cameraLocation = shaderInstanceGetUniformLocation(gs, "camera");
            r->lightParamsLocation = shaderInstanceGetUniformLocation(gs, "params");
            // The same refusal as the vertex shader's: a -1 location is a write
            // before a .bss array (review-9 F5).
            r->litReady = r->sunDirLocation >= 0 && r->sunColourLocation >= 0 &&
                          r->skyLocation >= 0 && r->groundLocation >= 0 &&
                          r->cameraLocation >= 0 && r->lightParamsLocation >= 0;
        }
        if (!r->litReady) {
            shaderProgramFree(&r->programLit);
        }
    }
    mdk::log3dsPrintf("lit program: %s (sun=%d sky=%d ground=%d camera=%d params=%d world=%d)",
                      r->litReady ? "ready" : "UNAVAILABLE", (int)r->sunDirLocation,
                      (int)r->skyLocation, (int)r->groundLocation, (int)r->cameraLocation,
                      (int)r->lightParamsLocation, (int)r->worldLocation);
    // THE SHADOWED LIT PROGRAM (Enhanced Mode enableSkyShadows): the same vertex
    // shader plus the DVLB's third DVLE, mdk_litshadow.g.pica -- the light AND the
    // sun's shadow in one pass -- with the same input stride. Optional, like the
    // lit program: without it there are no sun shadows.
    if (r->litReady && r->dvlb->numDVLE >= 3 &&
        R_SUCCEEDED(shaderProgramInit(&r->programShadow))) {
        const Result shadowVsh = shaderProgramSetVsh(&r->programShadow, &r->dvlb->DVLE[0]);
        const Result shadowGsh = shaderProgramSetGsh(&r->programShadow, &r->dvlb->DVLE[2], 12);
        if (R_SUCCEEDED(shadowVsh) && R_SUCCEEDED(shadowGsh)) {
            shaderInstance_s* gs = r->programShadow.geometryShader;
            r->shadowSunDirLocation = shaderInstanceGetUniformLocation(gs, "sunDir");
            r->shadowSunColourLocation = shaderInstanceGetUniformLocation(gs, "sunColour");
            r->shadowSkyLocation = shaderInstanceGetUniformLocation(gs, "skyColour");
            r->shadowGroundLocation = shaderInstanceGetUniformLocation(gs, "groundColour");
            r->shadowCameraLocation = shaderInstanceGetUniformLocation(gs, "camera");
            r->shadowParamsLocation = shaderInstanceGetUniformLocation(gs, "params");
            r->shadowSunMtxLocation = shaderInstanceGetUniformLocation(gs, "sunMtx");
            r->shadowLutMapLocation = shaderInstanceGetUniformLocation(gs, "lutMap");
            r->shadowProgramReady =
                r->shadowSunDirLocation >= 0 && r->shadowSunColourLocation >= 0 &&
                r->shadowSkyLocation >= 0 && r->shadowGroundLocation >= 0 &&
                r->shadowCameraLocation >= 0 && r->shadowParamsLocation >= 0 &&
                r->shadowSunMtxLocation >= 0 && r->shadowLutMapLocation >= 0;
        }
        if (!r->shadowProgramReady) {
            shaderProgramFree(&r->programShadow);
        }
    }
    // ... and THE CASTER PROGRAM: the DVLB's fourth DVLE, mdk_caster.v.pica, a
    // vertex shader on its own. Sun shadows need both.
    if (r->shadowProgramReady && r->dvlb->numDVLE >= 4 &&
        R_SUCCEEDED(shaderProgramInit(&r->programCaster))) {
        if (R_SUCCEEDED(shaderProgramSetVsh(&r->programCaster, &r->dvlb->DVLE[3]))) {
            r->casterMvpLocation =
                shaderInstanceGetUniformLocation(r->programCaster.vertexShader, "mvp");
            r->casterProgramReady = r->casterMvpLocation >= 0;
        }
        if (!r->casterProgramReady) {
            shaderProgramFree(&r->programCaster);
        }
    }
    // THE PASS-THROUGH PROGRAM: the vertex shader plus the DVLB's fifth DVLE,
    // mdk_pass.g.pica -- an unlit draw that keeps the PICA in geometry mode
    // (03_frame.inc bindProgram has the hardware rule). Optional: without it
    // every return from a geometry program to an unlit draw is a split.
    if (r->litReady && r->dvlb->numDVLE >= 5 && R_SUCCEEDED(shaderProgramInit(&r->programPass))) {
        r->passReady = R_SUCCEEDED(shaderProgramSetVsh(&r->programPass, &r->dvlb->DVLE[0])) &&
                       R_SUCCEEDED(shaderProgramSetGsh(&r->programPass, &r->dvlb->DVLE[4], 12));
        if (!r->passReady) {
            shaderProgramFree(&r->programPass);
        }
    }
    // The vertex shader's uniform registers run from c0 to the world matrix's
    // last row (mdk_c3d.v.pica: mvp c0-3, uvtransform, tint, haze, world c7-10).
    r->vshUniformVecs = r->worldLocation + 4;
    mdk::log3dsPrintf("pass program: %s; vertex uniforms c0-c%d",
                      r->passReady ? "ready" : "UNAVAILABLE", (int)r->vshUniformVecs - 1);
    mdk::log3dsPrintf("shadow programs: lit+shadow %s (sun=%d sky=%d ground=%d camera=%d "
                      "params=%d mtx=%d lut=%d), caster %s (mvp=%d)",
                      r->shadowProgramReady ? "ready" : "UNAVAILABLE",
                      (int)r->shadowSunDirLocation, (int)r->shadowSkyLocation,
                      (int)r->shadowGroundLocation, (int)r->shadowCameraLocation,
                      (int)r->shadowParamsLocation, (int)r->shadowSunMtxLocation,
                      (int)r->shadowLutMapLocation,
                      r->casterProgramReady ? "ready" : "UNAVAILABLE", (int)r->casterMvpLocation);
    // THE IDLE TEXTURE for units 1 and 2 (01_types.inc unitIdle): 8x8 opaque
    // white, linear heap, made once. Without it the shadowed lit draws are off
    // (they would leave a surface bound on unit 1 for every later fragment).
    r->unitIdleReady = C3D_TexInit(&r->unitIdle, 8, 8, GPU_RGBA8);
    if (r->unitIdleReady) {
        memset(r->unitIdle.data, 0xFF, 8 * 8 * 4);
        C3D_TexFlush(&r->unitIdle);
        C3D_TexSetFilter(&r->unitIdle, GPU_NEAREST, GPU_NEAREST);
    }
    mdk::log3dsPrintf("uniforms: mvp=%d uvtransform=%d tint=%d haze=%d", (int)r->mvpLocation,
                      (int)r->uvLocation, (int)r->tintLocation, (int)r->hazeLocation);
    // A NAME THE SHADER DOES NOT EXPORT is not a location, and citro3d does not
    // bounds-check one: `C3D_FVUnifWritePtr` (c3d/uniforms.h:15-21) indexes
    // `C3D_FVUnifDirty[type][id+i]` and `C3D_FVUnif[type][id]` with no test, so a
    // -1 from a stale or stripped .shbin is a write before a .bss array on every
    // eye and every canvas draw -- silent corruption where a refusal belongs.
    // The Makefile has explicit anti-staleness rules for the shader, so this is a
    // narrow window; it is still the only place it can be caught (review-9 F5).
    if (r->mvpLocation < 0 || r->uvLocation < 0 || r->tintLocation < 0 ||
        r->hazeLocation < 0 || r->worldLocation < 0) {
        mdk::log3dsPrintf("shader is missing a uniform it must export -- refusing");
        shaderProgramFree(&r->program);
        return nullptr;   // the same refusal every other create failure uses
    }

    r->scratchSize = kGpuScratchSize;
    r->scratch = linearAlloc(r->scratchSize);
    if (!r->scratch) {
        mdk::log3dsPrintf("linearAlloc(%u) FAILED", (unsigned)r->scratchSize);
        shaderProgramFree(&r->program);
        DVLB_Free(r->dvlb);
        releaseEyeTargets(r);
        C3D_Fini();
        delete r;
        return nullptr;
    }

    mdk::log3dsPrintf("renderer created: target=%p dvlb=%p numDVLE=%d scratch=%p",
                      (void*)r->targetLeft, (void*)r->dvlb, (int)r->dvlb->numDVLE, r->scratch);
    r->eyeSeparation = desc ? (float)desc->stereo.separation : 0.0f;
    if (desc) {
        r->toggles = desc->enhanced_toggles;
    }

    r->ready = true;
    r->hdReader.start();

    // THE CANVAS'S WHITE TEXTURE, made once here and never released.
    //
    // A flat canvas quad (the health bar, the damage flash, the selection box) is
    // a colour with no texture, and this combiner takes its colour from the
    // texture times the tint. White is the identity of that multiply, so binding
    // white makes the tint the whole colour.
    //
    // EIGHT texels and not one, which is what `SurfaceCache::flatColourId` uses
    // for the scene's flat surfaces. The UV transform scales a texture by real
    // over padded, and a 1x1 is padded to 8x8 with TRANSPARENT texels, so its
    // scale is 1/8 and a quad's far edge samples the padding instead of the
    // colour. An 8x8 of white has a scale of exactly 1: every uv in [0,1] lands
    // on a white texel whatever the filter does with it.
    //
    // It is made after `ready` because the seam's own texture factory refuses to
    // run before it, and it is never released: the canvas asks for it on any
    // frame, and a texture the backend owns cannot be evicted from under it.
    {
        uint8_t white[8 * 8 * 4];
        for (int i = 0; i < 8 * 8; i++) {
            white[i * 4 + 0] = 255;
            white[i * 4 + 1] = 255;
            white[i * 4 + 2] = 255;
            white[i * 4 + 3] = 255;
        }
        r->canvasWhite = mdk_render_create_rgba_texture(r, 8, 8, white);
        if (r->canvasWhite < 0) {
            mdk::log3dsPrintf("canvas: the white texture FAILED to allocate -- flat "
                              "canvas quads (flashes, bars, the selection box) are dropped");
        }
    }
    return r;
}

 void mdk_render_destroy(MdkRenderer rr) {
    RendererC3D* r = asC3d(rr);
    if (!r) return;

    // The HD reader first: its thread is joined and the buffers it still holds
    // are freed before anything it could name goes away (AGENTS.md P4).
    r->hdReader.stop();
    if (r->scratch) {
        linearFree(r->scratch);
        r->scratch = nullptr;
    }
    // The textures and the meshes are NOT freed here, and that is
    // deliberate.
    //
    // Their memory comes from the linear heap (C3D_TexInit's linear path is
    // a linearAlloc, and so is a mesh's vertex array), and `C3D_Fini` below
    // is what waits for the render queue to drain -- it calls
    // gxCmdQueueWait with an infinite timeout before it stops the queue.
    // Freeing the textures before that wait would hand back memory the GPU
    // may still be reading (AGENTS.md P3); the heap itself goes when the
    // process exits, which is the only caller of this destructor.
    releaseGlow(r);
    releaseShadowMap(r);
    releaseEyeTargets(r);
    if (r->dvlb) {
        if (r->casterProgramReady) {
            shaderProgramFree(&r->programCaster);
        }
        if (r->shadowProgramReady) {
            shaderProgramFree(&r->programShadow);
        }
        if (r->passReady) {
            shaderProgramFree(&r->programPass);
        }
        if (r->litReady) {
            shaderProgramFree(&r->programLit);
        }
        shaderProgramFree(&r->program);
        DVLB_Free(r->dvlb);
        r->dvlb = nullptr;
    }
    C3D_Fini();
    // The glow's linear memory after C3D_Fini, which waited for the queue that
    // may still have been reading it (AGENTS.md P3).
    if (r->glowScratchTiled) {
        linearFree(r->glowScratchTiled);
    }
    // The tint and the idle texture: linear heap, sampled by the queue C3D_Fini
    // has just waited for.
    if (r->shadowTintReady) {
        C3D_TexDelete(&r->shadowTint);
        r->shadowTintReady = false;
    }
    if (r->unitIdleReady) {
        C3D_TexDelete(&r->unitIdle);
        r->unitIdleReady = false;
    }
    if (r->copyDrain) {
        linearFree(r->copyDrain);
        r->copyDrain = nullptr;
    }
    if (r->glowScratch) {
        linearFree(r->glowScratch);
    }
    if (r->glowQuad) {
        linearFree(r->glowQuad);
    }
    if (r->glowLevelQuad) {
        linearFree(r->glowLevelQuad);
    }
    delete r;
}

 const char* mdk_render_backend_name(MdkRenderer r) {
    (void)r;
    return "PICA200 Citro3D";
}

 mdk::MdkCapabilities mdk_render_capabilities(MdkRenderer r) {
    (void)r;
    mdk::MdkCapabilities caps;
    // THE 3DS DOES NOT HAVE THE PC'S ENHANCED LOOK AS A WHOLE: that needs a
    // programmable fragment shader (the tone curve, the ambient occlusion, the
    // post pass) and the PICA200's fragment stage is fixed-function (SPEC.md 5.3,
    // 7.3). What the PICA CAN express lands feature by feature behind the
    // Enhanced Mode toggles instead (SPEC.md 7.9, owner direction 2026-11-17):
    // so far the haze (pica_matrix.h, THE HAZE), bilinear filtering and mip
    // chains (06_draw.inc). This flag stays false because it means the whole look.
    caps.enhanced_look = false;
    caps.msaa = false;              // the PICA200 has no multisampling
    caps.colour_mips = false;       // the PC's colour-texture chains; the 3DS's own
                                    // chains are Enhanced Mode enableMipmaps
    caps.hardware_stereo = true;    // the top screen's parallax barrier
    caps.flat_stereo = false;       // SBS / interlaced composites are for flat displays
    caps.insets = true;             // the PICA viewport is a hardware clip rect
    caps.canvas_filtering = false;  // the palette lookup is fixed-function
    caps.panorama_linear = false;
    caps.max_texture_size = 1024;
    // ONE NUMBER, ONE DEFINITION. This used to say 512 while
    // `mdk_render_canvas_capacity` — the value the queue and the mesh actually
    // enforce — said 4096, so the capability the frontend was told was smaller
    // than the one it got, and a frontend that trusted `caps` would have
    // truncated the menus and the message queue, which want hundreds of quads
    // (review-1 F4). Both now read `kCanvasQuadCapacity`.
    caps.max_canvas_quads = kCanvasQuadCapacity;

    // THE SPRITE RESIDENCY BUDGET.
    //
    // THE ARITHMETIC. The 3DS has six megabytes of VRAM and the linear heap
    // starts there, so everything the GPU reads comes out of it first:
    //
    //   the two eye targets      2 x (400x240 colour + 400x240 depth)  1.5 MiB
    //   the command buffer        C3D_DEFAULT_CMDBUF_SIZE x 2         0.5 MiB
    //   an arena's textures       the expanded, padded RGBA8          ~3.0 MiB
    //                                                            ----------
    //                                                            ~5.0 MiB
    //
    // which leaves about a megabyte for everything else -- and Kurt's
    // sprite frames are 96 KiB each as GPU textures (measured: 49.94 MiB for
    // all 360 frames of TRAVSPRT.BNI, 12.00 MiB for K_CHUTEC alone). So the
    // sprite cache keeps ONE megabyte resident and makes the rest again as
    // they are wanted.
    //
    // THE NUMBER IS AN ESTIMATE from the arithmetic above, not a
    // measurement: the Old-3DS timing and the real residency are still
    // UNMEASURED (AGENTS.md P6). What the bound buys is that the set can
    // never be the 49.94 MiB the eager upload asked for.
    caps.sprite_residency_bytes = 1024 * 1024;
    return caps;
}


