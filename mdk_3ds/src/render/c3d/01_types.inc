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
//   GX_TRANSFER_SCALING       none at scale 1; 2x2 averaging (SCALE_XY) for
//                             edge smoothing at scale 2
//
// The set is displayTransferFlags(scale) in 02_lifecycle.inc.

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

// THE PER-DRAW STATE DEDUPE's categories (03_frame_state.inc, THE DEDUPE).
enum DedupeCategory {
    kDedupeTexEnv0 = 0,   // combiner stage 0
    kDedupeBufInfo,       // the vertex buffer table
    kDedupeUv,            // the uv-transform uniform
    kDedupeTint,          // the tint uniform
    kDedupeMvp,           // the mvp matrix (scene, canvas and caster draws)
    kDedupeWorld,         // the lit program's world matrix
    kDedupeTexBind,       // a texture unit's bind
    kDedupeCategories
};

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
// THE HD BUDGET: the linear-heap bytes the HD copies may hold at once -- a
// CEILING, and below it whatever the linear heap has free above a reserve
// (hdBudgetNow, 04_hd_stream.inc). Under the HD rule (spec 3.1) a covered
// surface's original is never made while the toggle is on, so the HD copies ARE
// those surfaces' textures, and the fixed 6 MB share the copies had beside the
// originals (level 3's pack is 21.6 MB against ~14-16 MB free after a load,
// spec 4.1) left the heap the originals no longer use idle while the copies
// evicted each other. The ceiling covers level 7's whole set (12.1 MB with its
// chains); the reserve is kept for what is not an HD copy -- meshes, the
// expansions of the uncovered textures, Kurt's frames, the glow -- which also
// evict HD copies first when they find the heap full (makeTexture, the mesh
// allocator).
static constexpr size_t kHdBudgetBytes = 12u * 1024u * 1024u;
static constexpr size_t kHdLinearReserveBytes = 2u * 1024u * 1024u;
// How many .t3x a frame boundary imports at most. The read and the LZ
// decompress are the reader thread's (04_hd_stream.inc hdUnpackWork), so an
// import is a copy into the linear heap -- ~1 ms, the uncompressed orig pack's
// measured figure (37 ms for 39 makes) -- and four keep the boundary's share
// small while an arena's set lands in a few frames. They wait at the boundary,
// never inside a draw.
static constexpr int32_t kHdImportsPerFrame = 4;
// The most one pack texture can need: the worst file of the LARGEST cap the
// manifest gate accepts (main_3ds.cpp loadCompressedManifest). The HD pack is
// capped at 512 -- 512x512 ETC1A4 with its chain is 349504 bytes -- and the
// orig pack (romfs:/orig) at 1024, whose arena textures pad up to 1024x512:
// 1024x512 ETC1A4 with its chain is 699008 bytes (pica::textureBytes,
// mipLevelsFor(1024,512) = 6; an ETC1 copy of the same texture is half,
// 349504). A read is only handed out when this much could be made room for, so
// a full budget does not re-read the same files from SD every frame (measured
// with a 1 MB budget: 13-15 queued textures re-read continuously before this
// rule). Raising it for the bigger pack makes the dispatch gate STRICTER, not
// looser -- eviction and the import's own budget check (which books the file's
// real size, not this bound) are untouched. No buffer is sized from this: the
// reader mallocs the file's actual length (async_file_reader.cpp readWhole).
static constexpr size_t kHdLargestBytes = 699008u;
// A failed import waits this many frames before it is asked for again, at most
// kHdMaxFailures times.
static constexpr int32_t kHdRetryFrames = 120;
static constexpr int32_t kHdMaxFailures = 3;
// An unlinked copy is freed after this many frames (10 s at 60 fps).
static constexpr int32_t kHdUnlinkedFrames = 600;
// THE ORIG PACK'S READS (C3dTexture::packPath, 04_streams.inc pumpPackOriginals):
// at most this many of its files are with the reader at once -- an unpolled
// finished read still holds its slot, so the import budget below throttles the
// reads as well -- and at most this many are imported per frame boundary. An
// uncompressed .t3x imports in ~0.1-4 ms on the Old 3DS (the largest, 1024x512
// with its chain, is the 4), so two keep the boundary's share small.
static constexpr int32_t kPackReadsInFlight = 2;
static constexpr int32_t kPackImportsPerFrame = 2;
// A packed original NO DRAW ASKED FOR (a hint: the pump's walk, its re-ask, a
// hidden group) is read and imported only into FREE linear memory, this much of
// it kept back for the makes the player's view needs: it never evicts. Without
// the rule, a resident set bigger than the pool (level 7's is: 762 of 1686 makes
// failed at full residency) turned hints into an endless evict-and-read-again
// cycle. A first figure, not a measured one: one large arena make's worth.
static constexpr size_t kPackHintReserveBytes = 1024u * 1024u;

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

    // The source has holes (pica::textureSourceHasHoles), decided when the
    // source is taken: a surface drawn with it does not cast the sun's shadow.
    bool hasHoles = false;

    // THE HD COPY this texture is linked to: an index into
    // RendererC3D::hdCopies, -1 for none (docs/HD_TEXTURES_3DS_SPEC.md). The
    // copies are keyed by FILE and owned by that table, not by the texture: a
    // texture released and made again -- Kurt's animation frames, cycled by the
    // sprite residency -- relinks to a copy still resident instead of reading it
    // from SD again (measured before the table: 440 imports in 3000 frames on
    // level 3 with resident bytes flat, Kurt's idle loop re-read each cycle).
    int32_t hdCopy = -1;
    // MADE FROM THE PACKS' FILES ALONE (mdk_render_create_compressed_file with an
    // HD path; docs/HD_TEXTURES_3DS_SPEC.md 3.1): the packed original (packPath)
    // and the HD copy (hdCopy) of one texture. While enableHdTextures is on the
    // HD copy is the texture -- requested ahead, drawn when resident, and the
    // packed original is never read for it (drawsHdOnly, 04_make.inc) --
    // and while it is off the packed original is, as for any other. Cleared when
    // the texture stops being a packed original (a stamp, its index pixels).
    bool hdOnly = false;
    // A CANVAS picture's copy (mdk_render_create_hd_image): eligible on
    // the canvas's Original-shaded draws, where a world texture's never is.
    bool hdCanvas = false;

    // The mip levels below level 0 this texture was MADE with (Enhanced Mode
    // enableMipmaps, decided when the GPU copy is made; pica::mipLevelsFor).
    // The draw path lets the sampler use them only while the toggle is on.
    int32_t mipLevels = 0;
    // NEVER DRAWN WITH A CHAIN (mdk_render_mark_unmipped, mdk_render_mark_canvas,
    // the first canvas draw): its draws are all Original-shaded -- the sky's
    // slices, the canvas's pictures and font atlases -- and 06_draw.inc holds an
    // Original draw at level 0 whatever the texture holds. So no chain is made
    // for it: the same pixels, a quarter less memory than a texture with one.
    bool noMips = false;

    // The STORE the GPU copy was made in (EnhancedToggles::texture16: RGBA8, or
    // RGB565 / RGBA5551 / RGBA4 by pica::expandedTexelFormat), decided when the
    // GPU copy is made, from the expanded texels' alpha.
    mdk::pica::TexelFormat format = mdk::pica::TexelFormat::Rgba8;
    // DRAWN ON THE CANVAS: stays RGBA8 whatever texture16 says (06_draw.inc sets
    // it on the first Overlay draw and has a 16-bit copy made again at the next
    // frame boundary). The HUD's digits and the fonts keep the palette's exact
    // colours; they are small, so the bandwidth they would save is nothing.
    bool keepRgba8 = false;

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
    //
    // A PACKED ORIGINAL (packPath below) keeps NO source: its file is the
    // source, and an eviction is undone by reading it again.
    std::vector<uint8_t> source;
    // The 256x1 palette the indices expand through (index textures only).
    std::vector<uint8_t> palette;
    // True when `source` holds palette indices, false when it holds the
    // RGBA bytes of an RGBA texture.
    bool indexed = false;

    // A COMPRESSED texture (mdk_render_create_compressed_texture: Kurt's original
    // frames as ETC1A4, EnhancedToggles::kurtCompressed; and the PACKED ORIGINAL,
    // packPath below, whose bytes are read from its file instead). `source` holds
    // the .t3x file's bytes and the GPU copy is tex3ds' import of them (importCompressed,
    // 04_textures.inc) -- NEVER a palette expansion; an eviction is undone by
    // importing again. `rect` is the real image's rectangle in the imported
    // texture (pica::compressedFrameRect), drawn through like an HD copy's, and
    // `gpuBytes` what the import holds (C3D_TexCalcTotalSize, chain included).
    bool compressed = false;
    mdk::pica::CompressedRect rect;
    uint32_t gpuBytes = 0;

    // THE PACKED ORIGINAL (mdk_render_create_compressed_file: an arena texture's
    // original look from the orig pack, romfs:/orig): a compressed texture made
    // from a FILE the reader thread reads. The bytes are not kept -- three
    // hundred arena textures' .t3x would be ~11 MB of the main heap -- so an
    // eviction is undone by reading the file again. Its id is good from creation
    // while its GPU copy waits (valid=false), as a chunked expansion's does:
    //   PackIdle    made (valid), or evicted and not asked for yet;
    //   PackQueued  in RendererC3D::packQueue, waiting for a reader slot;
    //   PackReading with the reader on kChannelOrig, tagged by the texture's id;
    //   PackFailed  unreadable, or not this set's .t3x: the core gives it its
    //               index pixels (mdk_render_texture_needs_pixels) and it is an
    //               ordinary index texture from then on.
    // `palette` holds the stamp those index pixels expand through.
    enum PackState : uint8_t { PackIdle, PackQueued, PackReading, PackFailed };
    std::string packPath;
    PackState packState = PackIdle;
    // The frame whose draw-time make found no room for it (finishPackNow): the
    // file is not read again by a later record or the other eye of that frame.
    int32_t packNoRoomFrame = -1;

    // A STRIP OVER THE PICA'S 1024 (mdk_render_create_compressed_file's
    // createStrip): never drawn itself, only its PAGES are -- packed originals of
    // their own, `framesPerPage` whole frames each (pica::stripPage), shared by
    // every palette's strip of the same file and released with the level's
    // scope. mdk_render_draw_mesh sends each draw to its frame's page.
    std::vector<int32_t> pages;
    int32_t framesPerPage = 0;

    // The source changed while the old GPU copy may still be in flight. The
    // replacement waits for the next frame boundary (AGENTS.md P3).
    bool stale = false;

    // A BULLET HOLE changed the source (mdk_render_update_texture, package #16).
    // The GPU copy is made again at a frame boundary, ONE such texture a frame
    // (settleDeferredTextures): an expansion is a whole texture's palette lookup
    // and Morton tiling, and the owner's rule is no main-thread stalls. Until its
    // turn the old copy draws -- the hole appears a frame or two late.
    bool restamped = false;

    // A STREAMED picture (mdk_render_update_paletted, package #20c: a movie's
    // frames, the splash): its GPU copy is an RGB565 store with no mip chain,
    // index 0 black, rewritten IN PLACE from `source` through `palette` at a frame
    // boundary (pica::streamTexelsRgb565), ONE streamed texture per boundary.
    // `streamDirty`: a new picture waits for that boundary; the updates before it
    // collapse into the newest (drop frames, never stall).
    bool streamed = false;
    bool streamDirty = false;

    // The caller released this texture while its GPU copy may still be in
    // flight. The copy waits for the next frame boundary; the id is already
    // invalid to the caller.
    bool releasePending = false;

    // A CHUNKED EXPANSION in progress (04_expansion.inc): a large indexed
    // texture is expanded in slices across frame boundaries instead of in
    // one ~40-80 ms make. `expandPixels` is the private RGBA8 chain buffer
    // (malloc, freed by dropExpansion or the finishing upload); the stage
    // walks row slices -> filter slices -> mip slices -> format+pack slices
    // -> upload. While pending the texture is not valid and must never be
    // bound; a draw that needs it finishes the expansion synchronously
    // (makeTexture), so the picture is never wrong, only early.
    bool expandPending = false;
    bool expandWanted = false;   // chunked start deferred by the pending cap
    // A stale re-make expanding in slices while the OLD copy still draws
    // (valid stays true): the finish swaps the copy at a frame boundary.
    bool stalePending = false;
    uint8_t* expandPixels = nullptr;
    int32_t expandRow = 0;
    int32_t expandStage = 0;
    mdk::pica::FilterCursor expandFilter;   // stage 1's, then stage 2's, slices
    bool expandSawTransparent = false;
    int32_t expandLevels = 0;
    size_t expandPackAt = 0;
    mdk::pica::TexelFormat expandFormat = mdk::pica::TexelFormat::Rgba8;
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

