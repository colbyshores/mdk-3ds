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
#include "game/level_data.h"
#include "game/level_view.h"
#include "render/surface_cache.h"

#include <stdint.h>
#include <stdlib.h>

extern int mdk_test_meshDraws;
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
