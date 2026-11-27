// ============================================================================
// arena_view_tests.cpp -- LevelView publication regressions.
//
// A refresh whose replacement mesh cannot be created must retain the old
// drawable scene and mesh. These run against the real game data, as the
// format tests do; without it they report the missing fixture and return.
// ============================================================================

#include "test_support.h"

#include "formats/case_insensitive_path.h"
#include "formats/mdk_data.h"
#include "game/arena_views.h"
#include "game/level_data.h"
#include "game/level_view.h"
#include "render/surface_cache.h"
#include "scripts/animated_textures.h"

#include <stdint.h>
#include <stdlib.h>

#include <utility>
#include <vector>

extern int mdk_test_meshDraws;
extern int mdk_test_backDraws;
extern int mdk_test_coverDraws;
extern int mdk_test_meshes;
extern int mdk_test_failMeshCreates;
extern std::vector<std::pair<int32_t, int32_t> > mdk_test_drawFrames;

namespace {

mdk::formats::MdkData locateData() {
    const mdk::formats::FileSystem* system = mdk::formats::fileSystem();
    if (const char* env = getenv("MDK_DATA_DIR")) {
        if (env[0] != '\0') {
            const std::string marker =
                mdk::formats::caseInsensitiveResolve(system, env, "TRAVERSE/TRAVSPRT.BNI");
            if (system->fileExists(system->user, marker.c_str())) {
                return mdk::formats::MdkData(system, env);
            }
        }
    }
    const char* relatives[] = {"../../MDK", "../../../MDK", "MDK"};
    for (size_t i = 0; i < sizeof(relatives) / sizeof(relatives[0]); i++) {
        const std::string marker =
            mdk::formats::caseInsensitiveResolve(system, relatives[i], "TRAVERSE/TRAVSPRT.BNI");
        if (system->fileExists(system->user, marker.c_str())) {
            return mdk::formats::MdkData(system, relatives[i]);
        }
    }
    return mdk::formats::MdkData();
}

} // namespace

MDK_TEST(LevelViewRefreshFailureKeepsOldMesh) {
    mdk::formats::MdkData game = locateData();
    MDK_CHECK(game.found());
    if (!game.found()) {
        return;
    }

    mdk::LevelData level;
    MDK_CHECK(mdk::LevelData::load(&level, &game, 3));
    if (!level.arenas().empty()) {
        // The seam only inspects the pointer's identity; its functions ignore it.
        MdkRenderer renderer = (MdkRenderer)(uintptr_t)1;
        mdk::SurfaceCache surfaces(renderer);

        bool exercised = false;
        for (size_t i = 0; i < level.arenas().size() && !exercised; i++) {
            const mdk::formats::Arena* arena = level.arenas()[i];
            mdk::LevelView view;
            if (!view.load(renderer, &level, arena, &surfaces)) {
                continue;
            }

            const int drawsBefore = mdk_test_meshDraws;
            view.draw(mdk::ArenaLayer::Plain, mdk::Vec3{}, mdk::Vec3{}, mdk::Vec3{}, 0.0f);
            if (mdk_test_meshDraws == drawsBefore) {
                view.unload();
                continue;
            }

            mdk_test_failMeshCreates = 1;
            MDK_CHECK(!view.refresh());
            MDK_CHECK_EQ(mdk_test_failMeshCreates, 0);

            const int drawsAfterFailure = mdk_test_meshDraws;
            view.draw(mdk::ArenaLayer::Plain, mdk::Vec3{}, mdk::Vec3{}, mdk::Vec3{}, 0.0f);
            MDK_CHECK(mdk_test_meshDraws > drawsAfterFailure);
            view.unload();
            exercised = true;
        }
        MDK_CHECK(exercised);
    }
    mdk_test_failMeshCreates = 0;
}

namespace {

bool loadLevel3(mdk::LevelData* level) {
    mdk::formats::MdkData game = locateData();
    return game.found() && mdk::LevelData::load(level, &game, 3);
}

const mdk::formats::Arena* firstDrawableArena(mdk::LevelData* level, mdk::SurfaceCache* surfaces,
                                                MdkRenderer renderer) {
    for (size_t i = 0; i < level->arenas().size(); i++) {
        const mdk::formats::Arena* arena = level->arenas()[i];
        mdk::LevelView view;
        if (!view.load(renderer, level, arena, surfaces)) {
            continue;
        }
        const int drawsBefore = mdk_test_meshDraws;
        view.draw(mdk::ArenaLayer::Plain, mdk::Vec3{}, mdk::Vec3{}, mdk::Vec3{}, 0.0f);
        view.unload();
        if (mdk_test_meshDraws > drawsBefore) {
            return arena;
        }
    }
    return nullptr;
}

} // namespace

