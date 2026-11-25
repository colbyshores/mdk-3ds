// 03_toggles.inc -- Enhanced Mode between frames: the haze, the HD-only originals, the toggles' setter, the 30 fps cap.
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

static void prepareHaze(RendererC3D* r) {
    // pica::hazeApplies with the draw-independent half decided here, once; the
    // per-draw half (shading, pass) is drawRecorded's.
    r->hazeLive = mdk::pica::hazeApplies(r->toggles.enableHaze, r->lighting, mdk::Shading::Lit,
                                         mdk::Pass::Solid);
    r->hazeExponent = r->hazeLive ? mdk::pica::hazeExponent(r->lighting.haze_density) : 0.0f;
}

// THE ORIGINALS OF THE HD-ONLY TEXTURES WHOSE SET COPY IS COMING (spec 3.1/3.2):
// with the toggle on they are never drawn (06_draw.inc draws the HD copy, or
// nothing until it lands), so their GPU copies are given back. `onlyResident`:
// only those whose copy is already resident (the frame boundary's pass); else
// every one whose copy is not given up for good (the toggle-on pass, before the
// hold reads the set, so the set and the originals are never resident together).
// A copy that fails later leaves its texture to the packed original, which is
// read again like any evicted texture (packPath kept). The caller guarantees the
// GPU no longer reads them (P3). True when an original is still waiting for its
// copy (only with onlyResident).
static bool freeHdOnlyOriginals(RendererC3D* renderer, bool onlyResident) {
    bool waiting = false;
    for (size_t i = 0; i < renderer->textures.size(); i++) {
        C3dTexture& texture = renderer->textures[i];
        if (!texture.hdOnly || !texture.valid || texture.releasePending ||
            texture.hdCopy < 0 || (size_t)texture.hdCopy >= renderer->hdCopies.size()) {
            continue;
        }
        const HdCopy& copy = renderer->hdCopies[(size_t)texture.hdCopy];
        if (!copy.pinned) {
            continue;
        }
        if (onlyResident && copy.state != HdCopy::Resident) {
            waiting = true;
            continue;
        }
        if (copy.state == HdCopy::Failed && (copy.broken || copy.failures >= kHdMaxFailures)) {
            continue;
        }
        C3D_TexDelete(&texture.tex);
        texture.valid = false;
    }
    return waiting;
}

void mdk_render_set_enhanced_toggles(MdkRenderer rr, const mdk::EnhancedToggles* toggles) {
    RendererC3D* r = asC3d(rr);
    if (!r || !toggles) {
        return;
    }
    const bool hdBackOn = !r->toggles.enableHdTextures && toggles->enableHdTextures;
    if (hdBackOn) {
        r->hdFreeOriginals = true;
    }
    r->toggles = *toggles;
    // THE TOGGLE BACK ON, BETWEEN FRAMES (the pause menu, before its hold): the
    // HD-off stretch's originals go NOW, not once their copies are resident, or
    // the set is read while both are in the linear heap (measured on the Old 3DS
    // walk: 0.27 MB free on level 8 right after the hold). The last frame's
    // commands may still be running: gxCmdQueueWait waits for the GX queue
    // citro3d submitted (C3D_FrameBegin's own wait; libctru 2.7.0's
    // gxCmdQueueWait ignores its queue argument and waits on the one running --
    // read off libctru.a's gxqueue.o -- so the wait needs no citro3d internals).
    if (hdBackOn && !r->inFrame && r->hdPinnedMode) {
        gxCmdQueueWait(nullptr, -1);
        r->gpuPending = false;
        freeHdOnlyOriginals(r, false);
    }
    mdk::pica::dialledLighting(r->rig, r->toggles, &r->lighting);
    applyFramePacing(r);
}

// THE 30 FPS CAP (Enhanced Mode cap30Fps): citro3d's OWN frame pacing, not a
// wait of ours. C3D_FrameRate(fps) sets how many vblanks make one tick of the
// frame counters that C3D_FrameBegin(C3D_FRAME_SYNCDRAW) -- this renderer's
// begin -- already waits on (libcitro3d's onVBlank0/1 subtract fps from a
// 60-per-vblank budget; C3D_FrameSync waits for both counters to move). At 30
// a frame is presented every second vblank; at 60, every one, as before. No
// VBlank wait, swap or flush is added (AGENTS.md P3). The game's clock is the
// wall clock (main_3ds.cpp nowSeconds), so the fixed-step accumulator runs two
// 1/60 s steps a 30 fps frame and the game keeps its speed. Called at create
// and on every toggles change; C3D_FrameRate only when the rate changes.
static void applyFramePacing(RendererC3D* r) {
    const float want = r->toggles.cap30Fps ? 30.0f : 60.0f;
    if (want != r->pacedFps) {
        C3D_FrameRate(want);
        r->pacedFps = want;
        mdk::log3dsPrintf("pacing: %d fps (cap30Fps=%d)", (int)want, (int)r->toggles.cap30Fps);
    }
}

 void mdk_render_set_glow_quads(MdkRenderer rr, const mdk::GlowQuad* quads, int32_t count) {
    RendererC3D* r = asC3d(rr);
    if (!r) {
        return;
    }
    const int32_t kept = count < 0 ? 0 : (count > RendererC3D::kMaxGlowQuads
                                              ? RendererC3D::kMaxGlowQuads : count);
    if (quads && kept > 0) {
        memcpy(r->glowQuads, quads, sizeof(mdk::GlowQuad) * (size_t)kept);
    }
    r->glowQuadCount = quads ? kept : 0;
}

 void mdk_render_set_point_lights(MdkRenderer rr, const mdk::PointLight* lights, int32_t count) {
    (void)rr;
    (void)lights;
    (void)count;
}

 void mdk_render_set_panorama(MdkRenderer rr, const mdk::Panorama* p) {
    (void)rr;
    (void)p;
}

 void mdk_render_set_fade(MdkRenderer rr, float level) {
    (void)rr;
    (void)level;
}

 void mdk_render_set_gamma(MdkRenderer rr, float gamma) {
    (void)rr;
    (void)gamma;
}

