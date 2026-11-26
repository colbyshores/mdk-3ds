// 04_streams.inc -- the orig pack's boundary pump, the loading screen's settle, the level's resident HD set.
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

// THE ORIG PACK'S PUMP, at the frame boundary, BEFORE the HD stream's
// (03_frame.inc) -- after C3D_FrameBegin has waited for the previous frame, so
// an import's evictions never free what the GPU is reading (P3), and an import
// only ever fills a texture with no GPU copy. Two halves, as pumpHdTextures:
//
// (1) QUEUED FILES TO THE READER, at most kPackReadsInFlight of them with it
//     and never past kStreamSlots of all its slots. A DEMANDED texture goes
//     first -- one a draw of the frame that just ended wanted (prepareDraw
//     stamps a pending texture), the largest of those, whose read saves the
//     most -- else the queue's head, in the order the core asked (the arena
//     pair, the drawn views, then the hints nearest first). A HINT (not
//     demanded) is only read while the linear pool has room for the largest
//     pack texture above kPackHintReserveBytes: otherwise it waits in the
//     queue, unread, until memory frees or a draw demands it -- so a pool the
//     resident set has filled costs no reads at all.
// (2) FINISHED READS IMPORTED, at most kPackImportsPerFrame. Each is checked
//     before it is used: a read whose texture was released, made by the draw's
//     last resort, or given its index pixels since it was asked for is thrown
//     away, never imported over a live copy. NO import evicts what the last
//     frame drew (`protectFrom` is its first stamp) -- not even a demanded one:
//     a texture still pending here was culled in both eyes last frame (a draw
//     in view would have made it), so it must not push out what is on screen.
//     A HINT evicts NOTHING: it is imported only into free memory above the
//     reserve, or not at all (idle again; the re-ask asks again, and the read
//     waits for room as above). Nor is it stamped newest: it keeps the stamp
//     its last draw left (0 if none), so the LRU takes it before anything the
//     player has seen since -- a texture read ahead of time never outranks one
//     on screen. A demanded import is stamped fresh, as any make is.
//
// `previousFrameStart` is the first draw stamp of the frame that just ended.
static void pumpPackOriginals(RendererC3D* renderer, uint32_t previousFrameStart) {
    // The entries whose texture is no longer waiting (made by a draw, released,
    // failed, or a second entry for one already handed out) are dropped here.
    std::vector<int32_t>& queue = renderer->packQueue;
    size_t kept = 0;
    for (size_t i = 0; i < queue.size(); i++) {
        const int32_t id = queue[i];
        if (id >= 0 && (size_t)id < renderer->textures.size() &&
            renderer->textures[(size_t)id].packState == C3dTexture::PackQueued) {
            queue[kept++] = id;
        }
    }
    queue.resize(kept);

    // (1) Hand out reads.
    while (!queue.empty() && renderer->hdReader.running() &&
           renderer->hdReader.busy(mdk::AsyncFileReader::kChannelOrig) < kPackReadsInFlight &&
           renderer->hdReader.busy(mdk::AsyncFileReader::kChannelHd) +
                   renderer->hdReader.busy(mdk::AsyncFileReader::kChannelOrig) <
               mdk::AsyncFileReader::kStreamSlots) {
        size_t pick = 0;
        bool demanded = false;
        int32_t pickTexels = 0;
        for (size_t i = 0; i < queue.size(); i++) {
            const C3dTexture& texture = renderer->textures[(size_t)queue[i]];
            if (texture.lastUsed < previousFrameStart) {
                continue;
            }
            const int32_t texels = texture.realWidth * texture.realHeight;
            if (!demanded || texels > pickTexels) {
                pick = i;
                pickTexels = texels;
                demanded = true;
            }
        }
        if (!demanded && linearSpaceFree() < kPackHintReserveBytes + kHdLargestBytes) {
            break;   // only hints wait, and there is no room for one: no read
        }
        const int32_t id = queue[pick];
        C3dTexture& texture = renderer->textures[(size_t)id];
        if (texture.packState == C3dTexture::PackQueued) {
            if (!renderer->hdReader.requestFile(mdk::AsyncFileReader::kChannelOrig, id,
                                                texture.packPath.c_str(), nullptr)) {
                break;   // every slot is busy: it waits in the queue
            }
            texture.packState = C3dTexture::PackReading;
        }
        queue.erase(queue.begin() + (long)pick);
    }

    // (2) Import finished reads.
    int32_t imported = 0;
    int32_t id = -1;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    bool ok = false;
    char path[mdk::AsyncFileReader::kPathMax];
    while (imported < kPackImportsPerFrame &&
           renderer->hdReader.poll(mdk::AsyncFileReader::kChannelOrig, &id, path, &bytes, &size,
                                   &ok)) {
        if (id < 0 || (size_t)id >= renderer->textures.size()) {
            free(bytes);
            continue;
        }
        C3dTexture& texture = renderer->textures[(size_t)id];
        if (texture.packState != C3dTexture::PackReading || texture.valid ||
            texture.releasePending || texture.packPath != path) {
            free(bytes);
            continue;
        }
        if (!ok) {
            free(bytes);
            markPackFailed(renderer, texture, "cannot read");
            continue;
        }
        const bool demanded = texture.lastUsed >= previousFrameStart;
        if (!demanded && linearSpaceFree() < kPackHintReserveBytes + size) {
            // A hint and no free room for it (the pool filled since its read was
            // handed out): idle, not imported, nothing evicted.
            free(bytes);
            texture.packState = C3dTexture::PackIdle;
            continue;
        }
        forgetTextureBinds(renderer);   // texel memory refilled (THE DEDUPE)
        bool refused = false;
        const bool made = importCompressed(renderer, texture, bytes, size, previousFrameStart,
                                           demanded, &refused);
        free(bytes);
        imported++;
        if (made) {
            texture.packState = C3dTexture::PackIdle;
            if (demanded) {
                texture.lastUsed = ++renderer->drawCounter;   // fresh: not the next victim
            }
            renderer->packImported++;
        } else if (refused) {
            markPackFailed(renderer, texture, "not an ETC .t3x of that size");
        } else {
            // No room (a demanded one: without evicting what is protected; a
            // hint: without evicting at all): idle, asked for again by the next
            // prepare or draw that wants it -- the pump itself never re-reads it.
            texture.packState = C3dTexture::PackIdle;
        }
    }
}

