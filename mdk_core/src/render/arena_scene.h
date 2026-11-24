#pragma once

// ============================================================================
// arena_scene.h — an arena's geometry, ready to draw.
//
// THE SHARED HALF OF THE LEVEL VIEW. The C#'s LevelView
// (src/Mdk.Game/Level/LevelView.cs) builds, for each arena, a vertex buffer
// and a list of batches -- runs of triangles sharing a surface.
//
// This is the same thing, minus the parts that are the ENHANCED look's
// business: the triangle groups, the outlines and the torn pieces. Those need
// the game logic (the scripts move the groups), so they land with it.
//
// The arena's DEPTH LAYERS are here: they decide which of two coplanar
// surfaces is a detail over the other, and the renderer pulls the detail
// towards the eye so the two do not fight for the same depth.
//
// WHY IT IS SHARED. The vertices and the batches are the same on both
// backends; only the GPU objects differ. A backend that built its own
// vertices would eventually draw a different picture, which the whole
// design forbids (SPEC.md 5.3).
//
// THE BATCHES ARE CONTIGUOUS RUNS OF THE SAME MATERIAL VALUE, IN THE C#'S
// SORTED ORDER: the kept triangles are stably sorted by (group, material
// value, depth layer) at build, so every run the builder emits is already
// the merged batch a draw wants (the port's earlier unsorted emission was
// correct, just not optimally batched: ~135 batches for an arena the
// sorted build makes ~21 of). Stability keeps triangles equal on the
// whole key in their original order, so a coplanar tie draws as before.
// ============================================================================

#include <stdint.h>

#include <map>
#include <tuple>

#include <vector>

#include "formats/arena.h"
#include "formats/palette.h"
#include "formats/texture_archive.h"
#include "render/material_resolver.h"
#include "render/render_types.h"

namespace mdk {

// A run of triangles sharing a surface AND a group.
struct ArenaBatch {
    // The first VERTEX of the run. The count is in vertices, so a run of
    // triangles is a multiple of three.
    int32_t first = 0;
    int32_t count = 0;
    Surface surface;
    // The triangle group (the top byte of the triangle flags) every triangle
    // in this run belongs to. A run never spans two groups: the build breaks a
    // run at a group boundary as well as a material change, so the drawing can
    // skip a whole group the scripts hid mid-play (arenaBatchVisible) without
    // rebuilding the mesh -- the C# LevelView checks Hidden per group at draw
    // time (LevelView.cs:126), and this is the same decision per batch.
    int32_t group = 0;

    // The triangle's depth layer (the C# Levels.Of). A detail over a larger
    // surface in the same plane is one layer above it.
    int32_t depthLayer = 0;

    // The run's world-space bounding box, computed when the run is built.
    // The backend culls the batch per eye against the camera frustum
    // (render_types.h MeshDraw bounds) -- geometry behind the camera costs
    // the PICA200's vertex pipeline nothing only if someone tells it to skip,
    // and the original's BSP did that telling (the port draws flat batches).
    float boundsMin[3] = {0.0f, 0.0f, 0.0f};
    float boundsMax[3] = {0.0f, 0.0f, 0.0f};
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

// The depth-layer computation in resumable steps: computeArenaDepthLayers
// is this builder run to completion, with the same output. step() spends
// at most `budget` work units (a triangle collected, or one overlap check
// in the per-plane stacking) and returns true when the layers are done.
class DepthLayersBuilder {
public:
    // Not copyable: the layers pointer names the caller's vector (and the
    // owning builder's own member), so a copy would write into another
    // object's storage. Reset in place instead of assigning a fresh one.
    DepthLayersBuilder() = default;
    DepthLayersBuilder(const DepthLayersBuilder&) = delete;
    DepthLayersBuilder& operator=(const DepthLayersBuilder&) = delete;
    void reset() {
        planes_.clear();
        assignValid_ = false;
        nextTriangle_ = 0;
        planeSorted_ = false;
        assignI_ = 1;
        assignJ_ = 0;
        vertices_ = nullptr;
        triangleIndices_ = nullptr;
        layers_ = nullptr;
        phase_ = Phase::Idle;
    }
    bool begin(const std::vector<formats::Vec3f>* vertices,
               const std::vector<int32_t>* triangleIndices, std::vector<int32_t>* layers);
    bool step(int32_t budget);
    bool done() const { return phase_ == Phase::Done; }
    bool failed() const { return phase_ == Phase::Failed; }

private:
    struct Entry {
        int32_t triangle = 0;
        float area = 0.0f;
        formats::Vec3f normal;
    };
    typedef std::tuple<int32_t, int32_t, int32_t, int32_t> PlaneKey;
    enum class Phase : uint8_t { Idle, Collect, Assign, Done, Failed };

