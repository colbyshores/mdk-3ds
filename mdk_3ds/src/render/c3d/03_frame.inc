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

// The HD stream's entry points (04_textures.inc, THE HD STREAM), declared here
// because the frame boundary calls them before they are defined.
static void dropHd(RendererC3D* renderer, HdCopy& copy);
static void pumpHdTextures(RendererC3D* renderer, uint32_t protectFrom);
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
        texture.source.clear();
        texture.palette.clear();
    }
    for (size_t i = 0; i < renderer->textures.size(); i++) {
        C3dTexture& texture = renderer->textures[i];
        if (!texture.stale) {
            continue;
        }
        texture.stale = false;
        if (texture.valid) {
            C3D_TexDelete(&texture.tex);
            texture.valid = false;
        }
        if (!texture.source.empty() && makeTexture(renderer, texture, false)) {
            // The replacement is protected until it is first drawn.
            texture.lastUsed = ++renderer->drawCounter;
        }
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
static void installLitUniforms(RendererC3D* r);
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
    C3D_DepthTest(state.test, state.test ? GPU_LEQUAL : GPU_ALWAYS,
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
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   gpuBlendFactor(eff.srcColour), gpuBlendFactor(eff.dstColour),
                   gpuBlendFactor(eff.srcAlpha), gpuBlendFactor(eff.dstAlpha));
    r->blendPass = static_cast<int>(pass);
    r->blendMode = static_cast<int>(blend);
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
    C3D_AlphaTest(true, GPU_GREATER, reference);
    r->alphaReference = reference;
}

static void installEye(RendererC3D* r, C3D_RenderTarget* target, float offset) {
    C3D_FrameDrawOn(target);

    // The MVP for this eye: the view-projection with both PICA conversions
    // baked in. It is KEPT on the renderer, because a draw that carries a
    // world matrix composes it against this matrix instead of rebuilding the
    // whole chain per draw.
    mdk::pica::eyeViewProjection(r->view, r->projection, offset, r->eyeVp);
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

    C3D_BindProgram(&r->program);

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
    C3D_SetViewport(0, 0, kScreenWidth, kScreenHeight);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, kScreenWidth, kScreenHeight);
}

 void mdk_render_begin_frame(MdkRenderer rr, const MdkFrameParams* params) {
    RendererC3D* r = asC3d(rr);
    if (!r || !r->ready) return;
    if (r->inFrame) return;   // a begin without an end is a bug, not a re-entry

    // C3D_FrameBegin returns FALSE when it could not begin (with
    // C3D_FRAME_NONBLOCK). A failed begin does NOT authorise drawing.
    if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) {
        return;
    }
    r->inFrame = true;
    r->frame++;
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
    r->census.active = (r->frame == kCensusFrame);
    r->census.triangles = 0;
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
    pumpHdTextures(r, previousFrameStart);
    // The stream's state, every 300 frames once it has done anything: the
    // evidence a budget is read from (docs/HD_TEXTURES_3DS_SPEC.md 4.2).
    if ((r->frame % 300) == 0 && (r->hdImported > 0 || !r->hdQueue.empty())) {
        mdk::log3dsPrintf("hd: frame %d: %d imported, %u bytes resident of %u, %d evicted, "
                          "%d failed, %d queued, %d reading",
                          (int)r->frame, (int)r->hdImported, (unsigned)r->hdResidentBytes,
                          (unsigned)kHdBudgetBytes, (int)r->hdEvicted, (int)r->hdFailedCount,
                          (int)r->hdQueue.size(), (int)r->hdReader.busy());
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
    r->eyes = mdk::pica::eyePlan((float)osGet3DSliderState(), r->eyeSeparation);

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
    C3D_RenderTargetClear(r->targetLeft, C3D_CLEAR_ALL, clear,
                          mdk::pica::kDepthClearValue);
    C3D_RenderTargetClear(r->targetRight, C3D_CLEAR_ALL, clear,
                          mdk::pica::kDepthClearValue);
}

// Report one censused frame. Cold: called once per process, from the end of the
// first eye's replay of frame `kCensusFrame`.
static void logCensus(RendererC3D* r) {
    const Census& c = r->census;
    mdk::log3dsPrintf("census frame=%d draws=%d tris=%d behind=%d offscreen=%d onscreen=%d depthout=%d near60=%d cw=%d ccw=%d w=[%.1f,%.1f] areaset=%.4f",
                      r->frame, (int)r->drawCount, (int)c.triangles, (int)c.behind,
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
static bool writeScreenshotBmp(RendererC3D* r, const char* path) {
    if (!r->targetLeft || !r->targetLeft->frameBuf.colorBuf) {
        return false;
    }
    const int32_t srcW = kScreenWidth;    // 240, portrait width
    const int32_t srcH = kScreenHeight;   // 400, portrait height
    const size_t bytes = (size_t)srcW * (size_t)srcH * 4;
    u32* pixels = (u32*)linearAlloc(bytes);
    if (!pixels) {
        mdk::log3dsPrintf("screenshot: linearAlloc(%u) FAILED for %s", (unsigned)bytes, path);
        return false;
    }
    const u32 flags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
                      GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                      GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                      GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    GX_DisplayTransfer((u32*)r->targetLeft->frameBuf.colorBuf, GX_BUFFER_DIM(srcW, srcH),
                       pixels, GX_BUFFER_DIM(srcW, srcH), flags);
    GSPGPU_FlushDataCache(pixels, bytes);
    gspWaitForEvent(GSPGPU_EVENT_PPF, false);
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
    prepareHaze(r);
    prepareLighting(r);
    for (int eye = 0; eye < r->eyes.passes; eye++) {
        installEye(r, eye == 0 ? r->targetLeft : r->targetRight, r->eyes.offset[eye]);
        replayQueue(r);
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
        if (r->canvasFrames == 1 || (r->canvasFrames % 60) == 0) {
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
    if (r->litLive && (r->frame % 300) == 0) {
        mdk::log3dsPrintf("light: frame %d: %d program switches (%d eyes)", (int)r->frame,
                          (int)r->programSwitches, (int)r->eyes.passes);
    }
    r->programSwitches = 0;
    r->canvasQuadsThisFrame = 0;
    r->canvasDrawsThisFrame = 0;
    r->queue.reset();
    // C3D_FrameEnd is the ONLY present path. It triggers the auto display
    // transfer set up by C3D_RenderTargetSetOutput.
    C3D_FrameEnd(0);
    r->inFrame = false;
    // A pending capture reads the frame just ended (the transfer queues
    // behind its draws), then the request is spent either way.
    if (r->pendingScreenshot[0]) {
        if (writeScreenshotBmp(r, r->pendingScreenshot)) {
            r->capturesWritten++;
        }
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
    r->lighting = *light;
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

