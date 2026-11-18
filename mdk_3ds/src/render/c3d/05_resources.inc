// 05_resources.inc -- the resource seam: textures, palettes, meshes, scopes.
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

MdkTextureId mdk_render_create_index_texture(MdkRenderer r, int32_t w, int32_t h, const uint8_t* px,
                                              const uint8_t* paletteRgba) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !renderer->ready || w <= 0 || h <= 0 || w > 1024 || h > 1024 || !px) {
        return -1;
    }
    if (!paletteRgba) {
        // Without a palette there is nothing to expand through. The Vulkan
        // backend binds the palette as a texture of its own and does not
        // need it here, but the 3DS does.
        return -1;
    }

    // THE EXPANSION. The palette lookup happens on the CPU because the
    // PICA200 has no palette texture format and no programmable fragment
    // shader (SPEC.md 7.3).
    //
    // The rule is the C#'s palette shader's rule exactly: index 0 is
    // transparent, and the palette's alpha is NOT used -- the shader takes
    // the palette's RGB and writes an alpha of 1.
    // THE DIMENSIONS GO UP TO A POWER OF TWO, and to at least 8.
    //
    // The PICA200's texture size register encodes log2(width) and
    // log2(height), so a texture's dimensions MUST be powers of two. A
    // 128x176 texture is not one the hardware can hold, and the allocation
    // fails with NO DIAGNOSTIC AT ALL -- which is how this was found: a
    // whole arena's textures silently refused to allocate, one after
    // another, and the screen showed only the clear colour.
    //
    // MDK's textures are 256x400, 128x176, 256x320 and so on, so EVERY one
    // of them has to be padded. The cost is VRAM: 256x400 becomes 256x512,
    // a third more. The price of the hardware's rule.
    C3dTexture texture;
    texture.realWidth = w;
    texture.realHeight = h;
    texture.paddedWidth = nextPowerOfTwo(w);
    texture.paddedHeight = nextPowerOfTwo(h);
    texture.indexed = true;
    // THE SOURCE IS KEPT. An evicted texture is made again from this, and
    // this is the only copy: the caller holds an id, not the pixels.
    texture.source.assign(px, px + (size_t)w * (size_t)h);
    texture.palette.assign(paletteRgba, paletteRgba + 256 * 4);
    texture.lastUsed = ++renderer->drawCounter;
    if (renderDebugFlags() & kRenderDebugTexLog) {
        // rdebug `texlog`: what the expansion will be given, for the first 24.
        static int32_t logged = 0;
        if (logged < 24) {
            logged++;
            int32_t colourful = 0, opaque = 0;
            for (int32_t e = 0; e < 256; e++) {
                const uint8_t* c = paletteRgba + e * 4;
                if (c[0] | c[1] | c[2]) colourful++;
                if (c[3]) opaque++;
            }
            size_t nonzero = 0;
            const size_t n = (size_t)w * (size_t)h;
            for (size_t i = 0; i < n; i++) {
                if (px[i]) nonzero++;
            }
            mdk::log3dsPrintf("texlog: index %ldx%ld palette=%p entries non-black=%ld alpha>0=%ld "
                              "entry1=(%u,%u,%u,%u) texels nonzero=%lu/%lu",
                              (long)w, (long)h, (const void*)paletteRgba, (long)colourful, (long)opaque,
                              paletteRgba[4], paletteRgba[5], paletteRgba[6], paletteRgba[7],
                              (unsigned long)nonzero, (unsigned long)n);
        }
    }

    const int32_t id = acquireTextureSlot(renderer);
    C3dTexture& slot = renderer->textures[(size_t)id];
    slot = std::move(texture);
    if (!makeTexture(renderer, slot, true)) {
        // The failed slot goes back to being free: no GPU copy, no source.
        slot = C3dTexture();
        return -1;
    }
    return id;
}

MdkTextureId mdk_render_create_rgba_texture(MdkRenderer r, int32_t w, int32_t h, const uint8_t* px) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !renderer->ready || w <= 0 || h <= 0 || w > 1024 || h > 1024 || !px) {
        return -1;
    }
    C3dTexture texture;
    texture.realWidth = w;
    texture.realHeight = h;
    // The PICA's texture-size register wants powers of two, and at least 8,
    // for RGBA textures as well as index textures. The real rectangle is
    // mapped into the padded one by the draw path's UV transform.
    texture.paddedWidth = nextPowerOfTwo(w);
    texture.paddedHeight = nextPowerOfTwo(h);
    texture.indexed = false;
    // The source is kept, so an evicted texture can be made again.
    texture.source.assign(px, px + (size_t)w * (size_t)h * 4);
    texture.lastUsed = ++renderer->drawCounter;

    const int32_t id = acquireTextureSlot(renderer);
    C3dTexture& slot = renderer->textures[(size_t)id];
    slot = std::move(texture);
    // The linear heap, not the texture VRAM -- see the index-texture
    // creation for why.
    if (!makeTexture(renderer, slot, true)) {
        slot = C3dTexture();
        return -1;
    }
    return id;
}

