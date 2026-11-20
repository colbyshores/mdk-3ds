// ============================================================================
// stream_tests.cpp -- the arena stream (docs/ARENA_STREAMING_SPEC.md Phase 2,
// mdk_core/src/game/arena_stream.h).
//
// What these pin:
//   - pixels that were dropped come back BYTE-IDENTICAL, for every texture of
//     every MTO arena of levels 3-8, by both paths (the 32 KiB stream and the
//     whole read), and the Texture objects never move;
//   - the state machine: which arena has the stream, when it is a step and when
//     it is a stall, that only ONE MTO arena keeps pixels, and what a reader of
//     evicted pixels gets;
//   - the three facts about the retail files the design rests on (connections
//     join an arena to a corridor; corridors use only the level's textures; one
//     arena borrows another's);
//   - a file that stops reading is a counted failure, not a retry per reader.
// ============================================================================

#include "test_support.h"

#include "formats/arena.h"
#include "formats/case_insensitive_path.h"
#include "formats/dti.h"
#include "formats/mdk_data.h"
#include "formats/mto.h"
#include "formats/sni.h"
#include "formats/texture_archive.h"
#include "game/arena_stream.h"
#include "game/arena_views.h"
#include "game/level_data.h"
#include "render/surface_cache.h"

#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

// The render seam's counters (render_seam_stubs.cpp).
extern int mdk_test_indexTextures;
extern int mdk_test_texturesReleased;
extern int mdk_test_palettes;
extern int mdk_test_palettesReleased;
extern int mdk_test_meshes;
extern int mdk_test_meshesReleased;
extern int mdk_test_meshDraws;
extern int mdk_test_nullIndexTextures;
extern int mdk_test_failMeshCreates;
extern int mdk_test_dynamicMeshes;
extern int mdk_test_meshUpdates;
extern std::vector<mdk::Vertex> mdk_test_lastUpdate;

namespace {

using namespace mdk::formats;

// A DTI record that joins two arenas (level_data.cpp kConnection).
const uint32_t kConnection = 6;

// The data's folder through `system` (format_tests.cpp's search).
MdkData locateDataWith(const FileSystem* system) {
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

MdkData locateData() {
    return locateDataWith(mdk::test::host::fileSystem());
}

// A file system whose range reads can be switched off: the harness's own, with
// one function replaced. Everything else forwards.
bool g_rangeReadsFail = false;
int32_t g_rangeReads = 0;

bool switchableReadRange(void* user, const char* path, int32_t offset, int32_t length,
                         std::vector<uint8_t>* outBytes) {
    g_rangeReads++;
    if (g_rangeReadsFail) {
        return false;
    }
    return mdk::test::host::readRange(user, path, offset, length, outBytes);
}

const FileSystem* switchableFileSystem() {
    static FileSystem instance;
    instance = *mdk::test::host::fileSystem();
    instance.readRange = switchableReadRange;
    return &instance;
}

std::string mtoPath(int32_t number) {
    char path[64];
    snprintf(path, sizeof(path), "TRAVERSE/LEVEL%d/LEVEL%dO.MTO", (int)number, (int)number);
    return path;
}

// Whether every texture of `arena` holds exactly the pixels of `reference`'s.
bool samePixels(const Arena* arena, const Arena* reference) {
    const std::vector<std::pair<std::string, Texture>>& a = arena->textures().textures();
    const std::vector<std::pair<std::string, Texture>>& b = reference->textures().textures();
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        const Texture& x = a[i].second;
        const Texture& y = b[i].second;
        if (a[i].first != b[i].first || x.width() != y.width() || x.height() != y.height() ||
            x.frameCount() != y.frameCount() || x.residentBytes() != y.residentBytes()) {
            return false;
        }
        // residentBytes, not indexCount: indexCount would fetch pixels that are
        // away, and this helper is asking whether they are in.
        if (x.residentBytes() > 0 &&
            memcmp(x.indices(), y.indices(), (size_t)x.residentBytes()) != 0) {
            return false;
        }
    }
    return true;
}

// The MTO arenas of a level, and its corridors.
std::vector<std::string> mtoArenas(const mdk::LevelData& level) {
    std::vector<std::string> names;
    for (size_t i = 0; i < level.arenas().size(); i++) {
        if (level.arenas()[i]->texturePrefixBytes() > 0) {
            names.push_back(level.arenas()[i]->name());
        }
    }
    return names;
}

std::string firstCorridor(const mdk::LevelData& level) {
    for (size_t i = 0; i < level.arenas().size(); i++) {
        if (level.arenas()[i]->texturePrefixBytes() == 0) {
            return level.arenas()[i]->name();
        }
    }
    return std::string();
}

} // namespace

