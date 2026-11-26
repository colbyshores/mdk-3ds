// 03_eye.inc -- one eye's state: installEye, and the edge smoothing.
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

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
    r->eyeOffset = offset;
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

