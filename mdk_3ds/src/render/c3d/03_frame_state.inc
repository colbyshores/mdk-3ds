// 03_frame_state.inc -- the render debug switches, the deferred-texture settle, the GPU state setters and their dedupe.
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

// ---------------------------------------------------------------------------
// The MVP for one eye lives in render/pica_matrix.cpp.
//
// It moved there so a host test can pin the three PICA conversions (the free
// transpose, the [0,1]->[-1,0] depth remap, the 90-degree screen rotation) and
// the SIDE a per-draw world matrix composes on -- none of which can be checked
// from a file that includes <3ds.h>, and all of which fail as INVISIBLE
// GEOMETRY rather than as a wrong picture. The conventions, and the identity
// `W*(V*P*E*R) == (W*V*P)*E*R` that makes a per-draw matrix legal at all, are
// written out in that header.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

// RENDER DEBUG SWITCHES, armed by a file (the drawlog.txt pattern): the
// contents of sdmc:/3ds/mdk/rdebug.txt name the switches, any of
//     flat     scene draws ignore the texture: constant colour per pass
//     texlog   describe the first 24 index textures as they are created
// They exist to bisect a draw that reaches the GPU and produces no visible
// pixels (2026-11-17: the arena on every level, census onscreen=947, while
// objects and the HUD draw). Read ONCE; absent, the cost is one failed open.
// The arming is logged so a capture always states which switches were on.
static const uint32_t kRenderDebugFlat = 1u;
static const uint32_t kRenderDebugTexLog = 4u;   // `texlog`: describe the first index textures
static const uint32_t kRenderDebugUvLog = 16u;   // `uvlog`: on frame 30, draws whose uv leaves [0,1]
// The canvas canary's arming (see the drawDiagnostic call in end_frame).
// Read once; absent, the cost is one failed open.
static bool canaryArmed() {
    static int32_t armed = -1;
    if (armed < 0) {
        FILE* file = fopen("sdmc:/3ds/mdk/canary.txt", "rb");
        armed = file ? 1 : 0;
        if (file) {
            fclose(file);
            mdk::log3dsPrintf("canary: ARMED by sdmc:/3ds/mdk/canary.txt (the (0,0,255) panel marker)");
        }
    }
    return armed != 0;
}

static uint32_t renderDebugFlags() {
    static int32_t flags = -1;
    if (flags < 0) {
        flags = 0;
        if (FILE* file = fopen("sdmc:/3ds/mdk/rdebug.txt", "rb")) {
            char text[128] = {};
            const size_t got = fread(text, 1, sizeof(text) - 1, file);
            fclose(file);
            text[got] = '\0';
            if (strstr(text, "flat")) flags |= (int32_t)kRenderDebugFlat;
            if (strstr(text, "texlog")) flags |= (int32_t)kRenderDebugTexLog;
            if (strstr(text, "uvlog")) flags |= (int32_t)kRenderDebugUvLog;
            mdk::log3dsPrintf("rdebug: ARMED by sdmc:/3ds/mdk/rdebug.txt flags=%ld (flat=%d texlog=%d)",
                              (long)flags, (flags & (int32_t)kRenderDebugFlat) ? 1 : 0,
                              (flags & (int32_t)kRenderDebugTexLog) ? 1 : 0);
        }
    }
    return (uint32_t)flags;
}

static bool makeTexture(RendererC3D* renderer, C3dTexture& texture, bool diagnostics);
// Starts a large stale texture's re-make in slices instead of whole
// (04_expansion.inc); false when the texture is not a chunked candidate.
static bool startStaleExpansion(RendererC3D* renderer, C3dTexture& texture);

// The HD stream's entry points (04_hd_stream.inc, THE HD STREAM), declared here
// because the frame boundary calls them before they are defined.
static void dropHd(RendererC3D* renderer, HdCopy& copy);
static void pumpHdTextures(RendererC3D* renderer, uint32_t protectFrom);
static void pumpPackOriginals(RendererC3D* renderer, uint32_t previousFrameStart);
static void pumpExpansions(RendererC3D* renderer);
static bool evictHd(RendererC3D* renderer, uint32_t protectFrom, bool unlinkedOnly);

