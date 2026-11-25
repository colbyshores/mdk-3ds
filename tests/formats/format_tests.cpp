// ============================================================================
// format_tests.cpp — the ports of the C# FormatTests.cs.
//
// The C# original is tests/Mdk.Formats.Tests/FormatTests.cs. The expected
// values are THEIRS, unchanged.
//
// THESE RUN AGAINST THE REAL GAME DATA. The C#'s tests need an MDK
// installation; the data is found the same way the game finds it, through
// the formats' own file-system seam.
//
// WHAT A PASS HERE PROVES. The C# could not be run on this machine (there
// is no .NET runtime), so the C#'s EXPECTED VALUES are the oracle: they are
// literals in the C# test file, and this file asserts the same literals
// against the C++ parser running on the real bytes. A green run therefore
// says the C++ parses the real data the way the C# does.
//
// WHAT IT DOES NOT PROVE. That the C# currently produces those values. The
// C#'s tests are the specification of the expected values, and they are
// believed to be green upstream; re-verifying them needs a .NET runtime.
// ============================================================================

#include "test_support.h"

#include "formats/arena.h"
#include "formats/bin_reader.h"
#include "formats/model_animation.h"
#include "formats/case_insensitive_path.h"
#include "formats/dti.h"
#include "formats/mdk_data.h"
#include "formats/mto.h"
#include "formats/texture_archive.h"
#include "game/level_data.h"
#include "scripts/object.h"
#include "scripts/pose_cache.h"

#include <stdio.h>
#include <stdlib.h>

#include <type_traits>

using namespace mdk::formats;

namespace {

// The game's levels are numbered 3 to 8.
const int kFirstLevel = 3;
const int kLastLevel = 8;

// The data's folder, found by walking up from the test binary's folder when
// the environment does not name it. The tests run from tests/formats, and
// the data is at the repository's root.
MdkData locateData() {
    if (const char* env = getenv("MDK_DATA_DIR")) {
        if (env[0] != '\0') {
            const FileSystem* system = fileSystem();
            const std::string marker = caseInsensitiveResolve(system, env, "TRAVERSE/TRAVSPRT.BNI");
            if (system->fileExists(system->user, marker.c_str())) {
                return MdkData(system, env);
            }
        }
    }
    // The candidates, relative to the test binary's folder.
    const char* relatives[] = {"../../MDK", "../../../MDK", "MDK"};
    const FileSystem* system = fileSystem();
    for (size_t i = 0; i < sizeof(relatives) / sizeof(relatives[0]); i++) {
        const std::string marker =
            caseInsensitiveResolve(system, relatives[i], "TRAVERSE/TRAVSPRT.BNI");
        if (system->fileExists(system->user, marker.c_str())) {
            return MdkData(system, relatives[i]);
        }
    }
    return MdkData();
}

// A file's bytes, read through the file-system seam.
bool readDataFile(const std::string& relative, std::vector<uint8_t>* bytes) {
    return locateData().read(relative, bytes);
}

} // namespace

// Every level parses, and every arena in it has a sane mesh.
MDK_TEST(EveryLevelParses) {
    const MdkData& game = locateData();
    MDK_CHECK(game.found());
    if (!game.found()) {
        return;
    }
    for (int level = kFirstLevel; level <= kLastLevel; level++) {
        char dtiPath[128];
        snprintf(dtiPath, sizeof(dtiPath), "TRAVERSE/LEVEL%d/LEVEL%d.DTI", level, level);
        char mtoPath[128];
        snprintf(mtoPath, sizeof(mtoPath), "TRAVERSE/LEVEL%d/LEVEL%dO.MTO", level, level);

        std::vector<uint8_t> dtiBytes;
        MDK_CHECK(readDataFile(dtiPath, &dtiBytes));
        Dti dti = Dti::parse(dtiBytes.data(), (int32_t)dtiBytes.size());
        MDK_CHECK(!dti.arenas().empty());

        std::vector<uint8_t> mtoBytes;
        MDK_CHECK(readDataFile(mtoPath, &mtoBytes));
        Mto mto = Mto::parse(mtoBytes.data(), (int32_t)mtoBytes.size());
        MDK_CHECK(!mto.arenaNames().empty());

        for (size_t i = 0; i < mto.arenaNames().size(); i++) {
            const Arena* arena = mto.getArena(mto.arenaNames()[i]);
            MDK_CHECK(arena != nullptr);
            if (!arena) {
                continue;
            }
            MDK_CHECK(!arena->vertices().empty());
            MDK_CHECK_EQ((int32_t)arena->triangleIndices().size(), arena->triangleCount() * 3);
            for (size_t t = 0; t < arena->triangleIndices().size(); t++) {
                const int32_t index = arena->triangleIndices()[t];
                MDK_CHECK(index >= 0 && index < (int32_t)arena->vertices().size());
            }
        }
    }
}

