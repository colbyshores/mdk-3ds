// 06_queue.inc -- the frame's queue: coalescing, the prepare walk, the per-eye replay.
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

// One replay of everything the frame has queued, in the oracle's pass order
// (draw_queue.h), into whatever target is current. It is separate from the
// frame end because a stereo frame replays the SAME queue once per eye: the
// queue already IS the recorded draw list, so the second eye is a replay with a
// different matrix and target, not a second walk of the scene.
//
// THE ONLY REPLAY POINT IS mdk_render_end_frame, once per eye. A mid-frame
// replay is not a harmless early flush: it emits the frame's records an extra
// time into whatever target installEye last selected, and a flushDraws-shaped
// one (this replay plus a reset) consumes the records as well, so the eye loop
// that follows would replay an empty queue. That is why the canvas and inset
// seams below do not flush (review-1 F4) -- and, since review-3 F1, why none of
// them may draw by itself either: an immediate C3D_DrawArrays in a seam is
// outside the per-eye replay, so it lands in one target or in none.
// Could the second of two adjacent prepared draws be folded into the
// first's DrawArrays? Strict: same mesh and contiguous range, every
// submit-state field equal (byte compares only -- no float predicate).
// Non-cullable only: a cullable record's per-eye frustum test uses its
// own bounds, and folding would submit or drop the wrong range.
static bool coalesciblePair(const PreparedDraw* a, const PreparedDraw* b) {
    const mdk::MeshDraw* da = a->draw;
    const mdk::MeshDraw* db = b->draw;
    if (!da || !db || a->meshIndex != b->meshIndex) return false;
    if (da->cullable || db->cullable) return false;
    if (da->first_vertex + a->vertCount != db->first_vertex) return false;
    // Whole triangles only: folding two partial-primitive ranges would
    // emit triangles neither draw emitted (validator F1).
    if (!a->wholeTriangles || !b->wholeTriangles) return false;
    // Triangle-list only (render_types.h has no strip/fan, and this
    // backend submits every range as a triangle list).
    if (da->primitive != mdk::Primitive::Triangles ||
        db->primitive != mdk::Primitive::Triangles) return false;
    if (a->texHd != b->texHd || a->texIndex != b->texIndex || a->texUnit != b->texUnit) return false;
    if (a->texPending != b->texPending) return false;
    if (a->stencil != b->stencil) return false;
    if (a->overlay != b->overlay || a->lit != b->lit || a->shadowLit != b->shadowLit ||
        a->worldIdentity != b->worldIdentity ||
        a->haze != b->haze || a->mipped != b->mipped) return false;
    if (a->alphaRef != b->alphaRef || a->stage0Index != b->stage0Index ||
        a->filter != b->filter || a->mipLevels != b->mipLevels) return false;
    if (memcmp(&a->scaleX, &b->scaleX, sizeof(float) * 4) != 0) return false;
    if (memcmp(&a->origScaleX, &b->origScaleX, sizeof(float) * 4) != 0) return false;
    if (da->material.pass != db->material.pass || da->material.blend != db->material.blend ||
        da->material.shading != db->material.shading) return false;
    if (da->material.depth_layer != db->material.depth_layer) return false;   // its own matrix
    if (memcmp(&da->material.colour, &db->material.colour, sizeof(da->material.colour)) != 0) return false;
    if (memcmp(da->world, db->world, sizeof(da->world)) != 0) return false;
    return true;
}

// Append one prepared record, folding it into the previous record when
// coalesciblePair allows: one DrawArrays then submits both ranges, with
// identical state and order, so the picture cannot change. The compared
// fields are every field drawRecorded submits from (validator F5:
// material.texture is covered -- for a non-HD record texIndex IS
// material.texture, and an HD record's texIndex names the one copy its
// resource owns). Every producer routes through here (the glow is an
// index into this array, not a record), so vertCount is always set
// (validator F2). The glow slot is a hard boundary: preparedGlowAt is
// the index the first post-glow record will take, so when the array's
// size equals it, back() is the last pre-glow record and folding the
// newcomer across would move it before the glow (validator F4).
static void pushPrepared(RendererC3D* r, const PreparedDraw* p) {
    if (!r->prepared.empty() && (int32_t)r->prepared.size() != r->preparedGlowAt &&
        coalesciblePair(&r->prepared.back(), p)) {
        r->prepared.back().vertCount += p->vertCount;
        return;
    }
    r->prepared.push_back(*p);
}

