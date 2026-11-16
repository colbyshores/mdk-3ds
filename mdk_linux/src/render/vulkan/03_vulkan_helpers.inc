// 03_vulkan_helpers.inc -- memory, one-shot submissions, refusals, host images, capture.
// Part of renderer_vk.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

// ---------------------------------------------------------------------------
// Vulkan helpers
// ---------------------------------------------------------------------------

uint32_t findMemoryType(mdk::vk::Context* ctx, uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps = {};
    vkGetPhysicalDeviceMemoryProperties(ctx->physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

// THE ONE-SHOT COMMAND BUFFER AND ITS POOL TRAVEL TOGETHER.
//
// They used to be paired by POSITION in a file-scope vector: `beginOneShotTracked`
// pushed the pool and `endOneShot` destroyed the front entry, which was sound only
// while every creation ran begin-use-end with no refusal in between. A refusal
// between the push and the return stranded an entry, and from then on EVERY
// `endOneShot` freed a foreign command buffer into a pool it had not come from and
// destroyed that pool one step early — `vkFreeCommandBuffers` requires the pool the
// handle was allocated from (review-6 F2). Handing the pool to the caller makes the
// pairing impossible to lose: there is no ledger to desynchronize, nothing left to
// drain at teardown, and a refusal has exactly one owner for the pool it made.
//
// THREE OUTCOMES, NOT TWO, AND THE THIRD ONE OWNS SOMETHING.
//
// This used to return a bool, which could only say "the upload did not happen" — and
// a bool has to be one of two things, so the case where the queue ACCEPTED the work
// and the wait then failed was reported as the case where the queue refused it. The
// two demand opposite things of the caller. A submit the queue refused never touched
// the resources its commands name, so they are still purely the CPU's to release. A
// submit the queue accepted and a wait that could not prove finished may STILL BE
// RUNNING, and releasing the memory it reads or writes is the use-after-free
// AGENTS.md P3 forbids in its own words: "do not overwrite, resize, or free an
// in-flight resource ... put the completion boundary BEFORE CPU reuse". A failed
// wait is a completion boundary that was NOT established, which is not the same fact
// as a completion (review-8 F1).
//
// The three VkResults here were discarded once, and the caller could not tell an
// upload that ran from one the driver refused: `createHostImage` returned true for an
// image whose layout transition and buffer->image copy lived in a command stream that
// was never submitted, which is review-5 F1's "an image that exists but is not the
// texture" re-opened one level down (review-7 F1). AGENTS.md P3 asks for the same
// distinction from the other side: CPU ownership, cache visibility and GPU
// completion are three obligations, and `vkQueueWaitIdle` is the completion boundary
// this file has for a one-shot queue.
//
// They are sequenced, not merely checked. A command buffer whose `vkEndCommandBuffer`
// failed is not finalised, and submitting an unfinalised command buffer is invalid
// use, so a failed end must not reach the submit; and there is no point waiting for a
// submission the queue refused.
enum class OneShot {
    done,             // the wait proved it: everything it named is safe to release
    neverSubmitted,   // the queue refused it: safe to release, and the resource is refused
    unfenced          // accepted, completion unknown: NOT this side of the boundary
};

OneShot endOneShot(mdk::vk::Context* ctx, VkCommandPool pool, VkCommandBuffer cb) {
    const bool ended = vkEndCommandBuffer(cb) == VK_SUCCESS;
    if (!ended) {
        refused("end of a one-shot command buffer: the recording is not finalised, so "
                "nothing it holds is submitted and its caller must not believe it was");
    }
    bool submitted = false;
    if (ended) {
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb;
        submitted = vkQueueSubmit(ctx->queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
        if (!submitted) {
            refused("submit of a one-shot upload: nothing was copied and nothing was "
                    "transitioned, so the resource this upload was made for is refused");
        }
    }
    OneShot result = OneShot::neverSubmitted;
    if (submitted) {
        // The queue has it. The retry at device scope is here because a device-wide
        // wait proves the same thing about this queue; it waits more than this queue
        // does — a stall, which is affordable on the one path where the narrow wait has
        // already failed — and a queue wait can fail where a device wait does not. If
        // both say no, the submission's completion is UNKNOWN, and unknown is not
        // finished: the caller must not release what the commands name.
        if (vkQueueWaitIdle(ctx->queue) == VK_SUCCESS ||
            vkDeviceWaitIdle(ctx->device) == VK_SUCCESS) {
            result = OneShot::done;
        } else {
            refused("wait for a one-shot upload: the copy is not known to have finished, "
                    "so the memory it reads or writes is NOT safe to release and its "
                    "caller must abandon it rather than free it");
            result = OneShot::unfenced;
        }
    }
    // AN UNFENCED SUBMISSION OWNS ITS OWN COMMAND STREAM. The queue has the copy, the
    // wait could not prove it finished, and the instructions of that copy live in `cb`,
    // allocated from `pool`. `vkFreeCommandBuffers` requires that every submitted
    // command in the buffer has completed (VUID-vkFreeCommandBuffers-pCommandBuffers-
    // 00047) and `vkDestroyCommandPool` requires the same of every command buffer made
    // from the pool (VUID-vkDestroyCommandPool-commandPool-00041). Batch 152 stopped the
    // CALLERS from releasing what an unfenced submission named, and this tail went on
    // freeing the stream those callers had just abandoned (review-1 F3).
    //
    // So the pair is abandoned on the callers' terms for the callers' reason: the
    // handles stop being ours instead of being destroyed. Two driver objects per failed
    // wait, next to the image and the staging memory the caller already abandoned above,
    // on a driver that fails a wait; a TRANSIENT pool is expected to be reclaimed with
    // the device. The guard returns, and everything below it is the proven-finished
    // path — that ordering is what B10 pins.
    if (result == OneShot::unfenced) {
        refused("one-shot command buffer and its pool are abandoned, not freed: the "
                "submission that may still be running is made of them");
        return result;
    }
    // A command buffer whose end failed is still in the RECORDING state, and
    // `vkFreeCommandBuffers` is not the way out of it: the reset that would be the
    // documented recovery needs VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    // which this pool deliberately does not ask for. Destroying the pool reclaims
    // every command buffer allocated from it, recording or not, which is what frees
    // this one (review-8 F6).
    if (ended) {
        vkFreeCommandBuffers(ctx->device, pool, 1, &cb);
    }
    vkDestroyCommandPool(ctx->device, pool, nullptr);
    return result;
}

// Returns the command buffer and, through `outPool`, the pool it came from; the
// caller hands both to `endOneShot`. When it returns VK_NULL_HANDLE it has already
// destroyed everything it made, so the caller has nothing left to undo.
VkCommandBuffer beginOneShotTracked(mdk::vk::Context* ctx, VkCommandPool* outPool) {
    *outPool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    alloc.queueFamilyIndex = ctx->queueFamily;
    alloc.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(ctx->device, &alloc, nullptr, &pool) != VK_SUCCESS) {
        refused("one-shot command pool: no texture, image or screenshot upload can be "
                "recorded while the driver keeps saying no");
        return VK_NULL_HANDLE;
    }
    // From here the pool is this function's to undo until the caller has a command
    // buffer to take away with it.
    VkCommandBufferAllocateInfo cbAlloc = {};
    cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAlloc.commandPool = pool;
    cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(ctx->device, &cbAlloc, &cb) != VK_SUCCESS) {
        refused("one-shot command buffer: the upload that asked for it copies into nothing "
                "and its caller refuses the resource instead");
        vkDestroyCommandPool(ctx->device, pool, nullptr);
        return VK_NULL_HANDLE;
    }
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &begin) != VK_SUCCESS) {
        refused("begin of a one-shot command buffer: the upload that asked for it has "
                "nowhere to be recorded");
        vkFreeCommandBuffers(ctx->device, pool, 1, &cb);
        vkDestroyCommandPool(ctx->device, pool, nullptr);
        return VK_NULL_HANDLE;
    }
    *outPool = pool;
    return cb;
}

