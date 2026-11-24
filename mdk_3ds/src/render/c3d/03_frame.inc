// 03_frame.inc -- frame begin/end, eye state, the frame's setters.
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
// (04_textures.inc); false when the texture is not a chunked candidate.
static bool startStaleExpansion(RendererC3D* renderer, C3dTexture& texture);

// The HD stream's entry points (04_textures.inc, THE HD STREAM), declared here
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
                  state.test ? (state.equal ? GPU_EQUAL : GPU_LEQUAL) : GPU_ALWAYS,
                  state.write ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
    r->depthTest = state.test;
    r->depthWrite = state.write;
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
//
// Off in a harness build made with DRAWDEDUPE=0 (kDrawDedupe): every category is
// then written on every draw, as before, for the owner's A/B profile.
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
    if (kDrawDedupe && tex && unit >= 0 && unit <= 2 && r->texUnitBound[unit] == tex &&
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
    if (kDrawDedupe) {
        const C3D_FVec want = FVec4_New(x, y, z, w);
        if (vsUniformsHold(location, &want, 1)) {
            r->dedupeSkipped[category]++;
            return;
        }
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, location, x, y, z, w);
    r->dedupeWritten[category]++;
}

static void setVsMatrix(RendererC3D* r, DedupeCategory category, int location,
                        const C3D_Mtx* m) {
    if (kDrawDedupe && vsUniformsHold(location, m->r, 4)) {
        r->dedupeSkipped[category]++;
        return;
    }
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, location, m);
    r->dedupeWritten[category]++;
}

// Combiner stage 0, against the context's own stage 0.
static void setTexEnv0(RendererC3D* r, C3D_TexEnv* want) {
    if (kDrawDedupe && r->liveTexEnv &&
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
    if (kDrawDedupe && r->liveBufInfo &&
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
// once it is in geometry mode (06_draw.inc drawRecorded), so an eye leaves
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

static void installEye(RendererC3D* r, C3D_RenderTarget* target, float offset) {
    // The last eye may have ended in the shadowed lit draw's layout (units 1-2,
    // stages 1-3): put back before this eye's own stages are installed.
    leaveShadowLitStages(r);
    // THE BORROWED DEPTH (edge smoothing, buildEyeTargets): at scale 2 the right
    // eye draws against the left eye's depth buffer, which still holds the left
    // eye's scene. The split submits the left eye's commands to the GX queue
    // first, so the fill that follows runs after them and before the right eye's
    // (a clear queued without the split would run before EITHER eye's draws: the
    // frame's commands go to the GPU at C3D_FrameEnd). Measured in Mandarine at
    // slider 60: without these two calls the right eye lost 43% of its pixels.
    if (target == r->targetRight && r->targetsShareDepth) {
        frameSplit(r);
        C3D_FrameBufClear(&target->frameBuf, C3D_CLEAR_DEPTH, 0, mdk::pica::kDepthClearValue);
    }
    C3D_FrameDrawOn(target);
    r->eyeTarget = target;

    // The MVP for this eye: the view-projection with both PICA conversions
    // baked in. It is KEPT on the renderer, because a draw that carries a
    // world matrix composes it against this matrix instead of rebuilding the
    // whole chain per draw.
    mdk::pica::eyeViewProjection(r->view, r->projection, offset, r->eyeVp);
    // THE PER-EYE FRUSTUM for drawRecorded's cull: the same eye matrix BEFORE
    // the PICA conversions (the conversions are right-multiplications by fixed
    // matrices, so the volume is the same and the engine's clip convention
    // gives the planes directly). One extraction per eye per frame; the queue
    // replay that follows tests each opted-in draw against it.
    {
        float vpEngine[16];
        mdk::pica::eyeViewProjectionEngine(r->view, r->projection, offset, vpEngine);
        mdk::pica::frustumPlanes(vpEngine, r->frustum);
        r->frustumValid = true;
    }
    // THE STRIDED READ -- see mdk::pica::registers: register i must hold
    // COLUMN i of the row-major matrix, or a rotation comes out inverted.
    //
    // ALL UNIFORM REGISTERS MUST BE WRITTEN EVERY FRAME. An unwritten
    // uniform holds the previous frame's value or garbage. The shader
    // reads registers 0-3 (the mvp), so exactly those are written. A frame
    // whose draws all carry an identity world is placed by this one write
    // and nothing more; drawRecorded adds one per draw that does not.
    C3D_Mtx gpuMtx;
    mdk::pica::registers(r->eyeVp, gpuMtx.m);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->mvpLocation, &gpuMtx);
    r->matrixIsFrame = true;

    // THE UNIFORM THAT EVERY DRAW WITHOUT A TINT LEAVES AT WHITE, written once
    // per eye for the same reason the matrix is: an unwritten uniform holds the
    // previous frame's value, and the last thing that wrote this one was the last
    // canvas quad of the last frame -- a half-transparent red, which would tint
    // the whole scene if it survived into this one. `tintIsWhite` records it so
    // the scene's draws do not re-write it and the first canvas draw does.
    C3D_FVUnifSet(GPU_VERTEX_SHADER, r->tintLocation, 1.0f, 1.0f, 1.0f, 1.0f);
    r->tintIsWhite = true;

    bindProgram(r, &r->program);
    // THE DEDUPE'S EYE BOUNDARY (above: THE DEDUPE). Every vertex uniform is sent
    // again from citro3d's array -- P2's "every uniform written every frame", and
    // what makes the array the registers' value for this eye's compares (the
    // caster pass before the eyes ran its own program and constants). The
    // texture shadow is forgotten, so the eye's first bind is a real one and
    // clears the texture cache (the shadow map was rendered since the last eye).
    if (r->vshUniformVecs > 0) {
        C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, 0, r->vshUniformVecs);
    }
    forgetTextureBinds(r);

    // EVERY GPU STATE, EVERY FRAME. Uninitialised registers give
    // invisible geometry, not an error.
    C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
    AttrInfo_Init(attrInfo);
    // THE ATTRIBUTES MUST MATCH THE VERTEX LAYOUT EXACTLY. The mesh's
    // vertex is FIVE floats -- the position and the texture coordinate
    // -- and the attribute loaders say so here. Declaring a third
    // loader (a colour) that the vertex does not have makes the
    // attribute unit read past the end of every vertex, and the
    // geometry vanishes with no error at all.
    AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);   // position
    AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2);   // texcoord

    // The combiner. The original look is an unlit, untextured surface for
    // now; the textured path replaces this when the palette expansion
    // lands.
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    // The context's own stage 0 and buffer table, for the dedupe's compares. The
    // GetBufInfo marks the table dirty too: it is sent once per eye whatever the
    // draws then skip (P2).
    r->liveTexEnv = env;
    r->liveBufInfo = C3D_GetBufInfo();

    // THE DEPTH TEST IS ON, with the oracle's LESS_OR_EQUAL and a write
    // mask, for the frame's default pass. The ORIGINAL look still draws
    // back to front — the queue's pass order is the oracle's and the
    // painter's order still holds — but the C# does not rely on that: it
    // builds every mesh pipeline with `compare_op = LESS_OR_EQUAL`, depth
    // test enabled and depth write enabled except on the blended and exempt
    // passes (Renderer.cs:603-607). Without a test, an arena batch that the
    // queue order happens to place late can paint over something nearer,
    // and the depth pull (`render/depth_pull.cpp`, the oracle's DepthPull
    // for a poster over its wall) has nothing to bias against.
    //
    // The map this pairs with, and the evidence for the pairing, are in
    // pica_matrix.h next to the constants; the ordering they imply is pinned
    // by `PASS PicaMatrix` because no picture on this host can show it.
    installDepthState(r, mdk::pica::depthStateForPass(mdk::Pass::Solid));

    // THE ALPHA TEST IS THE ORACLE'S DISCARD, and it is frame state: the
    // palette shader discards index 0 on every surface it draws, so the test is
    // installed once per eye and no draw is allowed to turn it off. The
    // reference is `pica::kAlphaTestReference` and the function is GPU_GREATER,
    // so the predicate the hardware applies is the one `pica::alphaTestPasses`
    // states and the host suite pins.
    //
    // citro3d's initialiser leaves this DISABLED
    // (`C3D_AlphaTest(false, GPU_ALWAYS, 0)`, measured from the same object
    // file as the blend call above), so before this line the port had no
    // discard at all: index-0 texels were made invisible by blending with alpha
    // 0 and went on writing depth, hiding the world behind every transparent
    // texel of every arena texture.
    //
    // THREE CORNERS ARE NOT THE ORACLE'S, and they are recorded rather than
    // hidden. (1) The discard is `palette.hlsl`'s and it sits inside its
    // TEXTURED branch, so a FLAT colour of alpha 0 on an unblended pass is
    // painted by the oracle and discarded here. No shipped material does that
    // -- the flat colours are glass (alpha from the DTI, on a blended pass) and
    // the mirrors (alpha 1) -- and the alternative is a per-draw test keyed on
    // "is this a palette texture", for a case the data does not contain.
    // (2) The oracle's OTHER fragment programs do not discard at all: both
    // `mirror.hlsl:36` and `sky.hlsl:63,67` return `float4(..., 1.0)` with no
    // `discard`, so a mirror or sky repaint of palette index 0 paints
    // palette[0].rgb in the oracle and is discarded by this frame-wide test.
    // Neither pass is reachable on this backend yet -- there is no mirror
    // panorama and no sky pass -- and both already diverge in colour for other
    // reasons, so the frame-wide test stays and the corner is written down
    // where the next mirror batch will find it (review-3 F5).
    // (3) `enhanced.hlsl:199-201`, the MODE_CANVAS and MODE_IMAGE branches,
    // return the texel's OWN alpha (`float4(..., texel.a) * colour`) with no
    // discard, so a canvas or image texel of alpha 0 is painted by the oracle
    // and discarded by this frame-wide test. RECORDED, NOT ESTABLISHED
    // (review-5 F5 -- the earlier version of this note claimed a divergence and
    // handed the canvas batch a work item, and neither holds). Every canvas call
    // site is `Pass.Overlay` in the oracle (`Renderer.cs:320,325,329`, and
    // `Renderer.Insets.cs:127` draws the whole canvas in that pass), and
    // Overlay is blended SRC_ALPHA / ONE_MINUS_SRC_ALPHA (`Renderer.cs:565,
    // 571-573`), where a fragment of alpha 0 leaves the target bit-for-bit as a
    // discard does; `Blend::Add` (SRC_ALPHA / ONE) adds zero, the same. And a
    // PARTIAL alpha is above the `> 0` reference, so it survives this test rather
    // than being cut -- the palette expansion's fractional EDGE_CLEAR coverage is
    // not at risk here. (Under Enhanced Mode FILTERING a SCENE draw's reference
    // is the half cover, 127, and a partial alpha below it IS cut, as the PC's
    // HALF_COVER cuts it; the canvas keeps the reference 0 because the PC's
    // canvas modes return before that discard -- pica::alphaTestReferenceFor.) The corner has TWO INDEPENDENT TRIGGERS and either one is
    // enough (review-6 F3 -- an earlier draft of this line made them one
    // conjunction, which is the narrower and wrong reading): an UNBLENDED pass
    // moves COLOUR, because a fragment with blend off replaces the target, so the
    // oracle paints palette[0].rgb exactly where this discard writes nothing, and
    // that is true with depth test and write both off; a DEPTH-WRITING pass moves
    // DEPTH, because the oracle records the fragment's depth and a discard records
    // none, so a later depth-tested draw can win or lose differently even when the
    // colour works out. Neither needs the other. Nothing satisfies either today:
    // Overlay is blended and exempt from depth test and write alike
    // (`Renderer.cs:606-607`).
    // The canvas batch (CONTINUATION item 5b) should re-read this when it picks the
    // canvas's pass, and must NOT spend itself scoping the alpha test per pass
    // before that choice is made (review-4 F9 for the claim, review-5 F5 for the
    // correction).
    // Under Enhanced Mode FILTERING a scene draw's cut is the PC's half cover
    // instead (pica::alphaTestReferenceFor; never the canvas's), so the reference
    // is per draw: installAlphaTest is the one writer, cached, and this is the
    // frame's default -- the first pass the queue draws.
    installAlphaTest(r, mdk::pica::alphaTestReferenceFor(r->toggles.enableFiltering,
                                                         mdk::Pass::Solid), true);

    // The frame's default blend state: the pass the queue starts with. Written
    // every eye so the register is never left to citro3d's initialiser, and
    // the cache is told what was written (the same discipline as the cull mode
    // below -- the cache must describe the context, not intend to).
    installBlendState(r, mdk::Pass::Solid, mdk::Blend::Alpha);

    // (B) THE CULL ENUM: kSolidCull (01_types.inc), measured in the emulator:
    // visible faces arrive CLOCKWISE, so back-face culling is
    // GPU_CULL_FRONT_CCW. The earlier GPU_CULL_BACK_CCW culled the FRONT faces
    // and hid the arena interior on every level. Still owed: a hardware look
    // (AGENTS.md P6).
    C3D_CullFace(kSolidCull);
    // The cache describes the state actually installed above. The mode
    // is GLOBAL to the context, so the first eye's install already
    // covers the second; re-installing it per eye is belt and braces,
    // not the mechanism.
    r->cullMode = kSolidCull;
    // THE HAZE: off, every eye, whatever the last frame left (P2: every GPU state
    // and every uniform set explicitly). The uniform at 0 keeps every vertex's
    // whole colour, and combiner stage 2 passes the previous stage through. A
    // draw that takes the haze switches both on through the hazeOn cache
    // (drawRecorded's setHaze).
    C3D_FVUnifSet(GPU_VERTEX_SHADER, r->hazeLocation, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_TexEnvInit(C3D_GetTexEnv(2));
    r->hazeOn = false;
    // Stages 3-5 and the combiner buffer belong to the glow pass alone, which
    // resets them after itself; written here too, every eye, because an
    // unwritten PICA register is frame state nobody set (AGENTS.md P2).
    for (int stage = 3; stage <= 5; stage++) {
        C3D_TexEnvInit(C3D_GetTexEnv(stage));
    }
    C3D_TexEnvBufUpdate(C3D_Both, 0);
    // THE LIGHT, likewise: the unlit program bound (above), combiner stage 1 a
    // passthrough, and the world matrix the identity (the VS writes a world
    // position for every draw; only the lit program reads it).
    C3D_TexEnvInit(C3D_GetTexEnv(1));
    r->lightStageOn = false;
    r->litBound = false;
    {
        static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        C3D_Mtx worldMtx;
        mdk::pica::registers(kIdentity, worldMtx.m);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->worldLocation, &worldMtx);
        r->worldIsIdentity = true;
    }
    C3D_DepthMap(mdk::pica::kDepthMapIsZBuffer, mdk::pica::kDepthMapScale,
                 mdk::pica::kDepthMapOffset);

    // The viewport is a HARDWARE CLIP RECTANGLE. A zero-area viewport
    // SILENTLY DROPS the draw -- the "invisible 3D trap". The full target
    // is the safe default; the insets set their own.
    // The TARGET's size, which is the panel's at scale 1 and twice it per axis
    // under edge smoothing (buildEyeTargets).
    C3D_SetViewport(0, 0, r->targetWidth, r->targetHeight);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, r->targetWidth, r->targetHeight);
}