// Every texture of every MTO arena, dropped and brought back by BOTH paths, is
// the texture the whole-file parse makes. The reference is parsed against the
// whole file (Mto::parse, the pre-streaming path), so this also proves a parse
// of just the block's prefix reads everything a texture needs -- the offsets a
// texture's pixels sit at are data, not a rule this code can assume.
MDK_TEST(StreamedPixelsAreTheWholeFilesPixels) {
    const MdkData game = locateData();
    if (!game.found()) {
        return;
    }
    int32_t arenasChecked = 0;
    int32_t texturesChecked = 0;
    int32_t emptyArenas = 0;
    for (int32_t number = 3; number <= 8; number++) {
        mdk::LevelData level;
        MDK_CHECK(mdk::LevelData::load(&level, &game, number));
        std::vector<uint8_t> whole;
        MDK_CHECK(game.read(mtoPath(number), &whole));
        Mto reference = Mto::parse(whole.data(), (int32_t)whole.size());

        const std::vector<std::string> names = mtoArenas(level);
        MDK_CHECK_EQ(names.size(), reference.arenaNames().size());
        // Nothing is resident after a load: the stream has not been told where
        // Kurt is, and nothing has read a texture.
        MDK_CHECK_EQ(level.stream().residentArenas(), 0);
        MDK_CHECK_EQ(level.stream().residentBytes(), (int64_t)0);

        for (size_t a = 0; a < names.size(); a++) {
            const Arena* arena = level.arenaNamed(names[a]);
            const Arena* expected = reference.getArena(names[a]);
            MDK_CHECK(arena != nullptr && expected != nullptr);
            if (!arena || !expected) {
                continue;
            }
            // The addresses the renderer's caches key on.
            std::vector<const Texture*> before;
            for (size_t t = 0; t < arena->textures().textures().size(); t++) {
                before.push_back(&arena->textures().textures()[t].second);
            }

            // Path 1: the whole read (a demand load's path).
            MDK_CHECK(level.stream().loadNow(names[a]));
            MDK_CHECK(samePixels(arena, expected));

            // Path 2: dropped by a switch to another arena, then streamed back
            // in 32 KiB steps as a PRELOAD (second, not active), counting them.
            const std::string other = names[(a + 1) % names.size()];
            const std::string corridor = firstCorridor(level);
            level.stream().update(other, std::string(), false);
            MDK_CHECK_EQ(arena->texturePixelBytes(), (int64_t)0);
            const int32_t stepsBefore = level.stream().stats().steps;
            level.stream().update(corridor, names[a], false);
            int32_t guard = 0;
            while (!level.stream().streaming().empty() && guard++ < 4096) {
                MDK_CHECK_EQ(arena->texturePixelBytes(), (int64_t)0);   // nothing until the last chunk
                level.stream().update(corridor, names[a], false);
            }
            // An arena with no pixels of its own (ten small ones across the six
            // levels) has nothing to stream: it takes the stream and reads nothing.
            const int32_t prefix = arena->texturePrefixBytes();
            const int32_t expectedSteps =
                expected->texturePixelBytes() == 0
                    ? 0
                    : (prefix + mdk::ArenaStream::kChunkBytes - 1) / mdk::ArenaStream::kChunkBytes;
            if (expected->texturePixelBytes() == 0) {
                emptyArenas++;
            }
            MDK_CHECK_EQ(level.stream().stats().steps - stepsBefore, expectedSteps);
            MDK_CHECK(samePixels(arena, expected));

            for (size_t t = 0; t < before.size(); t++) {
                MDK_CHECK(before[t] == &arena->textures().textures()[t].second);
            }
            arenasChecked++;
            texturesChecked += (int32_t)before.size();
        }
        MDK_CHECK_EQ(level.stream().stats().readFailures, 0);
    }
    MDK_CHECK(arenasChecked > 0 && texturesChecked > 0);
    printf("  arena stream: %d arenas (%d with no pixels of their own), %d textures "
           "byte-identical by both paths\n",
           (int)arenasChecked, (int)emptyArenas, (int)texturesChecked);
}

