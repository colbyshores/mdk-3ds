#pragma once

// ============================================================================
// byname_index.h -- the orig pack's name index as a binary table
// (romfs:/orig/byname.bin), written at BUILD time.
//
// WHY. The orig pack's name index (game/orig_byname.h) used to be built at
// every level load from romfs:/orig/byname.txt: 415 KB of text, every level's
// 8164 rows read line by line and split into strings to keep one level's
// ~1300. Measured on the Old 3DS: 2.53-2.64 s of the main core on every level
// load (docs/WORK_LEDGER.md 2026-11-27, items 12 and 13). The table is the same
// rows, cut by level and joined with the manifest's source sizes on the host,
// so a level load is two reads (the header, then the level's block) and the
// block IS the index: no line is parsed and no row allocates.
//
// WRITTEN BY mdk_3ds/tools/byname_index.py, which stage_romfs.sh runs on the
// staged byname.txt + manifest.txt. The writer and buildBlock below are the
// format's two halves -- the same rows, the same order, the same bytes -- and
// tests/formats/byname_index_tests.cpp checks them against each other and
// against the text index the game built before the table existed.
//
// THE STAMP. The header records the size and FNV-1a 64 hash of the two inputs
// (manifest.txt, byname.txt). The game reads the manifest whole anyway and
// hashes it; byname.txt is checked by its size (reading it to hash it is the
// cost the table removes). A stamp that does not match, a file that does not
// parse or a block whose hash is wrong is NEVER used: the game builds the index
// from byname.txt, as it did before (game/orig_byname.cpp).
//
// THE FILE, little-endian throughout:
//
//   header (kHeaderBytes = 320)
//     0   char[8] magic "MDKBYNM\0"
//     8   u32     version (kVersion)
//     12  u32     level slots (kLevelSlots)
//     16  u32     file bytes: the whole file (a truncation is caught against
//                 the file's real size)
//     20  u32     manifest.txt bytes
//     24  u64     manifest.txt FNV-1a 64
//     32  u32     byname.txt bytes
//     36  u32     0
//     40  u64     byname.txt FNV-1a 64
//     48  u32[4]  0
//     64  slot[kLevelSlots], one per level number 0..15, 16 bytes each:
//           u32 block offset (from the start of the file, a multiple of 4)
//           u32 block bytes (0: the level has no rows)
//           u64 block FNV-1a 64
//   then each level's block:
//     0   u32 row count
//     4   u32 arena count
//     8   u32 string bytes
//     12  u32 0
//     16  arena table: arena count x u32, the arena's name (string offset),
//         in the order the rows first name them
//     ..  rows: row count x kRowBytes, in byname.txt's order (the order the
//         text index applied them in, which decides a collision):
//           u16 arena (index into the arena table)
//           u16 flags (kFlagDims: `dims` is the manifest's source size)
//           u32 name: the texture name, lower-cased (string offset)
//           u32 rel: the orig file, "textures/name@key" (string offset)
//           u32 key: the content key after rel's first '@', lower-cased --
//                the HD pack's key (string offset; kNoKey when rel has no '@')
//           u32 dims: (source width << 16) | (source stacked height & 0xFFFF),
//                the manifest's columns 3 and 4 for rel
//     ..  sorted: row count x u32 row numbers, by (name bytes, row number) --
//         the lookup's binary search
//     ..  strings: NUL-terminated, each once, in the order first used (arena
//         when the arena table first takes it, then name, rel, key per row);
//         the last byte is a NUL
//     ..  zero padding to a multiple of 4
// ============================================================================

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <string>
#include <vector>

namespace mdk {
namespace formats {

class BynameIndex {
public:
    static constexpr uint32_t kVersion = 1;
    static constexpr uint32_t kLevelSlots = 16;
    static constexpr uint32_t kHeaderBytes = 64 + kLevelSlots * 16;
    static constexpr uint32_t kBlockHeaderBytes = 16;
    static constexpr uint32_t kRowBytes = 20;
    static constexpr uint32_t kNoKey = 0xFFFFFFFFu;
    static constexpr uint16_t kFlagDims = 1;
    // Bounds no honest table comes near (the largest level has 1662 rows, its
    // block ~70 KB): what they refuse is a corrupt size before it sizes an
    // allocation.
    static constexpr uint32_t kMaxBlockBytes = 4u << 20;
    static constexpr uint32_t kMaxRows = 0xFFFFu;
    // The text index's line buffer (game/orig_byname.cpp, the manifest's): a
    // line of more characters than this, its newline not counted, is skipped.
    static constexpr size_t kMaxLineChars = 510;