// Settle the texture changes that could not be made while the GPU might
// still be reading the old copy.
//
// C3D_FrameBegin has waited for the previous queue by the time this runs,
// so deleting and replacing these copies is safe (AGENTS.md P3). A release
// wins over an update: the id is already invalid and its source is gone.
static void settleDeferredTextures(RendererC3D* renderer) {
    if (!renderer) {
        return;
    }
    // The mirror cubes given back while a frame could still read them
    // (mdk_render_release_mirror_cubes).
    for (int32_t i = 0; i < mdk::kMirrorRowShiftsMax; i++) {
        RendererC3D::MirrorCube& cube = renderer->mirrorCubes[i];
        if (cube.releasePending) {
            if (cube.valid) {
                C3D_TexDelete(&cube.tex);
            }
            cube.valid = false;
            cube.releasePending = false;
            cube.faces = 0;
        }
    }
    // Releases come first: they return memory without needing an
    // allocation, and a stale replacement below may need that memory.
    for (size_t i = 0; i < renderer->textures.size(); i++) {
        C3dTexture& texture = renderer->textures[i];
        if (!texture.releasePending) {
            continue;
        }
        if (texture.valid) {
            C3D_TexDelete(&texture.tex);
            texture.valid = false;
        }
        texture.releasePending = false;
        texture.stale = false;
        texture.restamped = false;
        // swap, not clear(): clear keeps the capacity, and an arena-sized release
        // (game/arena_views.h) must hand the heap back for the next arena's stream.
        std::vector<uint8_t>().swap(texture.source);
        std::vector<uint8_t>().swap(texture.palette);
    }
    for (size_t i = 0; i < renderer->textures.size(); i++) {
        C3dTexture& texture = renderer->textures[i];
        if (!texture.stale) {
            continue;
        }
        texture.stale = false;
        // A large stale re-make in slices, not whole at this boundary
        // (measured: a 1024x256 canvas-pinned texture re-expanded for
        // 320 ms at the DANT_2 entry, then finished whole by its own
        // draw when the copy was deleted first). The old copy keeps
        // drawing while the new one expands; the finish swaps it at a
        // frame boundary (advanceExpansion). Until then the surface
        // shows its 16-bit copy -- the transient the canvas pin already
        // accepts for one frame, stretched over the expansion.
        if (!texture.source.empty() && startStaleExpansion(renderer, texture)) {
            texture.stalePending = true;
            texture.lastUsed = ++renderer->drawCounter;
            continue;
        }
        if (texture.valid) {
            C3D_TexDelete(&texture.tex);
            texture.valid = false;
        }
        if (!texture.source.empty() && makeTexture(renderer, texture, false)) {
            // The replacement is protected until it is first drawn.
            texture.lastUsed = ++renderer->drawCounter;
        }
    }
    // THE BULLET HOLES' BUDGET (mdk_render_update_texture): one restamped texture is
    // made again per frame boundary, the first in id order. One that is not on the
    // GPU any more (evicted, or made again above from the same new source) only
    // loses its flag: its next making takes the source with the hole.
    for (size_t i = 0; i < renderer->textures.size(); i++) {
        C3dTexture& texture = renderer->textures[i];
        if (!texture.restamped) {
            continue;
        }
        texture.restamped = false;
        if (!texture.valid || texture.source.empty()) {
            continue;
        }
        C3D_TexDelete(&texture.tex);
        texture.valid = false;
        if (makeTexture(renderer, texture, false)) {
            texture.lastUsed = ++renderer->drawCounter;
        }
        break;
    }
    // THE STREAMED PICTURES' BUDGET (mdk_render_update_paletted, package #20c): one
    // a boundary, after C3D_FrameBegin's wait, so the store the last frame sampled
    // is not rewritten under the GPU (AGENTS.md P3). A copy already in the stream
    // store is rewritten in place (no allocation, no free); anything else (the
    // first picture, a copy an eviction re-made the ordinary way) is made again,
    // which makeTexture routes to the stream store.
    for (size_t i = 0; i < renderer->textures.size(); i++) {
        C3dTexture& texture = renderer->textures[i];
        if (!texture.streamDirty) {
            continue;
        }
        texture.streamDirty = false;
        if (texture.source.empty()) {
            continue;
        }
        if (texture.valid && texture.format == mdk::pica::TexelFormat::Rgb565 &&
            texture.mipLevels == 0 && texture.tex.data) {
            mdk::pica::streamTexelsRgb565(texture.source.data(), texture.realWidth,
                                          texture.realHeight, texture.palette.data(),
                                          texture.paddedWidth, (uint8_t*)texture.tex.data);
            C3D_TexFlush(&texture.tex);
        } else {
            if (texture.valid) {
                C3D_TexDelete(&texture.tex);
                texture.valid = false;
            }
            makeTexture(renderer, texture, false);
        }
        texture.lastUsed = ++renderer->drawCounter;
        break;
    }
}