// Level 7 starts in DANT_1.
MDK_TEST(Level7StartsInDant1) {
    const MdkData& game = locateData();
    if (!game.found()) {
        return;
    }
    std::vector<uint8_t> dtiBytes;
    MDK_CHECK(readDataFile("TRAVERSE/LEVEL7/LEVEL7.DTI", &dtiBytes));
    Dti dti = Dti::parse(dtiBytes.data(), (int32_t)dtiBytes.size());
    MDK_CHECK(dti.startArena() >= 0 && dti.startArena() < (int32_t)dti.arenas().size());
    if (dti.startArena() >= 0 && dti.startArena() < (int32_t)dti.arenas().size()) {
        MDK_CHECK_STR_EQ(dti.arenas()[dti.startArena()].name, "DANT_1");
    }

    std::vector<uint8_t> mtoBytes;
    MDK_CHECK(readDataFile("TRAVERSE/LEVEL7/LEVEL7O.MTO", &mtoBytes));
    Mto mto = Mto::parse(mtoBytes.data(), (int32_t)mtoBytes.size());
    MDK_CHECK(mto.has("DANT_1"));
    const Arena* arena = mto.getArena("DANT_1");
    MDK_CHECK(arena != nullptr);
    if (arena) {
        MDK_CHECK_EQ(arena->paletteRgbBytes(), Arena::kPaletteColors * 3);
    }
}

// Level 3's HMO_1 arena carries its own models (the scripts' FindModel
// looks a type up here when it is not a global CMI model). The names and
// count are the C# oracle's, read from the same MTO.
MDK_TEST(Level3Hmo1ArenaModels) {
    const MdkData& game = locateData();
    if (!game.found()) {
        return;
    }
    std::vector<uint8_t> mtoBytes;
    MDK_CHECK(readDataFile("TRAVERSE/LEVEL3/LEVEL3O.MTO", &mtoBytes));
    Mto mto = Mto::parse(mtoBytes.data(), (int32_t)mtoBytes.size());
    const Arena* arena = mto.getArena("HMO_1");
    MDK_CHECK(arena != nullptr);
    if (!arena) {
        return;
    }
    MDK_CHECK_EQ(arena->modelCount(), 6);
    const char* expected[] = {"XE", "XED", "XU", "XU_MISS", "XH1_DOOR", "XG_BOMB1"};
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        MDK_CHECK(arena->model(expected[i]) != nullptr);
    }
    // XG is a global CMI model, not an arena one: it is not here.
    MDK_CHECK(arena->model("XG") == nullptr);
}

