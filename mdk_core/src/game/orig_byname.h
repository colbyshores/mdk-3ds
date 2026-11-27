#pragma once

// ============================================================================
// orig_byname.h -- the orig pack's NAME INDEX for one level: (palette, name) ->
// the orig file, its source size and its HD file. The 3DS's surface lookup
// (mdk_3ds/src/packs_3ds.cpp hdPackLookup) answers from it, so play never
// content-hashes a texture to find its file (docs/HD_TEXTURES_3DS_SPEC.md 3.1).
//
// WHERE IT COMES FROM (docs/3DS_ARCHITECTURE.md 5): the level's block of the
// build-time table romfs:/orig/byname.bin (formats/byname_index.h) -- two
// reads, no parse. Only when that table is missing, does not parse, or was
// built from other inputs than the RomFS holds (its stamp) is the block made
// from byname.txt instead, the way every load made it before the table (2.5 s
// on the Old 3DS); the reason is reported and the answers are the same.
//
// WHAT STAYS AT LOAD: everything that depends on the loaded level or on the
// SD card. A row's arena NAME becomes its palette (LevelData::paletteOf: a
// corridor shares its arena's palette object), the rows that name one
// (palette, name) are resolved in file order exactly as the text index's map
// was (a later row replaces an earlier one, except a corridor's row that names
// another file than its arena's), and each answer is joined with the HD pack's
// manifest by content key -- the SD pack ships apart from the .3dsx, so that
// join cannot be built in: ~1300 map finds a level, no file read.
//
// Plain data, no 3DS calls: the host tests build it for every level.
// ============================================================================

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "formats/byname_index.h"

namespace mdk {

class LevelData;

// The orig manifest as the pack loader read it: its size and FNV-1a 64 hash
// (formats::BynameIndex::hash), what byname.bin's stamp must match.
struct OrigManifestStamp {
    bool valid = false;
    uint32_t bytes = 0;
    uint64_t hash = 0;
};

struct OrigByName {
    OrigByName() = default;
    // Not copied: `hd` and `hdArenaRows` point into this object's own hdByKey.
    OrigByName(const OrigByName&) = delete;
    OrigByName& operator=(const OrigByName&) = delete;

    // The level's rows (formats/byname_index.h), from byname.bin or byname.txt.
    formats::BynameIndex index;
    // The HD pack's files by content key (loadHdPack), kept: `hd` points into it.
    std::map<std::string, std::string> hdByKey;
    // Per arena of the index: its palette (null: the level has no such arena,
    // so its rows answer nothing).
    std::vector<const void*> arenaPalettes;
    // Per row: 1 when it is the answer for its (palette, name).
    std::vector<uint8_t> answer;
    // Per row: the answer's HD file (null: none, or not an answer).
    std::vector<const std::string*> hd;
    // Every (arena, HD file) the index pairs, in row order, for the HD window's
    // per-arena lists (buildHdWindow3ds). `row` names the arena.
    struct HdArenaRow {
        uint32_t row;
        const std::string* path;
    };
    std::vector<HdArenaRow> hdArenaRows;
    int32_t answers = 0;
    int32_t hdAnswers = 0;

    // The row answering (palette, lower-case name); -1 when none does.
    int32_t find(const void* palette, const char* lowerName) const;
    // The arena name of an index row.
    const char* arenaOf(uint32_t row) const;
    // `root` + "/" + the row's rel + ".t3x": the orig file's path.
    void origPath(const char* root, uint32_t row, std::string* out) const;
    void clear();
};

// Where the level's rows came from.
enum class BynameSource { None, Binary, Text };

struct BynameLoadReport {
    BynameSource source = BynameSource::None;
    // Why byname.bin was not used ("" when it was).
    std::string refused;
    // Bytes read from the file the rows came from, and the reads it took.
    uint32_t bytesRead = 0;
    int32_t reads = 0;
    // (palette, name) keys two rows name different files for (a corridor and
    // its arena): the first few, in words, and how many.
    int32_t collisions = 0;
    std::vector<std::string> collisionLines;
};

// `root`'s byname.bin (or, refused, byname.txt) for `level`, resolved against
// the loaded level and joined with `hdByKey` (swapped into `out`). `manifest`
// is the orig manifest as loaded; `origDims` its source sizes by rel (the text
// fallback's join). False when no row answers anything.
bool loadOrigByName(const char* root, int32_t level, LevelData& levelData,
                    const OrigManifestStamp& manifest,
                    std::map<std::string, std::string>* hdByKey,
                    const std::map<std::string, uint32_t>& origDims, OrigByName* out,
                    BynameLoadReport* report);

} // namespace mdk