int32_t mdk_render_index_texture_bytes(MdkRenderer r, int32_t width, int32_t height) {
    if (width <= 0 || height <= 0 || width > 1024 || height > 1024) {
        return 0;
    }
    // WHAT THE 3DS SPENDS ON AN INDEX TEXTURE. The indices are expanded
    // through the palette into RGBA8, and the dimensions are padded up to
    // powers of two, so a 95x168 frame costs 128x256x4 = 131072 bytes --
    // eight times the 15960 bytes of the indices themselves. AND ITS MIP CHAIN
    // when Enhanced Mode enableMipmaps is on (makeTexture builds one then): a
    // third more. Kurt's sprite residency budgets against this number, so it is
    // the same pica::textureBytes the allocation is costed with.
    RendererC3D* renderer = asC3d(r);
    const int32_t paddedW = nextPowerOfTwo(width);
    const int32_t paddedH = nextPowerOfTwo(height);
    const int32_t levels = (renderer && renderer->toggles.enableMipmaps)
                               ? mdk::pica::mipLevelsFor(paddedW, paddedH)
                               : 0;
    return (int32_t)mdk::pica::textureBytes(mdk::pica::TexelFormat::Rgba8, paddedW, paddedH,
                                            levels);
}

MdkPaletteId mdk_render_create_palette(MdkRenderer r, const uint8_t* rgba) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !renderer->ready || !rgba) {
        return -1;
    }
    // The 3DS keeps the palette only to expand the textures through it.
    renderer->palettes.push_back(std::vector<uint8_t>(rgba, rgba + 256 * 4));
    return (MdkPaletteId)(renderer->palettes.size() - 1);
}

void mdk_render_update_texture(MdkRenderer r, MdkTextureId t, int32_t w, int32_t h, const uint8_t* px) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || t < 0 || (size_t)t >= renderer->textures.size() || !px) {
        return;
    }
    C3dTexture& texture = renderer->textures[(size_t)t];
    if (texture.source.empty() || texture.releasePending) {
        return;
    }
    if (w != texture.realWidth || h != texture.realHeight) {
        return;  // the contract is same-size updates only
    }
    // A bullet hole's texture changes. The 3DS's texture is EXPANDED, so
    // the update has to go through the palette again -- and the palette the
    // texture was expanded through is kept with the texture, so it can.
    const size_t bytes = (size_t)w * (size_t)h * (texture.indexed ? 1u : 4u);
    texture.source.assign(px, px + bytes);

    // If the old GPU copy may still be in flight, keep it and replace it at
    // the next frame boundary. Otherwise the replacement can be made now.
    const bool inFlight = renderer->gpuPending ||
                          (renderer->inFrame && texture.lastUsed >= renderer->frameStartUse);
    if (!texture.valid) {
        return;  // the next draw makes the new source
    }
    if (inFlight) {
        texture.stale = true;
        return;
    }
    texture.stale = false;
    C3D_TexDelete(&texture.tex);
    texture.valid = false;
    if (!makeTexture(renderer, texture, false)) {
        return;
    }
    texture.lastUsed = ++renderer->drawCounter;
}

// The mod system's colour-image replacements: the 3DS holds no enhanced
// colour textures (the mod system is Linux-only, SPEC.md line 36) and the
// shared code never registers one here, so these are deliberate no-ops.
void mdk_render_replace_colour(MdkRenderer, MdkTextureId, MdkPaletteId, int32_t, int32_t,
                               int32_t, const uint8_t*) {}
void mdk_render_replace_image(MdkRenderer, MdkTextureId, MdkPaletteId, int32_t, int32_t,
                              const uint8_t*) {}

void mdk_render_release_texture(MdkRenderer r, MdkTextureId t) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || t < 0 || (size_t)t >= renderer->textures.size()) {
        return;
    }
    C3dTexture& texture = renderer->textures[(size_t)t];
    const bool inFlight = renderer->gpuPending ||
                          (renderer->inFrame && texture.lastUsed >= renderer->frameStartUse);
    if (texture.valid && !inFlight) {
        C3D_TexDelete(&texture.tex);
        texture.valid = false;
    } else if (texture.valid) {
        // The id is invalid to the caller now, but the GPU may still be
        // reading the old copy. The next frame boundary frees it.
        texture.releasePending = true;
    }
    // A RELEASE is final: the source goes with the GPU copy, so the slot is
    // free again once the deferred deletion settles. (An EVICTION is not a
    // release: it keeps the source, and the texture is made again when it is
    // next drawn.)
    texture.source.clear();
    texture.palette.clear();
    texture.stale = false;
}

