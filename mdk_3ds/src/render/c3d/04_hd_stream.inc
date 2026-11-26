// 04_hd_stream.inc -- the HD stream: the reader thread's unpack, the budget, eviction, the boundary pump.
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

// ---------------------------------------------------------------------------
// THE HD STREAM (RendererC3D::hdCopies, docs/HD_TEXTURES_3DS_SPEC.md 4.2)
// ---------------------------------------------------------------------------

// THE HD FILE UNPACKED ON THE READER THREAD (platform/async_file_reader.h's
// Work, run on the worker after its read). The HD pack's .t3x files are LZ11
// (hd_pack_convert.py runs tex3ds with its default compression), and
// Tex3DS_TextureImport decompresses inside the import -- at the frame boundary,
// on the main thread: measured on the Old 3DS, 1-94 ms an import, up to 136 ms
// of a frame's begin span at level start. So the worker rewrites the file into
// the same .t3x with its texture data stored uncompressed (tex3ds' header and
// subtextures as they are, then a "none" compression header -- type 0, the
// 24-bit size -- and the raw blocks: the layout the orig pack's `-z none`
// files have), and the boundary's import is a copy. The worker runs below the
// main thread's priority, so the decompress fills the main thread's waits.
// A file it does not understand (no subtexture, a size past 4 MiB, a failed
// decompress) is handed back as it was read: the import then fails or
// decompresses exactly as before. Thread-safe: libctru's decompress and malloc
// only, nothing of the renderer's.
//
// AND IT CAN DROP THE TOP LEVELS (`cap`, the residency tier, main_3ds.cpp): a
// texture wider or taller than `cap` loses its level 0 -- and the next, while
// still over -- so the import is its own chain from level 1 down, exactly a
// quarter of the bytes per level dropped, with no re-encode: tex3ds stores the
// levels one after another from level 0 (C3D_TexCalcTotalSize's order), so the
// file is rewritten with its size fields (log2(w) - 3, log2(h) - 3) and level
// count one smaller, its subtextures' pixel sizes halved (their normalised
// rectangle is unchanged) and the first level's blocks skipped. Only ETC1 (4
// bits a texel) and ETC1A4 (8), the pack's formats; 8 texels is the floor.
static uint8_t* hdUnpack(const uint8_t* bytes, uint32_t size, uint32_t* outSize, int32_t cap) {
    size_t head = 0;
    uint32_t subtextures = 0;
    if (bytes && size >= 5) {
        subtextures = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8);
        head = subtextures >= 1 && subtextures <= 64 ? 5u + 12u * subtextures : 0u;
    }
    decompressType type = DECOMPRESS_DUMMY;
    size_t raw = 0;
    const ssize_t consumed =
        head > 0 && size > head + 4
            ? decompressHeader(&type, &raw, nullptr, (void*)(bytes + head), size - head)
            : -1;
    uint8_t* out = nullptr;
    if (consumed > 0 && raw > 0 && raw <= 4u * 1024u * 1024u) {
        out = (uint8_t*)malloc(head + 4 + raw);
        if (out) {
            memcpy(out, bytes, head);
            bool unpacked = false;
            if (type == DECOMPRESS_DUMMY) {
                // Already stored raw: copied as it is (the levels may still drop).
                if (head + (size_t)consumed + raw <= size) {
                    memcpy(out + head + 4, bytes + head + consumed, raw);
                    unpacked = true;
                }
            } else {
                unpacked = decompress(out + head + 4, raw, nullptr, (void*)(bytes + head),
                                      size - head);
            }
            if (!unpacked) {
                free(out);
                out = nullptr;
            }
        }
    }
    if (!out) {
        uint8_t* copy = (uint8_t*)malloc(size > 0 ? size : 1);
        if (!copy) {
            return nullptr;
        }
        if (size > 0) {
            memcpy(copy, bytes, size);
        }
        *outSize = size;
        return copy;
    }
    // The top levels over the cap.
    const uint32_t bits =
        out[3] == (uint8_t)GPU_ETC1 ? 4u : (out[3] == (uint8_t)GPU_ETC1A4 ? 8u : 0u);
    size_t skip = 0;
    while (bits > 0 && cap > 0 && out[4] >= 1 && (out[2] & 7) >= 1 && ((out[2] >> 3) & 7) >= 1) {
        const uint32_t w = 8u << (out[2] & 7);
        const uint32_t h = 8u << ((out[2] >> 3) & 7);
        if ((int32_t)w <= cap && (int32_t)h <= cap) {
            break;
        }
        const size_t level0 = (size_t)w * h * bits / 8u;
        if (skip + level0 >= raw) {
            break;
        }
        skip += level0;
        out[2] = (uint8_t)((out[2] & ~0x3Fu) | (((out[2] & 7) - 1) & 7) |
                           (((((out[2] >> 3) & 7) - 1) & 7) << 3));
        out[4] = (uint8_t)(out[4] - 1);
        for (uint32_t t = 0; t < subtextures; t++) {
            uint8_t* sub = out + 5 + 12 * t;
            const uint32_t sw = ((uint32_t)sub[0] | ((uint32_t)sub[1] << 8)) >> 1;
            const uint32_t sh = ((uint32_t)sub[2] | ((uint32_t)sub[3] << 8)) >> 1;
            sub[0] = (uint8_t)(sw & 0xFFu);
            sub[1] = (uint8_t)(sw >> 8);
            sub[2] = (uint8_t)(sh & 0xFFu);
            sub[3] = (uint8_t)(sh >> 8);
        }
    }
    if (skip > 0) {
        memmove(out + head + 4, out + head + 4 + skip, raw - skip);
        raw -= skip;
    }
    out[head] = 0x00;
    out[head + 1] = (uint8_t)(raw & 0xFFu);
    out[head + 2] = (uint8_t)((raw >> 8) & 0xFFu);
    out[head + 3] = (uint8_t)((raw >> 16) & 0xFFu);
    *outSize = (uint32_t)(head + 4 + raw);
    return out;
}

