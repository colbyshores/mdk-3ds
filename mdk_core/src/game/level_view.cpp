// ============================================================================
// level_view.cpp — see level_view.h.
// ============================================================================

#include "game/level_view.h"

#include <string.h>

namespace mdk {

bool LevelView::load(MdkRenderer renderer, LevelData* level, const formats::Arena* arena,
                     SurfaceCache* surfaces) {
    if (!renderer || !level || !arena || !surfaces) {
        return false;
    }
    renderer_ = renderer;
    level_ = level;
    arena_ = arena;
    surfaces_ = surfaces;
    // A fresh scene is whole: an end of a level in progress tears it again from the
    // first torn triangle on the next tear() (the C#'s view is never rebuilt mid-end).
    tornShown_ = 0;
    dynamic_ = false;
    triangleFirsts_.clear();
    pumpAt_ = 0;

    palette_ = &level->paletteOf(arena);
    const std::vector<const formats::TextureArchive*> archives = level->archivesOf(arena);

    MaterialResolver resolver(&level->dti(), shading_);
    if (!buildArenaScene(arena, resolver, *palette_, archives, &scene_)) {
        return false;
    }

    // The mesh. The vertices are copied into the backend's buffer.
    mesh_ = mdk_render_create_mesh(renderer, scene_.vertices.data(),
                                   (int32_t)scene_.vertices.size());
    if (mesh_ < 0) {
        return false;
    }

    // The palette and the textures are made by the shared cache when they are
    // first drawn, not here: an arena that is loaded and never drawn costs
    // nothing, and the C#'s resolver makes them as surfaces resolve too.
    return true;
}

void LevelView::unload() {
    if (renderer_ && mesh_ >= 0) {
        mdk_render_release_mesh(renderer_, mesh_);
    }
    mesh_ = -1;
    scene_ = ArenaScene();
    arena_ = nullptr;
    palette_ = nullptr;
    tornShown_ = 0;
    dynamic_ = false;
    triangleFirsts_.clear();
    pumpAt_ = 0;
}

bool LevelView::pumpSurfaces(int32_t budget) {
    if (!renderer_ || surfaces_ == nullptr || !arena_) {
        return true;
    }
    for (int32_t i = 0; i < budget && pumpAt_ < scene_.batches.size(); i++) {
        const ArenaBatch& batch = scene_.batches[pumpAt_];
        // The same skip draw() makes: a group the scripts hid is not drawn, so
        // its texture is left to the frame that shows it again.
        if (arenaBatchVisible(arena_, batch)) {
            // A negative id (the backend refused) is left to draw() to ask
            // again: a refused texture is CACHED as refused
            // (surface_cache.cpp), so pinning the cursor here would block every
            // batch behind it for the level's life.
            surfaces_->surfaceId(batch.surface, palette_);
        }
        pumpAt_++;
    }
    return pumpAt_ >= scene_.batches.size();
}

bool LevelView::tear(const std::vector<int32_t>& torn) {
    if (!renderer_ || !arena_ || mesh_ < 0 || (int32_t)torn.size() <= tornShown_) {
        return true;
    }
    if (triangleFirsts_.empty()) {
        arenaSceneTriangleFirsts(arena_, &triangleFirsts_);
    }
    // C# Tear: each newly torn triangle's corners 1 and 2 become its corner 0.
    for (; tornShown_ < (int32_t)torn.size(); tornShown_++) {
        const int32_t t = torn[(size_t)tornShown_];
        if (t < 0 || (size_t)t >= triangleFirsts_.size()) {
            continue;
        }
        const int32_t first = triangleFirsts_[(size_t)t];
        if (first < 0 || (size_t)first + 2 >= scene_.vertices.size()) {
            continue;
        }
        scene_.vertices[(size_t)first + 1] = scene_.vertices[(size_t)first];
        scene_.vertices[(size_t)first + 2] = scene_.vertices[(size_t)first];
    }
    if (dynamic_) {
        return true;
    }
    // The first tear: the static mesh cannot be rewritten (a frame in flight may read
    // it), so a dynamic one of the same size takes its place and draw() fills it. The
    // static one is released at the backend's completion boundary.
    const MdkMeshId dynamicMesh =
        mdk_render_create_dynamic_mesh(renderer_, (int32_t)scene_.vertices.size());
    if (dynamicMesh < 0) {
        tornShown_ = 0;  // nothing shown torn yet: the next call collapses them all again
        return false;
    }
    mdk_render_release_mesh(renderer_, mesh_);
    mesh_ = dynamicMesh;
    dynamic_ = true;
    return true;
}

bool LevelView::bounds(Vec3* outMin, Vec3* outMax) const {
    if (!arena_ || outMin == nullptr || outMax == nullptr) {
        return false;
    }
    const std::vector<formats::Vec3f>& positions = arena_->vertices();
    if (positions.empty()) {
        return false;
    }
    Vec3 min = Vec3{positions[0].x, positions[0].y, positions[0].z};
    Vec3 max = min;
    for (size_t i = 1; i < positions.size(); i++) {
        if (positions[i].x < min.x) min.x = positions[i].x;
        if (positions[i].y < min.y) min.y = positions[i].y;
        if (positions[i].z < min.z) min.z = positions[i].z;
        if (positions[i].x > max.x) max.x = positions[i].x;
        if (positions[i].y > max.y) max.y = positions[i].y;
        if (positions[i].z > max.z) max.z = positions[i].z;
    }
    *outMin = min;
    *outMax = max;
    return true;
}

void LevelView::draw() {
    if (!renderer_ || mesh_ < 0 || surfaces_ == nullptr) {
        return;
    }
    // A torn arena (tear) draws from its dynamic mesh, which is written in every frame
    // that draws it: a dynamic mesh's slot holds only what its own frame wrote.
    if (dynamic_ && !scene_.vertices.empty()) {
        mdk_render_update_mesh(renderer_, mesh_, scene_.vertices.data(),
                               (int32_t)scene_.vertices.size());
    }
    for (size_t i = 0; i < scene_.batches.size(); i++) {
        const ArenaBatch& batch = scene_.batches[i];
        // A group the scripts hid mid-play is skipped here, not rebuilt away --
        // the C# LevelView checks Hidden per group at draw time (LevelView.cs:126).
        if (!arenaBatchVisible(arena_, batch)) {
            continue;
        }
        mdk::MeshDraw draw;
        draw.mesh = mesh_;
        draw.first_vertex = batch.first;
        draw.vertex_count = batch.count;
        draw.frame = 0;
        draw.primitive = Primitive::Triangles;
        memset(draw.world, 0, sizeof(draw.world));
        draw.world[0] = 1.0f;
        draw.world[5] = 1.0f;
        draw.world[10] = 1.0f;
        draw.world[15] = 1.0f;

        // The texture and the palette come from the shared cache, keyed by the
        // format's objects: an object drawn with this arena's palette reuses
        // these textures instead of making its own (render/surface_cache.h).
        draw.material.texture = surfaces_->surfaceId(batch.surface, palette_);
        draw.material.palette = surfaces_->paletteId(palette_);
        draw.material.frame_count = batch.surface.frameCount;
        draw.material.pass = batch.surface.pass;
        draw.material.shading = batch.surface.shading;
        draw.material.row_shift = batch.surface.rowShift;
        draw.material.depth_layer = batch.depthLayer;
        // The colour is applied EXACTLY ONCE. A flat surface's colour is
        // already carried by the one-pixel texture above (that is why the
        // flat texture exists -- the PICA200 cannot take a constant), so
        // the material colour must be white for both targets. Setting it to
        // the surface colour here squared the flat colours -- (19,11,11)
        // rendered as (1,0,0).
        draw.material.colour = Colour{1.0f, 1.0f, 1.0f, 1.0f};

        mdk_render_draw_mesh(renderer_, &draw);
    }
}


bool LevelView::refresh() {
    if (!renderer_ || !level_ || !arena_ || surfaces_ == nullptr) {
        return false;
    }
    // The C# LevelView.Rebuild (LevelView.cs:251): a group_set_texture changes a
    // group's material, so its surface and UV scale change and the mesh is built
    // again. The whole arena is rebuilt (it is one mesh), which is the same
    // picture the C#'s per-group rebuild produces. A hide/show does NOT come here
    // -- draw() reads the hidden state live, so it needs no rebuild.
    MaterialResolver resolver(&level_->dti(), shading_);
    const std::vector<const formats::TextureArchive*> archives = level_->archivesOf(arena_);
    ArenaScene rebuilt;
    buildArenaScene(arena_, resolver, *palette_, archives, &rebuilt);

    scene_.vertices.swap(rebuilt.vertices);
    scene_.batches.swap(rebuilt.batches);
    // The rebuilt scene is whole and its mesh static again: an end of a level in
    // progress tears it again on the next tear().
    tornShown_ = 0;
    dynamic_ = false;
    triangleFirsts_.clear();

    // The mesh is rebuilt. The C# creates a new mesh on Rebuild and lets the old
    // one live until the level ends; here the old GPU buffer is released first.
    if (mesh_ >= 0) {
        mdk_render_release_mesh(renderer_, mesh_);
        mesh_ = -1;
    }
    if (!scene_.vertices.empty()) {
        mesh_ = mdk_render_create_mesh(renderer_, scene_.vertices.data(),
                                       (int32_t)scene_.vertices.size());
    }
    return mesh_ >= 0 || scene_.vertices.empty();
}

} // namespace mdk