void mdk_render_release_palette(MdkRenderer r, MdkPaletteId p) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || p < 0 || (size_t)p >= renderer->palettes.size()) {
        return;
    }
    renderer->palettes[(size_t)p].clear();
}

MdkMeshId mdk_render_create_mesh(MdkRenderer r, const mdk::Vertex* v, int32_t n) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !renderer->ready || n <= 0 || !v) {
        return -1;
    }
    C3dMesh mesh;
    mesh.capacity = n;
    // The 3DS's vertex: the position and the texture coordinate, five
    // floats. The packed colour is NOT uploaded: the original look's world
    // surfaces are unlit, and the flat-coloured surfaces are drawn with a
    // one-pixel texture of that colour instead.
    mesh.data = linearAlloc((size_t)n * 5 * sizeof(float));
    if (!mesh.data) {
        return -1;
    }
    mesh.valid = true;
    // THE MESH IS PUSHED BEFORE IT IS FILLED.
    //
    // The update takes the mesh's ID and looks the mesh up by it, so a mesh
    // that is not in the table yet cannot be updated: the update finds
    // nothing, the vertex count stays ZERO, and every draw of that mesh is
    // then skipped -- silently, because a draw with no vertices is a draw
    // that is simply not made.
    //
    // The symptom was a screen that showed only the clear colour while the
    // log said the arena had loaded with 154 batches and 1197 vertices.
    renderer->meshes.push_back(mesh);
    const MdkMeshId id = (MdkMeshId)(renderer->meshes.size() - 1);
    mdk_render_update_mesh(r, id, v, n);
    return id;
}

MdkMeshId mdk_render_create_dynamic_mesh(MdkRenderer r, int32_t cap) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || !renderer->ready || cap <= 0) {
        return -1;
    }
    C3dMesh mesh;
    mesh.capacity = cap;
    mesh.data = linearAlloc((size_t)cap * 5 * sizeof(float));
    if (!mesh.data) {
        // Printed, like every other allocation this backend refuses (the scratch
        // buffer, the textures). A mesh that is not there is a draw that is simply
        // not made — no fault, no warning, no picture — and the emulator's bigger
        // heap cannot show it, so the guest log is the only place it can be seen
        // (AGENTS P3/P6, review-10 F3).
        if (!renderer->meshAllocFailureLogged) {
            renderer->meshAllocFailureLogged = true;
            mdk::log3dsPrintf("linearAlloc(%u) FAILED for a dynamic mesh of %d corners",
                              (unsigned)((size_t)cap * 5 * sizeof(float)), (int)cap);
        }
        return -1;
    }
    renderer->meshAllocFailureLogged = false;
    mesh.valid = true;
    renderer->meshes.push_back(mesh);
    return (MdkMeshId)(renderer->meshes.size() - 1);
}

