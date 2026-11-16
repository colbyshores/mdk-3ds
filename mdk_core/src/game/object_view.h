#pragma once

// ============================================================================
// object_view.h — an object's model, laid out once by surface.
//
// A port of src/Mdk.Game/Objects/ObjectView.cs. The C# draws an object in two
// stages, and this file is the first of them:
//
//   model + palette --once--> layout: batches by surface (part, vertex, UV per
//                               corner)                       <-- this file
//   object --pose (shared frames)--> stream vertices --(transform)--> renderer
//
// The layout is made once per (model, palette) at a level's load and reused by
// every object that draws that model with that palette, which is why it is a
// cache and not a per-frame build. The posed vertices are the second stage:
// they depend on the object's animation frame and hidden parts, and they are
// written into the frame's vertex stream per draw (`ObjectStream` below).
//
// THE STREAM IS NOT THE GPU BUFFER. The C# writes its posed corners into one
// per-frame stream that the renderer owns (`Renderer.Stream`, uploaded once in
// Present) and draws each batch at its offset in it. Neither C++ backend has a
// per-frame stream in its seam, and neither needs one: what decides the picture
// is the ORDER and CONTENT of the corners, the offset each batch is drawn at, and
// the (texture, palette, frame, transform) each draw carries, and all of that is
// here. The GPU half -- mapping a queued draw's surface and palette to a backend
// texture, uploading this stream once and issuing the draws -- is the next
// increment, and it changes none of what is here.
//
// WHAT IS NOT PORTED, and why:
//
//   * The GPU half (the stream's upload and the draws themselves). It is the
//     next increment; see docs/CONTINUATION.md.
//   * The mods' model replacements (`Swaps`/`ModelSwap`, Source::Mod). Those
//     are the enhanced look's HD models; the mods are not ported, so every
//     batch here is a model batch, exactly as it is when Swaps is null.
//
// WHY IT IS SHARED. The batches, the corner order and the UVs decide the
// picture, so both backends must agree on them; only the GPU objects differ
// (SPEC.md 5.3).
// ============================================================================

#include <stdint.h>

#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "render/material_resolver.h"
#include "render/render_types.h"
#include "scripts/object.h"
#include "util/matrix.h"

namespace mdk {

class ModModels;
class ModelSwap;
struct HdImage;

namespace formats {
class Model;
class Palette;
class TextureArchive;
} // namespace formats

// A batch's place when no part of it has a visible triangle (the C#'s Unseen).
static const int32_t kObjectUnseen = 2147483647; // int.MaxValue

// The C#'s Renderer.StreamVertices: how many corners one frame's stream holds.
// A batch that does not fit in what is left is NOT drawn, and the cursor does not
// move, so a later, smaller batch still fits (Renderer.cs's Stream returns an
// empty span and ObjectView.Draw skips the batch).
//
// This is the C#'s SEMANTIC bound, not an allocation: the stream below grows to
// the frame (2559 corners at level 3 tick 1, 5616 at level 8's, measured on the
// oracle) and only refuses a batch that would pass the bound. Dropping the rule
// would be a picture difference between two targets with no reason to differ,
// and on the 3DS an unbounded frame is an unbounded allocation.
static const int32_t kObjectStreamCapacity = 1 << 18;

// The C#'s ObjectView.Look: the palette and texture archives an arena's objects
// draw with (the level's own outside an arena).
struct ObjectLook {
    const formats::Palette* palette = nullptr;
    std::vector<const formats::TextureArchive*> archives;
};

// One surface's corners, in the model's order: their part, vertex and UV. Plus,
// per part, how many corners it contributes and where its first triangle comes
// in the model -- which is what the batch's place is when parts hide.
struct ObjectBatch {
    // The surface: the texture (or flat colour), its frame count, the pass.
    Surface surface;

    // A mod's replaced part's batch (Source::Mod): its corners' vertices
    // come from the swap's mesh through its fitted pose, not the model's
    // own pose. `image`, when set, is the surface's own picture (the GPU
    // texture is made from it at draw); the surface then only carries
    // the shading and pass.
    bool swapped = false;
    const HdImage* image = nullptr;