MDK_TEST(ArenaViewsTransientFailureBacksOffAcrossBuildStarts) {
    mdk::LevelData level;
    MDK_CHECK(loadLevel3(&level));
    if (level.arenas().empty()) {
        return;
    }
    MdkRenderer renderer = (MdkRenderer)(uintptr_t)1;
    mdk::SurfaceCache surfaces(renderer);
    const mdk::formats::Arena* arena = firstDrawableArena(&level, &surfaces, renderer);
    MDK_CHECK(arena != nullptr);
    if (arena == nullptr) {
        return;
    }

    mdk::ArenaViews views;
    views.start(renderer, &level, &surfaces, mdk::Shading::Original);
    const std::vector<std::string> drawn(1, arena->name());
    mdk_test_failMeshCreates = 100;
    for (int i = 0; i < 5; i++) {
        views.sync(arena->name(), std::string(), drawn);
    }
    // Four attempts earn the long backoff; the fifth sync must wait, not
    // discover that the successful sliced starts erased the history.
    MDK_CHECK_EQ(views.stats().failed, 4);
    views.sync(arena->name(), std::string(), drawn);
    MDK_CHECK_EQ(views.stats().failed, 4);
    mdk_test_failMeshCreates = 0;
    views.clear();
}

MDK_TEST(ArenaViewsPermanentSceneFailureIsNotRetriedWhileKept) {
    mdk::LevelData level;
    MDK_CHECK(loadLevel3(&level));
    if (level.arenas().empty()) {
        return;
    }
    MdkRenderer renderer = (MdkRenderer)(uintptr_t)1;
    mdk::SurfaceCache surfaces(renderer);
    const mdk::formats::Arena* arena = firstDrawableArena(&level, &surfaces, renderer);
    MDK_CHECK(arena != nullptr);
    if (arena == nullptr) {
        return;
    }

    mdk::formats::Arena* mutableArena = level.arenaNamedMutable(arena->name());
    MDK_CHECK(mutableArena != nullptr);
    if (mutableArena == nullptr) {
        return;
    }
    const std::vector<uint32_t> oldFlags = mutableArena->mutableTriangleFlags();
    std::vector<uint32_t>& flags = mutableArena->mutableTriangleFlags();
    for (size_t i = 0; i < flags.size(); i++) {
        flags[i] = 0xffffffffu;
    }

    mdk::ArenaViews views;
    views.start(renderer, &level, &surfaces, mdk::Shading::Original);
    const std::vector<std::string> drawn(1, arena->name());
    for (int i = 0; i < 10; i++) {
        views.sync(arena->name(), std::string(), drawn);
    }
    MDK_CHECK_EQ(views.stats().failed, 1);

    views.clear();
    mutableArena->mutableTriangleFlags() = oldFlags;
    mdk_test_failMeshCreates = 0;
}

MDK_TEST(LevelViewPendingRefreshRetriesAfterMeshFailure) {
    mdk::LevelData level;
    MDK_CHECK(loadLevel3(&level));
    if (level.arenas().empty()) {
        return;
    }
    MdkRenderer renderer = (MdkRenderer)(uintptr_t)1;
    mdk::SurfaceCache surfaces(renderer);
    const mdk::formats::Arena* arena = firstDrawableArena(&level, &surfaces, renderer);
    MDK_CHECK(arena != nullptr);
    if (arena == nullptr) {
        return;
    }

    mdk::LevelView view;
    MDK_CHECK(view.loadBegin(renderer, &level, arena, &surfaces));
    MDK_CHECK(!view.refresh());
    MDK_CHECK(view.pendingRefresh());

    const int createsBeforeBuild = mdk_test_meshes;
    while (!view.ready() && !view.buildFailed() && mdk_test_meshes == createsBeforeBuild) {
        view.buildStep();
    }
    MDK_CHECK(!view.ready());
    MDK_CHECK(!view.buildFailed());
    mdk_test_failMeshCreates = 1;
    MDK_CHECK(view.buildStep());
    MDK_CHECK(view.ready());
    MDK_CHECK(view.pendingRefresh());
    MDK_CHECK_EQ(mdk_test_failMeshCreates, 0);

    mdk_test_failMeshCreates = 0;
    MDK_CHECK(view.retryPendingRefresh());
    MDK_CHECK(!view.pendingRefresh());
    const int drawsBefore = mdk_test_meshDraws;
    view.draw(mdk::ArenaLayer::Plain, mdk::Vec3{}, mdk::Vec3{}, mdk::Vec3{}, 0.0f);
    MDK_CHECK(mdk_test_meshDraws > drawsBefore);
    view.unload();
}