// Defined with the resource code below; the frame boundary needs them and it
// comes first in the file.
static void settleDeferredMeshes(RendererC3D* renderer);
static void replayQueue(RendererC3D* r);
static void copyInsetsToSecondEye(RendererC3D* r);
static void drawDiagnostic(RendererC3D* r, float canvasWidth);
static void installEye(RendererC3D* r, C3D_RenderTarget* target, float offset);
static void prepareHaze(RendererC3D* r);
static void prepareLighting(RendererC3D* r);
static void prepareShadows(RendererC3D* r);
static void castShadows(RendererC3D* r);
static bool makeShadowRamp(RendererC3D* r);
static mdk::pica::ShadowByteOrder shadowByteOrder(const RendererC3D* r);
static void installLitUniforms(RendererC3D* r);
static void installShadowUniforms(RendererC3D* r);
static void leaveShadowLitStages(RendererC3D* r);
static void prepareQueue(RendererC3D* r);
static void drawRecorded(RendererC3D* r, const PreparedDraw* p);
static void installAlphaTest(RendererC3D* r, int reference, bool force);

// THE STENCIL, for the camera's arena over the other one (render/draw_queue.h,
// 06_queue.inc prepareQueue). The eye targets are DEPTH24_STENCIL8 and every
// depth clear (kDepthClearValue, a 32-bit fill) leaves the stencil 0. Mark: a
// fragment that passes writes 1. Test: a fragment draws only where the stencil
// is still 0. Off: no test, no write. One state per record, cached.
static void installStencil(RendererC3D* r, int mode) {
    if (r->stencilMode == mode) {
        return;
    }
    if (mode == kStencilMark) {
        C3D_StencilTest(true, GPU_ALWAYS, 1, 0xFF, 0xFF);
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
    } else if (mode == kStencilTest) {
        C3D_StencilTest(true, GPU_EQUAL, 0, 0xFF, 0x00);
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
    } else if (mode == kStencilKurtMark || mode == kStencilKurtMarkGlass) {
        // The surface's bit set wherever the mark passes its depth test.
        const int bit = mode == kStencilKurtMark ? mdk::kKurtStencilBit : mdk::kKurtGlassBit;
        C3D_StencilTest(true, GPU_ALWAYS, bit, 0xFF, bit);
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
    } else if (mode == kStencilKurtTested) {
        // Only where the mark is clear (the reference's mark bit is 0); "drawn"
        // set. The PICA compares and replaces with the one reference.
        C3D_StencilTest(true, GPU_EQUAL, mdk::kKurtDrawnBit, mdk::kKurtStencilBit,
                        mdk::kKurtDrawnBit);
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
    } else if (mode == kStencilKurtOver) {
        // Only where the mark is set; the mark cleared and "drawn" set.
        C3D_StencilTest(true, GPU_NOTEQUAL, mdk::kKurtDrawnBit, mdk::kKurtStencilBit,
                        mdk::kKurtStencilBit | mdk::kKurtDrawnBit);
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
    } else if (mode == kStencilKurtGlass) {
        // An arena glass batch: not where Kurt was drawn over his own glass.
        C3D_StencilTest(true, GPU_NOTEQUAL, mdk::kKurtGlassBit | mdk::kKurtDrawnBit,
                        mdk::kKurtGlassBit | mdk::kKurtDrawnBit, 0x00);
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
    } else {
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0x00);
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
    }
    r->stencilMode = mode;
}