    // Per corner: the part its vertex comes from, and the vertex's index
    // within that part. `parts` and `indices` are the same length as `uvs`.
    std::vector<int32_t> parts;
    std::vector<int32_t> indices;
    std::vector<Vec2> uvs;

    // Per part of the model: corners contributed, and the sequence number of
    // its first triangle in this batch (kObjectUnseen when it has none).
    std::vector<int32_t> partCorners;
    std::vector<int32_t> firstTriangle;
};

// A model's batches, in the order of their first triangles, and the arrange
// scratch. `order` and `keys` are filled by arrangeObjectLayout, per draw.
struct ObjectLayout {
    std::vector<ObjectBatch> batches;
    std::vector<int32_t> order;
    std::vector<int32_t> keys;

    // The model's mod replacement, when one replaced a part (its fitted
    // poses are the layout's scratch, fitted per draw in addObject).
    const ModelSwap* swap = nullptr;
};

// Whether `part` is hidden by the `hidden` mask (the C#'s IsHidden). The shift
// count is masked to five bits because that is what the C# does: `1 << part`
// with part >= 32 is the same shift mod 32 there, and UB in C++ otherwise.
bool objectPartHidden(int32_t hidden, int32_t part);

// The C#'s ObjectView.Arrange: orders the batches that have a visible triangle
// by their first one (as a mesh built from the visible parts would be), into
// `layout->order`, and returns how many there are. `layout->keys` is its
// insertion-sort scratch.
int32_t arrangeObjectLayout(ObjectLayout* layout, int32_t hidden);

// The C#'s Visible(batch, hidden): the corners a batch draws with the mask
// applied. Zero means the batch draws nothing.
int32_t objectBatchCorners(const ObjectBatch& batch, int32_t hidden);

// The C#'s Shine: an explosion's surfaces shine unlit rather than being lit
// (their own point light would wash them out). In the original look nothing is
// Lit, so this is the identity there; it is ported because the C# is the oracle
// and the enhanced look reaches it.
Surface shineSurface(const Surface& surface, bool shines);

// Lays out a model with a look (the C#'s ObjectView.Build), returning false
// when it has no batch at all -- which the C# also reports, as a null layout.
//
// `look.archives` is searched in order for each material name, exactly as the
// arena scene's is. The pass is the C#'s Pass.DoubleSided: models show both
// faces.
bool buildObjectLayout(const formats::Model& model, const ObjectLook& look,
                       const MaterialResolver& resolver, ObjectLayout* out,
                       const ModelSwap* swap = nullptr);

// One batch's draw, as the object pass queues it (the C#'s DrawCommand, for the
// object pass). The surface is the FORMAT's surface, not a GPU one: the backend
// maps it to its own texture and palette, exactly as LevelView maps an arena
// batch's. `first` is the batch's offset in the frame's stream, which is what
// the C#'s Renderer.Stream hands back as `first`.
//
// A draw carries EVERYTHING the C#'s Draw call carries, and nothing that only
// the caller knows: the C# hands the renderer `Shine(material, glows)`, `frame`
// and `transform`, and its Material was made with the palette in it
// (`new Material(TextureId(texture), PaletteId(palette), ...)` in
// MaterialResolver.cs). A backend resource is keyed by the PAIR -- the same index
// texture is drawn through different palettes in different arenas, which is why
// the palette is part of the key on the 3DS and of the descriptor set on Vulkan
// (see game/level_view.h). So the palette is here too: without it a queued draw
// cannot say which colours its texture's indices mean.
struct ObjectStreamDraw {
    int32_t first = 0;
    int32_t corners = 0;

    // The surface with Shine already applied (an explosion's surfaces are
    // unlit), so the GPU half maps surfaces to textures without knowing why.
    Surface surface;

    // The palette the surface's texture indices are read through (the C#'s
    // Material.PaletteId). A queued draw always names one: a layout is keyed by the
    // palette, and a look without one lays out nothing.
    const formats::Palette* palette = nullptr;

