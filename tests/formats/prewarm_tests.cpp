// ============================================================================
// prewarm_tests.cpp -- the prewarm (render/surface_cache.h THE PREWARM,
// game/arena_views.h, game/prewarm.h) against the real level data and the
// seam's counters (render_seam_stubs.cpp).
//
// THE CLAIMS, each one a reason the 3DS experiment can be trusted not to change
// the game while it moves work out of the frame:
//   * it makes EXACTLY what the first draw would have made: the same number of
//     textures, after which the draw makes none;
//   * a budgeted pass makes at most its budget and RESUMES where it stopped, so
//     the total over many frames is the same set, never a texture twice;
//   * no room means nothing is made (the platform's answer is the only gate);
//   * pixels that are away are NEVER read: no demand load, no texture from a
//     null pointer, and the texture is still away afterwards;
//   * an object's model prewarms its layout and textures once.
// ============================================================================

#include "test_support.h"

#include <deque>
#include <stdlib.h>
#include <string>
#include <vector>

#include "formats/arena.h"
#include "formats/case_insensitive_path.h"
#include "formats/mdk_data.h"
#include "game/arena_views.h"
#include "game/level_data.h"
#include "game/level_view.h"
#include "game/object_view.h"
#include "game/prewarm.h"
#include "render/material_resolver.h"
#include "render/surface_cache.h"
#include "scripts/object.h"

extern int mdk_test_indexTextures;
extern int mdk_test_nullIndexTextures;

namespace {

using namespace mdk::formats;

MdkData locatePrewarmData() {
    const FileSystem* system = mdk::test::host::fileSystem();
    if (const char* env = getenv("MDK_DATA_DIR")) {
        if (env[0] != '\0') {
            const std::string marker = caseInsensitiveResolve(system, env, "TRAVERSE/TRAVSPRT.BNI");
            if (system->fileExists(system->user, marker.c_str())) {
                return MdkData(system, env);
            }
        }
    }
    const char* relatives[] = {"../../MDK", "../../../MDK", "MDK"};
    for (size_t i = 0; i < sizeof(relatives) / sizeof(relatives[0]); i++) {
        const std::string marker =
            caseInsensitiveResolve(system, relatives[i], "TRAVERSE/TRAVSPRT.BNI");
        if (system->fileExists(system->user, marker.c_str())) {
            return MdkData(system, relatives[i]);
        }
    }
    return MdkData();
}

const MdkRenderer kRenderer = reinterpret_cast<MdkRenderer>(static_cast<uintptr_t>(1));

bool g_room = true;
int g_roomAsked = 0;
bool switchableRoom(void*, int32_t bytes) {
    g_roomAsked++;
    return g_room && bytes > 0;
}

std::string startArenaOf(const mdk::LevelData& level) {
    const int32_t start = level.dti().startArena();
    return level.dti().arenas()[(size_t)start].name;
}

} // namespace

MDK_TEST(ThePrewarmStopsOnlyWhereItMustResume) {
    // The walk's rule: stop (and come back to the same surface) for the budget,
    // the room and pixels that are not in yet; go on past a made, already-made or
    // refused one.
    MDK_CHECK(mdk::prewarmStops(mdk::PrewarmResult::Away));
    MDK_CHECK(mdk::prewarmStops(mdk::PrewarmResult::NoBudget));
    MDK_CHECK(mdk::prewarmStops(mdk::PrewarmResult::NoRoom));
    MDK_CHECK(!mdk::prewarmStops(mdk::PrewarmResult::Made));
    MDK_CHECK(!mdk::prewarmStops(mdk::PrewarmResult::Had));
    MDK_CHECK(!mdk::prewarmStops(mdk::PrewarmResult::Failed));
}

