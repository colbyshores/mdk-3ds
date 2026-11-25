// 07_canvas.inc -- the 2D canvas: quads, its mesh, insets, diagnostics.
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

// Two consecutive canvas quads share a draw when their material is the same.
//
// This is the oracle's batching, not an optimisation invented here: the C#
// merges a quad into the previous command when the materials compare equal
// (Renderer.cs:352-357, "Neighbouring quads of one material draw together"), so
// the HUD's four frame rectangles are one draw there and would be four here
// without this. The comparison is exact float equality on the colour because the
// C#'s is too -- a record's `==` on a Vector4 compares the four floats -- and
// being wrong in either direction is harmless: not merging draws the same
// picture in more calls.
static bool sameCanvasMaterial(const mdk::CanvasQuad& a, const mdk::CanvasQuad& b) {
    return a.texture == b.texture && a.palette == b.palette &&
           a.frame_count == b.frame_count &&
           a.additive == b.additive && a.colour.r == b.colour.r &&
           a.colour.g == b.colour.g && a.colour.b == b.colour.b &&
           a.colour.a == b.colour.a;
}

// The quad's six corners, in the oracle's order: two triangles, LT RT RB /
// LT RB LB (Renderer.cs:345-350). The winding is the scene's, and the Overlay
// pass is unculled anyway (`drawRecorded` culls only Solid).
static void appendCanvasCorners(std::vector<mdk::Vertex>& out, const mdk::CanvasQuad& q) {
    const float x[4] = {q.x0, q.x1, q.x2, q.x3};
    const float y[4] = {q.y0, q.y1, q.y2, q.y3};
    const float u[4] = {q.u0, q.u1, q.u2, q.u3};
    const float v[4] = {q.v0, q.v1, q.v2, q.v3};
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) {
        const int c = order[i];
        mdk::Vertex vert;
        vert.pos = mdk::Vec3{x[c], y[c], 0.0f};
        vert.uv = mdk::Vec2{u[c], v[c]};
        // The vertex colour is not uploaded on this target (five floats a corner,
        // and the tint is a uniform), but leaving it uninitialised would make the
        // mesh's bytes depend on what was in the vector before.
        vert.colour = 0u;
        out.push_back(vert);
    }
}

