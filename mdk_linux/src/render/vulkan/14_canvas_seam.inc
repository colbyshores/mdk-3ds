// 14_canvas_seam.inc -- the canvas seam, insets, diagnostics.
// Part of renderer_vk.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

void mdk_render_canvas_begin(MdkRenderer r) {
    RendererVk* vk = asVk(r);
    if (vk) {
        // The scene ends here: the queued draws go in BEFORE any canvas quad,
        // because the canvas is the Overlay pass drawn over the scene and the
        // oracle draws its Overlay bucket last (draw_queue.h).
        flushDraws(vk);
        vk->canvasOffset = 0;
        vk->canvasRecords.clear();
    }
}

// The deferred canvas replay: the quads a deferred (enhanced) frame
// recorded, drawn into the replayed scene pass in order.
void replayCanvas(RendererVk* r) {
    const uint32_t slot = r->ctx.currentFrame;
    for (size_t i = 0; i < r->canvasRecords.size(); i++) {
        const RendererVk::CanvasRecord& rec = r->canvasRecords[i];
        // The enhanced look filters the canvas (its ModeOf routes the
        // Overlay pass to the enhanced shader); the original look replays
        // through the palette pipeline below.
        if (r->enhancedLook && r->enhancedReady) {
            drawEnhancedCanvas(r, rec.texture, rec.palette, rec.frameCount, rec.colour,
                               rec.offset);
            continue;
        }
        PushConstants pc = {};
        canvasOrtho(pc.mvp, canvasWidth(r));
        pc.colour[0] = rec.colour[0];
        pc.colour[1] = rec.colour[1];
        pc.colour[2] = rec.colour[2];
        pc.colour[3] = rec.colour[3];
        pc.textured = (rec.texture >= 0) ? ((rec.palette >= 0) ? 1 : 2) : 0;
        pc.frame_count = rec.frameCount;
        pc.frame = 0;
        pc.unused_ = 0;

        VkPipeline pipeline = getPipeline(r, (int)mdk::Pass::Overlay, (int)mdk::Blend::Alpha, 0);
        if (!pipeline) {
            continue;
        }
        VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdPushConstants(cb, r->pipeLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(PushConstants), &pc);
        if (pc.textured != 0) {
            VkDescriptorSet set = getDescriptorSet(r, rec.texture, rec.palette, true);
            if (!set) {
                continue;
            }
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeLayout, 0, 1,
                                    &set, 0, nullptr);
        }
        VkDeviceSize offset = rec.offset;
        vkCmdBindVertexBuffers(cb, 0, 1, &r->canvasBuffer[slot], &offset);
        vkCmdDraw(cb, 6, 1, 0, 0);
        r->drawCalls++;
    }
    r->canvasRecords.clear();
}