// ---------------------------------------------------------------------------
// REFUSALS ARE PRINTED, NOT SWALLOWED.
//
// A refused resource is a missing picture: the frame still renders, the geometry
// is simply not in it, and every gate stays green because a frame with less in it
// is still a frame. The Citro3D backend has always said so — `renderer_c3d.cpp`
// prints `linearAlloc(N) FAILED for a dynamic mesh of %d corners` — and the
// Vulkan backend answered the same failures with a bare `return -1`, which is the
// half review-11 F4 recorded against `object_draw.h` and no batch fixed: "the
// Vulkan backend's mesh refusal prints nothing either, so a refused mesh on Linux
// is still a silent missing picture".
//
// Every refusal site below prints the request the driver was GIVEN, not a guess at
// it, for the reason review-11 F1 gives for the frontend's copy of this line: a
// number derived from something else (the frame's corner count, the previous
// mesh's size) disagrees with the backend's own byte count and sends the next
// reader after a refusal that never happened. Where the driver is asked for the
// same thing more than once — a dynamic mesh and the canvas ring are one buffer
// PER FRAME IN FLIGHT — the line prints the total as well as the per-slot figure,
// because "98304 bytes x 2 frames in flight" reads as a 96 KB budget when the
// driver was actually asked for 192 KB (review-5 F3).
//
// ONE LINE PER DISTINCT REQUEST, NOT ONE PER REFUSAL (review-5 F2). The Citro3D
// backend latches with a bool that a successful allocation clears
// (`renderer_c3d.cpp`'s `meshAllocFailureLogged`), and it does that because the
// line otherwise printed 1710 times in one run (review-10 F3, pinned by
// `c3d_frame_test.sh`). A bool is the right shape there and the wrong one here:
// the Vulkan sites repeat per DRAW as well as per frame — a descriptor set is
// allocated for every textured draw — so a bool would report the first refusal of
// a run and then hide every larger one after it, which is the review-11 F7 defect
// the frontend's re-arming latch exists to avoid. So this helper remembers every
// request it has already named and stays silent only for one of those: a sustained
// refusal of the same request is one line however many frames it lasts, and a
// request that changes — a mesh that grows, a ring that resizes, a second palette
// — prints. Measured, not assumed: with every descriptor allocation forced to
// fail, level 3 attempts 48 distinct (texture, palette, sampler) triples and
// draws 187 of them per frame, so printing per refusal is 22500 lines in a 120
// frame run and printing per distinct request is 48 (evidence:
// docs/evidence/2026-11-12-149-refusal-discovery.txt).
// ---------------------------------------------------------------------------
struct RefusalLine {
    char text[256];
};
// One entry per request already named. Refusals are rare and the table is bounded,
// so a linear scan is the right data structure. Past the cap everything prints:
// an unreported refusal is the bug this helper exists to fix, and a repeated line
// is only noise.
std::vector<RefusalLine> g_refusalLines;
const size_t kRefusalMemory = 256;