// Install one pass's depth state on the citro3d context and record it in the
// cache.
//
// THE WRITE MASK IS NOT THE TEST. The mask is the PICA's combined colour+depth
// write mask, so "test, but do not write depth" is GPU_WRITE_COLOR — write the
// colour, not the depth — and NEVER 0: a mask of 0 asks the hardware to write
// nothing at all, which is a draw that happens and is invisible. This frame used
// to install `(GPU_WRITEMASK)0` along with a disabled test; whether that mask
// gates colour is exactly the sort of thing this host cannot check, so nothing
// here relies on the answer.
//
// GPU_LEQUAL and not GPU_LESS: a fragment exactly on the far plane stores
// exactly the value the buffer was cleared to, so a strict LESS would reject the
// far plane itself. The oracle says LESS_OR_EQUAL (Renderer.cs:605), and
// `pica::depthPasses` pins that tie.
static void installDepthState(RendererC3D* r, mdk::pica::DepthState state) {
    C3D_DepthTest(state.test,
                  state.test && !state.reset && !state.always
                      ? (state.equal ? GPU_EQUAL : GPU_LEQUAL)
                      : GPU_ALWAYS,
                  state.reset ? GPU_WRITE_DEPTH : state.write ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
    r->depthTest = state.test;
    r->depthWrite = state.write;
    r->depthAlways = state.always;
}

// The host-readable factor to the GPU's. The switch is exhaustive over the
// four the oracle's rule can name, and the default is unreachable: a new
// factor in `pica_matrix.h` without a line here lands on ONE, which is the
// replace, i.e. the draw paints what it was given -- the least surprising
// failure for a factor this backend was never told about.
static GPU_BLENDFACTOR gpuBlendFactor(mdk::pica::BlendFactor factor) {
    switch (factor) {
    case mdk::pica::kBlendZero: return GPU_ZERO;
    case mdk::pica::kBlendOne: return GPU_ONE;
    case mdk::pica::kBlendSrcAlpha: return GPU_SRC_ALPHA;
    case mdk::pica::kBlendOneMinusSrcAlpha: return GPU_ONE_MINUS_SRC_ALPHA;
    case mdk::pica::kBlendOneMinusDstColour: return GPU_ONE_MINUS_DST_COLOR;
    case mdk::pica::kBlendConstantAlpha: return GPU_CONSTANT_ALPHA;
    case mdk::pica::kBlendOneMinusConstantAlpha: return GPU_ONE_MINUS_CONSTANT_ALPHA;
    case mdk::pica::kBlendDstColour: return GPU_DST_COLOR;
    }
    return GPU_ONE;
}

// Install one pass's blend state on the citro3d context and record what it was
// derived from.
//
// WHY THIS IS WRITTEN AT ALL, WHEN THE PICTURE ALREADY LOOKS BLENDED. citro3d
// does not leave this register alone: C3D_Init calls
// `C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA,
// GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA)` -- measured
// from the linked libcitro3dd.a, not from a doc -- so blending has been ON for
// every pass of every frame since the port's first build, with the oracle's
// COLOUR pair and not its alpha pair. That coincidence is what made glass look
// right, and it is a coincidence in three separate ways: the four opaque passes
// blend when the oracle says they must not (so a texel of alpha 128 is
// translucent here and opaque in the game), the alpha channel's source factor
// is SRC_ALPHA where the oracle writes ONE, and `Blend::Add` has no
// representation anywhere. AGENTS.md P2 is the rule that settles it: all GPU
// state is set explicitly every frame, and a register whose value comes from a
// library's initialiser is not set by this backend.
//
// THE DISABLE IS A PAIR, NOT A FLAG. The PICA has no "blend off"; the oracle's
// `enable_blend = false` is (ONE, ZERO) on both channels. That is only safe
// because of the alpha test installed next to it -- see
// `pica::alphaTestPasses` -- since an index-0 texel that is BLENDED with alpha 0
// still writes depth, and one that is DISCARDED does not.
// THE ONE BLEND WRITER. `pass`/`mode` are the cache key installBlendState
// compares; the glow (drawGlow) writes its screen blend with a key no pass has
// (-2), so the next queued draw rewrites its own.
static void writeBlendState(RendererC3D* r, const mdk::pica::BlendState& eff, int pass,
                            int mode) {
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   gpuBlendFactor(eff.srcColour), gpuBlendFactor(eff.dstColour),
                   gpuBlendFactor(eff.srcAlpha), gpuBlendFactor(eff.dstAlpha));
    r->blendPass = pass;
    r->blendMode = mode;
}

