// 06_draw_submit.inc -- the per-eye submit of one prepared draw, and the diagnostic triangle.
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

// THE PER-EYE SUBMIT: only what depends on the eye -- its frustum, its
// matrix -- plus the deduped state writes and the draw itself.
static void drawRecorded(RendererC3D* r, const PreparedDraw* p) {
    const mdk::MeshDraw* draw = p->draw;
    const bool overlay = p->overlay;
    const bool lit = p->lit;
    const bool shadowLit = p->shadowLit;
    const bool wholeTriangles = p->wholeTriangles;

    // THE PER-EYE FRUSTUM CULL. The level's batches opt in (MeshDraw::cullable,
    // game/level_view.cpp); everything else -- objects with a world matrix,
    // sprites, canvas, insets -- leaves `cullable` false and is never tested.
    // A batch whose box is wholly outside this eye's frustum is not submitted:
    // the PICA200 never sees its vertices. The original culled through the arena
    // BSP; the port draws flat batches, so two resident arenas paid the vertex
    // pipeline for geometry behind the camera (measured: the pair window cost
    // ~+17 ms/frame on Old 3DS, 2026-11-22). The box is exact at build; a torn
    // mesh only collapses vertices inward, so a stale box stays conservative.
    /* @vfp-exempt R3 — a frustum test IS a float predicate; culling invisible batches is the measured win, not a rounding-sensitive gameplay boundary. measured n/a (pending OG capture). Verified 2026-11-23. */
    if (draw->cullable && r->frustumValid) {
        bool outside = false;
        for (int plane = 0; plane < 6 && !outside; plane++) {
            const float* pl = r->frustum[plane];
            // The box corner that MAXIMIZES the plane's dot (the "positive
            // vertex"): the batch is outside when even that corner is behind
            // the plane. Sign only, so the planes need no normalization.
            const float vx = pl[0] >= 0.0f ? draw->bounds_max[0] : draw->bounds_min[0];
            const float vy = pl[1] >= 0.0f ? draw->bounds_max[1] : draw->bounds_min[1];
            const float vz = pl[2] >= 0.0f ? draw->bounds_max[2] : draw->bounds_min[2];
            if (pl[0] * vx + pl[1] * vy + pl[2] * vz + pl[3] < 0.0f) {
                outside = true;
            }
        }
        if (outside) {
            return;
        }
    }

    // Re-derive the pointers from the prepared indices (see PreparedDraw):
    // a texture evicted since prepare is revived here, as the old per-eye
    // resolve did, and a mesh that went invalid drops the draw. The tables
    // only grow within a frame (nothing erases or compacts meshes,
    // textures or hdCopies between prepare and the queue reset), so an
    // index still names the same entry; the range guard is repeated
    // because the mesh may have been re-made smaller since prepare
    // (validator F11).
    if (p->meshIndex < 0 || (size_t)p->meshIndex >= r->meshes.size()) {
        return;
    }
    const C3dMesh& mesh = r->meshes[(size_t)p->meshIndex];
    if (!mesh.valid || mesh.count <= 0) {
        return;
    }
    if (draw->first_vertex < 0 || p->vertCount <= 0 ||
        draw->first_vertex + p->vertCount > mesh.count) {
        return;
    }
    C3D_Tex* texture = nullptr;
    bool usedHd = false;
    if (p->texHd && p->texIndex >= 0 && (size_t)p->texIndex < r->hdCopies.size() &&
        r->hdCopies[(size_t)p->texIndex].state == HdCopy::Resident) {
        texture = &r->hdCopies[(size_t)p->texIndex].tex;
        usedHd = true;
    }
    // (An HD copy evicted since prepare falls back to the original below,
    // as the old per-eye resolve did -- never a dropped draw.)
    int32_t submitMipLevels = p->mipLevels;
    if (!texture && draw->material.texture >= 0 &&
        (size_t)draw->material.texture < r->textures.size()) {
        C3dTexture& resource = r->textures[(size_t)draw->material.texture];
        // A packed original pending at prepare is made HERE, after the cull
        // above: this eye sees its batch (finishPackNow, the last resort). One
        // that failed, or found no room, earlier this frame is not tried again.
        if (!resource.valid && !resource.releasePending &&
            (!resource.source.empty() || (p->texPending && packMakeableNow(r, resource)))) {
            makeTexture(r, resource, false);
        }
        if (resource.valid) {
            texture = &resource.tex;
            submitMipLevels = resource.mipLevels;
            // The fallback is a resolve too: stamp it, or the revived
            // original is the next eviction's first victim (validator F3).
            // (The keepRgba8 pin already ran in prepareDraw, above the HD
            // branch, so it is not repeated here.)
            resource.lastUsed = ++r->drawCounter;
        }
    }
    if (!texture) {
        return;
    }
    // The recorded sampler words, written at submit as the old per-eye
    // resolve wrote them (validator F1): plain writes into the texture's
    // own parameter words, carried by the bind below.
    {
        const GPU_TEXTURE_FILTER_PARAM filter = (GPU_TEXTURE_FILTER_PARAM)p->filter;
        C3D_TexSetFilter(texture, filter, filter);
        texture->maxLevel = (u8)(p->mipped ? submitMipLevels : 0);
        C3D_TexSetFilterMipmap(texture, filter);
    }
    bindTexUnitIfChanged(r, p->texUnit, texture);
    // The fallback to the original takes the original's transform.
    const bool origUv = p->texHd && !usedHd;
    float uvScaleX = origUv ? p->origScaleX : p->scaleX;
    float uvScaleY = origUv ? p->origScaleY : p->scaleY;
    float uvOffsetX = origUv ? p->origOffsetX : p->offsetX;
    float uvOffsetY = origUv ? p->origOffsetY : p->offsetY;
    // A packed original pending at prepare had no rectangle then: it is made
    // now (above, or by the first eye), so the uv is the made texture's.
    if (p->texPending) {
        compressedUv(r->textures[(size_t)draw->material.texture], draw, &uvScaleX, &uvScaleY,
                     &uvOffsetX, &uvOffsetY);
    }
    setVsUniform(r, kDedupeUv, r->uvLocation, uvScaleX, uvScaleY, uvOffsetX, uvOffsetY);
    installAlphaTest(r, p->alphaRef, false);
    // A shadowed lit draw's stages are installShadowLitStages' (below, with
    // the light); every other draw leaves that layout first.
    if (!shadowLit) {
        leaveShadowLitStages(r);
        setTexEnv0(r, stage0Variant(r, p->stage0Index));
    }

    // THE TINT, in uniform register 5. Only the canvas asks for one; the cache
    // restores white for the first scene draw after a canvas batch so the scene
    // cannot inherit the last quad's half-transparent red.
    if (overlay) {
        const mdk::Colour& tint = draw->material.colour;
        setVsUniform(r, kDedupeTint, r->tintLocation, tint.r, tint.g, tint.b, tint.a);
        r->tintIsWhite = false;
        r->canvasDrawsThisFrame++;
    } else if (!r->tintIsWhite) {
        setVsUniform(r, kDedupeTint, r->tintLocation, 1.0f, 1.0f, 1.0f, 1.0f);
        r->tintIsWhite = true;
    }

    // The vertex data. The buffer info names the linear memory the GPU
    // reads. Two attributes (position, texture coordinate), so the
    // permutation is 0x10 -- the same shape the devkitPro examples use.
    // Sent only when the context's table names another buffer (THE DEDUPE):
    // the arena's batches are ranges of one mesh.
    bindVertexBuffer(r, mesh.data);

    r->drawCount++;
    r->triangleCount += p->vertCount / 3;
    if (r->drawCount <= 3 && drawSampleArmed()) {
        // THE FIRST DRAWS, in full -- WHEN ARMED. A draw that never happens and a draw
        // that happens with the wrong data look the same on the screen, and
        // this is the difference between them. It is armed by a file because
        // this block is 3 flushed SD writes per frame; see drawSampleArmed().
        // THE SAMPLE IS THE DRAW'S OWN FIRST VERTEX, not the mesh's. Printing
        // `mesh.data[0..2]` made every draw of a mesh log the same vertex, and
        // that is what sent batch 77 looking for a fan in a triangle its draw did
        // not even contain.
        //
        // AND IT IS BOUNDED BY THE MESH (review-1 F5). A draw whose first_vertex
        // sits at or past the mesh's own vertex count — an empty tail draw is
        // enough, and a mesh updated with fewer vertices than its capacity is
        // another — would otherwise read 20 bytes past the end of the mesh's
        // linear buffer and print them as a vertex, which is the wrong-vertex
        // attribution this sample was rewritten to stop. A diagnostic must not
        // read outside the resource it is inspecting.
        const int32_t first = draw->first_vertex >= 0 ? draw->first_vertex : 0;
        const bool sampleable = mesh.count > 0 && first + 1 <= mesh.count;
        float v[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        if (sampleable) {
            memcpy(v, static_cast<const float*>(mesh.data) + (size_t)first * 5u, sizeof(v));
        }
        mdk::log3dsPrintf("draw %d: mesh=%d first=%d count=%d tex=%d pass=%d v0=(%.1f,%.1f,%.1f) uv=(%.3f,%.3f) "
                          "frame=%d frames=%d uvt=(%.4f,%.4f,%.4f,%.4f)%s",
                          (int)r->drawCount, (int)draw->mesh, (int)draw->first_vertex,
                          (int)p->vertCount, (int)draw->material.texture,
                          (int)draw->material.pass,
                          v[0], v[1], v[2], v[3], v[4],
                          (int)draw->frame, (int)draw->material.frame_count,
                          uvScaleX, uvScaleY, uvOffsetX, uvOffsetY,
                          sampleable ? "" : " [first is past the mesh's end]");
    }
    // THE DIAGNOSTIC TRIANGLE IS DRAWN LAST, AFTER THE REAL DRAW -- see below.

    // The draw.
    //
    // THE PICA200 HAS NO LINE PRIMITIVE. Its only primitives are triangles,
    // a triangle strip and a triangle fan, so the original's outline edges
    // (the game's "glass frames") cannot be drawn as lines here. They would
    // have to be thin quads, which is a rendering decision and not a port
    // of the original's draw call. For v1 the 3DS draws the surfaces and
    // not the outlines; the picture differs from the original in that one
    // respect, and it is recorded rather than hidden.
    const int32_t first = draw->first_vertex;
    const int32_t count = p->vertCount;   // the record's (possibly folded) range

    // THE PER-DRAW MATRIX.
    //
    // `MeshDraw::world` is the object's transform, and the seam's contract is
    // that the backend places the draw with it: the Vulkan backend multiplies it
    // into the same chain in its own drawRecorded. Ignoring it here meant every
    // queued draw landed at the arena's own coordinates, so the 3DS could not
    // place an object however the script moved it (docs/OPEN_ITEMS.md, the
    // `MeshDraw::world` item).
    //
    // The composition is ONE 4x4 multiply, and only for the draws that need it.
    // The arena batches and Kurt's sprite both carry an identity world, and for
    // those the frame's matrix is already installed, so a frame of level
    // geometry costs no multiply and no uniform write; an object draw pays one
    // multiply and one write. That is the same caching the cull mode below uses,
    // for the same reason -- a per-draw C3D_* call is real ARM11 time and this
    // one dirties the uniform buffer.
    //
    // THE TWO PICA conversions are already inside r->eyeVp and they are
    // right-multiplications by fixed matrices, so composing the world against
    // the baked matrix is the same product as rebuilding it (see pica_matrix.h).
    //
    // EXCEPT FOR THE CANVAS, WHICH IS NOT IN THE WORLD AT ALL. The oracle picks
    // the transform by the pass (Renderer.cs:1073):
    //
    //     material.Pass == Pass.Overlay ? command.World : nearer * view.ViewProjection
    //
    // so an Overlay record's `world` IS the whole transform -- the orthographic
    // projection of the canvas's pixel space onto the panel, which is what
    // `pica::canvasMatrix` builds and `mdk_render_canvas_end` stores. Composing
    // it with the eye matrix would put the HUD at the arena's origin and behind
    // the camera, and it is exactly the mistake the pass is the flag for.
    float composed[16];
    const float* upload = r->eyeVp;
    if (overlay) {
        upload = draw->world;
    } else if (!p->worldIdentity) {
        mdk::pica::drawMatrix(draw->world, r->eyeVp, composed);
        upload = composed;
    }
    const bool frameMatrix = upload == r->eyeVp;
    if (!frameMatrix || !r->matrixIsFrame) {
        C3D_Mtx gpuMtx;
        mdk::pica::registers(upload, gpuMtx.m);
        // Deduped (THE DEDUPE): consecutive canvas quads share the canvas matrix.
        setVsMatrix(r, kDedupeMvp, r->mvpLocation, &gpuMtx);
        r->matrixIsFrame = frameMatrix;
    }

    // THE PASS'S CULLING, PER DRAW.
    //
    // The Vulkan backend culls only Pass::Solid (renderer_vk.cpp's pipeline
    // builder) and leaves every other pass -- DoubleSided, Mirror, Blended,
    // OnTop and Overlay -- uncullable. The oracle additionally requires a
    // mesh and a non-shadow output for back-face culling; this seam only
    // receives mesh surfaces, so those conjuncts cannot select another path
    // here. This backend has one pipeline, so the state is selected for the
    // draw that asks for it.
    //
    // Pass::DoubleSided is LOAD-BEARING for Kurt's sprite: the quad mirrors
    // `at.right` for a left-facing view, and that mirror reverses the
    // winding. With the frame's Solid cull default the mirrored sprite would
    // be a back face and vanish. The oracle's material says DoubleSided for
    // exactly this reason; do not "fix" it by emitting both windings, which
    // would double the sprite's vertex count.
    //
    // C3D_CullFace does not compare the requested mode with the current one:
    // it always dirties the effect block, which is flushed at the next draw.
    // Cache the installed mode and change it only on a pass transition. An
    // all-Solid frame then performs no per-draw cull calls at all.
    const bool solid = draw->material.pass == mdk::Pass::Solid;
    const GPU_CULLMODE wantedCull = solid ? kSolidCull : GPU_CULL_NONE;
    if (r->cullMode != wantedCull) {
        C3D_CullFace(wantedCull);
        r->cullMode = wantedCull;
    }

    // THE PASS'S DEPTH STATE, PER DRAW.
    //
    // The oracle derives it from the pass, per pipeline (Renderer.cs:606-607);
    // this backend has one program and one pipeline, so the state is selected
    // for the draw that asks for it — the same shape as the cull mode just
    // above, and the same reason: C3D_DepthTest dirties the effect block on
    // every call, so it is written only on a pass transition and an all-Solid
    // frame writes it once, at the frame's install.
    //
    // What changes in practice: `Pass::Blended` glass is tested against the
    // scene but does not write depth, so it can be seen through, and the
    // `OnTop`/`Overlay` exemptions exist. The rule itself is
    // `pica::depthStateForPass`, which the host suite holds to the oracle.
    const mdk::pica::DepthState wantedDepth =
        mdk::pica::depthStateForPass(draw->material.pass);
    if (r->depthTest != wantedDepth.test || r->depthWrite != wantedDepth.write) {
        installDepthState(r, wantedDepth);
    }

    // THE PASS'S BLEND STATE, PER DRAW -- same shape and same reason as the two
    // above: `C3D_AlphaBlend` dirties the effect block on every call, so it is
    // written on a pass transition and on nothing else. An all-Solid frame
    // writes it once, at the eye's install, and the glass that follows costs one
    // transition in and one back.
    //
    // `material.blend` is read here and not folded into the pass, because the
    // oracle keeps them independent: the pass decides WHETHER the draw mixes and
    // the blend decides HOW (Renderer.cs:565,572). A Blended pass with
    // Blend::Add is a different register pair from a Blended pass with Alpha,
    // and the queue's pass order cannot tell them apart.
    const int wantedBlendPass = static_cast<int>(draw->material.pass);
    const int wantedBlendMode = static_cast<int>(draw->material.blend);
    if (r->blendPass != wantedBlendPass || r->blendMode != wantedBlendMode) {
        installBlendState(r, draw->material.pass, draw->material.blend);
    }

    // THE LIGHT'S PROGRAM (decided with `lit` and `shadowLit` above), and
    // combiner stage 1 multiplies the texture by that light. Both switches are
    // cached.
    // THE GEOMETRY-STAGE RULE (03_frame_state.inc bindProgram): once this command
    // list has used a geometry program, an unlit draw goes through the
    // pass-through one -- the same picture, and no split to leave geometry mode.
    shaderProgram_s* const wanted =
        shadowLit ? &r->programShadow
        : lit     ? &r->programLit
                  : (r->gsInList && r->passReady && wholeTriangles ? &r->programPass : &r->program);
    if (wanted != r->boundProgram) {
        bindProgram(r, wanted);
        if (shadowLit) {
            installShadowUniforms(r);
        } else if (lit) {
            installLitUniforms(r);
        }
    }
    r->litBound = lit && !shadowLit;
    if (lit) {
        // The world position the geometry shader takes its normal from; the
        // identity (every arena batch) is written only when it is not there.
        const bool identity = p->worldIdentity;
        if (!identity || !r->worldIsIdentity) {
            C3D_Mtx worldMtx;
            mdk::pica::registers(draw->world, worldMtx.m);
            setVsMatrix(r, kDedupeWorld, r->worldLocation, &worldMtx);
            r->worldIsIdentity = identity;
        }
    }
    if (shadowLit) {
        installShadowLitStages(r, r->hazeLive);   // a Lit, non-Overlay draw: hazed when the frame is
    } else if (lit != r->lightStageOn) {
        C3D_TexEnv* light = C3D_GetTexEnv(1);
        C3D_TexEnvInit(light);
        if (lit) {
            C3D_TexEnvSrc(light, C3D_RGB, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(light, C3D_RGB, GPU_MODULATE);
            // The vertex colour carries F / 2 (mdk_c3d.g.pica): x2 lets the light
            // brighten a surface as well as darken it.
            C3D_TexEnvScale(light, C3D_RGB, GPU_TEVSCALE_2);
        }
        r->lightStageOn = lit;
    }

    // THE HAZE (SPEC.md 7.9 enableHaze) for a surface the PC hazes, nothing for
    // the rest, cached so a run of like draws writes nothing.
    // prepareHaze has already decided the toggle and the rig for the frame, so
    // the per-draw test is two enum compares behind one bool (no float compare
    // per draw: AGENTS.md R3).
    // (A shadowed lit draw's haze is its stage 3, installShadowLitStages'.)
    if (!shadowLit) {
        setHaze(r, p->haze);
    }
    if (r->boundProgram && r->boundProgram->geometryShader) {
        // The geometry shader decides the primitives (it emits each triangle).
        C3D_DrawArrays(GPU_GEOMETRY_PRIM, first, count);
    } else {
        C3D_DrawArrays(GPU_TRIANGLES, first, count);
    }

    // Restore the frame's default for the next draw, so every draw starts
    // from the state begin_frame installed. The transition back is cached in
    // the same way; repeated Solid draws do not dirty the effect block.
    if (r->cullMode != kSolidCull) {
        C3D_CullFace(kSolidCull);
        r->cullMode = kSolidCull;
    }

    // THE DIAGNOSTIC TRIANGLE IS NOT DRAWN HERE. It used to be, immediately
    // after the real draw, and that cost the frame: the triangle needs the depth
    // test OFF, so it wrote the state behind the per-draw cache's back and the
    // NEXT draw had to install its pass's state again -- two effect-block-
    // dirtying C3D_DepthTest calls per draw and one extra draw per real draw,
    // all of it multiplied again by the eye count (review-1 F3). It now runs
    // once per eye from mdk_render_end_frame; see drawDiagnostic.
}

// THE DIAGNOSTIC TRIANGLE, once per eye, after that eye's replay. Clip-space,
// with the identity matrix, no texture and no depth test: if THIS does not
// appear on a screen, the problem is in the frame or the program, not in the
// mesh, the transform or the texture.
//
// WHY IT MOVED OUT OF THE PER-DRAW PATH (review-1 F3). Per-draw it cost two
// C3D_DepthTest calls and one extra draw for every real draw, and the per-eye
// replay multiplies that by the number of eyes: on the pinned scene's frame 1
// (154 Solid + 1 DoubleSided, docs/OPEN_ITEMS.md) that was ~310 depth writes
// and 155 diagnostic draws per eye, where one eye now pays 2 and 1. Moving it
// also makes it a better instrument, not merely a cheaper one:
//
//   * It now runs on a frame that queued NOTHING. Inside the per-draw path it
//     could not -- no draw, no diagnostic -- and an empty frame is exactly where
//     "is the frame and the program alive at all" is the only question left.
//   * Nothing real is drawn after it inside an eye, so the state it leaves
//     behind (the context's single BufInfo and TexEnv, the identity uniform, the
//     test off) cannot damage a draw of this frame. That was not theory: the
//     vault's hardware-CONFIRMED pattern c3d-interleaved-draw-leaves-vertex-
//     uniforms-stale.md is this hazard class -- an interleaved draw that switches
//     vertex streams leaves the VERTEX UNIFORMS stale, and the victim is a
//     later, unrelated draw that lands off-screen while every state probe reads
//     correct. The next installEye cleans up after the diagnostic, and
//     installEye writes every uniform and every piece of fixed state anyway
//     (AGENTS.md P2).
//
// IT STILL MUST COME AFTER THE REAL DRAWS, NOT BEFORE THEM. citro3d hands out
// the CONTEXT's single BufInfo and TexEnv rather than a copy -- C3D_GetBufInfo()
// is called here and never handed back with C3D_SetBufInfo, so the two calls
// return the SAME struct. Drawn first, the diagnostic replaced the buffer
// description the real draw had just installed and the real draw read the
// triangle's 15 floats at the mesh's offsets -- invisible geometry, with the
// diagnostic itself on screen to say the frame was fine (batch 28's defect).
static void drawDiagnostic(RendererC3D* r, float canvasWidth) {
    // ARMED BY A FILE, NOT DRAWN IN EVERY BUILD. The (0,0,255) square is the
    // harness's proof that a canvas-state draw reaches the panel
    // (`emurun.sh --expect hud*` seeds sdmc:/3ds/mdk/canary.txt before launch).
    // Unarmed it was a blue square in the top-left of every frame a player
    // would see (2026-11-17, hardware and emulator captures).
    if (!canaryArmed()) {
        return;
    }
    if (!r->diagnosticTriangle || !r->scratch) {
        return;
    }
    // THE CANARY, AND WHY IT IS NOT THE TRIANGLE IT USED TO BE.
    //
    // What shipped here was a white triangle covering most of the clip square,
    // drawn through an identity matrix, whose stated job was "the frame and the
    // program are fine". Two problems, both found the hard way while chasing the
    // HUD: it never painted (so it proved nothing), and the moment anything
    // downstream of it started working it would have flashed the whole panel
    // WHITE. A diagnostic that cannot be seen and cannot be trusted is worse than
    // none, and one that blanks the game when the bug it watches is fixed is a
    // trap.
    //
    // So this is now a 16x16 marker in the canvas's OWN pixel space, through the
    // canvas's OWN matrix, with the canvas's OWN texture, combiner, tint path,
    // cull and depth state. That makes it the smallest possible proof of the one
    // question the HUD row is actually blocked on — "does a draw in the canvas's
    // state, in the canvas's space, reach the panel?" — and nothing more. It is
    // the question `emurun.sh --expect hud-visible` asks, and the answer today is
    // no: measured 2026-11-11, the panel contains zero pixels of the canary
    // colour, with the scene replayed, with the scene replayed and the canary
    // drawn first, and with the replay skipped so the canary is the only draw in
    // the frame.
    //
    // THE COLOUR IS THE CONTRACT. Pure blue (0,0,255) is not a colour the level
    // produces: the whole level-7 capture contains no exact (0,0,255) pixel, so
    // one in the panel can only have come from here. That is what lets a gate read
    // visibility out of a screenshot without a HUD-off reference build, which does
    // not exist and would need a renderer switch this project does not have.
    //
    // WHAT THE MEASUREMENTS RULED OUT, and they are here so the next batch does
    // not re-run them: the queue order (the canary alone, with the scene replay
    // skipped, is still invisible); the tint and GPU_PRIMARY_COLOR (the scene
    // rendered normally with its combiner switched to MODULATE(tex, PRIMARY), so
    // the primary arrives white with alpha 1); the combiner constant; the command
    // buffer (usage 0.101 at the canvas draw, and the canvas draws are #137-139
    // of ~150); the vertex data and the buffer addresses (the canvas mesh's first
    // corner reads back as (500.00, 287.00, 0.00) and every buffer sits in one
    // linear region); the depth state, the cull mode and the winding; and a
    // known-good STATIC scene mesh drawn at the tail through the frame's own
    // eyeVp, which is ALSO invisible — so the fault is not the canvas's geometry
    // and not its matrix either.
    float* marker = static_cast<float*>(r->scratch);
    static const float kSize = 16.0f;
    const float x1 = kSize;
    const float y1 = kSize;
    // pos xyz, uv — the canvas's top-left corner, y down like every other canvas
    // quad, and the uv the white texture does not care about.
    const float v[6][5] = {{0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
                           {x1,   0.0f, 0.0f, 1.0f, 0.0f},
                           {x1,   y1,   0.0f, 1.0f, 1.0f},
                           {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
                           {x1,   y1,   0.0f, 1.0f, 1.0f},
                           {0.0f, y1,   0.0f, 0.0f, 1.0f}};
    for (int i = 0; i < 6; i++) {
        for (int c = 0; c < 5; c++) {
            marker[i * 5 + c] = v[i][c];
        }
    }
    // THE SAME CACHE VISIBILITY AS `mdk_render_update_mesh`, and this one cannot
    // be hidden by a level load: the scratch is rewritten on the frame that draws
    // it, every frame, so without the clean the canary is the one thing this
    // backend is guaranteed never to show. Flushing here is what makes the
    // canary answer the question it was built to ask.
    GSPGPU_FlushDataCache(marker, (u32)sizeof(v));

    // THE CANVAS'S MATRIX, not the identity. The identity put the marker's 16
    // canvas pixels at clip 16, which is off the edge of the panel, and an
    // off-panel draw proves nothing about the HUD.
    float screen[16];
    mdk::pica::canvasMatrix(screen, canvasWidth);
    C3D_Mtx gpuMtx;
    mdk::pica::registers(screen, gpuMtx.m);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->mvpLocation, &gpuMtx);

    C3D_BufInfo* info = C3D_GetBufInfo();
    BufInfo_Init(info);
    BufInfo_Add(info, marker, 5 * sizeof(float), 2, 0x10);

    // THE CANVAS'S STATE, each of it the same call drawRecorded makes for an
    // Overlay quad -- except the uv uniform below, which stays the identity: the
    // marker samples the 8x8 all-white texture, where the last-row flip that
    // drawRecorded applies cannot change a texel.
    if (r->canvasWhite >= 0 && (size_t)r->canvasWhite < r->textures.size()) {
        bindTexUnit(r, 0, &r->textures[(size_t)r->canvasWhite].tex);
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, r->uvLocation, 1.0f, 1.0f, 0.0f, 0.0f);
    C3D_TexEnv* plain = C3D_GetTexEnv(0);
    C3D_TexEnvInit(plain);
    C3D_TexEnvSrc(plain, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(plain, C3D_Both, GPU_MODULATE);
    C3D_CullFace(GPU_CULL_NONE);
    r->cullMode = GPU_CULL_NONE;
    installDepthState(r, mdk::pica::DepthState{false, false});
    C3D_FVUnifSet(GPU_VERTEX_SHADER, r->tintLocation, 0.0f, 0.0f, 1.0f, 1.0f);
    r->tintIsWhite = false;
    // The canvas takes no haze and no light, and the last queued draw may have
    // left either on -- or the shadowed lit draw's whole layout.
    leaveShadowLitStages(r);
    setHaze(r, false);
    bindProgram(r, &r->program);
    r->litBound = false;
    if (r->lightStageOn) {
        C3D_TexEnvInit(C3D_GetTexEnv(1));
        r->lightStageOn = false;
    }
    C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
    // The uniform now holds the canvas's matrix, not the frame's. The next
    // installEye -- the other eye, or this one's next frame -- is what puts it
    // back, and it writes the uniform unconditionally.
    r->matrixIsFrame = false;
}

