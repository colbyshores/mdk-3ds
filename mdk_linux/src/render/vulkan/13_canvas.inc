// 13_canvas.inc -- the canvas ring's geometry and buffer.
// Part of renderer_vk.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

// The C# Renderer.CanvasWidth: 360 units high, as wide as the current target.
float canvasWidth(RendererVk* r) {
    const uint32_t w = r ? r->ctx.extent.width : 0;
    const uint32_t h = r ? r->ctx.extent.height : 0;
    const float aspect = h != 0 ? (float)w / (float)h : 4.0f / 3.0f;
    return mdk::kCanvasHeight * aspect;
}

// The orthographic matrix mapping canvas pixels (x right, y DOWN) to clip
// space over the full viewport. The canvas is screen space, so it is written
// straight for the framebuffer: canvas y 0 is the top row, which is clip y -1
// with Vulkan's y-down framebuffer. That is the C#'s
// CreateOrthographicOffCenter(..., CanvasHeight + margin, -margin, ...) with
// its y arguments swapped, expressed in the engine's row-major row-vector
// layout (the translation lives in row 3, indices 12..14).
void canvasOrtho(float out[16], float width) {
    for (int i = 0; i < 16; i++) {
        out[i] = 0.0f;
    }
    out[0] = 2.0f / width;      // x -> [-1,1]
    out[5] = 2.0f / mdk::kCanvasHeight; // y (down) -> clip y down, the framebuffer's own sense
    out[10] = 1.0f;
    out[12] = -1.0f;
    out[13] = -1.0f;
    out[15] = 1.0f;
}

bool ensureCanvasBuffer(RendererVk* r, VkDeviceSize size) {
    const uint32_t slot = r->ctx.currentFrame;
    if (r->canvasBuffer[slot] && r->canvasCapacity >= size) {
        return true;
    }
    // The ring is resized as a unit, and the slot that is NOT this frame's may
    // belong to a submission that is still running, so nothing here is
    // destroyed: it is retired and freed at the frame boundary that proves the
    // GPU has finished with it (see MeshVk).
    for (uint32_t i = 0; i < mdk::vk::kFramesInFlight; i++) {
        retireBuffer(r, r->canvasBuffer[i], r->canvasMemory[i]);
        r->canvasBuffer[i] = VK_NULL_HANDLE;
        r->canvasMemory[i] = VK_NULL_HANDLE;
        r->canvasMapped[i] = nullptr;
    }
    r->canvasCapacity = 0;
    const VkDeviceSize cap = size < (64 * 1024) ? (64 * 1024) : size;
    for (uint32_t i = 0; i < mdk::vk::kFramesInFlight; i++) {
        if (!createHostBuffer(&r->ctx, cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              &r->canvasBuffer[i], &r->canvasMemory[i], &r->canvasMapped[i])) {
            for (uint32_t done = 0; done < i; done++) {
                destroyBufferPair(r->ctx.device, r->canvasBuffer[done], r->canvasMemory[done]);
                r->canvasBuffer[done] = VK_NULL_HANDLE;
                r->canvasMemory[done] = VK_NULL_HANDLE;
                r->canvasMapped[done] = nullptr;
            }
            refused("canvas ring of %llu bytes total, %llu per slot, x %u frames in "
                    "flight: the HUD and every other canvas quad from this frame is "
                    "dropped",
                    (unsigned long long)(cap * mdk::vk::kFramesInFlight),
                    (unsigned long long)cap, (unsigned)mdk::vk::kFramesInFlight);
            return false;
        }
    }
    r->canvasCapacity = cap;
    return true;
}