// ONE HD COPY: a .t3x from the HD pack (ETC1 / ETC1A4, its mip chain prebuilt
// by tex3ds on the host). Asked for by a draw (Queued), read from SD by the
// reader thread (Reading), imported at a frame boundary under the budget
// (Resident), evicted least-recently-drawn first (back to None, its path kept
// so a later draw asks again). Until it is resident a draw binds the original
// texture: a surface turns HD a few frames after it is first seen.
struct HdCopy {
    enum State : uint8_t { None, Queued, Reading, Resident, Failed };
    std::string path;
    State state = None;
    C3D_Tex tex;
    uint32_t bytes = 0;
    int32_t levels = 0;
    uint32_t lastUsed = 0;
    // A failed read or import is retried, a few times, after a pause (review F4);
    // `broken` is a property of the FILE (a rotated subtexture), never retried.
    int32_t failures = 0;
    int32_t retryFrame = 0;
    bool broken = false;
    // How many textures link to this copy, and since when it has had none. An
    // UNLINKED copy is not freed at once -- Kurt's sprite residency unlinks his
    // idle frames every cycle and relinks them a second later (freeing at zero
    // re-read them: 373 imports in 2400 frames, measured) -- but it is the first
    // victim under memory pressure (texture or mesh allocation), it is freed once
    // unlinked for kHdUnlinkedFrames, and a queued or in-flight read of it is
    // cancelled (review of the copies table, F1/F2).
    int32_t links = 0;
    int32_t unlinkedSince = 0;
    // The image's rectangle in the texture's own texcoords (Tex3DS_SubTexture):
    // the draw maps the oracle's [0,1] uv through it, assuming no orientation.
    float left = 0.0f, top = 1.0f, right = 1.0f, bottom = 0.0f;
    // IN THE LEVEL'S RESIDENT SET (mdk_render_pin_hd_set): read on the loading
    // screen, never evicted, never freed as nobody's, while the set stands.
    bool pinned = false;
    // The pinned copy's level cap (hdUnpack: 0 none, 256, 128), the set's choice.
    int32_t topCap = 0;
};