// LEVEL8's walls paint black with index 0: drawn opaque, as the original
// uploads every texture without kind bit 0 (0x474c9c). GUNT_2's beam showed
// the sky through them.
MDK_TEST(OnlyKeyedTexturesSeeThrough) {
    const MdkData& game = locateData();
    if (!game.found()) {
        return;
    }
    std::vector<uint8_t> mtoBytes;
    MDK_CHECK(readDataFile("TRAVERSE/LEVEL8/LEVEL8O.MTO", &mtoBytes));
    Mto mto = Mto::parse(mtoBytes.data(), (int32_t)mtoBytes.size());
    const Arena* arena = mto.getArena("GUNT_2");
    MDK_CHECK(arena != nullptr);
    if (arena) {
        const Texture* wall = arena->textures().findTexture("I2_WALL1");
        MDK_CHECK(wall != nullptr);
        if (wall) {
            bool sawZero = false;
            for (int32_t i = 0; i < wall->indexCount(); i++) {
                if (wall->indices()[i] == 0) {
                    sawZero = true;
                    break;
                }
            }
            MDK_CHECK(!sawZero);
        }
    }

    // The level's own archive, read as a standalone file: its count is 16
    // bytes after the size word.
    std::vector<uint8_t> mtiBytes;
    MDK_CHECK(readDataFile("TRAVERSE/LEVEL8/LEVEL8S.MTI", &mtiBytes));
    TextureArchive level = TextureArchive::parse(mtiBytes.data(), (int32_t)mtiBytes.size(),
                                                 TextureArchive::kFileBase);
    const Texture* bullet = level.findTexture("BULLET");
    MDK_CHECK(bullet != nullptr);
    if (bullet) {
        bool sawZero = false;
        for (int32_t i = 0; i < bullet->indexCount(); i++) {
            if (bullet->indices()[i] == 0) {
                sawZero = true;
                break;
            }
        }
        MDK_CHECK(!sawZero);
    }
    const Texture* explode = level.findTexture("EXPLODE");
    MDK_CHECK(explode != nullptr);
    if (explode) {
        bool sawZero = false;
        for (int32_t i = 0; i < explode->indexCount(); i++) {
            if (explode->indices()[i] == 0) {
                sawZero = true;
                break;
            }
        }
        MDK_CHECK(sawZero);
    }
}

// LevelData::entryNamed finds the DTI arena entry by name the way the C#'s
// EntryNamed does: a linear scan, matched with `entry.Name == name`, which is
// ordinal (case-sensitive). The VM's spawn path looks up an arena's object
// records through this, so it must return the same entry the DTI lists and
// null for a name that is not there.
MDK_TEST(EntryNamedFindsTheDtiArena) {
    const MdkData& game = locateData();
    if (!game.found()) {
        return;
    }
    mdk::LevelData level;
    MDK_CHECK(mdk::LevelData::load(&level, &game, 3));
    const Dti& dti = level.dti();
    MDK_CHECK(!dti.arenas().empty());

    // The starting arena is found and is the very entry the DTI lists.
    const int32_t start = dti.startArena();
    if (start >= 0 && start < (int32_t)dti.arenas().size()) {
        const std::string name = dti.arenas()[start].name;
        const Dti::ArenaEntry* entry = level.entryNamed(name);
        MDK_CHECK(entry != nullptr);
        if (entry) {
            MDK_CHECK_STR_EQ(entry->name, name);
            MDK_CHECK(entry == &dti.arenas()[start]);
        }
    }

    // HMO_1 is a level-3 arena and carries the object records the tick-1
    // spawn reads.
    const Dti::ArenaEntry* hmo1 = level.entryNamed("HMO_1");
    MDK_CHECK(hmo1 != nullptr);
    if (hmo1) {
        MDK_CHECK(!hmo1->records.empty());
    }

    // Case-sensitive, and an unknown name is null -- both like the C#.
    MDK_CHECK(level.entryNamed("hmo_1") == nullptr);
    MDK_CHECK(level.entryNamed("NO_SUCH_ARENA") == nullptr);
}

// ---------------------------------------------------------------------------
// WHO FREES AN ARENA
// ---------------------------------------------------------------------------

// THE PIN THAT CANNOT BE SILENCED AT RUNTIME. Arena::liveCount() is the only
// way a test can see an arena die, and the implicit copy and move operations do
// not go through the constructor and destructor that maintain it: one copy of a
// live arena makes the counter say one object while two exist, and deleting the
// original then says zero while the copy is still being drawn -- which would
// make every case below lie in whichever direction the copy happened. So the
// operations are deleted in arena.h and these four lines are what stops anyone
// un-deleting them and quietly breaking the instrument. A copy site anywhere in
// the tree is a compile error for the same reason.
static_assert(!std::is_copy_constructible<Arena>::value,
              "Arena must not be copy-constructible: liveCount() would under-count");