// The eye targets' scale follows enableEdgeSmoothing and edgeSmoothing2x2
// (buildEyeTargets). Called between frames only. A refused build steps down --
// 2x2 to 2x1 (half its VRAM), 2x1 to 1x1 -- and each refusal is latched until
// the setting that asked for it goes off (a full VRAM is one line, not a
// rebuild every frame); a 1x1 build that fails leaves no target to draw into,
// and the renderer stops (ready=false) rather than draw into a deleted one.
static void applyEdgeSmoothing(RendererC3D* r) {
    if (!r->toggles.enableEdgeSmoothing) {
        r->smoothingRefused = false;
    }
    if (!r->toggles.edgeSmoothing2x2) {
        r->smoothing2x2Refused = false;
    }
    // 2x1 by default, 2x2 when the config asks (edgeSmoothing2x2): measured on
    // the Old 3DS, 2x2 cost ~22 ms of an 80 ms stereo frame (2026-11-18), 2x1
    // ~12 (2026-11-19).
    const bool smooth = r->toggles.enableEdgeSmoothing && !r->smoothingRefused;
    const bool fine = smooth && r->toggles.edgeSmoothing2x2 && !r->smoothing2x2Refused;
    const int wantX = smooth ? 2 : 1;
    const int wantY = fine ? 2 : 1;
    if (wantX == r->targetScaleX && wantY == r->targetScaleY && r->targetLeft) {
        return;
    }
    releaseGlow(r);
    releaseShadowMap(r);
    releaseEyeTargets(r);
    if (buildEyeTargets(r, wantX, wantY)) {
        return;
    }
    if (fine) {
        r->smoothing2x2Refused = true;
        mdk::log3dsPrintf("smoothing: 2x2 REFUSED, trying 2x1");
        if (buildEyeTargets(r, 2, 1)) {
            return;
        }
    }
    if (smooth) {
        r->smoothingRefused = true;
        mdk::log3dsPrintf("smoothing: 2x1 REFUSED, back to 1x1");
        if (buildEyeTargets(r, 1, 1)) {
            return;
        }
    }
    mdk::log3dsPrintf("targets: no eye target could be built; the renderer stops");
    r->ready = false;
}

// A GPU BUFFER READ BACK INTO LINEAR MEMORY, OUTSIDE A FRAME, AND KNOWN DONE.
// C3D_SyncTextureCopy out of a frame ends in gspWaitForEvent(PPF, false), which
// does not clear the event first: a display transfer of the frame just ended
// that completed unobserved leaves it set, and the wait then returns before
// THIS copy has run (review, 2026-11-18; the emulator completes transfers at
// once and cannot show it). So the destination is filled with `fill` (the
// answer that means "nothing seen") and a second, 64-byte copy follows into a
// renderer-owned drain: the transfer engine runs in order, and the PPF event
// the second wait sees is at the earliest the first copy's own. The drain is
// freed only after C3D_Fini; a destination freed after this cannot still be
// the target of a transfer. False when the drain cannot be had (nothing read).
static bool readBack(RendererC3D* r, void* source, u32 rowDim, u32* dest, size_t bytes, u8 fill) {
    if (!r->copyDrain) {
        r->copyDrain = static_cast<u32*>(linearAlloc(64));
        if (!r->copyDrain) {
            return false;
        }
    }
    memset(dest, fill, bytes);
    GSPGPU_FlushDataCache(dest, bytes);
    C3D_SyncTextureCopy(static_cast<u32*>(source), rowDim, dest, rowDim, (u32)bytes,
                        GX_TRANSFER_RAW_COPY(1));
    const u32 drainDim = GX_BUFFER_DIM(64u >> 4, 0);
    C3D_SyncTextureCopy(static_cast<u32*>(source), drainDim, r->copyDrain, drainDim, 64,
                        GX_TRANSFER_RAW_COPY(1));
    GSPGPU_InvalidateDataCache(dest, bytes);
    return true;
}

// The sampler's half of testNativeShadowWrites: the receive program over the
// test's quad (the bound vertex buffer) into `probe`, every receiver at depth
// 0.75 at the map's centre (sunMtx rows (0,0,0,c), no normal offset, no bias),
// the combiner the sampler's answer alone: black where the map holds a nearer
// depth. A geometry program; the caller keeps bindProgram's rule.
static void testSampleShadowMap(RendererC3D* r, C3D_Tex* tex, C3D_RenderTarget* probe,
                                const C3D_Mtx* identity) {
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_FrameBufClear(&probe->frameBuf, C3D_CLEAR_COLOR, 0xFFFFFFFFu, 0);
    C3D_FrameDrawOn(probe);
    C3D_SetViewport(0, 0, 8, 8);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 8, 8);
    bindProgram(r, &r->programShadow);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->mvpLocation, identity);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->worldLocation, identity);
    r->worldIsIdentity = true;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, r->uvLocation, 1.0f, 1.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, r->tintLocation, 1.0f, 1.0f, 1.0f, 1.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, r->hazeLocation, 0.0f, 0.0f, 0.0f, 0.0f);
    r->tintIsWhite = true;
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowSunDirLocation, 0.0f, 0.0f, -1.0f, 0.0f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowSunColourLocation, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowSkyLocation, 1.0f, 1.0f, 1.0f, 0.0f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowGroundLocation, 1.0f, 1.0f, 1.0f, 0.0f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowCameraLocation, 0.0f, 0.0f, 1.0f, 1.0f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowParamsLocation, 1.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowSunMtxLocation + 0, 0.0f, 0.0f, 0.0f, 0.5f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowSunMtxLocation + 1, 0.0f, 0.0f, 0.0f, 0.5f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowSunMtxLocation + 2, 0.0f, 0.0f, 0.0f, 0.75f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowSunMtxLocation + 3, 0.0f, 0.0f, 0.0f, 1.0f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowLutMapLocation, 0.0f, 0.0f, 0.0f, 0.0f);
    bindTexUnit(r, 0, tex);
    C3D_TexShadowParams(false, 0.0f);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, 0xFFFFFFFFu);
    C3D_DrawArrays(GPU_GEOMETRY_PRIM, 0, 6);
}

// ============================================================================
// THE PICA'S OWN SHADOW WRITES, MEASURED (2026-11-18). The caster pass has two
// ways to make the map:
//   * NATIVE: GPU_FRAGOPMODE_SHADOW, the mode the PICA has for it -- every
//     caster fragment's 24-bit depth goes into the map (the nearer one kept).
//   * THE RAMP: an ordinary draw whose texture coordinates look the depth's
//     bytes up in a ramp texture (s = depth, t = 256 * depth, REPEAT).
// Each fails on one instrument, and both failures were measured:
//   * on the Old 3DS the ramp's t is not repeated that far: the map the console
//     wrote, read back byte for byte against the emulator's (level 8 frame 60),
//     has the depth's middle byte 0 in every texel, so its depth is good to
//     1/256 of the range (31 world units) and the sunlit floor shadowed itself;
//     native writes land and the shadow sampler reads them.
//   * in the emulator (Mandarine's OpenGL renderer on llvmpipe) the ramp is
//     exact, and native writes reach memory and even an 8x8 test's sampler,
//     yet the game's own frame showed no shadow from them at all.
// So the renderer measures both, once, before the first frame that wants a sun
// shadow, in a frame of its own, waited for:
//   * THE RAMP: one quad at a depth whose middle byte is 0x37 drawn through it,
//     read back -- the byte is there, or it is 0 (the console);
//   * NATIVE: an 8x8 map SAMPLED (testSampleShadowMap, still clear), WRITTEN
//     natively at depth 0.5, and SAMPLED AGAIN with every receiver behind it.
// The ramp where it is exact; native writes where it is not and they work
// (the console); the ramp otherwise, logged as neither. end_frame then reads
// the real pass's map back once more in native mode.
static void testNativeShadowWrites(RendererC3D* r) {
    // A test that could not RUN (no VRAM or linear memory for it this time) is
    // tried again, up to three times, before the ramp is taken for the session:
    // on the console the ramp is the path that cannot work.
    r->shadowNativeTestRuns++;
    r->shadowNativeTested = r->shadowNativeTestRuns >= 3;
    r->shadowNative = false;
    C3D_Tex tex;
    if (!C3D_TexInitShadow(&tex, 8, 8)) {
        mdk::log3dsPrintf("shadows: native-write test could not make its texture (try %d)",
                          (int)r->shadowNativeTestRuns);
        return;
    }
    C3D_RenderTarget* target = C3D_RenderTargetCreateFromTex(&tex, GPU_TEXFACE_2D, 0, -1);
    // ... and the RECEIVER: an 8x8 colour target the shadow sampler draws into.
    C3D_RenderTarget* probe = C3D_RenderTargetCreate(8, 8, GPU_RB_RGBA8, -1);
    // ... and the RAMP's: an 8x8 colour target one ramp quad is drawn into.
    C3D_RenderTarget* rampProbe = C3D_RenderTargetCreate(8, 8, GPU_RB_RGBA8, -1);
    float* quad = static_cast<float*>(linearAlloc(12 * 5 * sizeof(float)));
    u32* back = static_cast<u32*>(linearAlloc(8 * 8 * 4));
    u32* seen = static_cast<u32*>(linearAlloc(8 * 8 * 4));
    u32* rampSeen = static_cast<u32*>(linearAlloc(8 * 8 * 4));
    if (target && probe && rampProbe && quad && back && seen && rampSeen &&
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) {
        // Clip space through the identity: the whole target, z -0.5 -> depth 0.5
        // (pica::kDepthMapScale z + kDepthMapOffset).
        static const int kTestCorners = 6;
        static const int kRampTestMiddle = 0x37;
        static const float kCorners[kTestCorners][2] = {
            {-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
        // The second quad, for the ramp: a depth whose middle byte is 0x37 and
        // whose high byte is 0x80, half a step into its 1/65536 (no boundary near).
        const float rampDepth = (float)(0x80 * 256 + kRampTestMiddle) / 65536.0f + 0.5f / 65536.0f;
        for (int q = 0; q < 2; q++) {
            for (int c = 0; c < kTestCorners; c++) {
                float* corner = quad + (q * kTestCorners + c) * 5;
                corner[0] = kCorners[c][0];
                corner[1] = kCorners[c][1];
                corner[2] = q == 0 ? -0.5f : rampDepth - 1.0f;
                corner[3] = 0.0f;
                corner[4] = 0.0f;
            }
        }
        GSPGPU_FlushDataCache(quad, 12 * 5 * sizeof(float));
        C3D_FrameBufClear(&target->frameBuf, C3D_CLEAR_COLOR, 0xFFFFFFFFu, 0);
        C3D_Mtx identity;
        Mtx_Identity(&identity);
        C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
        AttrInfo_Init(attrInfo);
        AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);
        AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2);
        C3D_BufInfo* bufInfo = C3D_GetBufInfo();
        BufInfo_Init(bufInfo);
        BufInfo_Add(bufInfo, quad, 5 * sizeof(float), 2, 0x10);
        C3D_TexEnv* env = C3D_GetTexEnv(0);
        for (int stage = 1; stage <= 5; stage++) {
            C3D_TexEnvInit(C3D_GetTexEnv(stage));
        }
        C3D_TexEnvBufUpdate(C3D_Both, 0);
        r->hazeOn = false;
        r->lightStageOn = false;
        C3D_DepthMap(mdk::pica::kDepthMapIsZBuffer, mdk::pica::kDepthMapScale,
                     mdk::pica::kDepthMapOffset);
        installDepthState(r, mdk::pica::DepthState{false, false});
        installAlphaTest(r, -1, true);
        writeBlendState(r, mdk::pica::kBlendReplace, -3, -3);   // castShadows' own
        C3D_CullFace(GPU_CULL_NONE);
        r->cullMode = GPU_CULL_NONE;
        // THE RAMP, MEASURED: one quad through the caster program and the ramp,
        // as castShadows draws the map in ramp mode. Where t = 256 * depth is
        // repeated (the emulator) the texel's middle byte is 0x37; where it is
        // not (the Old 3DS: 0) the ramp cannot make a map.
        const bool rampMade = makeShadowRamp(r);
        if (rampMade) {
            C3D_FrameBufClear(&rampProbe->frameBuf, C3D_CLEAR_COLOR, 0, 0);
            C3D_FrameDrawOn(rampProbe);
            C3D_SetViewport(0, 0, 8, 8);
            C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 8, 8);
            bindProgram(r, &r->programCaster);
            r->litBound = false;
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->casterMvpLocation, &identity);
            bindTexUnit(r, 0, &r->shadowRamp);
            C3D_TexEnv* rampEnv = C3D_GetTexEnv(0);
            C3D_TexEnvInit(rampEnv);
            C3D_TexEnvSrc(rampEnv, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
            C3D_TexEnvFunc(rampEnv, C3D_Both, GPU_REPLACE);
            C3D_DrawArrays(GPU_TRIANGLES, kTestCorners, kTestCorners);
        }
        // FIRST, THE MAP IS SAMPLED while it still holds its clear, as the game's
        // map has been sampled by every frame before the one that writes it: an
        // instrument that caches a sampled texture and does not see later native
        // writes into it (the emulator: its own 8x8 test passed without this and
        // the game's map then showed no shadow) must fail HERE.
        testSampleShadowMap(r, &tex, probe, &identity);
        // THEN THE NATIVE WRITE (the caster program is plain: bindProgram splits
        // the list after the sampler's geometry program, the console's rule).
        C3D_FrameDrawOn(target);
        C3D_SetViewport(0, 0, 8, 8);
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 8, 8);
        bindProgram(r, &r->programCaster);
        r->litBound = false;
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->casterMvpLocation, &identity);
        // C3D_GetTexEnv again: it is what marks the stage dirty, and the sampler's
        // draw has uploaded (and cleaned) it since -- edits through the old
        // pointer would never reach the GPU, and the caster would run with the
        // sampler's combiner (measured on the console: green non-zero, the
        // penumbra path, softness 0xFE and no depth).
        env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_TexEnvColor(env, 0x00000000u);
        C3D_FragOpMode(GPU_FRAGOPMODE_SHADOW);
        C3D_FragOpShadow(0.0f, 1.0f);
        C3D_DrawArrays(GPU_TRIANGLES, 0, kTestCorners);
        C3D_FragOpMode(GPU_FRAGOPMODE_GL);
        // THE SAMPLER'S SIDE, in the same command list, as the game reads the map:
        // the receive program over the same quad, every receiver at depth 0.75 at
        // the map's centre (sunMtx rows (0,0,0,c), no normal offset, no bias), the
        // combiner the sampler's answer alone. Black = behind the 0.5 just written
        // = the sampler saw the native write. (The emulator writes natively, its
        // readback sees it -- and its sampler, in the same list, does not: level
        // 8 frame 60 then showed no shadow at all.) A geometry program is the last
        // thing in this list, so nothing plain follows it (bindProgram's rule).
        // AND THE SAMPLER AGAIN: now it must see the write.
        testSampleShadowMap(r, &tex, probe, &identity);
        C3D_FrameEnd(0);
        // The copies wait for the queue (the test's draws) to drain first.
        // Each pre-filled with the answer that means "did not work" (readBack):
        // unwritten (FF), not dark (FF), no middle byte (00).
        const u32 rowDim = GX_BUFFER_DIM((8u * 8u * 4u) >> 4, 0);
        const bool read = readBack(r, tex.data, rowDim, back, 8 * 8 * 4, 0xFF) &&
                          readBack(r, probe->frameBuf.colorBuf, rowDim, seen, 8 * 8 * 4, 0xFF) &&
                          readBack(r, rampProbe->frameBuf.colorBuf, rowDim, rampSeen, 8 * 8 * 4, 0x00);
        // The ramp texel's middle byte: memory byte 1 in the reversed order (the
        // map's [high, mid, low, softness]), byte 2 in the PICA's.
        const int midByte = shadowByteOrder(r) == mdk::pica::kShadowBytesReversed ? 1 : 2;
        int rampExact = 0;
        for (int i = 0; i < 64 && rampMade && read; i++) {
            const uint8_t* texel = reinterpret_cast<const uint8_t*>(&rampSeen[i]);
            if (texel[midByte] == kRampTestMiddle) {
                rampExact++;
            }
        }
        int moved = 0;
        int dark = 0;
        for (int i = 0; i < 64 && read; i++) {
            if (back[i] != 0xFFFFFFFFu) {
                moved++;
            }
            // RGBA8 in memory is [A, B, G, R]: the colour is the word's top 24 bits.
            if ((seen[i] & 0xFFFFFF00u) == 0) {
                dark++;
            }
        }
        r->shadowNativeTested = true;   // it ran: its answer stands
        // THE DECISION: the ramp where it is exact (its depths are the same 16
        // bits everywhere it works, and its own sampler reads them); native
        // writes where the ramp is not and they work; else the ramp, said so.
        const bool rampWorks = rampExact == 64;
        const bool nativeWorks = moved == 64 && dark == 64;
        r->shadowNative = !rampWorks && nativeWorks;
        mdk::log3dsPrintf("shadows: the ramp %s (%d of 64 texels carry the middle byte, first "
                          "%08lX); native writes %s (%d of 64 written, first %08lX; %d of 64 "
                          "receivers behind them dark, first %08lX); the caster pass uses %s",
                          rampWorks ? "WORKS" : "does not work", rampExact,
                          (unsigned long)rampSeen[0], nativeWorks ? "WORK" : "do not work", moved,
                          (unsigned long)back[0], dark, (unsigned long)seen[0],
                          r->shadowNative ? "native writes"
                                          : (rampWorks ? "the ramp"
                                                       : "the ramp (NEITHER path measured right)"));
        // A native session never draws through the ramp: its half megabyte back.
        // And a map an earlier, could-not-run try left behind was made for the
        // ramp (with a depth buffer the native writes were never measured with):
        // it goes too, and the next frame makes the native one (review,
        // 2026-11-18). Outside a frame here, as releaseShadowMap needs.
        if (r->shadowNative) {
            releaseShadowMap(r);
        }
        r->matrixIsFrame = false;
        r->hazeOn = false;
    } else {
        mdk::log3dsPrintf("shadows: native-write test could not run (try %d)",
                          (int)r->shadowNativeTestRuns);
    }
    if (rampSeen) {
        linearFree(rampSeen);
    }
    if (seen) {
        linearFree(seen);
    }
    if (back) {
        linearFree(back);
    }
    if (quad) {
        linearFree(quad);
    }
    if (rampProbe) {
        C3D_RenderTargetDelete(rampProbe);
    }
    if (probe) {
        C3D_RenderTargetDelete(probe);
    }
    if (target) {
        C3D_RenderTargetDelete(target);
    }
    // citro3d keeps the pointer it was bound with: unit 0 is unbound before the
    // test's texture (on this stack) goes, so nothing re-dirtied later (an APT
    // resume re-sends every unit) can read a dead C3D_Tex (review, 2026-11-18).
    // Every draw that samples unit 0 binds its own texture first.
    bindTexUnit(r, 0, nullptr);
    C3D_TexDelete(&tex);
}