// The state machine on level 3, step by step, with the names the original's
// functions have (arena_stream.h).
MDK_TEST(TheStreamKeepsOneArenaAndShowsOnlyWholeOnes) {
    const MdkData game = locateData();
    if (!game.found()) {
        return;
    }
    mdk::LevelData level;
    MDK_CHECK(mdk::LevelData::load(&level, &game, 3));
    mdk::ArenaStream& stream = level.stream();
    const std::vector<std::string> names = mtoArenas(level);
    MDK_CHECK(names.size() >= 3);
    if (names.size() < 3) {
        return;
    }
    const std::string a = names[0];
    const std::string b = names[1];
    const std::string corridor = firstCorridor(level);
    MDK_CHECK(!corridor.empty());
    const Arena* arenaA = level.arenaNamed(a);
    const Arena* arenaB = level.arenaNamed(b);
    const int32_t prefixA = arenaA->texturePrefixBytes();
    const int32_t prefixB = arenaB->texturePrefixBytes();
    MDK_CHECK(prefixB > 2 * mdk::ArenaStream::kChunkBytes);

    // The level's start: Kurt's arena is shown, so it is read whole at once
    // (BSPShow at 0x41ba68) -- a stall of exactly its prefix.
    stream.update(a, std::string(), false);
    MDK_CHECK_STR_EQ(stream.resident(), a);
    MDK_CHECK(stream.streaming().empty());
    MDK_CHECK_EQ(stream.stats().switches, 1);
    MDK_CHECK_EQ(stream.stats().stalls, 1);
    MDK_CHECK_EQ(stream.stats().stallBytes, (int64_t)prefixA);
    MDK_CHECK_EQ(stream.residentArenas(), 1);
    MDK_CHECK(arenaA->texturePixelBytes() > 0);

    // A door opens onto a corridor, Kurt crosses into it, the door closes
    // behind him: corridors are not streamed, and the arena he left STAYS (the
    // original's buffer keeps it until another arena needs the stream).
    const int64_t readSoFar = stream.stats().bytesRead;
    stream.update(a, corridor, true);
    stream.update(corridor, a, true);
    stream.update(corridor, std::string(), false);
    MDK_CHECK_STR_EQ(stream.resident(), a);
    MDK_CHECK_EQ(stream.stats().switches, 1);
    MDK_CHECK_EQ(stream.stats().bytesRead, readSoFar);
    MDK_CHECK(arenaA->texturePixelBytes() > 0);

    // The next arena is PRELOADED (opcode 223 / a type-3 trigger: second, not
    // active): the switch drops the old arena's pixels at once and the new
    // one's arrive 32 KiB per frame, none usable before the last.
    stream.update(corridor, b, false);
    MDK_CHECK_STR_EQ(stream.resident(), b);
    MDK_CHECK_STR_EQ(stream.streaming(), b);
    MDK_CHECK_EQ(stream.stats().switches, 2);
    MDK_CHECK_EQ(stream.streamedBytes(), mdk::ArenaStream::kChunkBytes);
    MDK_CHECK_EQ(stream.streamTotalBytes(), prefixB);
    MDK_CHECK_EQ(arenaA->texturePixelBytes(), (int64_t)0);
    MDK_CHECK_EQ(arenaB->texturePixelBytes(), (int64_t)0);
    MDK_CHECK_EQ(stream.residentArenas(), 0);
    MDK_CHECK_EQ(stream.stats().stalls, 1);   // a preload is not a stall

    // The door opens before the stream is done: the arena is SHOWN, so the rest
    // is read now (BSPShow's loop) and counted as a stall of what was left.
    stream.update(corridor, b, true);
    MDK_CHECK(stream.streaming().empty());
    MDK_CHECK_EQ(stream.stats().stalls, 2);
    MDK_CHECK_EQ(stream.stats().stallBytes,
                 (int64_t)prefixA + (int64_t)(prefixB - mdk::ArenaStream::kChunkBytes));
    MDK_CHECK(arenaB->texturePixelBytes() > 0);
    MDK_CHECK_EQ(stream.residentArenas(), 1);

    // A reader of the evicted arena's pixels gets them (a demand load), and the
    // next update puts them away again: one MTO arena after every trim.
    MDK_CHECK_EQ(stream.stats().demandLoads, 0);
    const Texture& evicted = arenaA->textures().textures()[0].second;
    MDK_CHECK(evicted.pixelsAway());
    MDK_CHECK(evicted.indices() != nullptr);
    MDK_CHECK(!evicted.pixelsAway());
    MDK_CHECK_EQ(stream.stats().demandLoads, 1);
    MDK_CHECK_EQ(stream.stats().demandBytes, (int64_t)prefixA);
    MDK_CHECK_EQ(stream.residentArenas(), 2);
    // A second read of the same arena is not a second load.
    MDK_CHECK(arenaA->textures().textures().back().second.indices() != nullptr);
    MDK_CHECK_EQ(stream.stats().demandLoads, 1);
    stream.update(b, corridor, true);
    MDK_CHECK_EQ(stream.residentArenas(), 1);
    MDK_CHECK_EQ(arenaA->texturePixelBytes(), (int64_t)0);
    MDK_CHECK(stream.stats().trims >= 1);

    // In all of that -- everything walking can do -- never two arenas at a trim.
    MDK_CHECK_EQ(stream.stats().maxResidentArenas, 1);
    MDK_CHECK_EQ(stream.stats().pairFrames, 0);

    // A teleport's pair (two MTO arenas loaded, the second drawn): both are
    // kept, and it is counted rather than thrashed.
    stream.update(b, a, true);
    MDK_CHECK_EQ(stream.stats().pairFrames, 1);
    MDK_CHECK_STR_EQ(stream.resident(), b);
    MDK_CHECK(arenaA->texturePixelBytes() > 0 && arenaB->texturePixelBytes() > 0);
    MDK_CHECK_EQ(stream.stats().maxResidentArenas, 2);
    // The pair ends: back to one.
    stream.update(b, std::string(), false);
    MDK_CHECK_EQ(stream.residentArenas(), 1);
    MDK_CHECK_EQ(stream.stats().readFailures, 0);
}

// THE FACTS THE DESIGN RESTS ON, read from the retail files so a different data
// set says so here and not as a wrong picture:
//   1. every connection (a DTI type-6 record pair) joins an MTO arena to a
//      corridor, so walking never loads two MTO arenas together;
//   2. a corridor's materials come from the LEVEL's archive only, so a corridor
//      never depends on which arena has the stream;
//   3. one arena draws another arena's textures: level 6 OLYM_9, from OLYM_3
//      (the demand load's one retail customer).
MDK_TEST(TheLevelsAreArenasJoinedByCorridors) {
    const MdkData game = locateData();
    if (!game.found()) {
        return;
    }
    const int32_t expectedConnections[6] = {14, 14, 8, 16, 16, 16};
    int32_t borrowers = 0;
    std::string borrowerNames;
    for (int32_t number = 3; number <= 8; number++) {
        mdk::LevelData level;
        MDK_CHECK(mdk::LevelData::load(&level, &game, number));

        std::map<int32_t, std::vector<std::string>> byId;
        const std::vector<Dti::ArenaEntry>& entries = level.dti().arenas();
        for (size_t a = 0; a < entries.size(); a++) {
            for (size_t r = 0; r < entries[a].records.size(); r++) {
                if (entries[a].records[r].type == kConnection) {
                    byId[entries[a].records[r].id].push_back(entries[a].name);
                }
            }
        }
        int32_t joined = 0;
        for (std::map<int32_t, std::vector<std::string>>::const_iterator it = byId.begin();
             it != byId.end(); ++it) {
            MDK_CHECK_EQ(it->second.size(), (size_t)2);
            if (it->second.size() != 2) {
                continue;
            }
            const bool first = level.mto().has(it->second[0]);
            const bool second = level.mto().has(it->second[1]);
            MDK_CHECK(first != second);   // exactly one of the two is an MTO arena
            joined++;
        }
        MDK_CHECK_EQ(joined, expectedConnections[number - 3]);

        for (size_t a = 0; a < level.arenas().size(); a++) {
            const Arena* arena = level.arenas()[a];
            const bool corridor = arena->texturePrefixBytes() == 0;
            const std::vector<const TextureArchive*> archives = level.archivesOf(arena);
            int32_t foreign = 0;
            for (size_t m = 0; m < arena->materials().size(); m++) {
                for (size_t k = 0; k < archives.size(); k++) {
                    if (!archives[k]->findTexture(arena->materials()[m])) {
                        continue;
                    }
                    if (archives[k] != &arena->textures() && archives[k] != &level.levelTextures()) {
                        foreign++;
                    }
                    break;
                }
            }
            if (corridor) {
                MDK_CHECK_EQ(foreign, 0);
            } else if (foreign > 0) {
                borrowers++;
                borrowerNames += arena->name() + " ";
            }
        }
        // The census resolves by NAME: it must not have read a pixel.
        MDK_CHECK_EQ(level.stream().stats().demandLoads, 0);
    }
    MDK_CHECK_EQ(borrowers, 1);
    MDK_CHECK_STR_EQ(borrowerNames, "OLYM_9 ");
}

