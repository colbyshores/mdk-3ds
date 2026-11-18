// 01_types.inc -- the GPU vertex, the texture and mesh records, the renderer state.
// Part of renderer_c3d.cpp's single translation unit (see its header).
// No include guards, no includes of its own; the order is load-bearing.


// The 3DS's framebuffer is 240x400 portrait. The game is landscape, and the
// projection carries the 90-degree rotation.
static constexpr int kScreenWidth = 240;
static constexpr int kScreenHeight = 400;

// THE SOLID PASS'S CULL MODE, MEASURED (2026-11-17, Mandarine r1.2.1, level 7).
// Visible scene faces reach the PICA wound CLOCKWISE on screen, so culling the
// back faces is GPU_CULL_FRONT_CCW (cull the CCW ones). The previous
// GPU_CULL_BACK_CCW was chosen by reasoning about the camera's handedness and
// was marked as needing a look; the look showed it culled the FRONT faces: the
// tunnel interior and the lit floor under Kurt were invisible on every level.
// With GPU_CULL_FRONT_CCW the frame is pixel-identical to the same frame with
// culling OFF (0 of 109,312 crop pixels differ by more than 16), i.e. nothing
// visible is culled, and it matches the Linux build's layout and orientation
// (no mirror, which would also invert the winding).
static constexpr GPU_CULLMODE kSolidCull = GPU_CULL_FRONT_CCW;

// THE DISPLAY TRANSFER FLAGS.
//
// These are NOT optional. The default transfer format is NOT RGBA8, and a
// transfer that reads the framebuffer with the wrong format produces a
// striped garbage image -- no error, no crash, just noise. The devkitPro
// example (graphics/gpu/geoshader/source/main.c) spells the same set out.
//
//   GX_TRANSFER_OUT_TILED(0)  the framebuffer is in 32-bit tiled layout
//   GX_TRANSFER_RAW_COPY(0)   copy, do not convert
//   GX_TRANSFER_IN_FORMAT     RGBA8 -- the render target's format
//   GX_TRANSFER_OUT_FORMAT    RGB8 -- what the display takes
//   GX_TRANSFER_SCALING       no scaling
#define MDK_DISPLAY_TRANSFER_FLAGS                                     \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |             \
     GX_TRANSFER_RAW_COPY(0) |                                        \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |                    \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |                   \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// The 3DS's VRAM is 6 MB on the original, and the linear heap starts there,
// so EVERY linear allocation is VRAM until the heap runs out.
//
// The scratch is the per-frame staging area. It is 512 KB for now, because
// the meshes carry their own buffers and the scratch is not used yet. It
// grows when the frame's staging needs it, and the budget is checked
// against the free VRAM.
static constexpr size_t kGpuScratchSize = 256u * 1024u;

// The canvas's quad ceiling, in QUADS, and the single definition of it. The
// oracle's own cap is `CanvasQuads = 4096` (Renderer.cs:198), compared against
// the vertex count as `CanvasQuads * QuadVertices` (Renderer.cs:342); the
// Vulkan backend answers the same 4096. It is a ceiling and not an allocation —
// `ensureCanvasMesh` sizes from the quads actually queued and only clamps here.
static constexpr int32_t kCanvasQuadCapacity = 4096;

// The next power of two at or above `value`, and at least 8.
//
// The PICA200's texture size register encodes log2 of the dimensions, so a
// texture's width and height must be powers of two. A dimension below 8 is
// not addressable either: the texture unit works in 8x8 blocks.
static int32_t nextPowerOfTwo(int32_t value) {
    int32_t result = 8;
    while (result < value && result < 1024) {
        result *= 2;
    }
    return result;
}

// The 36-byte all-float vertex the PICA200 wants. The engine's 24-byte
// vertex (position, uv, packed RGBA8 colour) is expanded into this.
struct GpuVertex {
    float pos[3];
    float colour[4];
    float texcoord[2];
};

