// 06_draw.inc -- recording one queued draw, the diagnostic, the queue replay.
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

// THE PER-DRAW SAMPLE IS ARMED BY A FILE, NEVER ALWAYS ON.
//
// This block writes THREE lines per frame, and log3dsPrintf fflush()es each one,
// so at 60 fps it is 180 SD writes a second inside the draw path. That is the
// one diagnostic in this backend that contends with the frames it describes:
// the vault's 3ds-autonomous-hardware-profiling-loop runbook names exactly this
// ("Kill periodic diagnostic SD writes ... it murders anything doing ROM
// streaming off the same SD"), and AGENTS.md P5 keeps verbose logging out of
// frame-critical work. It also put the only unguarded blocking file write in
// the loop, at 3/frame, for the whole run -- the device log from the 2026-11-17
// freeze ends MID-LINE in this block, which is what a stop inside fflush() looks
// like. So it is off unless the card asks for it:
//
//     touch sdmc:/3ds/mdk/drawlog.txt
//
// Absent, the cost is one failed open() at the first draw and nothing per frame.
// The check is once, not per draw, because the file is placed before the run.
static bool drawSampleArmed() {
    static int armed = -1;
    if (armed < 0) {
        FILE* file = fopen("sdmc:/3ds/mdk/drawlog.txt", "rb");
        armed = file ? 1 : 0;
        if (file) {
            fclose(file);
            mdk::log3dsPrintf("draw: sample logging ARMED by sdmc:/3ds/mdk/drawlog.txt"
                              " (3 lines/frame, flushed)");
        }
    }
    return armed != 0;
}