// THE DEDUPE'S LINE (THE DEDUPE, above): per category, the writes made and the
// writes skipped over the window, and the command list's length in 32-bit words
// a frame -- what a DRAWDEDUPE=0 build is compared against. Prints and starts a
// new window. Cold: every 300 frames in the logging harness, once at teardown.
static void logDedupeWindow(RendererC3D* r) {
    if (r->cmdWordsFrames == 0) {
        return;
    }
    const int32_t* w = r->dedupeWritten;
    const int32_t* k = r->dedupeSkipped;
    mdk::log3dsPrintf("dedupe: frame %d (%s): skipped/written over %d frames: stage0 %d/%d "
                      "bufinfo %d/%d uv %d/%d tint %d/%d mvp %d/%d world %d/%d "
                      "texbind %d/%d; command list %lu words in all, %u in the "
                      "longest frame",
                      (int)r->frame, kDrawDedupe ? "on" : "OFF", (int)r->cmdWordsFrames,
                      (int)k[kDedupeTexEnv0], (int)w[kDedupeTexEnv0],
                      (int)k[kDedupeBufInfo], (int)w[kDedupeBufInfo],
                      (int)k[kDedupeUv], (int)w[kDedupeUv],
                      (int)k[kDedupeTint], (int)w[kDedupeTint],
                      (int)k[kDedupeMvp], (int)w[kDedupeMvp],
                      (int)k[kDedupeWorld], (int)w[kDedupeWorld],
                      (int)k[kDedupeTexBind], (int)w[kDedupeTexBind],
                      (unsigned long)r->cmdWordsSum, (unsigned)r->cmdWordsPeak);
    for (int c = 0; c < kDedupeCategories; c++) {
        r->dedupeWritten[c] = 0;
        r->dedupeSkipped[c] = 0;
    }
    r->cmdWordsSum = 0;
    r->cmdWordsPeak = 0;
    r->cmdWordsFrames = 0;
}

void mdk_render_begin_frame(MdkRenderer rr, const MdkFrameParams* params) {
    RendererC3D* r = asC3d(rr);
    if (!r || !r->ready) return;
    if (r->inFrame) return;   // a begin without an end is a bug, not a re-entry

    // EDGE SMOOTHING follows its toggle here, OUTSIDE the frame: the rebuild
    // deletes the eye targets, and C3D_RenderTargetDelete waits for the GPU queue
    // first (and breaks inside a frame), so nothing in flight still reads them.
    applyEdgeSmoothing(r);
    // A changed shadowMapSize (config.ini; the menu has no row for it) remakes
    // the map, here, between frames: the next frame that casts makes it at the
    // new size. Against the size ASKED for when the map was made (or refused),
    // not the one made: a map VRAM held to a smaller size would otherwise be
    // remade every frame. A refusal is forgotten too, so a smaller size is tried.
    if ((r->shadowMapReady || r->shadowRefused) &&
        r->toggles.shadowMapSize != r->shadowMapAsked) {
        releaseShadowMap(r);
        r->shadowMapAsked = r->toggles.shadowMapSize;
    }
    if (!r->ready) return;
    // The native shadow-write test, once, before the first frame that may cast
    // a sun shadow (testNativeShadowWrites): outside the frame, like the rebuild.
    if (!r->shadowNativeTested && r->toggles.enableSkyShadows && r->casterProgramReady &&
        r->shadowProgramReady) {
        testNativeShadowWrites(r);
    }

    // C3D_FrameBegin returns FALSE when it could not begin (with
    // C3D_FRAME_NONBLOCK). A failed begin does NOT authorise drawing.
    if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) {
        return;
    }
    r->inFrame = true;
    r->frame++;
    // The frustum belongs to an installed eye; a frame that has not installed
    // one yet has none (installEye sets it before each per-eye replay).
    r->frustumValid = false;
    // THE COMMAND LIST'S LENGTH (the dedupe's evidence): libctru's write
    // position, which only moves forward through the frame's splits
    // (GPUCMD_Split advances gpuCmdBuf by what it hands over).
    r->cmdFrameStart = gpuCmdBuf + gpuCmdBufOffset;
    // A new command list: nothing of this frame has used a geometry program.
    r->gsInList = false;
    r->geometrySplits = 0;
    r->drawCount = 0;
    r->triangleCount = 0;
    // A capture request: counted here, honoured at this frame's end
    // (writeScreenshotBmp below) with a readback of the left eye target.
    if (params && params->screenshot_path && *params->screenshot_path) {
        r->capturesRequested++;
        snprintf(r->pendingScreenshot, sizeof(r->pendingScreenshot), "%s",
                 params->screenshot_path);
    }
    // The one frame that gets censused (see `Census`). Everything else pays the
    // compare and nothing more.
    r->census.active = mdk::log3dsPeriodic() && r->frame == kCensusFrame;
#if defined(MDK_HW_PROFILE)
    // Corridor fill-vs-batch samples (HANDOFF_MUSE.md task 1): DANT_1-only
    // (765), pair corridor (785/805/815), CDANT_1-only after the drop (845).
    // Route-specific to the FIXED=1 level-7 walk (HANDOFF_MUSE.md sec.2); a
    // different route must re-derive the frames. PROFILE builds silence the
    // periodic census, so arm it explicitly here; the per-triangle loop
    // perturbs these frames, so only their COUNTS are evidence, never their
    // periods (timing comes from uncensused runs).
    if (r->frame == 765 || r->frame == 785 || r->frame == 805 || r->frame == 815 ||
        r->frame == 845) {
        r->census.active = true;
    }