// A GPU texture.
//
// THE 3DS HAS NO PALETTE TEXTURE FORMAT, so an index texture is EXPANDED
// on the CPU when it is created: the palette lookup happens here, once, and
// the GPU gets a plain RGBA8 image. The expansion's rule is the C#'s
// palette shader's rule exactly:
//
//     index == 0   -> TRANSPARENT (the shader discards it)
//     otherwise     -> the palette's RGB, and the alpha is FORCED to 255
//
// The palette's own alpha is NOT used: the C#'s shader takes
// `palette_texture.Load(index).rgb` and writes an alpha of 1.0, and the
// transparency comes from the index-0 discard.
//
// The data is LINEAR, not Morton-tiled: the PICA200's 32-bit texture
// formats are read linearly, and only the 16-bit and 8-bit formats go
// through the tiling transform. RGBA8 is also the only format that is
// LOSSLESS for MDK's palettes -- a measurement over the GOG data found that
// RGBA4 round-trips 0 of the 926 textures exactly, because the palettes
// are dithered ramps and 8->4 bits only round-trips multiples of 17. So
// the 3DS spends its VRAM on colour fidelity.
struct C3dTexture {
    C3D_Tex tex;
    bool valid = false;

    // The real size, and the size the texture was PADDED to. The
    // difference is what the UV transform has to undo.
    int32_t realWidth = 0;
    int32_t realHeight = 0;
    int32_t paddedWidth = 0;
    int32_t paddedHeight = 0;

    // realHeight / frameCount, computed once per texture on first draw.
    // The ARM11 has no integer divide (R11), so the per-draw site caches the
    // quotient here instead of dividing every batch every frame.
    int32_t frameHeight = 0;

    // When this texture was last drawn, for the eviction order.
    uint32_t lastUsed = 0;

    // THE SOURCE, kept so that an EVICTED texture can be made again.
    //
    // THE EVICTION HAS TO BE REVERSIBLE. The 3DS has six megabytes of VRAM
    // and the linear heap starts there, so a texture that does not fit is
    // evicted -- and an evicted texture whose source is gone can never come
    // back, because the caller holds an id and not the pixels. The symptom
    // is an arena surface that stops being textured for the rest of the
    // session: the id is still good, the GPU copy is not, and the draw is
    // skipped.
    //
    // The source is in SYSTEM memory, not the GPU's: the palette indices
    // are a quarter of the expanded RGBA8 and an eighth of the padded one.
    std::vector<uint8_t> source;
    // The 256x1 palette the indices expand through (index textures only).
    std::vector<uint8_t> palette;
    // True when `source` holds palette indices, false when it holds the
    // RGBA bytes of an RGBA texture.
    bool indexed = false;

    // The source changed while the old GPU copy may still be in flight. The
    // replacement waits for the next frame boundary (AGENTS.md P3).
    bool stale = false;

    // The caller released this texture while its GPU copy may still be in
    // flight. The copy waits for the next frame boundary; the id is already
    // invalid to the caller.
    bool releasePending = false;
};

// A mesh: the vertices, in LINEAR memory, where the GPU reads them.
struct C3dMesh {
    void* data = nullptr;
    int32_t capacity = 0;
    int32_t count = 0;
    bool valid = false;
    // The caller has given the mesh up, but the linear memory is still owed to
    // a frame that may be on the GPU. Freed at the next frame boundary, exactly
    // like a texture's releasePending (AGENTS.md P3).
    bool releasePending = false;
};

// The one frame whose geometry is censused (see `Census`). It has to be a frame
// after the level-7 landing (the golden lands at step 43) so the census measures
// the standing camera the golden pins, and it has to be well inside the 30 s the
// emulator harness runs, which reaches frame ~1700.
static constexpr int32_t kCensusFrame = 120;

// A one-shot census of ONE frame's triangles, for the named level-7 picture
// defect (docs/CONTINUATION.md "NEXT": "sample triangles near the spawn and
// compare the vertices/MVP/clip values with host-derived geometry; inspect the
// PICA winding and depth/cull behavior before modifying renderer state").
//
// Batch 77 traced the FIRST triangle of the first draw and that was not enough:
// it is a far-distance triangle at y=438 and it says nothing about the room the
// camera is standing in. This counts every triangle the frame queues, in the
// frame's real per-draw matrix, and sorts it by where it landed: behind the
// camera, outside the clip square, or on screen -- and for the on-screen ones,
// which way they wind. The winding is what decides the cull enum, and the
// on-screen count is what separates "the room is not queued" from "the room is
// queued and the GPU is throwing it away".
//
// It is COLD by construction: `active` is true for exactly one frame of the
// process, so the per-triangle loop runs once in a run and every other frame
// pays one integer compare.
struct Census {
    bool active = false;
    int32_t triangles = 0;   // queued triangles seen
    int32_t behind = 0;      // a vertex with clip w <= 0
    int32_t offscreen = 0;   // the whole bbox outside [-1,1] IN X/Y
    int32_t onscreen = 0;    // the bbox touches the clip square IN X/Y — see
                             // depthOut for the depth half, which this does NOT
                             // test: "inside the clip square" is an x/y statement
                             // and reading it as "the GPU will show it" over-reads
                             // it (review-1 F5).
    int32_t depthOut = 0;    // of those, the ones wholly outside [-1,1] in ndc z
    int32_t cw = 0;          // on-screen, negative signed area in NDC (y up)
    int32_t ccw = 0;         // on-screen, positive
    int32_t near60 = 0;      // on-screen AND within 60 units of the eye
    float areaSum = 0.0f;    // sum of |ndc signed area| over the on-screen ones
    float loX, hiX, loY, hiY;   // the ndc bbox of everything on screen
    float nLoX, nHiX, nLoY, nHiY;  // the same, for the near60 ones only
    float minW = 0.0f;       // nearest and farthest clip w seen
    float maxW = 0.0f;
    float bestW = -1.0f;     // the closest on-screen triangle's w, for the sample
    int32_t bestDraw = -1;
    int32_t bestFirst = -1;
    float bestV[9] = {0};    // its three world positions
    float bestArea = 0.0f;
    float bestNdc[6] = {0};  // its first two vertices' ndc x,y
};

