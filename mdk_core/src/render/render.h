#pragma once

// ============================================================================
// render.h — the flat backend seam for the MDK renderer.
//
// This is the ONLY rendering header shared game/engine code (the frontends,
// the HUD, the menus, the level view) is allowed to include. It exposes the
// renderer as a flat C-style API over an opaque handle so exactly one backend
// is selected at BUILD TIME — no virtual interface, no vtable, no per-frame
// dynamic dispatch. See docs/DOCTRINE.md "Rendering backend seam".
//
// Backends (one compiled in, chosen by which tree is compiled):
//   * mdk_linux/src/render/renderer_vk.cpp   (Vulkan 1.3, Linux)
//   * mdk_3ds/src/render/renderer_c3d.cpp    (PICA200 / Citro3D, Nintendo 3DS)
//
// No Vulkan, PICA, C3D or SDL type ever crosses this header: the handle is
// opaque and every GPU object lives behind it inside the active backend.
//
// WHY FLAT FUNCTIONS AND NOT A STRUCT OF FUNCTION POINTERS: a vtable (or a
// hand-rolled one) would put a runtime decision where the build already made
// it. Flat functions over an opaque handle keep every call a direct,
// inlinable call.
//
// ERROR HANDLING: every call returns a status. No exceptions cross this seam,
// ever. A failed creation returns a null handle; a failed per-frame
// allocation sets a sticky flag on the backend and degrades the frame rather
// than aborting.
// ============================================================================

#include "render_types.h"

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

// Opaque renderer handle. Callers never dereference it; each backend casts it
// to its own concrete renderer type internally.
typedef void* MdkRenderer;

// GPU resources. Ids are per-kind and start at 0; a negative id is "none".
// The backend owns them; the game asks for them and releases them.
typedef int32_t MdkTextureId;   // an index texture (8-bit) or an RGBA texture
typedef int32_t MdkPaletteId;   // a 256x1 RGBA palette
typedef int32_t MdkMeshId;      // a vertex buffer

// A resource scope. The C# port's `Renderer.Mark()` / `Renderer.Release()`:
// a screen marks when it starts and releases when it ends, so a screen's
// textures and meshes are freed when it does. A scope is a pair of counters,
// not a stack.
struct MdkScope {
    int32_t textures = 0;
    int32_t buffers = 0;
};

struct MdkRenderDesc {
    // The target size in pixels. The 3DS ignores this and uses the screen.
    int32_t width = 0;
    int32_t height = 0;

    // The Linux backend needs the platform's surface and the game's window
    // handle; the 3DS backend needs neither. They are void* so that no
    // platform type crosses the seam. The Linux frontend passes its
    // SDL_Window* here; the 3DS frontend passes nullptr.
    void* surface = nullptr;

    bool vsync = true;
    bool validation = false;     // Vulkan validation layers
    // Render with no window behind the swapchain (VK_EXT_headless_surface).
    // The golden-frame harness needs this: it reads the image back instead
    // of presenting it, and it is the only way to run on a machine whose
    // display server cannot present a Vulkan swapchain.
    bool headless = false;
    mdk::StereoSettings stereo;
    bool enhanced_look = false;  // the Linux build can do the enhanced look
    // The Enhanced Mode feature toggles (SPEC.md 7.9); all on unless a
    // config file or the settings menu says otherwise.
    mdk::EnhancedToggles enhanced_toggles;
    int32_t hidden_width = 1280; // the offscreen size for the tests
    int32_t hidden_height = 960;
};