MDK_TEST(ArenaViewsRetriesFailedRefreshAtSync) {
    mdk::LevelData level;
    MDK_CHECK(loadLevel3(&level));
    if (level.arenas().empty()) {
        return;
    }
    MdkRenderer renderer = (MdkRenderer)(uintptr_t)1;
    mdk::SurfaceCache surfaces(renderer);
    const mdk::formats::Arena* arena = firstDrawableArena(&level, &surfaces, renderer);
    MDK_CHECK(arena != nullptr);
    if (arena == nullptr) {
        return;
    }

    mdk::ArenaViews views;
    views.start(renderer, &level, &surfaces, mdk::Shading::Original);
    const std::vector<std::string> drawn(1, arena->name());
    views.sync(arena->name(), std::string(), drawn);
    MDK_CHECK(views.find(arena->name()) != nullptr && views.find(arena->name())->ready());

    mdk_test_failMeshCreates = 1;
    MDK_CHECK(!views.refresh(arena->name()));
    const int createsAfterFailure = mdk_test_meshes;
    mdk_test_failMeshCreates = 0;
    views.sync(arena->name(), std::string(), drawn);
    MDK_CHECK(mdk_test_meshes > createsAfterFailure);
    views.clear();
}

MDK_TEST(ArenaViewsReadyRefreshBacksOffAcrossSyncs) {
    mdk::LevelData level;
    MDK_CHECK(loadLevel3(&level));
    if (level.arenas().empty()) {
        return;
    }
    MdkRenderer renderer = (MdkRenderer)(uintptr_t)1;
    mdk::SurfaceCache surfaces(renderer);
    const mdk::formats::Arena* arena = firstDrawableArena(&level, &surfaces, renderer);
    MDK_CHECK(arena != nullptr);
    if (arena == nullptr) {
        return;
    }

    mdk::ArenaViews views;
    views.start(renderer, &level, &surfaces, mdk::Shading::Original);
    const std::vector<std::string> drawn(1, arena->name());
    views.sync(arena->name(), std::string(), drawn);
    MDK_CHECK(views.find(arena->name()) != nullptr && views.find(arena->name())->ready());

    // A refused refresh on a READY view must ladder like a failed build:
    // three quick retries (one per sync), then kRetrySyncs of silence. The
    // publication loop used to erase the record the retry loop had just
    // written, so attempts never left 1 and the full scene rebuild ran on
    // EVERY sync forever. Failed attempts are counted by the seam's
    // failMeshCreates counter, which only decrements on a refusal.
    mdk_test_failMeshCreates = 100;
    MDK_CHECK(!views.refresh(arena->name()));
    MDK_CHECK_EQ(mdk_test_failMeshCreates, 99);
    for (int i = 0; i < 3; i++) {
        views.sync(arena->name(), std::string(), drawn);
    }
    MDK_CHECK_EQ(mdk_test_failMeshCreates, 96);   // the three quick retries
    for (int i = 0; i < 30; i++) {
        views.sync(arena->name(), std::string(), drawn);
    }
    MDK_CHECK_EQ(mdk_test_failMeshCreates, 96);   // inside the long backoff
    mdk_test_failMeshCreates = 0;
    const int createsBefore = mdk_test_meshes;
    for (int i = 0; i < 120; i++) {
        views.sync(arena->name(), std::string(), drawn);
    }
    MDK_CHECK_EQ(mdk_test_meshes, createsBefore + 1);   // the backoff expired
    MDK_CHECK(!views.find(arena->name())->pendingRefresh());
    views.clear();
}