static uint8_t* hdUnpackWork(const uint8_t* bytes, uint32_t size, uint32_t* outSize) {
    return hdUnpack(bytes, size, outSize, 0);
}

// The residency tiers' unpacks (HdCopy::topCap): the chain from the
// level at or under 256, or 128, texels on its longer side.
static uint8_t* hdUnpackWork256(const uint8_t* bytes, uint32_t size, uint32_t* outSize) {
    return hdUnpack(bytes, size, outSize, 256);
}

static uint8_t* hdUnpackWork128(const uint8_t* bytes, uint32_t size, uint32_t* outSize) {
    return hdUnpack(bytes, size, outSize, 128);
}

// The HD budget NOW: kHdBudgetBytes, or less when the linear heap has less free
// than kHdLinearReserveBytes above what the copies already hold -- the copies
// grow into free memory and give way to everything else.
static size_t hdBudgetNow(const RendererC3D* renderer) {
    const size_t free = (size_t)linearSpaceFree();
    const size_t room = free > kHdLinearReserveBytes ? free - kHdLinearReserveBytes : 0u;
    const size_t budget = renderer->hdResidentBytes + room;
    return budget < kHdBudgetBytes ? budget : kHdBudgetBytes;
}

// Frees one HD copy's texture. Only where the GPU cannot be reading it (P3): the
// pump at the frame boundary (after C3D_FrameBegin's wait), or makeTexture's
// fallback for a copy older than this frame's first draw (this frame's commands
// are not submitted yet, and the last frame's finished at C3D_FrameBegin).
static void dropHd(RendererC3D* renderer, HdCopy& copy) {
    if (copy.state == HdCopy::Resident) {
        C3D_TexDelete(&copy.tex);
        renderer->hdResidentBytes -= copy.bytes;
    }
    copy.state = HdCopy::None;
    copy.bytes = 0;
    copy.levels = 0;
}

// The policy's view of the copies (pica::HdSlot), in a buffer reused across
// calls.
static void hdSlots(RendererC3D* renderer) {
    std::vector<mdk::pica::HdSlot>& slots = renderer->hdSlotScratch;
    slots.resize(renderer->hdCopies.size());
    for (size_t i = 0; i < renderer->hdCopies.size(); i++) {
        // A pinned copy (the level's resident set) is never a victim.
        slots[i].resident =
            renderer->hdCopies[i].state == HdCopy::Resident && !renderer->hdCopies[i].pinned;
        slots[i].lastUsed = renderer->hdCopies[i].lastUsed;
        slots[i].linked = renderer->hdCopies[i].links > 0;
    }
}

// Evicts the copy pica::hdEvictionVictim names (host-tested), or returns false.
static bool evictHd(RendererC3D* renderer, uint32_t protectFrom, bool unlinkedOnly) {
    hdSlots(renderer);
    const int32_t victim = mdk::pica::hdEvictionVictim(
        renderer->hdSlotScratch.data(), (int32_t)renderer->hdSlotScratch.size(), protectFrom,
        unlinkedOnly);
    if (victim < 0) {
        return false;
    }
    dropHd(renderer, renderer->hdCopies[(size_t)victim]);
    renderer->hdEvicted++;
    return true;
}