// One queue record resolved for the frame (STEREO-REPLAY analogue,
// 2026-11-22): everything in drawRecorded that does not depend on the eye --
// mesh/texture resolution, filter and mip setup, the UV transform, the
// lit/shadow decision, stage 0, the alpha cut -- is decided ONCE here before
// the eye loop; each eye's submit then only culls against its own frustum,
// composes its own matrix and issues the draw. Frame-scoped: `draw` points
// into the queue, which outlives the eye loop and is reset after it.
struct PreparedDraw {
    const mdk::MeshDraw* draw = nullptr;
    // Indices, not pointers: the glow slot runs between prepare and the
    // later submits, and a table growth there must not strand a record.
    // Pointers are re-derived at submit (validator finding, 2026-11-22).
    int32_t meshIndex = -1;
    int32_t texIndex = -1;          // into textures, or hdCopies when texHd
    bool texHd = false;
    // A PACKED ORIGINAL whose GPU copy was not made at prepare (C3dTexture::
    // packPath): the record is kept, and the submit makes the copy only if the
    // batch is in the eye's frustum, then takes its uv from the made texture.
    bool texPending = false;
    int32_t texUnit = 0;
    float scaleX = 1.0f, scaleY = 1.0f, offsetX = 0.0f, offsetY = 0.0f;
    // The original texture's UV, when the bound surface is an HD copy: a
    // copy evicted before submit falls back to the original, with its own
    // transform (never the copy's).
    float origScaleX = 1.0f, origScaleY = 1.0f, origOffsetX = 0.0f, origOffsetY = 0.0f;
    bool overlay = false, lit = false, shadowLit = false;
    bool wholeTriangles = false, worldIdentity = false, haze = false;
    // The sampler words (filter, mip ceiling) live on the shared C3D_Tex,
    // and two draws of one texture may disagree, so they are recorded here
    // and written at submit, as the old per-eye resolve wrote them
    // (validator F1, 2026-11-23). mipLevels is the bound surface's chain;
    // the HD fallback substitutes the original's at submit.
    int32_t filter = 0;             // GPU_TEXTURE_FILTER_PARAM
    bool mipped = false;
    int32_t mipLevels = 0;
    // Vertices this record submits: the draw's own, extended at prepare
    // when an adjacent same-state non-cullable draw folds into it
    // (submit coalescing, 06_draw.inc).
    int32_t vertCount = 0;
    int alphaRef = 0;
    int32_t stage0Index = -1;       // 0 scene, 1 overlay, 2..5 flat; -1 shadow
};

