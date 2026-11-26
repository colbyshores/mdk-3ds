#pragma once

// ============================================================================
// fall_models.h — the fall's models (FALL3D.BNI, without the arena models' flags
// word) in the textures of FALL3D_n.MTI and the FALLPn palette, both faces drawn.
//
// THE C# ORIGINAL is src/Mdk.Game/Fall/FallModels.cs: a mesh per pose (model,
// animation, frame) with a batch per surface (a texture of the MTI, or a palette
// colour: the negative values, the MTI's colours, PEN_n), drawn at a world
// matrix. The surfaces, their order, the UVs and the materials are the C#'s.
//
//   (model, animation, frame) ──► mesh + batches per surface ──(world matrix)──► renderer
//
// WHAT IS BUILT WHEN, AND WHY IT DIFFERS FROM THE C#. The C# builds a static
// mesh the first time each pose is drawn and keeps it. KURTANIM alone is 199
// poses of a 363-triangle Kurt, ~5 MB of vertex buffers once he has played
// through, which the 3DS's linear heap is not for, and a first-draw build is a
// main-thread stall on the console. So:
//   * the models at rest (the missile, the chute, the explosion, the fall's
//     pickups) are built once, at load, as static meshes -- the C#'s meshes;
//   * an ANIMATED model (Kurt, Bones) has one dynamic mesh, and a draw writes the
//     pose's triangles into it (C# Build's arithmetic, per frame: ~1100
//     vertices). Its animations are baked at load (ModelAnimation.Bake), as the
//     C# bakes them on first use.
// A model is drawn at most once a frame in a given pose this way; the fall
// draws one Kurt and one Bones.
//
// TEXTURES TALLER THAN THE BACKEND TAKES. EXPLODE stacks 26 frames of 128² --
// 3328 rows, which the 3DS (1024 at most) refuses. When an animated texture is
// taller than `mdk_render_capabilities().max_texture_size`, its frames become a
// texture each and a draw picks the frame's texture instead of the frame's rows.
// The desktop keeps the C#'s stacked texture.
//
// The flat colours are the palette's BASE colours: the palette's effects are
// drawn over the frame (fall_palette.h).
//
// Cold: AGENTS.md §2. No virtuals, no templates, no exceptions (AGENTS.md §10).
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "formats/model.h"
#include "formats/model_animation.h"
#include "render/render.h"
#include "render/render_types.h"
#include "util/matrix.h"

namespace mdk {

namespace formats {
class Bni;
class Texture;
class TextureArchive;
}

namespace fall {

class FallModels {
public:
    FallModels() = default;
    FallModels(const FallModels&) = delete;
    FallModels& operator=(const FallModels&) = delete;

    // Loads the models the fall draws: `still` at rest, and `animated[i]` with
    // the animations `animations[i]` (a model and its animations, e.g. KURT with
    // KURTANIM and KURT_HIT). `paletteRgba` is FALLPn's base colours (the 3DS
    // expands the textures through it). The BNI and the MTI must outlive this.
    // False when a model the list names does not parse.
    bool load(MdkRenderer renderer, const formats::Bni& bni, const formats::TextureArchive& mti,
              int32_t palette, const uint8_t* paletteRgba, const std::vector<std::string>& still,
              const std::vector<std::string>& animated,
              const std::vector<std::vector<std::string>>& animations);

    // FallModels.Draw: a model at rest (`animation` null or empty) or in a frame
    // of an animation; `textureFrame` picks an animated texture's frame, `pass`
    // how it meets the scene. A model that was not loaded draws nothing.
    void draw(MdkRenderer renderer, const char* name, const Mat4& world, const char* animation,
              int32_t frame, int32_t textureFrame, Pass pass);

    // FallModels.Find: a loaded model of FALL3D.BNI (its data: the trails' ends),
    // or null.
    const formats::Model* find(const char* name) const;

private:
    // FallModels.Batch: triangles of a surface; `pen` is a flat colour's palette
    // index (-1 for a texture).
    struct Batch {
        int32_t first = 0;
        int32_t count = 0;
        Material material;
        int32_t pen = -1;
        int32_t surface = -1;   // the textured surface's index in surfaces_, or -1
    };
    // A textured surface: the texture's id, or its frames' ids when split.
    struct Surface {
        const formats::Texture* texture = nullptr;
        int32_t id = -1;
        std::vector<int32_t> frames;
    };
    struct Baked {
        std::string name;
        std::vector<std::vector<std::vector<formats::Vec3f>>> frames;
    };
    struct Entry {
        std::string name;
        formats::Model model;
        bool animated = false;
        int32_t mesh = -1;
        std::vector<Batch> batches;   // the rest pose's (static), or the last drawn (animated)
        std::vector<Baked> baked;
        int32_t capacity = 0;
    };

    bool add(const std::string& name, bool animated, const std::vector<std::string>* animations);
    // FallModels.Build's grouping: the pose's triangles by surface, in the C#'s
    // order (a surface's batch where it first appears).
    void build(const formats::Model& model, const std::vector<std::vector<formats::Vec3f>>& pose,
               std::vector<Vertex>* vertices, std::vector<Batch>* batches);
    // FallModels.Resolve / Colour: a triangle's surface; false when not drawn.
    bool resolve(int32_t value, const formats::Model& model, Batch* out);
    bool colour(int32_t index, Batch* out);
    int32_t surfaceOf(const formats::Texture* texture);

    MdkRenderer renderer_ = nullptr;
    const formats::Bni* bni_ = nullptr;
    const formats::TextureArchive* mti_ = nullptr;
    int32_t palette_ = -1;
    const uint8_t* paletteRgba_ = nullptr;
    int32_t maxTexture_ = 1024;
    std::vector<Entry> entries_;
    std::vector<Surface> surfaces_;
    // The build's scratch, reserved once (no per-frame growth past the largest pose).
    std::vector<Vertex> scratch_;
    std::vector<Vertex> sorted_;
    std::vector<std::vector<Vertex>> groups_;
};

} // namespace fall
} // namespace mdk