static_assert(!std::is_copy_assignable<Arena>::value,
              "Arena must not be copy-assignable: liveCount() would under-count");
static_assert(!std::is_move_constructible<Arena>::value,
              "Arena must not be movable: the source's destructor decrements for an "
              "object the destination is now using");
static_assert(!std::is_move_assignable<Arena>::value,
              "Arena must not be move-assignable: same reason as the move constructor");

// The Mto frees the arenas it parsed for itself.
//
// THE DEFECT THIS PINS. An Mto parsed arenas with `new` and nothing ever
// freed them. LevelData::~LevelData deletes the corridors and says the
// arenas "belong to the MTO", and the MTO had no destructor at all, so a
// level's arenas were lost on every level load, and one more arena per arena
// a script asked for late in a level (ScriptRuntime's FindModel path calls
// getArena too). ASan on this very target measured 47394202 bytes in 15442
// allocations with every stack under Mto::getArena.
MDK_TEST(MtoFreesTheArenasItParsed) {
    const MdkData& game = locateData();
    if (!game.found()) {
        return;
    }
    std::vector<uint8_t> mtoBytes;
    MDK_CHECK(readDataFile("TRAVERSE/LEVEL3/LEVEL3O.MTO", &mtoBytes));

    const int64_t before = Arena::liveCount();
    int64_t parsed = 0;
    {
        Mto mto = Mto::parse(mtoBytes.data(), (int32_t)mtoBytes.size());
        const size_t names = mto.arenaNames().size();
        MDK_CHECK(names > 0);
        for (size_t i = 0; i < names; i++) {
            if (mto.getArena(mto.arenaNames()[i])) {
                parsed++;
            }
        }
        MDK_CHECK(parsed > 0);
        // They are alive while the Mto is: the cache is what stops a corridor
        // that is re-entered from being parsed twice, so they must not die
        // early. This is also the proof that liveCount counts.
        MDK_CHECK_EQ(Arena::liveCount() - before, parsed);
    }
    // And they are gone with it. This is the assertion the leak violated.
    MDK_CHECK_EQ(Arena::liveCount(), before);
}

// The Mto does NOT free the arenas it was handed with add().
//
// The other half of the contract, and the reason a deleting destructor was
// not simply written: beta_demo.h's "THE ARENAS BELONG TO THE CALLER". A
// destructor that deleted everything in the cache would free the demo's
// arenas from under the vector that still holds them. This arm is also the
// positive control for the counter: it shows a live arena staying live, so
// the arm above cannot pass because the counter is stuck.
MDK_TEST(MtoKeepsTheArenasItWasGiven) {
    const int64_t before = Arena::liveCount();

    // A world section of nothing: four zero counts -- materials, nodes,
    // triangles, vertices -- is a valid empty arena.
    std::vector<uint8_t> empty(64, 0);
    Arena* mine = Arena::parseWorld("GIVEN_1", empty.data(), (int32_t)empty.size(), 0);
    MDK_CHECK(mine != nullptr);
    if (!mine) {
        return;
    }
    MDK_CHECK_EQ(Arena::liveCount(), before + 1);

    {
        Mto mto;
        mto.add(mine);
        MDK_CHECK(mto.has("GIVEN_1"));
        // The cache hands back the very object that was added, so the demo's
        // arenas are not re-parsed, and not adopted.
        MDK_CHECK(mto.getArena("GIVEN_1") == mine);
    }

    // The Mto died and did not take the arena with it.
    MDK_CHECK_EQ(Arena::liveCount(), before + 1);
    // And it is a live object, not a dangling pointer: reading its name is
    // reading memory somebody still owns.
    MDK_CHECK_STR_EQ(mine->name(), "GIVEN_1");
    delete mine;
    MDK_CHECK_EQ(Arena::liveCount(), before);
}

