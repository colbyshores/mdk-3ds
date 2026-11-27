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

// A GLOW BILLBOARD: the 3DS's stand-in for the PC's bloom (Enhanced Mode
// enableGlowBillboards; owner decision 2026-11-18). A bright effect sprite's
// halo -- a camera-facing quad in world space, two triangles, uv 0..1 across it
// -- and the colour it adds. The game builds them (EffectsView) from the sprite
// effects whose textures the PC's bloom would catch (texels over its 0.8
// threshold); the 3DS draws each as one additive soft quad. The PC ignores them:
// its bloom finds the bright pixels itself.
struct GlowQuad {
    Vertex corners[6];
    float colour[3];
};

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

// What the scene is drawn over (C# Mdk.Engine.Render.Backdrop, Renderer.cs:78):
// the sky panorama, a clear to the frame's clear colour, or the last frame kept
// (no colour clear). The scripts choose it through the sky mode (opcode 202,
// game/sky_modes.h); the frontends pass it in MdkFrameParams.
enum class Backdrop { Sky, Clear, Keep };

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
    bool enableFragmentLights = true; // fragment point lights (the PC's point lights only)
    bool enableHaze = true;           // atmospheric haze / fog blend
    bool enableMirrors = true;        // planar mirror draws
    bool enableGlowBillboards = true; // additive glow (post glow / glow billboards)
    bool enableFiltering = true;      // bilinear texture filtering (the PC's filtered look)
    bool enableMipmaps = true;        // 3DS: mip chains (made at texture creation, sampled live,
                                      // only with enableFiltering); the PC always mips
    bool enableHdTextures = true;     // 3DS: the HD pack's ETC1/ETC1A4 replacements,
                                      // streamed (docs/HD_TEXTURES_3DS_SPEC.md)
    bool enableLighting = true;       // 3DS: the sky/ground/sun light per triangle (the lit
                                      // program's geometry shader); the PC always lights
    bool enableEdgeSmoothing = true;  // 3DS: supersampled eye targets that the display
                                      // transfer averages down (the PC's MSAA, by the PICA's
                                      // own downscale); the PC always multisamples
    bool edgeSmoothing2x2 = false;    // 3DS: 2x2 (480x800) instead of 2x1 (480x400) --
                                      // ~22 ms on the Old 3DS at 2x2; the PC ignores it
    bool glowBloom = false;           // 3DS: the glow as the PC's bloom, ported (~10 ms on
                                      // the Old 3DS), instead of the default additive
                                      // billboards on bright effects; the PC ignores it
    bool texture16 = true;            // 3DS: the palette-expanded textures stored 16 bits a
                                      // texel (RGB565 / RGBA5551 / RGBA4, chosen per texture
                                      // by pica::expandedTexelFormat) instead of RGBA8 --
                                      // half the texture bandwidth and memory. Decided when a
                                      // texture is MADE: a change takes effect at the next
                                      // level load (or an evicted texture made again). The
                                      // PC ignores it
    bool kurtCompressed = true;       // 3DS: Kurt's sprite frames drawn from the ETC1A4 set
                                      // made offline from the player's TRAVSPRT.BNI
                                      // (kurt_etc1_convert.py, romfs:/kurt/kurt.pak):
                                      // 1 byte a texel, never expanded on the ARM11, and the
                                      // start set loaded at level load. Off (or a frame with
                                      // no compressed copy): the palette expansion, in the
                                      // texture16 store. Read at level load. The PC ignores it

    // NOT A LOOK: a diagnostic of the 3DS sun shadow, off by default. The caster
    // pass writes the shadow map's four bytes in the order the GPU's shadow unit
    // reads them (pica_matrix.h, ShadowByteOrder): measured in the emulator and,
    // by the format's documentation, the console's too -- but no console has
    // run it. If the sun's shadow is wrong on hardware in the way a wrong order
    // is wrong -- everything the map covers half dark, or nothing shadowed at
    // all -- this swaps the order without a rebuild. The PC ignores it.
    bool shadowMapFlipBytes = false;

    // NOT A TOGGLE: the 3DS sun shadow map's side, for the owner's cost A/B.
    // 0 = automatic (512 texels, else 256, else 128 as VRAM allows -- every
    // enhancement on at edge smoothing 2x gives 256); 128, 256 or 512 = that
    // size, or the next smaller one VRAM allows, down to 128. Anything else reads as 0. The
    // self-shadow guards follow the size (pica::sunShadowNormalOffset). The PC
    // ignores it (its map is 2048).
    int32_t shadowMapSize = 0;

    // NOT A TOGGLE: how often the 3DS redraws the sun shadow's map, in frames
    // (the map is the same picture while the sun and the static world stand
    // still; only moving casters lag). 0 = automatic (about 15 Hz: every 4th
    // frame at 60 fps, every 2nd at 30 -- pica::shadowRefreshDue); 1-4 =
    // every that many frames. It is redrawn at once whatever this says when the
    // camera has moved its box on, the sun has turned or the map is new. The PC
    // ignores it.
    int32_t shadowMapRefresh = 0;

    // THE LOOK DIALS (the pause menu's sliders; the owner's t2k-style dial-in):
    // each scales, in PERCENT of today's value, a number the 3DS renderer already
    // uploads every frame, so 100 -- the default -- is the shipped look to the
    // byte (x * 1.0f is x). Ranges and steps: enhanced_config.h kLookDials. The
    // PC ignores them.
    int32_t hazeDensity = 100;     // the rig's haze density (level_light kHazeDensity)
                                   // before the vertex shader's exponent
    int32_t glowStrength = 100;    // the glow billboards' halo colour (the tint
                                   // uniform; the bloom's fitted curve is not scaled)
    int32_t shadowDarkness = 100;  // how much of the sun's shadow lands: the shadow
                                   // tint's alpha (the receive combiner's stage 0)
    int32_t exposure = 100;        // the rig's exposure (lit and shadow programs,
                                   // the shadow tint)
    int32_t tintSaturation = 100;  // the rig's sun / sky / ground saturation about
                                   // their luminance (level_light's k*Saturation)

    // 3DS: present every second vblank (citro3d's own frame pacing,
    // C3D_FrameRate(30)) instead of every one. The simulation still runs the
    // fixed steps the wall clock owes, so the game does not slow down. Off by
    // default. The PC ignores it.
    bool cap30Fps = false;

    // 3DS: the movies (MDK12.FLC behind the main menu, the end movies) upload
    // every second decoded frame instead of every one (package #20c). Each upload
    // is a whole 600 x 360 picture through its palette on the ARM11
    // (pica::streamTexelsRgb565, ~10 ms on the Old 3DS by the host estimate); the
    // decode keeps the C#'s 34 ms rate either way, so the movie keeps its length.
    // Off by default (the C#'s rate). The PC ignores it.
    bool movieHalfRate = false;

    // 3DS: a debris piece (or spark) whose bounding sphere covers this many
    // screen pixels or fewer across is not drawn (projectedPixelSize at the
    // frame's camera and zoom, on the top screen's 400 x 240). 0 = off; 1, 2
    // or 3 pixels; anything else reads as 2. Drawing only: the pieces still
    // fly, bounce and expire as before. The PC ignores it.
    int32_t tinyDebrisPixels = 2;

    // 3DS: an enemy (the original's GameStats.IsEnemy types, less its
    // missiles and bombs, never the boss the health bar is showing) whose
    // bounding sphere covers this many screen pixels or fewer across is not
    // drawn, and drawn again once it covers more than 1.25 times as many (2 px:
    // hidden at 2, shown above 2.5), so one at the edge does not flicker. Same
    // measure as tinyDebrisPixels: the frame's camera and zoom, 400 x 240, one
    // decision for both eyes. 0 = off; 1, 2 or 3; anything else reads as 2.
    // Drawing only: its script, motion, aim and hits run as before. The PC
    // ignores it.
    int32_t tinyEnemyPixels = 2;

    // 3DS: the sniper scope drawn in 2D (one eye, the parallax barrier off)
    // whatever the 3D slider says, while the scope is up: half the scene's
    // draws and fill in the scope. Off by default, because it changes the
    // look (the scope loses its depth). The PC ignores it.
    bool scopeMono = false;

    // 3DS: while the sniper scope's opaque frame is up, the scene is drawn
    // only inside the frame's 600 x 360 window (a GPU scissor; the frame
    // covers everything outside it), so the fill the frame hides is not
    // paid. The picture is the same. On by default. The PC ignores it.
    bool scopeClip = true;

    // 3DS: the sniper's round cameras (the three windows at the top of the scope)
    // show the round's chase view, as the C#'s: the scene again through each
    // watching camera, in both eyes. Off, a watching window is an empty fill.
    // Measured on the Old 3DS (level 3, stereo): ~44 ms a frame while a camera
    // watches, against ~33.5 with it off. On by default. The PC ignores it.
    bool sniperRoundCams = true;
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
    // The draw's world-space bounding box, for the backend's per-eye frustum
    // cull. Valid ONLY when `world` is the identity and `cullable` is true
    // (the level's batches); every other draw leaves them and is never culled.
    float bounds_min[3] = {0.0f, 0.0f, 0.0f};
    float bounds_max[3] = {0.0f, 0.0f, 0.0f};
    bool cullable = false;
    // THE CAMERA'S ARENA OVER THE OTHER ONE (upstream e1eb36bb; the original's
    // 0x41df35). The original has no depth buffer: it draws the other drawn arena
    // first and the camera's arena over it, so where their geometry overlaps on
    // screen the camera's always shows (LEVEL4: MEAT_3's flat shell lies 2.3
    // units above CMEAT_1's floor and, depth-tested, swallowed the tube and
    // Kurt's legs). Set only while that can happen (game/arena_views.cpp): `back`
    // on the other arena's batches and objects, `cover` on the camera arena's
    // batches they may hide. The backends then draw the camera's arena over the
    // other one wherever its opaque cover batches are drawn (render/draw_queue.h),
    // at full depth precision: the bands upstream split the depth range into are
    // not used. With neither set the frame is drawn exactly as before.
    bool back = false;    // the other drawn arena's batch or object
    bool cover = false;   // the camera's arena's batch, which covers it
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

