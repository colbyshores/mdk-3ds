// 02_lifecycle.inc -- create/destroy, the backend name and capabilities.
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

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

    // The top screen's eye pair. 240x400 portrait; the projection rotates it.
    r->targetLeft = C3D_RenderTargetCreate(kScreenWidth, kScreenHeight,
                                           GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!r->targetLeft) {
        mdk::log3dsPrintf("C3D_RenderTargetCreate(left) FAILED");
        C3D_Fini();
        delete r;
        return nullptr;
    }
    r->targetRight = C3D_RenderTargetCreate(kScreenWidth, kScreenHeight,
                                             GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!r->targetRight) {
        mdk::log3dsPrintf("C3D_RenderTargetCreate(right) FAILED");
        C3D_RenderTargetDelete(r->targetLeft);
        C3D_Fini();
        delete r;
        return nullptr;
    }
    C3D_RenderTargetSetOutput(r->targetLeft, GFX_TOP, GFX_LEFT,
                              MDK_DISPLAY_TRANSFER_FLAGS);
    C3D_RenderTargetSetOutput(r->targetRight, GFX_TOP, GFX_RIGHT,
                              MDK_DISPLAY_TRANSFER_FLAGS);

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
        C3D_RenderTargetDelete(r->targetLeft);
        C3D_RenderTargetDelete(r->targetRight);
        C3D_Fini();
        delete r;
        return nullptr;
    }
    const Result spInit = shaderProgramInit(&r->program);
    if (R_FAILED(spInit)) {
        mdk::log3dsPrintf("shaderProgramInit FAILED result=0x%08lx", (unsigned long)spInit);
        DVLB_Free(r->dvlb);
        C3D_RenderTargetDelete(r->targetLeft);
        C3D_RenderTargetDelete(r->targetRight);
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
        C3D_RenderTargetDelete(r->targetLeft);
        C3D_RenderTargetDelete(r->targetRight);
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
        r->hazeLocation < 0) {
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
        C3D_RenderTargetDelete(r->targetLeft);
        C3D_RenderTargetDelete(r->targetRight);
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
    if (r->targetLeft) {
        C3D_RenderTargetDelete(r->targetLeft);
        r->targetLeft = nullptr;
    }
    if (r->targetRight) {
        C3D_RenderTargetDelete(r->targetRight);
        r->targetRight = nullptr;
    }
    if (r->dvlb) {
        shaderProgramFree(&r->program);
        DVLB_Free(r->dvlb);
        r->dvlb = nullptr;
    }
    C3D_Fini();
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


