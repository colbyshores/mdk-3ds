// 12_draw.inc -- recording one queued draw, flushing the queue by pass.
// Part of renderer_vk.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.


// A MIRROR (Pass::Mirror, MIRRLOW...MIRRHIGH): the oracle's DrawOne mirror
// branch (Renderer.cs:1082-1092) -- nothing without a panorama, otherwise the
// mirror program over the mirror panorama and the palette, CLAMPED like the
// sky's (BindPanorama), with the camera and the material's row shift. `wv` is
// the draw's world (depth pull included) times the view, as drawRecorded made it.
//
// Until 2026-11-27 this backend had no mirror program, and a mirror drew as the
// resolver's placeholder material: no texture, colour white -- the owner's
// "white panels" (LEVEL6's OLYM_6/7 floors, and LEVEL3/5/8's mirror tiles).
static void drawMirror(RendererVk* r, const mdk::MeshDraw* draw, const float wv[16]) {
    if (r->panorama.mirror_sky < 0 || r->panorama.palette < 0) {
        return;
    }
    MirrorPush mp;
    mulMatrix(wv, r->activeCamera.projection, mp.mvp);
    flipClipY(mp.mvp);   // Vulkan's framebuffer runs y down; see flipClipY
    mp.wrap_width = r->panorama.wrap_width;
    mp.horizon_row = r->panorama.horizon_row;
    mp.offset = r->panorama.offset;
    mp.height = r->panorama.height;
    mp.camera[0] = r->activeCamera.position.x;
    mp.camera[1] = r->activeCamera.position.y;
    mp.camera[2] = r->activeCamera.position.z;
    mp.row_shift = draw->material.row_shift;

    const int primitive = (draw->primitive == mdk::Primitive::Lines) ? 1 : 0;
    VkPipeline pipeline =
        getPipeline(r, (int)mdk::Pass::Mirror, (int)draw->material.blend, primitive, 0);
    if (!pipeline) {
        return;
    }
    VkDescriptorSet set = getDescriptorSet(r, r->panorama.mirror_sky, r->panorama.palette, false);
    if (!set) {
        return;  // no set means no draw, not a stale one: see drawRecorded
    }
    const MeshVk& mesh = r->meshes[draw->mesh];
    const VkBuffer buffer = mesh.buffer[meshSlot(mesh, &r->ctx)];
    if (!buffer) {
        return;
    }
    VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdPushConstants(cb, r->pipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(MirrorPush), &mp);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeLayout, 0, 1, &set, 0,
                            nullptr);
    const VkDeviceSize offset = sizeof(mdk::Vertex) * (VkDeviceSize)draw->first_vertex;
    vkCmdBindVertexBuffers(cb, 0, 1, &buffer, &offset);
    vkCmdDraw(cb, (uint32_t)draw->vertex_count, 1, 0, 0);
    r->drawCalls++;
    if (!primitive) {
        r->triangles += draw->vertex_count / 3;
    }
}