struct RendererC3D {
    C3D_RenderTarget* targetLeft = nullptr;
    C3D_RenderTarget* targetRight = nullptr;
    // EDGE SMOOTHING (enableEdgeSmoothing, SPEC.md 7.9): the eye targets' scale
    // per axis. 1x1 is the panel's own 240x400; 2x1 a 480x400 target (the
    // framebuffer's 240-wide axis doubled) the display transfer averages 2x1
    // into the panel -- the 3DS default (config edgeSmoothing2x2 = 0); 2x2 a
    // 480x800 target averaged 2x2 (edgeSmoothing2x2 = 1). buildEyeTargets,
    // 02_lifecycle.inc. targetScale is the larger of the two (> 1: smoothed).
    // The viewport, the scissor and the capture read these, never kScreenWidth.
    int targetScale = 1;
    int targetScaleX = 1;
    int targetScaleY = 1;
    int targetWidth = kScreenWidth;
    int targetHeight = kScreenHeight;
    // Smoothed, the right eye borrows the left eye's depth buffer (VRAM), so the
    // second eye pass clears it first (mdk_render_end_frame).
    bool targetsShareDepth = false;
    // A scale-2 build that failed: stay at scale 1 until the toggle goes off, so
    // a full VRAM is one refusal line, not a rebuild attempt every frame.
    bool smoothingRefused = false;
    bool smoothing2x2Refused = false; // 2x2 refused: 2x1 until edgeSmoothing2x2 goes off
    // The target installEye selected last: the glow pass copies from it.
    C3D_RenderTarget* eyeTarget = nullptr;