// Two bakes, frame by frame, part by part, vertex by vertex, bit for bit.
static bool samePose(const std::vector<std::vector<Vec3f>>& a,
                     const std::vector<std::vector<Vec3f>>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t p = 0; p < a.size(); p++) {
        if (a[p].size() != b[p].size() ||
            (!a[p].empty() && memcmp(a[p].data(), b[p].data(), a[p].size() * sizeof(Vec3f)) != 0)) {
            return false;
        }
    }
    return true;
}

static bool sameBake(const std::vector<std::vector<std::vector<Vec3f>>>& a,
                     const std::vector<std::vector<std::vector<Vec3f>>>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t f = 0; f < a.size(); f++) {
        if (a[f].size() != b[f].size()) {
            return false;
        }
        for (size_t p = 0; p < a[f].size(); p++) {
            if (a[f][p].size() != b[f][p].size() ||
                (!a[f][p].empty() &&
                 memcmp(a[f][p].data(), b[f][p].data(), a[f][p].size() * sizeof(Vec3f)) != 0)) {
                return false;
            }
        }
    }
    return true;
}

// ARENA STREAMING PHASE 1: THE MTO IS NOT RESIDENT, AND NOTHING READ FROM IT
// CHANGED. LevelData::load reads each arena's block by range (Mto::open) and
// frees it; an arena keeps only a copy of its models section, where its
// animations' tracks are decoded at bake time. If any track ran past that
// copy, its bake would silently fall back to the rest pose -- so this bakes
// EVERY (model, animation) pair of every arena of levels 3-8 both ways: from
// the streamed level, and from a reference parsed against the WHOLE file (the
// old, borrowing path), and requires them identical.
MDK_TEST(StreamedArenasBakeExactlyAsTheWholeFile) {
    const MdkData& game = locateData();
    if (!game.found()) {
        return;
    }
    int32_t pairs = 0;
    // The arenas' embedded sounds, a level: the sum of their entries' lengths
    // in LEVELnO.MTO (2026-11-18). They are the tail of each models section and
    // are NOT kept (Arena::parseModels); what is cut is that sum plus at most 3
    // bytes of padding an arena.
    const int32_t embeddedSounds[6] = {943740, 313960, 318536, 335482, 279976, 537680};
    for (int32_t number = 3; number <= 8; number++) {
        mdk::LevelData level;
        MDK_CHECK(mdk::LevelData::load(&level, &game, number));
        {
            int32_t cut = 0;
            int32_t kept = 0;
            for (size_t a = 0; a < level.arenas().size(); a++) {
                cut += level.arenas()[a]->embeddedSoundBytes();
                kept += level.arenas()[a]->keptModelBytes();
            }
            MDK_CHECK(cut >= embeddedSounds[number - 3]);
            MDK_CHECK(cut <= embeddedSounds[number - 3] + 3 * (int32_t)level.arenas().size());
            MDK_CHECK(kept > 0);
        }
        char path[64];
        snprintf(path, sizeof(path), "TRAVERSE/LEVEL%d/LEVEL%dO.MTO", (int)number, (int)number);
        std::vector<uint8_t> whole;
        MDK_CHECK(game.read(path, &whole));
        const Mto directory = Mto::parse(whole.data(), (int32_t)whole.size());
        for (size_t a = 0; a < directory.arenaNames().size(); a++) {
            const Arena* streamed = level.arenaNamed(directory.arenaNames()[a]);
            MDK_CHECK(streamed != nullptr);
            if (!streamed) {
                continue;
            }
            MDK_CHECK_EQ(streamed->droppedAnimations(), 0);
            // The reference: the models directory read straight from the file
            // (Arena::parse's offsets: the block's buffer starts after its size).
            const int32_t buf = directory.arenaOffsets()[a] + 4;
            const int32_t models = buf + (int32_t)binU32(whole.data(), buf);
            const int32_t base = models + 4;
            const uint32_t animationCount = binU32(whole.data(), base);
            MDK_CHECK_EQ((int32_t)animationCount, streamed->animationCount());
            // What the arena KEEPS of its models section is exactly the section
            // minus its embedded sounds: not less (a bake below would differ) and
            // not more -- keepAnimationBytes falls back to the WHOLE BLOCK when a
            // track starts outside the span it is given, which keeps every bake
            // right and would hide a cut made too early behind a megabyte of
            // memory (mutant M3, 2026-11-18).
            const int32_t palette = buf + (int32_t)binU32(whole.data(), buf + 4);
            if (animationCount > 0) {
                MDK_CHECK_EQ(streamed->keptModelBytes() + streamed->embeddedSoundBytes(),
                             palette - models);
            } else {
                MDK_CHECK_EQ(streamed->keptModelBytes(), 0);
            }
            for (uint32_t i = 0; i < animationCount; i++) {
                const int32_t entry = base + 12 + (int32_t)i * 12;
                char name[9] = {};
                memcpy(name, whole.data() + entry, 8);
                const int32_t offset = base + (int32_t)binU32(whole.data(), entry + 8);
                ModelAnimation reference;
                if (!ModelAnimation::parse(&reference, name, whole.data(), (int32_t)whole.size(),
                                           offset)) {
                    continue;
                }
                const ModelAnimation* mine = streamed->animation(name);
                MDK_CHECK(mine != nullptr);
                if (!mine) {
                    continue;
                }
                // Against EVERY model of the level, not only this arena's: the
                // scripts pair an arena's animation with whatever model the
                // object has (ScriptRuntime::getAnimation), and a foreign model
                // with more vertices on a same-named part reads further into
                // the track than the arena's own models do (review, 2026-11-18).
                for (size_t o = 0; o < level.arenas().size(); o++) {
                    const Arena* owner = level.arenas()[o];
                    for (int32_t m = 0; m < owner->modelCount(); m++) {
                        const Model* model = owner->model(owner->modelName(m));
                        if (!model || !reference.animates(*model)) {
                            continue;
                        }
                        pairs++;
                        MDK_CHECK(sameBake(mine->bake(*model), reference.bake(*model)));
                    }
                }
            }
        }
    }
    MDK_CHECK(pairs > 0);
    printf("  streamed arenas: %d (model, animation) pairs baked identically\n", (int)pairs);
}