static void drawRecorded(RendererC3D* r, const mdk::MeshDraw* draw) {
    if (draw->mesh < 0 || (size_t)draw->mesh >= r->meshes.size()) {
        return;
    }
    const C3dMesh& mesh = r->meshes[(size_t)draw->mesh];
    if (!mesh.valid || mesh.count <= 0) {
        return;
    }
    if (draw->first_vertex < 0 || draw->vertex_count <= 0 ||
        draw->first_vertex + draw->vertex_count > mesh.count) {
        return;
    }

    if ((renderDebugFlags() & kRenderDebugUvLog) && r->frame == 30 && mesh.data != nullptr &&
        draw->material.texture >= 0 && (size_t)draw->material.texture < r->textures.size()) {
        // rdebug `uvlog`: which draws TILE (uv outside [0,1]) and on what size.
        static int32_t uvLogged = 0;
        const float* f = static_cast<const float*>(mesh.data) + (size_t)draw->first_vertex * 5u;
        float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
        for (int32_t i = 0; i < draw->vertex_count; i++) {
            const float u = f[(size_t)i * 5u + 3u], v = f[(size_t)i * 5u + 4u];
            if (u < u0) u0 = u;
            if (u > u1) u1 = u;
            if (v < v0) v0 = v;
            if (v > v1) v1 = v;
        }
        const C3dTexture& t = r->textures[(size_t)draw->material.texture];
        if ((u0 < -0.01f || u1 > 1.01f || v0 < -0.01f || v1 > 1.01f) && uvLogged < 80) {
            uvLogged++;
            mdk::log3dsPrintf("uvlog: tex=%d %dx%d padded %dx%d frames=%d pass=%d u=[%.2f,%.2f] v=[%.2f,%.2f] verts=%d",
                              (int)draw->material.texture, (int)t.realWidth, (int)t.realHeight,
                              (int)t.paddedWidth, (int)t.paddedHeight, (int)draw->material.frame_count,
                              (int)draw->material.pass, u0, u1, v0, v1, (int)draw->vertex_count);
        }
    }

    // The texture. A surface with no texture is a flat colour, and it is
    // drawn with a ONE-PIXEL texture of that colour: the PICA200's
    // combiner has no constant-colour source for a vertex shader's output,
    // so a colour has to come from a texture.
    C3D_Tex* texture = nullptr;
    if (draw->material.texture >= 0 && (size_t)draw->material.texture < r->textures.size()) {
        C3dTexture& resource = r->textures[(size_t)draw->material.texture];
        // A released texture whose GPU copy is waiting for the next frame
        // boundary must not be drawn again: the caller has given the id up.
        if (!resource.releasePending) {
            if (!resource.valid && !resource.source.empty()) {
                // THE EVICTION IS REVERSIBLE. The texture was made again from
                // the source the backend kept: the caller's id is still good,
                // and the picture comes back with it.
                makeTexture(r, resource, false);
            }
            if (resource.valid) {
                texture = &resource.tex;
                // THE EVICTION ORDER. The texture is stamped every time it is
                // drawn, so the least recently used one is the one that goes
                // when the VRAM is full.
                resource.lastUsed = ++r->drawCounter;
            }
        }
    }
    if (!texture) {
        return;
    }
    C3D_TexBind(0, texture);

    // THE UV TRANSFORM, in uniform register 4.
    //
    //     uv' = uv * scale + offset
    //
    // The scale undoes the padding: the real width over the padded width.
    // The offset picks the frame of an animated texture.
    //
    // The frames are stacked VERTICALLY, and the original's shader picks
    // one with `uv.y = (frac(uv.y) + frame) / frame_count`, so the frame's
    // strip is `frameHeight` rows tall out of the padded height, and the
    // frame's origin is `frame * frameHeight` rows down.
    const C3dTexture& resource = r->textures[(size_t)draw->material.texture];
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    if (resource.paddedWidth > 0 && resource.paddedHeight > 0) {
        // R11: the quotient is cached on the texture. frameCount is a
        // property of the texture itself (material_resolver sets it from
        // texture->frameCount()), so one cached value is exact for every
        // batch that uses this texture.
        C3dTexture& mutableResource = r->textures[(size_t)draw->material.texture];
        if (mutableResource.frameHeight <= 0) {
            const int32_t frameCount = draw->material.frame_count > 0 ? draw->material.frame_count : 1;
            /* @vfp-exempt R11 — quotient cached on the texture: this divide runs at most once per texture lifetime, not per draw; the per-frame path below reads the cached int. measured one __aeabi_idivmod per texture. Verified 2026-10-21. */
            mutableResource.frameHeight = mutableResource.realHeight > 0 ? mutableResource.realHeight / frameCount : 1;
        }
        const int32_t frameHeight = mutableResource.frameHeight;
        scaleX = (float)resource.realWidth / (float)resource.paddedWidth;
        scaleY = (float)frameHeight / (float)resource.paddedHeight;
        offsetY = (float)(draw->frame * frameHeight) / (float)resource.paddedHeight;
    }
    if (draw->material.pass != mdk::Pass::Overlay) {
        // The PICA's t runs from the LAST memory row (pica::sceneUvFromTop has
        // the rule, the measurement and why the canvas is exempt).
        const mdk::pica::SceneUv flipped = mdk::pica::sceneUvFromTop(scaleY, offsetY);
        scaleY = flipped.scaleY;
        offsetY = flipped.offsetY;
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, r->uvLocation, scaleX, scaleY, offsetX, offsetY);

    // THE COMBINER, and with it the question of where a draw's colour comes from.
    //
    // A scene draw: the texture's colour, with its own alpha (so index 0 is
    // transparent), replacing the primary colour. The world is unlit and untinted
    // -- the palette decides the colour -- so there is nothing else to multiply.
    //
    // A canvas draw (Pass::Overlay): the texture's colour TIMES the primary
    // colour, which is the tint uniform. That is the oracle's `color * tint`
    // (shaders/palette.hlsl), and it covers both kinds of canvas quad: an image
    // faded by an alpha (the skull) and a flat rectangle, which is drawn on white
    // so the tint is the whole colour. The alpha channel gets the same multiply,
    // which is what makes the fade a fade: the frame-wide alpha test then still
    // discards the texels the palette made transparent, because 0 times any tint
    // is 0.
    //
    // WHY THE SCENE KEEPS REPLACE AND DOES NOT just always MODULATE: white times
    // the texture is the same colour in arithmetic, but every capture this
    // backend has ever measured was taken with REPLACE installed, and a combiner
    // op is not the place to spend a fidelity question. The cost of the split is
    // nothing: `C3D_GetTexEnv` marks the stage dirty on every call (measured from
    // the linked libcitro3dd.a: it sets bit 26+id of the context's dirty word),
    // so this block already re-sends the combiner on every draw either way.
    const bool overlay = draw->material.pass == mdk::Pass::Overlay;
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    if (overlay) {
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
    } else if (renderDebugFlags() & kRenderDebugFlat) {
        // rdebug `flat`: the texture is ignored; a constant colour per pass
        // (Solid red, DoubleSided green, Blended blue, others yellow), opaque.
        uint32_t c = 0xFF00FFFFu;  // ABGR: yellow
        if (draw->material.pass == mdk::Pass::Solid) c = 0xFF0000FFu;
        else if (draw->material.pass == mdk::Pass::DoubleSided) c = 0xFF00FF00u;
        else if (draw->material.pass == mdk::Pass::Blended) c = 0xFFFF0000u;
        C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_TexEnvColor(env, c);
    } else {
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    }

    // THE TINT, in uniform register 5. Only the canvas asks for one; the cache
    // restores white for the first scene draw after a canvas batch so the scene
    // cannot inherit the last quad's half-transparent red.
    if (overlay) {
        const mdk::Colour& tint = draw->material.colour;
        C3D_FVUnifSet(GPU_VERTEX_SHADER, r->tintLocation, tint.r, tint.g, tint.b, tint.a);
        r->tintIsWhite = false;
        r->canvasDrawsThisFrame++;
    } else if (!r->tintIsWhite) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, r->tintLocation, 1.0f, 1.0f, 1.0f, 1.0f);
        r->tintIsWhite = true;
    }

    // The vertex data. The buffer info names the linear memory the GPU
    // reads. Two attributes (position, texture coordinate), so the
    // permutation is 0x10 -- the same shape the devkitPro examples use.
    C3D_BufInfo* bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
    BufInfo_Add(bufInfo, mesh.data, 5 * sizeof(float), 2, 0x10);

    r->drawCount++;
    r->triangleCount += draw->vertex_count / 3;
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
                          (int)draw->vertex_count, (int)draw->material.texture,
                          (int)draw->material.pass,
                          v[0], v[1], v[2], v[3], v[4],
                          (int)draw->frame, (int)draw->material.frame_count,
                          scaleX, scaleY, offsetX, offsetY,
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
    const int32_t count = draw->vertex_count;

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
    } else if (!mdk::pica::isIdentity(draw->world)) {
        mdk::pica::drawMatrix(draw->world, r->eyeVp, composed);
        upload = composed;
    }
    const bool frameMatrix = upload == r->eyeVp;
    if (!frameMatrix || !r->matrixIsFrame) {
        C3D_Mtx gpuMtx;
        mdk::pica::registers(upload, gpuMtx.m);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->mvpLocation, &gpuMtx);
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

    // THE ONE-FRAME GEOMETRY CENSUS (see the `Census` struct). Cold by
    // construction: `census.active` is true for exactly one frame of the
    // process, so every other frame pays one integer compare and this loop runs
    // once in a run.
    //
    // It measures, in the SAME matrix the GPU is about to receive, where each of
    // this draw's triangles lands and which way it winds. Batch 77's single
    // triangle could not separate "the room is not queued" from "the room is
    // queued and the GPU discards it", and could not say what the room's winding
    // is, which is what the cull enum turns on.
    if (r->census.active && count >= 3) {
        float cm[16];
        memcpy(cm, upload, sizeof(cm));
        const float* cv = static_cast<const float*>(mesh.data);
        const int32_t tris = count / 3;
        for (int32_t t = 0; t < tris; t++) {
            float nx[3];
            float ny[3];
            float nz[3];
            float w[3];
            bool behind = false;
            for (int32_t v = 0; v < 3; v++) {
                const int32_t at = (first + t * 3 + v) * 5;
                const float x = cv[at];
                const float y = cv[at + 1];
                const float z = cv[at + 2];
                const float cx = x * cm[0] + y * cm[4] + z * cm[8] + cm[12];
                const float cy = x * cm[1] + y * cm[5] + z * cm[9] + cm[13];
                const float cz = x * cm[2] + y * cm[6] + z * cm[10] + cm[14];
                const float cw = x * cm[3] + y * cm[7] + z * cm[11] + cm[15];
                if (cw <= 0.0f) {
                    behind = true;
                }
                w[v] = cw;
                const float inv = cw != 0.0f ? 1.0f / cw : 0.0f;
                nx[v] = cx * inv;
                ny[v] = cy * inv;
                nz[v] = cz * inv;
            }
            Census& c = r->census;
            c.triangles++;
            float wmin = w[0];
            float wmax = w[0];
            float wsum = 0.0f;
            for (int32_t v = 0; v < 3; v++) {
                if (w[v] < wmin) {
                    wmin = w[v];
                }
                if (w[v] > wmax) {
                    wmax = w[v];
                }
                wsum += w[v];
            }
            if (c.triangles == 1 || wmin < c.minW) {
                c.minW = wmin;
            }
            if (c.triangles == 1 || wmax > c.maxW) {
                c.maxW = wmax;
            }
            if (behind) {
                c.behind++;
                continue;
            }
            float lox = nx[0];
            float hix = nx[0];
            float loy = ny[0];
            float hiy = ny[0];
            for (int32_t v = 1; v < 3; v++) {
                if (nx[v] < lox) {
                    lox = nx[v];
                }
                if (nx[v] > hix) {
                    hix = nx[v];
                }
                if (ny[v] < loy) {
                    loy = ny[v];
                }
                if (ny[v] > hiy) {
                    hiy = ny[v];
                }
            }
            if (hix < -1.0f || lox > 1.0f || hiy < -1.0f || loy > 1.0f) {
                c.offscreen++;
                continue;
            }
            // The depth half, counted separately rather than folded into onscreen:
            // a triangle inside the clip SQUARE in x/y but entirely past the far
            // plane (or in front of the near one) is not something the GPU will
            // show, and the census's on-screen count was read that way (review-1
            // F5). The PICA's depth map decides which side of [-1,1] is "in", so
            // this is reported as a range and not as a verdict.
            float loz = nz[0];
            float hiz = nz[0];
            for (int32_t v = 1; v < 3; v++) {
                if (nz[v] < loz) {
                    loz = nz[v];
                }
                if (nz[v] > hiz) {
                    hiz = nz[v];
                }
            }
            if (hiz < -1.0f || loz > 1.0f) {
                c.depthOut++;
            }
            const float area = (nx[1] - nx[0]) * (ny[2] - ny[0]) -
                               (ny[1] - ny[0]) * (nx[2] - nx[0]);
            c.onscreen++;
            if (area < 0.0f) {
                c.cw++;
            } else {
                c.ccw++;
            }
            c.areaSum += area < 0.0f ? -area : area;
            if (lox < c.loX) { c.loX = lox; }
            if (hix > c.hiX) { c.hiX = hix; }
            if (loy < c.loY) { c.loY = loy; }
            if (hiy > c.hiY) { c.hiY = hiy; }
            const float wc = wsum / 3.0f;
            if (wc < 60.0f) {
                c.near60++;
                if (lox < c.nLoX) { c.nLoX = lox; }
                if (hix > c.nHiX) { c.nHiX = hix; }
                if (loy < c.nLoY) { c.nLoY = loy; }
                if (hiy > c.nHiY) { c.nHiY = hiy; }
            }
            if (c.bestW < 0.0f || wc < c.bestW) {
                c.bestW = wc;
                c.bestDraw = (int32_t)r->drawCount;
                c.bestFirst = first + t * 3;
                c.bestArea = area;
                for (int32_t v = 0; v < 3; v++) {
                    const int32_t at = (c.bestFirst + v) * 5;
                    c.bestV[v * 3 + 0] = cv[at];
                    c.bestV[v * 3 + 1] = cv[at + 1];
                    c.bestV[v * 3 + 2] = cv[at + 2];
                }
                c.bestNdc[0] = nx[0];
                c.bestNdc[1] = ny[0];
                c.bestNdc[2] = nx[1];
                c.bestNdc[3] = ny[1];
            }
        }
    }

    C3D_DrawArrays(GPU_TRIANGLES, first, count);
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
    // Overlay quad.
    if (r->canvasWhite >= 0 && (size_t)r->canvasWhite < r->textures.size()) {
        C3D_TexBind(0, &r->textures[(size_t)r->canvasWhite].tex);
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
    C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
    // The uniform now holds the canvas's matrix, not the frame's. The next
    // installEye -- the other eye, or this one's next frame -- is what puts it
    // back, and it writes the uniform unconditionally.
    r->matrixIsFrame = false;
}

