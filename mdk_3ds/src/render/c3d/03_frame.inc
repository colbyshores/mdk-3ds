// 03_frame.inc -- the frame begin (mdk_render_begin_frame), and the dedupe's log line.
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

// THE DEDUPE'S LINE (THE DEDUPE, above): per category, the writes made and the
// writes skipped over the window, and the command list's length in 32-bit words
// a frame. Prints and starts a new window. Cold: once, at teardown.
static void logDedupeWindow(RendererC3D* r) {
    if (r->cmdWordsFrames == 0) {
        return;
    }
    const int32_t* w = r->dedupeWritten;
    const int32_t* k = r->dedupeSkipped;
    mdk::log3dsPrintf("dedupe: frame %d: skipped/written over %d frames: stage0 %d/%d "
                      "bufinfo %d/%d uv %d/%d tint %d/%d mvp %d/%d world %d/%d "
                      "texbind %d/%d; command list %lu words in all, %u in the "
                      "longest frame",
                      (int)r->frame, (int)r->cmdWordsFrames,
                      (int)k[kDedupeTexEnv0], (int)w[kDedupeTexEnv0],
                      (int)k[kDedupeBufInfo], (int)w[kDedupeBufInfo],
                      (int)k[kDedupeUv], (int)w[kDedupeUv],
                      (int)k[kDedupeTint], (int)w[kDedupeTint],
                      (int)k[kDedupeMvp], (int)w[kDedupeMvp],
                      (int)k[kDedupeWorld], (int)w[kDedupeWorld],
                      (int)k[kDedupeTexBind], (int)w[kDedupeTexBind],
                      (unsigned long)r->cmdWordsSum, (unsigned)r->cmdWordsPeak);
    for (int c = 0; c < kDedupeCategories; c++) {
        r->dedupeWritten[c] = 0;
        r->dedupeSkipped[c] = 0;
    }
    r->cmdWordsSum = 0;
    r->cmdWordsPeak = 0;
    r->cmdWordsFrames = 0;
}