#endif
    r->census.triangles = 0;
    r->census.culled = 0;
    r->census.behind = 0;
    r->census.offscreen = 0;
    r->census.onscreen = 0;
    r->census.depthOut = 0;
    r->census.cw = 0;
    r->census.ccw = 0;
    r->census.near60 = 0;
    r->census.areaSum = 0.0f;
    r->census.loX = r->census.nLoX = 1e30f;
    r->census.hiX = r->census.nHiX = -1e30f;
    r->census.loY = r->census.nLoY = 1e30f;
    r->census.hiY = r->census.nHiY = -1e30f;
    r->census.bestW = -1.0f;
    r->census.bestDraw = -1;
    // C3D_FrameBegin waited for the previous queue above. The old frame's
    // GPU reads are complete, so deferred releases and replacements can be
    // made now (AGENTS.md P3).
    r->gpuPending = false;
    // THE FRAME'S BOUNDARY FOR THE EVICTION. A texture stamped at or after
    // this has been used by THIS frame's commands, and the GPU may still be
    // reading it, so it cannot be freed (AGENTS.md P3).
    // The frame that just ENDED drew stamps from the old boundary on: the HD
    // pump keeps those (the view's working set) and imports within the rest.
    const uint32_t previousFrameStart = r->frameStartUse;
    r->frameStartUse = r->drawCounter + 1;
    settleDeferredTextures(r);
    settleDeferredMeshes(r);
    pumpExpansions(r);
    // The orig pack's reads before the HD stream's: a packed original is a
    // surface about to draw, an HD copy only replaces one that already does.
    pumpPackOriginals(r, previousFrameStart);
    pumpHdTextures(r, previousFrameStart);
    // The stream's state, every 300 frames once it has done anything: the
    // evidence a budget is read from (docs/HD_TEXTURES_3DS_SPEC.md 4.2).
    if (mdk::log3dsPeriodic() && (r->frame % 300) == 0 &&
        (r->hdImported > 0 || !r->hdQueue.empty())) {
        mdk::log3dsPrintf("hd: frame %d: %d imported, %u bytes resident of %u, %d evicted, "
                          "%d failed, %d queued, %d reading",
                          (int)r->frame, (int)r->hdImported, (unsigned)r->hdResidentBytes,
                          (unsigned)kHdBudgetBytes, (int)r->hdEvicted, (int)r->hdFailedCount,
                          (int)r->hdQueue.size(),
                          (int)r->hdReader.busy(mdk::AsyncFileReader::kChannelHd));
    }
    // The orig pack's reads on the same clock: imported at the boundary, made by
    // a draw that could not wait (the number to keep near zero), failed.
    if (mdk::log3dsPeriodic() && (r->frame % 300) == 0 &&
        (r->packImported > 0 || r->packSync > 0 || r->packLanded > 0 ||
         !r->packQueue.empty())) {
        mdk::log3dsPrintf("pack: frame %d: %d imported, %d read by a draw, %d landed for a "
                          "draw, %d failed, %d queued, %d reading",
                          (int)r->frame, (int)r->packImported, (int)r->packSync,
                          (int)r->packLanded, (int)r->packFailed, (int)r->packQueue.size(),
                          (int)r->hdReader.busy(mdk::AsyncFileReader::kChannelOrig));
    }
    // THE STORES THE TEXTURES WERE MADE IN (EnhancedToggles::texture16): once a
    // level's textures are made (frame 30), then every 300 frames when more were
    // made since (an arena streamed in, an evicted texture made again). The
    // makings are counted, so a texture made twice counts twice; the resident
    // half is a walk of the table (a few hundred entries, at most once per 300
    // frames). Cold: this is the A/B's evidence that the store changed at all.
    {
        const int32_t made = r->texturesMade[0] + r->texturesMade[1] + r->texturesMade[2] +
                             r->texturesMade[3] + r->compressedMade;
        // Frame 30 once in every build (the A/B evidence, one write); every 300
        // frames only where the log is periodic (no SD write inside play).
        if ((r->frame == 30 || (mdk::log3dsPeriodic() && (r->frame % 300) == 0)) &&
            made != r->texturesMadeLogged) {
            r->texturesMadeLogged = made;
            int32_t resident16 = 0;
            int32_t resident = 0;
            size_t residentBytes = 0;
            size_t residentRgba8Bytes = 0;
            for (size_t i = 0; i < r->textures.size(); i++) {
                const C3dTexture& t = r->textures[i];
                if (!t.valid) {
                    continue;
                }
                resident++;
                if (t.format != mdk::pica::TexelFormat::Rgba8) {
                    resident16++;
                }
                residentBytes += mdk::pica::textureBytes(t.format, t.paddedWidth, t.paddedHeight,
                                                         t.mipLevels);
                residentRgba8Bytes += mdk::pica::textureBytes(
                    mdk::pica::TexelFormat::Rgba8, t.paddedWidth, t.paddedHeight, t.mipLevels);
            }
            mdk::log3dsPrintf("textures: frame %d texture16=%d: made %d rgb565, %d rgba5551, "
                              "%d rgba4, %d rgba8, %u bytes saved vs rgba8; kurtCompressed=%d: "
                              "%d etc imported, %u bytes saved; resident %d (%d "
                              "not rgba8) %u bytes of %u as rgba8",
                              (int)r->frame, (int)r->toggles.texture16, (int)r->texturesMade[0],
                              (int)r->texturesMade[1], (int)r->texturesMade[2],
                              (int)r->texturesMade[3], (unsigned)r->textureBytesSaved,
                              (int)r->toggles.kurtCompressed, (int)r->compressedMade,
                              (unsigned)r->compressedBytesSaved,
                              (int)resident, (int)resident16, (unsigned)residentBytes,
                              (unsigned)residentRgba8Bytes);
        }
    }
    r->queue.reset();

    // THE CLEAR COLOUR IS CONVERTED, NOT PASSED THROUGH. The seam carries
    // 0xAARRGGBB (what the Vulkan backend unpacks, and what both frontends
    // therefore write); the PICA's fill pattern is 0xRRGGBBAA. Handing the seam
    // value to C3D_RenderTargetClear directly painted the panel with the alpha
    // byte as red — measured, 0xFF102030 became #ff1020. See pica::clearColour.
    const uint32_t clear =
        mdk::pica::clearColour(params ? params->clear_colour : 0xFF000000u);

    // THE EYE PLAN, read once, here, and stored on the renderer.
    //
    // The eye passes happen at the END of the frame -- see mdk_render_end_frame:
    // the draws are queued between begin and end, so an eye installed here is
    // installed before there is anything to draw into it, and every draw then
    // lands in whichever eye was installed last. Installing both here, as this
    // function used to, meant they all landed in the RIGHT target while the top
    // screen shows the LEFT one: a frame of nothing but the clear colour, on a
    // backend nobody can look at from this host.
    //
    // The slider must still be read here and not at the end, so that the two
    // eyes cannot disagree if the user moves it mid-frame.
    //
    // THE SLIDER MUST BE CLAMPED. Mandarine returns a garbage value (~134
    // million) when it does not model the slider, and Forsaken shipped a bug
    // from exactly that: an unclamped depth became an eye separation of
    // billions of units and the screen went black. The clamp, the sign of each
    // eye and the mono skip are pica::eyePlan, which a host test pins.
    //
    // A harness build may force mono (`make AUTOQUIT=90 MONO=1`): the slider
    // reads as 0, so one pass, for the owner's stereo-cost A/B on a console
    // whose slider nobody is there to move.
#if defined(MDK_AUTOQUIT) && defined(MDK_HW_MONO)
    const float slider = 0.0f;
#else
    const float slider = (float)osGet3DSliderState();
#endif
    r->eyes = mdk::pica::eyePlan(slider, r->eyeSeparation);

    // Both eye buffers are cleared here, whether or not both get drawn: a buffer
    // that is not cleared keeps the previous frame's picture, and the right
    // buffer starts being displayed the moment the slider goes up.
    //
    // The depth is cleared to the BUFFER'S MAXIMUM.
    //
    // The frame's depth map stores 0 at the near plane and 1 at the far, so
    // the value that accepts every fragment before one is drawn is the top
    // of the buffer, not the bottom. The devkitPro examples clear to 0 and
    // that is correct for them: citro3d's own default map is the reversed
    // range (see mdk::pica::kDepthMapScale for both pairings). Clearing to
    // 0 with this map rejects everything, which is a screen showing only the
    // clear colour and no error.
    //
    // The value is the raw fill pattern GX_MemoryFill writes into the
    // DEPTH24_STENCIL8 buffer (measured: C3D_FrameBufClear passes
    // `clearDepth` straight to GX_MemoryFill as the fill value), and
    // 0x00FFFFFF is the 24-bit maximum under either byte order of the word.
    //
    // THE BACKDROP KEEP (the scripts' sky mode -1, e.g. level 3's closed HMO_3; C#
    // Backdrop.Keep loads the scene target instead of clearing it; package #15): each
    // eye's colour buffer keeps the last frame, so only the depth is cleared. The right
    // eye then also keeps its own last picture, which is the one it showed.
    const bool keepColour = params && params->backdrop == mdk::Backdrop::Keep;
    C3D_RenderTargetClear(r->targetLeft, keepColour ? C3D_CLEAR_DEPTH : C3D_CLEAR_ALL, clear,
                          mdk::pica::kDepthClearValue);
    // At edge-smoothing scale 2 the right eye's depth is the LEFT eye's buffer,
    // just filled above and filled again before the right eye draws (installEye),
    // so only its colour is cleared here: a third 1.5 MB depth fill is wasted fill
    // bandwidth on the Old 3DS. (Under Keep there is then nothing to clear on it.)
    if (!(keepColour && r->targetsShareDepth)) {
        C3D_RenderTargetClear(r->targetRight,
                              keepColour ? C3D_CLEAR_DEPTH
                                         : (r->targetsShareDepth ? C3D_CLEAR_COLOR : C3D_CLEAR_ALL),
                              clear, mdk::pica::kDepthClearValue);
    }
}

// Report one censused frame. Cold: called once per process, from the end of the
// first eye's replay of frame `kCensusFrame`.
static void logCensus(RendererC3D* r) {
    const Census& c = r->census;
    mdk::log3dsPrintf("census frame=%d draws=%d culled=%d tris=%d behind=%d offscreen=%d onscreen=%d depthout=%d near60=%d cw=%d ccw=%d w=[%.1f,%.1f] areaset=%.4f",
                      r->frame, (int)r->drawCount, (int)c.culled, (int)c.triangles, (int)c.behind,
                      (int)c.offscreen, (int)c.onscreen, (int)c.depthOut, (int)c.near60,
                      (int)c.cw, (int)c.ccw, c.minW,
                      c.maxW, c.areaSum);
    if (c.bestDraw >= 0) {
        mdk::log3dsPrintf("census nearest draw=%d first=%d w=%.1f area=%+.8f v0=(%.3f,%.3f,%.3f) v1=(%.3f,%.3f,%.3f) v2=(%.3f,%.3f,%.3f)",
                          (int)c.bestDraw, (int)c.bestFirst, c.bestW, c.bestArea,
                          c.bestV[0], c.bestV[1], c.bestV[2], c.bestV[3],
                          c.bestV[4], c.bestV[5], c.bestV[6], c.bestV[7],
                          c.bestV[8]);
        mdk::log3dsPrintf("census nearest ndc v0=(%.5f,%.5f) v1=(%.5f,%.5f)",
                          c.bestNdc[0], c.bestNdc[1], c.bestNdc[2], c.bestNdc[3]);
    }
    {
        // The ndc bbox is the mapping probe: the panel's painted rectangle has to
        // be this box, or the framebuffer is not showing what the matrix produced.
        //
        // IT PRINTS THE PIXEL BOX NOW, BECAUSE THE TEST WAS NEVER APPLIED (review-1
        // F2). Batch 78 logged the ndc box, said "or the framebuffer is not
        // showing what the matrix produced", and then moved on without applying it.
        // Applied, it FAILS: the box maps outside the target on three sides, so it
        // covers the whole panel, while the panel was 61% clear colour. That is not
        // a contradiction — the box is the UNION of every triangle's bbox with no
        // per-pixel coverage and no depth test in it, so "the union covers the
        // panel" and "the panel is mostly clear" are both true and neither says
        // where the geometry is. The probe is kept because a box that landed wholly
        // INSIDE the panel while the panel stayed clear would be a real finding; a
        // box that overflows it proves only that the union is large.
        //
        // THE SPACE IS STATED, because a number without one is how batch 78 ended
        // up reporting a "407x9 strip" in a 400-pixel-wide panel (review-1 F6).
        // The target is 240x400 PORTRAIT; the capture's 400x240 panel is that
        // transposed, so a pixel box here is not directly a pixel box there.
        const Census& c = r->census;
        const float pxLo = (c.loX + 1.0f) * 0.5f * (float)kScreenWidth;
        const float pxHi = (c.hiX + 1.0f) * 0.5f * (float)kScreenWidth;
        const float pyLo = (1.0f - c.hiY) * 0.5f * (float)kScreenHeight;
        const float pyHi = (1.0f - c.loY) * 0.5f * (float)kScreenHeight;
        mdk::log3dsPrintf("census ndc box=[%.3f,%.3f]x[%.3f,%.3f] near60 box=[%.3f,%.3f]x[%.3f,%.3f]",
                          c.loX, c.hiX, c.loY, c.hiY, c.nLoX, c.nHiX,
                          c.nLoY, c.nHiY);
        mdk::log3dsPrintf("census mapping: target px x=[%.1f,%.1f] y=[%.1f,%.1f] of %dx%d portrait"
                          " (panel capture is this transposed); covers=%s",
                          pxLo, pxHi, pyLo, pyHi, kScreenWidth, kScreenHeight,
                          (pxLo < 0.0f && pxHi > (float)kScreenWidth &&
                           pyLo < 0.0f && pyHi > (float)kScreenHeight)
                              ? "whole target"
                              : "part of the target");
    }
}