MDK_TEST(ThePrewarmMakesWhatTheFirstDrawWouldHaveMade) {
    const MdkData game = locatePrewarmData();
    if (!game.found()) {
        return;
    }
    mdk::LevelData level;
    MDK_CHECK(mdk::LevelData::load(&level, &game, 3));
    const std::string a = startArenaOf(level);
    level.stream().update(a, std::string(), false);
    const std::vector<std::string> drawn(1, a);

    // THE DRAW ALONE: what the first frame makes today.
    int drawMade = 0;
    {
        mdk::SurfaceCache surfaces(kRenderer);
        mdk::ArenaViews views;
        views.start(kRenderer, &level, &surfaces, mdk::Shading::Lit);
        views.sync(a, std::string(), drawn);
        const int before = mdk_test_indexTextures;
        views.draw(drawn);
        drawMade = mdk_test_indexTextures - before;
        MDK_CHECK(drawMade > 0);
        views.clear();
    }

    // THE PREWARM, then the draw: the same textures, all of them before it.
    {
        mdk::SurfaceCache surfaces(kRenderer);
        mdk::ArenaViews views;
        views.start(kRenderer, &level, &surfaces, mdk::Shading::Lit);
        views.sync(a, std::string(), drawn);
        const int before = mdk_test_indexTextures;
        mdk::PrewarmBudget budget;
        MDK_CHECK(views.prewarm(drawn, &budget));
        const int prewarmMade = mdk_test_indexTextures - before;
        MDK_CHECK_EQ(prewarmMade, drawMade);
        MDK_CHECK(budget.made >= prewarmMade);   // flat colours are RGBA textures
        MDK_CHECK_EQ(budget.away, 0);
        MDK_CHECK(views.find(a)->warm());
        const int afterPrewarm = mdk_test_indexTextures;
        views.draw(drawn);
        MDK_CHECK_EQ(mdk_test_indexTextures, afterPrewarm);   // the draw made nothing
        // A second pass over a warm set costs nothing and makes nothing.
        mdk::PrewarmBudget again;
        MDK_CHECK(views.prewarm(drawn, &again));
        MDK_CHECK_EQ(again.made, 0);
        views.clear();
    }

    // ONE A PASS: the per-frame step's budget. Exactly one texture a call until
    // the view is warm, the same total, never one twice.
    {
        mdk::SurfaceCache surfaces(kRenderer);
        mdk::ArenaViews views;
        views.start(kRenderer, &level, &surfaces, mdk::Shading::Lit);
        views.sync(a, std::string(), drawn);
        const int before = mdk_test_indexTextures;
        int32_t passes = 0;
        int32_t made = 0;
        bool warm = false;
        while (!warm && passes < 10000) {
            mdk::PrewarmBudget budget;
            budget.makes = 1;
            warm = views.prewarm(drawn, &budget);
            MDK_CHECK(budget.made <= 1);
            made += budget.made;
            passes++;
        }
        MDK_CHECK(warm);
        MDK_CHECK_EQ(mdk_test_indexTextures - before, drawMade);
        MDK_CHECK(made >= drawMade);
        MDK_CHECK(passes >= made);
        const int afterPrewarm = mdk_test_indexTextures;
        views.draw(drawn);
        MDK_CHECK_EQ(mdk_test_indexTextures, afterPrewarm);
        views.clear();
    }

    // NO ROOM: the platform's answer is the gate. Nothing is made, the view is not
    // warm, and the walk resumes when there is room.
    {
        mdk::SurfaceCache surfaces(kRenderer);
        mdk::ArenaViews views;
        views.start(kRenderer, &level, &surfaces, mdk::Shading::Lit);
        views.sync(a, std::string(), drawn);
        const int before = mdk_test_indexTextures;
        g_room = false;
        g_roomAsked = 0;
        mdk::PrewarmBudget refused;
        refused.room = switchableRoom;
        MDK_CHECK(!views.prewarm(drawn, &refused));
        MDK_CHECK_EQ(refused.made, 0);
        MDK_CHECK(refused.noRoom > 0);
        MDK_CHECK(g_roomAsked > 0);
        MDK_CHECK_EQ(mdk_test_indexTextures, before);
        MDK_CHECK(!views.find(a)->warm());
        g_room = true;
        mdk::PrewarmBudget allowed;
        allowed.room = switchableRoom;
        MDK_CHECK(views.prewarm(drawn, &allowed));
        MDK_CHECK_EQ(mdk_test_indexTextures - before, drawMade);
        views.clear();
    }
}

MDK_TEST(ThePrewarmNeverReadsPixelsThatAreAway) {
    const MdkData game = locatePrewarmData();
    if (!game.found()) {
        return;
    }
    mdk::LevelData level;
    MDK_CHECK(mdk::LevelData::load(&level, &game, 3));
    mdk::ArenaStream& stream = level.stream();
    const std::string a = startArenaOf(level);
    stream.update(a, std::string(), false);

    // An MTO arena that is not the resident one: its texture pixels are away.
    const Arena* far = nullptr;
    for (size_t i = 0; i < level.arenas().size() && !far; i++) {
        const Arena* candidate = level.arenas()[i];
        if (candidate->name() != a && level.mto().has(candidate->name()) &&
            !candidate->textures().textures().empty() &&
            candidate->textures().textures()[0].second.pixelsAway()) {
            far = candidate;
        }
    }
    MDK_CHECK(far != nullptr);
    if (!far) {
        return;
    }
    mdk::SurfaceCache surfaces(kRenderer);
    mdk::LevelView view;
    view.setShading(mdk::Shading::Lit);
    MDK_CHECK(view.load(kRenderer, &level, far, &surfaces));
    const int32_t demand0 = stream.stats().demandLoads;
    const int null0 = mdk_test_nullIndexTextures;
    mdk::PrewarmBudget budget;
    MDK_CHECK(!view.prewarm(&budget));
    MDK_CHECK(budget.away > 0);
    MDK_CHECK(!view.warm());
    // Nothing read, nothing made from nothing, and the pixels are still away.
    MDK_CHECK_EQ(stream.stats().demandLoads, demand0);
    MDK_CHECK_EQ(mdk_test_nullIndexTextures, null0);
    MDK_CHECK(far->textures().textures()[0].second.pixelsAway());

    // The surface cache's own answer for one such texture, directly.
    {
        mdk::Surface surface;
        surface.texture = &far->textures().textures()[0].second;
        mdk::PrewarmBudget one;
        MDK_CHECK(surfaces.prewarm(surface, &level.paletteOf(far), &one) ==
                  mdk::PrewarmResult::Away);
        MDK_CHECK(!surfaces.has(surface, &level.paletteOf(far)));
    }

    // Its pixels come in (the stream's switch): the same walk now finishes.
    MDK_CHECK(stream.loadNow(far->name()));
    MDK_CHECK(!far->textures().textures()[0].second.pixelsAway());
    mdk::PrewarmBudget after;
    MDK_CHECK(view.prewarm(&after));
    MDK_CHECK(view.warm());
    MDK_CHECK(after.made > 0);
    MDK_CHECK_EQ(mdk_test_nullIndexTextures, null0);
    view.unload();
}