struct RendererC3D {
    C3D_RenderTarget* targetLeft = nullptr;
    C3D_RenderTarget* targetRight = nullptr;
    C3D_RenderTarget* targetBottom = nullptr;

    // The camera, kept so each eye can be rendered with its own offset.
    float view[16] = {0};
    float projection[16] = {0};

    // The eyes' separation, in the game's units.
    float eyeSeparation = 0.0f;

    DVLB_s* dvlb = nullptr;
    shaderProgram_s program;

    // The uniforms' LOCATIONS, not their register numbers.
    //
    // THE LOCATION COMES FROM THE SHADER'S OWN TABLE, AND THAT IS THE ONLY
    // PART OF THE OLD STORY HERE THAT SURVIVED.
    //
    // WHAT THIS COMMENT USED TO ASSERT, AND THE MEASUREMENT THAT KILLED IT
    // (2026-11-12, iteration 145): it said the `.constf constants(...)` declared
    // at the top of `mdk_c3d.v.pica` pushes the named `.fvec` uniforms up, so
    // `mvp` is NOT at vertex uniform 0 and writing it there would overwrite the
    // constants. The guest log says otherwise, on every run: `uniforms: mvp=0
    // uvtransform=4 tint=5`. Picasso gives the named `.fvec` uniforms addresses
    // from 0 in DECLARATION ORDER and allocates `.constf` separately; the
    // constants do not push anything up, and `mvp` really is vertex uniform 0.
    // (Verified by reordering the declarations: with `uvtransform, tint, mvp[4]`
    // the same line reads `uniforms: mvp=2 uvtransform=0 tint=1`.)
    //
    // WHY THAT MATTERS AND WHY IT ALSO FIXED NOTHING: the wrong claim made
    // "the matrix the vertex shader holds is not the matrix
    // `registers()`+`C3D_FVUnifMtx4x4` wrote" look like a location collision.
    // It is not one -- moving `mvp` to register 2 changes nothing at all, and
    // the canvas is invisible at both locations. See docs/OPEN_ITEMS.md, "The
    // 3DS canvas draws no pixels", for what IS measured.
    //
    // The location is what the shader's own uniform table says. Read it; do not
    // assume a register number.
    int mvpLocation = 0;
    int uvLocation = 0;

    // The frame's per-eye view-projection, with both PICA conversions baked in,
    // for the target this frame's draws go to. A queued draw that carries a
    // world matrix composes it against THIS matrix (pica::drawMatrix), which is
    // what lets the 3DS place an object at all: with one frame-wide matrix and
    // no per-draw one, every object was drawn at the arena's own coordinates no
    // matter where its script put it.
    //
    // `installEye` writes it once per eye pass, immediately before that eye's
    // replay of the queue, and the draws read the matrix of the eye they are
    // in. There is one entry and not two because a frame draws an eye, then the
    // next: the queue is replayed per eye (batch 30), so the matrix that is
    // current is always the matrix of the eye being drawn -- and a draw that
    // carries its own world composes against it (`drawMatrix`, cached below).
    float eyeVp[16] = {0};