    const std::vector<formats::Vec3f>* vertices_ = nullptr;
    const std::vector<int32_t>* triangleIndices_ = nullptr;
    std::vector<int32_t>* layers_ = nullptr;
    std::map<PlaneKey, std::vector<Entry>> planes_;
    Phase phase_ = Phase::Idle;
    int32_t nextTriangle_ = 0;
    // The Assign cursor is a KEY, re-found in planes_ at every step --
    // never an iterator into the builder's own map, so copying or
    // assigning a builder cannot leave a cursor aliasing another
    // builder's storage.
    PlaneKey assignKey_;
    bool assignValid_ = false;
    bool planeSorted_ = false;
    size_t assignI_ = 1;
    size_t assignJ_ = 0;
};

// The scene build in resumable steps (the door pump): buildArenaScene is
// this builder run to completion, so a stepped build produces the same
// scene, triangle for triangle. The caller keeps the arena, the resolver,
// the palette and the archives alive until the build finishes or is
// dropped. step() advances at most maxTriangles of the main loop (the
// setup -- depth layers and the sorted order -- is its own first step)
// and returns true when the scene is complete; failed() says the build
// gave up (no geometry), which buildArenaScene reports as false.
class ArenaSceneBuilder {
public:
    enum class Failure : uint8_t { None, InvalidData, EmptyScene };
    // Not copyable (its depth builder points into this builder's own
    // depthLayers_): reset in place instead of assigning a fresh one.
    ArenaSceneBuilder() = default;
    ArenaSceneBuilder(const ArenaSceneBuilder&) = delete;
    ArenaSceneBuilder& operator=(const ArenaSceneBuilder&) = delete;
    void reset() {
        failure_ = Failure::None;
        depth_.reset();
        scene_.clear();
        depthLayers_.clear();
        order_.clear();
        oi_ = 0;
        surfaces_.clear();
        drawn_.clear();
        scaleX_.clear();
        scaleY_.clear();
        haveRun_ = false;
        arena_ = nullptr;
        resolver_ = nullptr;
        palette_ = nullptr;
        archives_.clear();
        phase_ = Phase::Idle;
    }
    bool begin(const formats::Arena* arena, const MaterialResolver* resolver,
               const formats::Palette* palette,
               const std::vector<const formats::TextureArchive*>& archives);
    bool step(int32_t maxTriangles);
    bool done() const { return phase_ == Phase::Done; }
    bool failed() const { return phase_ == Phase::Failed; }
    Failure failure() const { return failure_; }
    ArenaScene& scene() { return scene_; }

private:
    enum class Phase : uint8_t { Idle, Setup, Loop, Done, Failed };
    void finish();

    const formats::Arena* arena_ = nullptr;
    const MaterialResolver* resolver_ = nullptr;
    const formats::Palette* palette_ = nullptr;
    std::vector<const formats::TextureArchive*> archives_;
    ArenaScene scene_;
    Phase phase_ = Phase::Idle;
    Failure failure_ = Failure::None;
    DepthLayersBuilder depth_;
    std::vector<int32_t> depthLayers_;
    std::vector<int32_t> order_;
    size_t oi_ = 0;
    std::map<int32_t, Surface> surfaces_;
    std::map<int32_t, bool> drawn_;
    std::map<int32_t, float> scaleX_;
    std::map<int32_t, float> scaleY_;
    int32_t runMaterial_ = 0;
    int32_t runGroup_ = 0;
    int32_t runDepthLayer_ = 0;
    bool haveRun_ = false;
    float runMin_[3] = {0.0f, 0.0f, 0.0f};
    float runMax_[3] = {0.0f, 0.0f, 0.0f};
};

// Whether buildArenaScene leaves a triangle out (the C# LevelView.Build's skip,
// LevelView.cs:294: the clip flag's or the Disabled flag's triangles). The one rule
// both functions here use.
bool arenaSceneSkips(const formats::Arena* arena, int32_t triangle);

// The first vertex in buildArenaScene's vertex list of every triangle, -1 for a
// skipped one (the C# LevelView's Placements[t].First). Computed on demand -- only
// the end of a level (LevelView::tear) needs it -- so a scene carries no per-triangle
// table it never reads.
bool arenaSceneTriangleFirsts(const formats::Arena* arena, std::vector<int32_t>* out);

// Whether a batch is drawn right now: its group is not Hidden. This is the
// draw-time check the C# LevelView makes per group (LevelView.cs:126) -- a
// group the scripts hid with group_set_state disappears with no mesh rebuild,
// because the geometry is still in the buffer and only the draw is skipped.
// Group 0 (the rest of the arena) never changes and is always drawn.
bool arenaBatchVisible(const formats::Arena* arena, const ArenaBatch& batch);

// Computes the depth layer of every triangle in `triangleIndices`.
//
// This is the shared geometry half of the C# Layers.Of
// (src/Mdk.Game/Level/Layers.cs): triangles on the same plane are stacked
// from the biggest outwards, and a smaller triangle that overlaps a bigger
// one is one layer above it. `vertices` and `triangleIndices` use the
// formats' own vector type; the returned vector has one layer per triangle.
//
// Returns false when the output pointer is null or the index list does not
// contain three indices per triangle.
bool computeArenaDepthLayers(const std::vector<formats::Vec3f>& vertices,
                             const std::vector<int32_t>& triangleIndices,
                             std::vector<int32_t>* layers);

} // namespace mdk