// Record one queued draw into this frame's command buffer. This is what the
// oracle's `DrawOne` is (Renderer.cs:1060-1110): a pipeline for the pass, the
// matrices, the texture pair, one draw.
//
// It runs at FLUSH time, not at queue time, so the frame-wide state it reads --
// the camera, the viewport, the render pass -- is the state the frame is drawn
// with, exactly as the C# reads `view.ViewProjection` when it renders the
// command rather than when the view queued it.
//
// `cover`: this draw's depth only, through getCoverPipeline (render/draw_queue.h
// step 2), the same matrices and texture so the same fragments are kept (the
// caller has narrowed the viewport's depth range to the far plane).
void drawRecorded(RendererVk* r, const mdk::MeshDraw* draw, bool cover) {
    if (draw->mesh < 0 || draw->mesh >= (int32_t)r->meshes.size() || !r->meshes[draw->mesh].used) {
        return;
    }
    if (draw->vertex_count <= 0) {
        return;
    }
    // Kurt's marks and his draw over them need the stencil (render_types.h
    // KurtPass); without one only his Tested draw is made (upstream's depth test).
    if (!r->ctx.depthHasStencil &&
        (draw->kurt == mdk::KurtPass::Mark || draw->kurt == mdk::KurtPass::MarkGlass ||
         draw->kurt == mdk::KurtPass::Over)) {
        return;
    }

    // clip = p * world * depth_pull * view * projection -- the row-vector
    // chain, the same order the C# uses (Renderer.cs:1072). The depth pull
    // keeps a detail coplanar with the surface under it in the same place on
    // screen, but nearer in the depth buffer.
    float world[16];
    if (draw->material.depth_layer != 0 || draw->primitive == mdk::Primitive::Lines) {
        float pulled[16];
        mdk::buildDepthPullMatrix(r->activeCamera.position, draw->primitive,
                                  draw->material.depth_layer, pulled);
        mulMatrix(draw->world, pulled, world);
    } else {
        memcpy(world, draw->world, sizeof(world));
    }
    float wv[16];
    mulMatrix(world, r->activeCamera.view, wv);

    // A mirror has its own program, in both looks (the C#'s EnhancedMode
    // returns null for Pass.Mirror, Renderer.Enhanced.cs:283).
    if (!cover && draw->material.pass == mdk::Pass::Mirror) {
        drawMirror(r, draw, wv);
        return;
    }

    // The enhanced look's Lit/Sprite draws go through the lit pipeline;
    // everything else (Original shading, mirrors, the canvas's own draws)
    // keeps the palette path below.
    if (!cover && r->enhancedLook && r->enhancedReady &&
        (draw->material.shading == mdk::Shading::Lit ||
         draw->material.shading == mdk::Shading::Sprite)) {
        drawEnhanced(r, draw, world, wv);
        return;
    }

    PushConstants pc;
    mulMatrix(wv, r->activeCamera.projection, pc.mvp);
    // Vulkan's framebuffer runs y down; see flipClipY.
    flipClipY(pc.mvp);
    pc.colour[0] = draw->material.colour.r;
    pc.colour[1] = draw->material.colour.g;
    pc.colour[2] = draw->material.colour.b;
    pc.colour[3] = draw->material.colour.a;
    pc.textured = 0;
    if (draw->material.texture >= 0 &&
        draw->material.texture < (int32_t)r->textures.size() &&
        r->textures[draw->material.texture].used) {
        pc.textured = r->textures[draw->material.texture].isIndex ? 1 : 2;
    }
    pc.frame_count = draw->material.frame_count > 0 ? draw->material.frame_count : 1;
    pc.frame = draw->frame;
    pc.unused_ = 0;

    const int pass = (int)draw->material.pass;
    const int blend = (int)draw->material.blend;
    const int primitive = (draw->primitive == mdk::Primitive::Lines) ? 1 : 0;
    VkPipeline pipeline =
        cover ? getCoverPipeline(r, pass)
              : getPipeline(r, pass, blend, primitive, kurtPipelinePass(r, draw));
    if (!pipeline) {
        return;
    }

    VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdPushConstants(cb, r->pipeLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &pc);

    if (pc.textured != 0) {
        // A surface's texture REPEATS, like the C#'s (Renderer.cs:1098).
        VkDescriptorSet set = getDescriptorSet(r, draw->material.texture, draw->material.palette, true);
        if (!set) {
            // SKIPPED, NOT DRAWN BLIND. A textured draw that does not bind a set
            // samples whatever the PREVIOUS draw bound: a wall wearing another
            // surface's pixels, with nothing in the log to say so. That is a
            // wrong picture, which is worse than the missing one every other
            // refusal here produces (review-5 F1).
            return;
        }
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeLayout, 0, 1,
                                &set, 0, nullptr);
    }

    // THE SLOT, not "the" buffer: a dynamic mesh is drawn from the slot this
    // frame wrote, which is the one whose fence beginFrame waited.
    const MeshVk& mesh = r->meshes[draw->mesh];
    const VkBuffer buffer = mesh.buffer[meshSlot(mesh, &r->ctx)];
    if (!buffer) {
        return;
    }
    const VkDeviceSize offset = sizeof(mdk::Vertex) * (VkDeviceSize)draw->first_vertex;
    vkCmdBindVertexBuffers(cb, 0, 1, &buffer, &offset);
    vkCmdDraw(cb, (uint32_t)draw->vertex_count, 1, 0, 0);

    r->drawCalls++;
    if (!primitive) {
        r->triangles += draw->vertex_count / 3;
    }
}

