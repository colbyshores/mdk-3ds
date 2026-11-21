// 14_canvas_seam.inc -- the canvas seam, insets, diagnostics.
// Part of renderer_vk.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

void mdk_render_canvas_begin(MdkRenderer r) {
    RendererVk* vk = asVk(r);
    if (vk) {
        // The scene ends here: the queued draws go in BEFORE any canvas quad,
        // because the canvas is the Overlay pass drawn over the scene and the
        // oracle draws its Overlay bucket last (draw_queue.h). Then the insets
        // under the canvas (Renderer.Insets.cs RenderInsets: scene, insets under
        // the canvas, canvas, insets over it), which a Scene inset draws the
        // queue again for -- so the queue is dropped only after them. A deferred
        // or stereo frame does all of this at its end instead (flushDraws waits).
        const bool direct = vk->inFrame && vk->stereo.mode == mdk::StereoMode::Off &&
                            !vk->enhancedLook;
        if (direct && !vk->insetsUnderDrawn) {
            drawBuckets(vk, vk->queue, true);
            renderInsets(vk, true);
            vk->queue.reset();
        } else {
            flushDraws(vk);
        }
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

// The C#'s Renderer.Insets.cs. BeginInset RECORDS the inset (no flush: a Scene
// inset is the whole frame's queue seen again, the draws queued after it
// included, exactly as the C#'s RenderInset reads `_commands` at present time);
// an Own inset takes the draws made until EndInset into its own queue
// (mdk_render_draw_mesh). renderInsets draws them: after the scene for the ones
// under the canvas, after the canvas for the others.
void mdk_render_begin_inset(MdkRenderer rr, const mdk::Inset* i) {
    RendererVk* r = asVk(rr);
    if (!r || !i || !r->inFrame) {
        return;
    }
    if (r->insetCount == r->insets.size()) {
        r->insets.push_back(RendererVk::InsetVk());  // a new most-insets frame only
    }
    RendererVk::InsetVk& slot = r->insets[r->insetCount++];
    slot.inset = *i;
    slot.own.reset();
    r->openInset = i->scene ? -1 : (int32_t)(r->insetCount - 1);
}
void mdk_render_end_inset(MdkRenderer rr) {
    RendererVk* r = asVk(rr);
    if (r) {
        r->openInset = -1;
    }
}

// The C#'s RenderInsets(commands, layer) and RenderInset: each inset of the layer
// in its rectangle (canvas units scaled to the target, the viewport and the
// scissor), its depth cleared, through its own camera: the sky and the frame's
// queue for a Scene inset, its own queue otherwise, without the Overlay bucket.
// Called inside an open pass that has a depth attachment (the scene pass, or the
// post's canvas pass). The frame's camera, viewport and scissor are put back.
void renderInsets(RendererVk* r, bool underCanvas) {
    if (!r || !r->inFrame) {
        return;
    }
    if (underCanvas) {
        r->insetsUnderDrawn = true;
    }
    if (r->insetCount == 0) {
        return;
    }
    if (r->stereo.mode != mdk::StereoMode::Off) {
        // The C# draws an inset per eye (View.Eye); the eye passes here replay the
        // scene queue only. Stated, not hidden: a stereo frame has no insets.
        refused("insets: a stereo frame draws no insets (sniper mode's round cameras and "
                "clip are left out)");
        return;
    }
    VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
    const float scale = (float)r->ctx.extent.height / mdk::kCanvasHeight;
    const mdk::Camera frameCamera = r->camera;
    const mdk::Camera frameActive = r->activeCamera;
    for (size_t n = 0; n < r->insetCount; n++) {
        const RendererVk::InsetVk& slot = r->insets[n];
        const mdk::Inset& inset = slot.inset;
        if (inset.under_canvas != underCanvas) {
            continue;
        }
        VkViewport viewport = {};
        viewport.x = inset.x * scale;
        viewport.y = inset.y * scale;
        viewport.width = inset.width * scale;
        viewport.height = inset.height * scale;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        int32_t sx = (int32_t)viewport.x;
        int32_t sy = (int32_t)viewport.y;
        int32_t sw = (int32_t)std::ceil(viewport.width);
        int32_t sh = (int32_t)std::ceil(viewport.height);
        // Clipped to the target: Vulkan refuses a scissor outside it.
        if (sx < 0) {
            sw += sx;
            sx = 0;
        }
        if (sy < 0) {
            sh += sy;
            sy = 0;
        }
        if (sx + sw > (int32_t)r->ctx.extent.width) {
            sw = (int32_t)r->ctx.extent.width - sx;
        }
        if (sy + sh > (int32_t)r->ctx.extent.height) {
            sh = (int32_t)r->ctx.extent.height - sy;
        }
        if (sw <= 0 || sh <= 0 || viewport.width <= 0.0f || viewport.height <= 0.0f) {
            continue;
        }
        VkRect2D scissor = {{sx, sy}, {(uint32_t)sw, (uint32_t)sh}};
        vkCmdSetViewport(cb, 0, 1, &viewport);
        vkCmdSetScissor(cb, 0, 1, &scissor);
        // The C#'s BeginOver: the rectangle's depth cleared.
        VkClearAttachment clear = {};
        clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        clear.clearValue.depthStencil.depth = 1.0f;
        clear.clearValue.depthStencil.stencil = 0;
        VkClearRect clearRect = {};
        clearRect.rect = scissor;
        clearRect.baseArrayLayer = 0;
        clearRect.layerCount = 1;
        vkCmdClearAttachments(cb, 1, &clear, 1, &clearRect);

        r->camera = inset.camera;
        r->activeCamera = inset.camera;
        if (inset.scene) {
            drawSky(r);
            drawBuckets(r, r->queue, false);
        } else {
            drawBuckets(r, slot.own, false);
        }
    }
    r->camera = frameCamera;
    r->activeCamera = frameActive;
    VkViewport full = {};
    full.width = (float)r->ctx.extent.width;
    full.height = (float)r->ctx.extent.height;
    full.minDepth = 0.0f;
    full.maxDepth = 1.0f;
    VkRect2D fullScissor = {{0, 0}, r->ctx.extent};
    vkCmdSetViewport(cb, 0, 1, &full);
    vkCmdSetScissor(cb, 0, 1, &fullScissor);
}

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