// Phase 5: the corridors come out of LEVELnO.SNI by its DIRECTORY, one entry
// read at a time, and the music around them is never read. A corridor parsed
// from its own entry's bytes must be the corridor the whole file parsed to --
// which is only true if a world section never reads outside its entry.
MDK_TEST(CorridorsReadByEntryAreTheWholeFiles) {
    const MdkData game = locateData();
    if (!game.found()) {
        return;
    }
    int32_t corridors = 0;
    int32_t placeholders = 0;
    for (int32_t number = 3; number <= 8; number++) {
        mdk::LevelData level;
        MDK_CHECK(mdk::LevelData::load(&level, &game, number));

        char path[96];
        snprintf(path, sizeof(path), "TRAVERSE/LEVEL%d/LEVEL%dO.SNI", (int)number, (int)number);
        std::vector<uint8_t> bytes;
        MDK_CHECK(game.read(path, &bytes));
        Sni whole;
        MDK_CHECK(Sni::parse(&whole, bytes.data(), (int32_t)bytes.size()));
        for (size_t e = 0; e < whole.entries().size(); e++) {
            const Sni::Entry& entry = whole.entries()[e];
            if (whole.isSound(entry)) {
                continue;
            }
            // The whole-file parse, exactly as level_data.cpp did it before.
            Arena* reference = Arena::parseWorld(entry.name, bytes.data(), (int32_t)bytes.size(),
                                                 entry.offset);
            MDK_CHECK(reference != nullptr);
            if (!reference) {
                continue;
            }
            const Arena* loaded = level.arenaNamed(entry.name);
            if (reference->materials().size() == 1 && reference->materials()[0] == "NONE") {
                placeholders++;
                MDK_CHECK(loaded == nullptr);   // a placeholder is not a place
                delete reference;
                continue;
            }
            MDK_CHECK(loaded != nullptr);
            if (loaded) {
                corridors++;
                MDK_CHECK(loaded->materials() == reference->materials());
                MDK_CHECK_EQ(loaded->vertices().size(), reference->vertices().size());
                MDK_CHECK_EQ(loaded->nodes().size(), reference->nodes().size());
                MDK_CHECK(loaded->triangleIndices() == reference->triangleIndices());
                MDK_CHECK(loaded->triangleMaterials() == reference->triangleMaterials());
                MDK_CHECK(loaded->triangleFlags() == reference->triangleFlags());
                MDK_CHECK(loaded->vertices().size() == reference->vertices().size() &&
                          memcmp(loaded->vertices().data(), reference->vertices().data(),
                                 loaded->vertices().size() * sizeof(Vec3f)) == 0);
                MDK_CHECK(loaded->triangleUvs().size() == reference->triangleUvs().size() &&
                          memcmp(loaded->triangleUvs().data(), reference->triangleUvs().data(),
                                 loaded->triangleUvs().size() * sizeof(Vec2f)) == 0);
                MDK_CHECK(loaded->nodes().size() == reference->nodes().size() &&
                          memcmp(loaded->nodes().data(), reference->nodes().data(),
                                 loaded->nodes().size() * sizeof(BspNode)) == 0);
                MDK_CHECK(!loaded->vertices().empty() && !loaded->nodes().empty());
            }
            delete reference;
        }
    }
    // Every level has corridors; the census is the files' own (2026-11-18).
    MDK_CHECK(corridors >= 40);
    printf("  corridors compared: %d (%d placeholders)\n", (int)corridors, (int)placeholders);
}