// THE LOADING SCREEN'S WAIT (render.h mdk_render_settle_streams): the two
// streams' pumps run between frames -- the GPU is idle at level load, and no
// frame is being recorded -- with this thread asleep between them so the
// reader thread, below it in priority, has the CPU: what would otherwise land
// over the first seconds of play, one boundary at a time while the frames keep
// the reader starved, lands on the loading screen. Everything waiting is a
// HINT here (no draw has stamped it), so the pumps' own rules hold: a read is
// handed out only while there is room for it, nothing is evicted, and a file
// that does not fit stays queued for play. It stops when both streams are
// idle, when nothing is being read and the last pass made nothing (the rest
// does not fit), or at `maxMs`.
int32_t mdk_render_settle_streams(MdkRenderer rr, int32_t maxMs) {
    RendererC3D* renderer = asC3d(rr);
    if (!renderer || !renderer->ready || renderer->inFrame || !renderer->hdReader.running() ||
        maxMs <= 0) {
        return 0;
    }
    const uint64_t start = svcGetSystemTick();
    const uint64_t limit = (uint64_t)maxMs * (uint64_t)(SYSCLOCK_ARM11 / 1000);
    const int32_t before = renderer->hdImported + renderer->packImported;
    for (;;) {
        const int32_t made = renderer->hdImported + renderer->packImported;
        const uint32_t stamp = renderer->drawCounter + 1;
        pumpPackOriginals(renderer, stamp);
        pumpHdTextures(renderer, stamp);
        const int32_t reading = renderer->hdReader.busy(mdk::AsyncFileReader::kChannelHd) +
                                renderer->hdReader.busy(mdk::AsyncFileReader::kChannelOrig);
        const bool waiting = !renderer->hdQueue.empty() || !renderer->packQueue.empty();
        if (reading == 0 && (!waiting || renderer->hdImported + renderer->packImported == made)) {
            break;
        }
        if (svcGetSystemTick() - start > limit) {
            break;
        }
        svcSleepThread(1000000LL);
    }
    return renderer->hdImported + renderer->packImported - before;
}