// Resolve the whole queue once per frame, in pass order; the glow's slot
// and the insets-over-canvas tail keep their places in the prepared array.
static void prepareQueue(RendererC3D* r) {
    if (!r) {
        return;
    }
    r->prepared.clear();
    r->preparedGlowAt = -1;
    if (r->queue.empty() || !r->inFrame) {
        return;
    }
    // Exact capacity for the frame (the records point into the queue, which
    // nothing mutates between here and the queue reset after the eye loop).
    {
        size_t total = r->insetOverDraws.size();
        for (int32_t pass = 0; pass < mdk::kDrawQueuePassCount; pass++) {
            total += r->queue.bucket(pass).size() + r->queue.backBucket(pass).size();
        }
        if (r->prepared.capacity() < total) {
            r->prepared.reserve(total);
        }
    }
    // THE CAMERA'S ARENA OVER THE OTHER ONE (render/draw_queue.h), with the
    // stencil instead of the depth step: no draw is added. The camera arena's
    // opaque `cover` draws go first and mark where they draw; the other arena's
    // `back` draws follow, only where nothing was marked; then every bucket as
    // before, without the cover draws already drawn. The picture is the depth
    // step's (Vulkan, 12_draw.inc): the camera's arena wherever it is drawn, the
    // depth buffer everywhere else. Without back draws only the last runs.
    const bool layered = r->queue.hasBack();
    if (layered) {
        for (int32_t pass = static_cast<int32_t>(mdk::Pass::Solid);
             pass <= static_cast<int32_t>(mdk::Pass::DoubleSided); pass++) {
            const std::vector<mdk::MeshDraw>& bucket = r->queue.bucket(pass);
            for (size_t i = 0; i < bucket.size(); i++) {
                PreparedDraw p;
                if (bucket[i].cover && prepareDraw(r, &bucket[i], &p)) {
                    p.stencil = kStencilMark;
                    pushPrepared(r, &p);
                }
            }
        }
        for (int32_t pass = 0; pass < mdk::kDrawQueuePassCount; pass++) {
            const std::vector<mdk::MeshDraw>& back = r->queue.backBucket(pass);
            for (size_t i = 0; i < back.size(); i++) {
                PreparedDraw p;
                if (prepareDraw(r, &back[i], &p)) {
                    p.stencil = kStencilTest;
                    pushPrepared(r, &p);
                }
            }
        }
    }
    for (int32_t pass = 0; pass < mdk::kDrawQueuePassCount; pass++) {
        // The glow lands between the scene and the canvas, where the PC's post
        // pass sits (SPEC.md 6: post -> frame -> canvas).
        if (pass == static_cast<int32_t>(mdk::Pass::Overlay)) {
            r->preparedGlowAt = (int32_t)r->prepared.size();
        }
        const std::vector<mdk::MeshDraw>& bucket = r->queue.bucket(pass);
        const bool opaque = pass == static_cast<int32_t>(mdk::Pass::Solid) ||
                            pass == static_cast<int32_t>(mdk::Pass::DoubleSided);
        for (size_t i = 0; i < bucket.size(); i++) {
            if (layered && opaque && bucket[i].cover) {
                continue;   // drawn first, above
            }
            PreparedDraw p;
            if (prepareDraw(r, &bucket[i], &p)) {
                pushPrepared(r, &p);
            }
        }
    }
    // The insets over the canvas, after it (render.h; the C#'s RenderInsets
    // OverCanvas): the sniper clip's rounds.
    for (size_t i = 0; i < r->insetOverDraws.size(); i++) {
        PreparedDraw p;
        if (prepareDraw(r, &r->insetOverDraws[i], &p)) {
            pushPrepared(r, &p);
        }
    }
}

