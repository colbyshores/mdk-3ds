#pragma once

// ============================================================================
// render_types.h — the plain-old-data that crosses the renderer seam.
//
// These are the C# port's `readonly record struct`s, ported literally. They
// carry no behaviour and no ownership: a value copied out of this header is
// still valid after the caller's buffers are gone.
//
// NOTHING GPU- OR PLATFORM-SPECIFIC APPEARS HERE. No Vulkan handle, no
// citro3d type, no SDL type. If a type crosses the seam it is because the
// GAME needs it, not because a backend does.
//
// See render.h for the seam itself and docs/DOCTRINE.md for why the seam is a
// flat C-style API rather than an interface.
// ============================================================================

#include <stdint.h>

namespace mdk {

// ---------------------------------------------------------------------------
// Vector / colour basics
// ---------------------------------------------------------------------------

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

struct Colour {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

// The 3DS's 400x240 top screen and the game's canvas are different spaces;
// the canvas scale is what maps one to the other. The canvas is 360 units
// high and its width follows the frame's aspect (mdk_render_canvas_width).
static constexpr float kCanvasHeight = 360.0f;

// ---------------------------------------------------------------------------
// Vertex
// ---------------------------------------------------------------------------

// The vertex the original Direct3D engine and the C# port both use:
// position, texture coordinate, and a packed RGBA8 colour (R in the low
// byte). The colour multiplies the surface; the original's Gouraud-blended
// stream is this vertex's colour.
//
// 20 bytes, tightly packed. The Linux backend uploads it as
// R32G32B32_SFLOAT / R32G32_SFLOAT / R8G8B8A8_UNORM; the 3DS backend expands
// it to its 36-byte all-float form.
struct Vertex {
    Vec3 pos;
    Vec2 uv;
    uint32_t colour;
};

static constexpr uint32_t kVertexWhite = 0xFFFFFFFFu;

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------

// The draw buckets, in the order the original draws them back to front.
enum class Pass {
    Solid,        // opaque, depth-tested and written
    DoubleSided,  // opaque, no culling
    Mirror,       // shows the mirror panorama, no reflection
    Blended,      // alpha-blended, sorted back to front
    OnTop,        // drawn over everything, no depth test
    Overlay,      // the 2D canvas

