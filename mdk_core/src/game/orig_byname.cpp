// ============================================================================
// orig_byname.cpp -- see orig_byname.h.
// ============================================================================

#include "game/orig_byname.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "formats/arena.h"
#include "game/level_data.h"

namespace mdk {

using formats::BynameIndex;

namespace {

// A collision past this many is counted, not described.
const size_t kCollisionLines = 4;

// The size of an open file, left at its start (fseek/ftell: a RomFS file and a
// host file alike); -1 when it cannot be measured.
long fileSize(FILE* file) {
    if (fseek(file, 0, SEEK_END) != 0) {
        return -1;
    }
    const long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        return -1;
    }
    return size;
}

// THE TABLE (AGENTS.md P5): the header, then the level's block -- two reads,
// each checked whole. "" on success, else why the table is not used.
// byname.txt is checked by its SIZE: hashing it would mean reading it, the cost
// the table removes. stage_romfs.sh makes the table from the staged text every
// build, so a same-size edit can only come from a hand-edited RomFS.
std::string readTable(const std::string& path, int32_t level, const OrigManifestStamp& manifest,
                      const std::string& textPath, BynameIndex* index, bool* levelHasRows,
                      BynameLoadReport* report) {
    *levelHasRows = false;
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        return "no byname.bin";
    }
    const long size = fileSize(file);
    if (size < 0 || size > 0x7FFFFFFFL) {
        fclose(file);
        return "byname.bin: size unreadable";
    }
    uint8_t header[BynameIndex::kHeaderBytes];
    const size_t got = fread(header, 1, sizeof(header), file);
    report->reads++;
    report->bytesRead += (uint32_t)got;
    BynameIndex::Header parsed;
    const BynameIndex::Status status =
        BynameIndex::parseHeader(header, got, (uint32_t)size, &parsed);
    if (status != BynameIndex::Status::Ok) {
        fclose(file);
        return std::string("byname.bin header: ") + BynameIndex::statusName(status);
    }
    // THE STAMP: built from exactly this manifest and a byname.txt of this size.
    if (!manifest.valid || parsed.stamp.manifestBytes != manifest.bytes ||
        parsed.stamp.manifestHash != manifest.hash) {
        fclose(file);
        char why[160];
        snprintf(why, sizeof(why),
                 "byname.bin stamp: built from a %u-byte manifest %016llx, the pack's is %u "
                 "bytes %016llx",
                 (unsigned)parsed.stamp.manifestBytes,
                 (unsigned long long)parsed.stamp.manifestHash, (unsigned)manifest.bytes,
                 (unsigned long long)manifest.hash);
        return why;
    }
    struct stat text;
    if (stat(textPath.c_str(), &text) != 0 || (uint64_t)text.st_size != parsed.stamp.bynameBytes) {
        fclose(file);
        char why[128];
        snprintf(why, sizeof(why), "byname.bin stamp: built from a %u-byte byname.txt, the pack's "
                                   "is %ld bytes",
                 (unsigned)parsed.stamp.bynameBytes,
                 stat(textPath.c_str(), &text) == 0 ? (long)text.st_size : -1L);
        return why;
    }
    if (level < 0 || level >= (int32_t)BynameIndex::kLevelSlots ||
        parsed.slots[level].bytes == 0) {
        fclose(file);
        return "";   // the table holds no rows for this level, as the text would not
    }
    const BynameIndex::Slot& slot = parsed.slots[level];
    std::vector<uint8_t> block(slot.bytes);
    const bool read = fseek(file, (long)slot.offset, SEEK_SET) == 0 &&
                      fread(block.data(), 1, block.size(), file) == block.size();
    report->reads++;
    fclose(file);
    if (!read) {
        return "byname.bin: short read of the level's block";
    }
    report->bytesRead += slot.bytes;
    const BynameIndex::Status adopted = index->adopt(&block, true, slot.hash);
    if (adopted != BynameIndex::Status::Ok) {
        return std::string("byname.bin level block: ") + BynameIndex::statusName(adopted);
    }
    *levelHasRows = true;
    return "";
}

