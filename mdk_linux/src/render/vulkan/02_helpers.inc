// 02_helpers.inc -- the refusal table's declaration, retirement, asVk.
// Part of renderer_vk.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

// they come later in the file.
void flushDraws(RendererVk* r);
// A queue's buckets in the pass order (12_draw.inc).
void drawBuckets(RendererVk* r, const mdk::DrawQueue& queue, bool overlay);

RendererVk* asVk(MdkRenderer r) {
    return static_cast<RendererVk*>(r);
}

// Defined below, with the rest of the refusal reporting. Declared here because
// `settleRetired` and the one-shot helpers are the first sites in the file that
// can be refused.
void refused(const char* fmt, ...);

// Stop using a buffer without destroying it yet.
//
// The caller has finished with it on the CPU, but a submission that reads it may
// still be running: a mesh released in frame N was drawn by frame N or earlier,
// and nothing has waited for that work. The deadline is two frames out, and the
// wait that makes it real is the fence beginFrame performs on the slot it is
// about to record. With two frames in flight the slot frame N used is next
// recorded at frame N+2, and the fence waited there is frame N's submission --
// so destroying at N+2 is after the GPU's last read. A buffer retired between
// frames (frameIndex has not advanced yet) is only more conservative.
void retireBuffer(RendererVk* r, VkBuffer buffer, VkDeviceMemory memory) {
    if (!buffer && !memory) {
        return;
    }
    RetiredBuffer retired;
    retired.buffer = buffer;
    retired.memory = memory;
    retired.safeFrame = r->frameIndex + mdk::vk::kFramesInFlight;
    r->retiring.push_back(retired);
}

// Destroy the retired buffers whose reading frame has finished. Called at the
// frame boundary AFTER beginFrame's fence wait, which is the GPU-completion
// boundary (AGENTS.md P3): a fence, not a lock and not a cache flush.
void settleRetired(RendererVk* r) {
    for (size_t i = 0; i < r->retiring.size();) {
        const RetiredBuffer retired = r->retiring[i];
        if (retired.safeFrame > r->frameIndex) {
            i++;
            continue;
        }
        destroyBufferPair(r->ctx.device, retired.buffer, retired.memory);
        r->retiring.erase(r->retiring.begin() + (long)i);
    }
    // The sets the cache forgot. A descriptor set is a slot in a pool with a
    // fixed `maxSets`, and nothing in this backend has ever given one back: no
    // `vkFreeDescriptorSets`, no `vkResetDescriptorPool`. So every texture or
    // palette released and re-made — and `mdk_render_update_texture` does that
    // on every same-size update — spent a set for the rest of the process, and
    // when the pool ran out every draw that needed a new pair was skipped.
    //
    // THE SET waits for the boundary the buffers wait for, for the set's own
    // reason: `vkFreeDescriptorSets` requires the set not be in use, and a frame
    // that recorded `vkCmdBindDescriptorSets` with it may still be on the queue.
    // Only the SET waits here for the SET's own reason. The image view it names
    // used to be destroyed at the drop site with no deadline at all; it now goes
    // through `retireImage` and settles in the loop below this one, on the same
    // boundary, so the pair is a completion boundary and not half of one.
    for (size_t i = 0; i < r->retiringSets.size();) {
        const RetiredSet retired = r->retiringSets[i];
        if (retired.safeFrame > r->frameIndex) {
            i++;
            continue;
        }
        VkDescriptorSet set = retired.set;
        const VkResult freed = vkFreeDescriptorSets(r->ctx.device, r->descPool, 1, &set);
        if (freed != VK_SUCCESS) {
            // The handle is dropped rather than retried, and two rules decide that.
            // They are stated as rules and not quoted (review-10 F3: an earlier draft
            // of this comment put one of them in quotation marks, and the vendored
            // header carries no spec prose to check a quotation against, so a quote
            // here is unfalsifiable and belongs in the spec, not here). First: the
            // out-of-memory result codes this call can return do not say whether the
            // sets were taken when one of them comes back. Second: a set handed to it
            // must be a valid handle this pool issued and not one already returned.
            // So a retry can be a second free of a handle the driver already took.
            // Losing one set is the smaller failure — but it is COUNTED, because the
            // refusal below prints once and a pool quietly holding N fewer sets than
            // its reader believes is the same silent-budget bug this batch closed.
            r->lostDescriptorSets++;
            refused("return a descriptor set to the pool: the set is lost and the "
                    "pool has one fewer to hand out; when it empties, every draw "
                    "that needs a new texture/palette pair is skipped");
        }
        r->retiringSets.erase(r->retiringSets.begin() + (long)i);
    }
    // The images those sets name. A descriptor set holds a VIEW, so retiring the
    // handle and destroying the resource in the same call defers half of a
    // lifetime: a frame still on the queue can be sampling a view that no longer
    // exists, which is AGENTS.md P3's "do not free an in-flight resource" read on
    // the resource rather than the handle (review-9 F2). Same boundary, same
    // reason, and the destroy order is `destroyImage`'s — view, then image, then
    // memory — because freeing memory bound to a live image is invalid use.
    for (size_t i = 0; i < r->retiringImages.size();) {
        const RetiredImage retired = r->retiringImages[i];
        if (retired.safeFrame > r->frameIndex) {
            i++;
            continue;
        }
        VkImage image = retired.image;
        VkDeviceMemory memory = retired.memory;
        VkImageView view = retired.view;
        destroyImage(&r->ctx, &image, &memory, &view, nullptr);
        r->retiringImages.erase(r->retiringImages.begin() + (long)i);
    }
}

// Stop using a descriptor set without returning it to the pool yet.
//
// The cache drops a set because the image view it names is about to be destroyed
// or replaced, so the entry must go NOW — but the set itself is named by whatever
// this frame has already recorded, and `vkFreeDescriptorSets` on a set an
// in-flight command buffer binds is invalid use. So the SET waits for the
// boundary `settleRetired` implements, which is the fence `vk::beginFrame` just
// waited (AGENTS.md P3), not a lock and not a cache flush.
//
// THE VIEW WAITS TOO. A retired set is a handle, and the handle is only half of
// what a queued frame touches: the other half is the image view the set names.
// `retireImage` below defers that pair to the same boundary, because deferring
// the handle and destroying the resource on the spot is the half-fix review-9 F2
// named — AGENTS.md P3 puts the boundary on the resource, not on the reference.
void retireDescriptorSet(RendererVk* r, VkDescriptorSet set) {
    if (!set) {
        return;
    }
    RetiredSet retired;
    retired.set = set;
    retired.safeFrame = r->frameIndex + mdk::vk::kFramesInFlight;
    r->retiringSets.push_back(retired);
}

// Stop using an image without destroying it yet, on the boundary
// `retireDescriptorSet` uses. Every caller of `forgetDescriptorSets` was
// destroying the image, its view and its memory in the same call that retired
// the set naming them, so the set waited two frames and the resource waited
// none. The destroy itself stays `destroyImage`'s, at the settle, so the view /
// image / memory order is one place and not two.
void retireImage(RendererVk* r, VkImage image, VkDeviceMemory memory, VkImageView view) {
    if (!image && !memory && !view) {
        return;
    }
    RetiredImage retired;
    retired.image = image;
    retired.memory = memory;
    retired.view = view;
    retired.safeFrame = r->frameIndex + mdk::vk::kFramesInFlight;
    r->retiringImages.push_back(retired);
}

