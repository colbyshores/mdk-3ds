// 07_frame.inc -- begin/end frame, present, the frame's setters.
// Part of renderer_vk.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

void mdk_render_begin_frame(MdkRenderer rr, const MdkFrameParams* params) {
    RendererVk* r = asVk(rr);
    if (!r || !r->ready) {
        return;
    }
    if (r->inFrame) {
        return;
    }

    uint32_t w = r->ctx.extent.width;
    uint32_t h = r->ctx.extent.height;

    const uint32_t clear = params ? params->clear_colour : 0xFF000000u;
    r->clearColour = clear;
    r->backdrop = params ? params->backdrop : mdk::Backdrop::Sky;

    // The frame's first pass targets the scene target, or the left eye's
    // when stereo is on; the active camera follows the same choice. A
    // stereo resource failure falls back to the mono frame with a refusal,
    // never to a half-drawn pair of eyes.
    // The eye targets first: they own the composite pass the stereo
    // pipeline is created against, so creating the pipeline first hands
    // vkCreateGraphicsPipelines a null render pass and the frame falls
    // back to mono with the scene queue never replayed.
    const bool stereoOn = r->stereo.mode != mdk::StereoMode::Off &&
                          mdk::vk::ensureEyeTargets(&r->ctx) && ensureStereo(r);
    if (stereoOn) {
        mdk::vk::setSceneOverride(&r->ctx, r->ctx.eyeFramebuffer[0]);
        r->activeCamera = eyeCamera(r, -1.0f);
    } else {
        if (r->stereo.mode != mdk::StereoMode::Off) {
            refused("stereo frame: the eye targets or composite could not be "
                    "created, so this frame is presented mono");
        }
        mdk::vk::setSceneOverride(&r->ctx, VK_NULL_HANDLE);
        r->activeCamera = r->camera;
    }
    mdk::vk::setPresentImage(&r->ctx, VK_NULL_HANDLE);
    if (r->enhancedLook && ensureEnhanced(r)) {
        // This frame slot's enhanced sets and uniform ring start empty.
        if (vkResetDescriptorPool(r->ctx.device, r->enhancedPools[r->ctx.currentFrame],
                                  0) != VK_SUCCESS) {
            refused("enhanced descriptor pool reset: this frame's lit draws "
                    "may be dropped at allocation");
        }
        r->uniformCursor = 0;
    }
    if (r->enhancedLook && !r->enhancedReady) {
        // Stated once per distinct request (refused dedups): without the
        // lit pipeline an enhanced frame falls back to the original look.
        // Never a silent substitution.
        refused("enhanced look: the lit pipeline could not be created, so "
                "this frame renders the original look");
    }
    // A FRAME THAT DID NOT BEGIN IS NOT A FRAME. `beginFrame` returns false when the
    // fence wait, the image acquire, the command-buffer reset or the buffer's begin
    // failed — every one of which was read by nobody until this batch — and the
    // behaviour here was a bare `return`: the frame is black, the build is green,
    // and nothing anywhere says why. This is the seam, so this is where it prints,
    // through the helper that remembers each distinct request so a device that
    // refuses every frame costs one line and not one per frame.
    if (!mdk::vk::beginFrame(&r->ctx, w, h, true, clear)) {
        refused("begin the frame at %ux%u: nothing was recorded and nothing will be "
                "presented this frame",
                w, h);
        return;
    }
    r->inFrame = true;
    r->drawCalls = 0;
    r->triangles = 0;
    r->screenshotPath = params && params->screenshot_path ? params->screenshot_path : "";

    // The frame's own number, then the buffers the previous frames left behind.
    // The order matters: `settleRetired` reads frameIndex, and the fence that
    // makes those buffers safe is the one vk::beginFrame just waited.
    r->frameIndex++;
    settleRetired(r);
    r->queue.reset();

    // The viewport and scissor are dynamic state; set them once per frame so
    // every pipeline shares them.
    VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)r->ctx.extent.width;
    viewport.height = (float)r->ctx.extent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor = {{0, 0}, {r->ctx.extent.width, r->ctx.extent.height}};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &scissor);

    // The scene is deferred under the enhanced look without stereo: the
    // shadow pass at end_frame reads the whole queue, so nothing draws
    // into this first pass and the sky waits for the replay.
    r->deferScene = r->enhancedLook && r->stereo.mode == mdk::StereoMode::Off;

    // The sky is drawn first, before the level (SPEC.md 6.3 step 2).
    if (!r->deferScene) {
        drawSky(r);
    }
}

