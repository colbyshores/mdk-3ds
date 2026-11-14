#pragma once

// ============================================================================
// level_view.h — an arena on the GPU.
//
// The shared half of the C#'s LevelView (src/Mdk.Game/Level/LevelView.cs).
//
// It turns an arena into the GPU objects the renderer draws:
//
//   the mesh        the arena's vertices, in one buffer
//   the textures    one per distinct texture, through the arena's palette
//   the batches     the runs of triangles sharing a surface
//
// and it draws them.
//
// THE GPU OBJECTS ARE THE BACKEND'S, BUT THE IDENTITY IS NOT. The textures
// are keyed by the format's Texture object, not by a name, because two
// arenas can hold textures with the same name and different pixels -- and
// the SAME texture is drawn through different palettes in different
// arenas, which is why the palette is part of the key on the 3DS. That
// mapping is not this file's private business either: the object pass draws
// surfaces through the same palettes, so both ask one SurfaceCache
// (render/surface_cache.h), as the C# does with one MaterialResolver.
//
// THE FRAME INDEX of an animated texture changes per frame, and it is
// applied by the BACKEND's shader, not by rebuilding the mesh: the C#'s
// palette shader does `uv.y = (frac(uv.y) + frame) / frame_count`.
// ============================================================================

#include <stdint.h>

#include <map>
#include <vector>

#include "formats/arena.h"
#include "formats/palette.h"
#include "formats/texture.h"
#include "game/level_data.h"
#include "render/arena_scene.h"
#include "render/material_resolver.h"
#include "render/render.h"
#include "render/surface_cache.h"

namespace mdk {

// An arena on the GPU.
class LevelView {
public:
    LevelView() = default;

    // Build the view for an arena. Returns false when the arena has no
    // geometry, or when its GPU objects could not be made.
    //
    // `surfaces` is the frame's shared texture/palette cache and is BORROWED:
    // the C# gives the arena view and the object view the SAME resolver, so an
    // arena and an object using one texture through one palette are one GPU
    // texture (see render/surface_cache.h for why that is a memory number on the
    // 3DS and not a tidiness one). It must outlive this view.
    bool load(MdkRenderer renderer, LevelData* level, const formats::Arena* arena,
              SurfaceCache* surfaces);

    // The look the arena's surfaces are shaded with (the C#'s
    // EnhancedLook.Surfaces for the settings' graphics): Lit under the
    // enhanced look, Original otherwise. The frontend sets it before
    // load; refresh() keeps it.
    void setShading(Shading shading) { shading_ = shading; }

    // Draw the arena. The camera and the lighting are the caller's.
    //
    // A group the scripts hid mid-play (group_set_state) is skipped here, per
    // batch, with no rebuild -- the C# LevelView checks Hidden at draw time
    // (LevelView.cs:126). A group the scripts RETEXTURED (group_set_texture)
    // needs refresh() first: its surface is baked into the batches.
    void draw();

    // Rebuild the scene from the arena's CURRENT group materials and re-upload
    // the mesh. This is the C# LevelView.Rebuild (LevelView.cs:251), the path a
    // group_set_texture takes: the group's triangles take the new material, so
    // their surface (and UV scale) changes and the mesh is built again. Call it
    // after a material change, between frames; a hide/show needs no refresh
    // because draw() reads the hidden state live.
    bool refresh();

    // The scene, for the tests and for the diagnostics.
    const ArenaScene& scene() const { return scene_; }
    int32_t batchCount() const { return (int32_t)scene_.batches.size(); }
    int32_t vertexCount() const { return (int32_t)scene_.vertices.size(); }

    // The arena this view is for.
    const formats::Arena* arena() const { return arena_; }

    // The arena's bounds, in the game's units. False when the arena has no
    // geometry.
    bool bounds(Vec3* outMin, Vec3* outMax) const;

private:
    MdkRenderer renderer_ = nullptr;
    LevelData* level_ = nullptr;
    const formats::Arena* arena_ = nullptr;
    ArenaScene scene_;

    SurfaceCache* surfaces_ = nullptr;

    Shading shading_ = Shading::Original;

    MdkMeshId mesh_ = -1;

    // The arena's palette, borrowed from LevelData (whose map keeps it stable).
    // It is not copied: the object pass draws the same arena's objects through
    // the same palette, and the cache keys on this pointer so the two share one
    // GPU palette and one set of expanded textures.
    const formats::Palette* palette_ = nullptr;
};

} // namespace mdk