// One framebuffer readback: the left eye target (240x400 portrait RGBA8)
// GX-transferred into linear memory, rotated into the panel's 400x240
// landscape, and written as a 24-bit BMP. The transfer is queued after
// the frame's draws (the GPU runs it in order), and the PPF wait makes
// the pixels final before the CPU reads them. A capture is a diagnostic
// path, never frame-critical: any failure is a refusal line and no file.
static bool writeScreenshotBmp(RendererC3D* r, C3D_RenderTarget* target, const char* path) {
    if (!target || !target->frameBuf.colorBuf) {
        return false;
    }
    // The capture is the PANEL's picture: at edge-smoothing scale 2 the same
    // 2x2-averaging transfer the display gets brings the 480x800 target down to
    // 240x400, so the file shows exactly what the screen shows.
    const int32_t srcW = kScreenWidth;    // 240, portrait width
    const int32_t srcH = kScreenHeight;   // 400, portrait height
    const int32_t scaleX = r->targetScaleX;
    const int32_t scaleY = r->targetScaleY;
    const size_t bytes = (size_t)srcW * (size_t)srcH * 4;
    u32* pixels = (u32*)linearAlloc(bytes);
    if (!pixels) {
        mdk::log3dsPrintf("screenshot: linearAlloc(%u) FAILED for %s", (unsigned)bytes, path);
        return false;
    }
    const u32 flags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
                      GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                      GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                      transferScaling(scaleX, scaleY);
    // BOTH dimensions are the INPUT's, as citro3d's own display output passes
    // them: the transfer engine halves the output dimension itself when it
    // scales (measured: passing the panel's 240x400 as the output read a
    // quarter of the target, unscaled, twice).
    const u32 dim = GX_BUFFER_DIM(srcW * scaleX, srcH * scaleY);
    // SYNCHRONOUS: C3D_SyncDisplayTransfer waits for citro3d's queue to drain
    // (the frame's draws and the panel transfers), THEN runs this transfer and
    // waits for ITS completion. A bare GX_DisplayTransfer + PPF wait here could be
    // satisfied by the queue's own panel transfer firing first and read the
    // buffer early -- on hardware, not in the emulator, which finishes transfers
    // at once (review, 2026-11-18: four times likelier at scale 2, and a stereo
    // capture now makes two). The flush keeps no dirty line over the buffer.
    GSPGPU_FlushDataCache(pixels, bytes);
    C3D_SyncDisplayTransfer((u32*)target->frameBuf.colorBuf, dim, pixels, dim, flags);
    GSPGPU_InvalidateDataCache(pixels, bytes);

    FILE* file = fopen(path, "wb");
    if (!file) {
        mdk::log3dsPrintf("screenshot: cannot open %s", path);
        linearFree(pixels);
        return false;
    }
    // 24-bit BMP, bottom-up. The panel image is the portrait buffer
    // rotated: landscape (x, y) reads portrait (srcW-1-y, x). The PICA's
    // RGBA8 texel in memory is [A, B, G, R], so BMP's B, G, R are the
    // source bytes 1, 2, 3.
    const int32_t outW = srcH;  // 400
    const int32_t outH = srcW;  // 240
    const int32_t rowBytes = outW * 3;
    const int32_t imageBytes = rowBytes * outH;
    uint8_t header[54] = {};
    header[0] = 'B';
    header[1] = 'M';
    const uint32_t fileSize = 54 + (uint32_t)imageBytes;
    memcpy(header + 2, &fileSize, 4);
    const uint32_t dataOffset = 54;
    memcpy(header + 10, &dataOffset, 4);
    const uint32_t infoSize = 40;
    memcpy(header + 14, &infoSize, 4);
    memcpy(header + 18, &outW, 4);
    memcpy(header + 22, &outH, 4);
    header[26] = 1;
    header[28] = 24;
    memcpy(header + 34, &imageBytes, 4);
    // ONE fwrite PER ROW, not per pixel. On the console the sdmc devoptab
    // turns small writes into FS calls, and 96,000 three-byte fwrites took
    // ~69 s on old3ds-lan167 (2026-11-17: frame 600 reached at 21 s of game
    // time, the capture's frame 601 at the 90 s autoquit). A 1200-byte row
    // on the main thread's stack is the same bytes in the same order.
    const uint8_t* src = (const uint8_t*)pixels;
    uint8_t row[kScreenHeight * 3];
    if ((size_t)rowBytes > sizeof(row)) {
        mdk::log3dsPrintf("screenshot: row %d B exceeds its %u B buffer for %s",
                          (int)rowBytes, (unsigned)sizeof(row), path);
        fclose(file);
        linearFree(pixels);
        return false;
    }
    bool written = fwrite(header, 1, sizeof(header), file) == sizeof(header);
    for (int32_t y = outH - 1; y >= 0 && written; y--) {
        for (int32_t x = 0; x < outW; x++) {
            const int32_t sx = srcW - 1 - y;
            const int32_t sy = x;
            const uint8_t* px = src + ((size_t)sy * (size_t)srcW + (size_t)sx) * 4;
            row[x * 3 + 0] = px[1];
            row[x * 3 + 1] = px[2];
            row[x * 3 + 2] = px[3];
        }
        written = fwrite(row, 1, (size_t)rowBytes, file) == (size_t)rowBytes;
    }
    written = (fclose(file) == 0) && written;
    linearFree(pixels);
    if (!written) {
        // No "wrote" line: the harness treats a capture as real only when
        // this session logged a completed write.
        mdk::log3dsPrintf("screenshot: write FAILED for %s", path);
        return false;
    }
    mdk::log3dsPrintf("screenshot: wrote %s (%dx%d)", path, (int)outW, (int)outH);
    return true;
}

 void mdk_render_end_frame(MdkRenderer rr) {
    RendererC3D* r = asC3d(rr);
    if (!r || !r->inFrame) return;
    // THE PER-EYE REPLAY, and it has to happen HERE rather than in
    // mdk_render_begin_frame: the draws are queued between the two, so this is
    // the first moment there is anything to draw, and the last one before the
    // queue is submitted.
    //
    // One pass per eye, each with its own target and matrix installed
    // immediately before its own replay. eyePlan decides how many there are:
    // ONE when the 3D slider is closed, because the panel then shows the left
    // buffer and drawing the right one is invisible work on a 268 MHz ARM11 --
    // Forsaken measures the same cost on the same hardware ("two full render
    // passes ~ 2x CPU+GPU cost ... Slider = 0 disables stereo and restores full
    // framerate", 3DS_PORT_NOTES.md).
    //
    // The queue is reset after the LAST eye, not inside the loop: the second eye
    // replays the same records.
    // The diagnostic is drawn INSIDE the loop, once per eye, so each eye gets
    // its own copy in its own target: drawn once after the loop it would be a
    // triangle in one eye and a hole in the other.
    // THE LOOK DIALS (config / the pause menu's sliders) over the game's rig,
    // every frame before anything reads it: the haze exponent, the lit and
    // shadow programs' uniforms and the shadow tint below all take the dialled
    // rig. At the defaults (100) it is the rig, copied.
    mdk::pica::dialledLighting(r->rig, r->toggles, &r->lighting);
    prepareHaze(r);
    prepareLighting(r);
    // The sun's shadow map (Enhanced Mode enableSkyShadows), once per frame and
    // before either eye: both eyes read the one map.
    prepareShadows(r);
    castShadows(r);
    // The glow (Enhanced Mode enableGlowBillboards) follows the enhanced rig, as
    // the PC's post pass does; a session whose glow memory failed stays off.
    // Billboards by default (the owner's 2026-11-18 decision); the PC's bloom,
    // ported, with config glowBloom.
    r->glowLive = r->toggles.enableGlowBillboards && r->toggles.glowBloom &&
                  r->lighting.enabled && !r->glowRefused;
    r->billboardsLive = r->toggles.enableGlowBillboards && !r->toggles.glowBloom &&
                        r->lighting.enabled;
    r->billboardsDrawn = 0;
    // Resolve the queue once for both eyes (STEREO-REPLAY analogue,
    // 06_draw.inc prepareQueue): the eye loop below only culls, composes
    // matrices and submits.
    // A frame that draws no eye resolves nothing (validator F4; eyePlan
    // only yields 1-2 today, so this is the guard, not a behaviour change).
    if (r->eyes.passes > 0) {
#ifdef MDK_HW_PROFILE
        const uint64_t prepT0 = svcGetSystemTick();
        prepareQueue(r);
        r->prepareTicks += svcGetSystemTick() - prepT0;
#else
        prepareQueue(r);
#endif
    }
    for (int eye = 0; eye < r->eyes.passes; eye++) {
        installEye(r, eye == 0 ? r->targetLeft : r->targetRight, r->eyes.offset[eye]);
#ifdef MDK_HW_PROFILE
        // Per-eye replay account (2026-11-22): sizes what a prepared-draw
        // replay could save -- the second eye's share of replayQueue. Ticks
        // only; the print rides the gputime cadence below FrameEnd.
        const uint64_t eyeT0 = svcGetSystemTick();
#endif
        replayQueue(r);
#ifdef MDK_HW_PROFILE
        r->eyeTicks[eye & 1] += svcGetSystemTick() - eyeT0;
        if (eye == 0) r->eyeFrames++;
#endif
        if (eye == 0 && r->shadowLive && mdk::log3dsPeriodic() &&
            (r->frame == 30 || (r->frame % 600) == 0)) {
            mdk::log3dsPrintf("shadows: frame %lu: %d caster draws into the map at its last cast (%d more left "
                              "out: their textures have holes), %d shadowed lit draws (both eyes); "
                              "%d frames reused the map since the last line, %d casters culled by the sun frustum",
                              (unsigned long)r->frame, (int)r->shadowCastersLast,
                              (int)r->shadowHoleCastersLast, (int)r->shadowReceivers,
                              (int)r->shadowReuses, (int)r->shadowCastersCulled);
            r->shadowReuses = 0;
        }
        // The census is read after the FIRST eye's replay: the second eye draws
        // the same records and would double every count.
        if (r->census.active) {
            logCensus(r);
            r->census.active = false;
        }
        // The canvas's own width, because the canary is drawn in the canvas's own
        // pixel space and that space is what `canvasMatrix` projects.
        // drawDiagnostic returns at once unless the canary is armed by a file
        // (canaryArmed): the eye loop's statements stay unconditional.
        drawDiagnostic(r, mdk_render_canvas_width(rr));
    }
    // THE CANVAS LINE, and it is the evidence `mdk_3ds/tools/emurun.sh --expect
    // hud` reads. Why a log line and not a pixel: the HUD's own colours are white,
    // grey and black, so a screenshot cannot tell "the HUD drew" from "the HUD was
    // queued and dropped", and the two numbers on this line can -- `quads` is what
    // the frontend asked for and `draws` is what the per-eye replay actually
    // emitted, so a canvas that was queued but never drawn, or drawn into one eye
    // only, is a different pair of numbers and not the same picture.
    //
    // On the first frame and then every sixtieth, which is the cadence the
    // frontend's own frame line uses: a line every frame is a line nobody reads,
    // and a line once is a line that cannot show the HUD disappearing mid-run.
    //
    // IT PRINTS EVEN WHEN THE ANSWER IS ZERO, and that is the whole point of the
    // line being evidence (review-9 F3). `emurun.sh --expect hud` reads the LAST
    // line, on the argument that the last line is the one that says the HUD was
    // still drawing when the run ended. A line printed only when quads > 0 cannot
    // honour that: a HUD that stops mid-run simply stops printing, and the last
    // good line keeps the gate green. Zero has to be sayable for the gate to be
    // able to say it.
    {
        r->canvasFrames++;
        if (r->canvasFrames == 1 || (mdk::log3dsPeriodic() && (r->canvasFrames % 60) == 0)) {
            mdk::log3dsPrintf("canvas: quads=%d draws=%d mesh=%d corners=%d eyes=%d "
                              "(canvas frame %d)",
                              (int)r->canvasQuadsThisFrame, (int)r->canvasDrawsThisFrame,
                              (int)r->canvasMesh, (int)r->canvasVerts.size(),
                              (int)r->eyes.passes, (int)r->canvasFrames);
        }
    }
    // The lit program's switches (each re-sends the dirty uniforms of both
    // shaders and its own six): the number a draw sort by program would be
    // judged by on hardware (review of the lighting batch).
    if (r->litLive && mdk::log3dsPeriodic() && (r->frame % 300) == 0) {
        mdk::log3dsPrintf("light: frame %d: %d program switches, %d geometry splits (%d eyes)",
                          (int)r->frame, (int)r->programSwitches, (int)r->geometrySplits,
                          (int)r->eyes.passes);
    }
    r->programSwitches = 0;
    if (r->glowLive && mdk::log3dsPeriodic() && (r->frame % 300) == 0) {
        mdk::log3dsPrintf("glow (bloom): frame %d: %d passes (%d eyes, scale %dx%d)", (int)r->frame,
                          (int)r->glowPasses, (int)r->eyes.passes, (int)r->targetScaleX,
                          (int)r->targetScaleY);
    }
    if (r->billboardsLive) {
        if (r->glowQuadCount > r->billboardsPeak) {
            r->billboardsPeak = r->glowQuadCount;
        }
        if (mdk::log3dsPeriodic() && (r->frame % 300) == 0) {
            mdk::log3dsPrintf("glow (billboards): frame %d: %d halos now (%d drawn, both eyes), "
                              "at most %d a frame since the last line, %d dropped past the cap",
                              (int)r->frame, (int)r->glowQuadCount, (int)r->billboardsDrawn,
                              (int)r->billboardsPeak, (int)r->glowQuadsDropped);
            r->billboardsPeak = 0;
            r->glowQuadsDropped = 0;
        }
    }
    if (mdk::log3dsPeriodic() && (r->frame % 300) == 0) {
        mdk::log3dsPrintf("programs: frame %d: %d lit, %d lit+shadow, %d pass-through, %d plain "
                          "draws (both eyes)",
                          (int)r->frame, (int)r->drawsLit, (int)r->drawsShadowLit,
                          (int)r->drawsPass, (int)r->drawsPlain);
    }
    r->drawsLit = r->drawsShadowLit = r->drawsPass = r->drawsPlain = 0;
    // THE DEDUPE'S LINE (03_frame.inc, THE DEDUPE): per category, the writes
    // made and the writes skipped over the window, and the command list's length
    // in 32-bit words a frame (C3D_FrameBegin to here, every split included) --
    // what a DRAWDEDUPE=0 build is compared against.
    {
        const u32* now = gpuCmdBuf + gpuCmdBufOffset;
        if (r->cmdFrameStart && now >= r->cmdFrameStart) {
            const uint32_t words = (uint32_t)(now - r->cmdFrameStart);
            r->cmdWordsSum += words;
            r->cmdWordsPeak = words > r->cmdWordsPeak ? words : r->cmdWordsPeak;
            r->cmdWordsFrames++;
        }
        // Every 300 frames only where the log is periodic (harness builds without
        // PROFILE): an SD write inside a play or profile frame costs 25-30 ms --
        // measured 2026-11-19 as the spikes at frames 600 and 1200 of a profile
        // run (the same defect class as 4cde286). Every build still prints the
        // last window once at teardown (mdk_render_destroy).
        if (mdk::log3dsPeriodic() && (r->frame % 300) == 0) {
            logDedupeWindow(r);
        }
    }
    // The halos are this frame's: a frame whose game hands none over (no
    // runtime) must not draw the last frame's again (review, 2026-11-19).
    r->glowQuadCount = 0;
    r->glowPasses = 0;
    r->canvasQuadsThisFrame = 0;
    r->canvasDrawsThisFrame = 0;
    r->queue.reset();
    r->insetOverDraws.clear();
    r->insetOpen = false;
    // C3D_FrameEnd is the ONLY present path. It triggers the auto display
    // transfer set up by C3D_RenderTargetSetOutput.
    C3D_FrameEnd(0);
#ifdef MDK_HW_PROFILE
    // The GPU's own account of the frame just submitted (citro3d's PICA
    // timestamps): the port has never measured GPU time -- every span in
    // the profile table is CPU. Sampled, not logged, here; the harness
    // window at frame 300 boundaries prints the mean.
    {
        static float gpuDrawSum = 0.0f;
        static float gpuProcSum = 0.0f;
        static int32_t gpuSamples = 0;
        gpuDrawSum += C3D_GetDrawingTime();
        gpuProcSum += C3D_GetProcessingTime();
        gpuSamples++;
        if ((r->frame % 300) == 0 && r->eyeFrames > 0) {
            const double ticksPerMs = (double)SYSCLOCK_ARM11 / 1000.0;
            mdk::log3dsPrintf("eyetime: frame %d mean prepare %.2f eye0 %.2f eye1 %.2f ms over %d frames",
                              (int)r->frame,
                              (double)r->prepareTicks / ticksPerMs / (double)r->eyeFrames,
                              (double)r->eyeTicks[0] / ticksPerMs / (double)r->eyeFrames,
                              (double)r->eyeTicks[1] / ticksPerMs / (double)r->eyeFrames,
                              (int)r->eyeFrames);
            r->eyeTicks[0] = r->eyeTicks[1] = 0;
            r->prepareTicks = 0;
            r->eyeFrames = 0;
            mdk::log3dsPrintf("coalesce: frame %d merged %llu of %llu prepared draws",
                              (int)r->frame, (unsigned long long)r->coalescePairs,
                              (unsigned long long)r->coalesceTotal);
            r->coalescePairs = 0;
            r->coalesceTotal = 0;
        }
        if ((r->frame % 300) == 0 && gpuSamples > 0) {
            mdk::log3dsPrintf("gputime: frame %d mean drawing %.2f processing %.2f ms over %d frames",
                              (int)r->frame, (double)(gpuDrawSum / (float)gpuSamples),
                              (double)(gpuProcSum / (float)gpuSamples), (int)gpuSamples);
            gpuDrawSum = 0.0f;
            gpuProcSum = 0.0f;
            gpuSamples = 0;
        }
    }
#endif
    r->inFrame = false;
    // THE NATIVE WRITES, SEEN ON THE REAL PASS, ONCE. testNativeShadowWrites
    // decides from 8x8 quads; the emulator's native writes pass that and land
    // nowhere in the scene's caster pass (level 8 frame 60: 0 texels), which is
    // why the ramp is taken wherever it is exact, and native only where the ramp
    // measured broken (the Old 3DS: 13,285 texels, the emulator's ramp map's own,
    // depths equal to 1/65536). So the first native frames have their map read
    // back, as evidence: the first that wrote any settles it; three in a row
    // with casters drawn and nothing written settle it too, and the map stays
    // native -- a frame CAN legitimately write nothing (every caster back-facing
    // the sun, or outside the map's box), and the only other path is the one
    // that cannot work on this GPU (review, 2026-11-18: demoting to the ramp here
    // put a console on it for the session).
    if (r->shadowNative && !r->shadowNativeVerified && r->shadowLive && r->shadowCasters > 0 &&
        r->shadowMapReady && r->shadowMapSize > 0) {
        const u32 side = (u32)r->shadowMapSize;
        const size_t bytes = (size_t)side * side * 4;
        u32* copy = static_cast<u32*>(linearAlloc(bytes));
        if (copy) {
            const u32 rowDim = GX_BUFFER_DIM((side * 8u * 4u) >> 4, 0);
            const bool read = readBack(r, r->shadowTex.data, rowDim, copy, bytes, 0xFF);
            uint32_t written = 0;
            for (size_t i = 0; i < (size_t)side * side && read; i++) {
                if (copy[i] != 0xFFFFFFFFu) {
                    written++;
                }
            }
            linearFree(copy);
            if (written > 0) {
                r->shadowNativeVerified = true;
                r->shadowNativeEmptyFrames = 0;
                mdk::log3dsPrintf("shadows: native writes verified in the caster pass (%d "
                                  "casters, %u texels written)",
                                  (int)r->shadowCasters, (unsigned)written);
            } else if (++r->shadowNativeEmptyFrames >= 3) {
                // Settled without a sighting: native writes are only chosen where
                // the ramp measured BROKEN, so there is nothing better to fall
                // back to (the ramp shadows the floor with itself there); the
                // readbacks stop, the map stays native.
                r->shadowNativeVerified = true;
                mdk::log3dsPrintf("shadows: native writes not seen in the caster pass (%d "
                                  "casters, 0 texels, 3 frames running); kept (the ramp "
                                  "measured broken on this GPU)", (int)r->shadowCasters);
            }
        }
    }
    // A pending capture reads the frame just ended (the transfer queues
    // behind its draws), then the request is spent either way.
    if (r->pendingScreenshot[0]) {
        if (writeScreenshotBmp(r, r->targetLeft, r->pendingScreenshot)) {
            r->capturesWritten++;
        }
        // A STEREO frame's right eye as well, beside it: the both-eye evidence
        // AGENTS.md P6 asks of rendering changes, and the only picture of the
        // edge-smoothing pass that draws against a borrowed depth buffer. The
        // name keeps the left file's suffix (hwcap.bmp.log -> hwcap.right.bmp.log)
        // because the console's HTTP serves only /3ds/**.log and .txt. The
        // harness's verdict stays the left eye's file.
        if (r->eyes.passes > 1) {
            char rightPath[sizeof(r->pendingScreenshot) + 16];
            const char* slash = strrchr(r->pendingScreenshot, '/');
            const char* dot = strchr(slash ? slash : r->pendingScreenshot, '.');
            if (dot) {
                snprintf(rightPath, sizeof(rightPath), "%.*s.right%s",
                         (int)(dot - r->pendingScreenshot), r->pendingScreenshot, dot);
            } else {
                snprintf(rightPath, sizeof(rightPath), "%s.right", r->pendingScreenshot);
            }
            writeScreenshotBmp(r, r->targetRight, rightPath);
        }
#ifdef MDK_AUTOQUIT
        // THE SHADOW MAP AS THE GPU WROTE IT (harness builds only): its tiled
        // bytes beside the capture (sdmc:/3ds/mdk/shadowmap.bin.log; the
        // console's HTTP serves /3ds/**.log), so the map the console made can be
        // compared byte for byte with the emulator's, read back through
        // readBack (known finished, the drain behind it).
        if (r->shadowMapReady && r->shadowLive && r->shadowMapSize > 0) {
            const u32 side = (u32)r->shadowMapSize;
            const size_t bytes = (size_t)side * side * 4;
            u32* copy = (u32*)linearAlloc(bytes);
            if (copy) {
                const u32 rowDim = GX_BUFFER_DIM((side * 8u * 4u) >> 4, 0);
                FILE* f = readBack(r, r->shadowTex.data, rowDim, copy, bytes, 0xFF)
                              ? fopen("sdmc:/3ds/mdk/shadowmap.bin.log", "wb") : nullptr;
                if (f) {
                    const size_t wrote = fwrite(copy, 1, bytes, f);
                    fclose(f);
                    mdk::log3dsPrintf("shadows: map %ux%u dumped (%u bytes) to shadowmap.bin.log",
                                      (unsigned)side, (unsigned)side, (unsigned)wrote);
                }
                linearFree(copy);
            }
        }
#endif
        r->pendingScreenshot[0] = '\0';
    }
    // C3D_FrameEnd submits the queue; it does not wait for it. Until the
    // next C3D_FrameBegin, the GPU may still be reading this frame's
    // textures, so they cannot be freed or replaced (AGENTS.md P3).
    r->gpuPending = true;
}

 void mdk_render_present(MdkRenderer rr) {
    // The present IS the frame end on this backend. Kept as a separate seam
    // entry so the Linux backend can differ, but there is nothing to do.
    (void)rr;
}

 void mdk_render_set_camera(MdkRenderer rr, const mdk::Camera* cam) {
    RendererC3D* r = asC3d(rr);
    if (!r || !cam) return;

    // The camera is KEPT, not baked: each eye of the 3DS's stereo pair needs
    // its own offset, and the offset is applied to the view matrix.
    memcpy(r->view, cam->view, sizeof(r->view));
    memcpy(r->projection, cam->projection, sizeof(r->projection));
    // The eye's world position, for the lit program's normal flip.
    r->cameraPos[0] = cam->position.x;
    r->cameraPos[1] = cam->position.y;
    r->cameraPos[2] = cam->position.z;
}

 void mdk_render_set_lighting(MdkRenderer rr, const mdk::Lighting* light) {
    // The rig is shared DATA (SPEC.md 5.3). The 3DS consumes what its PICA200
    // can express behind each Enhanced Mode toggle (SPEC.md 7.9): so far the
    // haze. The game hands it over when the arena changes, not per frame.
    RendererC3D* r = asC3d(rr);
    if (!r || !light) {
        return;
    }
    r->rig = *light;
    mdk::pica::dialledLighting(r->rig, r->toggles, &r->lighting);
    // The rig, once per change (the game hands it over per arena): the numbers
    // the lit program's light is made of.
    mdk::log3dsPrintf("light rig: sun dir (%.2f,%.2f,%.2f) rgb (%.2f,%.2f,%.2f) sky (%.2f,%.2f,%.2f) "
                      "ground (%.2f,%.2f,%.2f) exposure %.3f knee %.3f",
                      light->sun_direction.x, light->sun_direction.y, light->sun_direction.z,
                      light->sun.r, light->sun.g, light->sun.b, light->sky.r, light->sky.g,
                      light->sky.b, light->ground.r, light->ground.g, light->ground.b,
                      light->exposure, light->sky.a);
}