static void installBlendState(RendererC3D* r, mdk::Pass pass, mdk::Blend blend) {
    const mdk::pica::BlendState s = mdk::pica::blendStateForPass(pass, blend);
    // `enable` is read here, not only asserted by the host suite: a state the
    // oracle calls disabled installs as the replace pair whatever factors it
    // carries, so the struct cannot claim "off" and mix anyway (review-4 F6).
    // This changes no pixel -- the four opaque states already carry that pair
    // (pica::kBlendReplace) -- it removes a way of being wrong that nothing
    // downstream could check. One call site, because the frame gate counts them
    // (`alpha-blend-single-writer`) and a second writer would write behind the
    // blend cache's back.
    const mdk::pica::BlendState eff = s.enable ? s : mdk::pica::kBlendReplace;
    writeBlendState(r, eff, static_cast<int>(pass), static_cast<int>(blend));
}

// Install ONE EYE: its target, its matrix, and every piece of fixed state the
// draws need. Called from mdk_render_end_frame once per eye pass, immediately
// before that eye's replay of the queue -- C3D_FrameDrawOn selects the target
// for the draws that FOLLOW it, so an install that is not immediately followed
// by its own draws is an install that gets overwritten.
//
// Most of this state is context-global in citro3d (C3D_FrameDrawOn does not
// restore it -- see the cull-mode cache above), so re-installing it per eye is
// belt and braces rather than the mechanism. It is written every frame anyway,
// because an unwritten PICA register is the invisible-geometry failure mode and
// not an error (AGENTS.md P2).
// THE ONE ALPHA-TEST WRITER. `reference` comes from pica::alphaTestReferenceFor;
// `force` writes it whatever the cache says (installEye: the register is frame
// state and the cache must describe the context, the cullMode discipline).
static void installAlphaTest(RendererC3D* r, int reference, bool force) {
    if (!force && reference == r->alphaReference) {
        return;
    }
    // A negative reference is the test OFF (the shadow caster pass: its alpha is
    // a depth byte, not a coverage).
    if (reference < 0) {
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
    } else {
        C3D_AlphaTest(true, GPU_GREATER, reference);
    }
    r->alphaReference = reference;
}