// A rectangle of the canvas (x, y, width, height) through the canvas's own
// matrix (pica::canvasMatrix, the panel's quarter turn included) into the eye
// target's pixels: its low and high corners.
static void canvasRectOnTarget(RendererC3D* r, const float rect[4], float lo[2], float hi[2]) {
    float m[16];
    mdk::pica::canvasMatrix(m, mdk_render_canvas_width((MdkRenderer)r));
    const float xs[2] = {rect[0], rect[0] + rect[2]};
    const float ys[2] = {rect[1], rect[1] + rect[3]};
    lo[0] = 1e9f;
    lo[1] = 1e9f;
    hi[0] = -1e9f;
    hi[1] = -1e9f;
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            const float clipX = xs[i] * m[0] + ys[j] * m[4] + m[12];
            const float clipY = xs[i] * m[1] + ys[j] * m[5] + m[13];
            const float px = (clipX + 1.0f) * 0.5f * (float)r->targetWidth;
            const float py = (clipY + 1.0f) * 0.5f * (float)r->targetHeight;
            lo[0] = px < lo[0] ? px : lo[0];
            hi[0] = px > hi[0] ? px : hi[0];
            lo[1] = py < lo[1] ? py : lo[1];
            hi[1] = py > hi[1] ? py : hi[1];
        }
    }
}

// The per-eye replay of the prepared array: thin submit only (see
// drawRecorded), with the glow still drawn per eye at its slot.
// THE SCENE WINDOW's scissor (mdk_render_set_scene_window): the canvas
// rectangle on the eye target, grown by two pixels and rounded outward, so
// every pixel the window shows is inside it.
static void sceneWindowScissor(RendererC3D* r, u32* left, u32* top, u32* right, u32* bottom) {
    float lo[2];
    float hi[2];
    canvasRectOnTarget(r, r->sceneWindow, lo, hi);
    const int32_t x0 = (int32_t)floorf(lo[0]) - 2;
    const int32_t y0 = (int32_t)floorf(lo[1]) - 2;
    const int32_t x1 = (int32_t)ceilf(hi[0]) + 2;
    const int32_t y1 = (int32_t)ceilf(hi[1]) + 2;
    *left = (u32)(x0 < 0 ? 0 : x0);
    *top = (u32)(y0 < 0 ? 0 : y0);
    *right = (u32)(x1 > (int32_t)r->targetWidth ? (int32_t)r->targetWidth : x1);
    *bottom = (u32)(y1 > (int32_t)r->targetHeight ? (int32_t)r->targetHeight : y1);
}

// THE SCENE INSETS (the sniper's round cameras; C# Renderer.Insets.cs
// RenderInset with InsetContent.Scene): each one draws the frame's scene records
// again -- every pass but the canvas, as the C#'s does -- through its own
// camera, into its own viewport, after the scene and the glow and before the
// canvas (the C#'s UnderCanvas). The colour under it is kept and its depth
// cleared (the C#'s BeginOver: LOAD the colour, CLEAR the depth). This target
// has no rectangle depth clear (a target clear is the whole buffer), so the
// depth is reset by a quad over the viewport at the far plane, depth test
// ALWAYS, depth written and colour not (vault: forsaken
// citro3d-missile-chase-cam-per-eye-scissored-clear). It leaves the scissor off.
// ONE EYE (owner, 2026-11-26: "we dont need stereoscopy there"): the insets are
// drawn in the first eye only, at zero parallax (no eye offset), and the second
// eye gets the same pixels by a copy after its replay (copyInsetsToSecondEye).
// The lit programs turn each face to the camera (mdk_c3d.g.pica): the inset's
// camera while its records draw, the eye's again after. Their uniforms are
// written at every bind from these rows, and now if one is bound.
static void setLitCamera(RendererC3D* r, const float position[3]) {
    for (int i = 0; i < 3; i++) {
        r->litUniforms[4][i] = position[i];
        r->shadowUniforms[4][i] = position[i];
    }
    if (r->boundProgram == &r->programLit) {
        C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->cameraLocation, position[0], position[1],
                      position[2], 1.0f);
    } else if (r->boundProgram == &r->programShadow) {
        C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowCameraLocation, position[0], position[1],
                      position[2], 1.0f);
    }
}