// One replay of everything the frame has queued, in the oracle's pass order
// (draw_queue.h), into whatever target is current. It is separate from the
// frame end because a stereo frame replays the SAME queue once per eye: the
// queue already IS the recorded draw list, so the second eye is a replay with a
// different matrix and target, not a second walk of the scene.
//
// THE ONLY REPLAY POINT IS mdk_render_end_frame, once per eye. A mid-frame
// replay is not a harmless early flush: it emits the frame's records an extra
// time into whatever target installEye last selected, and a flushDraws-shaped
// one (this replay plus a reset) consumes the records as well, so the eye loop
// that follows would replay an empty queue. That is why the canvas and inset
// seams below do not flush (review-1 F4) -- and, since review-3 F1, why none of
// them may draw by itself either: an immediate C3D_DrawArrays in a seam is
// outside the per-eye replay, so it lands in one target or in none.
static void replayQueue(RendererC3D* r) {
    if (!r || r->queue.empty() || !r->inFrame) {
        return;
    }
    for (int32_t pass = 0; pass < mdk::kDrawQueuePassCount; pass++) {
        const std::vector<mdk::MeshDraw>& bucket = r->queue.bucket(pass);
        for (size_t i = 0; i < bucket.size(); i++) {
            drawRecorded(r, &bucket[i]);
        }
    }
}

 void mdk_render_draw_mesh(MdkRenderer rr, const mdk::MeshDraw* draw) {
    RendererC3D* r = asC3d(rr);
    if (!r || !draw || !r->inFrame) {
        return;
    }
    // Queued, not recorded: the oracle renders the frame's commands one pass at
    // a time, and render.h gives that ordering job to the backend. With the
    // depth test off on this target, the order IS the occlusion, so this is not
    // a tidiness change.
    r->queue.add(*draw);
}

