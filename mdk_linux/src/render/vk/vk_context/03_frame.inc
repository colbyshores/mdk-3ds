// vk_context/03_frame.inc -- the per-frame entry points: passes, beginFrame, endFrame, submitFrame.
// Part of vk_context.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

void setSceneOverride(Context* ctx, VkFramebuffer framebuffer) {
    ctx->sceneOverride = framebuffer;
}

void setPresentImage(Context* ctx, VkImage image) {
    ctx->presentImage = image;
}

void endPass(Context* ctx) {
    FrameContext& frame = ctx->frames[ctx->currentFrame];
    vkCmdEndRenderPass(frame.commandBuffer);
}

bool beginPass(Context* ctx, VkFramebuffer framebuffer, VkRenderPass pass,
               uint32_t clearColour) {
    FrameContext& frame = ctx->frames[ctx->currentFrame];
    // The clear colour is 0xAARRGGBB (the seam's convention); the swapchain
    // is B8G8R8A8, so the bytes land reversed. UNORM, not SRGB: the C#
    // clears with raw bytes and does its own gamma at the end.
    VkClearValue clears[2] = {};
    clears[0].color = {{
        (float)((clearColour >> 16) & 0xFFu) / 255.0f,
        (float)((clearColour >> 8) & 0xFFu) / 255.0f,
        (float)(clearColour & 0xFFu) / 255.0f,
        (float)(clearColour >> 24) / 255.0f,
    }};
    clears[1].depthStencil = {1.0f, 0};

    const bool scenePass = pass == ctx->renderPass;
    VkRenderPassBeginInfo passInfo = {};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = pass;
    passInfo.framebuffer = framebuffer;
    passInfo.renderArea.extent = ctx->extent;
    passInfo.clearValueCount = scenePass ? 2 : 0;
    passInfo.pClearValues = scenePass ? clears : nullptr;

    vkCmdBeginRenderPass(frame.commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
    return true;
}

bool beginDepthPass(Context* ctx, VkFramebuffer framebuffer, VkRenderPass pass,
                    uint32_t width, uint32_t height) {
    FrameContext& frame = ctx->frames[ctx->currentFrame];
    VkClearValue clear = {};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo passInfo = {};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = pass;
    passInfo.framebuffer = framebuffer;
    passInfo.renderArea.offset = {0, 0};
    passInfo.renderArea.extent = {width, height};
    passInfo.clearValueCount = 1;
    passInfo.pClearValues = &clear;
    vkCmdBeginRenderPass(frame.commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
    return true;
}

bool beginFrame(Context* ctx, uint32_t width, uint32_t height, bool vsync, uint32_t clearColour) {
    if (ctx->extent.width != width || ctx->extent.height != height) {
        if (!recreateSwapchain(ctx, width, height, vsync)) {
            return false;
        }
    }

    // THE FENCE WAIT IS THE COMPLETION BOUNDARY, so its result is the answer to "is
    // it safe to touch this frame's resources". Discarding it means a failed wait —
    // or a lost device — goes on to reset, begin and record a command buffer on a
    // device that will not run it. It returns false quietly on purpose: the seam
    // prints, once per distinct request, because a frame-boundary failure repeats
    // every frame and this file has no refusal memory of its own.
    // WHY A REFUSED BEGIN NEEDS NO FENCE RECOVERY, AND WHY A REFUSED SUBMIT DOES.
    // `vkWaitForFences` does not RESET a fence: a successful wait leaves it signalled,
    // and only `vkResetFences` — which this function never calls, and which C3 of
    // linux_refusal_report_test.sh keeps it that way — takes it back. So every
    // `return false` below leaves this slot's fence SIGNALLED, which is the state the
    // slot is supposed to be in when it is free, and the next `beginFrame` waits on a
    // signalled fence and returns immediately. The slot is reused; nothing hangs.
    // endFrame's failed submit is the opposite case: there the fence HAD been reset
    // for a submission that never happened, so nothing would ever signal it, and that
    // one does carry the signalled replacement it has.
    FrameContext& frame = ctx->frames[ctx->currentFrame];
    VkResult result = vkWaitForFences(ctx->device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        return false;
    }

    uint32_t imageIndex = 0;
    result = vkAcquireNextImageKHR(ctx->device, ctx->swapchain, UINT64_MAX,
                                            frame.presentWait, VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        if (!recreateSwapchain(ctx, width, height, vsync)) {
            return false;
        }
        result = vkAcquireNextImageKHR(ctx->device, ctx->swapchain, UINT64_MAX,
                                       frame.presentWait, VK_NULL_HANDLE, &imageIndex);
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return false;
    }
    ctx->currentImage = imageIndex;

    // A command buffer that failed to RESET is still recording, and beginning a
    // recording command buffer is invalid use. A BEGIN that failed leaves every
    // vkCmd* the renderer then issues invalid, so the frame would report success
    // with nothing in it — the invisible empty picture, at the widest possible
    // scale. Both are refused where they become known.
    //
    // WHAT THESE TWO EXITS COST, and what is done about it: the acquire has already
    // posted `presentWait`, and a binary semaphore that stays posted is not a harmless
    // leftover — the next frame's acquire would hand a signalled one to
    // `vkAcquireNextImageKHR`, which is invalid use. So the signal is consumed here,
    // by the empty submit `drainPresentWait` explains. What is NOT repaired is the
    // OUT_OF_DATE retry above: a failed acquire's semaphore is not known to be
    // untouched, and waiting on a signal that may never arrive is a queue stall, not a
    // fix. The obvious reordering — reset and begin BEFORE the acquire — was considered
    // and rejected: a later acquire failure would then leave the command buffer
    // recording, and `vkResetCommandBuffer` on a recording buffer is invalid use.
    result = vkResetCommandBuffer(frame.commandBuffer, 0);
    if (result != VK_SUCCESS) {
        drainPresentWait(ctx, frame);
        return false;
    }
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
        drainPresentWait(ctx, frame);
        return false;
    }

    // The scene renders into the scene target (or, while the renderer is
    // drawing a stereo eye, into that eye's target): see beginPass below.
    beginPass(ctx,
              ctx->sceneOverride != VK_NULL_HANDLE ? ctx->sceneOverride
                                                   : ctx->sceneFramebuffer,
              ctx->renderPass, clearColour);

    // The viewport and scissor are dynamic state and MUST be set every frame;
    // without them nothing rasterizes. POSITIVE height: the y flip that puts
    // the picture upright is folded into the projection matrix
    // (renderer_vk.cpp flipClipY), not into the viewport. A negative height
    // would flip it a second time and show the world upside down.
    //
    // The renderer sets its own viewport again right after this one, so this
    // value is not the one that rasterizes anything; it is here so that a frame
    // with a begin and no further state still has defined viewport state. It
    // is kept consistent with the renderer on purpose -- two comments in this
    // tree used to claim the opposite ("the negative viewport height restores
    // the oracle's orientation") and a reader could not tell which one was
    // live.
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)ctx->extent.width;
    viewport.height = (float)ctx->extent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    VkRect2D scissor = {{0, 0}, ctx->extent};
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
    return true;
}