// The canvas mesh, made on the first frame that needs one and grown when a frame
// needs more corners than it holds.
//
// Growing means releasing the old one and making a bigger one, which is the
// deferred path every other resource here uses: the old buffer is freed at the
// next frame boundary, after C3D_FrameBegin has waited for the queue that may
// still be reading it (AGENTS.md P3). The cost is one allocation per size change,
// and the HUD's size settles after the first frames of a level.
static bool ensureCanvasMesh(MdkRenderer rr, RendererC3D* r, int32_t corners) {
    if (r->canvasMesh >= 0 && r->canvasCapacity >= corners) {
        return true;
    }
    const int32_t maxCorners = mdk_render_canvas_capacity(rr) * 6;
    int32_t cap = corners < 64 * 6 ? 64 * 6 : corners * 2;
    if (cap > maxCorners) {
        cap = maxCorners;
    }
    if (corners > cap) {
        return false;
    }
    if (r->canvasMesh >= 0) {
        mdk_render_release_mesh(rr, r->canvasMesh);
        r->canvasMesh = -1;
        r->canvasCapacity = 0;
    }
    r->canvasMesh = mdk_render_create_dynamic_mesh(rr, cap);
    if (r->canvasMesh < 0) {
        // The factory prints its own refusal line, edge-triggered (review-10 F3).
        return false;
    }
    r->canvasCapacity = cap;
    // `canvas mesh:` and not `canvas:` -- the frame line below is the harness's
    // evidence and an allocation note that shares its prefix shadows it. The
    // object stream's mesh line uses the same `things: stream mesh N corners`
    // shape for the same reason.
    mdk::log3dsPrintf("canvas mesh: %d corners = %d KB linear", (int)cap,
                      (int)((int64_t)cap * 5 * sizeof(float) / 1024));
    return true;
}

 void mdk_render_canvas_begin(MdkRenderer rr) {
    RendererC3D* r = asC3d(rr);
    if (!r) {
        return;
    }
    // NO FLUSH HERE, and that is a contract rather than an omission (review-1
    // F4). With the per-eye replay at the frame end, a mid-frame flush emits
    // THIS frame's queued records into whatever target installEye last
    // selected -- the previous frame's last eye, or no target at all on frame 1
    // -- and a flushDraws-shaped one empties the queue too, so the eye loop
    // replays nothing: a scene that vanished behind a correct clear colour.
    //
    // What the canvas does instead is what this seam was always meant to do: it
    // accumulates, and `canvas_end` puts the batch IN THE QUEUE as Overlay
    // records, which the per-eye replay draws once per eye, after every other
    // pass -- the oracle's order (draw_queue.h, Renderer.cs:1000-1004). The
    // gate that refuses a flush or an immediate draw here covers canvas_quad too,
    // the seam hud_view.cpp actually calls (review-3 F1).
    r->canvasQuads.clear();
}

 void mdk_render_canvas_quad(MdkRenderer rr, const mdk::CanvasQuad* q) {
    RendererC3D* r = asC3d(rr);
    if (!r || !q || !r->ready || !r->inFrame) {
        return;
    }
    // The batch's bound, and it is the oracle's: past its own canvas cap the C#
    // drops the quad (Renderer.cs:341-344) rather than growing without limit, and
    // `mdk_render_canvas_capacity` is that cap for this backend. Dropping is the
    // right failure because the alternative is a per-frame allocation in the HUD's
    // draw path, which is what AGENTS.md 10 forbids.
    const int32_t cap = mdk_render_canvas_capacity(rr);
    if (cap > 0 && (int32_t)r->canvasQuads.size() >= cap) {
        return;
    }
    r->canvasQuads.push_back(*q);
}

 void mdk_render_canvas_end(MdkRenderer rr) {
    RendererC3D* r = asC3d(rr);
    if (!r || !r->inFrame) {
        return;
    }
    const int32_t quads = (int32_t)r->canvasQuads.size();
    if (quads == 0) {
        return;
    }
    // RECORDED BEFORE THE WAYS THIS CAN STILL FAIL. `canvasQuadsThisFrame` is the
    // `quads=` half of the line `emurun.sh --expect hud` reads, and it means
    // "what the frontend asked for" — not "what was drawn". Setting it only on the
    // success path made an allocation failure read as `quads=0`, i.e. as "the
    // frontend asked for nothing", which is the one thing the line must not
    // confuse with a dropped batch (review-1 F5). `draws` stays the honest half.
    r->canvasQuadsThisFrame = quads;
    const int32_t corners = quads * 6;
    if (!ensureCanvasMesh(rr, r, corners)) {
        r->canvasQuads.clear();
        return;
    }

    r->canvasVerts.clear();
    for (size_t i = 0; i < r->canvasQuads.size(); i++) {
        appendCanvasCorners(r->canvasVerts, r->canvasQuads[i]);
    }
    mdk_render_update_mesh(rr, r->canvasMesh, r->canvasVerts.data(), corners);

    // THE TRANSFORM, once for the batch: the canvas's pixel space onto the whole
    // panel, with the panel's quarter turn baked in (pica::canvasMatrix). The C#
    // builds it once in QueueCanvas too (Renderer.cs:736) and hands the same
    // matrix to every canvas command.
    float screen[16];
    mdk::pica::canvasMatrix(screen, mdk_render_canvas_width(rr));

    int32_t i = 0;
    while (i < quads) {
        int32_t run = 1;
        while (i + run < quads && sameCanvasMaterial(r->canvasQuads[(size_t)i],
                                                     r->canvasQuads[(size_t)(i + run)])) {
            run++;
        }
        const mdk::CanvasQuad& q = r->canvasQuads[(size_t)i];
        mdk::MeshDraw d;
        d.mesh = r->canvasMesh;
        d.first_vertex = i * 6;
        d.vertex_count = run * 6;
        // A flat quad has no texture of its own; white is the identity of the
        // combiner's multiply, so the tint is the whole colour. If the white
        // texture could not be allocated the id is -1 and drawRecorded drops the
        // draw, which is the refusal the create path logged.
        d.material.texture = q.texture >= 0 ? q.texture : r->canvasWhite;
        d.material.palette = q.palette;
        d.material.colour = q.colour;
        d.material.frame_count = q.frame_count > 0 ? q.frame_count : 1;
        d.material.pass = mdk::Pass::Overlay;
        // The oracle's AddRect is the same quad with `Blend = Blend.Add`
        // (Renderer.cs:329); `blendStateForPass` already knows what that means.
        d.material.blend = q.additive ? mdk::Blend::Add : mdk::Blend::Alpha;
        d.frame = 0;   // the oracle hardcodes it: `new DrawCommand(..., material, 0, screen)`
        memcpy(d.world, screen, sizeof(screen));
        r->queue.add(d);
        i += run;
    }

    r->canvasQuads.clear();
}

 float mdk_render_canvas_width(MdkRenderer r) {
    (void)r;
    // The canvas is 360 units high and its width follows the view's aspect,
    // like C# Renderer.CanvasWidth. The ratio looks INVERTED and is not:
    // kScreenWidth/kScreenHeight name the ROTATED FRAMEBUFFER axes (240x400
    // portrait, renderer_c3d.cpp:62-63), not the landscape 400x240 the game
    // draws into, so the landscape aspect is height/width = 400/240 and the
    // canvas is 600 units wide. Renaming those constants to the landscape
    // orientation would silently turn this into 216.
    return mdk::kCanvasHeight * (float)kScreenHeight / (float)kScreenWidth;
}
 int32_t mdk_render_canvas_capacity(MdkRenderer r) {
    (void)r;
    // The oracle's own cap, in the oracle's own unit: `CanvasQuads = 4096`
    // (Renderer.cs:198) compared against the VERTEX count as
    // `CanvasQuads * QuadVertices` (Renderer.cs:342), i.e. 4096 quads. It is a
    // ceiling, not an allocation -- `ensureCanvasMesh` sizes the buffer from the
    // quads actually queued and only clamps at `capacity * 6` corners -- so
    // matching the oracle costs nothing today and stops the menus and the message
    // queue, which want hundreds of quads, from silently dropping everything past
    // the 512th (review-9 F6).
    //
    // `caps.max_canvas_quads` reports this same number, from the same constant,
    // so the advertised capability and the enforced one cannot drift (review-1 F4).
    return kCanvasQuadCapacity;
}