    enum class Status {
        Ok,
        TooSmall,     // fewer bytes than the header or block header says
        BadMagic,
        BadVersion,
        BadLayout,    // sizes, offsets, a string, an arena or the sort order
        Truncated,    // the file is shorter than the header says
        BadHash,      // a block whose bytes are not the ones the header hashed
    };

    // What the table was built from (the header's 20..47).
    struct Stamp {
        uint32_t manifestBytes = 0;
        uint64_t manifestHash = 0;
        uint32_t bynameBytes = 0;
        uint64_t bynameHash = 0;
    };

    struct Slot {
        uint32_t offset = 0;
        uint32_t bytes = 0;
        uint64_t hash = 0;
    };

    struct Header {
        uint32_t version = 0;
        uint32_t fileBytes = 0;
        Stamp stamp;
        Slot slots[kLevelSlots];
    };

    struct Row {
        uint16_t arena = 0;
        uint16_t flags = 0;
        const char* name = "";
        const char* rel = "";
        const char* key = nullptr;   // null: rel has no '@'
        uint32_t dims = 0;
        bool hasDims() const { return (flags & kFlagDims) != 0; }
    };

    // THE LINES OF A TEXT FILE as the game's fgets(512) read them, from `*pos`:
    // the next line's start, its length without the newline, and whether it had
    // one; a line of more than kMaxLineChars is passed over whole. False at the
    // end. The text index (buildBlock) and the pack manifests
    // (packs_3ds.cpp loadCompressedManifest) both read their files with it.
    static bool nextLine(const char* text, size_t size, size_t* pos, const char** line,
                         size_t* length, bool* newline);

    // FNV-1a 64 of `size` bytes (the stamp's and the slots' hash).
    static uint64_t hash(const uint8_t* bytes, size_t size);

    // The header's kHeaderBytes, checked against the file's real size: every
    // slot lies inside the file, after the header, aligned.
    static Status parseHeader(const uint8_t* bytes, size_t size, uint32_t actualFileBytes,
                              Header* out);

    // Takes a level block (its bytes are swapped out of `bytes`) and checks all
    // of it: sizes, every string offset, every arena index, the sorted list a
    // permutation in order. With `checkHash` the bytes must hash to `hash` (a
    // block read from the file; a block buildBlock just made is not hashed). On
    // anything but Ok the index is left empty.
    Status adopt(std::vector<uint8_t>* bytes, bool checkHash, uint64_t hash);

    // THE TEXT INDEX AS A BLOCK: `level`'s rows of byname.txt (`text`, the whole
    // file) joined with the manifest's source sizes (`dims`, keyed by rel) --
    // exactly the bytes byname_index.py writes for that level. Rows the text
    // index skipped are skipped here by the same rules: comment and empty lines,
    // lines over kMaxLineChars, lines without exactly four fields, another
    // level's rows (the level is the first field's atoi). One rule is new: a line
    // holding a NUL byte is skipped whole (the text index's fgets/strlen read
    // such a line short and could take the next line with it; no pack has one).
    // False when the level has more rows or arenas than a block can hold.
    static bool buildBlock(int32_t level, const uint8_t* text, size_t size,
                           const std::map<std::string, uint32_t>& dims,
                           std::vector<uint8_t>* out);

    bool empty() const { return rows_ == 0; }
    uint32_t rowCount() const { return rows_; }
    uint32_t arenaCount() const { return arenas_; }
    const char* arenaName(uint32_t arena) const;
    Row row(uint32_t index) const;
    // The row at position `i` of the sorted list.
    uint32_t sortedRow(uint32_t i) const;
    // The sorted list's positions [*first, *last) whose name is `name`.
    void equalRange(const char* name, uint32_t* first, uint32_t* last) const;
    size_t bytes() const { return block_.size(); }
    void clear();

    static const char* statusName(Status status);

private:
    const char* string(uint32_t offset) const;

    std::vector<uint8_t> block_;
    uint32_t rows_ = 0;
    uint32_t arenas_ = 0;
    uint32_t arenaTable_ = 0;
    uint32_t rowTable_ = 0;
    uint32_t sortedTable_ = 0;
    uint32_t strings_ = 0;
    uint32_t stringBytes_ = 0;
};

} // namespace formats
} // namespace mdk