void refused(const char* fmt, ...) {
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    // THE KEY IS THE RENDERED REQUEST, not the call site. A source-site field used
    // to be part of this key on the theory that "the address of `fmt` IS the site".
    // That is not true: identical literals are merged at -O1 and above, so two
    // distinct sites can share one address, and a field that cannot tell them apart
    // is worse than no field, because the comment claimed it could (review-6 F4).
    // What is true is weaker and it is enough: a request is a rendered sentence, two
    // sites that render the same sentence are reporting the same fact, and it is the
    // SET that stops the flood. A per-draw site needs a set rather than a latch: the
    // descriptor refusal names 48 different requests in a level, and remembering
    // only the last one printed would re-print all 48 on every frame after that.
    //
    // THE MEMORY LASTS FOR THE LIFE OF THE PROCESS, deliberately, and that is an
    // asymmetry with the frontends rather than an oversight. The frontend latch
    // re-arms on every frame that did not refuse, because it is told once per frame
    // whether the pass refused. This helper is never told about a success, so "print
    // again when it comes back" needs a per-request success signal the backend does
    // not have, and clearing the table on a frame boundary restores the 22500-lines-
    // in-one-run flood the set exists to prevent (review-5 F2, review-10 F3). A
    // request that recovers and then recurs byte-identically is therefore silent the
    // second time (review-6 F5); one that recurs DIFFERENTLY — a bigger canvas, a
    // different texture — is a different request and does print.
    for (size_t i = 0; i < g_refusalLines.size(); i++) {
        if (std::strcmp(g_refusalLines[i].text, text) == 0) {
            return;  // this exact request was named already
        }
    }
    if (g_refusalLines.size() < kRefusalMemory) {
        RefusalLine line;
        std::snprintf(line.text, sizeof(line.text), "%s", text);
        g_refusalLines.push_back(line);
    }
    std::printf("[vk] REFUSED %s\n", text);
    std::fflush(stdout);
}