void mdk_render_end_frame(MdkRenderer rr) {
    RendererVk* r = asVk(rr);
    if (!r || !r->inFrame) {
        return;
    }
    // The deferred enhanced frame: the first pass (begun by begin_frame)
    // holds nothing; close it, render the sun's depth from the whole
    // queue, then replay sky, scene and canvas into the scene target.
    bool deferredFrame = false;
    if (r->deferScene && r->enhancedReady) {
        r->replayingEyes = true;
        mdk::vk::endPass(&r->ctx);
        if (r->lighting.shadows && r->toggles.enableSkyShadows && ensureShadow(r)) {
            drawShadowMap(r);
        }
        // The post pass, when its targets and pipelines exist and the
        // camera's clip matrices invert: the scene then renders into
        // the mipmapped scene target and is post-processed into the
        // scene target, with the canvas over it (the oracle's order).
        // Without it the frame takes the pre-post path below, and the
        // refusal has already been printed by ensurePost.
        bool post = ensurePost(r);
        if (post && !mdk::postClipMatrices(r->activeCamera.forward, r->activeCamera.up,
                                           r->activeCamera.projection, r->postClipToView,
                                           r->postViewToClip)) {
            refused("post pass: the camera's clip matrices do not invert, so the "
                    "frame renders without occlusion and glow");
            post = false;
        }
        if (post) {
            drawCameraDepth(r);
        }
        mdk::vk::beginPass(&r->ctx,
                           post ? r->postSceneFramebuffer : r->ctx.sceneFramebuffer,
                           r->ctx.renderPass, r->clearColour);
        {
            VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
            VkViewport viewport = {};
            viewport.x = 0.0f;
            viewport.y = 0.0f;
            viewport.width = (float)r->ctx.extent.width;
            viewport.height = (float)r->ctx.extent.height;
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            VkRect2D scissor = {{0, 0}, r->ctx.extent};
            vkCmdSetViewport(cb, 0, 1, &viewport);
            vkCmdSetScissor(cb, 0, 1, &scissor);
        }
        drawSky(r);
        flushDraws(r);
        r->queue.reset();
        if (!post) {
            replayCanvas(r);
        }
        mdk::vk::endPass(&r->ctx);
        if (post) {
            generateSceneMips(r);
            drawPost(r);
            mdk::vk::beginPass(&r->ctx, r->canvasFramebuffer, r->canvasLoadPass, 0);
            replayCanvas(r);
            mdk::vk::endPass(&r->ctx);
        }
        r->replayingEyes = false;
        deferredFrame = true;
    }
    bool stereoFrame = false;
    if (r->stereo.mode != mdk::StereoMode::Off && r->ctx.eyesReady && r->stereoReady) {
        // THE STEREO FRAME: replay the queued draws through the right eye,
        // then composite the two eyes into the scene target, which the
        // shared submit blits to present. The left eye's pass (begun by
        // begin_frame, sky included) is still open here.
        r->replayingEyes = true;
        flushDraws(r);
        mdk::vk::endPass(&r->ctx);

        r->activeCamera = eyeCamera(r, 1.0f);
        mdk::vk::beginPass(&r->ctx, r->ctx.eyeFramebuffer[1], r->ctx.renderPass,
                           r->clearColour);
        drawSky(r);
        flushDraws(r);
        r->queue.reset();
        mdk::vk::endPass(&r->ctx);

        // Both eyes now hold finished frames in the render pass's final
        // layout; the composite samples them.
        VkCommandBuffer cb = mdk::vk::commandBuffer(&r->ctx);
        VkImageMemoryBarrier eyeBarriers[2] = {};
        for (int eye = 0; eye < 2; eye++) {
            eyeBarriers[eye].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            eyeBarriers[eye].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            eyeBarriers[eye].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            eyeBarriers[eye].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            eyeBarriers[eye].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            eyeBarriers[eye].image = r->ctx.eyeImage[eye];
            eyeBarriers[eye].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            eyeBarriers[eye].subresourceRange.levelCount = 1;
            eyeBarriers[eye].subresourceRange.layerCount = 1;
            eyeBarriers[eye].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            eyeBarriers[eye].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        }
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 2, eyeBarriers);

        mdk::vk::beginPass(&r->ctx, r->ctx.compositeFramebuffer, r->ctx.compositePass, 0);
        drawComposite(r);
        mdk::vk::endPass(&r->ctx);
        r->replayingEyes = false;
        r->activeCamera = r->camera;

        stereoFrame = true;
    }
    // The gamma grade, when the display's gamma is not 1: the finished
    // scene (or the stereo composite) is graded into its own target and
    // that is presented instead.
    if ((stereoFrame || deferredFrame) && r->gamma != 1.0f && ensureGamma(r)) {
        drawGamma(r);
    }
    bool submitted = false;
    if (stereoFrame || deferredFrame) {
        submitted = mdk::vk::submitFrame(&r->ctx);
    }
    if (!stereoFrame && !deferredFrame && r->gamma != 1.0f && ensureGamma(r)) {
        flushDraws(r);
        mdk::vk::endPass(&r->ctx);
        drawGamma(r);
        submitted = mdk::vk::submitFrame(&r->ctx);
    } else if (!stereoFrame && !deferredFrame) {
        // The frame's queued draws go in now, in the oracle's pass order, while the
        // render pass is still open (draw_queue.h). A frontend that drew its HUD
        // already flushed at canvas_begin; this is the catch-all for a frame with no
        // canvas, and it must come before endFrame closes the render pass.
        flushDraws(r);
        // THE FRAME'S OWN RESULT WAS DISCARDED ENTIRELY. `endFrame` returns false when
        // the fence reset, the submit or the present failed; until now the frame was
        // reported as rendered either way, so a pinned frame whose commands never ran
        // would be measured, published and committed as if it were the picture.
        submitted = mdk::vk::endFrame(&r->ctx);
    }
    r->inFrame = false;
    if (!submitted) {
        refused("end the frame: its commands were not submitted or not presented, so this "
                "frame did not reach the display");
    }
    if (!r->screenshotPath.empty()) {
        const std::string path = r->screenshotPath;
        r->screenshotPath.clear();
        r->capturesRequested++;
        if (!submitted) {
            // The readback would succeed and print its usual "wrote N" line about an
            // image this frame never drew into. A frame record that cannot be trusted
            // is worse than no frame record, because a frame record gets committed.
            refused("capture the frame to %s: the frame it was asked to capture was never "
                    "submitted, so the image would not be this frame's",
                    path.c_str());
        } else if (captureSwapchain(r, path.c_str())) {
            // The one place this counter may rise: `captureSwapchain` returns false
            // for a failed open, a short write, a failed readback and a mapping it
            // could not make, and every one of those is a picture that does not exist.
            r->capturesWritten++;
        }
    }
}

