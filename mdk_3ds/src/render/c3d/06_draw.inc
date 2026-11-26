// 06_draw.inc -- preparing one queued draw: the combiner stages, haze, the prepare.
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
static void setHaze(RendererC3D* r, bool on) {
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
static void installShadowLitStages(RendererC3D* r, bool haze) {
    if (!r->shadowLitStages) {
        bindTexUnit(r, 0, &r->shadowTex);
        C3D_TexShadowParams(false, 0.0f);   // orthographic: no divide; the bias is the GS's
        bindTexUnit(r, 2, &r->shadowTint);
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
    const int8_t want = haze ? 1 : 0;
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
    bindTexUnit(r, 1, &r->unitIdle);
    bindTexUnit(r, 2, &r->unitIdle);
    r->shadowLitStages = false;
    r->shadowLitHaze = -1;
}

// The cached stage-0 variant a prepared index names (see prepareDraw).
static C3D_TexEnv* stage0Variant(RendererC3D* r, int32_t index) {
    if (index == 1) return &r->stage0Overlay;
    if (index >= 2 && index <= 5) return &r->stage0Flat[index - 2];
    return &r->stage0Scene;
}

// A COMPRESSED texture's uv transform (Kurt's ETC1A4 frames, the orig pack's
// arena textures): its image's rectangle (pica::compressedFrameRect), in
// tex3ds' convention, with no last-row flip on top -- the converters feed
// tex3ds the image's top row first, as hd_pack_convert.py does, and tex3ds' top
// is t = 1. ONE function for the prepare and for a packed original made at
// submit (drawRecorded), so the two cannot drift. An animated strip's draw
// takes its own frame's slice of the rectangle (pica::compressedFrameUv); one
// frame is the whole rectangle.
static void compressedUv(const C3dTexture& resource, const mdk::MeshDraw* draw, float* scaleX,
                         float* scaleY, float* offsetX, float* offsetY) {
    const mdk::pica::CompressedUv uv =
        mdk::pica::compressedFrameUv(resource.rect, draw->frame, draw->material.frame_count);
    *scaleX = uv.scaleX;
    *offsetX = uv.offsetX;
    *scaleY = uv.scaleY;
    *offsetY = uv.offsetY;
}

// THE PREPARE (STEREO-REPLAY analogue): resolve one queue record for the
// frame. Everything here is eye-invariant; the per-eye drawRecorded below
// keeps only the frustum cull, the matrix compose and the state/draw calls.
// Side effects that used to run once PER EYE now run once per frame, which
// is what they always meant: the HD queue push, the keepRgba8 pin, the
// texture revival and the LRU stamps.
static bool prepareDraw(RendererC3D* r, const mdk::MeshDraw* draw, PreparedDraw* out) {
    if (draw->mesh < 0 || (size_t)draw->mesh >= r->meshes.size()) {
        return false;
    }
    const C3dMesh& mesh = r->meshes[(size_t)draw->mesh];
    if (!mesh.valid || mesh.count <= 0) {
        return false;
    }
    if (draw->first_vertex < 0 || draw->vertex_count <= 0 ||
        draw->first_vertex + draw->vertex_count > mesh.count) {
        return false;
    }
    out->draw = draw;
    out->meshIndex = draw->mesh;
    out->vertCount = draw->vertex_count;

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
            // THE CANVAS STAYS RGBA8 (texture16, pica::expandedTexelFormat). The
            // first Overlay draw of a texture pins it, and a copy already made in a
            // 16-bit store is made again as RGBA8 at the next frame boundary
            // (settleDeferredTextures: after C3D_FrameBegin's wait, so the copy
            // this frame draws is not freed under the GPU, AGENTS.md P3). The
            // HUD's digits and the fonts keep the palette's exact colours; at most
            // the first frame that draws one shows the 16-bit copy.
            if (draw->material.pass == mdk::Pass::Overlay && !resource.keepRgba8 &&
                !resource.compressed && !resource.streamed) {
                resource.keepRgba8 = true;
                // Canvas draws are Original-shaded (level 0 only): its next
                // make has no chain. Not re-made for that alone.
                resource.noMips = true;
                if (resource.valid && resource.format != mdk::pica::TexelFormat::Rgba8) {
                    resource.stale = true;
                }
            }
            // THE HD COPY (enableHdTextures, docs/HD_TEXTURES_3DS_SPEC.md): wanted
            // for a Lit or Sprite surface -- the PC draws a mod's colour image only
            // in its enhanced lit/sprite modes, never on an Original-shaded world
            // pass (sky, glass) -- and for a CANVAS picture linked through
            // mdk_render_create_hd_image (hdCanvas: the PC replaces those
            // through its own ReplaceImage); never for a stacked-frame (animated)
            // texture (spec 3).
            HdCopy* copy = resource.hdCopy >= 0 && (size_t)resource.hdCopy < r->hdCopies.size()
                               ? &r->hdCopies[(size_t)resource.hdCopy]
                               : nullptr;
            // AN HD-ONLY TEXTURE (drawsHdOnly, the HD rule): its HD copy is the
            // texture whatever the draw's shading -- the original is never read
            // for it while the toggle is on, so there is no other copy to take.
            // (Its pair was asked for by a Lit or Sprite draw, and never for a
            // stacked texture: main_3ds.cpp hdPackLookup.)
            const bool hdOnlyNow = drawsHdOnly(r, resource);
            const bool wantsHd = r->toggles.enableHdTextures && copy &&
                                 ((draw->material.frame_count <= 1 &&
                                   (draw->material.shading != mdk::Shading::Original ||
                                    resource.hdCanvas)) ||
                                  hdOnlyNow);
            if (wantsHd && copy->state == HdCopy::Resident) {
                // Bound whether or not the original is resident: an evicted
                // original must not take a resident HD copy's surface with it (F1).
                texture = &copy->tex;
                out->texHd = true;
                out->texIndex = resource.hdCopy;
                mipLevels = copy->levels;
                resource.lastUsed = ++r->drawCounter;
                copy->lastUsed = r->drawCounter;
                hd = copy;
            } else if (hdOnlyNow) {
                // NOT RESIDENT YET: asked for (the pump reads it on the reader
                // thread and imports it at a boundary), stamped -- the pump's sign
                // that a draw wants it, which reads it before any hint -- and the
                // standard-definition copy is NOT made. An original already
                // resident (made before the toggle went on) stands in, costing
                // nothing; otherwise this draw draws nothing, which
                // the early asks (the pump, the warms) keep rare.
                requestHdCopy(r, resource.hdCopy);
                copy->lastUsed = ++r->drawCounter;
                // THE WINDOW'S PROMISE BROKEN (docs/3DS_ARCHITECTURE.md 4, 8): a
                // surface drawn without its HD copy -- nothing, or a stand-in. The
                // frontend waits for the drawn arenas' files before it draws, so
                // this stays 0; counted for the profile and the log.
                r->hdDrawnMissing++;
                if (resource.valid) {
                    texture = &resource.tex;
                    out->texHd = false;
                    out->texIndex = draw->material.texture;
                    mipLevels = resource.mipLevels;
                    resource.lastUsed = ++r->drawCounter;
                }
            } else if (!resource.valid && !resource.packPath.empty() &&
                       resource.packState != C3dTexture::PackFailed &&
                       resource.hdOnly && copy && copy->state == HdCopy::Resident) {
                // THE TOGGLE JUST WENT OFF: an HD-only texture's original is not
                // made yet, and its HD copy -- still resident, kept by the HD pump
                // while a draw uses it -- stands in until the boundary pump has
                // read and imported the original. Nothing is read on this thread,
                // so switching the set off is no freeze.
                queuePackRead(r, draw->material.texture);
                resource.lastUsed = ++r->drawCounter;
                texture = &copy->tex;
                out->texHd = true;
                out->texIndex = resource.hdCopy;
                mipLevels = copy->levels;
                copy->lastUsed = r->drawCounter;
                hd = copy;
            } else if (!resource.valid && !resource.packPath.empty() &&
                       resource.packState != C3dTexture::PackFailed) {
                // A PACKED ORIGINAL NOT MADE YET (or evicted): NOT made here. Its
                // file is asked for (the boundary pump reads it), it is stamped --
                // the pump's sign that a draw wants it, and its order among those
                // -- and the record is kept: the submit makes it only if the batch
                // is in the eye's frustum (drawRecorded, finishPackNow), so a
                // texture behind the camera never costs a read inside the frame.
                queuePackRead(r, draw->material.texture);
                resource.lastUsed = ++r->drawCounter;
                out->texPending = true;
                out->texHd = false;
                out->texIndex = draw->material.texture;
            } else {
                if (!resource.valid && !resource.source.empty()) {
                    // THE EVICTION IS REVERSIBLE. The texture was made again from
                    // the source the backend kept: the caller's id is still good,
                    // and the picture comes back with it.
                    makeTexture(r, resource, false);
                }
                if (resource.valid) {
                    texture = &resource.tex;
                    out->texHd = false;
                    out->texIndex = draw->material.texture;
                    mipLevels = resource.mipLevels;
                    // THE EVICTION ORDER. The texture is stamped every time it is
                    // drawn, so the least recently used one is the one that goes
                    // when the VRAM is full.
                    resource.lastUsed = ++r->drawCounter;
                }
                // Not resident: this draw takes the original and the copy is asked
                // for -- or asked again once a failure's pause has passed (F4) --
                // and stamped, so the pump reads it before the hints.
                if (wantsHd) {
                    requestHdCopy(r, resource.hdCopy);
                    copy->lastUsed = ++r->drawCounter;
                }
            }
        }
    }
    if (!texture && !out->texPending) {
        return false;
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
    // Recorded, not written: the sampler words sit on the shared C3D_Tex,
    // so writing them here (once per frame, in queue order) would leave
    // every draw of a texture with the LAST prepared draw's filter.
    // drawRecorded writes them at submit, before the bind (validator F1).
    out->filter = (int32_t)filter;
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
    out->mipped = mipped;
    out->mipLevels = mipLevels;
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
    out->overlay = overlay;
    out->wholeTriangles = wholeTriangles;
    out->lit = lit;
    out->shadowLit = shadowLit;
    // The unit is chosen here; the bind itself is per-eye submit state
    // (bindTexUnitIfChanged, THE DEDUPE: a run of one texture binds once).
    out->texUnit = shadowLit ? 1 : 0;

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
        // The original's own transform, kept for the submit-time fallback:
        // the padded-rect mapping with the last-row flip (the else path's).
        const mdk::pica::SceneUv flippedOrig = mdk::pica::sceneUvFromTop(scaleY, offsetY);
        out->origScaleX = scaleX;
        out->origScaleY = flippedOrig.scaleY;
        out->origOffsetX = offsetX;
        out->origOffsetY = flippedOrig.offsetY;
        // AN HD COPY maps the oracle's uv (0 = image top) through the image's own
        // rectangle from its .t3x (Tex3DS_SubTexture: top/bottom are already the
        // texture's t, so no last-row flip is applied on top). A .t3x is a whole
        // power-of-two image: no padding to undo.
        scaleX = hd->right - hd->left;
        offsetX = hd->left;
        scaleY = hd->bottom - hd->top;
        offsetY = hd->top;
    } else if (resource.compressed) {
        // A COMPRESSED texture is a tex3ds texture too: its image's rectangle,
        // or this draw's frame of it (compressedUv). A packed original not made
        // yet has no rectangle; its submit takes the made one's (drawRecorded).
        compressedUv(resource, draw, &scaleX, &scaleY, &offsetX, &offsetY);
    } else {
        // The PICA's t runs from the LAST memory row (pica::sceneUvFromTop has
        // the rule and the measurement). Every draw takes it, the canvas too:
        // the canvas was once exempt, and its non-power-of-two images (the
        // health digits) read their transparent padding rows.
        const mdk::pica::SceneUv flipped = mdk::pica::sceneUvFromTop(scaleY, offsetY);
        scaleY = flipped.scaleY;
        offsetY = flipped.offsetY;
    }
    // The UV transform is stored, not sent: the register write is per-eye
    // submit state (setVsUniform, THE DEDUPE -- every unpadded single-frame
    // texture has the same transform, so a run sends it once per eye).
    out->scaleX = scaleX;
    out->scaleY = scaleY;
    out->offsetX = offsetX;
    out->offsetY = offsetY;

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
    // one stage-0 write at each scene/canvas transition: `C3D_GetTexEnv` marks
    // the stage dirty on every call (measured from the linked libcitro3dd.a: it
    // sets bit 26+id of the context's dirty word), which is why this block no
    // longer calls it -- it builds the stage locally and setTexEnv0 sends it only
    // when the context's stage 0 holds something else.
    // THE CUT for this draw (pica::alphaTestReferenceFor): the half cover for a
    // filtered scene draw, the oracle's discard for the canvas and for nearest.
    // Stored; installAlphaTest itself is per-eye submit state.
    out->alphaRef = mdk::pica::alphaTestReferenceFor(r->toggles.enableFiltering,
                                                     draw->material.pass);
    // Stage 0's variant is chosen here and sent per eye only when the
    // context's stage 0 differs (setTexEnv0, THE DEDUPE). The variants are
    // pure functions of the debug flags, built once (r->stage0*); rdebug
    // `flat` colours per pass: Solid red, DoubleSided green, Blended blue,
    // others yellow, all opaque. A shadowed lit draw's stage 0 is
    // installShadowLitStages' instead, so it stores none.
    if (!shadowLit) {
        if (!r->stage0Built) {
            C3D_TexEnvInit(&r->stage0Scene);
            C3D_TexEnvSrc(&r->stage0Scene, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
            C3D_TexEnvFunc(&r->stage0Scene, C3D_Both, GPU_REPLACE);
            C3D_TexEnvInit(&r->stage0Overlay);
            C3D_TexEnvSrc(&r->stage0Overlay, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(&r->stage0Overlay, C3D_Both, GPU_MODULATE);
            static const uint32_t kFlatColour[4] = {
                0xFF0000FFu, 0xFF00FF00u, 0xFF00FFFFu, 0xFFFF0000u}; // ABGR
            for (int i = 0; i < 4; i++) {
                C3D_TexEnvInit(&r->stage0Flat[i]);
                C3D_TexEnvSrc(&r->stage0Flat[i], C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
                C3D_TexEnvFunc(&r->stage0Flat[i], C3D_Both, GPU_REPLACE);
                C3D_TexEnvColor(&r->stage0Flat[i], kFlatColour[i]);
            }
            r->stage0Built = true;
        }
        if (overlay) out->stage0Index = 1;
        else if (renderDebugFlags() & kRenderDebugFlat) {
            int fi = (draw->material.pass == mdk::Pass::Solid) ? 0
                   : (draw->material.pass == mdk::Pass::DoubleSided) ? 1
                   : (draw->material.pass == mdk::Pass::Blended) ? 3 : 2;
            out->stage0Index = 2 + fi;
        } else out->stage0Index = 0;
    }
    out->worldIdentity = mdk::pica::isIdentity(draw->world);
    out->haze = r->hazeLive && draw->material.shading != mdk::Shading::Original &&
                draw->material.pass != mdk::Pass::Overlay;
    return true;
}