// THE HD WINDOW (render.h mdk_render_set_hd_window, docs/3DS_ARCHITECTURE.md 4):
// the files of the arenas around Kurt, by path, each a copy of the table (made
// for a file not yet seen), pinned and asked for; the copies that left the window
// are unpinned, and a resident one no texture links to is freed now (between
// frames: its last draw has finished). With `count` 0 the window is dropped and
// the stream is per-surface again (between levels).
//
// THE ORDER IS THE PRIORITY: the frontend passes the drawn arenas' files first.
// The pump hands out the queue's pinned copies in queue order, so the queue is
// rebuilt here with the window's copies in the order given, ahead of anything
// else waiting.
int32_t mdk_render_set_hd_window(MdkRenderer rr, const char* const* paths, const int32_t* caps,
                                 int32_t count) {
    RendererC3D* renderer = asC3d(rr);
    if (!renderer) {
        return 0;
    }
    for (size_t i = 0; i < renderer->hdCopies.size(); i++) {
        renderer->hdCopies[i].pinned = false;
    }
    renderer->hdPinnedMode = false;
    renderer->hdTopCap = 0;
    int32_t pinned = 0;
    std::vector<int32_t> order;
    for (int32_t p = 0; paths && p < count; p++) {
        if (!paths[p] || !paths[p][0] || strlen(paths[p]) >= mdk::AsyncFileReader::kPathMax) {
            continue;
        }
        int32_t index = -1;
        const std::map<std::string, int32_t>::const_iterator found =
            renderer->hdByPath.find(paths[p]);
        if (found != renderer->hdByPath.end()) {
            index = found->second;
        } else {
            index = (int32_t)renderer->hdCopies.size();
            HdCopy copy;
            copy.path = paths[p];
            renderer->hdCopies.push_back(copy);
            renderer->hdByPath[paths[p]] = index;
        }
        HdCopy& copy = renderer->hdCopies[(size_t)index];
        copy.pinned = true;
        const int32_t cap = caps ? caps[p] : 0;
        if (copy.topCap != cap && copy.state == HdCopy::Resident) {
            dropHd(renderer, copy);   // read again at this window's cap (between frames)
        }
        copy.topCap = cap;
        if (cap > 0 && (renderer->hdTopCap == 0 || cap < renderer->hdTopCap)) {
            renderer->hdTopCap = cap;
        }
        if (copy.state == HdCopy::Failed && !copy.broken) {
            copy.state = HdCopy::None;
            copy.failures = 0;
        }
        if (renderer->toggles.enableHdTextures) {
            requestHdCopy(renderer, index);
        }
        order.push_back(index);
        pinned++;
    }
    // THE CANVAS PICTURES MADE HD-ONLY (mdk_render_create_hd_image: the HUD's, the
    // pickups', the bomber's sight) are level-resident: pinned with every window,
    // asked for, and so held for on the loading screen and brought back by the
    // toggle -- never drawn from their indices while HD is on.
    if (pinned > 0) {
        for (size_t t = 0; t < renderer->textures.size(); t++) {
            const C3dTexture& texture = renderer->textures[t];
            if (!texture.hdOnly || !texture.hdCanvas || texture.releasePending ||
                texture.hdCopy < 0 || (size_t)texture.hdCopy >= renderer->hdCopies.size()) {
                continue;
            }
            HdCopy& copy = renderer->hdCopies[(size_t)texture.hdCopy];
            if (!copy.pinned) {
                copy.pinned = true;
                if (renderer->toggles.enableHdTextures) {
                    requestHdCopy(renderer, texture.hdCopy);
                }
                order.push_back(texture.hdCopy);
                pinned++;
            }
        }
    }
    // The queue: the window's waiting copies in the window's order, then the rest.
    {
        std::vector<int32_t> queue;
        for (size_t i = 0; i < order.size(); i++) {
            if (renderer->hdCopies[(size_t)order[i]].state == HdCopy::Queued) {
                queue.push_back(order[i]);
            }
        }
        for (size_t i = 0; i < renderer->hdQueue.size(); i++) {
            const int32_t id = renderer->hdQueue[i];
            if (id >= 0 && (size_t)id < renderer->hdCopies.size() &&
                !renderer->hdCopies[(size_t)id].pinned) {
                queue.push_back(id);
            }
        }
        renderer->hdQueue.swap(queue);
    }
    // THE COPIES THAT LEFT THE WINDOW GO NOW, not 600 frames on: a resident copy
    // no texture links to and no window holds is freed here -- between frames,
    // its last draw finished -- so the window's arithmetic (main_3ds.cpp) holds.
    for (size_t i = 0; i < renderer->hdCopies.size(); i++) {
        HdCopy& copy = renderer->hdCopies[i];
        if (!copy.pinned && copy.links <= 0 && copy.state == HdCopy::Resident &&
            !renderer->inFrame) {
            dropHd(renderer, copy);
        }
    }
    renderer->hdPinnedMode = pinned > 0;
    return pinned;
}

// Whether a pinned copy is still to come: not resident and not given up on.
static bool hdStillToCome(const HdCopy& copy) {
    return copy.state != HdCopy::Resident &&
           !(copy.state == HdCopy::Failed && (copy.broken || copy.failures >= kHdMaxFailures));
}

// The window's copies not resident yet and still to come (render.h): of `paths`,
// or of the whole window (and a linked copy asked for, the HUD's) with null; 0
// when the toggle is off or there is no window.
int32_t mdk_render_hd_pending(MdkRenderer rr, const char* const* paths, int32_t count) {
    RendererC3D* renderer = asC3d(rr);
    if (!renderer || !renderer->hdPinnedMode || !renderer->toggles.enableHdTextures) {
        return 0;
    }
    int32_t pending = 0;
    if (paths) {
        for (int32_t p = 0; p < count; p++) {
            const std::map<std::string, int32_t>::const_iterator found =
                paths[p] ? renderer->hdByPath.find(paths[p]) : renderer->hdByPath.end();
            if (found != renderer->hdByPath.end() &&
                hdStillToCome(renderer->hdCopies[(size_t)found->second])) {
                pending++;
            }
        }
        return pending;
    }
    for (size_t i = 0; i < renderer->hdCopies.size(); i++) {
        const HdCopy& copy = renderer->hdCopies[i];
        if (copy.pinned && hdStillToCome(copy)) {
            pending++;
        } else if (!copy.pinned && copy.links > 0 &&
                   (copy.state == HdCopy::Queued || copy.state == HdCopy::Reading)) {
            pending++;   // a linked copy asked for (the HUD's): held for too
        }
    }
    return pending;
}

