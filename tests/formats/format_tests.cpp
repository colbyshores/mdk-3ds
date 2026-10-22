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
#include "formats/case_insensitive_path.h"
#include "formats/dti.h"
#include "formats/mdk_data.h"
#include "formats/mto.h"
#include "formats/texture_archive.h"
#include "game/level_data.h"

#include <stdio.h>
#include <stdlib.h>

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