    // Whether the uniform currently holds `eyeVp` (the frame's matrix) or some
    // draw's own.
    //
    // C3D_FVUnifMtx4x4 dirties the uniform buffer every time it is called, so
    // the level geometry -- every draw of which carries an identity world --
    // must not pay for a write it does not need. This is the cache the cull mode
    // below uses, for the same reason: a per-draw C3D_* call is real ARM11 time,
    // and the vault's "redundant per-draw PICA200 state writes" note is that
    // lesson confirmed on two other projects. Anything that writes the matrix
    // behind this backend's back has to say so here.
    bool matrixIsFrame = true;

    // This frame's eye plan (how many passes, and each eye's offset), computed
    // once in mdk_render_begin_frame and consumed by mdk_render_end_frame.
    mdk::pica::EyePlan eyes;

    // The vertex scratch. Everything the CPU writes for the frame lives
    // here until it is uploaded.
    void* scratch = nullptr;
    size_t scratchSize = 0;

    // The GPU resources. The ids the seam hands out are indices into these.
    std::vector<C3dTexture> textures;

    // The draw counter, for the eviction order.
    uint32_t drawCounter = 0;
    // The draw counter's value when the current frame began. A texture
    // stamped at or after it has been used by THIS frame's commands, and
    // the GPU may still be reading it: it cannot be freed (AGENTS.md P3).
    uint32_t frameStartUse = 0;
    std::vector<C3dMesh> meshes;

    // Edge-trigger for the dynamic-mesh allocation failure below. A frame that
    // cannot make its mesh calls this again on EVERY frame — measured 1710 lines
    // in 27 s of guest time on the forced-failure run — and a log that prints 60
    // times a second is a log nobody reads, which is the opposite of the
    // visibility review-10 F3 asked for. Cleared by the next success.
    bool meshAllocFailureLogged = false;

    // The frame's queued draws, submitted in the oracle's pass order. The C#
    // collects every command of the frame and renders it one pass at a time
    // (Renderer.cs:1000-1004); recording draws as they arrive instead puts a
    // blended object in front of arena glass that had not been drawn yet, and
    // on THIS backend -- where the depth test is off and paint order is the
    // only occlusion there is -- that is the whole picture (draw_queue.h).
    mdk::DrawQueue queue;

    // The palettes, kept so a texture can be expanded later. The 3DS has
    // no palette texture, so the palette's only job is the expansion.
    std::vector<std::vector<uint8_t>> palettes;

    // The cull mode last written to the CITRO3D CONTEXT. citro3d keeps the
    // render state in ONE global context, not per render target --
    // C3D_FrameDrawOn does not restore it -- so anything that binds another
    // target mid-frame inherits this mode.
    //
    // C3D_CullFace always marks the effect block dirty, even when the value
    // is unchanged; without this cache every mesh draw would flush an
    // effect-block rebind at the next draw.
    GPU_CULLMODE cullMode = GPU_CULL_NONE;

    // The depth state last written to the citro3d CONTEXT, for the same reason
    // the cull mode above is cached: C3D_DepthTest dirties the effect block on
    // every call, so an all-Solid frame must not re-install it per draw.
    //
    // Two bools and not one enum, because the oracle's rule has two independent
    // bits (Renderer.cs:606-607): the blended pass tests without writing, and
    // the exempt passes neither test nor write.
    bool depthTest = true;
    bool depthWrite = true;

    // The blend state last written to the citro3d CONTEXT, cached the same way.
    //
    // The pair it was derived from is cached rather than the factors, because
    // `pica::blendStateForPass` is a pure function of exactly these two and
    // two integer compares are cheaper than five. -1 is "nothing installed",
    // so the first draw of a frame always writes.
    int blendPass = -1;
    int blendMode = -1;

    // The tint uniform's location (see the shader: register 5, after the mvp
    // and the uv transform). A location and not a literal, for the reason the two
    // above are: the shader's own table is what says where it lives.
    int tintLocation = 0;

    // Whether the tint uniform currently holds white -- the state the cull mode,
    // the depth state, the blend state and the matrix are cached in, for the same
    // reason: `C3D_FVUnifSet` dirties the uniform buffer on every call, and an
    // all-Solid frame must not pay for a write that changes nothing. installEye
    // writes white and sets this true, so the first canvas draw of an eye is the
    // first write and the scene draws after it cost one compare.
    bool tintIsWhite = true;