// THE FALLBACK: byname.txt read whole (one read, checked) and cut to the level
// by the same rules the table's writer uses.
bool readText(const std::string& path, int32_t level,
              const std::map<std::string, uint32_t>& origDims, BynameIndex* index,
              BynameLoadReport* report) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        return false;
    }
    const long size = fileSize(file);
    if (size < 0 || size > (long)(64u << 20)) {
        fclose(file);
        return false;
    }
    std::vector<uint8_t> text((size_t)size);
    const size_t got = size > 0 ? fread(text.data(), 1, text.size(), file) : 0;
    report->reads++;
    fclose(file);
    report->bytesRead += (uint32_t)got;
    if (got != text.size()) {
        return false;
    }
    std::vector<uint8_t> block;
    if (!BynameIndex::buildBlock(level, text.data(), text.size(), origDims, &block)) {
        return false;
    }
    return index->adopt(&block, false, 0) == BynameIndex::Status::Ok;
}

// The row's HD file: its content key in the HD pack, unless it has none or is a
// strip over 1024 (orig-pack pages are animated, and an animated texture never
// draws an HD copy -- pairing one would pin the pack's squeezed EXPLODE).
const std::string* hdOf(const BynameIndex::Row& row,
                        const std::map<std::string, std::string>& hdByKey, std::string* probe) {
    if (!row.key || (row.hasDims() && (row.dims & 0xFFFFu) > 1024u)) {
        return nullptr;
    }
    probe->assign(row.key);
    const std::map<std::string, std::string>::const_iterator hd = hdByKey.find(*probe);
    return hd == hdByKey.end() ? nullptr : &hd->second;
}

// THE RESOLVE: the text index's map, row by row in file order, without the
// map. For one (palette, name) key a row replaces the answer so far, except a
// row whose arena borrows its palette (a corridor) and names ANOTHER file than
// the answer's: the arena with colours of its own owns the name, and the
// corridor keeps drawing the owner's file -- in its HD window too. The rows of
// one key are contiguous in the sorted list (same name), in file order.
void resolve(LevelData& levelData, const char* root, OrigByName* out, BynameLoadReport* report) {
    const BynameIndex& index = out->index;
    const uint32_t rows = index.rowCount();
    const uint32_t arenas = index.arenaCount();
    std::vector<uint8_t> owner(arenas, 0);
    out->arenaPalettes.assign(arenas, nullptr);
    for (uint32_t a = 0; a < arenas; a++) {
        const formats::Arena* arena = levelData.arenaNamed(index.arenaName(a));
        if (arena) {
            out->arenaPalettes[a] = &levelData.paletteOf(arena);
            owner[a] = arena->paletteRgbBytes() != 0 ? 1 : 0;
        }
    }
    std::vector<const std::string*> rowHd(rows, nullptr);
    std::string probe;
    for (uint32_t r = 0; r < rows; r++) {
        const BynameIndex::Row row = index.row(r);
        if (out->arenaPalettes[row.arena]) {
            rowHd[r] = hdOf(row, out->hdByKey, &probe);
        }
    }
    out->answer.assign(rows, 0);
    out->hd.assign(rows, nullptr);
    // pushFrom[r]: the row whose HD file row r's arena takes into its window.
    std::vector<int32_t> pushFrom(rows, -1);
    std::vector<uint8_t> done(rows, 0);
    uint32_t groupEnd = 0;
    for (uint32_t i = 0; i < rows; i = groupEnd) {
        const char* name = index.row(index.sortedRow(i)).name;
        groupEnd = i + 1;
        while (groupEnd < rows && strcmp(index.row(index.sortedRow(groupEnd)).name, name) == 0) {
            groupEnd++;
        }
        for (uint32_t k = i; k < groupEnd; k++) {
            const uint32_t first = index.sortedRow(k);
            const void* palette = out->arenaPalettes[index.row(first).arena];
            if (done[first] || !palette) {
                continue;
            }
            int32_t current = -1;
            for (uint32_t j = k; j < groupEnd; j++) {
                const uint32_t r = index.sortedRow(j);
                const BynameIndex::Row row = index.row(r);
                if (done[r] || out->arenaPalettes[row.arena] != palette) {
                    continue;
                }
                done[r] = 1;
                if (current >= 0 && strcmp(index.row((uint32_t)current).rel, row.rel) != 0) {
                    const bool ownerRow = owner[row.arena] != 0;
                    report->collisions++;
                    if (report->collisionLines.size() < kCollisionLines) {
                        const BynameIndex::Row kept = ownerRow ? row : index.row((uint32_t)current);
                        const BynameIndex::Row dropped =
                            ownerRow ? index.row((uint32_t)current) : row;
                        report->collisionLines.push_back(
                            std::string("arena=") + index.arenaName(row.arena) + " name=" +
                            row.name + " keeping=" + root + "/" + kept.rel + ".t3x ignoring=" +
                            root + "/" + dropped.rel + ".t3x");
                    }
                    if (!ownerRow) {
                        if (rowHd[(uint32_t)current]) {
                            pushFrom[r] = current;
                        }
                        continue;
                    }
                }
                current = (int32_t)r;
                if (rowHd[r]) {
                    pushFrom[r] = (int32_t)r;
                }
            }
            out->answer[(uint32_t)current] = 1;
            out->hd[(uint32_t)current] = rowHd[(uint32_t)current];
            out->answers++;
            if (rowHd[(uint32_t)current]) {
                out->hdAnswers++;
            }
        }
    }
    for (uint32_t r = 0; r < rows; r++) {
        if (pushFrom[r] >= 0) {
            OrigByName::HdArenaRow pair;
            pair.row = r;
            pair.path = rowHd[(uint32_t)pushFrom[r]];
            out->hdArenaRows.push_back(pair);
        }
    }
}

} // namespace