void mdk_render_update_mesh(MdkRenderer r, MdkMeshId m, const mdk::Vertex* v, int32_t n) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || m < 0 || (size_t)m >= renderer->meshes.size() || !v || n <= 0) {
        return;
    }
    C3dMesh& mesh = renderer->meshes[(size_t)m];
    if (!mesh.valid || n > mesh.capacity) {
        return;
    }
    // SAFE INSIDE THE FRAME IT BELONGS TO, which is the seam's contract
    // (render.h): C3D_FrameBegin waited for the previous queue, so nothing is
    // reading this buffer when the frame's own draws write it. An update outside a
    // frame would be an in-flight write; no caller does one.
    float* out = static_cast<float*>(mesh.data);
    for (int32_t i = 0; i < n; i++) {
        out[i * 5 + 0] = v[i].pos.x;
        out[i * 5 + 1] = v[i].pos.y;
        out[i * 5 + 2] = v[i].pos.z;
        out[i * 5 + 3] = v[i].uv.x;
        out[i * 5 + 4] = v[i].uv.y;
    }
    // THE CPU/GPU CACHE-VISIBILITY OBLIGATION (AGENTS.md P3) -- AND THIS CALL IS
    // NOT WHAT MEETS IT.
    //
    // WHAT IS MEASURED (review-1 F1, re-measured 2026-11-12 against the linked
    // `/opt/devkitpro/libctru/lib/libcitro3dd.a`):
    //   * `C3D_FrameEnd` flushes the ENTIRE linear heap: `tst r4,#2`, then
    //     `ldr r0, __ctru_linear_heap`; `ldr r1, __ctru_linear_heap_size`;
    //     `bl GSPGPU_FlushDataCache` (`arm-none-eabi-objdump -dr renderqueue.o`,
    //     relocs 0x168/0x16c). `mdk_render_end_frame`'s caller reaches
    //     `C3D_FrameEnd(0)`.
    //   * citro3d submits the command queue at that same frame end, so the flush
    //     precedes the GPU ever reading the vertex data. That is AGENTS.md P3's
    //     own sentence: the default `C3D_FrameEnd(0)` path "flushes linear memory
    //     before queue execution".
    //   * `GSPGPU_FlushDataCache` in this libctru is a SYNCHRONOUS service call:
    //     it ends in `bl svcSendSyncRequest` (`libctru.a:gspgpu.o`). So this line
    //     costs one GSP round-trip per mesh upload per frame, and the object
    //     stream, Kurt, the canvas and the diagnostic all upload every frame.
    //
    // THE ORDERING ARGUMENT, WRITTEN DOWN RATHER THAN IMPLIED. There is none that
    // favours keeping it. The mesh bytes are written here, the draws are recorded
    // later in the same frame, and the queue is not handed to the GPU until
    // `C3D_FrameEnd(0)` -- which flushes first. No path lets the GPU read these
    // bytes before that flush, so the frame-end flush is sufficient and this call
    // is redundant. The old comment claimed the opposite ("P3 asks for it and real
    // silicon needs it"); that claim is withdrawn.
    //
    // IT STAYS ANYWAY, FOR ONE HONEST REASON: the only target this loop can
    // measure on is Mandarine, whose PICA renderer reads the same host memory the
    // emulated CPU wrote, so NO EMULATOR RUN CAN DISTINGUISH this call from its
    // absence (measured 2026-11-11: the canvas is absent both with this flush and
    // with the D-cache deliberately thrashed by 512 KB of unrelated linear writes
    // before the draw). Removing a cache clean on the strength of an instrument
    // that cannot see it would be the same mistake as adding one. Dropping it is a
    // named follow-up in docs/OPEN_ITEMS.md, gated on an original-3DS capture --
    // and it is a PERFORMANCE win on the 268 MHz target, not a correctness fix.
    //
    // AND IT WAS NEVER THE CANVAS BUG. The theory that a mesh written during the
    // frame was still dirty when the replay handed its address to
    // `C3D_DrawArrays` is refuted; see docs/OPEN_ITEMS.md, "The 3DS canvas draws
    // no pixels", for what the measurements now do say.
    //
    // THE RANGE IS THE WRITTEN PREFIX, NOT THE BUFFER. The clean is per line and
    // the cost is proportional to the range; a mesh of 4096 quads is 245760
    // bytes and flushing all of it to publish 24 corners would be paying for a
    // capacity the frame did not use.
    GSPGPU_FlushDataCache(mesh.data, (u32)n * 5u * sizeof(float));
    mesh.count = n;
}

void mdk_render_release_mesh(MdkRenderer r, MdkMeshId m) {
    RendererC3D* renderer = asC3d(r);
    if (!renderer || m < 0 || (size_t)m >= renderer->meshes.size()) {
        return;
    }
    C3dMesh& mesh = renderer->meshes[(size_t)m];
    // DEFERRED, LIKE A TEXTURE. The mesh may have been drawn by a frame whose
    // queue has not been waited yet -- C3D_DrawArrays only records the command,
    // and the queue is submitted at C3D_FrameEnd -- so the linear memory is
    // still an in-flight resource. C3D_FrameBegin waits the previous queue, so
    // the next frame boundary is the completion boundary (AGENTS.md P3).
    //
    // The cost is that a mesh that grows (the object stream's, which doubles
    // when a frame needs more) holds its old buffer for one more frame. That is
    // a bounded transient, and it is what P3 asks for.
    mesh.valid = false;
    mesh.count = 0;
    if (mesh.data) {
        mesh.releasePending = true;
    }
}

// Free the meshes whose frame has finished. Called at the frame boundary, after
// C3D_FrameBegin waited for the previous queue (AGENTS.md P3).
static void settleDeferredMeshes(RendererC3D* renderer) {
    if (!renderer) {
        return;
    }
    for (size_t i = 0; i < renderer->meshes.size(); i++) {
        C3dMesh& mesh = renderer->meshes[i];
        if (!mesh.releasePending) {
            continue;
        }
        if (mesh.data) {
            linearFree(mesh.data);
            mesh.data = nullptr;
        }
        mesh.releasePending = false;
        mesh.capacity = 0;
    }
}

MdkScope mdk_render_mark(MdkRenderer r) { (void)r; return MdkScope{}; }
void mdk_render_release(MdkRenderer r, MdkScope s) { (void)r; (void)s; }