// THE POSE CACHE BUILDS A FRAME AT A TIME (scripts/pose_cache.cpp, 2026-11-24): an
// object's pose_parts() decodes each part's track only as far as the frame asked
// for and makes only that frame, where it used to bake the whole animation at the
// first ask (56.5 ms of one Old 3DS step for XG's 101-frame CMI_e028d). Every
// (model, animation) pair of every arena of levels 3-8 -- each arena animation
// against every model of the level it moves -- is asked in an order that makes
// the decode resume (0, 1, 3, 2: frame 3 decodes past a frame not yet built), then
// the last frame, then every frame from the last down; each must be exactly the
// whole bake's frame. The first frame built must keep its address and bytes while
// the rest are built (pose_parts hands out a reference), and the clamp must answer
// frame 0 below the range and the last frame above it.
MDK_TEST(PoseCacheBuildsEachFrameAsTheWholeBake) {
    const MdkData& game = locateData();
    if (!game.found()) {
        return;
    }
    int32_t pairs = 0;
    int64_t frames = 0;
    for (int32_t number = 3; number <= 8; number++) {
        mdk::LevelData level;
        MDK_CHECK(mdk::LevelData::load(&level, &game, number));
        for (size_t a = 0; a < level.arenas().size(); a++) {
            const Arena* arena = level.arenas()[a];
            for (int32_t i = 0; i < arena->animationCount(); i++) {
                const ModelAnimation* animation = arena->animation(arena->animationName(i));
                if (animation == nullptr || animation->frameCount <= 0) {
                    continue;
                }
                for (size_t o = 0; o < level.arenas().size(); o++) {
                    const Arena* owner = level.arenas()[o];
                    for (int32_t m = 0; m < owner->modelCount(); m++) {
                        const Model* model = owner->model(owner->modelName(m));
                        if (model == nullptr || !animation->animates(*model)) {
                            continue;
                        }
                        pairs++;
                        const std::vector<std::vector<std::vector<Vec3f>>> whole = animation->bake(*model);
                        const int32_t last = animation->frameCount - 1;
                        mdk::scripts::MdkObject obj;
                        obj.model = model;
                        obj.animation = animation;
                        obj.animation_frame = 0;
                        const std::vector<std::vector<Vec3f>>* first = &obj.pose_parts();
                        MDK_CHECK(samePose(*first, whole[0]));
                        const int32_t opening[4] = {1, 3, 2, last};
                        for (int32_t k = 0; k < 4; k++) {
                            obj.animation_frame = opening[k] < last ? opening[k] : last;
                            MDK_CHECK(samePose(obj.pose_parts(),
                                               whole[(size_t)obj.animation_frame]));
                        }
                        for (int32_t f = last; f >= 0; f--) {
                            obj.animation_frame = f;
                            MDK_CHECK(samePose(obj.pose_parts(), whole[(size_t)f]));
                            frames++;
                        }
                        obj.animation_frame = 0;
                        MDK_CHECK(&obj.pose_parts() == first);
                        MDK_CHECK(samePose(*first, whole[0]));
                        obj.animation_frame = -3;
                        MDK_CHECK(samePose(obj.pose_parts(), whole[0]));
                        obj.animation_frame = last + 5;
                        MDK_CHECK(samePose(obj.pose_parts(), whole[(size_t)last]));
                    }
                }
            }
        }
        // The caches key on this level's addresses (scripts/pose_cache.h).
        mdk::scripts::forgetPoseCaches();
    }
    MDK_CHECK(pairs > 0);
    printf("  pose cache: %d (model, animation) pairs, %lld frames built one at a time "
           "as the whole bake\n", (int)pairs, (long long)frames);
}