// ---------------------------------------------------------------------------
// THE PUMP AND A SCRIPT-HIDDEN GROUP (LevelView::pumpSurfaces): the walk skips a
// hidden batch's texture -- it would be a palette expansion for a surface nobody
// may see -- but one the orig pack holds is a pending id with no make, so it is
// asked for (SurfaceCache::requestPacked) and its file read before a script
// shows the group (measured on the Old 3DS: level 7, four such reads in the
// frame a group was shown, 154 ms).
// ---------------------------------------------------------------------------

extern MdkTextureId mdk_test_compressedFileId;
extern int mdk_test_compressedFileCalls;
extern int mdk_test_prepareCalls;

namespace {

// The lookup answers for ONE texture only (the hidden one): a packed original
// when g_packAnswer, a miss otherwise. Every other surface is a miss.
const mdk::formats::Texture* g_packTarget = nullptr;
bool g_packAnswer = true;
int g_packLookups = 0;

bool targetLookup(void*, const mdk::Surface& surface, const mdk::formats::Palette*,
                  mdk::ColourReplacement* out) {
    if (surface.texture == nullptr || surface.texture != g_packTarget) {
        return false;
    }
    g_packLookups++;
    if (!g_packAnswer) {
        return false;
    }
    out->compressedPath = "romfs:/orig/textures/HIDDEN.t3x";
    return true;
}

// A small Lit texture that ONLY batches of one non-zero group draw, so hiding
// that group hides every use of it (a texture also drawn by a shown batch would
// be made by the walk anyway, and the test would read nothing). Small: the
// large-surface scan prepares hidden large textures on purpose.
bool findHiddenOnlyTexture(const mdk::ArenaScene& scene, int32_t* group,
                           const mdk::ArenaBatch** batchOut) {
    for (size_t i = 0; i < scene.batches.size(); i++) {
        const mdk::ArenaBatch& candidate = scene.batches[i];
        const mdk::formats::Texture* texture = candidate.surface.texture;
        if (texture == nullptr || candidate.group == 0 ||
            candidate.surface.shading != mdk::Shading::Lit ||
            (int64_t)texture->width() * texture->height() * texture->frameCount() >= 32768) {
            continue;
        }
        bool only = true;
        for (size_t j = 0; j < scene.batches.size() && only; j++) {
            if (scene.batches[j].surface.texture == texture &&
                scene.batches[j].group != candidate.group) {
                only = false;
            }
        }
        if (only) {
            *group = candidate.group;
            *batchOut = &candidate;
            return true;
        }
    }
    return false;
}

// Pumps `view` with a fresh cache and the target lookup until its walk is done;
// returns whether a hidden-only texture was found in `arena` to run it on, and
// (through prepared) whether the hidden pair ended up with a texture id.
bool pumpWithHiddenTarget(mdk::LevelData* level, const mdk::formats::Arena* arena,
                          bool packAnswer, bool* prepared) {
    MdkRenderer renderer = (MdkRenderer)(uintptr_t)1;
    mdk::SurfaceCache surfaces(renderer);
    mdk::LevelView view;
    view.setShading(mdk::Shading::Lit);
    if (!view.load(renderer, level, arena, &surfaces)) {
        return false;
    }
    int32_t group = 0;
    const mdk::ArenaBatch* batch = nullptr;
    if (!findHiddenOnlyTexture(view.scene(), &group, &batch)) {
        view.unload();
        return false;
    }
    mdk::formats::Arena* mutableArena = level->arenaNamedMutable(arena->name());
    if (mutableArena != arena) {
        view.unload();
        return false;
    }
    const uint32_t oldState = mutableArena->groupState(group);
    mutableArena->setGroupState(group, oldState | mdk::formats::Arena::kGroupHidden);
    g_packTarget = batch->surface.texture;
    g_packAnswer = packAnswer;
    g_packLookups = 0;
    mdk_test_compressedFileId = 42;
    mdk_test_compressedFileCalls = 0;
    surfaces.setColourReplacementLookup(targetLookup, nullptr);
    for (int i = 0; i < 100000 && view.pumpSurfaces(64, true) > 0; i++) {
    }
    mdk_test_prepareCalls = 0;
    surfaces.prepareCached(batch->surface, &level->paletteOf(arena));
    *prepared = mdk_test_prepareCalls == 1;
    mutableArena->setGroupState(group, oldState);
    mdk_test_compressedFileId = -1;
    view.unload();
    return true;
}

} // namespace