    // A mod surface's own picture, when the surface has one: the GPU
    // half makes its texture from this and draws with no palette.
    const HdImage* image = nullptr;

    // The animated texture's frame (the C#'s Draw `frame` argument).
    int32_t frame = 0;

    // The object's transform (the C#'s Draw `world` argument). Row-major,
    // row-vector, like MeshDraw::world.
    Mat4 world = Mat4::identity();
};

// The frame's streamed corners and the draws they make: the C#'s
// `Renderer._stream` plus the draw queue, minus the GPU. One per frame, reset at
// its start (the C#'s QueueStream clears the stream the same way). The vectors
// keep their capacity between frames, so a frame after the first does not
// allocate (AGENTS.md 10: no per-frame container growth where the size is known).
//
// The C# sizes its stream once, for kObjectStreamCapacity vertices, and a batch
// that does not fit is skipped. This keeps that rule and drops the fixed size: it
// grows to the frame (which never reaches the bound in a shipped level) and
// refuses a batch that would pass it, exactly as Renderer.Stream does.
struct ObjectStream {
    std::vector<Vertex> vertices;
    std::vector<ObjectStreamDraw> draws;

    void reset()
    {
        vertices.clear();
        draws.clear();
    }
};

// The (model, palette) layout cache. The C# keeps it per ObjectView for the
// life of the level; clear() is what a level change calls.
class ObjectView {
public:
    // The resolver is BORROWED (the C#'s constructor parameter). It must
    // outlive this view.
    explicit ObjectView(const MaterialResolver& resolver) : resolver_(&resolver) {}

    // The C#'s LayoutOf: the model's layout, made on first use and kept.
    // Returns null for a model with nothing to draw (and remembers that too, so
    // it is not rebuilt every draw).
    ObjectLayout* layoutOf(const formats::Model* model, const ObjectLook& look);

    // The C#'s ObjectView.Draw, up to the point where the C# hands the corners to
    // the renderer: arrange the batches, copy the posed corners onto the end of
    // the frame's stream, and queue one draw per batch at its offset. `transform`
    // is the C#'s Draw argument (the C# passes FrameBlend.TransformOf, which is
    // obj.Transform with blending off).
    //
    // A batch that does not fit in what the frame's stream has left
    // (kObjectStreamCapacity) is refused and the cursor does not move, exactly as
    // the C#'s Renderer.Stream returns an empty span for one.
    //
    // Visibility is the CALLER's, as it is in the C#: Viewer.DrawObjects skips
    // the invisible objects and ObjectView.Draw never looks at Visible. The
    // caller walks the objects in the C#'s order, so the stream's offsets are
    // the oracle's.
    //
    // Returns false when the object draws nothing at all -- no model, dead, or a
    // model with no batch, which is the C#'s three early returns. A visible
    // object with a layout but no visible corner returns true and adds nothing.
    //
    // HOT PATH (AGENTS.md 2): this runs once per drawn object per frame and its
    // inner loop runs once per corner -- thousands of times a frame. It is
    // copies and integer indices only: no float->int conversion (R2), no float
    // comparison (R3), no division (the UV scale was taken once at layout time),
    // no transcendental (R6), no double (R4).
    bool addObject(const scripts::MdkObject& obj, const ObjectLook& look,
                   const Mat4& transform, ObjectStream* stream);

    // The C#'s Preload: lay a model out before it is drawn (a level's load).
    void preload(const formats::Model* model, const ObjectLook& look) { (void)layoutOf(model, look); }

    // The mods' model replacements (the C#'s ObjectView.Swaps; null:
    // none, the shipped behaviour). Set before the first layout.
    void setSwaps(ModModels* swaps) { swaps_ = swaps; }

    void clear() { layouts_.clear(); }

private:
    const MaterialResolver* resolver_;
    ModModels* swaps_ = nullptr;
    // A map, not a vector: layoutOf hands out a pointer into it, and a map's
    // elements do not move when later entries are inserted.
    std::map<std::pair<const formats::Model*, const formats::Palette*>,
             std::unique_ptr<ObjectLayout> > layouts_;
};

} // namespace mdk