// A short range read is a failure, not a truncated success (AGENTS.md P5).
MDK_TEST(RangeReadsAreExact) {
    const MdkData& game = locateData();
    if (!game.found()) {
        return;
    }
    std::vector<uint8_t> whole;
    MDK_CHECK(game.read("TRAVERSE/LEVEL3/LEVEL3O.MTO", &whole));
    std::vector<uint8_t> part;
    MDK_CHECK(game.readRange("TRAVERSE/LEVEL3/LEVEL3O.MTO", 100, 64, &part));
    MDK_CHECK(part.size() == 64 && memcmp(part.data(), whole.data() + 100, 64) == 0);
    MDK_CHECK(!game.readRange("TRAVERSE/LEVEL3/LEVEL3O.MTO", (int32_t)whole.size() - 8, 16, &part));
}

// A whole level gives back every arena it took: the MTO's and its own
// corridors. The two owners, and the two destructors, have to agree.
MDK_TEST(LevelDataReleasesItsArenasAndCorridors) {
    const MdkData& game = locateData();
    if (!game.found()) {
        return;
    }
    const int64_t before = Arena::liveCount();
    {
        mdk::LevelData level;
        MDK_CHECK(mdk::LevelData::load(&level, &game, 3));
        const int64_t alive = (int64_t)level.arenas().size();
        MDK_CHECK(alive > 0);
        // Every arena the level lists is one object, alive, and owned by
        // exactly one of the two: the MTO took the arenas, the level took the
        // corridors. No double count, no orphan.
        MDK_CHECK_EQ(Arena::liveCount() - before, alive);
    }
    MDK_CHECK_EQ(Arena::liveCount(), before);
}