    // THE CANVAS BATCH. The quads are held here between canvas_begin and
    // canvas_end, and canvas_end turns them into ONE mesh upload plus one queued
    // record per material group -- the oracle's own shape (Renderer.cs:340-358
    // accumulates and merges, :727-745 queues them). Nothing is emitted until the
    // per-eye replay, which is what `c3d_frame_test.sh` refuses anything else to
    // do (review-1 F4, review-3 F1).
    //
    // The quads are kept as well as the corners because each record needs its
    // material, and because merging consecutive same-material quads -- which is
    // what the oracle does -- has to read them.
    //
    // Both are cleared and refilled every frame and neither is freed: the capacity
    // they reach is the HUD's steady state, and per-frame growth is what
    // AGENTS.md 10 forbids in a hot path (the same argument `DrawQueue` makes).
    std::vector<mdk::CanvasQuad> canvasQuads;
    std::vector<mdk::Vertex> canvasVerts;

    // The mesh the canvas's corners are uploaded into, and what it can hold.
    // Made on the first frame that draws a canvas, sized to that frame, and grown
    // by doubling after that: the HUD's quad count changes with the inventory and
    // the digits, and a frame that needs more than the mesh holds is the only
    // time any of this reallocates.
    MdkMeshId canvasMesh = -1;
    int32_t canvasCapacity = 0;

    // The texture a flat canvas quad is drawn with: eight texels of white, so the
    // tint is the whole colour. See `canvasWhiteNote` in the create path.
    MdkTextureId canvasWhite = -1;

    // The frame's canvas census, for the log line the emulator harness reads
    // (`emurun.sh --expect hud`): the quads queued by canvas_end and the Overlay
    // draws the replay actually emitted.
    int32_t canvasQuadsThisFrame = 0;
    int32_t canvasDrawsThisFrame = 0;
    int32_t canvasFrames = 0;

    int frame = 0;
    int32_t drawCount = 0;
    int32_t triangleCount = 0;
    Census census;
    bool diagnosticTriangle = true;
    bool ready = false;
    bool inFrame = false;

    // The Enhanced Mode feature toggles (SPEC.md 7.9). The 3DS enhanced
    // features land behind these flags, each strictly on the PICA200;
    // until a feature exists its flag is stored and read by nothing,
    // which is the point of plumbing them first: the config file, the
    // runtime setter and the future settings menu all work today.
    mdk::EnhancedToggles toggles;

    // The enhanced look's light rig (mdk_render_set_lighting), held for the haze
    // (pica_matrix.h, THE HAZE). hazeLive and hazeExponent are decided once per
    // frame, before the eye loop; hazeOn caches whether the haze uniform and
    // combiner stage 1 currently haze (the same discipline as cullMode: the cache
    // describes the context, and installEye resets both every eye).
    mdk::Lighting lighting;
    int hazeLocation = 0;
    float hazeExponent = 0.0f;
    bool hazeLive = false;
    bool hazeOn = false;

    // True after C3D_FrameEnd and before the next C3D_FrameBegin. The GPU
    // may still be reading the last frame's textures then. C3D_FrameBegin
    // waits for the previous queue, so it is the completion boundary.
    bool gpuPending = false;

    // The capture accounting, for the frontend's "asked for a picture, got a
    // picture" rule. The request is counted at begin_frame; the readback
    // (03_frame.inc's writeScreenshotBmp: a GX transfer of the left eye
    // target into linear memory, written as a BMP) runs at that frame's
    // end, and `capturesWritten` counts the files it actually wrote -- so
    // the frontend can still tell "asked and got nothing" from "nobody
    // asked", which is the rule this accounting exists for.
    // See `mdk_render_capture_stats`.
    int32_t capturesRequested = 0;
    int32_t capturesWritten = 0;
    // The pending capture's output path (a fixed buffer: a frame-time
    // allocation for a diagnostic path is not worth it).
    char pendingScreenshot[128] = {};
};

RendererC3D* asC3d(MdkRenderer r) {
    return static_cast<RendererC3D*>(r);
}

// The MDK vertex, expanded into the PICA200's layout.
inline GpuVertex expand(const mdk::Vertex& v) {
    GpuVertex g;
    g.pos[0] = v.pos.x;
    g.pos[1] = v.pos.y;
    g.pos[2] = v.pos.z;
    // The engine's colour is RGBA8 with R in the low byte.
    g.colour[0] = (float)(v.colour & 0xFFu) / 255.0f;
    g.colour[1] = (float)((v.colour >> 8) & 0xFFu) / 255.0f;
    g.colour[2] = (float)((v.colour >> 16) & 0xFFu) / 255.0f;
    g.colour[3] = (float)((v.colour >> 24) & 0xFFu) / 255.0f;
    g.texcoord[0] = v.uv.x;
    g.texcoord[1] = v.uv.y;
    return g;
}