// ============================================================================
// THE DEDUPE (Old 3DS, level 8 stereo: ~245 queued draws an eye, and the CPU
// spends 11-14 ms a frame building their command list -- the vault's
// pica200-many-pass-frame-is-drawcall-bound-not-fill-bound and
// citro3d-redundant-per-draw-state-writes). Every per-draw write below used to
// be made on every draw; each is now compared with what the GPU will already
// hold and skipped when BIT-IDENTICAL, so the picture cannot change: a skipped
// write is one whose register value is already the value it would have sent.
//
// WHAT "ALREADY HOLD" IS READ FROM, per category (RendererC3D, THE PER-DRAW
// STATE DEDUPE):
//   * a vertex uniform: citro3d's own C3D_FVUnif array. Every uniform write in
//     this backend goes through it (C3D_FVUnifSet/Mtx4x4/WritePtr), and
//     C3D_UpdateUniforms sends what it holds, so a write that leaves it unchanged
//     changes no register. The two ways a register can differ from the array are
//     both handled where they happen: shader constants (uploaded straight to
//     their registers at a bind -- measured in this build's shbin at c94/c95, far
//     above the c0-c10 uniforms; create logs it, `draw dedupe:`) and shader unit 3 leaving geometry mode
//     (bindProgram re-dirties the vertex uniforms). installEye re-dirties them
//     all anyway, so each eye starts from registers that match the array.
//   * combiner stage 0 and the vertex buffer table: the context's own copies,
//     through the pointers C3D_GetTexEnv(0) and C3D_GetBufInfo return (taken in
//     installEye). Whoever wrote them last -- the glow, the canvas canary, the
//     caster pass, the shadowed lit layout -- the compare sees it.
//   * a texture unit: the shadow bindTexUnit keeps (citro3d has no getter), so
//     C3D_TexBind has exactly one call site, here.
static void bindTexUnit(RendererC3D* r, int unit, C3D_Tex* tex) {
    if (unit < 0 || unit > 2) {
        return;
    }
    C3D_TexBind(unit, tex);
    // C3D_TexBind refuses a non-2D texture on units 1-2 (its own check of the
    // mode bits): the shadow must not then claim the bind happened.
    const bool refused = unit > 0 && tex && (tex->param & (7u << 28)) != 0;
    r->texUnitBound[unit] = refused ? nullptr : tex;
    if (tex && !refused) {
        memcpy(&r->texUnitState[unit], tex, sizeof(C3D_Tex));
    }
}

// The texture memory a bound C3D_Tex describes may now hold other texels (a
// texture made, remade or imported): the next bind of every unit is a real one,
// and with it citro3d's texture-cache clear (TEXUNIT_CONFIG bit 16).
static void forgetTextureBinds(RendererC3D* r) {
    for (int unit = 0; unit < 3; unit++) {
        r->texUnitBound[unit] = nullptr;
    }
}

// drawRecorded's bind: skipped when the unit already holds this C3D_Tex with
// the same address, size, format, filter, wrap and levels it was bound with.
static void bindTexUnitIfChanged(RendererC3D* r, int unit, C3D_Tex* tex) {
    if (tex && unit >= 0 && unit <= 2 && r->texUnitBound[unit] == tex &&
        memcmp(&r->texUnitState[unit], tex, sizeof(C3D_Tex)) == 0) {
        r->dedupeSkipped[kDedupeTexBind]++;
        return;
    }
    bindTexUnit(r, unit, tex);
    r->dedupeWritten[kDedupeTexBind]++;
}

// Whether vertex uniforms [location, location + count) already hold `want`, in
// citro3d's own array. A byte compare: no float compare (AGENTS.md R3), and a
// -0 against a +0 is a write, never a skip.
static bool vsUniformsHold(int location, const C3D_FVec* want, int count) {
    if (location < 0 || count <= 0 || location + count > C3D_FVUNIF_COUNT) {
        return false;
    }
    return memcmp(&C3D_FVUnif[GPU_VERTEX_SHADER][location], want,
                  sizeof(C3D_FVec) * (size_t)count) == 0;
}

static void setVsUniform(RendererC3D* r, DedupeCategory category, int location, float x,
                         float y, float z, float w) {
    const C3D_FVec want = FVec4_New(x, y, z, w);
    if (vsUniformsHold(location, &want, 1)) {
        r->dedupeSkipped[category]++;
        return;
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, location, x, y, z, w);
    r->dedupeWritten[category]++;
}

static void setVsMatrix(RendererC3D* r, DedupeCategory category, int location,
                        const C3D_Mtx* m) {
    if (vsUniformsHold(location, m->r, 4)) {
        r->dedupeSkipped[category]++;
        return;
    }
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, location, m);
    r->dedupeWritten[category]++;
}

// Combiner stage 0, against the context's own stage 0.
static void setTexEnv0(RendererC3D* r, C3D_TexEnv* want) {
    if (r->liveTexEnv &&
        memcmp(r->liveTexEnv, want, sizeof(C3D_TexEnv)) == 0) {
        r->dedupeSkipped[kDedupeTexEnv0]++;
        return;
    }
    C3D_SetTexEnv(0, want);
    r->dedupeWritten[kDedupeTexEnv0]++;
}

