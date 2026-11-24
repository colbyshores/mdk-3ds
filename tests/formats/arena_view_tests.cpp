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

#include <stdint.h>
#include <stdlib.h>

extern int mdk_test_meshDraws;
extern int mdk_test_meshes;
extern int mdk_test_failMeshCreates;

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
            view.draw();
            if (mdk_test_meshDraws == drawsBefore) {
                view.unload();
                continue;
            }

            mdk_test_failMeshCreates = 1;
            MDK_CHECK(!view.refresh());
            MDK_CHECK_EQ(mdk_test_failMeshCreates, 0);

            const int drawsAfterFailure = mdk_test_meshDraws;
            view.draw();
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
        view.draw();
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
    view.draw();
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