// A world section's counts are checked against the bytes that are left. What
// is NOT a world section -- a music track read as a corridor, which is what an
// archive misclassified at open would hand the level's load -- must come back
// as no geometry, at once, and not as a 1.18e9-material loop that ends when
// the allocator does (review, 2026-11-18).
MDK_TEST(AWorldSectionThatDoesNotFitIsNoGeometry) {
    // 'RIFF' as a material count, then nothing like enough bytes.
    std::vector<uint8_t> wav(4096, 0);
    memcpy(wav.data(), "RIFF", 4);
    Arena* notAWorld = Arena::parseWorld("WAV", wav.data(), (int32_t)wav.size(), 0);
    MDK_CHECK(notAWorld != nullptr);
    if (notAWorld) {
        MDK_CHECK(notAWorld->materials().empty());
        MDK_CHECK(notAWorld->nodes().empty());
        MDK_CHECK(notAWorld->vertices().empty());
        MDK_CHECK(notAWorld->triangleIndices().empty());
        delete notAWorld;
    }

    // Each count on its own, absurd, with the counts before it honest: nothing
    // may be SIZED by it. (A reader that has run off the end makes the next
    // bound refuse everything, so a missing bound in the middle gives the same
    // empty arena -- after an allocation of a gigabyte of nodes, which on the
    // console is the abort. Here it is the test binary that dies.)
    for (int32_t which = 0; which < 3; which++) {
        std::vector<uint8_t> section;
        const uint32_t counts[4] = {0, which == 0 ? 0x40000000u : 0,
                                    which == 1 ? 0x40000000u : 0,
                                    which == 2 ? 0x40000000u : 0};
        for (int32_t k = 0; k < 4 && k <= which + 1; k++) {
            for (int32_t b = 0; b < 4; b++) {
                section.push_back((uint8_t)(counts[k] >> (8 * b)));
            }
        }
        section.resize(section.size() + 64, 0);
        Arena* absurd = Arena::parseWorld("ABSURD", section.data(), (int32_t)section.size(), 0);
        MDK_CHECK(absurd != nullptr);
        if (absurd) {
            MDK_CHECK(absurd->nodes().empty());
            MDK_CHECK(absurd->triangleIndices().empty());
            MDK_CHECK(absurd->triangleFlags().empty());
            MDK_CHECK(absurd->vertices().empty());
            delete absurd;
        }
    }

    const MdkData game = locateData();
    if (!game.found()) {
        return;
    }
    // A REAL corridor cut short is EITHER the whole corridor (the cut fell in
    // the BSP leaf data that follows the vertices, which this parser does not
    // read) OR no geometry at all -- never half of one. And the same bytes whole
    // are the corridor: the bounds refuse what does not fit and nothing that
    // does.
    std::vector<uint8_t> bytes;
    MDK_CHECK(game.read("TRAVERSE/LEVEL3/LEVEL3O.SNI", &bytes));
    Sni whole;
    MDK_CHECK(Sni::parse(&whole, bytes.data(), (int32_t)bytes.size()));
    int32_t tried = 0;
    int32_t refused = 0;
    for (size_t e = 0; e < whole.entries().size(); e++) {
        const Sni::Entry& entry = whole.entries()[e];
        if (whole.isSound(entry) || entry.length < 4096) {
            continue;
        }
        std::vector<uint8_t> corridor;
        MDK_CHECK(whole.getBytes(entry, &corridor));
        Arena* full = Arena::parseWorld(entry.name, corridor.data(), (int32_t)corridor.size(), 0);
        MDK_CHECK(full != nullptr && !full->vertices().empty() && !full->nodes().empty() &&
                  !full->materials().empty());
        // Cut at 8 bytes and at every 32nd of the entry: every section of every
        // corridor (materials, nodes, triangles, vertices) is cut somewhere.
        const int32_t kCuts = 33;
        for (int32_t c = 0; c < kCuts; c++) {
            const int32_t at = c == 0 ? 8 : (int32_t)((int64_t)corridor.size() * c / kCuts);
            Arena* cut = Arena::parseWorld(entry.name, corridor.data(), at, 0);
            MDK_CHECK(cut != nullptr);
            if (cut && full) {
                // The same in CONTENT: a section sized by a count the bytes do
                // not hold reads as zeros past the end, and has the right sizes.
                const bool same =
                    cut->materials() == full->materials() &&
                    cut->nodes().size() == full->nodes().size() &&
                    memcmp(cut->nodes().data(), full->nodes().data(),
                           full->nodes().size() * sizeof(BspNode)) == 0 &&
                    cut->triangleIndices() == full->triangleIndices() &&
                    cut->triangleFlags() == full->triangleFlags() &&
                    cut->vertices().size() == full->vertices().size() &&
                    memcmp(cut->vertices().data(), full->vertices().data(),
                           full->vertices().size() * sizeof(Vec3f)) == 0;
                const bool none = cut->materials().empty() && cut->nodes().empty() &&
                                  cut->triangleIndices().empty() &&
                                  cut->triangleMaterials().empty() &&
                                  cut->triangleUvs().empty() && cut->triangleFlags().empty() &&
                                  cut->vertices().empty();
                MDK_CHECK(same || none);
                refused += none ? 1 : 0;
                // Eight bytes cannot be a corridor.
                if (c == 0) {
                    MDK_CHECK(none);
                }
            }
            delete cut;
            tried++;
        }
        delete full;
    }
    MDK_CHECK(tried >= 8);
    MDK_CHECK(refused >= tried / 2);
}

// A file that stops reading: the stream's own read fails once and is abandoned;
// readers then try (each a counted failure) until the arena is left alone, after
// which a read costs no I/O at all and simply finds no pixels.
MDK_TEST(AFailingFileIsACountedFailureNotARetryPerReader) {
    const MdkData game = locateDataWith(switchableFileSystem());
    if (!game.found()) {
        return;
    }
    g_rangeReadsFail = false;
    mdk::LevelData level;
    MDK_CHECK(mdk::LevelData::load(&level, &game, 3));
    mdk::ArenaStream& stream = level.stream();
    const std::vector<std::string> names = mtoArenas(level);
    MDK_CHECK(!names.empty());
    if (names.empty()) {
        return;
    }
    const Arena* arena = level.arenaNamed(names[0]);
    const Texture& texture = arena->textures().textures()[0].second;

    g_rangeReadsFail = true;
    stream.update(names[0], std::string(), false);
    MDK_CHECK_EQ(stream.stats().readFailures, 1);
    MDK_CHECK(stream.streaming().empty());
    MDK_CHECK_STR_EQ(stream.resident(), names[0]);
    MDK_CHECK_EQ(arena->texturePixelBytes(), (int64_t)0);

    for (int32_t i = 0; i < 10; i++) {
        MDK_CHECK_EQ(texture.indexCount(), 0);
        MDK_CHECK(texture.frame(0) == nullptr);
    }
    MDK_CHECK_EQ(stream.stats().readFailures, mdk::ArenaStream::kMaxReadFailures);
    const int32_t readsWhenLatched = g_rangeReads;
    for (int32_t i = 0; i < 10; i++) {
        MDK_CHECK_EQ(texture.indexCount(), 0);
        stream.update(names[0], std::string(), false);
    }
    MDK_CHECK_EQ(g_rangeReads, readsWhenLatched);   // no I/O once latched
    MDK_CHECK_EQ(stream.stats().readFailures, mdk::ArenaStream::kMaxReadFailures);

    // A level whose MTO cannot be read at all does not load.
    mdk::LevelData broken;
    MDK_CHECK(!mdk::LevelData::load(&broken, &game, 4));
    MDK_CHECK_EQ(broken.arenas().size(), (size_t)0);
    g_rangeReadsFail = false;
}