// The vertex buffer table for a mesh of five-float vertices (position, uv: the
// permutation 0x10), against the context's own table. The table is a pure
// function of the pointer, so it is rebuilt only when the mesh changes.
static void bindVertexBuffer(RendererC3D* r, const void* data) {
    if (data != r->vbufFor) {
        BufInfo_Init(&r->vbufInfo);
        BufInfo_Add(&r->vbufInfo, data, 5 * sizeof(float), 2, 0x10);
        r->vbufFor = data;
    }
    if (r->liveBufInfo &&
        memcmp(r->liveBufInfo, &r->vbufInfo, sizeof(C3D_BufInfo)) == 0) {
        r->dedupeSkipped[kDedupeBufInfo]++;
        return;
    }
    C3D_SetBufInfo(&r->vbufInfo);
    r->dedupeWritten[kDedupeBufInfo]++;
}

// ============================================================================
// THE GEOMETRY-STAGE RULE (HARDWARE; measured 2026-11-18 on the Old 3DS
// appliance with mdk_3ds/tools/gstest, which draws through this DVLB).
// On the PICA200 a program WITHOUT a geometry shader must not draw after a
// program WITH one in the same command list: the GPU locks up for good (the
// game's main thread then waits in C3D_FrameBegin for ever -- the owner's New
// 3DS and the appliance both, first frame with enableLighting on). Measured,
// stage by stage, 90 frames each:
//     plain only / geometry only / plain -> geometry      fine
//     geometry -> geometry (lit <-> pass-through)         fine
//     geometry -> C3D_FrameSplit -> plain                 fine
//     geometry -> plain                                   HANG
//     geometry -> plain with shaderProgramUse (a full
//       re-send of code, uniforms and configuration)      HANG
// The emulator does not model it, which is how every enhancement built on a
// geometry shader passed there. So EVERY program bind in this backend goes
// through bindProgram, which splits the command list (the GPU then finishes the
// geometry work before the plain program's configuration runs) when a plain
// program follows a geometry program in the list. The scene avoids most of
// those splits by drawing its unlit draws through the pass-through program
// once it is in geometry mode (06_draw_submit.inc drawRecorded), so an eye leaves
// geometry mode once, where its scene ends.
//
// ALSO ON THE WAY BACK: in geometry mode shader unit 3 is the geometry unit and
// keeps the geometry shader's float uniforms; back in plain mode it is a fourth
// vertex unit, and citro3d re-sends only the vertex uniforms it holds dirty.
// The tint (c5) would be the geometry shader's `params` on a quarter of the
// vertices. Every vertex uniform is marked dirty on that transition.
//
// Each split is one GX queue entry (32, a full queue is svcBreak). A frame
// makes at most kMaxGeometrySplits; past it, nothing binds a geometry program
// for the rest of the frame (gsAllowed), so the frame can still leave geometry
// mode once more and no further.
static const int32_t kMaxGeometrySplits = 6;

// The frame's command list ends here and a new one begins: the GPU runs this
// one to its end before the next starts.
static void frameSplit(RendererC3D* r) {
    C3D_FrameSplit(0);
    r->gsInList = false;
}

static bool gsAllowed(const RendererC3D* r) {
    return r->geometrySplits < kMaxGeometrySplits;
}

static void bindProgram(RendererC3D* r, shaderProgram_s* program) {
    if (program == r->boundProgram) {
        return;
    }
    const bool geometry = program->geometryShader != nullptr;
    const bool leavingGeometry =
        !geometry && r->boundProgram && r->boundProgram->geometryShader != nullptr;
    if (!geometry && r->gsInList) {
        frameSplit(r);
        r->geometrySplits++;
    }
    C3D_BindProgram(program);
    if (leavingGeometry && r->vshUniformVecs > 0) {
        C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, 0, r->vshUniformVecs);
    }
    if (geometry) {
        r->gsInList = true;
    }
    r->boundProgram = program;
}