MDK_TEST(LevelViewPumpAsksForAHiddenBatchsPackedOriginal) {
    mdk::LevelData level;
    MDK_CHECK(loadLevel3(&level));
    bool exercised = false;
    for (size_t i = 0; i < level.arenas().size() && !exercised; i++) {
        bool prepared = false;
        if (!pumpWithHiddenTarget(&level, level.arenas()[i], true, &prepared)) {
            continue;
        }
        exercised = true;
        MDK_CHECK_EQ(g_packLookups, 1);                // asked once for the pair
        MDK_CHECK_EQ(mdk_test_compressedFileCalls, 1); // ... and its pending id made
        MDK_CHECK(prepared);                           // ... and cached for the draw
    }
    MDK_CHECK(exercised);
}

MDK_TEST(LevelViewPumpMakesNothingForAHiddenNonPackSurface) {
    mdk::LevelData level;
    MDK_CHECK(loadLevel3(&level));
    bool exercised = false;
    for (size_t i = 0; i < level.arenas().size() && !exercised; i++) {
        bool prepared = true;
        if (!pumpWithHiddenTarget(&level, level.arenas()[i], false, &prepared)) {
            continue;
        }
        exercised = true;
        MDK_CHECK_EQ(g_packLookups, 1);                // asked (a miss), once
        MDK_CHECK_EQ(mdk_test_compressedFileCalls, 0); // nothing from a file
        MDK_CHECK(!prepared);                          // and no index texture made
    }
    MDK_CHECK(exercised);
}

// THE RE-ASK IS THE PAIR'S AND THE DRAWN VIEWS' ONLY (ArenaViews::sync sets it,
// LevelView::pumpSurfaces honours it): a hint-only resident re-asking its made
// textures would fill the linear pool with surfaces nobody is looking at (on
// the 3DS an evicted packed original re-asked is a read). The same view, pumped
// the same number of times, asks for more once it is the current arena; as a
// hint only its large-surface scan prepares (that scan is not the re-ask).
MDK_TEST(ArenaViewsReaskOnlyThePairAndTheDrawn) {
    mdk::LevelData level;
    MDK_CHECK(loadLevel3(&level));
    if (level.arenas().empty()) {
        return;
    }
    MdkRenderer renderer = (MdkRenderer)(uintptr_t)1;
    mdk::SurfaceCache surfaces(renderer);
    const mdk::formats::Arena* arena = firstDrawableArena(&level, &surfaces, renderer);
    MDK_CHECK(arena != nullptr);
    if (arena == nullptr) {
        return;
    }
    mdk::ArenaViews views;
    views.start(renderer, &level, &surfaces, mdk::Shading::Original);
    const std::vector<std::string> none;
    const std::vector<std::string> hints(1, arena->name());
    for (int i = 0; i < 400 && !(views.find(arena->name()) && views.find(arena->name())->ready());
         i++) {
        views.sync("", "", none, hints);
    }
    MDK_CHECK(views.find(arena->name()) != nullptr && views.find(arena->name())->ready());
    for (int i = 0; i < 2000; i++) {
        views.sync("", "", none, hints);
        views.pumpSurfaces(64);   // the walk finishes; the hint does not re-ask
    }
    mdk_test_prepareCalls = 0;
    for (int i = 0; i < 100; i++) {
        views.sync("", "", none, hints);
        views.pumpSurfaces(64);
    }
    const int asHint = mdk_test_prepareCalls;
    mdk_test_prepareCalls = 0;
    for (int i = 0; i < 100; i++) {
        views.sync(arena->name(), "", none, hints);
        views.pumpSurfaces(64);
    }
    const int asCurrent = mdk_test_prepareCalls;
    MDK_CHECK(asCurrent > asHint);
    views.clear();
}