void mdk_render_begin_frame(MdkRenderer rr, const MdkFrameParams* params) {
    RendererC3D* r = asC3d(rr);
    if (!r || !r->ready) return;
    if (r->inFrame) return;   // a begin without an end is a bug, not a re-entry

    // EDGE SMOOTHING follows its toggle here, OUTSIDE the frame: the rebuild
    // deletes the eye targets, and C3D_RenderTargetDelete waits for the GPU queue
    // first (and breaks inside a frame), so nothing in flight still reads them.
    applyEdgeSmoothing(r);
    // A changed shadowMapSize (config.ini; the menu has no row for it) remakes
    // the map, here, between frames: the next frame that casts makes it at the
    // new size. Against the size ASKED for when the map was made (or refused),
    // not the one made: a map VRAM held to a smaller size would otherwise be
    // remade every frame. A refusal is forgotten too, so a smaller size is tried.
    if ((r->shadowMapReady || r->shadowRefused) &&
        r->toggles.shadowMapSize != r->shadowMapAsked) {
        releaseShadowMap(r);
        r->shadowMapAsked = r->toggles.shadowMapSize;
    }
    if (!r->ready) return;
    // The native shadow-write test, once, before the first frame that may cast
    // a sun shadow (testNativeShadowWrites): outside the frame, like the rebuild.
    if (!r->shadowNativeTested && r->toggles.enableSkyShadows && r->casterProgramReady &&
        r->shadowProgramReady) {
        testNativeShadowWrites(r);
    }

    // C3D_FrameBegin returns FALSE when it could not begin (with
    // C3D_FRAME_NONBLOCK). A failed begin does NOT authorise drawing.
    if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) {
        return;
    }
    r->inFrame = true;
    r->frame++;
    // The frustum belongs to an installed eye; a frame that has not installed
    // one yet has none (installEye sets it before each per-eye replay).
    r->frustumValid = false;
    // THE COMMAND LIST'S LENGTH (the dedupe's evidence): libctru's write
    // position, which only moves forward through the frame's splits
    // (GPUCMD_Split advances gpuCmdBuf by what it hands over).
    r->cmdFrameStart = gpuCmdBuf + gpuCmdBufOffset;
    // A new command list: nothing of this frame has used a geometry program.
    r->gsInList = false;
    r->geometrySplits = 0;
    r->drawCount = 0;
    r->triangleCount = 0;
    // A capture request: counted, never honoured (mdk_render_capture_stats).
    if (params && params->screenshot_path && *params->screenshot_path) {
        r->capturesRequested++;
    }
    // C3D_FrameBegin waited for the previous queue above. The old frame's
    // GPU reads are complete, so deferred releases and replacements can be
    // made now (AGENTS.md P3).
    r->gpuPending = false;
    // THE FRAME'S BOUNDARY FOR THE EVICTION. A texture stamped at or after
    // this has been used by THIS frame's commands, and the GPU may still be
    // reading it, so it cannot be freed (AGENTS.md P3).
    // The frame that just ENDED drew stamps from the old boundary on: the HD
    // pump keeps those (the view's working set) and imports within the rest.
    const uint32_t previousFrameStart = r->frameStartUse;
    r->frameStartUse = r->drawCounter + 1;
    settleDeferredTextures(r);
    settleDeferredMeshes(r);
    pumpExpansions(r);
    // The orig pack's reads before the HD stream's: a packed original is a
    // surface about to draw, an HD copy only replaces one that already does.
    pumpPackOriginals(r, previousFrameStart);
    pumpHdTextures(r, previousFrameStart);
    // THE STORES THE TEXTURES WERE MADE IN (EnhancedToggles::texture16): once a
    // level's textures are made (frame 30), then every 300 frames when more were
    // made since (an arena streamed in, an evicted texture made again). The
    // makings are counted, so a texture made twice counts twice; the resident
    // half is a walk of the table (a few hundred entries, at most once per 300
    // frames). Cold: this is the A/B's evidence that the store changed at all.
    {
        const int32_t made = r->texturesMade[0] + r->texturesMade[1] + r->texturesMade[2] +
                             r->texturesMade[3] + r->compressedMade;
        // Frame 30, once (one write, no SD write inside play after it).
        if (r->frame == 30 && made != r->texturesMadeLogged) {
            r->texturesMadeLogged = made;
            int32_t resident16 = 0;
            int32_t resident = 0;
            size_t residentBytes = 0;
            size_t residentRgba8Bytes = 0;
            for (size_t i = 0; i < r->textures.size(); i++) {
                const C3dTexture& t = r->textures[i];
                if (!t.valid) {
                    continue;
                }
                resident++;
                if (t.format != mdk::pica::TexelFormat::Rgba8) {
                    resident16++;
                }
                residentBytes += mdk::pica::textureBytes(t.format, t.paddedWidth, t.paddedHeight,
                                                         t.mipLevels);
                residentRgba8Bytes += mdk::pica::textureBytes(
                    mdk::pica::TexelFormat::Rgba8, t.paddedWidth, t.paddedHeight, t.mipLevels);
            }
            mdk::log3dsPrintf("textures: frame %d texture16=%d: made %d rgb565, %d rgba5551, "
                              "%d rgba4, %d rgba8, %u bytes saved vs rgba8; kurtCompressed=%d: "
                              "%d etc imported, %u bytes saved; resident %d (%d "
                              "not rgba8) %u bytes of %u as rgba8",
                              (int)r->frame, (int)r->toggles.texture16, (int)r->texturesMade[0],
                              (int)r->texturesMade[1], (int)r->texturesMade[2],
                              (int)r->texturesMade[3], (unsigned)r->textureBytesSaved,
                              (int)r->toggles.kurtCompressed, (int)r->compressedMade,
                              (unsigned)r->compressedBytesSaved,
                              (int)resident, (int)resident16, (unsigned)residentBytes,
                              (unsigned)residentRgba8Bytes);
        }
    }
    r->queue.reset();

    // THE CLEAR COLOUR IS CONVERTED, NOT PASSED THROUGH. The seam carries
    // 0xAARRGGBB (what the Vulkan backend unpacks, and what both frontends
    // therefore write); the PICA's fill pattern is 0xRRGGBBAA. Handing the seam
    // value to C3D_RenderTargetClear directly painted the panel with the alpha
    // byte as red — measured, 0xFF102030 became #ff1020. See pica::clearColour.
    const uint32_t clear =
        mdk::pica::clearColour(params ? params->clear_colour : 0xFF000000u);

    // THE EYE PLAN, read once, here, and stored on the renderer.
    //
    // The eye passes happen at the END of the frame -- see mdk_render_end_frame:
    // the draws are queued between begin and end, so an eye installed here is
    // installed before there is anything to draw into it, and every draw then
    // lands in whichever eye was installed last. Installing both here, as this
    // function used to, meant they all landed in the RIGHT target while the top
    // screen shows the LEFT one: a frame of nothing but the clear colour, on a
    // backend nobody can look at from this host.
    //
    // The slider must still be read here and not at the end, so that the two
    // eyes cannot disagree if the user moves it mid-frame.
    //
    // THE SLIDER MUST BE CLAMPED. Mandarine returns a garbage value (~134
    // million) when it does not model the slider, and Forsaken shipped a bug
    // from exactly that: an unclamped depth became an eye separation of
    // billions of units and the screen went black. The clamp, the sign of each
    // eye and the mono skip are pica::eyePlan, which a host test pins.

    // With the parallax barrier off (gfxSet3D(false): the frontend's scope in
    // 2D, EnhancedToggles::scopeMono) the panel shows the left buffer alone,
    // so the frame is planned as with the slider closed: one eye, no offset.
    const float slider = gfxIs3D() ? (float)osGet3DSliderState() : 0.0f;
    r->eyes = mdk::pica::eyePlan(slider, r->eyeSeparation);

    // Both eye buffers are cleared here, whether or not both get drawn: a buffer
    // that is not cleared keeps the previous frame's picture, and the right
    // buffer starts being displayed the moment the slider goes up.
    //
    // The depth is cleared to the BUFFER'S MAXIMUM.
    //
    // The frame's depth map stores 0 at the near plane and 1 at the far, so
    // the value that accepts every fragment before one is drawn is the top
    // of the buffer, not the bottom. The devkitPro examples clear to 0 and
    // that is correct for them: citro3d's own default map is the reversed
    // range (see mdk::pica::kDepthMapScale for both pairings). Clearing to
    // 0 with this map rejects everything, which is a screen showing only the
    // clear colour and no error.
    //
    // The value is the raw fill pattern GX_MemoryFill writes into the
    // DEPTH24_STENCIL8 buffer (measured: C3D_FrameBufClear passes
    // `clearDepth` straight to GX_MemoryFill as the fill value), and
    // 0x00FFFFFF is the 24-bit maximum under either byte order of the word.
    //
    // THE BACKDROP KEEP (the scripts' sky mode -1, e.g. level 3's closed HMO_3; C#
    // Backdrop.Keep loads the scene target instead of clearing it; package #15): each
    // eye's colour buffer keeps the last frame, so only the depth is cleared. The right
    // eye then also keeps its own last picture, which is the one it showed.
    const bool keepColour = params && params->backdrop == mdk::Backdrop::Keep;
    C3D_RenderTargetClear(r->targetLeft, keepColour ? C3D_CLEAR_DEPTH : C3D_CLEAR_ALL, clear,
                          mdk::pica::kDepthClearValue);
    // At edge-smoothing scale 2 the right eye's depth is the LEFT eye's buffer,
    // just filled above and filled again before the right eye draws (installEye),
    // so only its colour is cleared here: a third 1.5 MB depth fill is wasted fill
    // bandwidth on the Old 3DS. (Under Keep there is then nothing to clear on it.)
    if (!(keepColour && r->targetsShareDepth)) {
        C3D_RenderTargetClear(r->targetRight,
                              keepColour ? C3D_CLEAR_DEPTH
                                         : (r->targetsShareDepth ? C3D_CLEAR_COLOR : C3D_CLEAR_ALL),
                              clear, mdk::pica::kDepthClearValue);
    }
}

 void mdk_render_end_frame(MdkRenderer rr) {
    RendererC3D* r = asC3d(rr);
    if (!r || !r->inFrame) return;
    // THE PER-EYE REPLAY, and it has to happen HERE rather than in
    // mdk_render_begin_frame: the draws are queued between the two, so this is
    // the first moment there is anything to draw, and the last one before the
    // queue is submitted.
    //
    // One pass per eye, each with its own target and matrix installed
    // immediately before its own replay. eyePlan decides how many there are:
    // ONE when the 3D slider is closed, because the panel then shows the left
    // buffer and drawing the right one is invisible work on a 268 MHz ARM11 --
    // Forsaken measures the same cost on the same hardware ("two full render
    // passes ~ 2x CPU+GPU cost ... Slider = 0 disables stereo and restores full
    // framerate", 3DS_PORT_NOTES.md).
    //
    // The queue is reset after the LAST eye, not inside the loop: the second eye
    // replays the same records.
    // The diagnostic is drawn INSIDE the loop, once per eye, so each eye gets
    // its own copy in its own target: drawn once after the loop it would be a
    // triangle in one eye and a hole in the other.
    // THE LOOK DIALS (config / the pause menu's sliders) over the game's rig,
    // every frame before anything reads it: the haze exponent, the lit and
    // shadow programs' uniforms and the shadow tint below all take the dialled
    // rig. At the defaults (100) it is the rig, copied.
    mdk::pica::dialledLighting(r->rig, r->toggles, &r->lighting);
    prepareHaze(r);
    prepareLighting(r);
    // The sun's shadow map (Enhanced Mode enableSkyShadows), once per frame and
    // before either eye: both eyes read the one map.
    prepareShadows(r);
    castShadows(r);
    // The glow (Enhanced Mode enableGlowBillboards) follows the enhanced rig, as
    // the PC's post pass does; a session whose glow memory failed stays off.
    // Billboards by default (the owner's 2026-11-18 decision); the PC's bloom,
    // ported, with config glowBloom.
    r->glowLive = r->toggles.enableGlowBillboards && r->toggles.glowBloom &&
                  r->lighting.enabled && !r->glowRefused;
    r->billboardsLive = r->toggles.enableGlowBillboards && !r->toggles.glowBloom &&
                        r->lighting.enabled;
    r->billboardsDrawn = 0;
    // Resolve the queue once for both eyes (STEREO-REPLAY analogue,
    // 06_queue.inc prepareQueue): the eye loop below only culls, composes
    // matrices and submits.
    // A frame that draws no eye resolves nothing (validator F4; eyePlan
    // only yields 1-2 today, so this is the guard, not a behaviour change).
    if (r->eyes.passes > 0) {
        prepareQueue(r);
    }
    for (int eye = 0; eye < r->eyes.passes; eye++) {
        installEye(r, eye == 0 ? r->targetLeft : r->targetRight, r->eyes.offset[eye]);
        replayQueue(r);
        // The canvas's own width, because the canary is drawn in the canvas's own
        // pixel space and that space is what `canvasMatrix` projects.
        // drawDiagnostic returns at once unless the canary is armed by a file
        // (canaryArmed): the eye loop's statements stay unconditional.
        drawDiagnostic(r, mdk_render_canvas_width(rr));
    }
    // THE CANVAS LINE, and it is the evidence `mdk_3ds/tools/emurun.sh --expect
    // hud` reads. Why a log line and not a pixel: the HUD's own colours are white,
    // grey and black, so a screenshot cannot tell "the HUD drew" from "the HUD was
    // queued and dropped", and the two numbers on this line can -- `quads` is what
    // the frontend asked for and `draws` is what the per-eye replay actually
    // emitted, so a canvas that was queued but never drawn, or drawn into one eye
    // only, is a different pair of numbers and not the same picture.
    //
    // On the first frame: a line every frame is a line nobody reads.
    //
    // IT PRINTS EVEN WHEN THE ANSWER IS ZERO, and that is the whole point of the
    // line being evidence (review-9 F3). `emurun.sh --expect hud` reads the LAST
    // line, on the argument that the last line is the one that says the HUD was
    // still drawing when the run ended. A line printed only when quads > 0 cannot
    // honour that: a HUD that stops mid-run simply stops printing, and the last
    // good line keeps the gate green. Zero has to be sayable for the gate to be
    // able to say it.
    {
        r->canvasFrames++;
        if (r->canvasFrames == 1) {
            mdk::log3dsPrintf("canvas: quads=%d draws=%d mesh=%d corners=%d eyes=%d "
                              "(canvas frame %d)",
                              (int)r->canvasQuadsThisFrame, (int)r->canvasDrawsThisFrame,
                              (int)r->canvasMesh, (int)r->canvasVerts.size(),
                              (int)r->eyes.passes, (int)r->canvasFrames);
        }
    }
    // THE DEDUPE'S LINE (03_frame_state.inc, THE DEDUPE): per category, the writes
    // made and the writes skipped over the window, and the command list's length
    // in 32-bit words a frame (C3D_FrameBegin to here, every split included).
    {
        const u32* now = gpuCmdBuf + gpuCmdBufOffset;
        if (r->cmdFrameStart && now >= r->cmdFrameStart) {
            const uint32_t words = (uint32_t)(now - r->cmdFrameStart);
            r->cmdWordsSum += words;
            r->cmdWordsPeak = words > r->cmdWordsPeak ? words : r->cmdWordsPeak;
            r->cmdWordsFrames++;
        }
        // Printed once, at teardown (mdk_render_destroy): an SD write inside a
        // play frame costs 25-30 ms.
    }
    // The halos are this frame's: a frame whose game hands none over (no
    // runtime) must not draw the last frame's again (review, 2026-11-19).
    r->glowQuadCount = 0;
    r->glowPasses = 0;
    r->canvasQuadsThisFrame = 0;
    r->canvasDrawsThisFrame = 0;
    r->queue.reset();
    r->insetOverDraws.clear();
    r->insetOpen = false;
    r->sceneWindowOn = false;
    // C3D_FrameEnd is the ONLY present path. It triggers the auto display
    // transfer set up by C3D_RenderTargetSetOutput.
    C3D_FrameEnd(0);
    r->inFrame = false;
    // THE NATIVE WRITES, SEEN ON THE REAL PASS, ONCE. testNativeShadowWrites
    // decides from 8x8 quads; the emulator's native writes pass that and land
    // nowhere in the scene's caster pass (level 8 frame 60: 0 texels), which is
    // why the ramp is taken wherever it is exact, and native only where the ramp
    // measured broken (the Old 3DS: 13,285 texels, the emulator's ramp map's own,
    // depths equal to 1/65536). So the first native frames have their map read
    // back, as evidence: the first that wrote any settles it; three in a row
    // with casters drawn and nothing written settle it too, and the map stays
    // native -- a frame CAN legitimately write nothing (every caster back-facing
    // the sun, or outside the map's box), and the only other path is the one
    // that cannot work on this GPU (review, 2026-11-18: demoting to the ramp here
    // put a console on it for the session).
    if (r->shadowNative && !r->shadowNativeVerified && r->shadowLive && r->shadowCasters > 0 &&
        r->shadowMapReady && r->shadowMapSize > 0) {
        const u32 side = (u32)r->shadowMapSize;
        const size_t bytes = (size_t)side * side * 4;
        u32* copy = static_cast<u32*>(linearAlloc(bytes));
        if (copy) {
            const u32 rowDim = GX_BUFFER_DIM((side * 8u * 4u) >> 4, 0);
            const bool read = readBack(r, r->shadowTex.data, rowDim, copy, bytes, 0xFF);
            uint32_t written = 0;
            for (size_t i = 0; i < (size_t)side * side && read; i++) {
                if (copy[i] != 0xFFFFFFFFu) {
                    written++;
                }
            }
            linearFree(copy);
            if (written > 0) {
                r->shadowNativeVerified = true;
                r->shadowNativeEmptyFrames = 0;
                mdk::log3dsPrintf("shadows: native writes verified in the caster pass (%d "
                                  "casters, %u texels written)",
                                  (int)r->shadowCasters, (unsigned)written);
            } else if (++r->shadowNativeEmptyFrames >= 3) {
                // Settled without a sighting: native writes are only chosen where
                // the ramp measured BROKEN, so there is nothing better to fall
                // back to (the ramp shadows the floor with itself there); the
                // readbacks stop, the map stays native.
                r->shadowNativeVerified = true;
                mdk::log3dsPrintf("shadows: native writes not seen in the caster pass (%d "
                                  "casters, 0 texels, 3 frames running); kept (the ramp "
                                  "measured broken on this GPU)", (int)r->shadowCasters);
            }
        }
    }
    // C3D_FrameEnd submits the queue; it does not wait for it. Until the
    // next C3D_FrameBegin, the GPU may still be reading this frame's
    // textures, so they cannot be freed or replaced (AGENTS.md P3).
    r->gpuPending = true;
}

 void mdk_render_present(MdkRenderer rr) {
    // The present IS the frame end on this backend. Kept as a separate seam
    // entry so the Linux backend can differ, but there is nothing to do.
    (void)rr;
}

 void mdk_render_set_camera(MdkRenderer rr, const mdk::Camera* cam) {
    RendererC3D* r = asC3d(rr);
    if (!r || !cam) return;

    // The camera is KEPT, not baked: each eye of the 3DS's stereo pair needs
    // its own offset, and the offset is applied to the view matrix.
    memcpy(r->view, cam->view, sizeof(r->view));
    memcpy(r->projection, cam->projection, sizeof(r->projection));
    // The eye's world position, for the lit program's normal flip.
    r->cameraPos[0] = cam->position.x;
    r->cameraPos[1] = cam->position.y;
    r->cameraPos[2] = cam->position.z;
}

 void mdk_render_set_lighting(MdkRenderer rr, const mdk::Lighting* light) {
    // The rig is shared DATA (SPEC.md 5.3). The 3DS consumes what its PICA200
    // can express behind each Enhanced Mode toggle (SPEC.md 7.9): so far the
    // haze. The game hands it over when the arena changes, not per frame.
    RendererC3D* r = asC3d(rr);
    if (!r || !light) {
        return;
    }
    r->rig = *light;
    mdk::pica::dialledLighting(r->rig, r->toggles, &r->lighting);
    // The rig, once per change (the game hands it over per arena): the numbers
    // the lit program's light is made of.
    mdk::log3dsPrintf("light rig: sun dir (%.2f,%.2f,%.2f) rgb (%.2f,%.2f,%.2f) sky (%.2f,%.2f,%.2f) "
                      "ground (%.2f,%.2f,%.2f) exposure %.3f knee %.3f",
                      light->sun_direction.x, light->sun_direction.y, light->sun_direction.z,
                      light->sun.r, light->sun.g, light->sun.b, light->sky.r, light->sky.g,
                      light->sky.b, light->ground.r, light->ground.g, light->ground.b,
                      light->exposure, light->sky.a);
}

