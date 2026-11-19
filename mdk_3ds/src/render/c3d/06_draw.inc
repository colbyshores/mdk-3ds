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

// The haze's two halves, switched together: the shader's exponent (each vertex
// keeps ex2(exponent * eye depth) of its colour, in the primary alpha) and
// combiner stage 2 (after the light's stage 1), which blends the previous stage
// toward the haze colour by
// that alpha -- INTERPOLATE is src0 * src2 + src1 * (1 - src2). Off, the uniform
// is 0 (ex2(0) = 1) and the stage passes its input through. Cached on hazeOn;
// installEye resets both every eye.
//
// THE FOG UNIT'S HAZE (config hazeFog, fogLive): the same decision, made by the
// PICA's fixed-function fog instead -- the uniform stays 0 and stage 2 a
// passthrough, and the fog mode follows `on` (setFog).
static void setHaze(RendererC3D* r, bool on) {
    if (r->fogLive) {
        setFog(r, on);
        return;
    }
    if (on == r->hazeOn) {
        return;
    }
    C3D_TexEnv* stage = C3D_GetTexEnv(2);
    C3D_TexEnvInit(stage);
    if (on) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, r->hazeLocation, r->hazeExponent, 0.0f, 0.0f, 0.0f);
        C3D_TexEnvSrc(stage, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
        C3D_TexEnvOpRgb(stage, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                        GPU_TEVOP_RGB_SRC_ALPHA);
        C3D_TexEnvFunc(stage, C3D_RGB, GPU_INTERPOLATE);
        C3D_TexEnvColor(stage, mdk::pica::hazeColourWord(r->lighting.haze_colour));
    } else {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, r->hazeLocation, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    r->hazeOn = on;
}