// THE HINTS ARE BUILT IN ORDER, THE FIRST ONE WITHOUT A VIEW (ArenaViews::sync):
// with the bubble's second hop the first hint is often a connector already built,
// and the arena past it is the one Kurt reaches next. Building only hints[0] left
// LEVEL4's MEAT_5 behind its 31-unit CMEAT_5 to the synchronous finish (257 ms on
// the Old 3DS, 2026-11-27). Here the second hint is built once the first is.
MDK_TEST(ArenaViewsBuildTheFirstHintWithoutAView) {
    // The data stays in scope: a view's build reads texture pixels from the MTO
    // through it (LevelData::loadPixelsFromMto), after the load returned.
    mdk::formats::MdkData game = locateData();
    if (!game.found()) {
        return;
    }
    mdk::LevelData level;
    MDK_CHECK(mdk::LevelData::load(&level, &game, 3));
    if (level.arenas().empty()) {
        return;
    }
    MdkRenderer renderer = (MdkRenderer)(uintptr_t)1;
    mdk::SurfaceCache surfaces(renderer);
    // Two arenas that each build a view.
    std::vector<std::string> buildable;
    for (size_t i = 0; i < level.arenas().size() && buildable.size() < 2; i++) {
        mdk::LevelView view;
        if (view.load(renderer, &level, level.arenas()[i], &surfaces)) {
            buildable.push_back(level.arenas()[i]->name());
        }
        view.unload();
    }
    MDK_CHECK_EQ((int)buildable.size(), 2);
    if (buildable.size() < 2) {
        return;
    }
    mdk::ArenaViews views;
    views.start(renderer, &level, &surfaces, mdk::Shading::Original);
    const std::vector<std::string> none;
    const std::vector<std::string> hints = buildable;
    for (int i = 0; i < 2000 && !(views.find(buildable[1]) && views.find(buildable[1])->ready());
         i++) {
        views.sync("", "", none, hints);
    }
    MDK_CHECK(views.find(buildable[0]) != nullptr && views.find(buildable[0])->ready());
    MDK_CHECK(views.find(buildable[1]) != nullptr && views.find(buildable[1])->ready());
    // Hints are kept: both views stay while hinted, and go when no longer named.
    views.sync("", "", none, hints);
    MDK_CHECK_EQ(views.count(), 2);
    views.sync("", "", none, none);
    MDK_CHECK_EQ(views.count(), 0);
    views.clear();
}

// THE CAMERA'S ARENA OVER THE OTHER ONE (upstream e1eb36bb; game/arena_views.cpp):
// LevelView::draw's marks. Plain marks nothing (the frame as before); Back marks
// every batch of the other arena; Cover marks the camera arena's batches whose box,
// grown to take in the eye, cuts the given box -- all of them for a box over the
// whole arena, none for a box and an eye far from it.
MDK_TEST(LevelViewMarksTheLayers) {
    mdk::LevelData level;
    MDK_CHECK(loadLevel3(&level));
    if (level.arenas().empty()) {
        return;
    }
    MdkRenderer renderer = (MdkRenderer)(uintptr_t)1;
    mdk::SurfaceCache surfaces(renderer);
    const mdk::formats::Arena* arena = firstDrawableArena(&level, &surfaces, renderer);
    MDK_CHECK(arena != nullptr);
    if (arena == nullptr) {
        return;
    }
    mdk::LevelView view;
    MDK_CHECK(view.load(renderer, &level, arena, &surfaces));
    mdk::Vec3 lo{};
    mdk::Vec3 hi{};
    MDK_CHECK(view.bounds(&lo, &hi));
    const mdk::Vec3 centre = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};

    int draws = mdk_test_meshDraws;
    int backs = mdk_test_backDraws;
    int covers = mdk_test_coverDraws;
    view.draw(mdk::ArenaLayer::Plain, lo, hi, centre, 1.0f);
    const int drawn = mdk_test_meshDraws - draws;
    MDK_CHECK(drawn > 0);
    MDK_CHECK_EQ(mdk_test_backDraws - backs, 0);
    MDK_CHECK_EQ(mdk_test_coverDraws - covers, 0);

    backs = mdk_test_backDraws;
    view.draw(mdk::ArenaLayer::Back, lo, hi, centre, 1.0f);
    MDK_CHECK_EQ(mdk_test_backDraws - backs, drawn);

    // A box a little bigger than the arena: every batch's box cuts it.
    const mdk::Vec3 bigLo = {lo.x - 1.0f, lo.y - 1.0f, lo.z - 1.0f};
    const mdk::Vec3 bigHi = {hi.x + 1.0f, hi.y + 1.0f, hi.z + 1.0f};
    covers = mdk_test_coverDraws;
    view.draw(mdk::ArenaLayer::Cover, bigLo, bigHi, centre, 1.0f);
    MDK_CHECK_EQ(mdk_test_coverDraws - covers, drawn);

    // A box far beyond the arena on +x, the eye beyond it on -x: nothing of the
    // arena lies between the eye and that box, so nothing can be hidden. (With
    // the eye beyond the box, every batch would be: the box is between them.)
    const float far = (hi.x - lo.x) + 1000.0f;
    const mdk::Vec3 awayLo = {hi.x + far, lo.y, lo.z};
    const mdk::Vec3 awayHi = {hi.x + far + 10.0f, hi.y, hi.z};
    const mdk::Vec3 awayEye = {lo.x - far, centre.y, centre.z};
    covers = mdk_test_coverDraws;
    view.draw(mdk::ArenaLayer::Cover, awayLo, awayHi, awayEye, 1.0f);
    MDK_CHECK_EQ(mdk_test_coverDraws - covers, 0);
    // And with the eye beyond the box, the box is between the eye and every batch.
    const mdk::Vec3 behindEye = {hi.x + far + 20.0f, centre.y, centre.z};
    covers = mdk_test_coverDraws;
    view.draw(mdk::ArenaLayer::Cover, awayLo, awayHi, behindEye, 1.0f);
    MDK_CHECK_EQ(mdk_test_coverDraws - covers, drawn);
    view.unload();
}