// Create a host-visible buffer and map it persistently.
bool createHostBuffer(mdk::vk::Context* ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                      VkBuffer* outBuffer, VkDeviceMemory* outMemory, void** outMapped) {
    VkBufferCreateInfo bufInfo = {};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = size;
    bufInfo.usage = usage;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(ctx->device, &bufInfo, nullptr, outBuffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req = {};
    vkGetBufferMemoryRequirements(ctx->device, *outBuffer, &req);
    const uint32_t type = findMemoryType(ctx, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == 0xFFFFFFFFu) {
        vkDestroyBuffer(ctx->device, *outBuffer, nullptr);
        *outBuffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(ctx->device, &alloc, nullptr, outMemory) != VK_SUCCESS) {
        vkDestroyBuffer(ctx->device, *outBuffer, nullptr);
        *outBuffer = VK_NULL_HANDLE;
        return false;
    }
    // A discarded VkResult here left a buffer bound to nothing, mapped, and handed
    // back as a success: every memcpy into it landed in the CPU's copy of an
    // unbound allocation and the frame drew whatever the buffer actually held.
    if (vkBindBufferMemory(ctx->device, *outBuffer, *outMemory, 0) != VK_SUCCESS) {
        refused("bind of a host buffer of %llu bytes: the buffer and its memory are "
                "destroyed, so the caller refuses the resource that asked for them",
                (unsigned long long)size);
        vkDestroyBuffer(ctx->device, *outBuffer, nullptr);
        *outBuffer = VK_NULL_HANDLE;
        vkFreeMemory(ctx->device, *outMemory, nullptr);
        *outMemory = VK_NULL_HANDLE;
        return false;
    }
    if (outMapped) {
        if (vkMapMemory(ctx->device, *outMemory, 0, VK_WHOLE_SIZE, 0, outMapped) != VK_SUCCESS) {
            // Destroy the object BEFORE releasing the memory bound to it. Freeing
            // first is invalid use, and the same order now holds in every teardown
            // block in this file (review-6 F6; `linux_refusal_report_test.sh` B9).
            vkDestroyBuffer(ctx->device, *outBuffer, nullptr);
            *outBuffer = VK_NULL_HANDLE;
            vkFreeMemory(ctx->device, *outMemory, nullptr);
            *outMemory = VK_NULL_HANDLE;
            *outMapped = nullptr;
            return false;
        }
    }
    return true;
}

// Create a host-visible 2D image, write `bytes` into it, and transition it
// to SHADER_READ_ONLY. `bytesPerTexel` is 1 (R8) or 4 (RGBA8).
bool createHostImage(mdk::vk::Context* ctx, int32_t width, int32_t height, VkFormat format,
                     uint32_t bytesPerTexel, const uint8_t* pixels, VkImage* outImage,
                     VkDeviceMemory* outMemory, VkImageView* outView, void** outMapped) {
    const VkDeviceSize size = (VkDeviceSize)width * (VkDeviceSize)height * bytesPerTexel;

    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = format;
    imgInfo.extent.width = (uint32_t)width;
    imgInfo.extent.height = (uint32_t)height;
    imgInfo.extent.depth = 1;
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx->device, &imgInfo, nullptr, outImage) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req = {};
    vkGetImageMemoryRequirements(ctx->device, *outImage, &req);
    const uint32_t type = findMemoryType(ctx, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == 0xFFFFFFFFu) {
        vkDestroyImage(ctx->device, *outImage, nullptr);
        *outImage = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(ctx->device, &alloc, nullptr, outMemory) != VK_SUCCESS) {
        vkDestroyImage(ctx->device, *outImage, nullptr);
        *outImage = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindImageMemory(ctx->device, *outImage, *outMemory, 0) != VK_SUCCESS) {
        refused("bind of a %dx%d image of %llu bytes: the image and its memory are "
                "destroyed, so the caller gets no texture rather than an unbound one",
                width, height, (unsigned long long)size);
        vkDestroyImage(ctx->device, *outImage, nullptr);
        *outImage = VK_NULL_HANDLE;
        vkFreeMemory(ctx->device, *outMemory, nullptr);
        *outMemory = VK_NULL_HANDLE;
        return false;
    }
    *outMapped = nullptr;

    // The pixels go through a staging buffer and a buffer->image copy.
    // Copying straight into a mapped OPTIMAL image is legal API but the
    // layout is implementation-defined -- a linear memcpy lands as garbage
    // on at least one driver (llvmpipe), and validation says nothing.
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    void* stagingMapped = nullptr;
    if (pixels &&
        !createHostBuffer(ctx, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging,
                          &stagingMemory, &stagingMapped)) {
        vkDestroyImage(ctx->device, *outImage, nullptr);
        *outImage = VK_NULL_HANDLE;
        vkFreeMemory(ctx->device, *outMemory, nullptr);
        *outMemory = VK_NULL_HANDLE;
        return false;
    }
    if (pixels) {
        memcpy(stagingMapped, pixels, (size_t)size);
        vkUnmapMemory(ctx->device, stagingMemory);
    }

    VkCommandPool oneShotPool = VK_NULL_HANDLE;
    VkCommandBuffer cb = beginOneShotTracked(ctx, &oneShotPool);
    OneShot upload = OneShot::neverSubmitted;
    if (cb) {
        VkImageMemoryBarrier toTransfer = {};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.srcAccessMask = 0;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = *outImage;
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.levelCount = 1;
        toTransfer.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &toTransfer);

        if (pixels) {
            VkBufferImageCopy region = {};
            region.bufferOffset = 0;
            region.bufferRowLength = 0;   // tightly packed
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = {0, 0, 0};
            region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
            vkCmdCopyBufferToImage(cb, staging, *outImage,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }

        VkImageMemoryBarrier toSample = toTransfer;
        toSample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toSample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toSample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toSample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toSample);
        upload = endOneShot(ctx, oneShotPool, cb);
    }
    if (upload == OneShot::unfenced) {
        // NOTHING IN THIS BLOCK MAY RELEASE ANYTHING. The queue accepted the copy and
        // the wait could not prove it finished, so it may still be reading the staging
        // buffer and writing this image. Freeing either here is exactly what the
        // refusal that has already printed said was not safe, and it is the
        // use-after-free AGENTS.md P3 puts a completion boundary in front of
        // (review-8 F1: the bool this function used to get back could only say "it did
        // not happen", so this case was reported as the refused-submit case and fell
        // straight into the destroy below).
        //
        // So ownership is abandoned, out loud. The handles are nulled because this
        // function is no longer their owner, NOT because they were destroyed, and the
        // caller restores the image it already had and never sees them. The leak is
        // bounded by the number of failed waits — zero on every driver measured here,
        // and a driver that fails a wait is a driver on its way to a lost device.
        // The counter this line does not carry is deliberate: the refusal set keys on
        // the rendered sentence, so a counter would make every leak a new sentence and
        // re-open the flood B7 exists to stop.
        refused("%dx%d image of %llu bytes and its staging buffer are abandoned, not "
                "destroyed: a submission that may still be running wrote into them",
                width, height, (unsigned long long)size);
        *outImage = VK_NULL_HANDLE;
        *outMemory = VK_NULL_HANDLE;
        *outView = VK_NULL_HANDLE;
        return false;
    }
    if (upload != OneShot::done) {
        // THE IMAGE IS NOT THE TEXTURE. It exists, but nothing was ever copied
        // into it and its layout was never set, so a `return true` here handed
        // the caller a texture that samples as noise and a log with no failure
        // in it (review-5 F1). Destroy it; the caller's own line names the
        // texture this one was meant to become.
        //
        // There are three ways to arrive here and they used to be one: no command
        // buffer at all, a command buffer whose submit the driver refused, and (until
        // review-8 F1) a submission whose completion was never proven. Whoever refused
        // it printed the cause; this line names which image is being destroyed instead
        // of returned, which is the half the caller needs and the half nobody else can
        // say. The unfenced case above is the one that may NOT be destroyed.
        refused("%dx%d image of %llu bytes is destroyed, not returned: its fill and "
                "layout transition were never executed",
                width, height, (unsigned long long)size);
        if (staging) {
            vkDestroyBuffer(ctx->device, staging, nullptr);
        }
        if (stagingMemory) {
            vkFreeMemory(ctx->device, stagingMemory, nullptr);
        }
        vkDestroyImage(ctx->device, *outImage, nullptr);
        *outImage = VK_NULL_HANDLE;
        vkFreeMemory(ctx->device, *outMemory, nullptr);
        *outMemory = VK_NULL_HANDLE;
        return false;
    }
    if (staging) {
        vkDestroyBuffer(ctx->device, staging, nullptr);
    }
    if (stagingMemory) {
        vkFreeMemory(ctx->device, stagingMemory, nullptr);
    }

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = *outImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(ctx->device, &viewInfo, nullptr, outView) != VK_SUCCESS) {
        // The image and its memory were this function's to keep until it handed
        // them over. Returning false with both still bound and still non-null
        // leaked them and left the caller holding a handle it had been told not
        // to use (review-6 open question 3, CONTINUATION item 1).
        refused("view of a %dx%d image: the image and its memory are destroyed, so the "
                "caller creates no texture rather than one nothing can sample",
                width, height);
        vkDestroyImage(ctx->device, *outImage, nullptr);
        *outImage = VK_NULL_HANDLE;
        vkFreeMemory(ctx->device, *outMemory, nullptr);
        *outMemory = VK_NULL_HANDLE;
        *outView = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

// Write an uncompressed, top-down 32-bit BMP. The pixels are BGRA, the
// same layout the C# oracle's Bmp.Write emits. Keeping the two files'
// containers identical makes the frame comparator work on the pixels, not
// on two subtly different image encoders.
//
// THE PICTURE IS PUBLISHED BY RENAME, NOT BY WRITING IN PLACE. A half-written
// BMP at the requested path is a file that exists, is non-empty, and is not a
// frame -- and "the file exists and is non-empty" is exactly the test a capture
// harness reaches for. So the bytes go to a sibling `<path>.part` and only a
// complete write is renamed onto the destination: a short write, a full disk or
// a failed close leaves the destination exactly as it found it and removes the
// partial file it made, which is a file this function created and therefore
// always safe to unlink. The frontend separately clears any previous picture at
// the destination (`clearCaptureDestination` in main.cpp) so a capture that
// never got here cannot leave the last run's frame either.
bool writeBmp(const char* path, uint32_t width, uint32_t height, const uint8_t* bgra) {
    if (!path || !bgra || width == 0 || height == 0) {
        return false;
    }
    const uint64_t pixelBytes = (uint64_t)width * (uint64_t)height * 4u;
    const uint64_t fileBytes = 54u + pixelBytes;
    if (fileBytes > UINT32_MAX) {
        return false;
    }

    const std::string temp = std::string(path) + ".part";
    FILE* file = std::fopen(temp.c_str(), "wb");
    if (!file) {
        std::printf("[vk] screenshot: could not open %s\n", path);
        return false;
    }

    uint8_t header[54] = {};
    header[0] = 'B';
    header[1] = 'M';
    const uint32_t fileSize = (uint32_t)fileBytes;
    header[2] = (uint8_t)(fileSize & 0xFFu);
    header[3] = (uint8_t)((fileSize >> 8) & 0xFFu);
    header[4] = (uint8_t)((fileSize >> 16) & 0xFFu);
    header[5] = (uint8_t)((fileSize >> 24) & 0xFFu);
    const uint32_t pixelOffset = 54u;
    header[10] = (uint8_t)(pixelOffset & 0xFFu);
    header[11] = (uint8_t)((pixelOffset >> 8) & 0xFFu);
    header[14] = 40u;
    header[18] = (uint8_t)(width & 0xFFu);
    header[19] = (uint8_t)((width >> 8) & 0xFFu);
    header[20] = (uint8_t)((width >> 16) & 0xFFu);
    header[21] = (uint8_t)((width >> 24) & 0xFFu);
    // A negative height stores the rows top-down.
    const uint32_t signedHeight = (uint32_t)-(int32_t)height;
    header[22] = (uint8_t)(signedHeight & 0xFFu);
    header[23] = (uint8_t)((signedHeight >> 8) & 0xFFu);
    header[24] = (uint8_t)((signedHeight >> 16) & 0xFFu);
    header[25] = (uint8_t)((signedHeight >> 24) & 0xFFu);
    header[26] = 1u;
    header[28] = 32u;

    bool ok = std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
    if (ok) {
        ok = std::fwrite(bgra, 1, (size_t)pixelBytes, file) == (size_t)pixelBytes;
    }
    if (std::fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        // The partial file is this function's own; the destination was never
        // opened, so whatever was there is still there and still complete.
        std::remove(temp.c_str());
        std::printf("[vk] screenshot: could not write %s\n", path);
        return false;
    }
    if (std::rename(temp.c_str(), path) != 0) {
        std::remove(temp.c_str());
        std::printf("[vk] screenshot: could not place the picture at %s (%s)\n", path,
                    std::strerror(errno));
        return false;
    }
    return true;
}

// Copy the swapchain image just presented into a host buffer and write it as
// a BMP. The copy is queued after the frame's present, so it is ordered after
// all of that frame's draws; its fence waits for the readback before the
// pixels are inspected.
bool captureSwapchain(RendererVk* r, const char* path) {
    if (!r || !path || r->ctx.extent.width == 0 || r->ctx.extent.height == 0) {
        return false;
    }
    const uint32_t width = r->ctx.extent.width;
    const uint32_t height = r->ctx.extent.height;
    const VkDeviceSize size = (VkDeviceSize)width * (VkDeviceSize)height * 4u;

    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    if (!createHostBuffer(&r->ctx, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          &buffer, &memory, &mapped)) {
        std::printf("[vk] screenshot: host buffer failed\n");
        return false;
    }

    const VkImage image = r->ctx.images[r->ctx.currentImage];
    VkCommandPool oneShotPool = VK_NULL_HANDLE;
    VkCommandBuffer cb = beginOneShotTracked(&r->ctx, &oneShotPool);
    if (!cb) {
        vkUnmapMemory(r->ctx.device, memory);
        vkDestroyBuffer(r->ctx.device, buffer, nullptr);
        vkFreeMemory(r->ctx.device, memory, nullptr);
        std::printf("[vk] screenshot: command buffer failed\n");
        return false;
    }

    VkImageMemoryBarrier toTransfer = {};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &toTransfer);

    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buffer, 1, &region);

    VkImageMemoryBarrier toPresent = toTransfer;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &toPresent);
    const OneShot shot = endOneShot(&r->ctx, oneShotPool, cb);
    if (shot == OneShot::unfenced) {
        // The copy may still be writing this buffer. Unmapping, destroying or freeing
        // it here is the use-after-free the refusal that has already printed said was
        // not safe (review-8 F1), so ownership is abandoned rather than released: a
        // screenshot is a debug artefact and a corrupted heap is not.
        refused("screenshot buffer of %llu bytes is abandoned, not freed: a submission "
                "that may still be running writes into it",
                (unsigned long long)size);
        return false;
    }
    if (shot != OneShot::done) {
        // `endOneShot` already printed why. A buffer nobody copied into is not a
        // screenshot: writing it out would publish a BMP of whatever the allocator
        // left in the mapping and print "wrote" over the top of it.
        vkUnmapMemory(r->ctx.device, memory);
        vkDestroyBuffer(r->ctx.device, buffer, nullptr);
        vkFreeMemory(r->ctx.device, memory, nullptr);
        return false;
    }

    const uint8_t* raw = static_cast<const uint8_t*>(mapped);
    std::vector<uint8_t> bgra((size_t)size);
    const bool rawIsBgra = r->ctx.swapchainFormat == VK_FORMAT_B8G8R8A8_UNORM ||
                           r->ctx.swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0; i < (size_t)size; i += 4) {
        if (rawIsBgra) {
            bgra[i] = raw[i];
            bgra[i + 1] = raw[i + 1];
            bgra[i + 2] = raw[i + 2];
        } else {
            bgra[i] = raw[i + 2];
            bgra[i + 1] = raw[i + 1];
            bgra[i + 2] = raw[i];
        }
        bgra[i + 3] = raw[i + 3];
    }

    const bool ok = writeBmp(path, width, height, bgra.data());
    vkUnmapMemory(r->ctx.device, memory);
    vkDestroyBuffer(r->ctx.device, buffer, nullptr);
    vkFreeMemory(r->ctx.device, memory, nullptr);
    if (ok) {
        std::printf("[vk] screenshot: wrote %s (%ux%u)\n", path, width, height);
    }
    return ok;
}

void destroyImage(mdk::vk::Context* ctx, VkImage* image, VkDeviceMemory* memory,
                  VkImageView* view, void** mapped) {
    if (*view) { vkDestroyImageView(ctx->device, *view, nullptr); *view = VK_NULL_HANDLE; }
    if (*image) { vkDestroyImage(ctx->device, *image, nullptr); *image = VK_NULL_HANDLE; }
    if (*memory) { vkFreeMemory(ctx->device, *memory, nullptr); *memory = VK_NULL_HANDLE; }
    (void)mapped;  // unmapped implicitly when the memory is freed
}