void mdk_render_present(MdkRenderer rr) {
    // The present IS the frame end on this backend too.
    (void)rr;
}

void mdk_render_set_camera(MdkRenderer rr, const mdk::Camera* cam) {
    RendererVk* r = asVk(rr);
    if (!r || !cam) {
        return;
    }
    r->camera = *cam;
}

void mdk_render_set_lighting(MdkRenderer rr, const mdk::Lighting* light) {
    RendererVk* r = asVk(rr);
    if (!r || !light) {
        return;
    }
    r->lighting = *light;
}

void mdk_render_set_enhanced_toggles(MdkRenderer rr, const mdk::EnhancedToggles* toggles) {
    RendererVk* r = asVk(rr);
    if (!r || !toggles) {
        return;
    }
    r->toggles = *toggles;
}

// The PC's glow is its bloom (post.frag); the 3DS's billboards are not drawn
// here (render.h).
void mdk_render_set_glow_quads(MdkRenderer rr, const mdk::GlowQuad* quads, int32_t count) {
    (void)rr;
    (void)quads;
    (void)count;
}

void mdk_render_set_point_lights(MdkRenderer rr, const mdk::PointLight* lights, int32_t count) {
    RendererVk* r = asVk(rr);
    if (!r || !lights || count <= 0) {
        if (r) {
            r->pointLightCount = 0;
        }
        return;
    }
    const int32_t capped = (count < 64) ? count : 64;
    memcpy(r->pointLights, lights, sizeof(mdk::PointLight) * (size_t)capped);
    r->pointLightCount = capped;
}

void mdk_render_set_panorama(MdkRenderer rr, const mdk::Panorama* p) {
    RendererVk* r = asVk(rr);
    if (!r || !p) {
        return;
    }
    r->panorama = *p;
}

void mdk_render_set_fade(MdkRenderer rr, float level) {
    RendererVk* r = asVk(rr);
    if (r) {
        r->fade = level;
    }
}

void mdk_render_set_gamma(MdkRenderer rr, float gamma) {
    RendererVk* r = asVk(rr);
    if (r) {
        r->gamma = gamma;
    }
}