// OPCODE 133 (arena_texture_frame) REACHES THE DRAW: LevelView::draw gives an animated
// batch the frame the scripts set for its texture in its arena (LevelView.cs:139,
// FrameOf), and 0 without frames, before a frame is set, or for another arena's key.
// LEVEL7 DANT_5's comm device screen M_COMM (6 frames) is the retail case.
namespace {

int32_t mCommFrames(void*, const std::string&, const std::string& texture) {
    return texture == "M_COMM" ? 6 : 0;
}

// The frames the view's animated batches (frame_count 6) were queued with.
std::vector<int32_t> animatedDrawFrames(mdk::LevelView* view,
                                        const mdk::scripts::AnimatedTextures* frames) {
    mdk_test_drawFrames.clear();
    const mdk::Vec3 none{};
    view->draw(mdk::ArenaLayer::Plain, none, none, none, 1.0f, frames);
    std::vector<int32_t> out;
    for (size_t i = 0; i < mdk_test_drawFrames.size(); i++) {
        if (mdk_test_drawFrames[i].second == 6) {
            out.push_back(mdk_test_drawFrames[i].first);
        }
    }
    return out;
}

} // namespace

MDK_TEST(LevelViewDrawsTheScriptsTextureFrame) {
    mdk::formats::MdkData game = locateData();
    MDK_CHECK(game.found());
    if (!game.found()) {
        return;
    }
    mdk::LevelData level;
    MDK_CHECK(mdk::LevelData::load(&level, &game, 7));
    const mdk::formats::Arena* arena = level.arenaNamed("DANT_5");
    MDK_CHECK(arena != nullptr);
    if (arena == nullptr) {
        return;
    }
    MdkRenderer renderer = (MdkRenderer)(uintptr_t)1;
    mdk::SurfaceCache surfaces(renderer);
    mdk::LevelView view;
    MDK_CHECK(view.load(renderer, &level, arena, &surfaces));
    MDK_CHECK(view.ready());

    mdk::scripts::AnimatedTextures frames(&mCommFrames, nullptr);
    // No frames object, and no frame set yet: frame 0, as before the fix.
    std::vector<int32_t> drawn = animatedDrawFrames(&view, nullptr);
    MDK_CHECK(!drawn.empty());
    for (size_t i = 0; i < drawn.size(); i++) {
        MDK_CHECK_EQ(drawn[i], 0);
    }
    drawn = animatedDrawFrames(&view, &frames);
    for (size_t i = 0; i < drawn.size(); i++) {
        MDK_CHECK_EQ(drawn[i], 0);
    }
    // Another arena's frame does not move this one.
    frames.set("DANT_3", "M_COMM", mdk::scripts::AnimatedTextures::Mode::Absolute, 5.0f, 0.0f);
    drawn = animatedDrawFrames(&view, &frames);
    for (size_t i = 0; i < drawn.size(); i++) {
        MDK_CHECK_EQ(drawn[i], 0);
    }
    // mode 0 (Absolute) sets value - 1 = 3.5, drawn truncated: frame 3.
    frames.set("DANT_5", "M_COMM", mdk::scripts::AnimatedTextures::Mode::Absolute, 4.5f, 0.0f);
    drawn = animatedDrawFrames(&view, &frames);
    MDK_CHECK(!drawn.empty());
    for (size_t i = 0; i < drawn.size(); i++) {
        MDK_CHECK_EQ(drawn[i], 3);
    }
    view.unload();
}