// The fog unit on or off (the haze through the fog unit, pica_matrix.h): mixes
// the haze colour (C3D_FogColor, installEye) into the combiner's output by
// 1 - the table (C3D_FogLutBind, installEye) at the fragment's W-buffer depth.
// Cached on fogOn; installEye writes it off every eye, and every pass that is
// not a hazed scene draw turns it off (the canvas and the diagnostic through
// setHaze, the glow, the billboards, the caster pass, the shadow tests).
static void setFog(RendererC3D* r, bool on) {
    if (on == r->fogOn) {
        return;
    }
    C3D_FogGasMode(on ? GPU_FOG : GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    r->fogOn = on;
}

// THE SHADOWED LIT DRAW'S COMBINER (Enhanced Mode enableSkyShadows, one pass:
// mdk_litshadow.g.pica). Units: 0 the shadow map (the only unit that compares
// a shadow texture: 1 in the light, 0 in the shadow, in every channel), 1 the
// surface, 2 the shadow tint (F_shadow / 2 in rgb, the edge fade's survival in
// alpha). The vertex colour is F_lit / 2 with the haze's kept fraction in alpha.
//
//   stage 0  A = (1 - shadow) * survive            how much shadow lands here
//   stage 1  RGB = tint * A + F_lit * (1 - A)      mix(F_lit, F_shadow, A) / 2
//            A = the surface's alpha               (the alpha test's)
//   stage 2  RGB = surface * previous, x2          the light, as stage 1 is
//                                                  for an unshadowed lit draw
//   stage 3  the haze, by the vertex alpha         as setHaze's stage 2
//
// what the old two passes made (the lit draw, then a depth-EQUAL redraw that
// multiplied the picture by mix(1, F_shadow / F_lit, A)) in one draw. Stages
// 0-2 and the units are written once per run of these draws; stage 3 follows
// the haze, cached on shadowLitHaze. The caches of the ordinary layout
// (lightStageOn, hazeOn) are made to say "not installed" while it is here.
//
// THE SCALAR LAYOUT (config hazeFog, shadowScalar: mdk_litshadowq.g.pica). The
// haze is the fog unit's, so the vertex alpha carries q = survive * (1 - R), R
// the triangle's luma ratio F_shadow / F_lit, and there is no lookup: units 0
// (the map) and 1 (the surface) only -- unit 2 keeps the idle texture.
//
//   stage 0  RGB = F_lit / 2, A = (1 - shadow) * q
//   stage 1  RGB = previous * (1 - A)              F_lit * (1 - A) / 2
//            A = the surface's alpha               (the alpha test's)
//   stage 2  RGB = surface * previous, x2          (clamped after the shadow,
//                                                  as the tint layout's is)
//
// With the fog live the haze is the fog unit's in either layout, and stage 3
// stays a passthrough.
static void installShadowLitStages(RendererC3D* r, bool haze) {
    if (!r->shadowLitStages && r->shadowScalar) {
        C3D_TexBind(0, &r->shadowTex);
        C3D_TexShadowParams(false, 0.0f);   // orthographic: no divide; the bias is the GS's
        C3D_TexEnv* amount = C3D_GetTexEnv(0);
        C3D_TexEnvInit(amount);
        C3D_TexEnvSrc(amount, C3D_RGB, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(amount, C3D_RGB, GPU_REPLACE);
        C3D_TexEnvSrc(amount, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvOpAlpha(amount, GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA,
                          GPU_TEVOP_A_SRC_ALPHA);
        C3D_TexEnvFunc(amount, C3D_Alpha, GPU_MODULATE);
        C3D_TexEnv* light = C3D_GetTexEnv(1);
        C3D_TexEnvInit(light);
        C3D_TexEnvSrc(light, C3D_RGB, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
        C3D_TexEnvOpRgb(light, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA,
                        GPU_TEVOP_RGB_SRC_COLOR);
        C3D_TexEnvFunc(light, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvSrc(light, C3D_Alpha, GPU_TEXTURE1, GPU_TEXTURE1, GPU_TEXTURE1);
        C3D_TexEnvFunc(light, C3D_Alpha, GPU_REPLACE);
        C3D_TexEnv* surface = C3D_GetTexEnv(2);
        C3D_TexEnvInit(surface);
        C3D_TexEnvSrc(surface, C3D_RGB, GPU_TEXTURE1, GPU_PREVIOUS, GPU_PREVIOUS);
        C3D_TexEnvFunc(surface, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvScale(surface, C3D_RGB, GPU_TEVSCALE_2);
        r->shadowLitStages = true;
        r->shadowLitHaze = -1;
        r->lightStageOn = false;   // stage 1 is not the ordinary light stage
        r->hazeOn = false;         // stage 2 is not the ordinary haze stage
    }
    if (!r->shadowLitStages) {
        C3D_TexBind(0, &r->shadowTex);
        C3D_TexShadowParams(false, 0.0f);   // orthographic: no divide; the bias is the GS's
        C3D_TexBind(2, &r->shadowTint);
        C3D_TexEnv* amount = C3D_GetTexEnv(0);
        C3D_TexEnvInit(amount);
        C3D_TexEnvSrc(amount, C3D_RGB, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(amount, C3D_RGB, GPU_REPLACE);
        C3D_TexEnvSrc(amount, C3D_Alpha, GPU_TEXTURE0, GPU_TEXTURE2, GPU_TEXTURE2);
        C3D_TexEnvOpAlpha(amount, GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA,
                          GPU_TEVOP_A_SRC_ALPHA);
        C3D_TexEnvFunc(amount, C3D_Alpha, GPU_MODULATE);
        C3D_TexEnv* light = C3D_GetTexEnv(1);
        C3D_TexEnvInit(light);
        C3D_TexEnvSrc(light, C3D_RGB, GPU_TEXTURE2, GPU_PRIMARY_COLOR, GPU_PREVIOUS);
        C3D_TexEnvOpRgb(light, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                        GPU_TEVOP_RGB_SRC_ALPHA);
        C3D_TexEnvFunc(light, C3D_RGB, GPU_INTERPOLATE);
        C3D_TexEnvSrc(light, C3D_Alpha, GPU_TEXTURE1, GPU_TEXTURE1, GPU_TEXTURE1);
        C3D_TexEnvFunc(light, C3D_Alpha, GPU_REPLACE);
        C3D_TexEnv* surface = C3D_GetTexEnv(2);
        C3D_TexEnvInit(surface);
        C3D_TexEnvSrc(surface, C3D_RGB, GPU_TEXTURE1, GPU_PREVIOUS, GPU_PREVIOUS);
        C3D_TexEnvFunc(surface, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvScale(surface, C3D_RGB, GPU_TEVSCALE_2);
        r->shadowLitStages = true;
        r->shadowLitHaze = -1;
        r->lightStageOn = false;   // stage 1 is not the ordinary light stage
        r->hazeOn = false;         // stage 2 is not the ordinary haze stage
    }
    // The combiner haze only without the fog; with it, stage 3 passes through
    // and the fog unit takes the haze.
    const int8_t want = (haze && !r->fogLive) ? 1 : 0;
    if (r->fogLive) {
        setFog(r, haze);
    }
    if (want != r->shadowLitHaze) {
        C3D_TexEnv* stage = C3D_GetTexEnv(3);
        C3D_TexEnvInit(stage);
        if (haze) {
            C3D_FVUnifSet(GPU_VERTEX_SHADER, r->hazeLocation, r->hazeExponent, 0.0f, 0.0f, 0.0f);
            C3D_TexEnvSrc(stage, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
            C3D_TexEnvOpRgb(stage, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                            GPU_TEVOP_RGB_SRC_ALPHA);
            C3D_TexEnvFunc(stage, C3D_RGB, GPU_INTERPOLATE);
            C3D_TexEnvColor(stage, mdk::pica::hazeColourWord(r->lighting.haze_colour));
        } else {
            C3D_FVUnifSet(GPU_VERTEX_SHADER, r->hazeLocation, 0.0f, 0.0f, 0.0f, 0.0f);
        }
        r->shadowLitHaze = want;
    }
}

// Back to the ordinary layout: stages 1-3 pass through, the haze uniform is 0,
// the caches say so, and units 1 and 2 hold the idle texture (citro3d cannot
// unbind them). A no-op outside the shadowed layout. Every path that installs
// its own combiner calls it first (installEye, the canvas, the glow, the
// caster pass, and every unshadowed draw).
static void leaveShadowLitStages(RendererC3D* r) {
    if (!r->shadowLitStages) {
        return;
    }
    C3D_TexEnvInit(C3D_GetTexEnv(1));
    C3D_TexEnvInit(C3D_GetTexEnv(2));
    C3D_TexEnvInit(C3D_GetTexEnv(3));
    C3D_FVUnifSet(GPU_VERTEX_SHADER, r->hazeLocation, 0.0f, 0.0f, 0.0f, 0.0f);
    r->hazeOn = false;
    r->lightStageOn = false;
    C3D_TexBind(1, &r->unitIdle);
    C3D_TexBind(2, &r->unitIdle);
    r->shadowLitStages = false;
    r->shadowLitHaze = -1;
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
    int32_t mipLevels = 0;
    const HdCopy* hd = nullptr;   // the HD copy bound, if one is
    if (draw->material.texture >= 0 && (size_t)draw->material.texture < r->textures.size()) {
        C3dTexture& resource = r->textures[(size_t)draw->material.texture];
        // A released texture whose GPU copy is waiting for the next frame
        // boundary must not be drawn again: the caller has given the id up.
        if (!resource.releasePending) {
            // THE HD COPY (enableHdTextures, docs/HD_TEXTURES_3DS_SPEC.md): wanted
            // for a Lit or Sprite surface -- the PC draws a mod's colour image only
            // in its enhanced lit/sprite modes, never on an Original-shaded world
            // pass (sky, glass) -- and for a CANVAS picture linked through
            // mdk_render_replace_image_compressed (hdCanvas: the PC replaces those
            // through its own ReplaceImage); never for a stacked-frame (animated)
            // texture (spec 3).
            HdCopy* copy = resource.hdCopy >= 0 && (size_t)resource.hdCopy < r->hdCopies.size()
                               ? &r->hdCopies[(size_t)resource.hdCopy]
                               : nullptr;
            const bool wantsHd = r->toggles.enableHdTextures && copy &&
                                 draw->material.frame_count <= 1 &&
                                 (draw->material.shading != mdk::Shading::Original ||
                                  resource.hdCanvas);
            if (wantsHd && copy->state == HdCopy::Resident) {
                // Bound whether or not the original is resident: an evicted
                // original must not take a resident HD copy's surface with it (F1).
                texture = &copy->tex;
                mipLevels = copy->levels;
                resource.lastUsed = ++r->drawCounter;
                copy->lastUsed = r->drawCounter;
                hd = copy;
            } else {
                if (!resource.valid && !resource.source.empty()) {
                    // THE EVICTION IS REVERSIBLE. The texture was made again from
                    // the source the backend kept: the caller's id is still good,
                    // and the picture comes back with it.
                    makeTexture(r, resource, false);
                }
                if (resource.valid) {
                    texture = &resource.tex;
                    mipLevels = resource.mipLevels;
                    // THE EVICTION ORDER. The texture is stamped every time it is
                    // drawn, so the least recently used one is the one that goes
                    // when the VRAM is full.
                    resource.lastUsed = ++r->drawCounter;
                }
                // Not resident: this draw takes the original and the copy is asked
                // for -- or asked again once a failure's pause has passed (F4).
                if (wantsHd &&
                    (copy->state == HdCopy::None ||
                     (copy->state == HdCopy::Failed && copy->failures < kHdMaxFailures &&
                      r->frame >= copy->retryFrame))) {
                    copy->state = HdCopy::Queued;
                    r->hdQueue.push_back(resource.hdCopy);
                }
            }
        }
    }
    if (!texture) {
        return;
    }
    // THE FILTER (SPEC.md 7.9 enableFiltering): bilinear for every draw while
    // the toggle is on -- the PC's enhanced look filters the world, the sky and
    // the canvas alike -- nearest (the original look) otherwise. A bit field in
    // the texture's own parameter word, carried by the bind below; no state of
    // its own to cache.
    //
    // AN ANIMATED TEXTURE STAYS NEAREST. Its frames are stacked vertically in one
    // texture, so a bilinear tap at a frame's edge would take half of the next
    // frame's row (and REPEAT would wrap frame 0's top onto the last frame); the
    // PC samples one layer of an array and cannot bleed. Nearest never crosses.
    const bool animated = draw->material.frame_count > 1;
    const GPU_TEXTURE_FILTER_PARAM filter =
        (r->toggles.enableFiltering && !animated) ? GPU_LINEAR : GPU_NEAREST;
    C3D_TexSetFilter(texture, filter, filter);
    // THE MIP CHAIN (SPEC.md 7.9 enableMipmaps): the sampler may descend the
    // levels the texture was made with while the toggle is on -- trilinear with
    // FILTERING, nearest-of-nearest without -- and is held at level 0 otherwise,
    // which is the original look exactly. An animated texture is held at 0 for
    // the reason it stays nearest: a lower level averages across its stacked
    // frames. Like the filter, both live in the texture's own words and travel
    // with the bind.
    //
    // AND ONLY ON THE SURFACES THE PC MIPS: its lit and sprite modes sample the
    // mipped colour textures, while its sky (sky.frag, Panorama::linear) and its
    // canvas (palette_filtered) are filtered without a chain. Here those are the
    // Original-shaded draws; descending a chain there blurred the sky (measured,
    // level 6 frame 300).
    //
    // AND ONLY WITH FILTERING. A lower level carries averaged alpha, and the cut
    // is the half cover only while FILTERING is on (alphaTestReferenceFor); with
    // the oracle's > 0 a 25%-covered texel would draw opaque and every cut-out
    // would thicken with distance (review of the mipmap batch, F1). The PC always
    // pairs its chain with the half cover, so a chain without filtering is not a
    // PC look either.
    const bool mipped = r->toggles.enableMipmaps && r->toggles.enableFiltering && !animated &&
                        draw->material.shading != mdk::Shading::Original;
    texture->maxLevel = (u8)(mipped ? mipLevels : 0);
    C3D_TexSetFilterMipmap(texture, filter);
    // THE LIGHT AND THE SUN'S SHADOW, decided here because the shadow moves the
    // surface to unit 1 (unit 0 is the only one that compares a shadow map).
    // A Lit surface is drawn with the lit program -- the geometry shader lights
    // each triangle by its face normal (mdk_c3d.g.pica) -- and, when it is one
    // of the sun's casters/receivers (pica::castsSunShadow: opaque Lit
    // triangles) and the frame has the shadow, with the SHADOWED lit program
    // instead (mdk_litshadow.g.pica, installShadowLitStages): the light and the
    // shadow in ONE pass. Sprites (Kurt, effects), the sky and the canvas are
    // not lit, as on the PC (enhanced.frag modes 2-4).
    // Only a TRIANGLE mesh: a Lines mesh (the glass frames' outlines) is drawn
    // as triangles from its vertex pairs here (this GPU has no line primitive),
    // and the geometry shader would light that line data as if it were
    // triangles (measured: the lamp's wire frame went red/green/black). A
    // program with a geometry shader takes the vertices three at a time, so
    // only a whole number of triangles goes through one.
    const bool overlay = draw->material.pass == mdk::Pass::Overlay;
    const bool wholeTriangles = (draw->vertex_count % 3) == 0;
    const bool lit = r->litLive && draw->material.shading == mdk::Shading::Lit && !overlay &&
                     draw->primitive == mdk::Primitive::Triangles && wholeTriangles &&
                     gsAllowed(r) && !(renderDebugFlags() & kRenderDebugFlat);
    const bool shadowLit = lit && r->shadowLitLive &&
                           mdk::pica::castsSunShadow(draw->material.pass, draw->material.shading,
                                                     draw->primitive);
    C3D_TexBind(shadowLit ? 1 : 0, texture);

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
    if (hd) {
        // AN HD COPY maps the oracle's uv (0 = image top) through the image's own
        // rectangle from its .t3x (Tex3DS_SubTexture: top/bottom are already the
        // texture's t, so no last-row flip is applied on top). A .t3x is a whole
        // power-of-two image: no padding to undo.
        scaleX = hd->right - hd->left;
        offsetX = hd->left;
        scaleY = hd->bottom - hd->top;
        offsetY = hd->top;
    } else {
        // The PICA's t runs from the LAST memory row (pica::sceneUvFromTop has
        // the rule and the measurement). Every draw takes it, the canvas too:
        // the canvas was once exempt, and its non-power-of-two images (the
        // health digits) read their transparent padding rows.
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
    // THE CUT for this draw (pica::alphaTestReferenceFor): the half cover for a
    // filtered scene draw, the oracle's discard for the canvas and for nearest.
    installAlphaTest(r, mdk::pica::alphaTestReferenceFor(r->toggles.enableFiltering,
                                                         draw->material.pass), false);
    // A shadowed lit draw's stages are installShadowLitStages' (below, with the
    // light); every other draw leaves that layout first.
    if (!shadowLit) {
        leaveShadowLitStages(r);
    }
    C3D_TexEnv* env = shadowLit ? nullptr : C3D_GetTexEnv(0);
    if (shadowLit) {
        // (stage 0 is installShadowLitStages')
    } else if (overlay) {
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
    } else if (renderDebugFlags() & kRenderDebugFlat) {
        C3D_TexEnvInit(env);
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
        C3D_TexEnvInit(env);
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

    // THE LIGHT'S PROGRAM (decided with `lit` and `shadowLit` above), and
    // combiner stage 1 multiplies the texture by that light. Both switches are
    // cached.
    // THE GEOMETRY-STAGE RULE (03_frame.inc bindProgram): once this command
    // list has used a geometry program, an unlit draw goes through the
    // pass-through one -- the same picture, and no split to leave geometry mode.
#if defined(MDK_AUTOQUIT) && defined(MDK_HW_GSPASSALL)
    // THE HARNESS'S GEOMETRY-STAGE PROBE (`make AUTOQUIT=.. GSPASSALL=1`): the
    // list counts as in geometry mode from its first draw, so every whole-triangle
    // unlit draw goes through the pass-through geometry shader and a profile with
    // the light OFF still pays the geometry stage -- the light's cost split into
    // the stage's own and the arithmetic's. A harness build only.
    if (r->passReady && wholeTriangles) {
        r->gsInList = true;
    }
#endif
    // (The shadowed program is the frame's layout's: the one-scalar one under
    // config hazeFog, prepareShadows' shadowScalar.)
    shaderProgram_s* const wanted =
        shadowLit ? (r->shadowScalar ? &r->programShadowQ : &r->programShadow)
        : lit     ? &r->programLit
                  : (r->gsInList && r->passReady && wholeTriangles ? &r->programPass : &r->program);
    if (wanted != r->boundProgram) {
        bindProgram(r, wanted);
        if (shadowLit) {
            installShadowUniforms(r);
        } else if (lit) {
            installLitUniforms(r);
        }
        r->programSwitches++;
    }
    r->litBound = lit && !shadowLit;
    if (shadowLit) {
        r->shadowReceivers++;
    }
    if (r->boundProgram == &r->programShadow || r->boundProgram == &r->programShadowQ) {
        r->drawsShadowLit++;
    } else if (r->boundProgram == &r->programLit) {
        r->drawsLit++;
    } else if (r->boundProgram == &r->programPass) {
        r->drawsPass++;
    } else {
        r->drawsPlain++;
    }
    if (lit) {
        // The world position the geometry shader takes its normal from; the
        // identity (every arena batch) is written only when it is not there.
        const bool identity = mdk::pica::isIdentity(draw->world);
        if (!identity || !r->worldIsIdentity) {
            C3D_Mtx worldMtx;
            mdk::pica::registers(draw->world, worldMtx.m);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->worldLocation, &worldMtx);
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
        setHaze(r, r->hazeLive && draw->material.shading != mdk::Shading::Original &&
                       draw->material.pass != mdk::Pass::Overlay);
    }
    if (r->fogOn) {
        r->drawsFogged++;
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

// The glow quad: two triangles, the whole canvas. The level-pass quad: the same
// two triangles over a whole texture level (pica::glowLevelCorner).
static constexpr int kGlowQuadCorners = 6;

// THE GLOW'S MEMORY, made on first use (linear heap; the VRAM is the eye
// targets'). False when any part could not be made: the glow is then off for the
// session, with one refusal line.
static bool ensureGlow(RendererC3D* r) {
    // The refusal first: a failed set-up leaves its textures for releaseGlow
    // (which runs outside a frame), so glowReady may be true with nothing usable.
    if (r->glowRefused) {
        return false;
    }
    if (r->glowReady) {
        return true;
    }
    const size_t quadBytes = kGlowQuadCorners * 5 * sizeof(float);
    if (!r->glowQuad) {
        r->glowQuad = static_cast<float*>(linearAlloc(quadBytes));
        r->glowQuadWidth = 0.0f;
    }
    if (!r->glowLevelQuad) {
        r->glowLevelQuad = static_cast<float*>(linearAlloc(quadBytes));
    }
    C3D_TexInitParams params;
    params.width = (u16)mdk::pica::kGlowTexWidth;
    params.height = (u16)mdk::pica::kGlowTexHeight;
    params.maxLevel = 0;
    params.format = GPU_RGBA8;
    params.type = GPU_TEX_2D;
    // IN VRAM: citro3d renders only into VRAM (C3D_RenderTargetCreateFromTex
    // refuses a texture outside it -- measured: a linear-heap texture's targets
    // came back null), and every blur level is rendered into.
    params.onVram = true;
    bool ok = r->glowQuad && r->glowLevelQuad &&
              C3D_TexInitWithParams(&r->glowTex, nullptr, params);
    r->glowReady = ok;   // from here releaseGlow frees what was made
    for (int i = 0; ok && i < mdk::pica::kGlowSmallCount; i++) {
        const int level = (i == mdk::pica::kGlowSmallMix) ? mdk::pica::kGlowMidLevel : i + 1;
        C3D_TexInitParams small = params;
        small.width = (u16)(mdk::pica::kGlowTexWidth >> level);
        small.height = (u16)(mdk::pica::kGlowTexHeight >> level);
        r->glowSmallMade[i] = C3D_TexInitWithParams(&r->glowSmall[i], nullptr, small);
        ok = r->glowSmallMade[i];
        if (ok) {
            C3D_TexSetFilter(&r->glowSmall[i], GPU_LINEAR, GPU_LINEAR);
            C3D_TexSetWrap(&r->glowSmall[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            r->glowTarget[i] = C3D_RenderTargetCreateFromTex(&r->glowSmall[i], GPU_TEXFACE_2D, 0, -1);
            ok = r->glowTarget[i] != nullptr;
        }
    }
    if (!ok) {
        // Inside a frame, where C3D_RenderTargetDelete breaks: what was made waits
        // for releaseGlow (the next eye-target rebuild, or the destructor).
        mdk::log3dsPrintf("glow: texture/target/quad allocation FAILED; glow off (vram free %u)",
                          (unsigned)vramSpaceFree());
        r->glowRefused = true;
        return false;
    }
    // Black outside the picture: the copy fills only the 240x400 corner of the
    // picture texture, and the averaging carries the rest in (the glow only ever
    // adds, so the edges lose a little shine and gain nothing). A GPU fill,
    // queued ahead of this frame's first copy -- the CPU does not write VRAM.
    u32* picture = static_cast<u32*>(r->glowTex.data);
    GX_MemoryFill(picture, 0, picture + r->glowTex.size / 4,
                  GX_FILL_32BIT_DEPTH | GX_FILL_TRIGGER, nullptr, 0, nullptr, 0);
    C3D_TexSetFilter(&r->glowTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&r->glowTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    // The small-pass quad: the identity over a whole target.
    const float corner[kGlowQuadCorners][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
    for (int i = 0; i < kGlowQuadCorners; i++) {
        float* v = r->glowLevelQuad + i * 5;
        mdk::pica::glowLevelCorner(corner[i][0], corner[i][1], &v[0], &v[1]);
        v[2] = -0.5f;   // inside this GPU's [-w, 0] clip z (pica::canvasMatrix)
        v[3] = corner[i][0];
        v[4] = corner[i][1];
    }
    GSPGPU_FlushDataCache(r->glowLevelQuad, quadBytes);
    mdk::log3dsPrintf("glow: picture 256x512 + blur levels 1-5 + mix (VRAM, %u free)",
                      (unsigned)vramSpaceFree());
    return true;
}

// The state every glow draw shares: the scene program unlit, texture unit 0 on
// `view`, stage 0 = that texture (alpha 1, so the alpha test passes), the depth
// test off, no cull. Stages 1-5 are left to the caller.
static void glowDrawState(RendererC3D* r, C3D_Tex* view) {
    bindProgram(r, &r->program);
    r->litBound = false;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, r->uvLocation, 1.0f, 1.0f, 0.0f, 0.0f);
    C3D_TexBind(0, view);
    C3D_TexEnv* tex = C3D_GetTexEnv(0);
    C3D_TexEnvInit(tex);
    C3D_TexEnvSrc(tex, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(tex, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvSrc(tex, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(tex, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(tex, 0xFFFFFFFFu);
    installAlphaTest(r, 0, false);
    installDepthState(r, mdk::pica::DepthState{false, false});
    C3D_CullFace(GPU_CULL_NONE);
    r->cullMode = GPU_CULL_NONE;
}

// THE GLOW (Enhanced Mode enableGlowBillboards): the PC's post.frag shine, on the
// PICA200, between the scene and the canvas -- once per eye, from that eye's own
// picture. NO CPU TOUCHES A PIXEL, and that is a measured statement, not a hope:
// the first version called C3D_TexGenerateMipmap, which in the linked
// libcitro3d.a is a CPU box filter for a linear-heap texture (review,
// 2026-11-18: no GX call in its disassembly; it also read the previous frame's
// copy, since the GX queue is stopped until C3D_FrameEnd). The levels are made
// here by the GPU:
//
//   1. C3D_FrameSplit: the eye's scene so far goes to the GX queue, so what
//      follows reads it finished.
//   2. At edge-smoothing scale 2, the 480x800 target is brought to 240x400
//      first: a 2x2-averaging transfer TILED TO LINEAR (the screen output's own
//      form), then an unscaled LINEAR TO TILED one (the texture upload's form).
//      Not one tiled-to-tiled transfer (flag bit 5): on the console ANY display
//      transfer with bit 5 set stalls the transfer engine for good, scaled or
//      not, with or without OUT_TILED (measured 2026-11-18 on the Old 3DS
//      appliance, mdk_3ds/tools/gstest stages T20/T23/T27; the two-transfer
//      chain and the texture copy below ran 90 frames each, T22/T24-T26). The
//      emulator accepts bit 5, which is how this pass hung the first console
//      frame it ran on. Two GX queue entries per eye here, not one.
//   3. GX_TextureCopy, tile row by tile row (30 tiles = 7680 bytes of a 240-wide
//      framebuffer row into the 8192 bytes of a 256-wide texture row): the
//      picture becomes the 256x512 picture texture, unscaled (a raw copy).
//   4. Blur levels 1-5, each its own small texture: a quad into each one's
//      render target, sampling the level above bilinearly at texel corners (an
//      exact 2x2 average). Neither the transfer engine's tiled-to-tiled scaling
//      (citro3d never uses it, so nothing shows the console does it at these
//      widths) nor the picture texture's own mip levels (rendering into those
//      never reached the sampler in the emulator).
//   5. The mix texture := level 3, then += level 5 with a constant-alpha blend
//      (pica::kBlendGlowMix): (level 3 + level 5) / 2, the PC's two blur levels
//      folded into one, so ONE texture unit reads them. (Texture units 1 and 2
//      are not an option: citro3d cannot unbind them -- C3D_TexBind(1, NULL)
//      reads through the null pointer -- and a bound unit is sampled on every
//      later fragment of every frame.)
//   6. One full-panel quad into the eye, screen-blended (pica::kBlendScreen),
//      whose combiner is the shine: the threshold subtracted (SUBTRACT clamps at
//      0: the PC's max(.., 0)) and the fitted curve (pica_matrix.h, THE GLOW).
//
// GX QUEUE BUDGET: citro3d's queue holds 32 entries and a full one is svcBreak.
// This pass adds 2 per eye (the split, the copy) and 2 more at scale 2 (the
// shrink and the re-tile); the blur passes flush inside the command list instead
// of splitting. Measured on the console, stereo at scale 2, before the re-tile:
// 14 entries a frame.
// Everything this pass changes is restored, or its cache invalidated, before the
// canvas draws.
static void drawGlowBillboards(RendererC3D* r);

static void drawGlow(RendererC3D* r) {
    if (r->billboardsLive) {
        drawGlowBillboards(r);   // the 3DS default (config glowBloom = 0)
        return;
    }
    if (!r->glowLive || !r->eyeTarget || !ensureGlow(r)) {
        return;
    }
    C3D_RenderTarget* eye = r->eyeTarget;
    leaveShadowLitStages(r);
    frameSplit(r);
    // The glow takes no haze: its passes and its add over the eye unfogged.
    setFog(r, false);
    u32* source = static_cast<u32*>(eye->frameBuf.colorBuf);
    if (r->targetScale > 1) {
        const size_t scratchBytes = (size_t)kScreenWidth * kScreenHeight * 4;
        if (!r->glowScratch) {
            r->glowScratch = static_cast<u32*>(linearAlloc(scratchBytes));
        }
        if (!r->glowScratchTiled) {
            r->glowScratchTiled = static_cast<u32*>(linearAlloc(scratchBytes));
        }
        if (!r->glowScratch || !r->glowScratchTiled) {
            mdk::log3dsPrintf("glow: smoothing scratch allocation FAILED; glow off this session");
            r->glowRefused = true;
            r->glowLive = false;
            return;
        }
        const u32 rgba = GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                         GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8);
        // Step 2 (above): NEVER flag bit 5 (tiled to tiled) -- it hangs the
        // console's transfer engine. Tiled to linear with the display's own
        // averaging (2x2, or 2x1: transferScaling), both
        // dimensions the input's (the engine halves the output itself: see
        // writeScreenshotBmp) ...
        const u32 full = GX_BUFFER_DIM(r->targetWidth, r->targetHeight);
        GX_DisplayTransfer(source, full, r->glowScratch, full,
                           rgba | transferScaling(r->targetScaleX, r->targetScaleY));
        // ... then linear back to tiled, unscaled, for the tile-row copy below.
        const u32 half = GX_BUFFER_DIM(kScreenWidth, kScreenHeight);
        GX_DisplayTransfer(r->glowScratch, half, r->glowScratchTiled, half,
                           rgba | GX_TRANSFER_OUT_TILED(1));
        source = r->glowScratchTiled;
    }
    const u32 rowBytes = (u32)kScreenWidth * 8 * 4;                       // 7680
    const u32 texRowBytes = (u32)mdk::pica::kGlowTexWidth * 8 * 4;        // 8192
    const u32 rows = (u32)kScreenHeight / 8;                              // 50
    GX_TextureCopy(source, GX_BUFFER_DIM(rowBytes >> 4, 0),
                   static_cast<u32*>(C3D_Tex2DGetImagePtr(&r->glowTex, 0, nullptr)),
                   GX_BUFFER_DIM(rowBytes >> 4, (texRowBytes - rowBytes) >> 4),
                   rowBytes * rows, GX_TRANSFER_RAW_COPY(1));
    // THE BLUR (steps 4-6), through the identity matrix: seven small
    // render-to-texture passes. Each ends in a framebuffer FLUSH and INVALIDATE
    // written into the command list, so the next pass samples what this one
    // wrote (the texture cache is cleared by the rebind each pass makes:
    // C3Di_UpdateContext sets TEXUNIT_CONFIG bit 16 on a bound-texture change).
    // Not a C3D_FrameSplit per pass: each split is a GX queue entry, citro3d's
    // queue holds 32 and a full queue is svcBreak, and a stereo frame at scale 2
    // already needs ~14 (review, 2026-11-18).
    C3D_Mtx identity;
    Mtx_Identity(&identity);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->mvpLocation, &identity);
    r->matrixIsFrame = false;
    C3D_BufInfo* info = C3D_GetBufInfo();
    BufInfo_Init(info);
    BufInfo_Add(info, r->glowLevelQuad, 5 * sizeof(float), 2, 0x10);
    for (int stage = 1; stage <= 5; stage++) {
        C3D_TexEnvInit(C3D_GetTexEnv(stage));
    }
    r->lightStageOn = false;
    r->hazeOn = false;
    struct SmallPass {
        int into;       // a glowSmall index
        C3D_Tex* from;
        bool mix;
    };
    const int mix = mdk::pica::kGlowSmallMix;
    const SmallPass passes[7] = {
        {0, &r->glowTex, false},          // level 1 from the picture
        {1, &r->glowSmall[0], false},     // 2 from 1
        {2, &r->glowSmall[1], false},     // 3 from 2
        {3, &r->glowSmall[2], false},     // 4 from 3
        {4, &r->glowSmall[3], false},     // 5 from 4
        {mix, &r->glowSmall[2], false},   // mix := 3
        {mix, &r->glowSmall[4], true},    // mix += 5, at alpha 1/2
    };
    for (int i = 0; i < 7; i++) {
        C3D_FrameDrawOn(r->glowTarget[passes[i].into]);
        C3D_SetViewport(0, 0, r->glowSmall[passes[i].into].width,
                        r->glowSmall[passes[i].into].height);
        glowDrawState(r, passes[i].from);
        if (passes[i].mix) {
            C3D_BlendingColor(0x80000000u);   // alpha 128/255 (bits 24-31)
            writeBlendState(r, mdk::pica::kBlendGlowMix, -2, -2);
        } else {
            writeBlendState(r, mdk::pica::kBlendReplace, -2, -2);
        }
        C3D_DrawArrays(GPU_TRIANGLES, 0, kGlowQuadCorners);
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
    }

    // BACK TO THE EYE (step 7), with installEye's viewport (the whole target); the
    // scissor is still the one installEye disabled.
    C3D_FrameDrawOn(eye);
    C3D_SetViewport(0, 0, (u32)r->targetWidth, (u32)r->targetHeight);
    const float width = mdk_render_canvas_width(reinterpret_cast<MdkRenderer>(r));
    if (width != r->glowQuadWidth) {
        const float corner[kGlowQuadCorners][2] = {{0, 0}, {1, 0}, {1, 1},
                                                   {0, 0}, {1, 1}, {0, 1}};
        for (int i = 0; i < kGlowQuadCorners; i++) {
            float* v = r->glowQuad + i * 5;
            v[0] = corner[i][0] * width;
            v[1] = corner[i][1] * mdk::kCanvasHeight;
            v[2] = 0.0f;
            mdk::pica::glowTexCoord(corner[i][0], corner[i][1], &v[3], &v[4]);
        }
        GSPGPU_FlushDataCache(r->glowQuad, kGlowQuadCorners * 5 * sizeof(float));
        r->glowQuadWidth = width;
    }
    float screen[16];
    mdk::pica::canvasMatrix(screen, width);
    C3D_Mtx gpuMtx;
    mdk::pica::registers(screen, gpuMtx.m);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->mvpLocation, &gpuMtx);
    info = C3D_GetBufInfo();
    BufInfo_Init(info);
    BufInfo_Add(info, r->glowQuad, 5 * sizeof(float), 2, 0x10);
    glowDrawState(r, &r->glowSmall[mdk::pica::kGlowSmallMix]);

    // THE SHINE, s*(u) = P * min(1, u / u0) + Q * u (pica_matrix.h, THE GLOW).
    // Each constant is a byte, each scale 1, 2 or 4.
    //   0: b, the blur                           (glowDrawState: texture, alpha 1)
    //   1: 4 (b - t)          = 4 * band * u    SUBTRACT clamps at 0: max(.., 0)
    //   2: Q u                                  -> the combiner buffer
    //   3: 4 Q u                                (clamped at 1)
    //   4: 16 Q u = u / u0                      (clamped at 1: the min)
    //   5: P * prev + buffer                    MULTIPLY_ADD
    auto byteOf = [](float v) -> u32 {
        const float b = v * 255.0f + 0.5f;
        return b <= 0.0f ? 0u : (b >= 255.0f ? 255u : (u32)b);
    };
    auto grey = [](u32 b) -> u32 { return 0xFF000000u | (b << 16) | (b << 8) | b; };
    C3D_TexEnv* excess = C3D_GetTexEnv(1);
    C3D_TexEnvInit(excess);
    C3D_TexEnvSrc(excess, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(excess, C3D_RGB, GPU_SUBTRACT);
    C3D_TexEnvScale(excess, C3D_RGB, GPU_TEVSCALE_4);
    C3D_TexEnvColor(excess, grey(byteOf(mdk::pica::kGlowThresholdSrgb)));
    C3D_TexEnv* linear = C3D_GetTexEnv(2);
    C3D_TexEnvInit(linear);
    C3D_TexEnvSrc(linear, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(linear, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvColor(linear, grey(byteOf(mdk::pica::kGlowCurveQ / (4.0f * mdk::pica::kGlowBand))));
    C3D_TexEnvBufUpdate(C3D_RGB, 1 << 2);   // stage 2's output is the buffer from stage 3
    for (int stage = 3; stage <= 4; stage++) {
        C3D_TexEnv* steep = C3D_GetTexEnv(stage);
        C3D_TexEnvInit(steep);
        C3D_TexEnvScale(steep, C3D_RGB, GPU_TEVSCALE_4);   // REPLACE previous, x4
    }
    C3D_TexEnv* sum = C3D_GetTexEnv(5);
    C3D_TexEnvInit(sum);
    C3D_TexEnvSrc(sum, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PREVIOUS_BUFFER);
    C3D_TexEnvFunc(sum, C3D_RGB, GPU_MULTIPLY_ADD);
    C3D_TexEnvColor(sum, grey(byteOf(mdk::pica::kGlowCurveP)));
    writeBlendState(r, mdk::pica::kBlendScreen, -2, -2);
    C3D_DrawArrays(GPU_TRIANGLES, 0, kGlowQuadCorners);
    r->glowPasses++;

    // Back to the frame's state for the canvas: stages 1-5 passing through (the
    // light and haze caches say so) and the combiner buffer off. Stage 0, the
    // texture on unit 0, the matrix and the blend are written by the next draw
    // itself; the program is the scene's (glowDrawState left it unlit).
    for (int stage = 1; stage <= 5; stage++) {
        C3D_TexEnvInit(C3D_GetTexEnv(stage));
    }
    C3D_TexEnvBufUpdate(C3D_Both, 0);
    r->lightStageOn = false;
    r->hazeOn = false;
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
        // The glow lands between the scene and the canvas, where the PC's post
        // pass sits (SPEC.md 6: post -> frame -> canvas).
        if (pass == static_cast<int32_t>(mdk::Pass::Overlay)) {
            drawGlow(r);
        }
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
        r->programSwitches++;
    }
    r->litBound = false;
    if (!r->matrixIsFrame) {
        C3D_Mtx gpuMtx;
        mdk::pica::registers(r->eyeVp, gpuMtx.m);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->mvpLocation, &gpuMtx);
        r->matrixIsFrame = true;
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, r->uvLocation, 1.0f, 1.0f, 0.0f, 0.0f);   // a symmetric dot
    setHaze(r, false);
    if (r->lightStageOn) {
        C3D_TexEnvInit(C3D_GetTexEnv(1));
        r->lightStageOn = false;
    }
    C3D_TexBind(0, &r->glowHalo);
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
    C3D_BufInfo* bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
    BufInfo_Add(bufInfo, r->glowQuadMesh, 5 * sizeof(float), 2, 0x10);
    for (int32_t q = 0; q < r->glowQuadCount; q++) {
        const float* c = r->glowQuads[q].colour;
        C3D_FVUnifSet(GPU_VERTEX_SHADER, r->tintLocation, c[0], c[1], c[2], 1.0f);
        if (r->boundProgram && r->boundProgram->geometryShader) {
            C3D_DrawArrays(GPU_GEOMETRY_PRIM, q * 6, 6);
        } else {
            C3D_DrawArrays(GPU_TRIANGLES, q * 6, 6);
        }
        r->billboardsDrawn++;
    }
    r->tintIsWhite = false;
}