void mdk_render_canvas_quad(MdkRenderer rr, const mdk::CanvasQuad* q) {
    RendererVk* r = asVk(rr);
    if (!r || !r->ready || !r->inFrame || !q) {
        return;
    }

    const float xs[4] = {q->x0, q->x1, q->x2, q->x3};
    const float ys[4] = {q->y0, q->y1, q->y2, q->y3};
    const float us[4] = {q->u0, q->u1, q->u2, q->u3};
    const float vs[4] = {q->v0, q->v1, q->v2, q->v3};
    const int order[6] = {0, 1, 2, 0, 2, 3};

    mdk::Vertex verts[6];
    auto pack = [](float c) -> uint32_t {
        int v = (int)(c * 255.0f + 0.5f);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        return (uint32_t)v;
    };
    const uint32_t packed = pack(q->colour.r) | (pack(q->colour.g) << 8) |
                            (pack(q->colour.b) << 16) | (pack(q->colour.a) << 24);
    for (int i = 0; i < 6; i++) {
        const int c = order[i];
        verts[i].pos = mdk::Vec3{xs[c], ys[c], 0.0f};
        verts[i].uv = mdk::Vec2{us[c], vs[c]};
        verts[i].colour = packed;
    }

    const VkDeviceSize bytes = sizeof(verts);
    if (!ensureCanvasBuffer(r, r->canvasOffset + bytes)) {
        return;
    }
    const uint32_t slot = r->ctx.currentFrame;
    if (!r->canvasMapped[slot]) {
        refused("canvas slot %u has no mapping after a resize: this quad is not drawn", slot);
        return;
    }
    memcpy((char*)r->canvasMapped[slot] + r->canvasOffset, verts, bytes);

    PushConstants pc = {};
    canvasOrtho(pc.mvp, canvasWidth(r));
    pc.colour[0] = q->colour.r;
    pc.colour[1] = q->colour.g;
    pc.colour[2] = q->colour.b;
    pc.colour[3] = q->colour.a;
    pc.textured = (q->texture >= 0) ? ((q->palette >= 0) ? 1 : 2) : 0;
    pc.frame_count = q->frame_count > 0 ? q->frame_count : 1;
    pc.frame = 0;
    pc.unused_ = 0;

    // Under a deferred (enhanced) frame the quad is recorded, not drawn:
    // the scene pass it belongs to is replayed at end_frame, and the
    // canvas is drawn there, over the scene, in order (replayCanvas).
    if (r->deferScene) {
        RendererVk::CanvasRecord rec;
        rec.offset = r->canvasOffset;
        rec.texture = q->texture;
        rec.palette = q->palette;
        rec.frameCount = pc.frame_count;
        rec.colour[0] = q->colour.r;
        rec.colour[1] = q->colour.g;
        rec.colour[2] = q->colour.b;
        rec.colour[3] = q->colour.a;
        r->canvasRecords.push_back(rec);
        r->canvasOffset += bytes;
        return;
    }

    // The enhanced look filters the canvas even when the frame is not
    // deferred (stereo eye passes draw the canvas immediately).
    if (r->enhancedLook && r->enhancedReady) {
        drawEnhancedCanvas(r, q->texture, q->palette, pc.frame_count, pc.colour,
                           r->canvasOffset);
        r->canvasOffset += bytes;
        return;
    }

    VkPipeline pipeline = getPipeline(r, (int)mdk::Pass::Overlay, (int)mdk::Blend::Alpha, 0);
    if (!pipeline) {
        return;
    }
    VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdPushConstants(cb, r->pipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &pc);
    if (pc.textured != 0) {
        // The canvas's image repeats too: the C# draws it as an Overlay
        // material, and the Overlay path binds the REPEAT sampler.
        VkDescriptorSet set = getDescriptorSet(r, q->texture, q->palette, true);
        if (!set) {
            return;  // this quad is not drawn; see drawRecorded (review-5 F1)
        }
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeLayout, 0, 1, &set,
                                0, nullptr);
    }
    VkDeviceSize offset = r->canvasOffset;
    vkCmdBindVertexBuffers(cb, 0, 1, &r->canvasBuffer[slot], &offset);
    vkCmdDraw(cb, 6, 1, 0, 0);
    r->canvasOffset += bytes;
    r->drawCalls++;
}

void mdk_render_canvas_end(MdkRenderer r) { (void)r; }
float mdk_render_canvas_width(MdkRenderer r) {
    return canvasWidth(asVk(r));
}
int32_t mdk_render_canvas_capacity(MdkRenderer r) {
    (void)r;
    return 4096;
}

// ---------------------------------------------------------------------------
// Insets
// ---------------------------------------------------------------------------

// An inset is a 3D view inside a canvas rectangle, so its draws are their own
// scene group: the queued draws go in before the viewport changes and before it
// is restored, in the inset's own rectangle. This backend has no inset support
// yet (the rectangle is ignored), but the queue boundary is still the right one
// for the day it has.
void mdk_render_begin_inset(MdkRenderer r, const mdk::Inset* i) {
    (void)i;
    flushDraws(asVk(r));
}
void mdk_render_end_inset(MdkRenderer r) { flushDraws(asVk(r)); }

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

void mdk_render_stats(MdkRenderer r, int32_t* draw_calls, int32_t* triangles) {
    RendererVk* vr = asVk(r);
    if (draw_calls) {
        *draw_calls = vr ? vr->drawCalls : 0;
    }
    if (triangles) {
        *triangles = vr ? vr->triangles : 0;
    }
}

float mdk_render_gpu_wait_ms(MdkRenderer r) {
    (void)r;
    return 0.0f;
}

void mdk_render_capture_stats(MdkRenderer r, int32_t* requested, int32_t* written) {
    RendererVk* vr = asVk(r);
    if (requested) {
        *requested = vr ? vr->capturesRequested : 0;
    }
    if (written) {
        *written = vr ? vr->capturesWritten : 0;
    }
}
