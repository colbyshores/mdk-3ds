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
    int32_t hidden_width = 1280; // the offscreen size for the tests
    int32_t hidden_height = 960;
};

// The frame's parameters. Set once per frame, before the first draw.
struct MdkFrameParams {
    float elapsed = 0.0f;         // the frame's delta time, in seconds
    uint32_t frame_index = 0;
    uint32_t clear_colour = 0xFF000000u;
    bool present = true;          // false for a frame the frontend does not show
    const char* screenshot_path = nullptr;   // Linux only; nullptr elsewhere
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Create the active backend's renderer. Returns nullptr on failure.
MdkRenderer mdk_render_create(const MdkRenderDesc* desc);

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

// Create a 256x1 RGBA palette.
MdkPaletteId mdk_render_create_palette(MdkRenderer r, const uint8_t* rgba256);

// Replace a texture's contents in place. Used for the bullet holes, which
// change a texture's indices without changing its size.
void mdk_render_update_texture(MdkRenderer r, MdkTextureId texture,
                                int32_t width, int32_t height, const uint8_t* pixels);

// Release one resource. The id is invalid afterwards.
void mdk_render_release_texture(MdkRenderer r, MdkTextureId texture);
void mdk_render_release_palette(MdkRenderer r, MdkPaletteId palette);

// Create a static mesh. The vertices are copied; the caller keeps its own.
MdkMeshId mdk_render_create_mesh(MdkRenderer r, const mdk::Vertex* vertices, int32_t count);

// Create a mesh whose contents are rewritten every frame. The backend
// allocates the storage once; mdk_render_update_mesh writes into it.
MdkMeshId mdk_render_create_dynamic_mesh(MdkRenderer r, int32_t capacity);

// Rewrite a dynamic mesh's vertices. `count` may be less than the capacity.
void mdk_render_update_mesh(MdkRenderer r, MdkMeshId mesh, const mdk::Vertex* vertices,
                            int32_t count);

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

// The lighting rig. SHARED DATA: the 3DS backend ignores it and draws the
// original look. See render_types.h.
void mdk_render_set_lighting(MdkRenderer r, const mdk::Lighting* lighting);

// The frame's point lights (muzzle flashes, explosions, fires). The Vulkan
// backend uploads them; the 3DS backend ignores them.
void mdk_render_set_point_lights(MdkRenderer r, const mdk::PointLight* lights, int32_t count);

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

// ---------------------------------------------------------------------------
// The Linux-only enhanced look lives in a SEPARATE header
// (`mdk_linux/src/render/render_enhanced.h`), not here. The 3DS does not
// have it, and putting it in the shared header would either drag Vulkan
// types across the seam or invite a capability #ifdef into the shared code.
// The game's Lighting struct is the whole interface: the shared code
// computes the rig, the backend decides what it can do with it.
// ---------------------------------------------------------------------------