namespace {

// The arenas a connection (a type-6 record pair) joins `name` to.
std::vector<std::string> neighbours(const mdk::LevelData& level, const std::string& name) {
    std::map<int32_t, std::vector<std::string>> byId;
    const std::vector<Dti::ArenaEntry>& entries = level.dti().arenas();
    for (size_t a = 0; a < entries.size(); a++) {
        for (size_t r = 0; r < entries[a].records.size(); r++) {
            if (entries[a].records[r].type == kConnection) {
                byId[entries[a].records[r].id].push_back(entries[a].name);
            }
        }
    }
    std::vector<std::string> out;
    for (std::map<int32_t, std::vector<std::string>>::const_iterator it = byId.begin();
         it != byId.end(); ++it) {
        if (it->second.size() != 2) {
            continue;
        }
        if (it->second[0] == name) out.push_back(it->second[1]);
        if (it->second[1] == name) out.push_back(it->second[0]);
    }
    return out;
}

} // namespace

// THE WALK: arena A, the door to corridor C, through it, the next arena B
// preloading, its door, into B -- what the frontends do every frame (scripts'
// arenas -> ArenaStream::update -> ArenaViews::sync -> draw), against the seam's
// counters. The claims:
//   - a view exists for exactly the arenas the header says are kept;
//   - an arena that is drawn has its pixels: no texture is ever made from pixels
//     that are away, and WALKING NEVER FAULTS (zero demand loads) -- the stream
//     is ahead of every reader;
//   - an arena that stops being kept gives back its mesh and every texture made
//     through its palette, and at the end nothing is left on the GPU.
MDK_TEST(TheViewsFollowTheWalk) {
    const MdkData game = locateData();
    if (!game.found()) {
        return;
    }
    mdk::LevelData level;
    MDK_CHECK(mdk::LevelData::load(&level, &game, 3));
    mdk::ArenaStream& stream = level.stream();

    // A - C - B: the level's start arena, a corridor it connects to, and the
    // other arena on that corridor.
    const int32_t start = level.dti().startArena();
    const std::string a = level.dti().arenas()[(size_t)start].name;
    MDK_CHECK(level.mto().has(a));
    std::string c;
    std::string b;
    const std::vector<std::string> corridors = neighbours(level, a);
    for (size_t i = 0; i < corridors.size() && b.empty(); i++) {
        const std::vector<std::string> beyond = neighbours(level, corridors[i]);
        for (size_t k = 0; k < beyond.size(); k++) {
            if (beyond[k] != a && level.mto().has(beyond[k])) {
                c = corridors[i];
                b = beyond[k];
                break;
            }
        }
    }
    MDK_CHECK(!c.empty() && !b.empty());
    if (c.empty() || b.empty()) {
        return;
    }

    const MdkRenderer renderer = reinterpret_cast<MdkRenderer>(static_cast<uintptr_t>(1));
    mdk::SurfaceCache surfaces(renderer);
    mdk::ArenaViews views;
    views.start(renderer, &level, &surfaces, mdk::Shading::Lit);
    const int meshes0 = mdk_test_meshes;
    const int meshesReleased0 = mdk_test_meshesReleased;
    const int textures0 = mdk_test_indexTextures;
    const int texturesReleased0 = mdk_test_texturesReleased;
    const int palettes0 = mdk_test_palettes;
    const int palettesReleased0 = mdk_test_palettesReleased;
    const auto liveMeshes = [&]() {
        return (mdk_test_meshes - meshes0) - (mdk_test_meshesReleased - meshesReleased0);
    };
    const auto liveTextures = [&]() {
        return (mdk_test_indexTextures - textures0) - (mdk_test_texturesReleased - texturesReleased0);
    };
    const int nullTextures0 = mdk_test_nullIndexTextures;
    // One frame, as a frontend runs it: the pair, and ONE drawn list for the
    // stream, the views and the draw.
    const auto frameDrawing = [&](const std::string& current, const std::string& second,
                                  bool active, const std::vector<std::string>& drawn) {
        stream.update(current, second, active, drawn);
        views.sync(current, second, drawn);
        views.draw(drawn);
    };
    const auto frame = [&](const std::string& current, const std::string& second, bool active) {
        std::vector<std::string> drawn;
        drawn.push_back(current);
        if (active && !second.empty()) {
            drawn.push_back(second);
        }
        frameDrawing(current, second, active, drawn);
    };

    // The level starts in A.
    frame(a, std::string(), false);
    MDK_CHECK_EQ(views.count(), 1);
    MDK_CHECK(views.find(a) != nullptr);
    MDK_CHECK(mdk_test_meshDraws > 0);
    const int texturesOfA = liveTextures();
    MDK_CHECK(texturesOfA > 0);

    // The door to C opens; Kurt walks through; the door closes behind him.
    frame(a, c, true);
    MDK_CHECK_EQ(views.count(), 2);
    MDK_CHECK(liveTextures() > texturesOfA);   // the corridor's, in its own colours
    frame(c, a, true);
    MDK_CHECK_EQ(views.count(), 2);
    frame(c, std::string(), false);
    // A is still kept: it has the stream, and turning back costs nothing.
    MDK_CHECK_EQ(views.count(), 2);
    MDK_CHECK(views.find(a) != nullptr);
    MDK_CHECK_EQ(views.stats().released, 0);
    const int texturesBeforeSwitch = liveTextures();

    // B is preloaded: the stream switches, A is no longer kept, and A's mesh
    // and every texture made through A's palette go. B has a view at once (its
    // door can open on any frame) but is not drawn, and nothing reads a pixel.
    frame(c, b, false);
    MDK_CHECK_STR_EQ(stream.resident(), b);
    MDK_CHECK(views.find(a) == nullptr);
    MDK_CHECK(views.find(b) != nullptr);
    MDK_CHECK_EQ(views.count(), 2);
    MDK_CHECK_EQ(views.stats().released, 1);
    MDK_CHECK_EQ(liveTextures(), texturesBeforeSwitch - texturesOfA);
    MDK_CHECK_EQ(liveMeshes(), 2);
    int32_t guard = 0;
    while (!stream.streaming().empty() && guard++ < 4096) {
        frame(c, b, false);
    }
    MDK_CHECK_EQ(stream.stats().stalls, 1);   // only the level's start

    // B's door opens, Kurt walks in, the door closes.
    frame(c, b, true);
    MDK_CHECK(liveTextures() > texturesBeforeSwitch - texturesOfA);
    frame(b, c, true);
    frame(b, std::string(), false);
    // C is a corridor connected to the resident arena: kept, as the original
    // keeps it loaded.
    MDK_CHECK_EQ(views.count(), 2);
    MDK_CHECK(views.find(c) != nullptr);

    // The whole walk: the stream was ahead of every reader, and no texture was
    // ever made from pixels that were away.
    MDK_CHECK_EQ(stream.stats().demandLoads, 0);
    MDK_CHECK_EQ(mdk_test_nullIndexTextures - nullTextures0, 0);
    MDK_CHECK_EQ(stream.stats().maxResidentArenas, 1);
    MDK_CHECK_EQ(stream.stats().readFailures, 0);
    MDK_CHECK_EQ(views.stats().failed, 0);
    // A SCRIPTED TELEPORT's frame (opcodes 112/173 run in the middle of the
    // scripts' tick): the drawn list still says B, the pair already says A. B is
    // drawn this frame, so it keeps its pixels and its view, and nothing faults;
    // A streams in behind it. The next frame draws A and B goes.
    {
        const int32_t demandBefore = stream.stats().demandLoads;
        const int32_t releasedBefore = views.stats().released;
        std::vector<std::string> stillB;
        stillB.push_back(b);
        frameDrawing(a, std::string(), false, stillB);
        MDK_CHECK_STR_EQ(stream.resident(), a);
        MDK_CHECK(views.find(b) != nullptr);
        // Kept, not released and built again in the same breath.
        MDK_CHECK_EQ(views.stats().released, releasedBefore);
        MDK_CHECK(level.arenaNamed(b)->texturePixelBytes() > 0);
        MDK_CHECK(views.find(a) != nullptr);
        MDK_CHECK_EQ(stream.stats().demandLoads, demandBefore);
        MDK_CHECK_EQ(mdk_test_nullIndexTextures - nullTextures0, 0);
        frame(a, std::string(), false);
        MDK_CHECK(views.find(b) == nullptr);
        MDK_CHECK_EQ(level.arenaNamed(b)->texturePixelBytes(), (int64_t)0);
        MDK_CHECK(level.arenaNamed(a)->texturePixelBytes() > 0);
        MDK_CHECK_EQ(stream.stats().demandLoads, demandBefore);
        // Back to where the walk ended, for the checks below.
        frame(c, a, true);
        frame(c, b, false);
        int32_t spin = 0;
        while (!stream.streaming().empty() && spin++ < 4096) {
            frame(c, b, false);
        }
        frame(b, c, true);
        frame(b, std::string(), false);
    }

    // A WARP: Kurt is in A and then, the next frame, in B and B is drawn (a
    // teleport as the frame after it sees it, a respawn). B is shown at once, so
    // it is read whole in this one update -- and A must be gone BEFORE that read.
    frame(a, std::string(), false);
    frame(b, std::string(), false);
    MDK_CHECK(level.arenaNamed(b)->texturePixelBytes() > 0);
    MDK_CHECK_EQ(level.arenaNamed(a)->texturePixelBytes(), (int64_t)0);

    // THE PEAK, over the walk, the teleport and the warp: because whatever stops
    // being kept is dropped BEFORE anything is read, the most pixel bytes ever
    // resident is ONE arena's -- measured at every adoption, not after a trim.
    {
        // Read BEFORE the two loads below, which are how the arenas' sizes are
        // learnt and which hold both at once themselves.
        const int64_t peak = stream.stats().peakPixelBytes;
        int64_t largest = 0;
        const std::string walked[2] = {a, b};
        for (int32_t i = 0; i < 2; i++) {
            stream.loadNow(walked[i]);
            const int64_t bytes = level.arenaNamed(walked[i])->texturePixelBytes();
            if (bytes > largest) {
                largest = bytes;
            }
        }
        MDK_CHECK(peak > 0);
        MDK_CHECK(peak <= largest);
        frame(b, std::string(), false);   // the two loads above are trimmed again
    }

    // A VIEW THAT CANNOT BE BUILT IS TRIED AGAIN (a transient miss of GPU memory
    // must not hide an arena for the rest of the level): the next sync, here.
    {
        frame(c, std::string(), false);
        frame(c, a, false);                 // A preloads: B's view goes
        MDK_CHECK(views.find(b) == nullptr);
        const int32_t failedBefore = views.stats().failed;
        mdk_test_failMeshCreates = 1;
        frame(c, b, false);                 // B's view: the mesh is refused once
        MDK_CHECK(views.find(b) == nullptr);
        MDK_CHECK_EQ(views.stats().failed, failedBefore + 1);
        frame(c, b, false);                 // ...and built on the retry
        MDK_CHECK(views.find(b) != nullptr);
        MDK_CHECK_EQ(mdk_test_failMeshCreates, 0);
    }

    // AN ORPHAN: a texture made through the palette of an arena that has no view
    // (an effect left behind in an arena Kurt walked away from is still drawn).
    // Nothing would ever give it back but the sweep.
    {
        const Arena* far = nullptr;
        for (size_t i = 0; i < level.arenas().size() && !far; i++) {
            const std::string& name = level.arenas()[i]->name();
            if (name != a && name != b && name != c && !views.find(name) &&
                level.mto().has(name)) {
                far = level.arenas()[i];
            }
        }
        MDK_CHECK(far != nullptr);
        if (far && !level.levelTextures().textures().empty()) {
            const Palette* farPalette = &level.paletteOf(far);
            const int before = liveTextures();
            const int32_t sweptBefore = views.stats().orphansReleased;
            MDK_CHECK(surfaces.textureId(&level.levelTextures().textures()[0].second, farPalette) >= 0);
            MDK_CHECK_EQ(liveTextures(), before + 1);
            MDK_CHECK(surfaces.hasPalette(farPalette) || surfaces.paletteId(farPalette) >= 0);
            for (int32_t i = 0; i < mdk::ArenaViews::kOrphanSweepSyncs; i++) {
                views.sync(c, b);
            }
            MDK_CHECK_EQ(liveTextures(), before);
            MDK_CHECK(!surfaces.hasPalette(farPalette));
            MDK_CHECK_EQ(views.stats().orphansReleased, sweptBefore + 1);
        }
    }

    MDK_CHECK_EQ(stream.stats().readFailures, 0);

    // Everything goes back.
    views.clear();
    MDK_CHECK_EQ(views.count(), 0);
    MDK_CHECK_EQ(liveMeshes(), 0);
    MDK_CHECK_EQ(liveTextures(), 0);
    MDK_CHECK_EQ(mdk_test_palettes - palettes0, mdk_test_palettesReleased - palettesReleased0);
}

