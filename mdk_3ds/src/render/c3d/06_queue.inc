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
            total += r->queue.bucket(pass).size();
        }
        if (r->prepared.capacity() < total) {
            r->prepared.reserve(total);
        }
    }
    for (int32_t pass = 0; pass < mdk::kDrawQueuePassCount; pass++) {
        // The glow lands between the scene and the canvas, where the PC's post
        // pass sits (SPEC.md 6: post -> frame -> canvas).
        if (pass == static_cast<int32_t>(mdk::Pass::Overlay)) {
            r->preparedGlowAt = (int32_t)r->prepared.size();
        }
        const std::vector<mdk::MeshDraw>& bucket = r->queue.bucket(pass);
        for (size_t i = 0; i < bucket.size(); i++) {
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

// The per-eye replay of the prepared array: thin submit only (see
// drawRecorded), with the glow still drawn per eye at its slot.
// THE SCENE WINDOW's scissor (mdk_render_set_scene_window): the canvas
// rectangle through the canvas's own matrix (pica::canvasMatrix, the panel's
// quarter turn included) into the eye target's pixels, grown by two pixels
// and rounded outward, so every pixel the window shows is inside it.
static void sceneWindowScissor(RendererC3D* r, u32* left, u32* top, u32* right, u32* bottom) {
    float m[16];
    mdk::pica::canvasMatrix(m, mdk_render_canvas_width((MdkRenderer)r));
    const float xs[2] = {r->sceneWindow[0], r->sceneWindow[0] + r->sceneWindow[2]};
    const float ys[2] = {r->sceneWindow[1], r->sceneWindow[1] + r->sceneWindow[3]};
    float lo[2] = {1e9f, 1e9f};
    float hi[2] = {-1e9f, -1e9f};
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
    const int32_t x0 = (int32_t)floorf(lo[0]) - 2;
    const int32_t y0 = (int32_t)floorf(lo[1]) - 2;
    const int32_t x1 = (int32_t)ceilf(hi[0]) + 2;
    const int32_t y1 = (int32_t)ceilf(hi[1]) + 2;
    *left = (u32)(x0 < 0 ? 0 : x0);
    *top = (u32)(y0 < 0 ? 0 : y0);
    *right = (u32)(x1 > (int32_t)r->targetWidth ? (int32_t)r->targetWidth : x1);
    *bottom = (u32)(y1 > (int32_t)r->targetHeight ? (int32_t)r->targetHeight : y1);
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
        }
        drawRecorded(r, &r->prepared[i]);
    }
    if (r->preparedGlowAt == (int32_t)r->prepared.size()) {
        drawGlow(r);
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
        inset.material.pass = mdk::Pass::Overlay;
        if (r->insetOver) {
            r->insetOverDraws.push_back(inset);
        } else {
            r->queue.add(inset);
        }
        return;
    }
    r->queue.add(*draw);
}