// A 3D view drawn inside a canvas rectangle (the C#'s Renderer.Insets): sniper
// mode's round cameras and its clip of loaded rounds (game/sniper_view.h). The
// rectangle is in the canvas's units (360 high, render.h).
//
// `scene` is the C#'s InsetContent: true (Scene) draws the frame's whole scene
// again through `camera` -- every draw of the frame, whenever it was queued --
// and takes no draws of its own; false (Own) draws only the meshes drawn between
// mdk_render_begin_inset and mdk_render_end_inset, which then do not go into the
// scene. The rectangle's depth is cleared first either way.
struct Inset {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    bool under_canvas = true;    // drawn before the canvas, or after
    bool scene = true;           // InsetContent.Scene, or Own
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
    // Whether a scene draw multiplies by the vertex's colour (Vertex::colour). The
    // Vulkan backend does (palette.frag); the 3DS's PICA vertex is position + UV
    // and its scene combiner takes the texture alone, so a caller that needs a
    // Gouraud colour there draws it from a texture instead (stream/stream_view.h,
    // the tube). True by default: the C#'s renderer has them.
    bool vertex_colours = true;
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

    // The GPU bytes Kurt's START SET (K_STILL, K_FALL, K_LAND, K_IDLE: the
    // frames every level opens on; the chute's K_CHUTE, K_CHUTEC; the jump's
    // K_JUMP, K_RJMP) may take when it is made at LEVEL LOAD from
    // compressed frames (EnhancedToggles::kurtCompressed) and kept for the level,
    // outside sprite_residency_bytes' rotation. Only compressed frames are
    // preloaded: an import is a copy, an expansion is the ARM11 work the
    // preload exists to keep out of the frame. 0: no preload (the desktop, which
    // has no compressed frames and no bound).
    int32_t sprite_preload_bytes = 0;
};

} // namespace mdk