    // THE GLOW (Enhanced Mode enableGlowBillboards, 06_glow.inc drawGlow). Made on
    // first use, in VRAM (citro3d renders only into VRAM), and released whenever
    // the eye targets are rebuilt (releaseGlow): the picture's texture (256x512,
    // one level), one single-level texture per blur level 1-5 plus the mix, each
    // with a render target (pica::kGlowSmallCount), the 2x target's averaged copy
    // (linear), the full-panel quad and the small-pass quad (linear).
    C3D_Tex glowTex;
    C3D_Tex glowSmall[mdk::pica::kGlowSmallCount];
    bool glowSmallMade[mdk::pica::kGlowSmallCount] = {};
    C3D_RenderTarget* glowTarget[mdk::pica::kGlowSmallCount] = {};
    u32* glowScratch = nullptr;
    u32* glowScratchTiled = nullptr; // the shrink re-tiled (06_glow.inc drawGlow, step 2)
    u32* copyDrain = nullptr;        // 64 bytes the readbacks' trailing copy lands in (readBack)
    float* glowQuad = nullptr;
    // THE GLOW BILLBOARDS (the 3DS default: config glowBloom = 0; 06_glow_billboards.inc
    // drawGlowBillboards): the frame's halos from mdk_render_set_glow_quads, a
    // linear vertex buffer they are written into once a frame, and the 32x32
    // soft-dot texture they are drawn with (white, alpha falling off from the
    // centre), made on first use (linear heap, 4 KB + 12 KB).
    static constexpr int32_t kMaxGlowQuads = 64;
    mdk::GlowQuad glowQuads[kMaxGlowQuads];
    int32_t glowQuadCount = 0;
    float* glowQuadMesh = nullptr;
    int glowQuadMeshFrame = -1;   // the frame it was last written for
    C3D_Tex glowHalo;
    bool glowHaloReady = false;
    bool billboardsLive = false;   // the frame's glow is billboards (end_frame)
    int32_t billboardsDrawn = 0;   // halos drawn this frame, both eyes
    float* glowLevelQuad = nullptr;
    float glowQuadWidth = 0.0f;   // the canvas width the quad was built for
    bool glowReady = false;
    bool glowRefused = false;     // an allocation failed: no glow this session
    bool glowLive = false;        // decided once per frame (prepareGlow)
    int32_t glowPasses = 0;       // this frame's glow draws, for the log
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

    // The current eye's view frustum: six planes (a,b,c,d), a point inside a
    // plane when a*x+b*y+c*z+d >= 0, in the order left, right, bottom, top,
    // near, far. Extracted from the eye's pre-conversion view-projection when
    // the eye is installed (03_eye.inc) and read by drawRecorded's per-eye
    // cull (06_draw.inc). Valid only while `frustumValid`; a draw that does not
    // opt in (MeshDraw::cullable) is never tested.
    float frustum[6][4] = {{0}};
    bool frustumValid = false;

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

    // THE SCOPE LOG (mdk_render_mark / mdk_render_release: the C#'s Renderer.Mark
    // and Release(scope), which Game.Show calls between two screens). Every
    // texture, palette and mesh the seam made, in the order it made them; a level's
    // teardown releases everything made since the level's mark, newest first
    // (package 2, the next-level transition). Each release is the ordinary deferred
    // one, so the GPU-completion rule is the frame boundary's, as always (P3).
    struct Created {
        uint8_t kind;   // 0 texture, 1 palette, 2 mesh
        int32_t id;
    };
    std::vector<Created> creationLog;

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

    // THE STAGE-0 VARIANTS (cached 2026-11-22): stage 0 has seven possible
    // contents (scene, overlay, flat x pass-colour), pure functions of the
    // debug flags, so they are built once instead of on the stack per draw
    // (C3D_TexEnvInit + src/func calls ~280x/frame). setTexEnv0 still memcmps
    // against live state, so sending stays identical.
    C3D_TexEnv stage0Scene, stage0Overlay, stage0Flat[4];
    bool stage0Built = false;

    // The frame's prepared draws (capacity kept across frames; see
    // PreparedDraw). preparedGlowAt is the index at which the glow pass
    // lands (the Overlay bucket's first prepared draw), or -1.
    std::vector<PreparedDraw> prepared;
    int32_t preparedGlowAt = -1;

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