    // Not a pass. The cardinality sentinel, and the reason "every pass is
    // handled" can be stated as something the compiler checks instead of
    // something a reader counts (review-4 F1: a hand-counted array of the six
    // members could not fail when a seventh arrived). `mdk::kDrawQueuePassCount`
    // is derived from it, so the draw queue's bucket count cannot drift from the
    // enum either. Nothing may treat `Count` as a pass: `DrawQueue::add` would
    // fold such a draw into the `Solid` bucket -- it clamps an out-of-range
    // bucket to 0 rather than dropping the draw, so the draw would appear in the
    // opaque group instead of vanishing (review-5 F3) -- and the two exhaustive
    // switches over `Pass` name it only to stay exhaustive without a `default`.
    //
    // IT MUST STAY THE LAST ENUMERATOR (review-5 F6). Every claim built on it
    // counts from the top of the enum: `kDrawQueuePassCount` IS this value, and
    // the coverage `static_assert`s in `pica_matrix_tests.cpp` compare an array's
    // length with it. A member appended AFTER `Count` moves none of them, and
    // neither build compiles with `-Werror`, so the only signal would be a
    // `-Wswitch` warning nobody fails on. Append new passes BEFORE it.
    Count,
};

enum class Blend {
    Alpha,  // src_alpha, one_minus_src_alpha
    Add,    // src_alpha, one
};

enum class Shading {
    Original,  // the original look: flat, unlit, the palette decides
    Lit,       // the enhanced look: hemisphere + sun + point lights
    Sprite,    // the enhanced look: filtered, unlit, edges cut at half cover
};

enum class Primitive {
    Triangles,
    Lines,
};

// A material is the state a draw needs: which surface, which palette, how to
// blend, how to shade.
//
// `texture` is an index texture (8-bit) or an RGBA texture, or -1 for none.
// `palette` is a 256x1 palette, or -1 for none. In the ORIGINAL look the
// texture is sampled through the palette; index 0 is transparent.
//
// `frame_count` > 1 means the texture holds that many animation frames
// stacked VERTICALLY (the original's animated textures). The frame index
// selects the slice.
//
// `row_shift` is the mirror's vertical offset in rows.
//
// `depth_layer` is the layer index used by the depth-pull trick (see
// DepthPull): coplanar surfaces are pulled towards the eye by a fraction of
// their layer so they never z-fight.
struct Material {
    int32_t texture = -1;
    int32_t palette = -1;
    Colour colour = {1.0f, 1.0f, 1.0f, 1.0f};
    int32_t frame_count = 1;
    Pass pass = Pass::Solid;
    float row_shift = 0.0f;
    Shading shading = Shading::Original;
    Blend blend = Blend::Alpha;
    int32_t depth_layer = 0;
};

// ---------------------------------------------------------------------------
// Panorama (the sky)
// ---------------------------------------------------------------------------

// The sky is a screen-filling panorama scrolled by yaw and pitch. The
// original's backdrop is a 2D image, not a 3D dome, and the mirror surfaces
// show a second one.
struct Panorama {
    int32_t sky = -1;
    int32_t mirror_sky = -1;
    int32_t palette = -1;
    float wrap_width = 0.0f;
    float horizon_row = 0.0f;
    float offset = 0.0f;
    float height = 0.0f;
    uint32_t top_colour = 0xFF000000u;
    uint32_t bottom_colour = 0xFF000000u;
    bool linear = false;   // the enhanced look filters the panorama
};

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

// A right-handed view-projection pair, plus the eye position the point
// lights and the fog need.
struct Camera {
    float view[16] = {0};          // row-major, row-vector convention
    float projection[16] = {0};    // row-major, row-vector convention
    Vec3 position = {0.0f, 0.0f, 0.0f};
    Vec3 forward = {0.0f, 0.0f, 1.0f};
    Vec3 up = {0.0f, 1.0f, 0.0f};
    float near_plane = 1.0f;
    float far_plane = 1000.0f;
};

// ---------------------------------------------------------------------------
// The lighting rig — SHARED DATA, not a shared feature.
//
// The game computes this exactly as the C# port does (Level/LevelLight.cs,
// Objects/LightSources.cs). The Vulkan backend consumes it and produces the
// enhanced look. The Citro3D backend draws the ORIGINAL look and takes from
// the rig only what its PICA200 can express behind each Enhanced Mode toggle
// (SPEC.md 7.9; so far the haze colour and density).
//
// This is the whole graceful-degradation mechanism: the game code is
// identical on both targets, and the CAPABILITY differs. There is no #ifdef
// and no branch in the game code.
// ---------------------------------------------------------------------------

struct PointLight {
    Vec3 position = {0.0f, 0.0f, 0.0f};
    Colour colour = {1.0f, 1.0f, 1.0f, 1.0f};
    float radius = 0.0f;
};

struct Lighting {
    bool enabled = false;              // the enhanced look is on
    Vec3 sun_direction = {0.0f, 1.0f, 0.0f};
    Colour sun = {1.0f, 1.0f, 1.0f, 1.0f};
    Colour sky = {0.5f, 0.5f, 0.6f, 1.0f};
    Colour ground = {0.2f, 0.2f, 0.2f, 1.0f};
    float exposure = 1.0f;
    bool shadows = false;
    float shadow_distance = 0.0f;
    float glow = 0.0f;
    Colour haze_colour = {0.0f, 0.0f, 0.0f, 1.0f};
    float haze_density = 0.0f;
};

// ---------------------------------------------------------------------------
// The Enhanced Mode feature toggles (SPEC.md 7.9, owner directive
// 2026-11-14). One boolean per potentially expensive GPU operation, so
// each can be A/B-compared live on hardware. Every enhanced feature
// executes strictly on the GPU (the PICA200 on the 3DS, the Vulkan
// device on Linux); there is no CPU fallback path for any of them, and
// disabling a toggle skips the feature's GPU pass at setup time -- the
// check is one boolean read per pass, never per-vertex or per-pixel
// work on the CPU. All default on: the shipped look is the full
// enhanced look, and a missing config file means these values.
// ---------------------------------------------------------------------------
struct EnhancedToggles {
    bool enableSkyShadows = true;     // the sun shadow map pass
    bool enableFragmentLights = true; // fragment point lights (LUTs on the PICA200)
    bool enableHaze = true;           // atmospheric haze / fog blend
    bool enableMirrors = true;        // planar mirror draws
    bool enableGlowBillboards = true; // additive glow (post glow / glow billboards)
};

// ---------------------------------------------------------------------------
// Draw packets
// ---------------------------------------------------------------------------

// One mesh draw. `world` is a row-major, row-vector 4x4; the identity means
// "already in world space".
struct MeshDraw {
    int32_t mesh = -1;
    int32_t first_vertex = 0;
    int32_t vertex_count = 0;
    Material material;
    int32_t frame = 0;          // the animated texture's frame index
    float world[16] = {0};
    Primitive primitive = Primitive::Triangles;
};

// A sprite is a mesh drawn with the sprite shading (the enhanced look's
// filtered, unlit, edges cut at half cover). It shares MeshDraw's shape; the
// distinction lives in the material's `shading`.
using SpriteDraw = MeshDraw;

// ---------------------------------------------------------------------------
// The 2D canvas
// ---------------------------------------------------------------------------

// A canvas quad. The canvas is a 640x360 orthographic space; the backend
// scales it to whatever the target is. `u0/v0`..`u2/v2` address the source
// texture; `texture` is an index texture (through `palette`) or an RGBA
// texture (palette < 0).
struct CanvasQuad {
    float x0 = 0.0f, y0 = 0.0f;
    float x1 = 0.0f, y1 = 0.0f;
    float x2 = 0.0f, y2 = 0.0f;
    float x3 = 0.0f, y3 = 0.0f;
    float u0 = 0.0f, v0 = 0.0f;
    float u1 = 0.0f, v1 = 0.0f;
    float u2 = 0.0f, v2 = 0.0f;
    float u3 = 0.0f, v3 = 0.0f;
    Colour colour = {1.0f, 1.0f, 1.0f, 1.0f};
    int32_t texture = -1;
    int32_t palette = -1;
    int32_t frame_count = 1;
    int32_t frame = 0;
    bool additive = false;
};

// ---------------------------------------------------------------------------
// Insets
// ---------------------------------------------------------------------------

// A 3D view drawn inside a canvas rectangle: the sniper mode's round
// cameras. The rectangle is in the canvas's 640x360 space.
struct Inset {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    bool under_canvas = true;    // drawn before the canvas, or after
    Camera camera;
};

// ---------------------------------------------------------------------------
// Stereo
// ---------------------------------------------------------------------------

// MDK's stereo modes, from the C# port's `Stereo` enum. The 3DS uses
// hardware stereoscopic instead and answers false for `flat_stereo`.
enum class StereoMode {
    Off,
    Sbs,            // half side by side
    SbsFull,        // full side by side, each half with its own aspect
    CrossView,      // full side by side, halves exchanged
    Interlaced,     // row-interlaced, even rows are the left eye
    InterlacedReversed,
    Tab,            // top and bottom
    TabReversed,
    Vr,             // phone VR viewer: lens-corrected, the HUD stays centred
};

struct StereoSettings {
    StereoMode mode = StereoMode::Off;
    float separation = 0.25f;    // the eyes' distance apart, in the game's units
    float convergence = 10.0f;   // where the images meet; <= 0 means infinity
};

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------

// What the active backend can do. The shared game code asks before it uses
// an optional feature, so that the SAME game code produces the same picture
// on both targets, with the weaker target simply missing the feature.
//
// This is the graceful-degradation policy the Obsidian note
// `compile-richer-fixed-function-onto-weaker-with-graceful-degradation`
// describes: the richer backend gets the richer features; the weaker backend
// gets the same picture with the feature missing — never a different picture.
//
// NOTE: nothing here is a #ifdef. The capability is a runtime value so that
// the shared code has no target-dependent branches at all.
struct MdkCapabilities {
    bool enhanced_look = false;   // shadows, AO, glow, haze, tonemap, filtered textures
    bool msaa = false;            // 2x / 4x multisampling
    bool colour_mips = false;     // the enhanced look's expanded RGBA8 + mip chain
    bool hardware_stereo = false; // the 3DS's parallax barrier
    bool flat_stereo = false;     // the SBS / interlaced / TAB composites
    bool insets = false;          // viewport+scissor sub-rect 3D views
    bool canvas_filtering = false;// bilinear palette lookups in the canvas
    bool panorama_linear = false; // the filtered panorama
    int32_t max_texture_size = 1024;
    int32_t max_canvas_quads = 256;

    // The GPU memory, in bytes, that a caller-side sprite cache should keep
    // resident.
    //
    // The C# uploads every frame of every sprite animation at level load
    // (KurtSprite.Preload) and keeps them for ever: its `_textures` cache is
    // keyed by (name, frame) and is never evicted. The desktop can afford
    // that. The 3DS cannot: the same set is 49.9 MiB of palette-expanded
    // RGBA8 padded to powers of two (measured over TRAVSPRT.BNI: 19
    // animations, 360 frames) against a machine whose whole texture heap is
    // megabytes.
    //
    // So the weaker target names its budget and the shared code applies it:
    // the frames are uploaded when they are first drawn, and only as many as
    // this budget are kept. 0 means "no bound", which is what the desktop
    // answers -- the C#'s own behaviour.
    int32_t sprite_residency_bytes = 0;
};

} // namespace mdk