// The frame's light, decided once before the eye loop (Enhanced Mode
// enableLighting): whether any draw may take the lit program, and the
// values of its geometry-shader uniforms -- the rig's sun, sky and ground, the
// exposure, the eye's position (mdk_c3d.g.pica) -- which installLitUniforms
// writes at every bind of the lit program (P2).
static void prepareLighting(RendererC3D* r) {
    r->litLive = r->litReady && r->toggles.enableLighting && r->lighting.enabled;
    if (!r->litLive) {
        return;
    }
    const mdk::Lighting& l = r->lighting;
    const float values[6][4] = {
        {l.sun_direction.x, l.sun_direction.y, l.sun_direction.z, 0.0f},
        {l.sun.r, l.sun.g, l.sun.b, 0.0f},
        {l.sky.r, l.sky.g, l.sky.b, 0.0f},
        {l.ground.r, l.ground.g, l.ground.b, 0.0f},
        {r->cameraPos[0], r->cameraPos[1], r->cameraPos[2], 1.0f},
        // The exposure only: the PC's tone curve (knee 0.6, a constant of its
        // own, not the rig's) becomes the combiner's clamp (mdk_c3d.g.pica).
        {l.exposure, 0.0f, 0.0f, 0.0f},
    };
    memcpy(r->litUniforms, values, sizeof(values));
}

// The lit program's uniforms, written AT EVERY BIND of it. Writing them once a
// frame was not enough: a lit draw that followed an unlit one (an object after
// the sky or a sprite) read ZEROS -- measured, a lamp's wire frame lit to
// (1.43, 0, 0), and black when the light was set to the sky colour alone --
// while the level geometry drawn before any switch was right. THE CAUSE (review,
// libcitro3d.a disassembly): C3D_BindProgram of a program WITHOUT a geometry
// shader calls C3Di_ClearShaderUniforms(GPU_GEOMETRY_SHADER) -- on the PICA200
// the geometry unit runs vertex shading when no geometry shader is bound, so
// its uniform file is not the lit program's to keep. Six uniform writes per
// switch to the lit program.
static void installLitUniforms(RendererC3D* r) {
    const int locations[6] = {r->sunDirLocation, r->sunColourLocation, r->skyLocation,
                              r->groundLocation, r->cameraLocation, r->lightParamsLocation};
    for (int i = 0; i < 6; i++) {
        C3D_FVUnifSet(GPU_GEOMETRY_SHADER, locations[i], r->litUniforms[i][0], r->litUniforms[i][1],
                      r->litUniforms[i][2], r->litUniforms[i][3]);
    }
}

// The frame's haze, decided once before the eye loop: whether any draw may take
// it, and the shader's exponent for the rig's density. One multiply; the rest is
// the PICA's (mdk_c3d.v.pica, haze).
// The cull a Solid caster is drawn with (castShadows): the eye pipeline's own.
// The sun's matrix reaches clip space by the same proper transforms the eye's
// does, and the choice is measured, not derived: with the other winding the
// whole map was in shadow (level 8, 2026-11-18).
static const GPU_CULLMODE kCasterCull = kSolidCull;

// WHICH BYTE ORDER THE CASTER PASS WRITES THE MAP IN (pica_matrix.h,
// ShadowByteOrder): the REVERSED one, on the console and in the emulator alike.
//
// A shadow map's texel is (depth24 << 8) | softness, depth BIG-endian in the
// bytes (the vault's PICA200 note on shadow textures says so), and a PICA RGBA8
// texel is read as the word A:B:G:R. So the word's high byte -- the depth's high
// byte -- is the texel's A, then B, then G, and the softness is R: that is
// kShadowBytesReversed, and it is what the emulator was MEASURED to read. The
// emulator models the console's format (its shadow unit decodes `pixel >> 8` as
// the depth and `pixel & 0xFF` as the softness), so it is not a quirk of the
// host. An earlier version of this file wrote the other order on a console, on
// the argument that the emulator's was a host byte-order artefact; an
// independent review (2026-11-18) read the emulator's shader text and the
// format's documentation and found that argument wrong. The detection it rested
// on is gone.
//
// ON THE CONSOLE (2026-11-18) the map is written natively, and the console's map,
// read back, holds [depth high, mid, low, softness] per texel in memory: this
// order. config.ini's `shadowMapFlipBytes=1` writes the other one, and only the
// ramp (the emulator's path) reads it.
static mdk::pica::ShadowByteOrder shadowByteOrder(const RendererC3D* r) {
    return r->toggles.shadowMapFlipBytes ? mdk::pica::kShadowBytesPica
                                         : mdk::pica::kShadowBytesReversed;
}

// The ramp's texels, in `order` (ensureShadowMap; again when the order changes).
static void fillShadowRamp(RendererC3D* r, mdk::pica::ShadowByteOrder order) {
    const int width = mdk::pica::kSunShadowRampWidth;
    const int height = mdk::pica::kSunShadowRampHeight;
    uint8_t* texels = static_cast<uint8_t*>(r->shadowRamp.data);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint8_t rgba[4];
            mdk::pica::sunShadowRampTexel(x, y, order, rgba);
            const size_t slot =
                mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)width);
            mdk::pica::texelBytes(texels + slot * 4u, rgba[0], rgba[1], rgba[2], rgba[3]);
        }
    }
    C3D_TexFlush(&r->shadowRamp);
    r->shadowRampOrder = static_cast<int>(order);
}

// The ramp texture, made and filled once (linear heap): ensureShadowMap's in ramp
// mode, testNativeShadowWrites' to measure whether the ramp works at all.
static bool makeShadowRamp(RendererC3D* r) {
    if (r->shadowRampReady) {
        return true;
    }
    const int width = mdk::pica::kSunShadowRampWidth;
    const int height = mdk::pica::kSunShadowRampHeight;
    r->shadowRampReady = C3D_TexInit(&r->shadowRamp, (u16)width, (u16)height, GPU_RGBA8);
    if (r->shadowRampReady) {
        fillShadowRamp(r, shadowByteOrder(r));
        // NEAREST: a column and a row are bytes, not shades. s is clamped
        // (the depth is in [0, 1]); t REPEATS, 256 times across the range.
        C3D_TexSetFilter(&r->shadowRamp, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&r->shadowRamp, GPU_CLAMP_TO_EDGE, GPU_REPEAT);
    }
    return r->shadowRampReady;
}

