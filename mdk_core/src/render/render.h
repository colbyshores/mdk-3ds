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
// (material.texture), through the image's rectangle.
//
// Returns -1 when the backend has no compressed textures -- the Linux backend
// keeps the palette indices and answers -1, so its callers draw the index
// texture as before -- or when the bytes do not import.
MdkTextureId mdk_render_create_compressed_texture(MdkRenderer r, int32_t width, int32_t height,
                                                  const uint8_t* t3x, int32_t size);

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

// Create a 256x1 RGBA palette.
MdkPaletteId mdk_render_create_palette(MdkRenderer r, const uint8_t* rgba256);

// Replace a texture's contents in place. Used for the bullet holes, which
// change a texture's indices without changing its size.
void mdk_render_update_texture(MdkRenderer r, MdkTextureId texture,
                                int32_t width, int32_t height, const uint8_t* pixels);

// Give a (texture, palette) pair a whole colour image instead of the
// palette expansion (a mod's replacement: the C# Renderer's Replace). The
// image is `frames` frames of width x height stacked downwards, RGBA8
// premultiplied, any size; the enhanced look samples it with mips. Kept
// until the index texture changes (then the expansion returns) or either
// id is released. The 3DS backend holds no colour textures and ignores it.
void mdk_render_replace_colour(MdkRenderer r, MdkTextureId texture, MdkPaletteId palette,
                               int32_t frames, int32_t width, int32_t height,
                               const uint8_t* rgba);

// Give an index texture a COMPRESSED replacement read from a file: the 3DS HD
// pack's .t3x (ETC1 / ETC1A4, its mip chain prebuilt by tex3ds on the host;
// docs/HD_TEXTURES_3DS_SPEC.md). The backend records the path and makes the
// texture itself when it decides to (the 3DS streams them under a budget, and
// draws the original until the replacement is resident). The Linux backend
// decodes mods through mdk_render_replace_colour and ignores this.
void mdk_render_replace_compressed(MdkRenderer r, MdkTextureId texture, const char* path);

// The same for a CANVAS image (a HUD or menu picture): the HD pack's .t3x for
// it. Separate from mdk_render_replace_compressed because the canvas is drawn
// with the original shading, which a world texture's HD copy never applies to
// (the PC's mods replace a canvas image through its own path, ReplaceImage).
void mdk_render_replace_image_compressed(MdkRenderer r, MdkTextureId texture, const char* path);

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

// How many quads the canvas batch can hold before the backend must flush.
// The shared code batches into as many draws as it needs, so a small value
// is correct, not a failure.
int32_t mdk_render_canvas_capacity(MdkRenderer r);

// ---------------------------------------------------------------------------
// Insets
// ---------------------------------------------------------------------------

// A 3D view inside a canvas rectangle. The rectangle is in the canvas's
// 640x360 space. The 3DS's PICA viewport is a HARDWARE CLIP RECTANGLE, so a
// zero-area inset silently drops its draw; the backend guards against that.
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