// The frame's parameters. Set once per frame, before the first draw.
struct MdkFrameParams {
    float elapsed = 0.0f;         // the frame's delta time, in seconds
    uint32_t frame_index = 0;
    // THE CLEAR COLOUR IS 0xAARRGGBB, AND EVERY BACKEND MUST UNPACK IT THAT WAY.
    // The PICA's clear word is 0xRRGGBBAA, so the Citro3D backend converts on the
    // way in (mdk::pica::clearColour); passing the seam value straight through
    // paints the frame with the alpha byte as red, which is measured, not
    // theoretical — see the declaration there for the before/after pixels.
    uint32_t clear_colour = 0xFF000000u;
    // What the scene is drawn over (C# Renderer.Backdrop, which Viewer.Background sets
    // from the scripts' sky mode; package #15): Sky draws the panorama over the clear,
    // Clear only clears, Keep leaves the last frame's colour (no colour clear; the depth
    // is still cleared). A frontend that draws its own sky (the 3DS's dome) draws it
    // only under Sky.
    mdk::Backdrop backdrop = mdk::Backdrop::Sky;
    bool present = true;          // false for a frame the frontend does not show
    const char* screenshot_path = nullptr;   // Linux only; nullptr elsewhere
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Create the active backend's renderer. Returns nullptr on failure.
MdkRenderer mdk_render_create(const MdkRenderDesc* desc);

// Replace the Enhanced Mode feature toggles on a live renderer
// (SPEC.md 7.9): the settings menu's live A/B switch. Takes effect on
// the next frame; a null renderer or toggles is a safe no-op. The
// backend reads the flags once per pass at setup, so this is a plain
// struct copy, never per-draw work.
void mdk_render_set_enhanced_toggles(MdkRenderer r, const mdk::EnhancedToggles* toggles);

// Destroy a renderer created by mdk_render_create. nullptr is a safe no-op.
void mdk_render_destroy(MdkRenderer r);

// A human-readable backend name, e.g. "Vulkan 1.3" / "PICA200 Citro3D".
const char* mdk_render_backend_name(MdkRenderer r);

// What this backend can do. The shared game code asks before it uses a
// feature; it never asks the platform. See render_types.h's Lighting comment
// for the degradation policy this implements.
mdk::MdkCapabilities mdk_render_capabilities(MdkRenderer r);

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

// Create an 8-bit index texture. The original's world is paletted: the
// texture holds palette indices and the palette turns them into colours.
// `indices` is `width * height` bytes, row-major.
//
// `paletteRgba` is the 256x1 palette these indices are relative to, or
// null. It is what the 3DS NEEDS, because the PICA200 has no palette
// texture format and no programmable fragment shader, so the lookup has to
// happen on the CPU (SPEC.md 7.3). The Vulkan backend IGNORES it: it does
// the lookup in the shader, sampling the palette as a texture of its own,
// and the material's palette id binds it.
//
// The texture is `width * height` where the HEIGHT already includes the
// animation frames: the original stacks the frames vertically, and the
// frame is selected in the shader by the UV.
MdkTextureId mdk_render_create_index_texture(MdkRenderer r, int32_t width, int32_t height,
                                              const uint8_t* indices, const uint8_t* paletteRgba);

// The same, for a picture whose pixels and palette will be replaced as it plays
// (a movie frame, the splash: mdk_render_update_paletted): a backend that keeps
// such a picture in a cheaper streamed store makes it so from the start, instead
// of preparing the ordinary store first. Elsewhere identical to the above.
MdkTextureId mdk_render_create_streamed_texture(MdkRenderer r, int32_t width, int32_t height,
                                                const uint8_t* indices, const uint8_t* paletteRgba);

// Ask the backend to have a texture's GPU copy ready before it is drawn.
// On the 3DS a large indexed texture whose copy was evicted is re-expanded
// in slices across frames once prepared (its draw finishes the make
// synchronously if it comes first, so the picture is never wrong); a
// backend whose textures are resident at creation has nothing to do.
// Preparing an already-resident texture is a no-op.
void mdk_render_prepare_texture(MdkRenderer r, MdkTextureId texture);

// THE LOADING SCREEN'S WAIT (between frames, never inside one): the files the
// textures already made have asked for -- the 3DS's HD copies and packed
// originals -- read and imported now, the calling thread waiting on the
// backend's reader, until none is left that fits in memory or `maxMs` passes.
// The frontend calls it once per level, after asking for the first view's
// textures and before the first frame, so the loading screen stays up until
// that view can be drawn whole (docs/HD_TEXTURES_3DS_SPEC.md 3.1). Returns the
// files made; 0 on a backend whose textures are made at creation.
int32_t mdk_render_settle_streams(MdkRenderer r, int32_t maxMs);

// THE HD WINDOW (the 3DS, docs/3DS_ARCHITECTURE.md 4): the HD files of the
// arenas around Kurt -- his arena, its neighbours, the arenas its triggers and
// scripts name (mdk_3ds/src/hd_window_3ds.h) -- read in the given order (the
// drawn arenas' first), each with its levels wider or taller than its `caps`
// entry dropped (0 keeps them all), and kept resident while they stay in the
// window. A file that leaves it is freed once no texture draws it. Called
// between frames whenever the window changes; `count` 0 drops it. Returns the
// files pinned; 0 on a backend with no HD stream.
int32_t mdk_render_set_hd_window(MdkRenderer r, const char* const* paths, const int32_t* caps,
                                 int32_t count);
// How many of `paths` (null: the whole window) are not resident yet and still
// to come; 0 with the HD toggle off. The frontend waits on it (pumping
// mdk_render_settle_streams) before it draws an arena whose files are missing.
int32_t mdk_render_hd_pending(MdkRenderer r, const char* const* paths, int32_t count);

// Create an RGBA8 texture (the enhanced look's expanded colour textures, the
// canvas's images, the panorama).
//
// `rgba` IS RGBA IN MEMORY ON BOTH TARGETS, and each backend converts for its
// own GPU. Vulkan takes the bytes as they stand (`VK_FORMAT_R8G8B8A8_UNORM`);
// the 3DS rotates them, because the PICA's RGBA8 is a little-endian word with
// red in the TOP byte, i.e. `[A, B, G, R]` in memory. A caller that pre-rotated
// to suit one target would be wrong on the other.
MdkTextureId mdk_render_create_rgba_texture(MdkRenderer r, int32_t width, int32_t height,
                                            const uint8_t* rgba);

// The GPU memory, in bytes, that an INDEX texture of this size costs in the
// active backend.
//
// The two backends spend very differently on the same picture. Vulkan keeps
// the 8-bit palette indices as they are (one byte a pixel). The 3DS has no
// palette texture format and no programmable fragment shader, so it expands
// the indices through the palette into RGBA8 and pads the dimensions up to
// powers of two -- eight times the bytes for a frame that is 87x159.
//
// A caller that keeps a SET of textures resident asks this, so the bound it
// applies is the backend's own arithmetic and not a guess made in shared
// code. Returns 0 when the backend has no answer.
int32_t mdk_render_index_texture_bytes(MdkRenderer r, int32_t width, int32_t height);

// Create a texture from a COMPRESSED file's bytes: a tex3ds .t3x (ETC1 /
// ETC1A4, LZ-compressed or not) whose texture holds a width x height image at
// its TOP-LEFT, the rest padding (Kurt's original frames,
// mdk_3ds/tools/kurt_etc1_convert.py; EnhancedToggles::kurtCompressed). The
// bytes are copied: the backend makes the GPU copy by importing them -- no
// palette expansion, no ARM11 work but a decompress and a copy -- and makes it
// again from them after an eviction. Drawn like an index texture's id
// (material.texture), through the image's rectangle: a draw's uv 0..1 spans the
// width x height IMAGE, whatever size the quad is. So `width`/`height` are the
// image's size inside the texture, which for a Kurt frame fitted to 256x256 (the
// chute's; formats/kurt_pack.h's image size) is smaller than the frame the
// caller's quad still spans -- the frame keeps its size on screen.
//
// Returns -1 when the backend has no compressed textures -- the Linux backend
// keeps the palette indices and answers -1, so its callers draw the index
// texture as before -- or when the bytes do not import.
MdkTextureId mdk_render_create_compressed_texture(MdkRenderer r, int32_t width, int32_t height,
                                                  const uint8_t* t3x, int32_t size);

// The same, from a FILE (a pack's .t3x), stamping `paletteRgba` (256 RGBA8
// entries, may be null) on the texture so a later pixel update can fall back
// through it (mdk_render_update_texture's rule for a stamped compressed
// texture). This is the arena pack's path: a surface the pack holds as a .t3x of
// the ORIGINAL look is made from the file and the ARM11 never expands the
// palette (render/surface_cache.h's surfaceId). The texture is ASKED FOR, not
// made: the id is good at once and costs no I/O -- the backend reads the file
// off the main thread and makes the GPU copy at a frame boundary, and a draw
// that cannot wait for it makes it then. So the call is safe mid-frame. Returns
// -1 only when the backend has no compressed textures (the caller then makes
// the index texture as always) or the path is unusable. A file that turns out
// unreadable, or not a .t3x of this size, is reported later through
// mdk_render_texture_needs_pixels.
//
// `hdPath` (null or empty: none) is the HD pack's copy of the SAME texture (the
// 3DS's HD rule, docs/HD_TEXTURES_3DS_SPEC.md 3.1): the texture is then made
// from the two files alone. While the HD toggle is on only the HD file is read
// and drawn -- `path` is never read, nothing is expanded -- and while it is off
// only `path`, exactly as without one; the toggle stays live per draw.
//
// `frames` is the texture's stacked frame count (height is all of them). A
// strip taller than the 3DS's 1024 is held as pages of whole frames (the orig
// pack's <path minus .t3x>.p<k>.t3x) and each draw takes its frame's page.
MdkTextureId mdk_render_create_compressed_file(MdkRenderer r, int32_t width, int32_t height,
                                               int32_t frames, const char* path,
                                               const char* hdPath, const uint8_t* paletteRgba);

// A texture made from a FILE could not be (unreadable, or not this set's .t3x):
// the backend wants its index pixels through mdk_render_update_texture to draw
// it the ordinary way. False for every other texture, and on backends without
// files.
bool mdk_render_texture_needs_pixels(MdkRenderer r, MdkTextureId texture);

// The GPU bytes a MADE texture holds right now, in the store it was actually
// made in (an index texture's RGBA8 or 16-bit store and its mip chain, a
// compressed texture's ETC data). What a residency that wants to count real
// bytes asks once the texture exists. 0 when the texture is not resident or
// the backend has no answer (then mdk_render_index_texture_bytes is the
// estimate).
int32_t mdk_render_texture_bytes(MdkRenderer r, MdkTextureId texture);

// THE BACKEND'S FILE READER, lent to a caller that streams from a pack (Kurt's
// romfs:/kurt/kurt.pak, KurtSprite's stream): `size` bytes at `offset` of
// `path`, read OFF the main thread (AGENTS.md P5: no blocking read inside a
// frame). The 3DS shares its HD reader thread (platform/async_file_reader.h) on
// a channel of its own, so a pack read is never handed to the HD importer and
// no second thread is made. One read is in flight at a time: a request while
// one is outstanding, or while the reader's slots are full, is refused -- ask
// again on a later frame. False also when the backend has no reader (the
// desktop answers false: its callers never stream).
bool mdk_render_read_range_async(MdkRenderer r, int32_t tag, const char* path, uint32_t offset,
                                 uint32_t size);

// Takes one finished range read, if any: its tag, and either its bytes
// (malloc'd, exactly the size asked for; the caller free()s them) or ok=false
// with null bytes (the file could not be opened, or ended before the range did
// -- a short read is a failure, never a partial buffer). False when nothing has
// finished.
bool mdk_render_poll_range_read(MdkRenderer r, int32_t* tag, uint8_t** bytes, uint32_t* size,
                                bool* ok);

// THE SAME READER, lent for WHOLE FILES to the menu screens (game/async_read.h:
// the slideshow's GIFs and MDK12.FLC, which the C# reads on the frame that wants
// them). The 3DS reads them on its file thread, on a channel of their own
// (platform/async_file_reader.h's kChannelFile), and hands each back BY TAG, so
// two users never take each other's reads. has: whether the backend lends a
// reader at all (the desktop: false -- its screens read at once). read: asks for
// `path` (resolved) under `tag`; false when it cannot be queued now (ask again).
// poll: the read under `tag`, once finished -- its bytes (malloc'd; the caller
// free()s them) or ok=false with null bytes; false while it is not. cancel: the
// read under `tag` dropped, whatever its state.
// `work` (null: none) runs on the reader's thread on the bytes read and replaces
// them with what it returns (malloc'd, its size in *outSize; null: ok=false) --
// the slideshow's GIF decode leaves the main thread that way.
typedef uint8_t* (*MdkFileWork)(const uint8_t* bytes, uint32_t size, uint32_t* outSize);
bool mdk_render_has_file_reader(MdkRenderer r);
bool mdk_render_read_file_async(MdkRenderer r, int32_t tag, const char* path, MdkFileWork work);
bool mdk_render_poll_file_read(MdkRenderer r, int32_t tag, uint8_t** bytes, uint32_t* size,
                               bool* ok);
void mdk_render_cancel_file_read(MdkRenderer r, int32_t tag);

// Create a 256x1 RGBA palette.
MdkPaletteId mdk_render_create_palette(MdkRenderer r, const uint8_t* rgba256);

// Replace a texture's contents in place. Used for the bullet holes, which
// change a texture's indices without changing its size.
void mdk_render_update_texture(MdkRenderer r, MdkTextureId texture,
                                int32_t width, int32_t height, const uint8_t* pixels);

// A STREAMED picture (package #20c: a movie's frame, the splash's palette):
// replace an index texture's indices and/or the palette it is drawn through,
// in place, as often as every frame (the C#'s PalettedImage.SetIndices and
// SetPalette). `indices` (width * height, the texture's size) and
// `rgba256` (256 RGBA entries) may each be null: that half stays.
//
// Vulkan updates the index image and the palette image (`palette` is the
// palette id the draws name). The 3DS has no palette texture, so it expands the
// picture through the palette on the CPU: the first update turns the texture
// into a STREAMED one -- RGB565, no mip chain, index 0 black (a streamed picture
// is the bottom layer, over the black clear) -- and each frame boundary
// rewrites it in place, ONE streamed texture per boundary, after
// C3D_FrameBegin's wait (AGENTS.md P3). Updates between two boundaries
// collapse into one: the newest picture is the one drawn, the others are
// dropped (the owner's rule: drop frames, never stall).
void mdk_render_update_paletted(MdkRenderer r, MdkTextureId texture, MdkPaletteId palette,
                                const uint8_t* indices, const uint8_t* rgba256);

// Give a (texture, palette) pair a whole colour image instead of the
// palette expansion (a mod's replacement: the C# Renderer's Replace). The
// image is `frames` frames of width x height stacked downwards, RGBA8
// premultiplied, any size; the enhanced look samples it with mips. Kept
// until the index texture changes (then the expansion returns) or either
// id is released. The 3DS backend holds no colour textures and ignores it.
void mdk_render_replace_colour(MdkRenderer r, MdkTextureId texture, MdkPaletteId palette,
                               int32_t frames, int32_t width, int32_t height,
                               const uint8_t* rgba);

// A CANVAS PICTURE WITH AN HD IMAGE (the 3DS HD pack's .t3x at `hdPath`, resolved
// by ModImages::resolve): an index texture as mdk_render_create_index_texture
// makes it, except that with the backend's HD set on the HD file is its only
// copy -- the indices are not expanded, and the copy is held for with the
// level's set (owner, 2026-11-25: "covered pictures load ONLY the HD file").
// With HD off it is made from the indices at its first draw. A backend with no
// compressed HD pack (the desktop) never gets a path and makes the index texture.
MdkTextureId mdk_render_create_hd_image(MdkRenderer r, int32_t w, int32_t h, const uint8_t* px,
                                        const uint8_t* paletteRgba, const char* hdPath);

// A CANVAS texture (a menu picture, a font atlas), said at its creation: the
// canvas keeps the palette's exact colours, so a backend that would otherwise
// store an index texture in fewer bits (the 3DS's texture16) makes this one
// full-colour from the start instead of re-making it at its first canvas draw.
// Nothing changes on a backend that keeps every texture full-colour.
void mdk_render_mark_canvas(MdkRenderer r, MdkTextureId texture);

// A texture that is only ever drawn with the original shading (the sky's
// slices): a backend that would build it a mip chain (the 3DS's enableMipmaps)
// builds none, since an Original-shaded draw samples level 0 only. The same
// pixels. A canvas texture (mdk_render_mark_canvas) is one already. Nothing
// changes on a backend that mips nothing it does not sample.
void mdk_render_mark_unmipped(MdkRenderer r, MdkTextureId texture);

// Give a canvas image (an index texture through a palette) a whole colour
// image (a mod's replacement for a HUD/menu picture: the C# Renderer's
// ReplaceImage). Drawn by the enhanced look's canvas in place of the
// filtered index texture; kept through palette changes. One frame.
void mdk_render_replace_image(MdkRenderer r, MdkTextureId texture, MdkPaletteId palette,
                              int32_t width, int32_t height, const uint8_t* rgba);

// Release one resource. The id is invalid afterwards.
void mdk_render_release_texture(MdkRenderer r, MdkTextureId texture);
void mdk_render_release_palette(MdkRenderer r, MdkPaletteId palette);

// Create a static mesh. The vertices are copied; the caller keeps its own.
MdkMeshId mdk_render_create_mesh(MdkRenderer r, const mdk::Vertex* vertices, int32_t count);

// Create a mesh whose contents are rewritten every frame. The backend
// allocates the storage once; mdk_render_update_mesh writes into it.
//
// "Every frame" is why this is a different call from `create_mesh`: the frame
// being recorded is not the only one the GPU may be working on, so the storage
// a dynamic mesh writes this frame is the slot whose GPU work has already been
// waited, not the one the frame before is still reading (AGENTS.md P3).
MdkMeshId mdk_render_create_dynamic_mesh(MdkRenderer r, int32_t capacity);

// Rewrite a dynamic mesh's vertices. `count` may be less than the capacity.
// Call it while the frame the vertices belong to is being recorded -- between
// `mdk_render_begin_frame` and `mdk_render_end_frame` -- because that is the window
// in which the backend knows which of its slots is safe to write.
void mdk_render_update_mesh(MdkRenderer r, MdkMeshId mesh, const mdk::Vertex* vertices,
                            int32_t count);

// Release one mesh. The id is invalid afterwards.
//
// The backend frees the storage at its GPU-completion boundary, NOT on this
// call: a mesh that a frame still on the GPU has drawn is an in-flight resource,
// and the caller has no way to say which frame that was (AGENTS.md P3).
void mdk_render_release_mesh(MdkRenderer r, MdkMeshId mesh);

// Mark the current resource scope, and release everything created since the
// matching mark. The C# port's `Renderer.Mark()` / `Renderer.Release(scope)`.
MdkScope mdk_render_mark(MdkRenderer r);
void mdk_render_release(MdkRenderer r, MdkScope scope);

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

// Begin the frame. Everything between this and mdk_render_end_frame is drawn
// into the frame's target. A backend that fails to begin draws nothing.
void mdk_render_begin_frame(MdkRenderer r, const MdkFrameParams* params);

// End the frame and present it.
void mdk_render_end_frame(MdkRenderer r);

// The camera for this frame. The shared game code sets it once; the
// backends differ only in how they consume it.
void mdk_render_set_camera(MdkRenderer r, const mdk::Camera* camera);

// The lighting rig. SHARED DATA: the Vulkan backend's enhanced look consumes
// all of it; the 3DS backend consumes what the PICA200 can express behind the
// Enhanced Mode toggles (SPEC.md 7.9; so far the haze). See render_types.h.
void mdk_render_set_lighting(MdkRenderer r, const mdk::Lighting* lighting);

// The frame's point lights (muzzle flashes, explosions, fires). The Vulkan
// backend uploads them; the 3DS backend ignores them.
void mdk_render_set_point_lights(MdkRenderer r, const mdk::PointLight* lights, int32_t count);

// The frame's glow billboards (render_types.h GlowQuad), replacing the last
// frame's; count 0 clears them. The 3DS backend draws them when its glow is in
// billboard mode; the Vulkan backend ignores them (its glow is the PC's bloom).
// The array is copied: the caller's can go after the call.
void mdk_render_set_glow_quads(MdkRenderer r, const mdk::GlowQuad* quads, int32_t count);

// The sky. Drawn first, behind everything.
void mdk_render_set_panorama(MdkRenderer r, const mdk::Panorama* panorama);

// The screen fade: 0 is black, 1 is full brightness. Sticky: the last value
// set applies until it is changed.
void mdk_render_set_fade(MdkRenderer r, float level);

// The gamma correction applied to the final image: 0.5 to 2.0, 1.0 is none.
void mdk_render_set_gamma(MdkRenderer r, float gamma);

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// Draw a mesh. The material carries the pass, so the backend sorts and
// batches; the caller does not.
//
// "Sorts" is not decoration: the call QUEUES the draw, and the backend submits
// the frame's draws bucketed by pass -- every `Solid`, then `DoubleSided`,
// `Mirror`, `Blended`, `OnTop`, `Overlay`, each bucket in queue order -- which
// is what the oracle does with its one command list (Renderer.cs:1000-1004,
// 1049-1057; mdk_core/src/render/draw_queue.h).
//
// WHERE that submission happens belongs to the backend, and the two backends
// differ, so a caller may NOT assume a flush at any seam except
// `mdk_render_end_frame`:
//   * Vulkan flushes at `canvas_begin`, at each inset boundary, and at the frame
//     end (renderer_vk.cpp, `mdk_render_canvas_begin` / `mdk_render_begin_inset`
//     / `mdk_render_end_inset` / `mdk_render_end_frame`).
//   * Citro3D flushes ONCE, inside `mdk_render_end_frame`, once per eye. A
//     mid-frame flush there emits the frame's records an extra time into
//     whichever eye `installEye` selected last -- the previous frame's, or no
//     target at all on frame 1 -- and a `flushDraws`-shaped one (replay + reset)
//     empties the queue on top of it, so the per-eye replay that follows has
//     nothing left to draw: a scene vanished behind a correct clear colour
//     (AGENTS.md P2). The canvas and inset seams on that backend therefore emit
//     nothing, and a HUD or inset batch must put its records IN THE QUEUE, where
//     the per-eye replay draws them once per eye, instead of flushing at its own
//     boundary -- and "emit nothing" covers drawing directly too, which is the
//     half a HUD batch is most likely to step on, since `canvas_quad` is the seam
//     `game/hud_view.cpp` drives. `tests/scripts/c3d_frame_test.sh` refuses a
//     mid-frame flush and an immediate draw at every one of those seams.
void mdk_render_draw_mesh(MdkRenderer r, const mdk::MeshDraw* draw);

// ---------------------------------------------------------------------------
// The 2D canvas
// ---------------------------------------------------------------------------

// Open the canvas batch. Quads are accumulated until mdk_render_canvas_end,
// then uploaded and drawn in one go, in the canvas's orthographic space:
// 360 units high and as wide as the frame's aspect makes it.
void mdk_render_canvas_begin(MdkRenderer r);
void mdk_render_canvas_quad(MdkRenderer r, const mdk::CanvasQuad* quad);
void mdk_render_canvas_end(MdkRenderer r);

// The canvas's width for the current frame's aspect (the C# port's
// Renderer.CanvasWidth). The canvas is always 360 units high; its width is
// 360 * target_width / target_height, so HUD layout follows the frame rather
// than a fixed 640x360 assumption.
float mdk_render_canvas_width(MdkRenderer r);

// THE SCENE WINDOW, this frame only: the canvas rectangle (x, y, w, h in
// canvas units) outside which an opaque canvas picture will cover the scene
// -- the sniper scope's frame around its 600 x 360 view. A backend MAY skip
// the scene's pixels outside it (Citro3D: a scissor on the scene's draws, not
// the canvas's); one that ignores it draws the same picture (Vulkan: a no-op).
// Null, or no call, is no window. Cleared at every frame's begin.
void mdk_render_set_scene_window(MdkRenderer r, const float* rect);

// How many quads the canvas batch can hold before the backend must flush.
// The shared code batches into as many draws as it needs, so a small value
// is correct, not a failure.
int32_t mdk_render_canvas_capacity(MdkRenderer r);

// ---------------------------------------------------------------------------
// Insets
// ---------------------------------------------------------------------------

// A 3D view inside a canvas rectangle (render_types.h Inset; the C#'s
// Renderer.Insets.cs): sniper mode's round cameras and clip (game/sniper_view.h).
// The rectangle is in canvas units. Neither seam flushes: begin RECORDS the inset,
// and the meshes drawn until end go into an Own inset instead of the scene. The
// backend draws the insets under the canvas after the scene and the others after
// the canvas.
//   * Vulkan draws both kinds, each in its viewport and scissor with its depth
//     cleared; not in a stereo frame (refused once).
//   * Citro3D draws an Own inset's meshes the canvas's way (one composed matrix
//     onto the rectangle, no depth, no cull), and a Scene inset as the frame's
//     scene records again through its camera in its viewport, its depth reset by
//     a far-plane quad, in each eye (at most three a frame).
// A zero-area inset draws nothing.
void mdk_render_begin_inset(MdkRenderer r, const mdk::Inset* inset);
void mdk_render_end_inset(MdkRenderer r);

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

// The last frame's draw calls and triangles.
void mdk_render_stats(MdkRenderer r, int32_t* draw_calls, int32_t* triangles);

// The last frame's GPU wait, in milliseconds. The 3DS backend reports 0.
float mdk_render_gpu_wait_ms(MdkRenderer r);

// The captures this renderer was asked for, and the ones that reached the disk.
// `requested` counts the frames that named a `screenshot_path` above; `written`
// counts the pictures that were actually written to it. They differ when the
// file could not be opened or came up short on the way out, when the readback
// was refused, and when the frame it was asked to capture was never submitted.
//
// WHY THIS IS NOT ALREADY COVERED BY THE LOG LINE. Every one of those failures
// already prints something and already returns a bool, and the frame seam threw
// that bool away -- so the run exited 0, the harness upstream found the BMP its
// PREVIOUS run had left at that path, and committed a frame this build never
// drew. A refusal printed at a terminal nobody is reading has not been reported
// to anyone who can act on it; an exit code has.
//
// A frontend that asked for a picture and did not get one has FAILED, whatever
// this says, and it is that rule -- not these numbers -- that makes the answer
// safe. The 3DS backend has no readback of a rendered target at all, so it can
// only ever report a request and never a write.
//
// TWO THINGS THIS IS NOT, both learned the hard way (review-3 F1/F3):
//
//   * `requested` counts frames the BACKEND saw name the path, and a frame that
//     never began never names it on either backend. So "0 of 0 requested" can
//     describe a run that asked for a picture and never got a frame to put it
//     in; it is not evidence that nothing was asked. The frontend knows what it
//     asked for and must not read these numbers as the request.
//   * Reporting a failed capture does not un-write the destination. The Linux
//     frontend therefore clears a regular file already sitting at the path when
//     it commits to the request, before anything that can fail, so that "a file
//     is there" means "THIS run put it there" and a failed capture cannot leave
//     the previous run's frame to be committed in its place.
void mdk_render_capture_stats(MdkRenderer r, int32_t* requested, int32_t* written);

// ---------------------------------------------------------------------------
// The Linux-only enhanced look lives in a SEPARATE header
// (`mdk_linux/src/render/render_enhanced.h`), not here. The 3DS does not
// have it, and putting it in the shared header would either drag Vulkan
// types across the seam or invite a capability #ifdef into the shared code.
// The game's Lighting struct is the whole interface: the shared code
// computes the rig, the backend decides what it can do with it.
// ---------------------------------------------------------------------------