// THE SUN SHADOW'S MAP, made on first use: the map itself (a PICA shadow
// texture in VRAM) and, when the caster pass writes through the ramp (not
// natively: castShadows), a 16-bit depth buffer for it and the 512x256 ramp that
// turns a depth into the map's bytes (linear heap, half a megabyte). The map's
// side is config.ini's shadowMapSize (128 / 256 / 512) or, automatic (0), 512;
// each smaller size down to 128 is tried while VRAM refuses -- with both eyes'
// targets at the edge smoothing's double size and the glow's textures, 512 does
// not fit and 256 does. (Automatic also falls to 128 now, where it used to give
// up at 256: the owner's 128 measured the same frame time, 2026-11-18.) False
// when nothing could be made: sun shadows are then off for the session, with one
// line.
static bool ensureShadowMap(RendererC3D* r) {
    if (r->shadowRefused) {
        return false;
    }
    if (!(r->shadowMapReady && r->shadowTarget)) {
        // The config's size (shadowMapSize), else 512; then each smaller one
        // down to 128 while VRAM refuses.
        const int first = r->toggles.shadowMapSize > 0 ? (int)r->toggles.shadowMapSize
                                                       : mdk::pica::kSunShadowMapSize;
        const int sizes[3] = {first, first / 2, first / 4};
        const int tries = first >= 512 ? 3 : (first >= 256 ? 2 : 1);
        // THE GLOW'S SHARE IS LEFT FOR IT. Both are made on first use out of the
        // same VRAM, and this one is asked first (before the eyes; the glow is
        // made inside the first eye): at the edge smoothing's double-size targets
        // a 512 map took all but 110 KB and the glow was refused (measured,
        // 2026-11-18). So a size is only tried when it leaves the glow's
        // textures their room, while the glow is wanted and not yet made.
        const size_t glowShare =
            (r->toggles.enableGlowBillboards && r->toggles.glowBloom && !r->glowReady &&
             !r->glowRefused)
                ? mdk::pica::kGlowVramBytes : 0u;
        for (int i = 0; i < tries && !r->shadowTarget; i++) {
            const u16 size = (u16)sizes[i];
            if ((size_t)vramSpaceFree() < mdk::pica::sunShadowVramBytes(sizes[i]) + glowShare) {
                continue;
            }
            r->shadowMapReady = C3D_TexInitShadow(&r->shadowTex, size, size);
            if (!r->shadowMapReady) {
                continue;
            }
            // The ramp keeps the nearer caster with a depth buffer of its own;
            // native writes keep it themselves and take the target exactly as
            // testNativeShadowWrites measured it -- WITHOUT one (the emulator,
            // for one, lands native writes into a bare target and none into a
            // target with a depth buffer: level 8 frame 60, 0 texels written).
            r->shadowTarget = C3D_RenderTargetCreateFromTex(
                &r->shadowTex, GPU_TEXFACE_2D, 0,
                r->shadowNative ? C3D_DEPTHTYPE(-1) : C3D_DEPTHTYPE(GPU_RB_DEPTH16));
            if (!r->shadowTarget) {
                // No target was made, so nothing the queue could be using: the
                // texture can go at once, and the smaller size is tried.
                C3D_TexDelete(&r->shadowTex);
                r->shadowMapReady = false;
                continue;
            }
            r->shadowMapSize = sizes[i];
            r->shadowMapFresh = true;   // nothing in it: the next frame casts
            r->shadowMapAsked = r->toggles.shadowMapSize;
            // The unit blends the four nearest comparisons (the PC's 2x2).
            C3D_TexSetFilter(&r->shadowTex, GPU_LINEAR, GPU_LINEAR);
            C3D_TexSetWrap(&r->shadowTex, GPU_CLAMP_TO_BORDER, GPU_CLAMP_TO_BORDER);
            // Outside the map is LIT: the border is the farthest depth with the
            // full softness, 0xFFFFFFFF -- the same word in either byte order
            // (citro3d's default is 0, a dark wedge past the map's edge).
            r->shadowTex.border = 0xFFFFFFFFu;
            mdk::log3dsPrintf("shadows: map %dx%d%s (VRAM, %u free), %s",
                              (int)r->shadowMapSize, (int)r->shadowMapSize,
                              r->shadowNative ? "" : " + 16-bit depth", (unsigned)vramSpaceFree(),
                              r->shadowNative ? "written natively" : "written through the ramp");
        }
    }
    if (r->shadowTarget && !r->shadowNative && !r->shadowRampReady) {
        makeShadowRamp(r);
    }
    if (!r->shadowMapReady || !r->shadowTarget || (!r->shadowNative && !r->shadowRampReady)) {
        // Inside a frame, where C3D_RenderTargetDelete breaks: what was made
        // waits for releaseShadowMap (the next eye-target rebuild, or the
        // destructor).
        mdk::log3dsPrintf("shadows: map / depth / ramp allocation FAILED (map %d target %d "
                          "ramp %d); sun shadows off (vram free %u)",
                          (int)r->shadowMapReady, r->shadowTarget ? 1 : 0,
                          (int)r->shadowRampReady, (unsigned)vramSpaceFree());
        r->shadowRefused = true;
        r->shadowMapAsked = r->toggles.shadowMapSize;
        return false;
    }
    // The order can change while the game runs (the config's flip): the ramp is
    // written again. Nothing of the GPU's is reading it here -- C3D_FrameBegin
    // waited for the last frame's queue, and this frame has not drawn with it.
    const mdk::pica::ShadowByteOrder order = shadowByteOrder(r);
    if (r->shadowRampReady && r->shadowRampOrder != static_cast<int>(order)) {
        fillShadowRamp(r, order);
        r->shadowMapFresh = true;   // a kept map holds the old order's bytes
        mdk::log3dsPrintf("shadows: the ramp is rewritten in %s byte order",
                          order == mdk::pica::kShadowBytesReversed ? "reversed" : "the PICA's");
    }
    return true;
}

// THE SHADOW TINT, made on first use (linear heap, 2 KB) and refilled when the
// rig's sky, ground or exposure, or the shadowDarkness dial, differ from what
// it holds (the dial scales the alpha, pica::sunShadowTintAlphaDialled): inside a frame,
// after C3D_FrameBegin has waited for the last frame's queue, so nothing still
// reads it. Eight float compares a frame, not in any loop.
static bool ensureShadowTint(RendererC3D* r) {
    if (!r->shadowTintReady) {
        r->shadowTintReady = C3D_TexInit(&r->shadowTint, (u16)mdk::pica::kShadowTintWidth,
                                         (u16)mdk::pica::kShadowTintHeight, GPU_RGBA8);
        if (!r->shadowTintReady) {
            return false;
        }
        // LINEAR along t (the hemisphere is smooth) and down the edge fade;
        // clamped, so t = 0 / 1 and the fade's ends read their own texels.
        C3D_TexSetFilter(&r->shadowTint, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&r->shadowTint, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        r->shadowTintRig[0] = -1.0f;   // filled below
    }
    const mdk::Lighting& l = r->lighting;
    const float rig[8] = {l.sky.r, l.sky.g, l.sky.b, l.ground.r, l.ground.g, l.ground.b,
                          l.exposure, (float)r->toggles.shadowDarkness};
    if (memcmp(rig, r->shadowTintRig, sizeof(rig)) != 0) {
        // One colour per column (the powf), then down the rows (the alpha).
        uint8_t* texels = static_cast<uint8_t*>(r->shadowTint.data);
        for (int x = 0; x < mdk::pica::kShadowTintWidth; x++) {
            uint8_t rgba[4];
            mdk::pica::sunShadowTintColumn(x, &rig[0], &rig[3], rig[6], rgba);
            for (int y = 0; y < mdk::pica::kShadowTintHeight; y++) {
                rgba[3] = mdk::pica::sunShadowTintAlphaDialled(y, r->toggles.shadowDarkness);
                const size_t slot = mdk::pica::textureTexelIndex(
                    (uint32_t)x, (uint32_t)y, (uint32_t)mdk::pica::kShadowTintWidth);
                mdk::pica::texelBytes(texels + slot * 4u, rgba[0], rgba[1], rgba[2], rgba[3]);
            }
        }
        C3D_TexFlush(&r->shadowTint);
        memcpy(r->shadowTintRig, rig, sizeof(rig));
    }
    return true;
}

// The frame's sun shadow, decided once before the eye loop: whether there is
// one, the caster pass's matrix, and the shadowed lit program's uniforms.
static void prepareShadows(RendererC3D* r) {
    r->shadowCasters = 0;
    r->shadowHoleCasters = 0;
    r->shadowCastersCulled = 0;
    r->shadowReceivers = 0;
    r->shadowLitLive = false;
    // The tint first: without it no lit draw can take the shadow, and a map
    // cast for no receiver is a pass for nothing (review, 2026-11-19).
    r->shadowLive = r->litLive && r->shadowProgramReady && r->casterProgramReady &&
                    r->unitIdleReady && r->toggles.enableSkyShadows && ensureShadowTint(r) &&
                    r->lighting.shadows && r->lighting.shadow_distance > 0.0f &&
                    ensureShadowMap(r);
    if (!r->shadowLive) {
        return;
    }
    const mdk::Lighting& l = r->lighting;
    // REDRAW THE MAP, OR KEEP LAST FRAME'S (config shadowMapRefresh; measured
    // 2026-11-19: the caster pass is ~4 ms of the Old 3DS's stereo frame). The
    // map is the sun's view of the casters around the camera: while the sun,
    // the box and the static world stand still it is the same picture, so it is
    // redrawn every `every` frames, and AT ONCE when the map is new, the sun has
    // turned, the camera has moved a sixteenth of the box's reach from where it
    // was cast (the box's far edges are what that would move), or the native
    // writes are still to be seen (end_frame's check needs a cast). A kept map
    // keeps its matrices: the receivers sample it where it was cast.
    const int every = r->toggles.shadowMapRefresh > 0 ? (int)r->toggles.shadowMapRefresh : 2;
    const float dx = r->cameraPos[0] - r->shadowCastCentre[0];
    const float dy = r->cameraPos[1] - r->shadowCastCentre[1];
    const float dz = r->cameraPos[2] - r->shadowCastCentre[2];
    const float reach = l.shadow_distance / 16.0f;
    const bool moved = dx * dx + dy * dy + dz * dz > reach * reach;
    const bool sunTurned = l.sun_direction.x != r->shadowCastSun[0] ||
                           l.sun_direction.y != r->shadowCastSun[1] ||
                           l.sun_direction.z != r->shadowCastSun[2];
    // The reach and the rig: a changed shadow distance makes the bias and the
    // offset disagree with the map, and a new level brings a new rig (review,
    // 2026-11-19: a level change was otherwise caught only by the camera's move).
    const float rig[7] = {l.shadow_distance, l.sky.r, l.sky.g, l.sky.b,
                          l.ground.r, l.ground.g, l.ground.b};
    const bool rigChanged = memcmp(rig, r->shadowCastRig, sizeof(rig)) != 0;
    r->shadowRecast = r->shadowMapFresh || moved || sunTurned || rigChanged ||
                      r->frame - r->shadowCastFrame >= every ||
                      (r->shadowNative && !r->shadowNativeVerified);
    if (r->shadowRecast) {
        float sun[16];
        const mdk::Vec3 centre = {r->cameraPos[0], r->cameraPos[1], r->cameraPos[2]};
        mdk::sunShadowMatrix(l.sun_direction, centre, l.shadow_distance,
                             (uint32_t)r->shadowMapSize, sun);
        mdk::pica::sunCasterMatrix(sun, r->sunVp);
        // The sun volume for the caster cull (castShadows): the engine
        // matrix's own planes, before the PICA conversions.
        mdk::pica::frustumPlanes(sun, r->sunFrustum);
        r->sunFrustumValid = true;
        float map[16];
        mdk::pica::sunMapMatrix(sun, map);
        mdk::pica::registers(map, r->shadowSunMtx);
        r->shadowMapFresh = false;
        r->shadowCastFrame = r->frame;
        memcpy(r->shadowCastCentre, r->cameraPos, sizeof(r->shadowCastCentre));
        r->shadowCastSun[0] = l.sun_direction.x;
        r->shadowCastSun[1] = l.sun_direction.y;
        r->shadowCastSun[2] = l.sun_direction.z;
        memcpy(r->shadowCastRig, rig, sizeof(rig));
    } else {
        r->shadowReuses++;
    }
    const float values[6][4] = {
        {l.sun_direction.x, l.sun_direction.y, l.sun_direction.z, 0.0f},
        {l.sun.r, l.sun.g, l.sun.b, 0.0f},
        {l.sky.r, l.sky.g, l.sky.b, 0.0f},
        {l.ground.r, l.ground.g, l.ground.b, 0.0f},
        {r->cameraPos[0], r->cameraPos[1], r->cameraPos[2], 1.0f},
#if defined(MDK_SHADOW_PROBE)
        // The harness's probe (make SHADOWPROBE=8): every receiver pushed to the
        // map's far end (all shadow, if the unit compares at all).
        {l.exposure, mdk::pica::sunShadowNormalOffset(r->shadowMapSize),
         MDK_SHADOW_PROBE == 8 ? -2.0f : mdk::pica::sunShadowDepthBias(r->shadowMapSize), 0.0f},
#else
        {l.exposure, mdk::pica::sunShadowNormalOffset(r->shadowMapSize),
         mdk::pica::sunShadowDepthBias(r->shadowMapSize), 0.0f},
#endif
    };
    memcpy(r->shadowUniforms, values, sizeof(values));
    // The lit draws take the shadow in their own pass (06_draw.inc drawRecorded).
    r->shadowLitLive = true;
}

// The shadowed lit program's uniforms, written AT EVERY BIND of it, for the
// reason installLitUniforms gives: binding a program with no geometry shader
// clears the geometry stage's uniforms.
static void installShadowUniforms(RendererC3D* r) {
    float lutMap[4];
    mdk::pica::sunShadowTintMap(lutMap);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, r->shadowLutMapLocation, lutMap[0], lutMap[1], lutMap[2],
                  lutMap[3]);
    const int locations[6] = {r->shadowSunDirLocation, r->shadowSunColourLocation,
                              r->shadowSkyLocation, r->shadowGroundLocation,
                              r->shadowCameraLocation, r->shadowParamsLocation};
    for (int i = 0; i < 6; i++) {
        C3D_FVUnifSet(GPU_GEOMETRY_SHADER, locations[i], r->shadowUniforms[i][0],
                      r->shadowUniforms[i][1], r->shadowUniforms[i][2], r->shadowUniforms[i][3]);
    }
    C3D_Mtx sunMtx;
    memcpy(sunMtx.m, r->shadowSunMtx, sizeof(r->shadowSunMtx));
    C3D_FVUnifMtx4x4(GPU_GEOMETRY_SHADER, r->shadowSunMtxLocation, &sunMtx);
}