MDK_TEST(AnObjectsModelPrewarmsItsLayoutAndTexturesOnce) {
    const MdkData game = locatePrewarmData();
    if (!game.found()) {
        return;
    }
    mdk::LevelData level;
    MDK_CHECK(mdk::LevelData::load(&level, &game, 3));
    const std::string a = startArenaOf(level);
    level.stream().update(a, std::string(), false);
    const Arena* arena = level.arenaNamed(a);
    MDK_CHECK(arena != nullptr && arena->modelCount() > 0);
    if (!arena || arena->modelCount() == 0) {
        return;
    }
    mdk::ObjectLook look;
    look.palette = &level.paletteOf(arena);
    look.archives = level.archivesOf(arena);
    mdk::MaterialResolver resolver(&level.dti(), mdk::Shading::Lit);
    mdk::ObjectView objects(resolver);
    mdk::SurfaceCache surfaces(kRenderer);

    mdk::PrewarmBudget budget;
    for (int32_t m = 0; m < arena->modelCount(); m++) {
        MDK_CHECK(mdk::prewarmModel(&objects, arena->model(arena->modelName(m)), look, &surfaces,
                                    &budget));
    }
    MDK_CHECK(objects.layoutsBuilt() > 0);
    MDK_CHECK(objects.layoutsBuilt() <= arena->modelCount());
    MDK_CHECK(budget.made > 0);
    MDK_CHECK_EQ(surfaces.texturesMade(), budget.made);
    // Again: the layouts are kept and the textures are had.
    const int32_t layouts = objects.layoutsBuilt();
    mdk::PrewarmBudget again;
    for (int32_t m = 0; m < arena->modelCount(); m++) {
        MDK_CHECK(mdk::prewarmModel(&objects, arena->model(arena->modelName(m)), look, &surfaces,
                                    &again));
    }
    MDK_CHECK_EQ(objects.layoutsBuilt(), layouts);
    MDK_CHECK_EQ(again.made, 0);

    // THE WALK OVER OBJECTS: the scripts' objects, a few a step, the dead and
    // the model-less passed over, the look asked per object, and a lap wraps.
    std::deque<mdk::scripts::MdkObject> list(5);
    list[0].model = arena->model(arena->modelName(0));
    list[0].arena = a;
    list[1].model = nullptr;                   // an arena controller: passed over
    list[2].model = list[0].model;
    list[2].dead = true;                       // dead: passed over
    list[3].model = list[0].model;
    list[3].arena = "NOT_LOADED";              // its look says no: passed over
    list[4].model = list[0].model;
    list[4].arena = a;
    struct Looks {
        const mdk::ObjectLook* look;
        std::string loaded;
        int asked;
    } looks{&look, a, 0};
    const mdk::PrewarmLook lookOf = [](void* user, const std::string& name) -> const mdk::ObjectLook* {
        Looks* l = static_cast<Looks*>(user);
        l->asked++;
        return name == l->loaded ? l->look : nullptr;
    };
    mdk::ObjectPrewarm walk;
    mdk::PrewarmBudget step;
    MDK_CHECK_EQ(walk.step(&objects, list, lookOf, nullptr, &looks, &surfaces, &step, 3, false), 3);
    MDK_CHECK_EQ(looks.asked, 1);   // object 0 only: 1 has no model, 2 is dead
    MDK_CHECK_EQ(walk.step(&objects, list, lookOf, nullptr, &looks, &surfaces, &step, 3, false), 3);
    MDK_CHECK_EQ(looks.asked, 4);   // 3 (refused) and 4, then the lap wraps to 0
    MDK_CHECK_EQ(step.made, 0);     // everything was made above
    MDK_CHECK_EQ(walk.stats().visited, 6);
}