bool endFrame(Context* ctx) {
    FrameContext& frame = ctx->frames[ctx->currentFrame];
    vkCmdEndRenderPass(frame.commandBuffer);

    // PRESENT THE SCENE TARGET. The render pass left the scene image in its
    // colour attachment's final layout (PRESENT_SRC_KHR); move it to a blit
    // source, blit it whole into the acquired swapchain image, and leave that
    // image in PRESENT_SRC_KHR for vkQueuePresentKHR. Same format and extent
    // on both sides, so NEAREST is an exact copy: the presented bytes are the
    // bytes the scene pass produced, unchanged.
    {
        VkImage swapImage = ctx->images[ctx->currentImage];
        VkImageMemoryBarrier barriers[2] = {};
        barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].image = ctx->presentImage != VK_NULL_HANDLE ? ctx->presentImage
                                                                 : ctx->sceneImage;
        barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[0].subresourceRange.levelCount = 1;
        barriers[0].subresourceRange.layerCount = 1;
        barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].image = swapImage;
        barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[1].subresourceRange.levelCount = 1;
        barriers[1].subresourceRange.layerCount = 1;
        barriers[1].srcAccessMask = 0;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(frame.commandBuffer,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             2, barriers);

        VkImageBlit blit = {};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        blit.srcOffsets[1].x = (int32_t)ctx->extent.width;
        blit.srcOffsets[1].y = (int32_t)ctx->extent.height;
        blit.srcOffsets[1].z = 1;
        blit.dstOffsets[1].x = (int32_t)ctx->extent.width;
        blit.dstOffsets[1].y = (int32_t)ctx->extent.height;
        blit.dstOffsets[1].z = 1;
        vkCmdBlitImage(frame.commandBuffer,
                       ctx->presentImage != VK_NULL_HANDLE ? ctx->presentImage
                                                           : ctx->sceneImage,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_NEAREST);

        VkImageMemoryBarrier present = {};
        present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        present.image = swapImage;
        present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        present.subresourceRange.levelCount = 1;
        present.subresourceRange.layerCount = 1;
        present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        present.dstAccessMask = 0;
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &present);
    }

    // THE COMMAND STREAM HAS TO BE CLOSED BEFORE IT IS SUBMITTED. Only a pending or an
    // executable command buffer may be named by `VkSubmitInfo::pCommandBuffers`
    // (VUID-vkSubmitInfo-pCommandBuffers-00075), and a buffer that was begun and never
    // ended is still RECORDING. This file submitted one every frame. That it drew 138
    // committed frames is evidence that lavapipe does not enforce the state machine and
    // no evidence at all that the submission was legal (review-1 F1); the one-shot path
    // in renderer_vk.cpp already refused to submit an unfinalised buffer, and the frame
    // path is the same rule at the frame's scale.
    //
    // The end sits BEFORE the fence reset deliberately: a refused frame has to leave its
    // fence signalled (see the note in beginFrame), and the reset is the one call here
    // that would take it away. A failed end does leave the buffer recording — nothing
    // un-records a buffer — so the next frame's reset is then invalid use too; that is
    // a driver that has already refused an end, and it is printed rather than hidden.
    const VkResult ended = vkEndCommandBuffer(frame.commandBuffer);
    if (ended != VK_SUCCESS) {
        drainPresentWait(ctx, frame);
        vkFailed("vkEndCommandBuffer(the frame's command buffer; nothing was submitted)",
                 ended);
        return false;
    }

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &frame.presentWait;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &frame.renderFinished;

    // The reset is read too: if it failed the fence's state is unknown, and a
    // submit that relies on that fence is a guess. Refuse BEFORE the submit, so
    // nothing is queued behind a fence nobody can wait on.
    VkResult result = vkResetFences(ctx->device, 1, &frame.inFlight);
    if (result != VK_SUCCESS) {
        drainPresentWait(ctx, frame);
        return false;
    }
    result = vkQueueSubmit(ctx->queue, 1, &submitInfo, frame.inFlight);
    if (result != VK_SUCCESS) {
        // The queue refused the frame, so nothing waited on `presentWait` and its
        // signal is still posted. Consume it before the fence is touched: the next
        // frame's acquire is invalid use until somebody does.
        drainPresentWait(ctx, frame);
        // The fence was reset and nothing is going to signal it, so the NEXT
        // beginFrame would wait UINT64_MAX on a fence no submission will ever
        // signal: one refused submit becomes a hang two frames later. The fence is
        // not associated with any queue (the submit failed), so the way back to the
        // invariant "this slot's fence is signalled when the slot is free" is a
        // fresh signalled one. It is CREATED BEFORE the old one is destroyed, so a
        // failed recovery changes nothing and leaves the hazard printed rather than
        // swapping a hang for a null fence. (`vkSignalFence` is the direct way back
        // and is not declared by the vendored header this build compiles against,
        // so it is not available here.)
        VkFence replacement = VK_NULL_HANDLE;
        VkFenceCreateInfo recover = {};
        recover.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        recover.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        const VkResult refence = vkCreateFence(ctx->device, &recover, nullptr, &replacement);
        if (refence == VK_SUCCESS) {
            vkDestroyFence(ctx->device, frame.inFlight, nullptr);
            frame.inFlight = replacement;
        } else {
            vkFailed("vkCreateFence(replacing the fence of a refused submit; the next "
                     "frame's fence wait may not return)", refence);
        }
        return false;
    }

    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &frame.renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &ctx->swapchain;
    presentInfo.pImageIndices = &ctx->currentImage;
    result = vkQueuePresentKHR(ctx->queue, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        // The window was resized. The swapchain is rebuilt on the next
        // frame; this frame is simply dropped.
        return true;
    }
    if (result != VK_SUCCESS) {
        return false;
    }

    ctx->currentFrame = (ctx->currentFrame + 1) % kFramesInFlight;
    return true;
}