// THE CASTER PASS (Enhanced Mode enableSkyShadows): the frame's opaque Lit
// triangles -- the PC's casters, 11_shadow_post.inc drawShadowMap -- drawn from
// the sun into the shadow map, once per frame, before the eyes.
//
// TWO WAYS TO WRITE THE MAP, CHOSEN BY MEASUREMENT (testNativeShadowWrites and
// the end_frame verification have the facts): NATIVE, the PICA's own shadow
// fragment mode -- each caster fragment's 24-bit depth into the map, the nearer
// kept, the combiner black -- which the console lands; or THE RAMP, an ordinary
// draw whose texture coordinate is the vertex's depth from the sun into a ramp
// texture whose colour IS the map's encoding of that depth
// (pica::sunShadowRampTexel), the map's own 16-bit depth buffer keeping the
// nearest caster -- which the emulator lands. Either way the map is cleared to
// 0xFFFFFFFF, the farthest depth there is, and Solid casters show the sun only
// their front faces, as on the PC.
//
// WHAT A CASTER DOES NOT HAVE HERE IS ITS OWN TEXTURE: a cut-out (a grating, a
// fence) is left out of the pass rather than cast solid. The PC cuts its
// casters; this is the port's one known shortfall in the pass.
//
// installEye runs after this for each eye and installs the frame's state again
// (program, attributes, matrix, stages, cull); the depth, alpha and blend state
// this pass leaves are the ones their caches say.
static void castShadows(RendererC3D* r) {
    // (A frame that keeps last frame's map draws nothing: prepareShadows.)
    if (!r->shadowLive || !r->shadowRecast) {
        return;
    }
    leaveShadowLitStages(r);
    const u32 size = (u32)r->shadowMapSize;
#if defined(MDK_SHADOW_PROBE)
    // 10: the map cleared to the NEAREST depth and nothing cast (all shadow, if
    // the unit reads what this pass clears).
    const bool probeNoCasters = MDK_SHADOW_PROBE == 10;
    C3D_FrameBufClear(&r->shadowTarget->frameBuf, C3D_CLEAR_ALL,
                      probeNoCasters ? 0x000000FFu : 0xFFFFFFFFu, mdk::pica::kDepthClearValue);
#else
    const bool probeNoCasters = false;
    // A native map's target has no depth buffer to clear.
    C3D_FrameBufClear(&r->shadowTarget->frameBuf, r->shadowNative ? C3D_CLEAR_COLOR : C3D_CLEAR_ALL,
                      0xFFFFFFFFu, mdk::pica::kDepthClearValue);
#endif
    C3D_FrameDrawOn(r->shadowTarget);
    C3D_SetViewport(0, 0, size, size);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, size, size);

    // The fixed state, written whole (AGENTS.md P2): this pass comes before the
    // frame's first installEye, so nothing of this frame is installed yet.
    bindProgram(r, &r->programCaster);
    r->litBound = false;
    C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
    AttrInfo_Init(attrInfo);
    AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);   // position
    AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2);   // texcoord (not read)
    // NATIVE (testNativeShadowWrites measured that the writes land): the PICA
    // writes each fragment's depth itself and keeps the nearer, so no ramp and
    // no depth test; the combiner's output is black because its GREEN is the
    // penumbra input, and 0 is a hard shadow that keeps the clear's softness.
    C3D_TexEnv* ramp = C3D_GetTexEnv(0);
    C3D_TexEnvInit(ramp);
    C3D_TexEnvColor(ramp, 0x00000000u);   // native mode's source (unused by the ramp's)
    if (!r->shadowNative) {
        bindTexUnit(r, 0, &r->shadowRamp);
    }
    C3D_TexEnvSrc(ramp, C3D_RGB, r->shadowNative ? GPU_CONSTANT : GPU_TEXTURE0,
                  r->shadowNative ? GPU_CONSTANT : GPU_TEXTURE0,
                  r->shadowNative ? GPU_CONSTANT : GPU_TEXTURE0);
    C3D_TexEnvFunc(ramp, C3D_RGB, GPU_REPLACE);
    // The alpha is the RAMP'S, in both byte orders: in the reversed one (the
    // default) it is the depth's high byte, in the PICA one the softness (255).
    // A constant 255 here wrote every map texel's depth as the far plane in the
    // default order -- no sun shadow at all, measured 2026-11-18 (level 8 frame
    // 60, the shadow on and off: 0 pixels apart; independent review finding 1).
    // So the alpha test is OFF for this pass (below): it would discard every
    // caster whose depth's high byte is 0, and the casters it exists for --
    // textures with holes -- are left out of the pass anyway.
    C3D_TexEnvSrc(ramp, C3D_Alpha, r->shadowNative ? GPU_CONSTANT : GPU_TEXTURE0,
                  r->shadowNative ? GPU_CONSTANT : GPU_TEXTURE0,
                  r->shadowNative ? GPU_CONSTANT : GPU_TEXTURE0);
    C3D_TexEnvFunc(ramp, C3D_Alpha, GPU_REPLACE);
    for (int stage = 1; stage <= 5; stage++) {
        C3D_TexEnvInit(C3D_GetTexEnv(stage));
    }
    C3D_TexEnvBufUpdate(C3D_Both, 0);
    // (Stages 1-5 pass through, and the two caches that describe them say so.)
    r->hazeOn = false;
    r->lightStageOn = false;
    C3D_DepthMap(mdk::pica::kDepthMapIsZBuffer, mdk::pica::kDepthMapScale,
                 mdk::pica::kDepthMapOffset);
    // The ramp keeps the nearer caster with the map's own depth buffer; native
    // writes keep it themselves.
    installDepthState(r, mdk::pica::DepthState{!r->shadowNative, !r->shadowNative});
    if (r->shadowNative) {
        C3D_FragOpMode(GPU_FRAGOPMODE_SHADOW);
        C3D_FragOpShadow(0.0f, 1.0f);
    }
    // No alpha test (above): the reference -1 is the test off, and no draw uses
    // it, so the next installAlphaTest writes the register again (installEye
    // forces it anyway, every eye).
    installAlphaTest(r, -1, true);
    writeBlendState(r, mdk::pica::kBlendReplace, -3, -3);
    C3D_CullFace(GPU_CULL_NONE);
    r->cullMode = GPU_CULL_NONE;

    for (int32_t pass = static_cast<int32_t>(mdk::Pass::Solid);
         !probeNoCasters && pass <= static_cast<int32_t>(mdk::Pass::DoubleSided); pass++) {
        const std::vector<mdk::MeshDraw>& bucket = r->queue.bucket(pass);
        for (size_t i = 0; i < bucket.size(); i++) {
            const mdk::MeshDraw* draw = &bucket[i];
            if (!mdk::pica::castsSunShadow(draw->material.pass, draw->material.shading,
                                           draw->primitive)) {
                continue;
            }
            if (draw->mesh < 0 || (size_t)draw->mesh >= r->meshes.size()) {
                continue;
            }
            // A surface whose texture has holes casts nothing (pica::
            // textureSourceHasHoles): this pass cannot cut them.
            if (draw->material.texture >= 0 &&
                (size_t)draw->material.texture < r->textures.size() &&
                r->textures[(size_t)draw->material.texture].hasHoles) {
                r->shadowHoleCasters++;
                continue;
            }
            const C3dMesh& mesh = r->meshes[(size_t)draw->mesh];
            if (!mesh.valid || mesh.count <= 0 || draw->first_vertex < 0 ||
                draw->vertex_count <= 0 ||
                draw->first_vertex + draw->vertex_count > mesh.count) {
                continue;
            }
            // THE SUN-FRUSTUM CULL: a caster wholly outside the sun's
            // volume writes no map texel, so it is not drawn into the
            // map. Only the level's batches carry a world box (identity
            // world, like the eye cull); everything else is drawn as
            // before. The box is exact at build and a torn mesh only
            // collapses inward, so a skipped caster cast nothing.
            /* @vfp-exempt R3 — a frustum test IS a float predicate; culling casters outside the sun volume is the measured win, not a rounding-sensitive gameplay boundary. measured n/a (pending OG capture). Verified 2026-11-23. */
            if (draw->cullable && r->sunFrustumValid && mdk::pica::isIdentity(draw->world)) {
                bool outside = false;
                for (int p = 0; p < 6 && !outside; p++) {
                    const float* pl = r->sunFrustum[p];
                    const float vx = pl[0] >= 0.0f ? draw->bounds_max[0] : draw->bounds_min[0];
                    const float vy = pl[1] >= 0.0f ? draw->bounds_max[1] : draw->bounds_min[1];
                    const float vz = pl[2] >= 0.0f ? draw->bounds_max[2] : draw->bounds_min[2];
                    if (pl[0] * vx + pl[1] * vy + pl[2] * vz + pl[3] < 0.0f) {
                        outside = true;
                    }
                }
                if (outside) {
                    r->shadowCastersCulled++;
                    continue;
                }
            }
            float composed[16];
            const float* upload = r->sunVp;
            if (!mdk::pica::isIdentity(draw->world)) {
                mdk::pica::drawMatrix(draw->world, r->sunVp, composed);
                upload = composed;
            }
            C3D_Mtx gpuMtx;
            mdk::pica::registers(upload, gpuMtx.m);
            // Deduped (THE DEDUPE): a run of identity-world casters shares sunVp.
            setVsMatrix(r, kDedupeMvp, r->casterMvpLocation, &gpuMtx);
            // THE PC'S CULL (11_shadow_post.inc: depthPipelines[Solid ? 0 : 1]): a
            // Solid caster shows the sun only its FRONT faces, a DoubleSided one
            // both. It is not a detail: the levels' walls are one-sided shells,
            // and seen from behind -- from the sun's side of a canyon wall --
            // they cast nothing on the PC. Drawn two-sided here, the wall shadowed
            // the whole canyon floor the PC leaves in the sun (measured, level 8).
            const GPU_CULLMODE wantedCull =
                draw->material.pass == mdk::Pass::Solid ? kCasterCull : GPU_CULL_NONE;
            if (r->cullMode != wantedCull) {
                C3D_CullFace(wantedCull);
                r->cullMode = wantedCull;
            }
            bindVertexBuffer(r, mesh.data);
            C3D_DrawArrays(GPU_TRIANGLES, draw->first_vertex, draw->vertex_count);
            r->shadowCasters++;
        }
    }

#ifdef MDK_HW_PROFILE
    if (r->shadowCastersCulled > 0 && (r->frame % 120) == 0) {
        mdk::log3dsPrintf("shadowcull: frame %lu culled %d of %d casters by the sun frustum",
                          (unsigned long)r->frame, (int)r->shadowCastersCulled,
                          (int)(r->shadowCasters + r->shadowCastersCulled));
    }
#endif
    if (r->shadowNative) {
        C3D_FragOpMode(GPU_FRAGOPMODE_GL);
    }
    // The map is a texture the eyes' draws read in this same command list: what
    // the GPU still holds of it goes to memory first (the glow's rule).
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
    // The vertex stage's matrix registers hold the last caster's: installEye
    // uploads the eye's before anything else is drawn.
    r->matrixIsFrame = false;
    // The log reports the last cast's counts: a reused map's frame casts none.
    r->shadowCastersLast = r->shadowCasters;
    r->shadowHoleCastersLast = r->shadowHoleCasters;
}

static void prepareHaze(RendererC3D* r) {
    // pica::hazeApplies with the draw-independent half decided here, once; the
    // per-draw half (shading, pass) is drawRecorded's.
    r->hazeLive = mdk::pica::hazeApplies(r->toggles.enableHaze, r->lighting, mdk::Shading::Lit,
                                         mdk::Pass::Solid);
    r->hazeExponent = r->hazeLive ? mdk::pica::hazeExponent(r->lighting.haze_density) : 0.0f;
}

void mdk_render_set_enhanced_toggles(MdkRenderer rr, const mdk::EnhancedToggles* toggles) {
    RendererC3D* r = asC3d(rr);
    if (!r || !toggles) {
        return;
    }
    r->toggles = *toggles;
    mdk::pica::dialledLighting(r->rig, r->toggles, &r->lighting);
    applyFramePacing(r);
}

// THE 30 FPS CAP (Enhanced Mode cap30Fps): citro3d's OWN frame pacing, not a
// wait of ours. C3D_FrameRate(fps) sets how many vblanks make one tick of the
// frame counters that C3D_FrameBegin(C3D_FRAME_SYNCDRAW) -- this renderer's
// begin -- already waits on (libcitro3d's onVBlank0/1 subtract fps from a
// 60-per-vblank budget; C3D_FrameSync waits for both counters to move). At 30
// a frame is presented every second vblank; at 60, every one, as before. No
// VBlank wait, swap or flush is added (AGENTS.md P3). The game's clock is the
// wall clock (main_3ds.cpp nowSeconds), so the fixed-step accumulator runs two
// 1/60 s steps a 30 fps frame and the game keeps its speed. Called at create
// and on every toggles change; C3D_FrameRate only when the rate changes.
static void applyFramePacing(RendererC3D* r) {
    const float want = r->toggles.cap30Fps ? 30.0f : 60.0f;
    if (want != r->pacedFps) {
        C3D_FrameRate(want);
        r->pacedFps = want;
        mdk::log3dsPrintf("pacing: %d fps (cap30Fps=%d)", (int)want, (int)r->toggles.cap30Fps);
    }
}

 void mdk_render_set_glow_quads(MdkRenderer rr, const mdk::GlowQuad* quads, int32_t count) {
    RendererC3D* r = asC3d(rr);
    if (!r) {
        return;
    }
    const int32_t kept = count < 0 ? 0 : (count > RendererC3D::kMaxGlowQuads
                                              ? RendererC3D::kMaxGlowQuads : count);
    if (quads && kept > 0) {
        memcpy(r->glowQuads, quads, sizeof(mdk::GlowQuad) * (size_t)kept);
    }
    r->glowQuadCount = quads ? kept : 0;
    r->glowQuadsDropped += quads && count > kept ? count - kept : 0;
}

 void mdk_render_set_point_lights(MdkRenderer rr, const mdk::PointLight* lights, int32_t count) {
    (void)rr;
    (void)lights;
    (void)count;
}

 void mdk_render_set_panorama(MdkRenderer rr, const mdk::Panorama* p) {
    (void)rr;
    (void)p;
}

 void mdk_render_set_fade(MdkRenderer rr, float level) {
    (void)rr;
    (void)level;
}

 void mdk_render_set_gamma(MdkRenderer rr, float gamma) {
    (void)rr;
    (void)gamma;
}

