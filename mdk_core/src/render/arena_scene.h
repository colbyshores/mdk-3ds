#pragma once

// ============================================================================
// arena_scene.h — an arena's geometry, ready to draw.
//
// THE SHARED HALF OF THE LEVEL VIEW. The C#'s LevelView
// (src/Mdk.Game/Level/LevelView.cs) builds, for each arena, a vertex buffer
// and a list of batches -- runs of triangles sharing a surface.
//
// This is the same thing, minus the parts that are the ENHANCED look's
// business: the triangle groups, the depth layers, the outlines and the
// torn pieces. Those need the game logic (the scripts move the groups), so
// they land with it.
//
// WHY IT IS SHARED. The vertices and the batches are the same on both
// backends; only the GPU objects differ. A backend that built its own
// vertices would eventually draw a different picture, which the whole
// design forbids (SPEC.md 5.3).
//
// THE BATCHES ARE CONTIGUOUS RUNS OF THE SAME MATERIAL VALUE. The C# sorts
// the triangles by surface and by the depth layers, which needs the game's
// state; the unsorted run is correct, just not optimally batched. The
// sort lands with the game logic.
// ============================================================================

#include <stdint.h>

#include <vector>

#include "formats/arena.h"
#include "formats/palette.h"
#include "formats/texture_archive.h"
#include "render/material_resolver.h"
#include "render/render_types.h"

namespace mdk {

// A run of triangles sharing a surface.
struct ArenaBatch {
    // The first VERTEX of the run. The count is in vertices, so a run of
    // triangles is a multiple of three.
    int32_t first = 0;
    int32_t count = 0;
    Surface surface;
};

// An arena's geometry.
struct ArenaScene {
    std::vector<Vertex> vertices;
    std::vector<ArenaBatch> batches;

    void clear() {
        vertices.clear();
        batches.clear();
    }
};

// Build the scene for an arena.
//
// `archives` is the list of texture archives searched for the material
// names: the arena's own, the level's, then every other arena's.
//
// Returns false when the arena has no geometry. The scene is cleared
// first, so a failed build leaves nothing half-drawn.
bool buildArenaScene(const formats::Arena* arena,
                     const MaterialResolver& resolver,
                     const formats::Palette& palette,
                     const std::vector<const formats::TextureArchive*>& archives,
                     ArenaScene* out);

} // namespace mdk
