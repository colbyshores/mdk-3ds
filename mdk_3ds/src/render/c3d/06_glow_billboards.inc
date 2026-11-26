// 06_glow_billboards.inc -- the glow billboards (Enhanced Mode enableGlowBillboards).
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

// THE GLOW BILLBOARDS (Enhanced Mode enableGlowBillboards, the 3DS default:
// owner decision 2026-11-18, the PC keeps its bloom). The game hands over a
// halo for each sprite effect the PC's bloom would catch (EffectsView: its
// texels past the bloom's threshold, their colour at the bloom's gain); here
// each is ONE quad, additive (SRC_ALPHA, ONE), depth-tested and not written,
// textured with a soft dot (white, alpha falling off from the centre) and
// tinted with its colour: texture x tint in the combiner. No target switch, no
// transfer, no split -- the recipe of tsunami2010's soft dots (the vault's
// 3ds-citra-mandarine-no-geometry-shader-use-cpu-quads.md), measured there on
// hardware. Nothing on screen glows: nothing is drawn, and nothing is paid.
// Once per eye, where the bloom would run (replayQueue, before the canvas).
static bool ensureGlowBillboards(RendererC3D* r) {
    if (!r->glowHaloReady) {
        r->glowHaloReady = C3D_TexInit(&r->glowHalo, 32, 32, GPU_RGBA8);
        if (!r->glowHaloReady) {
            return false;
        }
        uint8_t* texels = static_cast<uint8_t*>(r->glowHalo.data);
        for (int y = 0; y < 32; y++) {
            for (int x = 0; x < 32; x++) {
                const float dx = ((float)x + 0.5f) / 16.0f - 1.0f;
                const float dy = ((float)y + 0.5f) / 16.0f - 1.0f;
                float fall = 1.0f - sqrtf(dx * dx + dy * dy);
                fall = fall < 0.0f ? 0.0f : fall;
                // White, the alpha falling off with the distance squared (a soft core).
                const uint8_t rgba[4] = {0xFF, 0xFF, 0xFF, (uint8_t)(fall * fall * 255.0f + 0.5f)};
                const size_t slot = mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, 32u);
                mdk::pica::texelBytes(texels + slot * 4u, rgba[0], rgba[1], rgba[2], rgba[3]);
            }
        }
        C3D_TexFlush(&r->glowHalo);
        C3D_TexSetFilter(&r->glowHalo, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&r->glowHalo, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    }
    if (!r->glowQuadMesh) {
        r->glowQuadMesh = static_cast<float*>(
            linearAlloc((size_t)RendererC3D::kMaxGlowQuads * 6u * 5u * sizeof(float)));
    }
    return r->glowQuadMesh != nullptr;
}

static void drawGlowBillboards(RendererC3D* r) {
    if (r->glowQuadCount <= 0 || !ensureGlowBillboards(r)) {
        return;
    }
    // The vertices, once a frame (both eyes draw the same world-space quads,
    // each through its own matrix): x y z u v, as every mesh here.
    if (r->glowQuadMeshFrame != r->frame) {
        for (int32_t q = 0; q < r->glowQuadCount; q++) {
            for (int32_t v = 0; v < 6; v++) {
                const mdk::Vertex& c = r->glowQuads[q].corners[v];
                float* out = r->glowQuadMesh + ((size_t)q * 6u + (size_t)v) * 5u;
                out[0] = c.pos.x;
                out[1] = c.pos.y;
                out[2] = c.pos.z;
                out[3] = c.uv.x;
                out[4] = c.uv.y;
            }
        }
        GSPGPU_FlushDataCache(r->glowQuadMesh, (u32)r->glowQuadCount * 6u * 5u * sizeof(float));
        r->glowQuadMeshFrame = r->frame;
    }
    leaveShadowLitStages(r);
    // Whole triangles: through the pass-through program in a geometry list.
    shaderProgram_s* const wanted =
        r->gsInList && r->passReady ? &r->programPass : &r->program;
    if (wanted != r->boundProgram) {
        bindProgram(r, wanted);
    }
    r->litBound = false;
    if (!r->matrixIsFrame) {
        C3D_Mtx gpuMtx;
        mdk::pica::registers(r->eyeVp, gpuMtx.m);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->mvpLocation, &gpuMtx);
        r->matrixIsFrame = true;
    }
    setVsUniform(r, kDedupeUv, r->uvLocation, 1.0f, 1.0f, 0.0f, 0.0f);   // a symmetric dot
    setHaze(r, false);
    if (r->lightStageOn) {
        C3D_TexEnvInit(C3D_GetTexEnv(1));
        r->lightStageOn = false;
    }
    bindTexUnit(r, 0, &r->glowHalo);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
    installAlphaTest(r, 0, false);
    installDepthState(r, mdk::pica::DepthState{true, false});
    installBlendState(r, mdk::Pass::Blended, mdk::Blend::Add);
    if (r->cullMode != GPU_CULL_NONE) {
        C3D_CullFace(GPU_CULL_NONE);
        r->cullMode = GPU_CULL_NONE;
    }
    bindVertexBuffer(r, r->glowQuadMesh);
    // The glowStrength dial (config / the pause menu): the halo's colour scaled
    // in the tint uniform it is already drawn with. 100 is 1.0f exactly, and
    // x * 1.0f is x: the shipped halos to the bit.
    const float strength = (float)r->toggles.glowStrength / 100.0f;
    // ONE DRAW PER RUN of adjacent halos with the same colour bits: the same tint
    // uniform, the same triangles in the same order, so the same pixels.
    for (int32_t q = 0; q < r->glowQuadCount;) {
        const float* c = r->glowQuads[q].colour;
        int32_t run = 1;
        while (q + run < r->glowQuadCount &&
               memcmp(r->glowQuads[q + run].colour, c, 3 * sizeof(float)) == 0) {
            run++;
        }
        setVsUniform(r, kDedupeTint, r->tintLocation, c[0] * strength, c[1] * strength,
                     c[2] * strength, 1.0f);
        if (r->boundProgram && r->boundProgram->geometryShader) {
            C3D_DrawArrays(GPU_GEOMETRY_PRIM, q * 6, run * 6);
        } else {
            C3D_DrawArrays(GPU_TRIANGLES, q * 6, run * 6);
        }
        r->billboardsDrawn += run;
        q += run;
    }
    r->tintIsWhite = false;
}