static void drawSceneInsets(RendererC3D* r) {
    if (r->sceneInsetCount <= 0 || r->preparedGlowAt <= 0 || r->eyeTarget != r->targetLeft) {
        return;
    }
    float frameVp[16];
    float frameFrustum[6][4];
    memcpy(frameVp, r->eyeVp, sizeof(frameVp));
    memcpy(frameFrustum, r->frustum, sizeof(frameFrustum));
    for (int32_t n = 0; n < r->sceneInsetCount; n++) {
        const RendererC3D::SceneInset& inset = r->sceneInsets[n];
        float lo[2];
        float hi[2];
        canvasRectOnTarget(r, inset.rect, lo, hi);
        int32_t x0 = (int32_t)(lo[0] + 0.5f);
        int32_t y0 = (int32_t)(lo[1] + 0.5f);
        int32_t x1 = (int32_t)(hi[0] + 0.5f);
        int32_t y1 = (int32_t)(hi[1] + 0.5f);
        x0 = x0 < 0 ? 0 : x0;
        y0 = y0 < 0 ? 0 : y0;
        x1 = x1 > (int32_t)r->targetWidth ? (int32_t)r->targetWidth : x1;
        y1 = y1 > (int32_t)r->targetHeight ? (int32_t)r->targetHeight : y1;
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        C3D_SetViewport((u32)x0, (u32)y0, (u32)(x1 - x0), (u32)(y1 - y0));
        C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)x0, (u32)y0, (u32)x1, (u32)y1);

        // The depth reset: the quad's corners are already clip coordinates.
        {
            static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            C3D_Mtx gpuMtx;
            mdk::pica::registers(kIdentity, gpuMtx.m);
            setVsMatrix(r, kDedupeMvp, r->mvpLocation, &gpuMtx);
            r->matrixIsFrame = false;
            leaveShadowLitStages(r);
            // The geometry-stage rule (bindProgram): in geometry mode the
            // pass-through program, as drawRecorded's unlit draws.
            bindProgram(r, r->gsInList && r->passReady ? &r->programPass : &r->program);
            r->litBound = false;
            bindVertexBuffer(r, static_cast<const char*>(r->scratch) + kInsetQuadOffset);
            C3D_CullFace(GPU_CULL_NONE);
            r->cullMode = GPU_CULL_NONE;
            const int alphaReference = r->alphaReference;
            installAlphaTest(r, -1, false);   // off: no fragment of the quad is discarded
            mdk::pica::DepthState reset;
            reset.reset = true;
            installDepthState(r, reset);
            // And the stencil to 0 there: the inset's records mark and test it
            // afresh (prepareQueue; the eye's marks are not the inset camera's).
            C3D_StencilTest(true, GPU_ALWAYS, 0, 0xFF, 0xFF);
            C3D_StencilOp(GPU_STENCIL_REPLACE, GPU_STENCIL_REPLACE, GPU_STENCIL_REPLACE);
            r->stencilMode = -1;
            C3D_DrawArrays(r->boundProgram->geometryShader ? GPU_GEOMETRY_PRIM : GPU_TRIANGLES,
                           0, 6);
            installStencil(r, kStencilOff);
            // Back to states the caches describe.
            installAlphaTest(r, alphaReference, false);
            installDepthState(r, mdk::pica::depthStateForPass(mdk::Pass::Solid));
            C3D_CullFace(kSolidCull);
            r->cullMode = kSolidCull;
        }

        // The inset's camera in place of the eye's, for the matrix and the cull.
        mdk::pica::eyeViewProjection(inset.view, inset.projection, 0.0f, r->eyeVp);
        {
            float vpEngine[16];
            mdk::pica::eyeViewProjectionEngine(inset.view, inset.projection, 0.0f, vpEngine);
            mdk::pica::frustumPlanes(vpEngine, r->frustum);
        }
        C3D_Mtx gpuMtx;
        mdk::pica::registers(r->eyeVp, gpuMtx.m);
        setVsMatrix(r, kDedupeMvp, r->mvpLocation, &gpuMtx);
        r->matrixIsFrame = true;
        setLitCamera(r, inset.position);
        memcpy(r->pullEye, inset.position, sizeof(r->pullEye));   // the depth pull's eye
        for (int32_t i = 0; i < r->preparedGlowAt; i++) {
            if (!r->prepared[(size_t)i].overlay) {
                drawRecorded(r, &r->prepared[(size_t)i]);
            }
        }
    }
    // The eye's own matrix, cull, viewport and scissor again.
    memcpy(r->eyeVp, frameVp, sizeof(frameVp));
    memcpy(r->frustum, frameFrustum, sizeof(frameFrustum));
    r->matrixIsFrame = false;
    setLitCamera(r, r->cameraPos);
    memcpy(r->pullEye, r->cameraPos, sizeof(r->pullEye));
    C3D_SetViewport(0, 0, r->targetWidth, r->targetHeight);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, r->targetWidth, r->targetHeight);
}