int32_t OrigByName::find(const void* palette, const char* lowerName) const {
    uint32_t first = 0;
    uint32_t last = 0;
    index.equalRange(lowerName, &first, &last);
    for (uint32_t i = first; i < last; i++) {
        const uint32_t r = index.sortedRow(i);
        if (answer[r] && arenaPalettes[index.row(r).arena] == palette) {
            return (int32_t)r;
        }
    }
    return -1;
}

const char* OrigByName::arenaOf(uint32_t row) const {
    return index.arenaName(index.row(row).arena);
}

void OrigByName::origPath(const char* root, uint32_t row, std::string* out) const {
    out->assign(root);
    out->append("/");
    out->append(index.row(row).rel);
    out->append(".t3x");
}

void OrigByName::clear() {
    index.clear();
    std::map<std::string, std::string>().swap(hdByKey);
    std::vector<const void*>().swap(arenaPalettes);
    std::vector<uint8_t>().swap(answer);
    std::vector<const std::string*>().swap(hd);
    std::vector<HdArenaRow>().swap(hdArenaRows);
    answers = 0;
    hdAnswers = 0;
}

bool loadOrigByName(const char* root, int32_t level, LevelData& levelData,
                    const OrigManifestStamp& manifest,
                    std::map<std::string, std::string>* hdByKey,
                    const std::map<std::string, uint32_t>& origDims, OrigByName* out,
                    BynameLoadReport* report) {
    out->clear();
    *report = BynameLoadReport();
    const std::string textPath = std::string(root) + "/byname.txt";
    bool levelHasRows = false;
    report->refused = readTable(std::string(root) + "/byname.bin", level, manifest, textPath,
                                &out->index, &levelHasRows, report);
    if (report->refused.empty()) {
        report->source = BynameSource::Binary;
        if (!levelHasRows) {
            return false;
        }
    } else {
        out->index.clear();
        report->bytesRead = 0;
        report->reads = 0;
        if (!readText(textPath, level, origDims, &out->index, report)) {
            out->index.clear();
            return false;
        }
        report->source = BynameSource::Text;
    }
    out->hdByKey.swap(*hdByKey);
    resolve(levelData, root, out, report);
    return out->answers > 0;
}

} // namespace mdk