bool submitFrame(Context* ctx) {
    FrameContext& frame = ctx->frames[ctx->currentFrame];

    // PRESENT THE SCENE TARGET. The render pass left the scene image in its
    // colour attachment's final layout (PRESENT_SRC_KHR); move it to a blit
    // source, blit it whole into the acquired swapchain image, and leave that
    // image in PRESENT_SRC_KHR for vkQueuePresentKHR. Same format and extent
    // on both sides, so NEAREST is an exact copy: the presented bytes are the
    // bytes the scene pass produced, unchanged.
    {
        VkImage swapImage = ctx->images[ctx->currentImage];
        VkImageMemoryBarrier barriers[2] = {};
        barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].image = ctx->presentImage != VK_NULL_HANDLE ? ctx->presentImage
                                                                 : ctx->sceneImage;
        barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[0].subresourceRange.levelCount = 1;
        barriers[0].subresourceRange.layerCount = 1;
        barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].image = swapImage;
        barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[1].subresourceRange.levelCount = 1;
        barriers[1].subresourceRange.layerCount = 1;
        barriers[1].srcAccessMask = 0;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(frame.commandBuffer,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             2, barriers);

        VkImageBlit blit = {};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        blit.srcOffsets[1].x = (int32_t)ctx->extent.width;
        blit.srcOffsets[1].y = (int32_t)ctx->extent.height;
        blit.srcOffsets[1].z = 1;
        blit.dstOffsets[1].x = (int32_t)ctx->extent.width;
        blit.dstOffsets[1].y = (int32_t)ctx->extent.height;
        blit.dstOffsets[1].z = 1;
        vkCmdBlitImage(frame.commandBuffer,
                       ctx->presentImage != VK_NULL_HANDLE ? ctx->presentImage
                                                           : ctx->sceneImage,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_NEAREST);

        VkImageMemoryBarrier present = {};
        present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        present.image = swapImage;
        present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        present.subresourceRange.levelCount = 1;
        present.subresourceRange.layerCount = 1;
        present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        present.dstAccessMask = 0;
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &present);
    }

    // THE COMMAND STREAM HAS TO BE CLOSED BEFORE IT IS SUBMITTED. Only a pending or an
    // executable command buffer may be named by `VkSubmitInfo::pCommandBuffers`
    // (VUID-vkSubmitInfo-pCommandBuffers-00075), and a buffer that was begun and never
    // ended is still RECORDING. This file submitted one every frame. That it drew 138
    // committed frames is evidence that lavapipe does not enforce the state machine and
    // no evidence at all that the submission was legal (review-1 F1); the one-shot path
    // in renderer_vk.cpp already refused to submit an unfinalised buffer, and the frame
    // path is the same rule at the frame's scale.
    //
    // The end sits BEFORE the fence reset deliberately: a refused frame has to leave its
    // fence signalled (see the note in beginFrame), and the reset is the one call here
    // that would take it away. A failed end does leave the buffer recording — nothing
    // un-records a buffer — so the next frame's reset is then invalid use too; that is
    // a driver that has already refused an end, and it is printed rather than hidden.
    const VkResult ended = vkEndCommandBuffer(frame.commandBuffer);
    if (ended != VK_SUCCESS) {
        drainPresentWait(ctx, frame);
        vkFailed("vkEndCommandBuffer(the frame's command buffer; nothing was submitted)",
                 ended);
        return false;
    }

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &frame.presentWait;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &frame.renderFinished;

    // The reset is read too: if it failed the fence's state is unknown, and a
    // submit that relies on that fence is a guess. Refuse BEFORE the submit, so
    // nothing is queued behind a fence nobody can wait on.
    VkResult result = vkResetFences(ctx->device, 1, &frame.inFlight);
    if (result != VK_SUCCESS) {
        drainPresentWait(ctx, frame);
        return false;
    }
    result = vkQueueSubmit(ctx->queue, 1, &submitInfo, frame.inFlight);
    if (result != VK_SUCCESS) {
        // The queue refused the frame, so nothing waited on `presentWait` and its
        // signal is still posted. Consume it before the fence is touched: the next
        // frame's acquire is invalid use until somebody does.
        drainPresentWait(ctx, frame);
        // The fence was reset and nothing is going to signal it, so the NEXT
        // beginFrame would wait UINT64_MAX on a fence no submission will ever
        // signal: one refused submit becomes a hang two frames later. The fence is
        // not associated with any queue (the submit failed), so the way back to the
        // invariant "this slot's fence is signalled when the slot is free" is a
        // fresh signalled one. It is CREATED BEFORE the old one is destroyed, so a
        // failed recovery changes nothing and leaves the hazard printed rather than
        // swapping a hang for a null fence. (`vkSignalFence` is the direct way back
        // and is not declared by the vendored header this build compiles against,
        // so it is not available here.)
        VkFence replacement = VK_NULL_HANDLE;
        VkFenceCreateInfo recover = {};
        recover.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        recover.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        const VkResult refence = vkCreateFence(ctx->device, &recover, nullptr, &replacement);
        if (refence == VK_SUCCESS) {
            vkDestroyFence(ctx->device, frame.inFlight, nullptr);
            frame.inFlight = replacement;
        } else {
            vkFailed("vkCreateFence(replacing the fence of a refused submit; the next "
                     "frame's fence wait may not return)", refence);
        }
        return false;
    }

    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &frame.renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &ctx->swapchain;
    presentInfo.pImageIndices = &ctx->currentImage;
    result = vkQueuePresentKHR(ctx->queue, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        // The window was resized. The swapchain is rebuilt on the next
        // frame; this frame is simply dropped.
        return true;
    }
    if (result != VK_SUCCESS) {
        return false;
    }

    ctx->currentFrame = (ctx->currentFrame + 1) % kFramesInFlight;
    return true;
}

VkCommandBuffer commandBuffer(Context* ctx) {
    return ctx->frames[ctx->currentFrame].commandBuffer;
}