// THE END OF A LEVEL'S TORN ARENA (LevelView::tear, the C#'s LevelView.Tear): the first
// tear swaps the static mesh for a dynamic one of the same size (the static one released),
// every draw then rewrites it (a dynamic mesh's slot holds only what its own frame wrote),
// a torn triangle's three corners are its first corner, the triangles not torn keep
// theirs, and a tear with nothing new changes nothing.
MDK_TEST(ATornArenaIsRewrittenEveryFrameWithItsTrianglesCollapsed) {
    const MdkData game = locateData();
    if (!game.found()) {
        return;
    }
    mdk::LevelData level;
    MDK_CHECK(mdk::LevelData::load(&level, &game, 3));
    const std::string a = level.dti().arenas()[(size_t)level.dti().startArena()].name;
    MDK_CHECK(level.stream().loadNow(a));
    const MdkRenderer renderer = reinterpret_cast<MdkRenderer>(static_cast<uintptr_t>(1));
    mdk::SurfaceCache surfaces(renderer);
    mdk::ArenaViews views;
    views.start(renderer, &level, &surfaces, mdk::Shading::Lit);
    std::vector<std::string> drawn(1, a);
    views.sync(a, std::string(), drawn);
    const mdk::LevelView* view = views.find(a);
    MDK_CHECK(view != nullptr);
    if (view == nullptr) {
        return;
    }
    std::vector<int32_t> firsts;
    MDK_CHECK(mdk::arenaSceneTriangleFirsts(view->arena(), &firsts));
    // Two triangles the scene placed.
    std::vector<int32_t> torn;
    for (size_t t = 0; t < firsts.size() && torn.size() < 2; t++) {
        if (firsts[t] >= 0) {
            torn.push_back((int32_t)t);
        }
    }
    MDK_CHECK_EQ((int)torn.size(), 2);
    const std::vector<mdk::Vertex> whole = view->scene().vertices;

    const int dynamic0 = mdk_test_dynamicMeshes;
    const int released0 = mdk_test_meshesReleased;
    const int updates0 = mdk_test_meshUpdates;
    views.draw(drawn);
    MDK_CHECK_EQ(mdk_test_meshUpdates, updates0);  // whole: the static mesh, no rewrite
    std::vector<int32_t> first(1, torn[0]);
    MDK_CHECK(views.tear(a, first));
    MDK_CHECK_EQ(mdk_test_dynamicMeshes, dynamic0 + 1);
    MDK_CHECK_EQ(mdk_test_meshesReleased, released0 + 1);
    views.draw(drawn);
    views.draw(drawn);
    MDK_CHECK_EQ(mdk_test_meshUpdates, updates0 + 2);  // every frame that draws it
    MDK_CHECK_EQ(mdk_test_lastUpdate.size(), whole.size());
    const size_t f0 = (size_t)firsts[(size_t)torn[0]];
    const size_t f1 = (size_t)firsts[(size_t)torn[1]];
    for (int k = 1; k < 3; k++) {
        MDK_CHECK(mdk_test_lastUpdate[f0 + k].pos.x == whole[f0].pos.x &&
                  mdk_test_lastUpdate[f0 + k].pos.y == whole[f0].pos.y &&
                  mdk_test_lastUpdate[f0 + k].pos.z == whole[f0].pos.z);
        MDK_CHECK(mdk_test_lastUpdate[f1 + k].pos.x == whole[f1 + k].pos.x &&
                  mdk_test_lastUpdate[f1 + k].pos.y == whole[f1 + k].pos.y &&
                  mdk_test_lastUpdate[f1 + k].pos.z == whole[f1 + k].pos.z);
    }
    // The second tear collapses only the new triangle, and makes no second mesh.
    MDK_CHECK(views.tear(a, torn));
    MDK_CHECK(views.tear(a, torn));
    MDK_CHECK_EQ(mdk_test_dynamicMeshes, dynamic0 + 1);
    views.draw(drawn);
    for (int k = 1; k < 3; k++) {
        MDK_CHECK(mdk_test_lastUpdate[f1 + k].pos.x == whole[f1].pos.x &&
                  mdk_test_lastUpdate[f1 + k].pos.y == whole[f1].pos.y &&
                  mdk_test_lastUpdate[f1 + k].pos.z == whole[f1].pos.z);
    }
    // An arena with no view is the C#'s early return.
    MDK_CHECK(views.tear("NO_SUCH_ARENA", torn));
    views.clear();
}

