// ============================================================================
// level_view.cpp — see level_view.h.
// ============================================================================

#include "game/level_view.h"

#include <math.h>
#include <string.h>

#include "scripts/animated_textures.h"

namespace mdk {

// The stacked texels a batch's texture will be made with -- the same
// product surface_cache.cpp's textureId creates by (width x height x
// frames) and the 3DS backend chunks above (its kChunkMinTexels). One
// expression, so the pump's idea of "large" cannot drift from the make's.
static int64_t surfaceTexels(const ArenaBatch& batch) {
    const formats::Texture* texture = batch.surface.texture;
    if (!texture) {
        return 0;
    }
    return (int64_t)texture->width() * (int64_t)texture->height() *
           (int64_t)texture->frameCount();
}

static const int64_t kLargeSurfaceTexels = 32768;

// The re-ask pass's batches per call (see pumpSurfaces): a cache lookup and a
// backend call each, no make, so it does not spend the caller's budget.
static const int32_t kReaskPerCall = 8;

// Start every large surface of a freshly built scene (see pumpSurfaces for
// the size rule and the measured 64x704 case). At load this is the whole
// point: the backend's sliced expansion then has every frame until the
// arena is drawn, instead of the pump's sequential walk reaching a late
// batch after its first draw already needed it.
static void startLargeSurfaces(const ArenaScene& scene, SurfaceCache* surfaces,
                               const formats::Palette* palette) {
    for (size_t i = 0; i < scene.batches.size(); i++) {
        if (surfaceTexels(scene.batches[i]) >= kLargeSurfaceTexels) {
            surfaces->prepare(scene.batches[i].surface, palette);
        }
    }
}

bool LevelView::load(MdkRenderer renderer, LevelData* level, const formats::Arena* arena,
                     SurfaceCache* surfaces) {
    if (!loadBegin(renderer, level, arena, surfaces)) {
        return false;
    }
    while (!buildStep()) {
        if (buildState_ == BuildState::Failed) {
            return false;
        }
    }
    return true;
}

bool LevelView::loadBegin(MdkRenderer renderer, LevelData* level, const formats::Arena* arena,
                          SurfaceCache* surfaces) {
    if (!renderer || !level || !arena || !surfaces) {
        delete buildResolver_;
        buildResolver_ = nullptr;
        buildFailure_ = LevelViewBuildFailure::PermanentData;
        buildState_ = BuildState::Failed;
        return false;
    }
    renderer_ = renderer;
    level_ = level;
    arena_ = arena;
    surfaces_ = surfaces;
    pendingRefresh_ = false;
    // A fresh scene is whole: an end of a level in progress tears it again from the
    // first torn triangle on the next tear() (the C#'s view is never rebuilt mid-end).
    tornShown_ = 0;
    dynamic_ = false;
    triangleFirsts_.clear();
    pumpAt_ = 0;
    pumpLargeAt_ = 0;
    pumpReaskAt_ = 0;

    palette_ = &level->paletteOf(arena);
    const std::vector<const formats::TextureArchive*> archives = level->archivesOf(arena);
    buildFailure_ = LevelViewBuildFailure::None;

    delete buildResolver_;
    buildResolver_ = new MaterialResolver(&level->dti(), shading_);
    if (!builder_.begin(arena, buildResolver_, palette_, archives)) {
        delete buildResolver_;
        buildResolver_ = nullptr;
        buildFailure_ = builder_.failure() == ArenaSceneBuilder::Failure::EmptyScene
                            ? LevelViewBuildFailure::SceneEmpty
                            : LevelViewBuildFailure::PermanentData;
        buildState_ = BuildState::Failed;
        return false;
    }
    buildState_ = BuildState::Scene;
    return true;
}

bool LevelView::buildStep() {
    // Triangles a step. Measured on the Old 3DS (L7 walk, DANT_2's build at
    // the corridor crossing): 1200 were ~7 ms a step. Small steps (~1 ms) so
    // the frontend's time budget (ArenaViews::stepBuild) bounds a frame's
    // share: one 600-triangle step a frame left LEVEL4's MEAT_5 build 15 frames
    // over 33 ms on its approach (2026-11-27).
    const int32_t kBuildTrianglesPerStep = 150;
    switch (buildState_) {
        case BuildState::Scene:
            if (builder_.step(kBuildTrianglesPerStep)) {
                scene_.vertices.swap(builder_.scene().vertices);
                scene_.batches.swap(builder_.scene().batches);
                buildState_ = BuildState::Mesh;
            } else if (builder_.failed()) {
                delete buildResolver_;
                buildResolver_ = nullptr;
                buildFailure_ = builder_.failure() == ArenaSceneBuilder::Failure::EmptyScene
                                    ? LevelViewBuildFailure::SceneEmpty
                                    : LevelViewBuildFailure::PermanentData;
                buildState_ = BuildState::Failed;
                return false;
            }
            return false;
        case BuildState::Mesh:
            // The mesh. The vertices are copied into the backend's buffer.
            mesh_ = mdk_render_create_mesh(renderer_, scene_.vertices.data(),
                                           (int32_t)scene_.vertices.size());
            if (mesh_ < 0) {
                delete buildResolver_;
                buildResolver_ = nullptr;
                buildFailure_ = LevelViewBuildFailure::TransientAllocation;
                buildState_ = BuildState::Failed;
                return false;
            }
            buildState_ = BuildState::Large;
            return false;
        case BuildState::Large:
            startLargeSurfaces(scene_, surfaces_, palette_);
            pumpLargeAt_ = scene_.batches.size();   // the build already did the scan
            // The palette and the textures are made by the shared cache when
            // they are first drawn, not here: an arena that is loaded and
            // never drawn costs nothing past this, and the C#'s resolver
            // makes them as surfaces resolve too.
            delete buildResolver_;
            buildResolver_ = nullptr;
            buildFailure_ = LevelViewBuildFailure::None;
            buildState_ = BuildState::Ready;
            if (pendingRefresh_ && refresh()) {
                pendingRefresh_ = false;
            }
            return true;
        case BuildState::Ready:
            return true;
        default:
            return false;
    }
}

void LevelView::unload() {
    if (renderer_ && mesh_ >= 0) {
        mdk_render_release_mesh(renderer_, mesh_);
    }
    mesh_ = -1;
    delete buildResolver_;
    buildResolver_ = nullptr;
    builder_.reset();
    buildState_ = BuildState::Empty;
    buildFailure_ = LevelViewBuildFailure::None;
    pendingRefresh_ = false;
    scene_ = ArenaScene();
    arena_ = nullptr;
    palette_ = nullptr;
    tornShown_ = 0;
    dynamic_ = false;
    triangleFirsts_.clear();
    pumpAt_ = 0;
    pumpLargeAt_ = 0;
    pumpReaskAt_ = 0;
}

int32_t LevelView::pumpSurfaces(int32_t budget, bool reask) {
    if (!renderer_ || surfaces_ == nullptr || !arena_ || !ready()) {
        return 0;
    }
    int32_t pumped = 0;
    // LARGE SURFACES FIRST. A sequential walk reaches a late batch's texture
    // only after every batch before it, and the pair window is ~20 frames:
    // measured on the Old 3DS, the 64x704 strip (batch far down CDANT_1's
    // list) was first asked for by its own draw and made in that frame
    // (77 ms). A texture of 32768+ texels is the expensive kind (the 3DS
    // backend expands those in slices across frames once started), so the
    // scan starts them ahead of the sequential walk. The scan itself is
    // one pass over the batch list across all calls; it makes nothing.
    // The scan's prepares do NOT spend the caller's budget: a resident
    // copy's prepare is a backend no-op, and counting it starved the
    // second arena's scan behind the current arena's re-prepares
    // (measured: the DANT_2 entry re-make survived two fix rounds that
    // way). The scan is bounded by its own per-call limit instead; the
    // sequential walk below keeps the budget for actual makes.
    int32_t largePrepared = 0;
    while (largePrepared < budget && pumpLargeAt_ < scene_.batches.size()) {
        const ArenaBatch& batch = scene_.batches[pumpLargeAt_++];
        if (surfaceTexels(batch) < kLargeSurfaceTexels) {
            continue;
        }
        // Hidden batches ARE started: the corridor strip's group is
        // script-hidden until its door unlocks, so a visibility skip leaves
        // exactly the surfaces this scan exists for to their first draw.
        // The cost of a never-shown start is bounded by the backend's
        // pending-expansion cap; the sequential walk below keeps its skip.
        // prepare, not just surfaceId: a large copy evicted while its arena
        // sat resident is re-expanded in slices from here, not in the frame
        // that first draws it again (measured: 1024x256, 320 ms at the
        // DANT_2 entry).
        surfaces_->prepare(batch.surface, palette_);
        largePrepared++;
    }
    if (pumpLargeAt_ >= scene_.batches.size()) {
        // The scan wraps: a large copy evicted while its arena sits
        // resident is prepared again on the next pass, not re-made in the
        // frame that first draws it again. The pass is one batch-list walk
        // per (large count / budget) calls; prepares of resident copies
        // are backend no-ops.
        pumpLargeAt_ = 0;
    }
    for (int32_t i = 0; i < budget && pumpAt_ < scene_.batches.size(); i++) {
        const ArenaBatch& batch = scene_.batches[pumpAt_];
        // The same skip draw() makes: a group the scripts hid is not drawn, so
        // its texture is left to the frame that shows it again -- UNLESS the
        // orig pack holds it (requestPacked): that is a pending id, no make,
        // and its file is then read off the frame instead of by the draw of
        // the frame a script shows the group (measured on the Old 3DS: level
        // 7's DANT_1, 154 ms at frame 549, four such reads in one frame).
        if (!arenaBatchVisible(arena_, batch)) {
            surfaces_->requestPacked(batch.surface, palette_);
        } else {
            // A negative id (the backend refused) is left to draw() to ask
            // again: a refused texture is CACHED as refused
            // (surface_cache.cpp), so pinning the cursor here would block every
            // batch behind it for the level's life.
            if (surfaceTexels(batch) >= kLargeSurfaceTexels) {
                surfaces_->prepare(batch.surface, palette_);
            } else {
                surfaces_->surfaceId(batch.surface, palette_);
            }
        }
        pumpAt_++;
        pumped++;
    }
    // THE RE-ASK, once the walk is done: a packed original keeps no pixels in
    // the backend, so one evicted while its arena sits resident would be read
    // again by the draw that next wants it (inside that frame). This pass asks
    // the made textures of the shown small batches to be ready again
    // (prepareCached: a backend no-op for a resident texture and for a small
    // index one; an evicted packed original is queued for the reader). Large
    // batches are the scan's above; hidden ones are not re-asked -- a texture
    // nobody draws would only be read, evicted and read again -- and neither
    // is a view the caller does not name (`reask`: only the pair and the drawn
    // views; a hint-only resident's textures would fill the linear pool with
    // surfaces nobody is looking at). It wraps, a few batches a call, and does
    // not spend the budget (it makes nothing). The backend reads such a
    // re-asked texture only into free memory (04_textures.inc, a hint never
    // evicts).
    if (reask && pumpAt_ >= scene_.batches.size() && !scene_.batches.empty()) {
        for (int32_t i = 0; i < kReaskPerCall; i++) {
            if (pumpReaskAt_ >= scene_.batches.size()) {
                pumpReaskAt_ = 0;
            }
            const ArenaBatch& batch = scene_.batches[pumpReaskAt_++];
            if (surfaceTexels(batch) < kLargeSurfaceTexels && arenaBatchVisible(arena_, batch)) {
                surfaces_->prepareCached(batch.surface, palette_);
            }
        }
    }
    return pumped;
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

bool LevelView::batchesCutting(const Vec3& boxMin, const Vec3& boxMax, Vec3* outMin,
                               Vec3* outMax) const {
    if (!ready()) {
        return false;
    }
    bool any = false;
    for (size_t i = 0; i < scene_.batches.size(); i++) {
        const ArenaBatch& batch = scene_.batches[i];
        if (!arenaBatchVisible(arena_, batch)) {
            continue;
        }
        const Vec3 lo = {batch.boundsMin[0], batch.boundsMin[1], batch.boundsMin[2]};
        const Vec3 hi = {batch.boundsMax[0], batch.boundsMax[1], batch.boundsMax[2]};
        if (!boxesCut(lo, hi, boxMin, boxMax)) {
            continue;
        }
        if (!any) {
            *outMin = lo;
            *outMax = hi;
            any = true;
        } else {
            *outMin = Vec3{fminf(outMin->x, lo.x), fminf(outMin->y, lo.y), fminf(outMin->z, lo.z)};
            *outMax = Vec3{fmaxf(outMax->x, hi.x), fmaxf(outMax->y, hi.y), fmaxf(outMax->z, hi.z)};
        }
    }
    return any;
}

void LevelView::draw(ArenaLayer layer, const Vec3& boxMin, const Vec3& boxMax, const Vec3& eye,
                     float eyeMargin, const scripts::AnimatedTextures* frames) {
    if (!renderer_ || mesh_ < 0 || surfaces_ == nullptr || !ready()) {
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
        // LevelView.cs:139: an animated texture's batch at the frame the scripts set
        // for it in this arena (FrameOf: 0 until one is set). Only M_COMM, the comm
        // device's screen (LEVEL4 MEAT_3, LEVEL7 DANT_5), is animated by a script,
        // so a level without one skips the lookup on the map's emptiness.
        draw.frame = 0;
        if (frames != nullptr && batch.surface.frameCount > 1 && batch.surface.texture != nullptr &&
            !frames->frames().empty()) {
            draw.frame = frames->frameOf(arena_->name(), batch.surface.texture->name());
        }
        draw.primitive = Primitive::Triangles;
        memset(draw.world, 0, sizeof(draw.world));
        draw.world[0] = 1.0f;
        draw.world[5] = 1.0f;
        draw.world[10] = 1.0f;
        draw.world[15] = 1.0f;
        // The batch's box, for the backend's per-eye frustum cull. The world
        // matrix is the identity here, so the batch box IS the world box.
        for (int j = 0; j < 3; j++) {
            draw.bounds_min[j] = batch.boundsMin[j];
            draw.bounds_max[j] = batch.boundsMax[j];
        }
        draw.cullable = true;
        // The camera's arena over the other one (render/arena_scene.h's box arithmetic).
        // The other arena whole (its glass after its own walls, as the original
        // draws it); of the camera's, the batches it may hide.
        draw.back = layer == ArenaLayer::Back;
        if (layer == ArenaLayer::Cover) {
            const Vec3 lo = {batch.boundsMin[0], batch.boundsMin[1], batch.boundsMin[2]};
            const Vec3 hi = {batch.boundsMax[0], batch.boundsMax[1], batch.boundsMax[2]};
            Vec3 reachMin{};
            Vec3 reachMax{};
            boxWithEye(lo, hi, eye, eyeMargin, &reachMin, &reachMax);
            draw.cover = boxesCut(reachMin, reachMax, boxMin, boxMax);
        }

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


bool LevelView::retryPendingRefresh() {
    if (!pendingRefresh_) {
        return true;
    }
    if (!ready() || !refresh()) {
        return false;
    }
    pendingRefresh_ = false;
    return true;
}

bool LevelView::refresh() {
    if (!renderer_ || !level_ || !arena_ || surfaces_ == nullptr) {
        return false;
    }
    if (!ready()) {
        // Asked for mid-build: the rebuild it wants is of a scene that
        // does not exist yet. Record it (it is applied at the Ready step,
        // and the build in flight also reads the current group materials
        // as it goes) but report NOT applied, as the synchronous build's
        // caller expects the rebuild to have happened: if the build then
        // fails, the view is released and the refresh is moot, not lost.
        if (buildState_ == BuildState::Scene || buildState_ == BuildState::Mesh ||
            buildState_ == BuildState::Large) {
            pendingRefresh_ = true;
        }
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
    if (!buildArenaScene(arena_, resolver, *palette_, archives, &rebuilt)) {
        pendingRefresh_ = true;
        return false;
    }

    // Publish only after the replacement mesh exists. A mesh refusal is
    // common enough under linear-heap pressure that turning a live room
    // into an empty one would be a visible regression: the caller still
    // has the last drawable scene, its mesh, and its torn state.
    MdkMeshId candidate = -1;
    if (!rebuilt.vertices.empty()) {
        candidate = mdk_render_create_mesh(renderer_, rebuilt.vertices.data(),
                                           (int32_t)rebuilt.vertices.size());
        if (candidate < 0) {
            pendingRefresh_ = true;
            return false;
        }
    }

    scene_.vertices.swap(rebuilt.vertices);
    scene_.batches.swap(rebuilt.batches);
    pumpLargeAt_ = 0;   // the rebuilt scene's large surfaces get their scan
    pumpAt_ = 0;        // ... and its sequential walk starts over (cache hits)
    pumpReaskAt_ = 0;
    // The rebuilt scene is whole and its mesh static again: an end of a level in
    // progress tears it again on the next tear().
    tornShown_ = 0;
    dynamic_ = false;
    triangleFirsts_.clear();

    if (mesh_ >= 0) {
        mdk_render_release_mesh(renderer_, mesh_);
    }
    mesh_ = candidate;
    pendingRefresh_ = false;
    return true;
}

} // namespace mdk