// ---------------------------------------------------------------------------
// Insets
// ---------------------------------------------------------------------------

 void mdk_render_begin_inset(MdkRenderer r, const mdk::Inset* i) {
    // The same contract as mdk_render_canvas_begin: nothing is emitted here. An
    // Own inset's transform is made once and its draws are RECORDED with it
    // (mdk_render_draw_mesh), to be drawn inside each eye's replay.
    RendererC3D* c = asC3d(r);
    if (!c || !i || !c->inFrame) {
        return;
    }
    c->insetOpen = false;
    if (i->scene) {
        if (!c->insetSceneRefused) {
            c->insetSceneRefused = true;
            mdk::log3dsPrintf("inset: a scene inset (a sniper round camera) is not drawn on "
                              "this target");
        }
        return;
    }
    // A ZERO-AREA inset draws nothing (render.h): its rectangle has no pixels.
    if (!(i->width > 0.0f) || !(i->height > 0.0f)) {
        return;
    }
    // The inset camera's clip -> its rectangle of the canvas, in homogeneous
    // canvas units: x' = x * w/2 + w * (rx + w/2), y' = -y * h/2 + w * (ry + h/2)
    // (the clip's y runs up, the canvas's down), z and w kept. Then the canvas's
    // own matrix, which takes canvas units to the panel (its quarter turn and its
    // mid-range depth included).
    mdk::Mat4 rect = mdk::Mat4::identity();
    rect.m[0] = i->width * 0.5f;
    rect.m[5] = -i->height * 0.5f;
    rect.m[12] = i->x + i->width * 0.5f;
    rect.m[13] = i->y + i->height * 0.5f;
    rect.m[15] = 1.0f;
    mdk::Mat4 view;
    mdk::Mat4 projection;
    memcpy(view.m, i->camera.view, sizeof(view.m));
    memcpy(projection.m, i->camera.projection, sizeof(projection.m));
    mdk::Mat4 canvas;
    mdk::pica::canvasMatrix(canvas.m, mdk_render_canvas_width(r));
    const mdk::Mat4 full =
        mdk::multiply(mdk::multiply(mdk::multiply(view, projection), rect), canvas);
    memcpy(c->insetMatrix, full.m, sizeof(c->insetMatrix));
    c->insetOver = !i->under_canvas;
    c->insetOpen = true;
}
 void mdk_render_end_inset(MdkRenderer r) {
    RendererC3D* c = asC3d(r);
    if (c) {
        c->insetOpen = false;
    }
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

 void mdk_render_stats(MdkRenderer r, int32_t* draw_calls, int32_t* triangles) {
    (void)r;
    if (draw_calls) *draw_calls = 0;
    if (triangles) *triangles = 0;
}

 float mdk_render_gpu_wait_ms(MdkRenderer r) { (void)r; return 0.0f; }

// The capture accounting. This backend never writes a picture, so `written` is a
// constant zero and that IS the honest answer rather than a stub: the PICA path
// has no readback of a rendered target, so a frontend that asked for a frame here
// did not get one and must be able to fail its run. The request half is counted
// in `mdk_render_begin_frame`.
 void mdk_render_capture_stats(MdkRenderer r, int32_t* requested, int32_t* written) {
    RendererC3D* cr = asC3d(r);
    if (requested) *requested = cr ? cr->capturesRequested : 0;
    if (written) *written = cr ? cr->capturesWritten : 0;
 }

// The first-use counters (render_c3d_stats.h): what this renderer has made since
// it was created, for the harness's arena-entry table. Copies, nothing else.
 void mdk_render_c3d_first_use(MdkRenderer r, MdkC3dFirstUse* out) {
    if (!out) return;
    *out = MdkC3dFirstUse();
    RendererC3D* cr = asC3d(r);
    if (!cr) return;
    out->texturesMade = cr->texturesMade[0] + cr->texturesMade[1] + cr->texturesMade[2] +
                        cr->texturesMade[3] + cr->compressedMade;
    out->texturesFirstMade = cr->texturesFirstMade;
    out->texturesRemade = cr->texturesRemade;
    out->hdReads = cr->hdReads;
    out->hdReadBytes = cr->hdReadBytes;
    out->hdImported = cr->hdImported;
    out->hdEvicted = cr->hdEvicted;
    out->texturesEvicted = cr->texturesEvicted;
    out->textureMakeTicks = cr->textureMakeTicks;
    out->hdImportTicks = cr->hdImportTicks;
    out->packImported = cr->packImported;
    out->packSync = cr->packSync;
    out->packSyncTicks = cr->packSyncTicks;
    out->packLanded = cr->packLanded;
    out->packFailed = cr->packFailed;
    out->hdCoveredSdMade = cr->hdCoveredSdMade;
    out->hdLateTextures = cr->hdLateTextures;
    out->hdLateDraws = cr->hdLateDraws;
    out->hdBlankDraws = cr->hdBlankDraws;
    out->hdResidentPeak = (uint32_t)cr->hdResidentPeak;
    out->hdLateLanded = cr->hdLateLanded;
    out->hdLateFramesSum = cr->hdLateFramesSum;
    out->hdLateFramesMax = cr->hdLateFramesMax;
    out->hdLateWorstFrame = cr->hdLateWorstFrame;
    memcpy(out->hdLateWorst, cr->hdLateWorst, sizeof(out->hdLateWorst));
    memcpy(out->hdLateRows, cr->hdLateRows, sizeof(out->hdLateRows));
    out->hdSlackMin = cr->hdSlackMin;
    out->hdSlackMinFrame = cr->hdSlackMinFrame;
    out->hdSlackUnder30 = cr->hdSlackUnder30;
    out->hdSlackFirsts = cr->hdSlackFirsts;
    memcpy(out->hdSlackWorst, cr->hdSlackWorst, sizeof(out->hdSlackWorst));
    for (size_t i = 0; i < cr->hdCopies.size(); i++) {
        const HdCopy& copy = cr->hdCopies[i];
        if (copy.pinned) {
            out->hdPinned++;
            if (copy.state == HdCopy::Resident) {
                out->hdPinnedResident++;
                out->hdPinnedBytes += copy.bytes;
            }
        }
    }
    out->hdPinFailed = cr->hdPinFailed;
    out->hdTopCap = cr->hdTopCap;
 }
