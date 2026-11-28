# SPEC — MDK C++ port: C with classes, Vulkan (Linux) + Citro3D (3DS)

Status: **v1 — 1:1 reimplementation.** Authored 2026-10-21.
Target: the Nintendo 3DS (PICA200) and Linux/x86-64 (Vulkan 1.3), from the
existing C# port: upstream 1.1.1 (`d5bb66ec`, in this repository's history; the
behavioural oracle, no longer in the tree).

This document is the contract. `AGENTS.md` is the *performance and platform*
contract that rides alongside it and is enforced by `tools/`.

---

## 1. Scope

### 1.1 Goals

| # | Goal | Measure |
|---|---|---|
| G1 | Port the C# game to C++ in the t2k "C with classes" style | 0 virtuals, 0 RTTI, 0 exceptions, 0 project templates; gated by `tools/runner/audit_c_with_classes.py` |
| G2 | Two renderer backends behind one build-time seam | `mdk_linux` (Vulkan 1.3) and `mdk_3ds` (Citro3D) both link the same `mdk_core` |
| G3 | 1:1 gameplay fidelity | Golden-frame and golden-state comparison against the C# port; identical BSP sweeps, script VM, and audio laws |
| G4 | Performance contract inherited from t2k | `tools/runner/gates_vfp.py` green; hot-path corpus derived from a real capture |
| G5 | Shippable 3DS artifacts | `mdk.3dsx` and `mdk.cia`, with the GOG MDK data bundled, running in mandarine |
| G6 | Checked completion | Phases P0–P8 are worked to green increment by increment, each one tested, gated and independently reviewed before it is committed (`AGENTS.md` §0) |

### 1.2 Non-goals (v1)

- **Not** a re-implementation of the *enhanced look* on 3DS. The 3DS ships the
  **original look** (pixel-faithful to `MDKD3D.EXE`). The enhanced look is
  Linux-only. See §5.3.
- **Not** OpenXR/VR on Linux beyond MDK's own `vr` stereo mode (the C# port's
  phone-VR composite). t2k's OpenXR path is not ported.
- **Not** a re-write of the MDK data formats. They are ported 1:1 from
  `src/Mdk.Formats` so the C# and C++ read byte-identical structures.
- **Not** the 1996 beta demo's *content*; the dialect support is ported (the
  `ScriptDecoder.Beta` path), the demo's own data files are not shipped.
- **Not** Real-ESRGAN HD texture generation on 3DS. The mod system is Linux-only.
- **Not** Android. SDL3's Android head does not carry over to a 3DS-oriented
  C++ core without a third platform tree; out of scope.

### 1.3 The oracle