static bool hdHasEvictable(RendererC3D* renderer, uint32_t protectFrom) {
    hdSlots(renderer);
    return mdk::pica::hdEvictionVictim(renderer->hdSlotScratch.data(),
                                       (int32_t)renderer->hdSlotScratch.size(),
                                       protectFrom) >= 0;
}

// A read or import that failed: counted, logged, and retried after
// kHdRetryFrames up to kHdMaxFailures times (the draw re-queues it, F4).
static void hdFailed(RendererC3D* renderer, HdCopy& copy, const char* why) {
    copy.state = HdCopy::Failed;
    copy.failures++;
    copy.retryFrame = renderer->frame + kHdRetryFrames;
    renderer->hdFailedCount++;
    mdk::log3dsPrintf("hd: %s: %s (failure %d; %u resident, %u linear free)", why,
                      copy.path.c_str(), (int)copy.failures, (unsigned)renderer->hdResidentBytes,
                      (unsigned)linearSpaceFree());
}

// THE PUMP, at the frame boundary -- after C3D_FrameBegin has waited for the
// previous frame, so an eviction never frees what the GPU is reading (P3).
// Two halves: queued copies are handed to the reader thread (their SD reads
// happen there, off this thread); finished reads are imported, at most
// kHdImportsPerFrame per frame, within the HD budget. `protectFrom` is the first
// draw stamp of the frame that just ended: what it drew is not evicted.
static void pumpHdTextures(RendererC3D* renderer, uint32_t protectFrom) {
    // THE TOGGLE OFF: no reads, no imports, and the resident copies are freed
    // (this is a frame boundary, so none is in flight) -- off means the memory is
    // the original look's again (review F5). Turned back on, draws ask anew.
    //
    // AND THE READS GO TOO. A read handed out before the toggle went off would
    // finish and sit in its slot for ever (nothing polls kChannelHd while off),
    // and every slot it holds counts against kStreamSlots -- which shuts the orig
    // pack's dispatch out, so every packed original would be read by its draw.
    // So the copies being read are discarded (a queued read freed now, one in
    // progress when the worker ends it), and anything already finished is taken
    // and freed. Only while there is something to drop: off costs a frame two
    // slot counts, not a walk of the table.
    //
    // EXCEPT A STAND-IN. An HD-only texture (C3dTexture::hdOnly) has no original
    // made while the toggle is on, so when it goes off its resident copy is drawn
    // until the boundary pump has read the original (06_draw.inc): a copy the
    // frame that just ended drew is kept, and freed at the first boundary after
    // the draws stop taking it -- the original has landed by then.
    if (!renderer->toggles.enableHdTextures) {
        if (renderer->hdResidentBytes > 0 || !renderer->hdQueue.empty() ||
            renderer->hdReader.busy(mdk::AsyncFileReader::kChannelHd) > 0) {
            for (size_t i = 0; i < renderer->hdCopies.size(); i++) {
                HdCopy& copy = renderer->hdCopies[i];
                if (copy.state == HdCopy::Resident && copy.lastUsed >= protectFrom) {
                    continue;
                }
                if (copy.state == HdCopy::Reading) {
                    renderer->hdReader.discard(mdk::AsyncFileReader::kChannelHd, (int32_t)i);
                }
                dropHd(renderer, copy);
            }
            renderer->hdQueue.clear();
            int32_t tag = -1;
            uint8_t* bytes = nullptr;
            size_t size = 0;
            bool ok = false;
            char path[mdk::AsyncFileReader::kPathMax];
            while (renderer->hdReader.poll(mdk::AsyncFileReader::kChannelHd, &tag, path, &bytes,
                                           &size, &ok)) {
                free(bytes);
            }
        }
        return;
    }
    // THE SET COMES BACK WITH THE TOGGLE: a pinned copy the toggle-off freed is
    // asked for again (the frontend holds until it is resident). The HUD's
    // pictures are pinned with the set (mdk_render_create_hd_image) and Kurt has
    // no HD copy (kurt.pak), so no other linked copy needs asking again.
    if (renderer->hdPinnedMode) {
        for (size_t i = 0; i < renderer->hdCopies.size(); i++) {
            const HdCopy& copy = renderer->hdCopies[i];
            if (copy.state == HdCopy::None && copy.pinned) {
                requestHdCopy(renderer, (int32_t)i);
            }
        }
    }
    // AND THE ORIGINALS GO: the packed originals the HD-off stretch made for
    // HD-only textures are never drawn while the set is on, so each is given
    // back once its pinned copy is resident (measured without this: 1.9 MB of
    // linear left after a toggle back on, against 7.2 MB at level start). At a
    // frame boundary: C3D_FrameBegin has waited for the last frame (P3), and
    // from here every draw of the texture binds its HD copy instead.
    if (renderer->hdFreeOriginals && !renderer->gpuPending) {
        renderer->hdFreeOriginals = freeHdOnlyOriginals(renderer, true);
    }
    // (0) NOBODY'S COPIES. A copy no texture links to any more (a level ended, a
    // sprite frame was trimmed) is cancelled if it was only asked for (a read in
    // flight is dropped when it lands) and freed once it has stayed unlinked for
    // kHdUnlinkedFrames -- this is the frame boundary, and the frame that drew it
    // last has finished (C3D_FrameBegin waited). Under memory pressure it goes
    // first (evictHd's unlinked preference, makeTexture and the mesh allocator).
    for (size_t i = 0; i < renderer->hdCopies.size(); i++) {
        HdCopy& copy = renderer->hdCopies[i];
        if (copy.links > 0 || copy.pinned) {
            continue;
        }
        if (copy.state == HdCopy::Queued ||
            (copy.state == HdCopy::Resident &&
             renderer->frame - copy.unlinkedSince >= kHdUnlinkedFrames)) {
            dropHd(renderer, copy);
        }
    }
    // (1) Hand out reads while the reader has free slots -- and only while each
    // could be imported (pica::hdMayDispatch, host-tested). Otherwise the queue
    // waits (the view's working set fills the budget), and nothing is read from
    // SD only to be thrown away (F3). The evictable scan only runs when
    // something waits.
    //
    // A DEMANDED COPY GOES FIRST: one a draw of the frame that just ended wanted
    // (its stamp at or after `protectFrom`: 06_draw.inc stamps a copy it asks
    // for), else the queue's head, in the order the copies were asked for (the
    // creations, the pump's walk of the resident views, the warms). A HINT -- no
    // draw wanted it -- is read only while the budget has room for it without
    // evicting anything (hdMayDispatch with nothing evictable): an HD-only
    // texture's copy IS its surface, so a hint must never push out a copy the
    // player may still be looking at.
    {
        size_t kept = 0;
        for (size_t i = 0; i < renderer->hdQueue.size(); i++) {
            const int32_t id = renderer->hdQueue[i];
            if (id >= 0 && (size_t)id < renderer->hdCopies.size() &&
                renderer->hdCopies[(size_t)id].state == HdCopy::Queued) {
                renderer->hdQueue[kept++] = id;
            }
        }
        renderer->hdQueue.resize(kept);
    }
    const bool evictable = !renderer->hdQueue.empty() && hdHasEvictable(renderer, protectFrom);
    const size_t budget = renderer->hdQueue.empty() ? kHdBudgetBytes : hdBudgetNow(renderer);
    while (!renderer->hdQueue.empty()) {
        // One slot stays free for Kurt's read and the menus' (kStreamSlots); the
        // orig pack's reads, handed out first this boundary, count against it.
        if (renderer->hdReader.busy(mdk::AsyncFileReader::kChannelHd) +
                renderer->hdReader.busy(mdk::AsyncFileReader::kChannelOrig) >=
            mdk::AsyncFileReader::kStreamSlots) {
            break;
        }
        // THE LEVEL'S SET FIRST, and past the budget: its fit was proved by
        // arithmetic when it was chosen (main_3ds.cpp), so it is never gated.
        size_t pick = 0;
        bool demanded = false;
        bool pinned = false;
        // (With a set resident, a LINKED copy -- the HUD's pictures, Kurt's
        // non-pack frames, a few KB each -- is not gated either: the gate's
        // 699 KB bound against a heap the set has filled would starve them.)
        for (size_t i = 0; i < renderer->hdQueue.size(); i++) {
            const HdCopy& queued = renderer->hdCopies[(size_t)renderer->hdQueue[i]];
            if (queued.pinned || (renderer->hdPinnedMode && queued.links > 0)) {
                pick = i;
                pinned = true;
                break;
            }
        }
        for (size_t i = 0; !pinned && i < renderer->hdQueue.size(); i++) {
            if (renderer->hdCopies[(size_t)renderer->hdQueue[i]].lastUsed >= protectFrom) {
                pick = i;
                demanded = true;
                break;
            }
        }
        const size_t reading =
            (size_t)renderer->hdReader.busy(mdk::AsyncFileReader::kChannelHd);
        if (!pinned && !mdk::pica::hdMayDispatch(renderer->hdResidentBytes, reading,
                                                 kHdLargestBytes, budget,
                                                 demanded && evictable)) {
            break;
        }
        const int32_t id = renderer->hdQueue[pick];
        HdCopy& copy = renderer->hdCopies[(size_t)id];
        const mdk::AsyncFileReader::Work work =
            !copy.pinned || copy.topCap <= 0
                ? hdUnpackWork
                : (copy.topCap <= 128 ? hdUnpackWork128 : hdUnpackWork256);
        if (!renderer->hdReader.requestFile(mdk::AsyncFileReader::kChannelHd, id,
                                            copy.path.c_str(), work)) {
            break;   // every slot is busy: this one waits in the queue
        }
        copy.state = HdCopy::Reading;
        renderer->hdQueue.erase(renderer->hdQueue.begin() + (long)pick);
    }

    // (2) Import finished reads.
    int32_t imported = 0;
    int32_t id = -1;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    bool ok = false;
    char path[mdk::AsyncFileReader::kPathMax];
    while (imported < kHdImportsPerFrame &&
           renderer->hdReader.poll(mdk::AsyncFileReader::kChannelHd, &id, path, &bytes, &size,
                                   &ok)) {
        if (id < 0 || (size_t)id >= renderer->hdCopies.size()) {
            free(bytes);
            continue;
        }
        HdCopy& copy = renderer->hdCopies[(size_t)id];
        if (copy.state != HdCopy::Reading || copy.path != path ||
            (copy.links <= 0 && !copy.pinned)) {
            // Dropped (the toggle went off) or nobody's (unlinked) while it was
            // being read: not imported.
            if (copy.state == HdCopy::Reading) {
                copy.state = HdCopy::None;
            }
            free(bytes);
            continue;
        }
        if (!ok) {
            if (copy.pinned) {
                copy.failures = kHdMaxFailures;
                renderer->hdPinFailed++;
            }
            hdFailed(renderer, copy, "cannot read");
            continue;
        }
        // Reserve the decoded upper bound, not the file's compressed size:
        // Tex3DS allocates the expanded ETC chain inside the import, so
        // admitting against `size` can overshoot the budget before the exact
        // total is booked below. Dispatch already plans around this bound.
        // A HINT (no draw of the last frame wanted it) evicts nothing: with no
        // room it is dropped (None), and the next ask -- a draw's, or the pump's
        // re-ask -- asks for it again.
        // A pinned copy (the level's set) is imported whatever the budget says:
        // its room was proved when the set was chosen.
        const bool ungated = copy.pinned || (renderer->hdPinnedMode && copy.links > 0);
        const bool demanded = copy.lastUsed >= protectFrom || ungated;
        bool room = true;
        while (!ungated &&
               renderer->hdResidentBytes + kHdLargestBytes > hdBudgetNow(renderer)) {
            if (!demanded || !evictHd(renderer, protectFrom, false)) {
                // Nothing of the stream's own to give: a demanded copy takes the
                // room of the least recently used texture the last frame did not
                // draw (an original that stood in before the toggle went on is
                // the first such), as its import would (the ladder below).
                if (demanded && evictTextures(renderer, protectFrom)) {
                    continue;
                }
                room = false;
                break;
            }
        }
        if (!room) {
            // Everything resident was drawn last frame: the budget is the view's
            // working set. A demanded copy goes back to the queue; it is asked
            // for again later.
            free(bytes);
            if (demanded) {
                copy.state = HdCopy::Queued;
                renderer->hdQueue.push_back(id);
            } else {
                copy.state = HdCopy::None;
            }
            continue;
        }
        // THE IMPORT: a copy into the linear heap, the ETC blocks already tiled
        // on the host and already decompressed on the reader thread
        // (hdUnpackWork) -- at the frame boundary, after C3D_FrameBegin's wait
        // (P3), at most kHdImportsPerFrame a frame (the read and the decompress
        // ran on the reader thread, off the frame). The ORIG pack's textures do NOT come through
        // here: they have their own pump (pumpPackOriginals, below, run before
        // this one) and import through importCompressed, timed as texture-make
        // work. No sampler state
        // is set here: 06_draw.inc rewrites filter and maxLevel per draw, so a
        // full mip chain holds at NEAREST level 0 in the original look; only
        // the wrap, which the draw does not rewrite, is set below from the
        // image's own rectangle.
        // A FULL LINEAR HEAP is made room in by the import ladder's order
        // (importCompressed): a nobody's HD copy, then the least recently used
        // texture -- an HD-only texture's original, not drawn while the toggle
        // is on, is the first such -- then any HD copy, each only from what the
        // last frame did not draw. A hint takes free memory only.
        forgetTextureBinds(renderer);   // texel memory refilled (THE DEDUPE)
        Tex3DS_Texture t3x = Tex3DS_TextureImport(bytes, size, &copy.tex, nullptr, false);
        while (!t3x && demanded &&
               (evictHd(renderer, protectFrom, true) || evictTextures(renderer, protectFrom) ||
                evictHd(renderer, protectFrom, false))) {
            t3x = Tex3DS_TextureImport(bytes, size, &copy.tex, nullptr, false);
        }
        free(bytes);
        if (!t3x && !demanded) {
            copy.state = HdCopy::None;   // a hint with no free room: asked for again later
            continue;
        }
        if (!t3x) {
            if (copy.pinned) {
                // The set's arithmetic was wrong for this heap: given up for the
                // level, counted, and its surfaces take their packed originals.
                copy.failures = kHdMaxFailures;
                renderer->hdPinFailed++;
            }
            hdFailed(renderer, copy, "import FAILED");
            continue;
        }
        // The image's rectangle. hd_pack_convert.py makes one unrotated image per
        // file; a rotated one (top < bottom) would need its axes swapped in the
        // shader's uv transform, so it is refused rather than drawn sideways (F7).
        const Tex3DS_SubTexture* sub = Tex3DS_GetSubTexture(t3x, 0);
        if (!sub || Tex3DS_SubTextureRotated(sub)) {
            Tex3DS_TextureFree(t3x);
            C3D_TexDelete(&copy.tex);
            copy.failures = kHdMaxFailures;   // a property of the file: no retry
            copy.broken = true;
            hdFailed(renderer, copy, sub ? "rotated subtexture" : "no subtexture");
            continue;
        }
        copy.left = sub->left;
        copy.top = sub->top;
        copy.right = sub->right;
        copy.bottom = sub->bottom;
        Tex3DS_TextureFree(t3x);
        // The whole chain cleaned out of the CPU cache by range: Tex3DS does not
        // for a linear-heap texture (importCompressed, 04_textures.inc, says why).
        C3D_TexFlush(&copy.tex);
        copy.levels = copy.tex.maxLevel;
        copy.bytes = (uint32_t)C3D_TexCalcTotalSize(copy.tex.size, copy.tex.maxLevel);
        // REPEAT on an axis the image fills (a power-of-two image: exact), CLAMP
        // on one tex3ds padded (an image under 8 texels sits in an 8-texel
        // texture, and REPEAT would wrap through the padding) (F7).
        const bool fullU = copy.left == 0.0f && copy.right == 1.0f;
        const bool fullV = copy.top == 1.0f && copy.bottom == 0.0f;
        C3D_TexSetWrap(&copy.tex, fullU ? GPU_REPEAT : GPU_CLAMP_TO_EDGE,
                       fullV ? GPU_REPEAT : GPU_CLAMP_TO_EDGE);
        renderer->hdResidentBytes += copy.bytes;
        copy.state = HdCopy::Resident;
        // A demanded copy is fresh: not the next eviction's victim. A HINT keeps
        // the stamp its last draw left (0 if none), so the LRU takes it before
        // anything the player has seen since -- a copy read ahead of time never
        // outranks one on screen (pumpPackOriginals' rule).
        if (demanded) {
            copy.lastUsed = protectFrom;
        }
        renderer->hdImported++;
        imported++;
        if (renderer->hdImported <= 3 || (renderer->hdImported % 50) == 0) {
            mdk::log3dsPrintf("hd: imported %s %ux%u levels=%d %u bytes (uv %.2f,%.2f-%.2f,%.2f); "
                              "%d imported, %u bytes resident, %d evicted, %d failed",
                              copy.path.c_str(), (unsigned)copy.tex.width,
                              (unsigned)copy.tex.height, (int)copy.levels, (unsigned)copy.bytes,
                              copy.left, copy.top, copy.right, copy.bottom,
                              (int)renderer->hdImported, (unsigned)renderer->hdResidentBytes,
                              (int)renderer->hdEvicted, (int)renderer->hdFailedCount);
        }
    }
}