// Assigning a different MTO over a live one releases what the live one owned.
//
// This is the shape of LevelData::load's `out->mto_ = Mto::parse(...)`, and it
// is why the move assignment is written by hand instead of defaulted: a
// defaulted move moves the owned_ vector out and drops the old Arena pointers
// on the floor, which is the leak above wearing the fix's clothes.
MDK_TEST(MtoAssignmentReleasesWhatItOwned) {
    const MdkData& game = locateData();
    if (!game.found()) {
        return;
    }
    std::vector<uint8_t> mtoBytes;
    MDK_CHECK(readDataFile("TRAVERSE/LEVEL3/LEVEL3O.MTO", &mtoBytes));

    const int64_t before = Arena::liveCount();
    Mto holder = Mto::parse(mtoBytes.data(), (int32_t)mtoBytes.size());
    MDK_CHECK(holder.getArena("HMO_1") != nullptr);
    MDK_CHECK(Arena::liveCount() > before);

    holder = Mto();
    MDK_CHECK_EQ(Arena::liveCount(), before);

    // And the object still works afterwards, rather than holding a buffer it
    // can no longer index.
    holder = Mto::parse(mtoBytes.data(), (int32_t)mtoBytes.size());
    MDK_CHECK(holder.getArena("HMO_1") != nullptr);
    MDK_CHECK(Arena::liveCount() > before);
}

// Loading a level into a LevelData that already holds one is a new level, not
// a second helping: the old arenas go, the old corridors go, and the count
// says one level's worth either way.
MDK_TEST(LevelDataLoadIntoALoadedLevelStartsOver) {
    const MdkData& game = locateData();
    if (!game.found()) {
        return;
    }
    const int64_t before = Arena::liveCount();
    mdk::LevelData level;
    MDK_CHECK(mdk::LevelData::load(&level, &game, 3));
    const int64_t oneLevel = Arena::liveCount() - before;
    MDK_CHECK(oneLevel > 0);
    MDK_CHECK_EQ((int64_t)level.arenas().size(), oneLevel);

    MDK_CHECK(mdk::LevelData::load(&level, &game, 3));
    MDK_CHECK_EQ(Arena::liveCount() - before, oneLevel);
    MDK_CHECK_EQ((int64_t)level.arenas().size(), oneLevel);
}

// A load that FAILS leaves an empty level, not a mixture of two levels.
//
// THE DEFECT THIS PINS. The reset at the top of LevelData::load released the
// arenas, the corridors and the palette cache, but not the DTI, the MTO's bytes
// or the level's texture archive -- and the reads that fill them are
// individually fallible, and a failed read does not touch the buffer it was
// given. So a load that failed after the reset kept the PREVIOUS level's DTI
// and textures in an object that claimed to hold another one, and archivesOf()
// searches the level archive for every arena's materials: the new level's
// arenas would resolve their surfaces against the old level's textures. It is
// unreachable with the shipped data (all six levels have their four files) and
// one step away from reachable with a mod, a corrupt install, or a level
// selector that tries the next number.
MDK_TEST(AFailedLoadLeavesAnEmptyLevel) {
    const MdkData& game = locateData();
    if (!game.found()) {
        return;
    }
    const int64_t before = Arena::liveCount();
    mdk::LevelData level;
    MDK_CHECK(mdk::LevelData::load(&level, &game, 3));
    MDK_CHECK(!level.arenas().empty());
    MDK_CHECK(!level.dti().arenas().empty());
    MDK_CHECK(!level.levelTextures().textures().empty());
    MDK_CHECK_EQ(level.number(), 3);
    MDK_CHECK(Arena::liveCount() > before);

    // Level 999 has no files at all, so this fails at the first read.
    MDK_CHECK(!mdk::LevelData::load(&level, &game, 999));

    // And nothing of level 3 is left: the arenas are gone with the MTO that
    // parsed them, and the DTI, the textures and the number are the empty
    // level's, not level 3's.
    MDK_CHECK_EQ(Arena::liveCount(), before);
    MDK_CHECK(level.arenas().empty());
    MDK_CHECK(level.dti().arenas().empty());
    MDK_CHECK(level.levelTextures().textures().empty());
    MDK_CHECK_EQ(level.number(), 0);

    // The object still works: a load of a real level afterwards is a level.
    MDK_CHECK(mdk::LevelData::load(&level, &game, 3));
    MDK_CHECK(!level.arenas().empty());
    MDK_CHECK(!level.levelTextures().textures().empty());
    MDK_CHECK_EQ(level.number(), 3);
    MDK_CHECK_EQ(Arena::liveCount() - before, (int64_t)level.arenas().size());
}