// Draw everything the frame has queued, in the oracle's pass order: every
// `Solid` batch, then `DoubleSided`, `Mirror`, `Blended`, `OnTop` and
// `Overlay`, each bucket in the order it was queued (draw_queue.h).
//
// The flush point is the end of the frame's 3D scene, which is where the oracle
// closes its render pass: before the 2D canvas, before an inset changes the
// viewport, and at the frame's end. A frontend that drew a mesh AFTER its
// canvas would get a second, later group -- the canvas is Overlay and the
// oracle draws the canvas last, so nothing correct does that.
void flushDraws(RendererVk* r) {
    if (!r || r->queue.empty() || !r->inFrame) {
        return;
    }
    // Under stereo the queue is replayed once per eye at end_frame, so a
    // mid-frame flush (canvas_begin, an inset boundary) must not consume
    // it: the draws wait and are recorded twice, once through each eye's
    // camera. The canvas's own immediate quads are not queued and still
    // draw into the left eye's pass only -- the seam hands the backend no
    // way to replay a frontend's canvas calls, so the HUD appears in the
    // left half of a stereo frame. Stated here because the composite
    // cannot say it.
    if (r->stereo.mode != mdk::StereoMode::Off && !r->replayingEyes) {
        return;
    }
    // Under the enhanced look the scene is deferred to end_frame (the
    // shadow pass reads the whole queue first), so a mid-frame flush
    // waits for the replay there too.
    if (r->enhancedLook && !r->replayingEyes) {
        return;
    }
    drawBuckets(r, r->queue, true);
    if (!r->replayingEyes) {
        r->queue.reset();
    }
}

// Draw a queue's buckets in the pass order -- the frame's queue, or an Own
// inset's -- through the camera installed now. `overlay` false leaves the Overlay
// bucket out, as the C#'s RenderInset does (Renderer.Insets.cs:113-119).
//
// THE CAMERA'S ARENA OVER THE OTHER ONE (render/draw_queue.h): the other
// arena's `back` draws first, then the camera arena's `cover` draws' depth over
// them, then every bucket as before. Without back draws only the last runs.
void drawBuckets(RendererVk* r, const mdk::DrawQueue& queue, bool overlay) {
    if (queue.hasBack()) {
        for (int32_t pass = 0; pass < mdk::kDrawQueuePassCount; pass++) {
            const std::vector<mdk::MeshDraw>& back = queue.backBucket(pass);
            for (size_t i = 0; i < back.size(); i++) {
                if (!r->toggles.enableMirrors && back[i].material.pass == mdk::Pass::Mirror) {
                    continue;
                }
                drawRecorded(r, &back[i], false);
            }
        }
        // The cover step writes the FAR PLANE (the viewport's depth range
        // narrowed to 1) under the camera arena's opaque cover draws, so step 3's
        // draw of the same batch passes whatever its shader's rounding: the
        // enhanced look draws it through another vertex program than this one.
        VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
        VkViewport farOnly = r->sceneViewport;
        farOnly.minDepth = 1.0f;
        farOnly.maxDepth = 1.0f;
        vkCmdSetViewport(cb, 0, 1, &farOnly);
        for (int32_t pass = static_cast<int32_t>(mdk::Pass::Solid);
             pass <= static_cast<int32_t>(mdk::Pass::DoubleSided); pass++) {
            const std::vector<mdk::MeshDraw>& bucket = queue.bucket(pass);
            for (size_t i = 0; i < bucket.size(); i++) {
                if (bucket[i].cover && bucket[i].primitive == mdk::Primitive::Triangles) {
                    drawRecorded(r, &bucket[i], true);
                }
            }
        }
        vkCmdSetViewport(cb, 0, 1, &r->sceneViewport);
    }
    for (int32_t pass = 0; pass < mdk::kDrawQueuePassCount; pass++) {
        if (!overlay && pass == static_cast<int32_t>(mdk::Pass::Overlay)) {
            continue;
        }
        const std::vector<mdk::MeshDraw>& bucket = queue.bucket(pass);
        for (size_t i = 0; i < bucket.size(); i++) {
            // enableMirrors off: mirror draws are skipped whole, at
            // queue setup, not shaded around (SPEC.md 7.9).
            if (!r->toggles.enableMirrors && bucket[i].material.pass == mdk::Pass::Mirror) {
                continue;
            }
            if (mdk::kurtAfterMirrors(bucket[i])) {
                continue;   // after the mirrors, below
            }
            drawRecorded(r, &bucket[i], false);
        }
        // KURT'S PASSES after the Mirror bucket, in queue order (draw_queue.h).
        if (pass == static_cast<int32_t>(mdk::Pass::Mirror)) {
            const std::vector<mdk::MeshDraw>& kurt =
                queue.bucket(static_cast<int32_t>(mdk::Pass::DoubleSided));
            for (size_t i = 0; i < kurt.size(); i++) {
                if (mdk::kurtAfterMirrors(kurt[i])) {
                    drawRecorded(r, &kurt[i], false);
                }
            }
        }
    }
}