    // THE INSETS (render.h; sniper mode's clip, game/sniper_view.h). An Own inset's
    // draws are recorded as Overlay records whose world is the whole transform --
    // the model's world, the inset camera's view-projection, the inset's rectangle
    // of the canvas and the canvas's own matrix (pica::canvasMatrix) -- so they go
    // through the canvas's path: no depth, no cull, drawn in record order. Those
    // over the canvas wait in `insetOverDraws` for the end of each eye's Overlay
    // bucket (replayQueue). A Scene inset (a round camera: the whole scene again in
    // a sub-viewport) is NOT drawn on this target: the frontend does not ask for
    // one (sniper_view.h), and one that arrives is refused once in the log.
    bool insetOpen = false;
    // THE SCENE WINDOW (mdk_render_set_scene_window), this frame's: the canvas
    // rectangle the scene's draws are scissored to in replayQueue.
    bool sceneWindowOn = false;
    float sceneWindow[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool insetOver = false;
    float insetMatrix[16] = {0};
    std::vector<mdk::MeshDraw> insetOverDraws;
    bool insetSceneRefused = false;

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
    // combiner stage 2 currently haze (the same discipline as cullMode: the cache
    // describes the context, and installEye resets both every eye).
    mdk::Lighting lighting;
    // The rig as the game handed it over; `lighting` is this with the look
    // dials applied (pica::dialledLighting, toggles' hazeDensity / exposure /
    // tintSaturation), made again every frame before the eye loop, so a dial
    // moved in the menu shows at the next frame. All dials at 100: a copy.
    mdk::Lighting rig;
    // The frame rate citro3d paces C3D_FrameBegin(SYNCDRAW) to (toggles'
    // cap30Fps: 30 or 60), as last set with C3D_FrameRate.
    float pacedFps = 60.0f;
    int hazeLocation = 0;
    // THE LIT PROGRAM (Enhanced Mode enableLighting): the same vertex
    // shader plus mdk_c3d.g.pica, bound for a Lit draw while the light is live.
    // Optional: if it cannot be made the renderer runs without it (litReady).
    shaderProgram_s programLit;
    bool litReady = false;
    bool litLive = false;      // decided once per frame (prepareLighting)
    bool litBound = false;     // the program bound is programLit (cache)
    bool lightStageOn = false; // combiner stage 1 multiplies by the light (cache)
    bool worldIsIdentity = true; // the world uniform holds the identity (cache)
    // THE GEOMETRY-STAGE RULE (03_frame_state.inc bindProgram): the pass-through
    // program (VS + mdk_pass.g.pica, DVLE[4]) for unlit scene draws once the
    // command list is in geometry mode; the program bound; whether a program
    // with a geometry shader has been bound since the command list began; and
    // the splits made this frame to leave geometry mode (each a GX queue entry).
    shaderProgram_s programPass;
    bool passReady = false;
    shaderProgram_s* boundProgram = nullptr;
    bool gsInList = false;
    int32_t geometrySplits = 0;
    int32_t vshUniformVecs = 0; // the vertex shader's uniform registers, c0 up
    int worldLocation = 0;     // the VS's world matrix (registers 7-10)
    int sunDirLocation = -1, sunColourLocation = -1, skyLocation = -1, groundLocation = -1;
    int cameraLocation = -1, lightParamsLocation = -1;
    float cameraPos[3] = {0.0f, 0.0f, 0.0f};
    // The frame's six geometry-shader uniforms (prepareLighting), kept so every
    // bind of the lit program can write them again (installLitUniforms).
    float litUniforms[6][4] = {};
    // THE SUN'S SHADOW (Enhanced Mode enableSkyShadows; pica_matrix.h, THE SUN'S
    // SHADOW). The receiving program -- the lit program WITH the shadow, one pass
    // (programShadow) -- is the lit program's vertex shader plus
    // mdk_litshadow.g.pica; the map is a PICA shadow texture in VRAM with its own
    // render target, made on first use. Optional at every step: a program or a
    // map that cannot be made leaves the frame without sun shadows, with one line.
    shaderProgram_s programShadow;
    bool shadowProgramReady = false;
    int shadowSunDirLocation = -1, shadowSunColourLocation = -1, shadowSkyLocation = -1;
    int shadowGroundLocation = -1, shadowCameraLocation = -1, shadowParamsLocation = -1;
    int shadowSunMtxLocation = -1, shadowLutMapLocation = -1;
    // THE SHADOW TINT (pica::sunShadowTintTexel): F_shadow(t) / 2 along u, the
    // edge fade's survival in alpha along v, on texture unit 2 for the shadowed
    // lit draws; refilled when the rig's sky, ground or exposure change.
    C3D_Tex shadowTint;
    bool shadowTintReady = false;
    float shadowTintRig[8] = {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
    // The frame's shadowed lit draws may run (prepareShadows): the map was made
    // for this frame, the program and the tint are ready.
    bool shadowLitLive = false;
    // The combiner holds the shadowed lit draw's four stages (06_draw.inc
    // installShadowLitStages) and units 1 and 2 hold its textures; every other
    // layout puts stages 1-3 and units 1-2 back first (leaveShadowLitStages).
    bool shadowLitStages = false;
    int8_t shadowLitHaze = -1;      // stage 3's haze in that layout: -1 unknown, 0 off, 1 on
    // An 8x8 opaque white texture: what units 1 and 2 are left holding when no
    // draw wants them. citro3d cannot unbind them (C3D_TexBind(1, NULL) reads
    // through the null pointer) and a bound unit is sampled by every fragment,
    // so it is the smallest texture there is, not the last surface.
    C3D_Tex unitIdle;
    bool unitIdleReady = false;
    // The caster pass's own program (mdk_caster.v.pica: no geometry shader) and
    // the ramp texture that turns a depth into the map's bytes.
    shaderProgram_s programCaster;
    bool casterProgramReady = false;
    int casterMvpLocation = -1;
    C3D_Tex shadowRamp;
    bool shadowRampReady = false;
    int shadowRampOrder = -1;       // the pica::ShadowByteOrder its texels are in
    // THE PICA'S OWN SHADOW WRITES (GPU_FRAGOPMODE_SHADOW), measured once
    // outside any frame (03_shadow_probe.inc testNativeShadowWrites): where they land
    // the caster pass uses them (24-bit depth); where they do not (the emulator
    // on llvmpipe) it draws through the ramp.
    bool shadowNativeTested = false;
    int32_t shadowNativeTestRuns = 0;  // tries that could not run (retried, up to 3)
    bool shadowNative = false;
    bool shadowNativeVerified = false; // the real pass wrote texels (end_frame)
    int32_t shadowNativeEmptyFrames = 0; // consecutive native frames that wrote none
    int32_t shadowMapAsked = 0;     // the config's shadowMapSize when the map was made or refused
    int32_t shadowMapSize = 0;      // the map's side: the config's (shadowMapSize) or 512, smaller while VRAM refuses
    C3D_Tex shadowTex;
    C3D_RenderTarget* shadowTarget = nullptr;
    bool shadowMapReady = false;    // shadowTex and shadowTarget are made
    bool shadowRefused = false;     // they could not be: off for the session
    bool shadowLive = false;        // decided once per frame (prepareShadows)
    float sunVp[16] = {};           // world -> the caster pass's clip (pica::sunCasterMatrix)
    float sunFrustum[6][4] = {};    // the sun volume's planes (engine VP, prepareShadows)
    bool sunFrustumValid = false;
    // THE MAP'S CACHE (config shadowMapRefresh): the map is redrawn every few
    // frames, and between redraws the receivers keep the matrices it was cast
    // with (sunVp, shadowSunMtx), so a reused map stays where the world is.
    bool shadowRecast = false;      // this frame redraws the map (prepareShadows)
    bool shadowMapFresh = false;    // the map was (re)made and holds nothing yet
    int shadowCastFrame = -1000;    // the frame it was last drawn
    uint64_t shadowCastTick = 0;    // ... and the system tick then (AUTO's clock)
    float shadowCastCentre[3] = {0.0f, 0.0f, 0.0f};
    float shadowCastSun[3] = {0.0f, 0.0f, 0.0f};
    // The rest of what the map was cast for: the reach, and the rig's sky and
    // ground (a new level's rig differs: its map is not the last level's).
    float shadowCastRig[7] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    // The receive program's uniforms, written at every bind of it: the six of
    // the light (the lit program's, with the offset and the bias in params)
    // and the four registers of world -> (u, v, depth, 1).
    float shadowUniforms[6][4] = {};
    float shadowSunMtx[16] = {};    // already in register order (pica::registers)
    int32_t shadowCasters = 0;      // draws the caster pass made this frame
    int32_t shadowHoleCasters = 0;  // casters left out: their textures have holes
    float hazeExponent = 0.0f;
    bool hazeLive = false;
    bool hazeOn = false;
    // The alpha-test reference installed (installAlphaTest's cache).
    int alphaReference = 0;

    // THE PER-DRAW STATE DEDUPE (03_frame_state.inc, THE DEDUPE). drawRecorded re-sent
    // combiner stage 0, the vertex buffer table, the uv/tint/matrix uniforms and
    // the texture bind on every draw whether or not they had changed; each is a
    // block of PICA register writes, twice per stereo frame (~490 draws). They
    // are now compared first and skipped when bit-identical. Three of the four
    // compare against CITRO3D'S OWN STORED STATE, not a copy of ours, so no
    // writer anywhere can leave them stale:
    //   * combiner stage 0 against `liveTexEnv` (C3D_GetTexEnv(0)'s pointer into
    //     the context, taken in installEye; a write is C3D_SetTexEnv);
    //   * the buffer table against `liveBufInfo` (C3D_GetBufInfo's, likewise;
    //     a write is C3D_SetBufInfo of `vbufInfo`, BufInfo_Init + BufInfo_Add of
    //     `vbufFor`, rebuilt only when the mesh changes);
    //   * a vertex uniform against citro3d's own C3D_FVUnif array.
    // The texture bind has no public getter, so it is the one SHADOW: what
    // bindTexUnit (the only C3D_TexBind) last bound on each unit, with a copy of
    // the C3D_Tex as it was bound (citro3d reads the address, size, format,
    // filter, wrap and levels out of it). installEye forgets it every eye (the
    // first bind of an eye is a real one, and with it the texture-cache clear),
    // and so does makeTexture/an HD import, where texel memory can be reused.
    C3D_TexEnv* liveTexEnv = nullptr;
    C3D_BufInfo* liveBufInfo = nullptr;
    const void* vbufFor = nullptr;
    C3D_BufInfo vbufInfo = {};
    C3D_Tex* texUnitBound[3] = {nullptr, nullptr, nullptr};
    C3D_Tex texUnitState[3] = {};
    // What it saved, by category, since the last `dedupe:` line (every 300
    // frames): writes made, writes skipped. And the command list's length in
    // 32-bit words, from C3D_FrameBegin to just before C3D_FrameEnd (every
    // split included), summed and the largest, over the same window.
    int32_t dedupeWritten[kDedupeCategories] = {};
    int32_t dedupeSkipped[kDedupeCategories] = {};
    const u32* cmdFrameStart = nullptr;
    uint32_t cmdWordsSum = 0;
    uint32_t cmdWordsPeak = 0;
    int32_t cmdWordsFrames = 0;

    // THE HD STREAM: the copies (by file), the copies a draw asked for, the
    // bytes resident against the budget, and what happened.
    std::vector<HdCopy> hdCopies;
    std::map<std::string, int32_t> hdByPath;
    std::vector<int32_t> hdQueue;
    std::vector<mdk::pica::HdSlot> hdSlotScratch;   // evictHd's view, reused
    // The SD reads, on their own thread (platform/async_file_reader.h): started
    // with the renderer, stopped and joined before C3D_Fini.
    mdk::AsyncFileReader hdReader;
    size_t hdResidentBytes = 0;
    int32_t hdImported = 0;
    int32_t hdEvicted = 0;
    int32_t hdFailedCount = 0;
    // THE LEVEL'S RESIDENT HD SET (mdk_render_pin_hd_set, spec 3.2): while
    // hdPinnedMode, an HD-only texture is drawn only from a pinned copy, the set
    // is read on the loading screen with each copy's levels over its topCap
    // dropped (hdUnpack), and no copy of it is evicted in play. hdTopCap: the
    // smallest cap in the set, for the log (0: none dropped).
    bool hdPinnedMode = false;
    // The toggle came back on: the originals the HD-off stretch made for HD-only
    // textures are given back once their set copies are resident (04_textures.inc).
    bool hdFreeOriginals = false;
    int32_t hdTopCap = 0;
    int32_t hdPinFailed = 0;

    // THE ORIG PACK'S READS (04_streams.inc pumpPackOriginals): the packed
    // originals waiting for a reader slot (texture ids; an entry whose texture
    // is no longer PackQueued is dropped when the pump next scans), and what
    // happened.
    std::vector<int32_t> packQueue;
    int32_t packImported = 0;
    int32_t packFailed = 0;
    // finishPackNow's reads on the main thread (the read had not landed): the
    // count, said with each one (`pack: MAIN-THREAD read`).
    int32_t packMainReads = 0;

    // THE STORES makeTexture chose (EnhancedToggles::texture16), counted per
    // making -- an evicted texture made again counts again -- indexed by
    // texelStoreIndex: RGB565, RGBA5551, RGBA4, RGBA8. And the bytes the 16-bit
    // makings did not allocate against RGBA8. Logged per level (03_frame.inc).
    int32_t texturesMade[4] = {0, 0, 0, 0};
    size_t textureBytesSaved = 0;
    int32_t texturesMadeLogged = -1;   // the making count at the last line
    // The COMPRESSED textures imported (EnhancedToggles::kurtCompressed: Kurt's
    // ETC frames), counted per import as above, and the level-0 bytes they did
    // not allocate against the RGBA8 expansion of the same padded size.
    int32_t compressedMade = 0;
    size_t compressedBytesSaved = 0;

    // True after C3D_FrameEnd and before the next C3D_FrameBegin. The GPU
    // may still be reading the last frame's textures then. C3D_FrameBegin
    // waits for the previous queue, so it is the completion boundary.
    bool gpuPending = false;

    // The capture accounting, for the frontend's "asked for a picture, got a
    // picture" rule: requests are counted at begin_frame and this backend
    // writes none (see `mdk_render_capture_stats`).
    int32_t capturesRequested = 0;
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