The C# port is the reference implementation that decides every "is this
right?" question. It was removed from the tree on 2026-11-27: it is upstream
`nautilusbits/mdk-sdl` at 1.1.1 (`d5bb66ec`, also in this repository's history),
and a later upstream fix the C++ ports is in upstream's history.

- Golden-state tests ran the same synthetic inputs through the C# `Mdk.Game`
  assembly (the probes, in history at `92a72116`) and pin its serialized state;
  the C++ `mdk_core` is compared against those pins.
- Golden-frame tests compare rendered RGBA8 output (Linux/Vulkan only) within a
  per-pixel tolerance; the 3DS is compared by screenshot against the same
  golden set, with a looser tolerance and a manual A/B checklist.

---

## 2. Reference material ("rosetta stones")

Ordered by how much they constrain this design.

| Source | What it fixes |
|---|---|
| `../t2k/AGENTS.md` | The performance + platform contract: hot-path rules R1–R11, T1/T2, exemptions, the C-with-classes language rules (§10), 3DS platform rules P1–P6, CIA identity rules (§9). Adapted wholesale — see `AGENTS.md` in this repo. |
| `../t2k/t2k_core/src/rendering/render.h` | The shape of the renderer seam: flat C-style free functions over an opaque `void*` handle, selected at build time by which tree is compiled. |
| `../t2k/t2k_pc/` | Vulkan backend structure: `vk_context` / `vk_resources` / `vk_pipeline` / `vk_scene` / `vk_post`, glslang→SPIR-V→`spv2h.py`, vendored Vulkan headers + volk. |
| `../t2k/t2k_3ds/` | 3DS tree structure: devkitARM Makefile, `picasso`→`bin2s` shader embedding, `C3D_*` state, FZ/DN FPSCR helpers, `C3D_FrameBegin/End` ordering, SMDH/CIA packaging. |
| `../forsaken/3DS_PORT_NOTES.md` | The **D3D→PICA200** conversion rules, which MDK needs identically (MDK is a Direct3D game): the depth-range remap, the cull-enum inversion, the "you do NOT need to negate View.z" list, the hard-float ABI trap, the ARM unaligned-access list, the colourkey-before-gamma bug. |
| `../forsaken/render.h` + `render_c3d.c` | A working D3D-derived fixed-function seam with a native Citro3D implementation; the `gpu_vertex_t` conversion, the 4 MB scratch, the ping-pong-free buffer strategy, `C3D_AttrInfo`/`TexEnv`/`DepthMap` initialisation, the portrait viewport mapping. |
| Obsidian `GX-to-PICA200/*` | PICA200 hardware model, the TEV→combiner story, the pixel-pipeline compare tables, the stereo/EFB mapping, the porting playbook's phase structure. |
| Obsidian `10-projects/forsaken/findings/citro3d-*.md` | The citro3d renderer architecture note, the rosetta-stone hub (the differential-debugging oracle idea), the display-list stereo replay pattern. |
| Obsidian `30-architecture/patterns/*` | `port-mvp-transform-locus-cpu-vs-gpu` (where the transform lives), `compile-richer-fixed-function-onto-weaker-with-graceful-degradation` (the enhanced→original degradation pattern), `pica200-alpha-zero-opaque-blend-quirk`, `invisible-3d-trap-zero-viewport`, `d3d-vertex-color-alpha-opaque-pass-only-assumption`. |
| `src/Mdk.Formats/` | The data formats. Ported 1:1. |
| `src/Mdk.Game/`, `src/Mdk.Engine/` | The game and the engine. Ported 1:1, with the renderer's *interface* re-cut per §5. |

**Cognee** was queried for Citro3D material; its recall endpoint was
unavailable during authoring (repeated `Error: Recall failed:`). The Obsidian
vault carries the same material and was used instead. If Cognee comes back, its
findings are additive, not corrective.

---

## 3. Repository layout

Mirrors t2k's three-tree shape; the C# is upstream 1.1.1 (`d5bb66ec`), not in the tree.

```
mdk-sdl/
├── SPEC.md                     ← this file
├── AGENTS.md                   ← performance + platform contract (gated)
├── docs/
│   ├── DOCTRINE.md             ← design doctrine, fidelity ladder
│   ├── formats.md              ← ported from src/Mdk.Formats docs
│   ├── original-engine.md
│   └── validation/             ← perf-*.log captures feed the hot-path corpus
├── mdk_core/                   ← SHARED: compiled into both targets
│   ├── src/
│   │   ├── formats/            ← DTI MTO MTI SNI FTI BNI Arena Texture Palette
│   │   │                         Model ModelAnimation SpriteAnimation Png Gif
│   │   │                         Glb Wav Flc Mve Font SniperScreen Fall3d
│   │   │                         BetaDemo BinReader CaseInsensitivePath
│   │   │                         LocalPaths MdkData
│   │   ├── scripts/            ← ScriptDecoder ScriptVm ScriptRuntime
│   │   │                         ObjectMotion Items ObjectBehaviors Effects
│   │   │                         Debris SniperRounds Snowboard Rides Fans
│   │   │                         AirStrike Bomber Twister StrikeScene Ribbon
│   │   │                         BulletHoles Snapshot ScriptMath
│   │   ├── game/               ← Kurt (+partials) FollowCamera Scope CameraRoll
│   │   │                         Inventory Collision/Bsp ArenaSpace Solids
│   │   │                         Level/ LevelView MaterialResolver
│   │   │                         TriangleGroups Layers Outlines ArenaShape
│   │   │                         EnhancedLook SkyLight LevelLight SkyModes
│   │   │                         ArenaStops FrameBlend CutsceneCamera
│   │   │                         Flow/ Game GameState Settings SaveGames
│   │   │                         Menu/ Stream/ Fall/ Hud/ Objects/ Mods/
│   │   │                         DevTools/ HdTextures/
│   │   ├── render/              ← render.h (the seam) + shared value types
│   │   │                         render_types.h render_capabilities.h
│   │   ├── audio/               ← AudioDevice Limiter SoundMixer LevelMusic
│   │   │                         SoundBank
│   │   ├── platform/            ← Input (the game's own key enum), GamepadMap
│   │   │                         HeadLook TouchControls
│   │   ├── util/                ← math (Vec3, Matrix4x4), BinReader helpers,
│   │   │                         LogRing Profiler Json (save files)
│   │   └── app/                 ← nothing target-specific
│   ├── third_party/             ← json (nlohmann or a small writer), nothing else
│   └── tools/                   ← bin2h.py spv2h.py
├── mdk_linux/                  ← Linux: Vulkan 1.3 + SDL3
│   ├── src/
│   │   ├── main.cpp
│   │   ├── render/
│   │   │   ├── renderer_vk.cpp   ← the seam implementation
│   │   │   ├── vk/vk_context.cpp vk_resources.cpp vk_pipeline.cpp
│   │   │   ├── vk_scene.cpp vk_canvas.cpp vk_post.cpp vk_stereo.cpp
│   │   │   └── vk_shaders.h
│   │   ├── platform/
│   │   │   ├── window_sdl.cpp input_sdl.cpp display_sdl.cpp
│   │   │   ├── gamepads_sdl.cpp touch_sdl.cpp http_curl.cpp
│   │   │   └── frame_limiter.cpp
│   │   └── audio/audio_sdl.cpp
│   ├── shaders/                 ← GLSL (.vert/.frag), glslang → SPIR-V
│   ├── third_party/vulkan/include  ← vendored Vulkan-Headers (from t2k)
│   ├── third_party/volk/           ← vendored volk (from t2k)
│   └── CMakeLists.txt
├── mdk_3ds/                    ← 3DS: Citro3D + libctru
│   ├── src/
│   │   ├── main_3ds.cpp
│   │   ├── render/
│   │   │   ├── renderer_c3d.cpp  ← the seam implementation
│   │   │   ├── c3d/01_constants.inc … (one TU, split by .inc)
│   │   │   ├── text_c3d.cpp
│   │   │   └── c3d_palette.cpp
│   │   ├── platform/
│   │   │   ├── input_3ds.cpp touch_3ds.cpp save_3ds.cpp
│   │   │   └── vfp_fpscr.h      ← R1 FZ/DN helpers
│   │   └── audio/audio_3ds.cpp
│   ├── shaders/                 ← .v.pica / .f.pica (picasso)
│   ├── cia/                     ← icon.png icon_small.png banner + rsf
│   ├── romfs/                   ← the MDK data tree
│   └── Makefile
├── tools/
│   ├── check.sh                 ← the gate suite
│   └── runner/
│       ├── gates_vfp.py
│       ├── audit_arm11_math.py
│       ├── audit_c_with_classes.py
│       ├── audit_r11_idiv.py
│       └── contracts/{vfp_hotpath.md, hotpath_corpus.json}
├── tests/
│   ├── formats/                 ← C++ unit tests (Catch2-free, hand-rolled)
│   ├── golden/                  ← C#-vs-C++ state + frame comparison
│   └── shell/                   ← console_test.sh, alloc_test.sh equivalents
```

**Rules.**
- Basenames must be unique across `mdk_core/src` and each platform tree: the
  3DS Makefile keys objects by `notdir`.
- `mdk_core/src` may contain **no** `#ifdef` for the target. The only sanctioned
  one is the OS-thread seam `mdk_core/src/util/thread.h` (mirrors t2k's
  `audio/audio_thread.h`).
- Platform trees include `mdk_core` headers root-relatively (`#include
  "game/kurt.h"`), never with `../`.

---

## 4. Language contract — "C with classes"

Verbatim intent from t2k `AGENTS.md` §10, restated here as binding on this tree.

**Banned outright** (gated by `audit_c_with_classes.py`, 0 in the tree today):

- `virtual`, `override`, `final` on member functions; inheritance that depends
  on virtual dispatch.
- RTTI: `dynamic_cast`, `typeid`, `<typeinfo>`.
- Exceptions: `throw`, `try`, `catch`, `<exception>`. Both builds compile
  `-fno-exceptions -fno-rtti`.
- Project-defined `template`.
- `std::function`, `std::bind`, `<functional>`.
- `assert()` / `<cassert>` — use `static_assert` for compile-time invariants and
  explicit return codes for runtime ones.

**Allowed and encouraged:** plain `struct` with public data and non-virtual
member functions, constructors, destructors, RAII, namespaces, `enum class`,
fixed-capacity arrays, `static_assert` layout checks, and local lambdas.

**The seam is chosen at build time, not run time.** The renderer backend is
selected by *which tree is compiled* (`renderer_vk.cpp` vs `renderer_c3d.cpp`),
never by `#ifdef` inside shared code and never by a vtable. This is the whole
architectural reason virtuals are banned here.

**Error handling:** return codes and status structs. `nullptr` on failure.
No exceptions cross the seam, ever.

**Memory:** whoever allocates owns it; the owner is named in the struct; freeing
is explicit and paired. Scratch buffers are borrowed and documented as
caller-owned.

**Naming:** `PascalCase` types, `snake_case` free functions, `SCREAMING_SNAKE`
constants, `enum class` values `PascalCase`, trailing underscore for private
members. The seam's free functions use the `mdk_render_` prefix; shared data
lives in `namespace mdk`.

**Include style:** `#pragma once` everywhere. C++ headers `<cstdint>`, `<cmath>`,
`<cstring>`, `<cstdio>`, `<cstdlib>`, `<vector>`, `<string>`, `<array>` only
where justified. No `<algorithm>` in a per-frame path.

---

## 5. Core architecture

### 5.1 The renderer seam

`mdk_core/src/render/render.h` is the **only** rendering header shared code may
include. It is a flat C-style API over an opaque handle. No GPU type crosses
it.

```cpp
#pragma once
namespace mdk { struct GameEngine; struct GameState; }

// Opaque renderer handle. Callers never dereference it; each backend casts it
// to its own concrete type internally.
typedef void* MdkRenderer;

// ---- lifecycle -------------------------------------------------------------
MdkRenderer mdk_render_create(const MdkRenderDesc* desc);
void        mdk_render_destroy(MdkRenderer r);
const char* mdk_render_backend_name(MdkRenderer r);

// ---- frame ----------------------------------------------------------------
void mdk_render_begin_frame(MdkRenderer r, const MdkFrameParams* p);
void mdk_render_end_frame(MdkRenderer r);
void mdk_render_present(MdkRenderer r);

// ---- camera / lighting rig (shared DATA; the backend decides the shading) --
void mdk_render_set_camera(MdkRenderer r, const MdkCamera* cam);
void mdk_render_set_lighting(MdkRenderer r, const MdkLighting* light);
void mdk_render_set_point_lights(MdkRenderer r, const MdkPointLight* lights, int count);
void mdk_render_set_fade(MdkRenderer r, float level);   // 0 = black .. 1 = full

// ---- 3D submission --------------------------------------------------------
void mdk_render_draw_mesh(MdkRenderer r, const MdkMeshDraw* draw);
// Sprites are meshes: `SpriteDraw` is an alias of `MeshDraw` (render_types.h)
// and the material's shading mode carries the distinction, so there is no
// separate draw_sprite entry point.

// ---- the 2D canvas (HUD, menus, text) -------------------------------------
void mdk_render_canvas_begin(MdkRenderer r);
void mdk_render_canvas_quad(MdkRenderer r, const MdkCanvasQuad* quad);
void mdk_render_canvas_end(MdkRenderer r);

// ---- insets (3D views inside canvas rectangles: sniper mode) --------------
void mdk_render_begin_inset(MdkRenderer r, const MdkInset* inset);
void mdk_render_end_inset(MdkRenderer r);
```

`MdkRenderDesc` carries the window surface (Linux) or the screen (3DS), the
target size, the stereo mode, and the requested look.

**Why flat free functions and not a struct of function pointers:** a vtable
would put a runtime decision where the build already made it (t2k §10). A
function-pointer table would do the same thing, more slowly. Flat functions
over an opaque handle keep every call a direct, inlinable call.

### 5.2 Value types (the shared vocabulary)

`mdk_core/src/render/render_types.h` holds the plain-old-data that crosses the
seam. These are the C# `readonly record struct`s, ported literally:

```cpp
struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
struct Vec4 { float x, y, z, w; };
struct Colour { float r, g, b, a; };

// The vertex the original and the C# port use: position, uv, packed RGBA8.
struct Vertex { Vec3 pos; Vec2 uv; uint32_t colour; };

enum class MdkPass { Solid, DoubleSided, Mirror, Blended, OnTop, Overlay };
enum class MdkBlend { Alpha, Add };
enum class MdkShading { Original, Lit, Sprite };
enum class MdkPrimitive { Triangles, Lines };

struct MdkMaterial {
    int32_t  texture;      // index-texture id, or -1
    int32_t  palette;      // 256x1 palette id, or -1
    Colour   colour;       // vertex-colour multiplier
    int32_t  frame_count;  // animated textures: frames stacked vertically
    MdkPass  pass;
    float    row_shift;    // mirror
    MdkShading shading;
    MdkBlend blend;
    int32_t  depth_layer;
};

struct MdkPanorama {
    int32_t sky, mirror_sky, palette;
    float   wrap_width, horizon_row, offset, height;
    uint32_t top_colour, bottom_colour;
    bool    linear;
};
```

### 5.3 Capability tiers — the graceful-degradation policy

The C# renderer has one class with everything: MSAA, shadows, AO, glow, haze,
mipmapped/anisotropic colour textures, 2D array textures, the 2D canvas, insets,
stereo, gamma. The 3DS can do a subset. The design rule (Obsidian:
`compile-richer-fixed-function-onto-weaker-with-graceful-degradation`):

> **The richer backend gets the richer features. The weaker backend gets the
> same picture with the feature missing. Never a different picture.**

Concretely, the *shared* code (game logic) is identical on both. The *backend*
declares capabilities and the shared code asks:

```cpp
// render_capabilities.h
struct MdkCapabilities {
    bool enhanced_look;   // shadows, AO, glow, haze, tonemap, filtered textures
    bool msaa;            // 2x / 4x
    bool colour_mips;     // the enhanced look's expanded RGBA8 + mip chain
    bool hardware_stereo; // 3DS parallax barrier
    bool flat_stereo;     // SBS / interlaced / TAB composites
    bool insets;          // viewport+scissor sub-rect 3D views
    bool canvas_filtering;// bilinear palette lookups in the canvas
    int  max_texture_size;
};
MdkCapabilities mdk_render_capabilities(MdkRenderer r);
```

And — this is the important part — **the lighting rig is shared *data*, not a
shared *feature*.** `mdk_core` always computes `MdkLighting` (sun direction, sun
colour, sky colour, ground colour, exposure, haze colour/density) and the point
light list, exactly as the C# `Level/LevelLight.cs` and
`Objects/LightSources.cs` do. The Vulkan backend uses them; the Citro3D
backend ignores them and shades the original way. **No `#ifdef`, no branch in
the game code** — the game code is byte-for-byte the same and the 3DS simply
renders the original look, which is what the 3DS should show anyway.

The one thing that *is* branched on is the *texture representation*, and that
branch lives in the backend, not the game.

### 5.4 Module map (C# → C++)

| C# | C++ | Notes |
|---|---|---|
| `Mdk.Formats/*` | `mdk_core/src/formats/*` | 1:1. `BinReader` → `BinReader` (a `struct` with a `span`-like view). |
| `Mdk.Game/Scripts/*` | `mdk_core/src/scripts/*` | 1:1. The script VM is the biggest single subsystem (5.5 KLoC). |
| `Mdk.Game/Kurt/*` | `mdk_core/src/game/kurt/*` | 1:1. Partial classes → one `struct Kurt` with several `.cpp` files (no inheritance). |
| `Mdk.Game/Collision/*` | `mdk_core/src/game/collision/*` | 1:1. The BSP sweep is bit-exact-sensitive. |
| `Mdk.Game/Level/*` | `mdk_core/src/game/level/*` | 1:1, except `EnhancedLook` becomes *data* the backend consumes. |
| `Mdk.Game/Flow/*`, `Menu/*`, `Fall/*`, `Stream/*`, `Hud/*`, `Objects/*`, `Mods/*`, `DevTools/*`, `HdTextures/*` | `mdk_core/src/game/**` | 1:1. |
| `Mdk.Engine/Render/Renderer*` | **split** | The *interface* becomes `mdk_core/src/render/render.h`; the *implementation* becomes `mdk_linux/src/render/renderer_vk.cpp` and `mdk_3ds/src/render/renderer_c3d.cpp`. |
| `Mdk.Engine/Render/{Tonemap,DepthPull,ColourMips,PointLights,SunShadow,Bmp}` | `mdk_linux/src/render/` + `mdk_core/src/render/` | `DepthPull` is shared (the game needs the pulled matrices); `Tonemap`/`ColourMips`/`SunShadow` are enhanced-look-only → Linux tree. |
| `Mdk.Engine/Audio/*` | `mdk_core/src/audio/` (mixer) + per-platform sink | The software mixer is shared; the *sink* is per-platform. |
| `Mdk.Engine/Platform/*` | `mdk_core/src/platform/` (the game's own key/pointer enums, `GamepadMap`, `HeadLook`) + per-platform drivers | The *game-visible* input API is shared; the SDL3/libctru plumbing is not. |
| `Mdk.Engine/Diagnostics/*` | `mdk_core/src/util/{profiler,log_ring}.{h,cpp}` | 1:1, including the RAII `Measurement` and the EMA. |
| `Mdk.Engine/Upscale/*` | `mdk_linux/src/upscale/` | Linux-only (network + GPU upscaler). |
| `Mdk.App/Program.cs` | `mdk_linux/src/main.cpp`, `mdk_3ds/src/main_3ds.cpp` | Two frontends, one per tree, each owning the platform lifecycle. |

---

## 6. The Linux backend (Vulkan 1.3)

### 6.1 Structure

Modeled on `../t2k/t2k_pc`:

```
mdk_linux/src/render/
├── renderer_vk.cpp        ← the seam implementation; owns the Renderer struct
├── vk/vk_context.cpp      ← instance, device, swapchain, surface
├── vk/vk_resources.cpp    ← textures, buffers, render targets, the ring
├── vk/vk_pipeline.cpp     ← the pipeline cache, keyed by state
├── vk_scene.cpp           ← the gameplay frame (pass order)
├── vk_canvas.cpp          ← the 2D canvas batch
├── vk_post.cpp            ← enhanced-look post: AO, glow, tonemap
├── vk_stereo.cpp          ← the stereo composite
└── vk_shaders.h           ← generated SPIR-V headers
```

`Renderer` is one fat struct with defaulted members, exactly as t2k's: it holds
the context, the pipelines, the targets, the per-frame ring, and the shared
builder state. No class hierarchy, no virtuals.

### 6.2 Shaders

The C# shaders are HLSL (`shaders/*.hlsl`, 930 lines). They are **re-authored in
GLSL** for the Vulkan backend, keeping the C# structure so the two stay
diffable:

| C# HLSL | GLSL | Notes |
|---|---|---|
| `palette.hlsl` | `palette.vert` / `palette.frag` | index texture → 256×1 palette; nearest; index 0 transparent; animated frames stacked vertically (`uv.y = (fract(uv.y)+frame)/frame_count`); vertex colour multiplies. |
| `mirror.hlsl` | `mirror.frag` | no reflection; shows `MirrorSky` at `row_shift` rows lower; rows wrap. |
| `sky.hlsl` | `sky.vert` / `sky.frag` | screen-filling triangle from `gl_VertexIndex`; clip→direction; panorama column from azimuth (`atan2`), row from elevation. |
| `gamma.hlsl` | `gamma.frag` | `pow(colour, 1/gamma)`. |
| `depth.hlsl` | `depth.frag` | depth-only; index 0 is a hole. |
| `enhanced.hlsl` | `enhanced.frag` | the 4 modes; flat normal from screen derivatives; hemisphere + sun·N·L·shadow + point lights; tonemap; haze; dither. |
| `occlusion.hlsl` | `occlusion.frag` | 12-sample Alchemy AO, golden-angle disc, Bayer 4×4. |
| `post.hlsl` | `post.frag` | occlusion blurred over 4×4 same-plane pixels; glow from mips; screen-blend; dither. |
| `stereo.hlsl` | `stereo.frag` | SBS / interlaced / TAB / VR barrel (`K1=0.22, K2=0.24`). |

Pipeline: `glslang -V --target-env vulkan1.3 -I<dir> -o out.spv in.glsl`, then
`mdk_linux/tools/spv2h.py` → a `uint32_t[]` header. Vendored Vulkan-Headers and
volk come from `../t2k/t2k_pc/third_party` (same loader strategy: volk dlopens
the loader, so a machine with no ICD gets a message, not a missing `.so`).

### 6.3 Frame graph — original look

```
Arena triangles ──Layers (depth layers)──► batches per surface ──► Renderer
                                                                      │
 surface = index texture (R8) + palette (256×1) │ flat colour        ▼
                                                                 offscreen colour + depth
                                                                      │ blit
                                                                   swapchain
```

1. `mdk_render_begin_frame` → begin the render pass (offscreen colour + depth,
   or MSAA targets with resolve).
2. Sky: one screen-filling triangle, first.
3. The 6 `MdkPass` values in enum order: `Solid, DoubleSided, Mirror,
   Blended, OnTop, Overlay`.
4. Insets, if any: `UnderCanvas` (viewport + scissor, depth cleared) → the
   canvas (`Overlay` pass) → `OverCanvas`.
5. Gamma → the graded target (only if `gamma != 1`).
6. Blit the shown target → the swapchain (NEAREST at 100 %, LINEAR when scaled).

### 6.4 Frame graph — enhanced look (Linux only)

Exactly the C# graph:

```
 sun's depth (Lit casters) ──► shadow map 2048² ─────────┐
 camera's depth (opaque) ───────────────────────────────┐ │
 scene: filtered sky, enhanced (MSAA) ──► scene ───────┼─┴─mips─► post ─► frame ─► canvas, insets
                                                       └ ambient occlusion
```

Constants carried over: `ShadowSize=2048`, `SceneMips=6`, `OcclusionRadius=12`,
`OcclusionStrength=1.5`, `GlowThreshold=0.8`, `GlowIntensity=0.8`, `GlowMip=3`,
`MaxTargetSide=16384`, `MaxAnisotropy=16`.

**Upload batching.** The C# code learned this the hard way (`Renderer.cs`:
"a level's thousands of uploads, each submitted on its own (a fence) through a
cycled buffer … outran the GPU and ran out of memory"): the Vulkan backend
uses a **4 MiB batch size, 512-byte alignment, one command buffer, one cycled
transfer buffer, at most two batches in flight.** The C++ port keeps that.

### 6.5 Stereo

MDK's modes, implemented in the composite pass, 1:1 with the C#:
`sbs`, `sbsfull`, `crossview`, `int`, `intr`, `tab`, `tabr`, `vr`. Each eye is
rendered *whole* into its own full-size target, then composited. The HUD and
the menus are drawn whole in both eyes (zero parallax), as in the C#.

### 6.6 Headless mode

The Linux build has a **headless mode** (`--headless`), which renders into a
`VK_EXT_headless_surface` swapchain with no window behind it.

This is not a test hack bolted on: it is what the golden-frame harness
(§11.3) needs — it reads the image back instead of presenting it — and it is
the only way to run the renderer on a machine whose display server cannot
present a Vulkan swapchain. A software X server has no **DRI3**, which the
Mesa Vulkan WSI requires for presentation, so every Xvfb display fails to
present; the headless path does not care.

In headless mode the target size comes from `MdkRenderDesc::hidden_width` /
`hidden_height`, not from a window.

### 6.7 Platform

SDL3 for the window, input, gamepads, touch and audio stream. `WinHttp.cs` is
**dropped** (Windows-only); `Curl.cs` becomes a direct libcurl link on Linux.
`Display.cs` and `Input.cs` are shared in spirit: the *game's* key enum
(`Key`, `MenuKey`, `RawKey`, `Pointer`) and the rebinding logic live in
`mdk_core/src/platform/input.{h,cpp}`; the SDL event pump is in the Linux tree.

---

## 7. The 3DS backend (Citro3D / PICA200)

### 7.1 Frame lifecycle

Borrowed from `../forsaken/render_c3d.c`, which is a working D3D→Citro3D port:

```
mdk_render_create   → C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 16)   ← 4 MB command buffer
                      C3D_RenderTargetCreate × 3 (left / right / bottom)
                      C3D_RenderTargetSetOutput
                      shader DVLB parse, scratch buffer alloc
begin_frame          → C3D_FrameBegin(C3D_FRAME_SYNCDRAW)
                      C3D_RenderTargetClear + C3D_FrameDrawOn(target)
                      C3D_BindProgram
                      C3D_AttrInfo / C3D_TexEnv / C3D_DepthTest / C3D_CullFace / C3D_DepthMap
                      (ALL GPU state must be set explicitly — uninitialised registers
                       produce INVISIBLE geometry, not a crash)
draw                 → convert + upload vertices, C3D_DrawArrays
end_frame            → C3D_FrameEnd(0)
present              → (C3D_FrameEnd triggers the auto display transfer)
```

**Do not link picaGL.** The native citro3d path is the shipping path, exactly
as in Forsaken. The GL1-over-picaGL shim exists there as a fallback; here it
does not, because a second backend on 3DS buys nothing and costs VRAM and
maintenance.

**`pglSwapBuffers` conflict.** Forsaken's black-screen bug: picaGL's swap
calls `GX_DisplayTransfer`, which conflicts with citro3d's auto-transfer from
`C3D_FrameEnd`. We never link picaGL, so the conflict cannot arise — but the
rule stands: **`C3D_FrameEnd` is the only present path.**

### 7.2 D3D → PICA200: the two mandatory conversions

MDK is a Direct3D game. These are the same two fixes Forsaken needed, and
getting them wrong is the single most common way a D3D port renders
inside-out. From `../forsaken/3DS_PORT_NOTES.md`:

**(A) Depth range.** D3D's LH projection gives `NDC.z ∈ [0,1]`. PICA200 wants
`z ∈ [-1,0]`. Do it in ONE step, on the combined matrix, as Forsaken's native
citro3d path does — not the two-step D3D→GL→PICA conversion:

```
row[2] -= row[3]        // equivalent to _33' = _33 - 1, _43' = _43
```

Leave `_34 = 1` alone: D3D's view matrix gives `v.z > 0` for in-front points,
so `clip.w = +v.z` is correct.

**(B) Winding / culling.** The PICA200 has **no CW-front cull mode, ever** —
only `GPU_CULL_NONE`, `GPU_CULL_FRONT_CCW`, `GPU_CULL_BACK_CCW`. MDK's meshes
are CW-front (the C# renderer uses `SDL_GPU_FRONTFACE_CLOCKWISE`). So:

| The game wants | Call |
|---|---|
| back-face culling (the default) | `C3D_CullFace(GPU_CULL_FRONT_CCW)` |
| front-face culling | `C3D_CullFace(GPU_CULL_BACK_CCW)` |
| no culling | `C3D_CullFace(GPU_CULL_NONE)` |

**Do NOT** negate `View.z`/`Look.z`. **Do NOT** negate any row or column of the
modelview. **Do NOT** reflect the geometry to flip winding. **Do NOT** use
`GX_TRANSFER_FLIP_VERT` for handedness. If you reach for a matrix reflection
while debugging, stop: you are probably solving a winding problem with a depth
operation, and the answer is at the `C3D_CullFace` call.

**The transpose is not free — the direction matters.** MDK's matrices are
row-major with a **row-vector** convention (`p_clip = p_world * M`); PICA's
vertex shader consumes uniform register *i* as **row i** of a **column-vector**
matrix (`dp4 outpos.x, mvp[0], r0` is `clip.x = dot(register 0, pos)`, and
citro3d's own `Mtx_Multiply`/`Mtx_Translate` treat `r[j]` as rows —
translation lives in `r[3]`).

For the two to describe the same transform, `M_pica = Mᵀ` — and *Mᵀ*'s row *i*
is *M*'s **column i**. So **register i gets our column i — a strided read**
(`r[i].x = m[i]; r[i].y = m[4+i]; r[i].z = m[8+i]; r[i].w = m[12+i]`), exactly
Forsaken's `matrix_to_c3d` (`render_c3d.c:1077`, whose engine matrix has the
same memory layout as ours).

The one way to get it wrong is to upload our memory verbatim and call it
done: a verbatim row copy puts *M*'s **rows** where its **columns** belong —
the transpose of what the shader needs. A pure rotation then comes out
**inverted**, and the picture is mirrored rather than obviously broken.
(`C3D_FVUnifMtx4x4` copies `mtx->r[i]` to register `id+i`, so `mtx->r[i]`
must be our *column* *i*.)

Because register rows are engine columns, every register-space row tweak —
the depth remap `r[2] -= r[3]` and the 90° rotation below — is a **column**
operation on the engine matrix, not a row operation.

**The 90° screen rotation.** The 3DS top screen is portrait-native; the game is
landscape. Forsaken's `matrix_to_c3d` handles it as `r[0] = r[1]; r[1] =
-old_r[0]` — a proper rotation (determinant +1). The upstream picaGL version
uses `+1.0`, which is a *reflection* (determinant −1) and would flip winding a
second time, undoing fix (B).

### 7.3 8-bit paletted textures — the hard problem, and the measured answer

MDK's world is 8-bit indexed: an `R8` index texture plus a 256×1 palette, and
**palette index 0 is transparent**. The C# `palette.hlsl` samples the index
texture and then the palette.

**The 3DS cannot do that, and the reason is structural, not a missing feature:**

1. **PICA200 has no palette texture format.** citro3d's `GPU_TEXCOLOR` is
   `RGBA8, RGB8, RGBA5551, RGB565, RGBA4, LA8, HILO8, L8, A8, LA4, L4, A4,
   ETC1, ETC1A4` — no indexed format, and the texture unit's palette
   conversion is not exposed.
2. **PICA200 has no programmable fragment shader.** citro3d binds a *vertex*
   shader and a *geometry* shader; the fragment stage is the fixed-function
   `C3D_TexEnv` combiner plus the fragment-lighting LUTs. A combiner samples
   each texture unit with its **own** texcoord, so a per-texel
   index→palette lookup — a dependent texture read — is not expressible.

Therefore: **the palette lookup is expanded on the CPU.** This is the same
conclusion the GX→PICA200 reference reached ("palette formats … have tile
geometry but no runtime decode — the converter must expand them").

**Measured cost** (probe over the GOG data, `./MDK/TRAVERSE/LEVEL3..8`):

| | value |
|---|---:|
| distinct world textures, levels 3–8 | 926 |
| index bytes (the paletted source) | 41.2 MiB |
| expanded to RGBA8 | **164.8 MiB** |
| expanded to RGBA4 / RGB565 | 82.4 MiB |
| textures lossless in RGBA4 | **0 / 926** |
| textures lossless in RGB565 | **0 / 926** |
| distinct (texture, palette) pairs | **2657** |
| expanded to RGBA8 over all pairs | **348.8 MiB** |

Two findings that decide the design:

- **A 4-bit format is never lossless here.** The palettes are *dithered
  ramps*: `H1_FRONT` alone uses 199 distinct indices with 48/50/52 distinct
  R/G/B values, and 8→4 bits only round-trips values that are multiples of 17
  — none of which are. A per-texture "best fit" format choice therefore buys
  **nothing**: every texture needs RGBA8 for 1:1 colour, and the "best fit"
  total equals the uniform RGBA8 total. (75 textures pass a loose
  "≤16 distinct per channel" test; all 75 still fail exactness.)
- **The cost is per (texture, palette), not per texture.** Palette indices
  64–175 are the *arena's* 112 colours, so a texture shared between arenas
  must be expanded once per arena palette. 2657 pairs across the six levels.

**So the 3DS must stream by arena.** The arena-private sets are far smaller
than the level totals: LEVEL 3's ten arena-private sets are 7.9 MiB of
indices → 31.4 MiB of RGBA8, i.e. **~3 MiB per arena** — which fits the
3DS's 6 MB of VRAM, if only one arena is resident.

**The chosen 3DS texture strategy:**

1. **The index data stays 8-bit and lives in system RAM**, loaded per level
   (8–10 MiB per level). The expansion is the expensive part, so it is done
   **once per (texture, palette)**, not per frame.
2. **Expansion is on demand, into VRAM, with an LRU.** The cache holds the
   expanded RGBA8 for the textures actually being drawn. A miss expands one
   texture (a 256×256 one is 65 536 palette lookups, ~0.5–1 ms on the
   ARM11) and uploads it. The working set of a level is stable, so the miss
   rate settles; a miss is a stall, and the budget is sized so the working
   set fits.
3. **Format: `GPU_RGBA8`** — the only lossless choice. The 3DS's 6 MB VRAM is
   spent on colour fidelity, not on saving bytes. (`GPU_RGBA4` would halve
   the bytes and change the picture; that is a *fidelity* decision and the
   fidelity requirement wins. A future "performance" toggle is a separate
   decision with its own evidence, not a default.)
4. **Animated textures are the worst case.** `EXPLODE` is 128×128×26 =
   1 663 KiB at RGBA8 — 27 % of VRAM by itself. Animated textures are 11 % of
   the bytes but the only ones needing per-frame attention, so they get a
   **dedicated small pool** and the frames are uploaded as the original's
   **vertical strip** (the frames are already stacked that way in the file),
   with the frame index selecting the slice in the shader's UV.
5. **Alpha is cheap.** Only 65 of 926 textures contain index 0 (the 54
   "keyed" effect textures plus 11 animated ones, because
   `TextureArchive.Parse` `continue`s before the `Zero.ByKind` index-0→16
   replacement for animated kinds). Every floor and wall texture is opaque.
6. **The sky panorama is paletted too** — 1804×360, ~2.5 MiB at RGBA8 per
   level. It is expanded and uploaded like any other texture.
7. **Bullet holes rewrite a texture's indices.** On the 3DS that is a
   re-expansion of one texture and a re-upload — strictly cheaper than the
   C# enhanced path, which re-expands the whole RGBA8 colour texture.

**The fallback** if the LRU thrashes in a busy arena: raise the cache
budget by dropping the system-RAM index copy for arenas that cannot be
returned to (the game never goes back more than one arena), and if that is
still not enough, halve the resident set by keeping the *static* textures
resident and re-expanding the *animated* ones per frame change. Both are
measurable; neither is the default.

**Contrast with the Linux backend.** The Vulkan backend has a programmable
fragment shader, so it does the lookup in the shader exactly as the C# does
(`palette.frag`: sample the index texture, then the palette). No expansion,
no LRU, no 4× memory. The two backends therefore differ in *how* they answer
the palette, not in what they draw.

### 7.4 The 2D canvas

MDK's canvas is a 2D orthographic overlay: 4096 quads × 6 vertices, drawn with
`DrawImage` / `FillRect` / `FrameRect` from the HUD, the menus, the fonts and
the movies. In the C# it is a separate ortho pass over a collected vertex
array, with neighbouring same-material quads merged.

On the 3DS:
- One shared vertex buffer, written by `mdk_render_canvas_quad`, uploaded once
  per frame.
- A separate ortho matrix, and the canvas is drawn **after** the 3D pass in the
  same `C3D_FrameBegin/End` bracket, with depth test off and depth write off.
- The canvas coordinate space is the C#'s **640 × 360** (`CanvasWidth = 640`,
  `CanvasHeight = 360`); the backend scales it to the 400×240 top screen.
  The *content* stays in 640×360 so the layout is 1:1 with the C#.
- 4096 quads × 6 verts × 20 bytes = 491 KB of vertex data. That is too much
  for a 3DS linear buffer alongside everything else, so the 3DS canvas capacity
  is **512 quads** (61 KB), and the shared code is written so the canvas
  allocates from a caller-provided pool: the backend states its capacity and
  the shared code batches quads into as many canvas draws as it needs. The
  original's HUD uses far fewer than 512 quads per frame; the menu screens are
  the worst case and are split across frames if needed.

### 7.5 Insets

MDK's sniper mode draws 3D views inside canvas rectangles, under or over the
canvas. On the 3DS an inset is a second `C3D_SetViewport` +
`C3D_SetScissor` pair inside the same frame, rendering into the same target.

**Two traps**, both from the Obsidian notes:

- The PICA viewport is a **hardware clip rectangle**, not just a scale. A
  zero-area viewport **silently drops the draw** (the "invisible 3D trap").
- The viewport must be in PICA portrait coordinates: Forsaken's chain is
  `C3D_SetViewport(bottom, (400-W)-X, H, W)` — the GL and picaGL chains are
  `glViewport(X, screenH-Y-H, W, H)` and `_picaViewport(viewportY, viewportX, H, W)`
  respectively. MDK's canvas is 640×360 in a 400×240 target, so the mapping is
  computed, not copied.

### 7.6 Stereo

MDK's flat-display stereo modes do not apply to the 3DS. The 3DS's top screen
is an **autostereoscopic parallax barrier** driven by two framebuffers,
`GFX_LEFT` and `GFX_RIGHT`, with the depth from `osGet3DSliderState()`.

- Render the frame twice, once per eye, into `s_targetLeft` and
  `s_targetRight`, with the eye separation applied to the camera (and, for the
  scope, to the projection).
- `gfxSet3D(true)` when stereo is active; `osGet3DSliderState()` for the
  depth. **Clamp it to [0,1]** — mandarine returns garbage (~134 million) and
  Forsaken shipped a bug from exactly that.
- **Do not apply the separation with a full matrix multiply.** Forsaken's
  `ApplyMatrix` adds the translation components, so a "direction" offset became
  a world-position-scaled offset of ±2 billion units. Scale the camera-right
  unit vector (the first row of the final matrix) directly.
- Slider = 0 disables stereo and restores the full framerate.
- Mandarine does not simulate the autostereo: both eyes land in the same
  framebuffer and only the right one is visible. This is an emulator
  limitation, not a bug — record it as such in the test evidence.

### 7.7 Memory budget (the real constraint)

The 3DS has 6 MB of VRAM and 64 MB of RAM (32 MB of "system" heap on OG).
Forsaken's notes are the guide:

- `GPU_RGBA8` textures exhaust VRAM after ~7 1024×1024 textures. **Use
  `GPU_RGBA4`** (16-bit) for the 3DS.
- The PICA200 requires **8×8 Morton (Z-order) tiling** for textures. The
  3DS backend tiles at upload.
- A 4 MB vertex scratch holds ~116 000 vertices of the 36-byte
  `gpu_vertex_t` layout. Forsaken raised it from 1 MB after heavy frames
  spiked past 30 K vertices.
- The 3DS command buffer is 4 MB (16× the default) to survive draw-call
  spikes; it costs +3 MB of linear memory.
- MDK's level data (arenas, textures, models, sounds) goes into **RomFS**, not
  RAM. The GOG data at `./MDK` is staged into `mdk_3ds/romfs/` at build time.

### 7.8 Platform

- libctru: `hidScanInput` for the circle pad and the buttons, `aptMainLoop` for
  the lifecycle, `gfxInitDefault` + `gfxSet3D`, `ndsp` for audio,
  `osGet3DSliderState` for the stereo depth.
- **The hard-float ABI trap.** With `-mfloat-abi=hard`, a float return comes
  back in VFP `s0`, not `r0`. A caller that lacks the `extern float f(void);`
  declaration reads garbage from `r0` (~65535) and converts it to float.
  Forsaken shipped exactly this bug: `eye_sep = 65535 × 30 = 1 966 050`.
  Every cross-TU float-returning function gets a proper declaration; the build
  uses `-Wall -Wextra` and the C++ compiler's implicit-declaration rules are
  stricter than C's, which helps.
- **ARM unaligned access.** The ARM11 (ARMv6K) traps or returns garbage on
  unaligned `float`/`uint32` loads. The MDK format parsers read `u16` fields
  and then cast the buffer to `float*` — the pointer is then 2-byte aligned.
  **All binary parsing goes through `memcpy` into a local** (or through
  `BinReader`'s `F32()`, which is `memcpy`-based). This is non-negotiable and
  is a port-time fix, not an optimisation: the C# `BinaryReader` was
  alignment-agnostic.
- **Save data** goes to `sdmc:/3ds/mdk/` (writable), never RomFS.
- **R1 FZ/DN.** The main thread and every worker get the FZ+DN FPSCR helper,
  with the read-back and the rationale comment. See `AGENTS.md` R1.

### 7.9 3DS Enhanced Mode feature toggles (owner directive 2026-11-14)

The 3DS enhanced look is best-effort against the PC render (§7, and the
owner direction recorded in `SPEC-COMPLETION.md` §3 item 6). Every
enhanced feature on the 3DS is individually switchable, so its cost and
its look can be A/B-compared on hardware:

- **Zero CPU fallback.** All Enhanced Look features (sky shadows,
  PICA200 fragment lighting/LUTs, atmospheric haze, mirrors, and
  additive glow billboards) execute strictly on the PICA200 GPU. No
  per-vertex lighting, UV scrolling, or software lighting calculations
  on the ARM11 CPU. A feature the PICA200 cannot express in TexEnv
  combiner stages, ProcTex/maestro extensions, fragment-lighting LUTs,
  or hardware blend modes is not emulated on the CPU; it is left out.
- **Individual runtime toggles.** The renderer configuration carries one
  boolean per potentially expensive PICA200 operation:
  `enableSkyShadows`, `enableFragmentLights`, `enableHaze`,
  `enableMirrors`, `enableGlowBillboards` (`mdk::EnhancedToggles` in
  `render/render_types.h`, carried in `MdkRenderDesc` and changeable at
  runtime through `mdk_render_set_enhanced_toggles`). Disabling a
  toggle cleanly skips the corresponding GPU command-list or
  render-target pass and introduces no CPU overhead: the check is one
  boolean read at pass setup, never per-vertex or per-pixel CPU work.
- **Menu & config file integration.** The toggle states persist in
  `sdmc:/3ds/mdk/config.ini` (loaded at boot, written whenever they
  change; a missing file means the shipped defaults, all on) and are
  exposed in an in-game Video/Graphics Settings menu so each feature
  can be toggled live during gameplay for side-by-side visual A/B
  comparisons. The menu (implemented 2026-11-14; owner 2026-11-27: the
  START button only): START opens and closes it over the paused game
  (SELECT is the run toggle, `controls_3ds.h`); D-pad moves,
  A (or Left/Right) flips the selected feature, and the flip calls
  `mdk_render_set_enhanced_toggles` and rewrites `config.ini` in the
  same frame. It is drawn on the canvas with the game's own interface
  fonts (`mdk::formats::Font` + `mdk::FontView`, the FONTBIG/FONTSML
  pair in SYS_PAL colours, ported from the oracle's Font.cs /
  FontView.cs) by `mdk::EnhancedMenu`; the menu subsystem the
  2026-11-05 directive cut (main menu, save screens) stays cut — this
  is a settings page over the running game, not that subsystem. If
  the fonts fail to load there is no menu; holding START for two
  seconds quits instead, and the boot log says so.

The same toggles are honoured by the Linux/Vulkan backend wherever the
feature already exists there (sky shadows = the sun shadow pass, haze =
the post pass's haze fade, glow = the post pass's glow term), so a
config written for the 3DS means the same thing on the PC build.

---

## 8. Audio

The C# `AudioDevice` is a **hand-written software mixer** over an SDL3 audio
stream: 64 voices, an effects bus and a music bus, a one-pole 6 kHz low-pass on
the music bus, a master limiter at −0.5 dB, and the original's 3D sound laws
(`SoundMixer.cs`: `dB = -25 + 25·volume/0x7FFF`, full volume to 20 units,
linear to 0 at 250, Doppler at 1100 u/s, pitch 0.25–3, scope listening).

**The mixer is shared code** (`mdk_core/src/audio/`), because the sound laws
are gameplay. Only the *sink* differs:

| Target | Sink |
|---|---|
| Linux | `SDL_OpenAudioDeviceStream` with `SDL_AUDIO_F32LE`, stereo, 44.1 kHz — 1:1 with the C#. |
| 3DS | `ndsp`, **the DSP mixes** (owner decision 2026-11-19: replicate `../forsaken`'s sound system, below). |

The `Muted` output mode (mix and drop, keep the frame accounting) is kept, so
the shared code and the tests run headless.

**The 3DS follows Forsaken's sound system** (owner decision 2026-11-19; reference
`../forsaken` at e8f0774, `forsaken_3ds/src/audio/sound_3ds.cpp` and
`music_3ds.cpp`, proven on the Old 3DS). The sound LAWS stay shared code -- which
sounds play, their volume, pan, Doppler pitch and scope are computed by
`mdk_core/src/audio/` exactly as on the PC -- but the summing moves off the ARM11:

- **Effects:** each live voice plays on its own `ndsp` channel and the DSP mixes
  them. A pool of **8 effect voices** (channels 0-7): the DSP firmware starts
  skipping audio when too many channels are active, which is why Forsaken caps it
  there. When all 8 are taken, the voice claimed longest ago is reset and handed
  over, and its sound is told it has no channel. Volume and pan become the
  channel's mix, pitch its rate.
- **Music:** DSP-ADPCM streamed on **channel 23**, outside the effect pool, by a
  worker thread that wakes every 25 ms and keeps four wave buffers queued, each
  carrying its own ADPCM context. The music bus's 6 kHz low-pass becomes the
  channel's biquad filter if it is kept.
- **The voice LIFECYCLE stays logical and shared.** The 64 voices, their start,
  length and end, keep being advanced by the shared device every frame, because
  the scripts read them: `if_sound_playing` (opcode 233,
  `ScriptRuntime::isSoundPlaying`) branches on it. Losing a DSP channel to
  eviction silences a sound; it must never end it early in the game's eyes.
- **Not carried over:** the ARM11 summing of the 64 voices into one stream, the
  80 ms mixed stream and the master limiter (the PC keeps all three). A worker thread follows AGENTS.md R1
  (FZ/DN at entry) and §10's small-stack rule.

This supersedes the earlier "voice pool shrinks to 24 and the queue to 40 ms" line,
which the port never shipped.

---

## 9. Build and packaging

### 9.1 Linux

CMake, mirroring `../t2k/t2k_pc/CMakeLists.txt`:

- C++17, `-fno-exceptions -fno-rtti`, `-Wall -Wextra -Wno-unused-parameter`,
  `-fno-strict-aliasing` (matching the 3DS Makefile — the shared code relies
  on it, and an optimised-but-unprotected desktop build is exposed to the same
  class of bug the 3DS was).
- SDL3, Vulkan (vendored headers + volk), glslang, Python3 (for `spv2h.py`).
- Core sources are listed **explicitly**, not globbed, so a source that lands
  on one target only is a build error rather than a silently different game.
- Default `CMAKE_BUILD_TYPE` is `RelWithDebInfo` (matching the 3DS's `-O2`).

### 9.2 3DS

Makefile, mirroring `../t2k/t2k_3ds/Makefile`:

```makefile
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mfpu=vfp
COMMONFLAGS := $(ARCH) -O2 -ffunction-sections -fdata-sections \
               -fno-math-errno -fno-strict-aliasing
CXXFLAGS := $(COMMONFLAGS) $(INCLUDE) -std=gnu++17 -fno-exceptions -fno-rtti \
            -Wall -Wextra -Wno-unused-parameter
LDFLAGS := $(ARCH) -specs=3dsx.specs -Wl,-Map,$(TARGET).map \
           -L$(DEVKITPRO)/libctru/lib -L$(PORTLIBS)/lib
LIBS := -lcitro3d -lctru -lm
```

- `-mtp=soft` is **kept** (runtime-helper access to the TLS pointer; it is
  separate from the float ABI).
- No `-ffast-math`, no `-Ofast`. `-fno-math-errno` is kept.
- The PICA shaders are compiled with `picasso` and embedded with `bin2s`,
  using a **grouped target** (`&:`) so `make -j` cannot run the recipe twice
  and pair a truncated `.s` with a stale header.
- `-fsingle-precision-constant` is **not** applied globally. MDK has no
  mechanics-gated double math that t2k does, but the same reasoning applies:
  widening it retypes every unsuffixed literal in the tree.
- The identity block sets the home-menu strings, and `tools/check_cia_smdh.py`
  re-reads the SMDH out of the **built** `.cia` and fails if a populated
  language slot disagrees. `APP_AUTHOR` is a fixed, chosen publisher.

### 9.3 Bundling the GOG data

`./MDK` holds the GOG distribution (`MDKD3D.EXE`, `MDK95.EXE`, `MDKD3D.EXE`,
`FALL3D/`, `Manual/`, …). The 3DS build stages the data the game needs into
RomFS:

```
MDK/            →  mdk_3ds/romfs/mdk/
├── MISC/       (MDKFONT.FTI, FLIC/, sound banks)
├── LEVEL*/     (per level: DTI, MTO, CMI, MTI, SNI)
├── FALL3D/     (BNI, the fall)
├── TRAVERSE.BNI, TRAVSPRT.BNI, OPTIONS.BNI
└── STREAM/
```

- The staging script **lowercases and hard-links** into a temp tree: the
  3DSx tool does not follow symlinks, and MDK's files are mixed-case while the
  3DS's filesystem is case-sensitive.
- The **case-insensitive resolver** (`CaseInsensitivePath.cs`) is ported to
  `mdk_core`, because the game asks for `MISC/MDKFONT.FTI` and the GOG tree
  ships `MISC/mdkfont.fti`. On the 3DS the resolver is a **portable** lookup
  against the staged RomFS tree, not a filesystem path.
- The CIA's RomFS is built by `mkromfs3ds`; the `.3dsx` embeds it with
  `3dsxtool --romfs=`.
- The staged data is **not** committed (it is the user's copy of a commercial
  game). The build script stages it, and the CI check verifies the *staging
  manifest*, not the bytes.
- **The staging is lower-case and hard-linked** (`mdk_3ds/tools/stage_romfs.sh`).
  Lower-case gives one canonical spelling for the case-insensitive resolver
  to find; hard links are used because **3dsxtool does not follow symlinks**
  and a symlinked tree would be silently empty in the `.3dsx`. The measured
  result: 69 files, 79 MB, producing a 118 MB `.3dsx`.
- **The `.3dsx` carries both the SMDH and the RomFS**:
  `3dsxtool --smdh=... --romfs=...`, so the artifact is self-contained and
  needs no data directory at run time.

**The `.cia` needs `makerom`, which is NOT part of the devkitPro toolchain as
installed here.** `mdk_3ds/tools/build_cia.sh` detects this and exits
non-zero with a plain explanation, rather than producing a `.cia` that
installs and then shows the wrong title. Building `makerom` needs
`libmxml` as well, which is also absent. The `.3dsx` — same RomFS, same
SMDH — is the artifact that runs in an emulator and through a homebrew
loader, and it does not need `makerom`.

### 9.4 The gate suite

`tools/check.sh` runs, in order, failing on the first failure:

1. `audit_c_with_classes.py` — the language contract.
2. `gates_vfp.py` — the hot-path rules, the exemption grammar, the corpus
   freshness, the build flags (R10).
3. `audit_arm11_math.py` — trig-LUT coverage, fixed-point candidates.
4. `audit_r11_idiv.py` — runtime integer division/modulo in hot loops.
5. `gates_smdh.py` — the CIA's home-menu identity.
6. `gates_romfs.py` — the staged data manifest is complete.

---

## 10. Performance contract (summary)

The full text lives in `AGENTS.md`. The shape is inherited from t2k, with the
MDK-specific parts filled in:

- **Hot path** = per-frame at ≥1× frame count **and** in the top 15 of a
  captured profile, **or** ≥256 calls per frame, **or** annotated
  `/* @hotpath */`. The corpus is derived from a capture, never from memory.
- **R1** FZ+DN on every float thread (3DS).
- **R2** no float→int conversion in a hot loop body.
- **R3** no float comparison driving control flow in a hot loop.
- **R4** no double precision in hot paths.
- **R5** loop counters and indices are integer.
- **R6** no libm transcendentals in hot paths — everything goes through
  `mdk::fastSin` / `fastCos` / `fastAtan2`, a **single LUT on both targets**
  (t2k's rule: a desktop SIMD path and a 3DS LUT path is two different games
  agreeing approximately).
- **R7–R9** SHOULD: division/sqrt hoisting, no implicit float promotion out
  of a fixed-point domain, stay in the 32 S-register file.
- **R10** the devkitPro baseline flags, verified in the build.
- **R11** no runtime integer division/modulo in a hot loop — **this ARM11 has
  no integer divide instruction.** MDK's script VM and the BSP sweep are the
  places to watch.
- **T1/T2** (temperature-independent): solve don't search; no float comparison
  in a loop condition.
- **Exemptions** use the exact grammar
  `/* @vfp-exempt R<id>[,R<id>...] — <reason>. measured <cost>. Verified <YYYY-MM-DD>. */`
  and are *not* a TODO.

**MDK-specific hot-path candidates** (to be confirmed by the first capture):
`Bsp::SweepBox` (the innermost loop of every move), `ScriptVm::Step`
(30 Hz, but a very large body), `Kurt::Update`, `ObjectMotion::Step`,
`LevelView::Draw` (the vertex transform), `AudioDevice::Update` (64 voices),
`KurtSprite` (the RLE decode), and the 3DS vertex conversion + upload.

---

## 11. Testing and validation

### 11.1 Unit tests

`tests/formats/` — a hand-rolled test harness (no Catch2: the 3DS build must
not pull a test framework, and the same source must compile on the host). Each
C# format test gets a C++ twin:

- `BinReader` round-trips, the `F32()` unaligned-safety property.
- `Dti` / `Mto` / `Arena` / `TextureArchive` / `Texture` / `Palette` /
  `Model` / `ModelAnimation` / `SpriteAnimation` / `Bni` / `Fti` / `Sni` /
  `Cmi` / `Cmi.Beta` / `Flc` / `Mve` / `Gif` / `Png` / `Glb` / `Wav` / `Font` /
  `SniperScreen` / `Fall3d` / `BetaDemo`.
- The BSP node layout: 44-byte retail and 36-byte beta-demo nodes.
- The script decoder: every opcode of the 251-opcode table.

The C# xUnit tests under `tests/Mdk.Formats.Tests` are the specification; the
C++ tests mirror them one-for-one, including the fixture bytes.

### 11.2 Golden-state tests

`tests/golden/` — the C# assembly is driven with a fixed input (a synthetic
level, a fixed frame sequence) and dumps its state as JSON; the C++ binary does
the same; a differ compares. Covered: the BSP sweep results, the script VM's
register file and object table, Kurt's full state after N steps, the mixer.

### 11.3 Golden-frame tests

The C# renderer is driven headless (hidden window) and dumps a PNG per frame;
the C++ Vulkan renderer does the same with the same camera; a differ compares
per-pixel with a tolerance (exact for the original look's flat colours, ±1 for
the enhanced look's dithering).

### 11.4 3DS testing in mandarine

- The emulator runs on a **separate X display** from the user's (`:1` is the
  user's; the tests use a dedicated one, e.g. `:401`).
- `mdk.3dsx` is launched with the emulator's CLI; the frame is captured with
  the emulator's screenshot facility, or by running the app in a headless
  capture mode that writes the framebuffer to a PNG.
- The A/B checklist: sky, level geometry, textures and palette, Kurt's sprite,
  the HUD, the menus, the depth ordering (no z-fighting), the culling (not
  inside-out), the stereo eyes, the 3DS's own `gfxSet3D` behaviour.
- **Mandarine is not the hardware.** Emulator-vs-hardware divergences are
  recorded as such (the Obsidian note `3ds-emulator-masks-real-hardware-bugs`).
  The 3DS evidence requirement is *reviewed*, not *machine-checked* — the same
  status t2k's P1–P6 rules have.

### 11.5 The cross-backend oracle

The Forsaken rosetta-stone lesson, applied here:

> **Any hypothesis pointing at shared engine code is disproved the moment the
> other backend renders the same data correctly.**

With two 3DS-class backends unavailable (we have one 3DS backend and one
Vulkan backend), the oracle is: *if the Vulkan backend is right and the 3DS
backend is wrong, the bug is in the 3DS backend or in the shared code's
target-dependent assumptions.* The 3DS-specific suspects are enumerated in
§7.2 (depth, winding, rotation), §7.3 (palette), §7.4 (canvas scale),
§7.5 (viewport), §7.6 (stereo), and §7.7 (memory).

---

## 12. Adversarial validation

The primary coding agent writes the code; an **independent reviewer** reviews it
adversarially.

- The reviewer's job is to **disprove** the code, not to praise it. It is
  given the spec, the diff, and the evidence, and it must produce a
  falsification attempt for each claim.
- The reviewer is a separate, fresh, read-only run that did not write the
  code.
- Review dimensions, in priority order:
  1. **Fidelity to the C# oracle** — does the C++ do what the C# does? Name
     the exact method and the exact divergence.
  2. **The 3DS traps** — depth range, cull enum, the 90° rotation's
     determinant, the float-return ABI, unaligned loads, the PICA viewport
     mapping, the VRAM budget.
  3. **The performance contract** — a hot path that violates R2/R3/R4/R6/R11.
  4. **The language contract** — a virtual, a template, an exception, a
     `std::function`.
  5. **Memory and lifetime** — an in-flight buffer overwritten, a texture
     freed while the GPU reads it.
- A finding is closed only by a **reproduction**, not by an argument. The
  validator is required to give the failing input and the observed output.

---

## 13. Phase plan

Each phase has an exit criterion that is *checked*, not asserted.

| Phase | Work | Exit criterion | Status (2026-11-19, from `docs/COMPLETION_LEDGER.tsv`) |
|---|---|---|---|
| **P0** | This spec, `AGENTS.md`, `docs/DOCTRINE.md` | The spec is reviewed; the doctrine states the fidelity ladder. | **Done.** SPEC.md, AGENTS.md and docs/DOCTRINE.md are in the tree and in force. |
| **P1** | The tree scaffold, the build systems, the toolchain | **DONE (verified 2026-10-21).** `mdk_linux` builds and renders headless on lavapipe; `mdk_3ds` builds a `.3dsx` that runs in mandarine at 60 Hz with the MDK data embedded in its RomFS. | **Done** (2026-10-21). |
| **P2** | `tools/` — the python gates | Each gate runs and **fails on a seeded violation** (a test that cannot fail is not a test). | **Done in practice, not proven per gate.** The gates run in `tools/check.sh`; the frame gate and the renderer pins carry seeded-violation arms (`tests/scripts`). Not every python gate has its own seeded-violation test. |
| **P3** | `mdk_core/formats` + the unit tests | Every C# format test has a passing C++ twin. | **Done:** `core.formats` (8ba4335). |
| **P4** | `mdk_core` — the game logic, the script VM, the collision, the audio mixer, the platform-neutral parts | The golden-state tests match the C# byte for byte. | **Done except the script VM's tail:** `core.goldens` (0b8c974), `core.flow` (0132ee8), `audio.mix-parity` (653524a). Open: `opcodes.remaining-27` (27 opcodes decoded but no-op). |
| **P5** | `mdk_linux` — the Vulkan renderer, the original look | The golden-frame tests match the C# within tolerance. | **Done:** `linux.render-frame` (f5314d3), `p5.0.frame-harness` (7f0d867), `p5.vulkan-goldens` (bd13abd). |
| **P6** | `mdk_linux` — the enhanced look, the stereo, the mods | The enhanced-look frames match; the HD-texture path is Linux-only and works. | **Open:** `p6.enhanced-stereo`. |
| **P7** | `mdk_3ds` — the Citro3D renderer, the platform, the audio sink, the RomFS | The 3DS build runs a level in mandarine with the A/B checklist passing. | **Done, with the 3DS work continuing:** the d1 3DS rows, `p7.platform-lifecycle` and `p7.romfs-streaming` (6f973de), `audio.3ds-sink` (34b230a), on-console `hw.harness-cycle`, `hw.pixels-rgba8`, `hw.profile-og`. Open: `hw.sleep-resume`; `spec.voice-pool` (the Forsaken sound system, §8). Beyond the phase: the 3DS Enhanced Mode toggles (§7.9) and the Old-3DS frame-time work (`docs/3DS_PERF_EXPERIMENTS.md`). |
| **P8** | The packaging (`.cia`), the gates green, the adversarial sweep | `tools/check.sh` is green; the adversarial validator has no open findings. | **Partly done:** `p8.cia-build` (ed9d653). Open: `hw.cia-boot`, which waits for the owner's own `.3dsx` test on a New 3DS first; the final `tools/check.sh` + adversarial sweep at the end. |

**Each phase is worked in increments** (`AGENTS.md` §0): pick the next
unfinished item, work it, run its exit criterion, and move on. A phase whose
exit criterion cannot be met is reported as blocked with the evidence, not
retried forever.

---

## 14. Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| The 3DS port cannot match the original's frame rate at 400×240. | High | The 3DS budget is planned (RGBA4, 512 canvas quads, 4 MB scratch, 16× command buffer) from Forsaken's measured numbers. The scope is the *original look*, which is cheap. If the frame rate is short, the levers are LOD, fewer shadows-of-a-shadow (none), and the arena's visible-set reduction — in that order. |
| The 8-bit palette path on PICA200 misbehaves (dependent read). | Medium | Option C (CPU expansion, cached per palette) is the documented fallback; the seam does not change. |
| The Vulkan backend is a lot of new code. | Medium | t2k's `vk_*` structure is a working reference and the vendored headers + volk are already available. The original look needs 5 pipelines, not 9. |
| The 3DS "enhanced look" is impossible and someone wants it. | Certain | It is out of scope by §1.2 and the capability struct says so. The 3DS shows the original look, which is the *correct* look for the platform. |
| The C# oracle drifts. | Low | The C# is frozen at upstream 1.1.1 (`d5bb66ec`) for the duration of the port; a divergence is resolved by deciding which is right, and a later upstream fix is ported deliberately. |
| The 3DS's 640×360 canvas does not fit 400×240 legibly. | Medium | The canvas is scaled; the HUD is legible because the original's HUD is drawn at 640×360 and the 3DS's 400×240 is a 0.625 scale — the same as a 640×480 original shown at 400×300. If it is too small, the 3DS build uses a **240×320 bottom screen** HUD or a scaled 320×240 letterbox, as Forsaken did. |
