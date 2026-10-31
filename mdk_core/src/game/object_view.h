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
// cache and not a per-frame build. The posed vertices are NOT here: they depend
// on the object's animation frame and hidden parts, and they are written into
// the renderer's vertex stream per draw.
//
// WHAT IS NOT PORTED, and why:
//
//   * The DRAW stage (ObjectView.Draw: arrange the batches, copy the posed
//     corners into the renderer's stream, draw each batch with the object's
//     transform). It needs the seam's per-frame vertex stream, which neither
//     backend has yet; the batch that adds it is the next one. Everything the
//     draw stage reads is here already: the layout, arrange's order, the corner
//     count per batch, MdkObject::pose_parts() and MdkObject::transform().
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

namespace mdk {

namespace formats {
class Model;
class Palette;
class TextureArchive;
} // namespace formats

// A batch's place when no part of it has a visible triangle (the C#'s Unseen).
static const int32_t kObjectUnseen = 2147483647; // int.MaxValue

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
                       const MaterialResolver& resolver, ObjectLayout* out);

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

    // The C#'s Preload: lay a model out before it is drawn (a level's load).
    void preload(const formats::Model* model, const ObjectLook& look) { (void)layoutOf(model, look); }

    void clear() { layouts_.clear(); }

private:
    const MaterialResolver* resolver_;
    // A map, not a vector: layoutOf hands out a pointer into it, and a map's
    // elements do not move when later entries are inserted.
    std::map<std::pair<const formats::Model*, const formats::Palette*>,
             std::unique_ptr<ObjectLayout> > layouts_;
};

} // namespace mdk