// THE SECOND EYE'S INSETS: the first eye's pixels of each inset's rectangle,
// copied into the second eye's target after its replay (the canvas is flat, so
// both eyes drew the same canvas there). The rectangle on the panel, through the
// rotation writeScreenshotBmp measured (memory row = panel x, memory column =
// 239 - panel y, both times the target's scale), grown out to whole 8x8 tiles:
// within one row of tiles the tiles are consecutive, 256 bytes each, so each
// rectangle is one GX_TextureCopy of tile-row lines with a gap. The few pixels
// the growth adds lie under the opaque frame around the windows. One split (the
// eye's commands run before the copy) and one copy per inset in the GX queue.
static void copyInsetsToSecondEye(RendererC3D* r) {
    if (r->sceneInsetCount <= 0 || r->eyes.passes < 2 || r->preparedGlowAt <= 0) {
        return;
    }
    frameSplit(r);
    const float canvasWidth = mdk_render_canvas_width((MdkRenderer)r);
    const int32_t width = (int32_t)r->targetWidth;     // kScreenWidth * scale x
    const int32_t height = (int32_t)r->targetHeight;   // kScreenHeight * scale y
    const int32_t tilesPerRow = width / 8;
    for (int32_t n = 0; n < r->sceneInsetCount; n++) {
        const float* rect = r->sceneInsets[n].rect;
        const float panelX0 = rect[0] * (float)kScreenHeight / canvasWidth;
        const float panelX1 = (rect[0] + rect[2]) * (float)kScreenHeight / canvasWidth;
        const float panelY0 = rect[1] * (float)kScreenWidth / (float)mdk::kCanvasHeight;
        const float panelY1 = (rect[1] + rect[3]) * (float)kScreenWidth / (float)mdk::kCanvasHeight;
        int32_t row0 = (int32_t)floorf(panelX0 * (float)r->targetScaleY);
        int32_t row1 = (int32_t)ceilf(panelX1 * (float)r->targetScaleY);
        int32_t col0 = width - (int32_t)ceilf(panelY1 * (float)r->targetScaleX);
        int32_t col1 = width - (int32_t)floorf(panelY0 * (float)r->targetScaleX);
        row0 = (row0 < 0 ? 0 : row0) / 8;
        row1 = ((row1 > height ? height : row1) + 7) / 8;
        col0 = (col0 < 0 ? 0 : col0) / 8;
        col1 = ((col1 > width ? width : col1) + 7) / 8;
        if (row1 <= row0 || col1 <= col0) {
            continue;
        }
        const u32 lineBytes = (u32)(col1 - col0) * 256u;
        const u32 gapBytes = (u32)(tilesPerRow - (col1 - col0)) * 256u;
        const size_t start = ((size_t)row0 * (size_t)tilesPerRow + (size_t)col0) * 256u;
        const u8* from = static_cast<const u8*>(r->targetLeft->frameBuf.colorBuf) + start;
        u8* to = static_cast<u8*>(r->targetRight->frameBuf.colorBuf) + start;
        GX_TextureCopy((u32*)from, GX_BUFFER_DIM(lineBytes >> 4, gapBytes >> 4), (u32*)to,
                       GX_BUFFER_DIM(lineBytes >> 4, gapBytes >> 4),
                       lineBytes * (u32)(row1 - row0), GX_TRANSFER_RAW_COPY(1));
    }
}

