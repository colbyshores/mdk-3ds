// 03_shadows.inc -- the frame's light and the sun shadow: lit uniforms, the shadow map, the caster pass.
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.

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
    // redrawn on the SHADOW REFRESH schedule (pica::shadowRefreshDue), and AT
    // ONCE when the map is new, the sun has turned, the camera has moved a
    // sixteenth of the box's reach from where it was cast (the box's far edges
    // are what that would move), or the native writes are still to be seen
    // (end_frame's check needs a cast). A kept map keeps its matrices: the
    // receivers sample it where it was cast.
    const uint64_t now = svcGetSystemTick();
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
                      mdk::pica::shadowRefreshDue(r->toggles.shadowMapRefresh,
                                                  r->frame - r->shadowCastFrame,
                                                  now - r->shadowCastTick) ||
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
        r->shadowCastTick = now;
        memcpy(r->shadowCastCentre, r->cameraPos, sizeof(r->shadowCastCentre));
        r->shadowCastSun[0] = l.sun_direction.x;
        r->shadowCastSun[1] = l.sun_direction.y;
        r->shadowCastSun[2] = l.sun_direction.z;
        memcpy(r->shadowCastRig, rig, sizeof(rig));
    }
    const float values[6][4] = {
        {l.sun_direction.x, l.sun_direction.y, l.sun_direction.z, 0.0f},
        {l.sun.r, l.sun.g, l.sun.b, 0.0f},
        {l.sky.r, l.sky.g, l.sky.b, 0.0f},
        {l.ground.r, l.ground.g, l.ground.b, 0.0f},
        {r->cameraPos[0], r->cameraPos[1], r->cameraPos[2], 1.0f},
        {l.exposure, mdk::pica::sunShadowNormalOffset(r->shadowMapSize),
         mdk::pica::sunShadowDepthBias(r->shadowMapSize), 0.0f},
    };
    memcpy(r->shadowUniforms, values, sizeof(values));
    // The lit draws take the shadow in their own pass (06_draw_submit.inc drawRecorded).
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
    const bool probeNoCasters = false;
    // A native map's target has no depth buffer to clear.
    C3D_FrameBufClear(&r->shadowTarget->frameBuf, r->shadowNative ? C3D_CLEAR_COLOR : C3D_CLEAR_ALL,
                      0xFFFFFFFFu, mdk::pica::kDepthClearValue);
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
}