static void replayQueue(RendererC3D* r) {
    if (!r || (r->prepared.empty() && r->preparedGlowAt < 0) || !r->inFrame) {
        return;
    }
    // The scene's draws (and the halos) inside the window; the canvas's never
    // (installEye left the scissor off, and it is off again at the end).
    u32 left = 0, top = 0, right = 0, bottom = 0;
    if (r->sceneWindowOn) {
        sceneWindowScissor(r, &left, &top, &right, &bottom);
    }
    bool clipped = false;
    for (size_t i = 0; i < r->prepared.size(); i++) {
        const bool clip = r->sceneWindowOn && !r->prepared[i].overlay;
        if (clip != clipped) {
            C3D_SetScissor(clip ? GPU_SCISSOR_NORMAL : GPU_SCISSOR_DISABLE, left, top, right,
                           bottom);
            clipped = clip;
        }
        if ((int32_t)i == r->preparedGlowAt) {
            drawGlow(r);
            // The record here is the canvas's first (every record from the glow's
            // slot on is an Overlay one), so the scissor is already off, as the
            // insets leave it.
            drawSceneInsets(r);
        }
        drawRecorded(r, &r->prepared[i]);
    }
    if (r->preparedGlowAt == (int32_t)r->prepared.size()) {
        drawGlow(r);
        if (clipped) {
            C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, r->targetWidth, r->targetHeight);
            clipped = false;
        }
        drawSceneInsets(r);
    }
    if (clipped) {
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, r->targetWidth, r->targetHeight);
    }
}

 void mdk_render_draw_mesh(MdkRenderer rr, const mdk::MeshDraw* draw) {
    RendererC3D* r = asC3d(rr);
    if (!r || !draw || !r->inFrame) {
        return;
    }
    // A STRIP OVER 1024 (createStrip) is drawn from its frame's page: the record
    // names the page, its frame in the page and the page's frame count, so the
    // prepare, both eyes' submits and every cache after this see an ordinary
    // packed strip (pica::stripPage, compressedFrameUv).
    mdk::MeshDraw paged;
    const int32_t t = draw->material.texture;
    if (t >= 0 && (size_t)t < r->textures.size() && !r->textures[(size_t)t].pages.empty()) {
        const C3dTexture& strip = r->textures[(size_t)t];
        const mdk::pica::StripPage at =
            mdk::pica::stripPage(draw->frame, draw->material.frame_count, strip.framesPerPage);
        if (at.page >= (int32_t)strip.pages.size()) {
            return;
        }
        paged = *draw;
        paged.material.texture = strip.pages[(size_t)at.page];
        paged.frame = at.frame;
        paged.material.frame_count = at.frameCount;
        draw = &paged;
    }
    // Queued, not recorded: the oracle renders the frame's commands one pass at
    // a time, and render.h gives that ordering job to the backend. With the
    // depth test off on this target, the order IS the occlusion, so this is not
    // a tidiness change.
    //
    // An open Own inset takes the draw (07_canvas.inc): its world becomes the
    // whole transform onto the inset's rectangle, drawn the canvas's way.
    if (r->insetOpen) {
        mdk::MeshDraw inset = *draw;
        mdk::Mat4 world;
        mdk::Mat4 transform;
        memcpy(world.m, draw->world, sizeof(world.m));
        memcpy(transform.m, r->insetMatrix, sizeof(transform.m));
        const mdk::Mat4 full = mdk::multiply(world, transform);
        memcpy(inset.world, full.m, sizeof(inset.world));
        // The bounds the copy carried were the batch's WORLD box, valid only
        // under the identity world (render_types.h MeshDraw). This draw now
        // carries the inset's own transform, so the box no longer describes it:
        // opt the copy back out of the frustum cull.
        inset.cullable = false;
        inset.back = false;
        inset.cover = false;
        inset.material.pass = mdk::Pass::Overlay;
        if (r->insetOver) {
            r->insetOverDraws.push_back(inset);
        } else {
            r->queue.add(inset);
        }
        return;
    }
    if (r->queueBack) {
        mdk::MeshDraw back = *draw;
        back.back = true;
        r->queue.add(back);
        return;
    }
    r->queue.add(*draw);
}

void mdk_render_set_back(MdkRenderer rr, bool back) {
    RendererC3D* r = asC3d(rr);
    if (r) {
        r->queueBack = back;
    }
}

