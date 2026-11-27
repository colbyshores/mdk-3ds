// ============================================================================
// byname_index_tests.cpp -- the orig pack's name index as a build-time table
// (romfs:/orig/byname.bin): the writer (mdk_3ds/tools/byname_index.py), the
// reader and its text fallback (formats/byname_index.h), and the resolve
// (game/orig_byname.h), against the text index the game built at every level
// load before the table existed.
//
// WHAT IS PINNED HERE.
//   1. THE SAME ANSWERS. The text index's code, kept below VERBATIM as the
//      oracle (it was mdk_3ds/src/packs_3ds.cpp loadOrigByName up to the table),
//      and the table give the same answer for every (palette, name) key of the
//      level: the same orig file, the same source size, the same HD file -- no
//      key more, no key less -- and the same (arena, HD file) list for the HD
//      window, in the same order. On real levels 3-8 (the game data, and the
//      built orig pack when there is one) and on a set built to hit every rule:
//      a corridor that names another file than its arena (both ways round), a
//      duplicate row, an unknown arena, another level's row, a paged strip, a
//      row with no size, a file with no key, mixed case, an over-long line, a
//      five-field line, a last line with no newline.
//   2. THE TWO WRITERS AGREE. byname_index.py and BynameIndex::buildBlock (the
//      game's fallback) make the same bytes for every level.
//   3. THE STAMP. A table built from another manifest, next to another
//      byname.txt, truncated, corrupted, of another format, or absent is NOT
//      used: the index is built from byname.txt, and its answers are still the
//      oracle's. With neither file there is no index.
// ============================================================================

#include "test_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "formats/arena.h"
#include "formats/byname_index.h"
#include "formats/case_insensitive_path.h"
#include "formats/mdk_data.h"
#include "game/level_data.h"
#include "game/orig_byname.h"

using mdk::formats::BynameIndex;

namespace {

const char kWriter[] = "../../mdk_3ds/tools/byname_index.py";
const char kRoot[] = "romfs:/orig";   // the oracle's path prefix, the table's too

std::string makeTempDir() {
    const char* base = getenv("TMPDIR");
    std::string pattern = std::string(base && *base ? base : "/tmp") + "/mdk-byname-XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const char* made = mkdtemp(buffer.data());
    return made ? std::string(made) : std::string();
}

void removeTree(const std::string& path) {
    if (path.empty()) {
        return;
    }
    const std::string command = "rm -rf '" + path + "'";
    if (system(command.c_str()) != 0) {
        // Nothing to do: the directory is a scratch one.
    }
}

bool writeFile(const std::string& path, const std::string& bytes) {
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) {
        return false;
    }
    const bool ok = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return fclose(file) == 0 && ok;
}

bool readFile(const std::string& path, std::string* out) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        return false;
    }
    out->clear();
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        out->append(chunk, got);
    }
    fclose(file);
    return true;
}

bool copyFile(const std::string& from, const std::string& to) {
    std::string bytes;
    return readFile(from, &bytes) && writeFile(to, bytes);
}

int run(const std::string& command, std::string* output) {
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
    if (!pipe) {
        return -1;
    }
    char line[512];
    while (fgets(line, sizeof(line), pipe)) {
        if (output) {
            *output += line;
        }
    }
    return pclose(pipe);
}

bool writeTable(const std::string& dir) {
    std::string output;
    const int status = run("python3 " + std::string(kWriter) + " '" + dir + "'", &output);
    if (status != 0) {
        std::printf("    byname_index.py failed (%d): %s", status, output.c_str());
    }
    return status == 0;
}

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

// ---------------------------------------------------------------------------
// THE ORACLE: the text index as the game built it before the table
// (mdk_3ds/src/packs_3ds.cpp at 71962017), verbatim but for the file's path and
// the log line. Do not "fix" it: it is the behaviour the table must keep.
// ---------------------------------------------------------------------------

struct OldByName {
    std::map<std::pair<const void*, std::string>, std::string> paths;
    std::map<std::pair<const void*, std::string>, std::string> hdPaths;
    std::map<std::pair<const void*, std::string>, uint32_t> dims;
    std::vector<std::pair<std::string, std::string>> hdArenaRows;
};

std::string lowerName3ds(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i] >= 'A' && out[i] <= 'Z') {
            out[i] = (char)(out[i] - 'A' + 'a');
        }
    }
    return out;
}

bool oldLoadOrigByName(const std::string& textPath, int32_t level, mdk::LevelData& levelData,
                       const std::map<std::string, std::string>& hdByKey,
                       const std::map<std::string, uint32_t>& origDims, OldByName* out) {
    FILE* file = fopen(textPath.c_str(), "r");
    if (!file) {
        return false;
    }
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        const size_t length = strlen(line);
        if (length > 0 && line[length - 1] != '\n' && !feof(file)) {
            int c;
            while ((c = fgetc(file)) != EOF && c != '\n') {
            }
            continue;
        }
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        // level \t arena \t name \t rel   (arena_etc1_convert.py)
        std::string fields[4];
        size_t field = 0;
        for (const char* p = line; *p && *p != '\n' && field < 4; p++) {
            if (*p == '\t') {
                field++;
            } else {
                fields[field] += *p;
            }
        }
        if (field != 3 || atoi(fields[0].c_str()) != (int)level) {
            continue;
        }
        const mdk::formats::Arena* arena = levelData.arenaNamed(fields[1]);
        if (arena == nullptr) {
            continue;
        }
        const mdk::formats::Palette& palette = levelData.paletteOf(arena);
        const std::pair<const void*, std::string> key(
            (const void*)&palette, lowerName3ds(fields[2]));
        const std::string path = std::string(kRoot) + "/" + fields[3] + ".t3x";
        const std::map<std::pair<const void*, std::string>, std::string>::iterator known =
            out->paths.find(key);
        if (known != out->paths.end() && known->second != path) {
            const bool ownerRow = arena->paletteRgbBytes() != 0;
            if (!ownerRow) {
                const std::map<std::pair<const void*, std::string>, std::string>::const_iterator
                    ownerHd = out->hdPaths.find(key);
                if (ownerHd != out->hdPaths.end()) {
                    out->hdArenaRows.push_back(std::make_pair(fields[1], ownerHd->second));
                }
                continue;
            }
        }
        out->paths[key] = path;
        const std::map<std::string, uint32_t>::const_iterator dims = origDims.find(fields[3]);
        if (dims != origDims.end()) {
            out->dims[key] = dims->second;
        } else {
            out->dims.erase(key);
        }
        const bool paged = dims != origDims.end() && (dims->second & 0xFFFFu) > 1024u;
        const size_t mark = fields[3].find('@');
        const std::map<std::string, std::string>::const_iterator hd =
            mark == std::string::npos || paged
                ? hdByKey.end()
                : hdByKey.find(lowerName3ds(fields[3].substr(mark + 1)));
        if (hd != hdByKey.end()) {
            out->hdPaths[key] = hd->second;
            out->hdArenaRows.push_back(std::make_pair(fields[1], hd->second));
        } else {
            out->hdPaths.erase(key);
        }
    }
    fclose(file);
    return !out->paths.empty();
}

// The old manifest loader's source sizes (loadCompressedManifest's `dims`,
// fgets line by line), verbatim.
void oldManifestDims(const std::string& path, std::map<std::string, uint32_t>* dims) {
    FILE* file = fopen(path.c_str(), "r");
    if (!file) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        const size_t length = strlen(line);
        if (length > 0 && line[length - 1] != '\n' && !feof(file)) {
            int c;
            while ((c = fgetc(file)) != EOF && c != '\n') {
            }
            continue;
        }
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        char* tab = strchr(line, '\t');
        if (!tab) {
            continue;
        }
        *tab = '\0';
        const char* format = tab + 1;
        const char* srcW = strchr(format, '\t');
        const char* srcH = srcW ? strchr(srcW + 1, '\t') : nullptr;
        if (srcW && srcH) {
            const uint32_t w = (uint32_t)atoi(srcW + 1);
            const uint32_t h = (uint32_t)atoi(srcH + 1);
            (*dims)[std::string(line)] = (w << 16) | (h & 0xFFFFu);
        }
    }
    fclose(file);
}

mdk::OrigManifestStamp stampOf(const std::string& manifestPath) {
    mdk::OrigManifestStamp stamp;
    std::string bytes;
    if (readFile(manifestPath, &bytes)) {
        stamp.valid = true;
        stamp.bytes = (uint32_t)bytes.size();
        stamp.hash = BynameIndex::hash((const uint8_t*)bytes.data(), bytes.size());
    }
    return stamp;
}

// ---------------------------------------------------------------------------
// The comparison: every key, both ways, and the window's list.
// ---------------------------------------------------------------------------

int sameAnswers(const OldByName& old, const mdk::OrigByName& now) {
    int differences = 0;
    if ((size_t)now.answers != old.paths.size() || (size_t)now.hdAnswers != old.hdPaths.size()) {
        std::printf("    counts: table %d/%d, text %zu/%zu\n", (int)now.answers,
                    (int)now.hdAnswers, old.paths.size(), old.hdPaths.size());
        differences++;
    }
    std::string path;
    for (std::map<std::pair<const void*, std::string>, std::string>::const_iterator it =
             old.paths.begin();
         it != old.paths.end(); ++it) {
        const int32_t row = now.find(it->first.first, it->first.second.c_str());
        if (row < 0) {
            if (differences++ < 5) {
                std::printf("    key %s: no table answer\n", it->first.second.c_str());
            }
            continue;
        }
        now.origPath(kRoot, (uint32_t)row, &path);
        const BynameIndex::Row r = now.index.row((uint32_t)row);
        const std::map<std::pair<const void*, std::string>, uint32_t>::const_iterator dims =
            old.dims.find(it->first);
        const std::map<std::pair<const void*, std::string>, std::string>::const_iterator hd =
            old.hdPaths.find(it->first);
        const std::string* nowHd = now.hd[(uint32_t)row];
        const bool same = path == it->second && (dims != old.dims.end()) == r.hasDims() &&
                          (dims == old.dims.end() || dims->second == r.dims) &&
                          (hd != old.hdPaths.end()) == (nowHd != nullptr) &&
                          (hd == old.hdPaths.end() || hd->second == *nowHd);
        if (!same && differences++ < 5) {
            std::printf("    key %s: table %s, text %s\n", it->first.second.c_str(), path.c_str(),
                        it->second.c_str());
        }
    }
    // No key more: every table answer is a text key.
    for (uint32_t row = 0; row < now.index.rowCount(); row++) {
        if (!now.answer[row]) {
            continue;
        }
        const BynameIndex::Row r = now.index.row(row);
        const std::pair<const void*, std::string> key(now.arenaPalettes[r.arena], r.name);
        if (old.paths.find(key) == old.paths.end() && differences++ < 5) {
            std::printf("    table answers %s, the text does not\n", r.name);
        }
    }
    if (now.hdArenaRows.size() != old.hdArenaRows.size()) {
        std::printf("    window rows: table %zu, text %zu\n", now.hdArenaRows.size(),
                    old.hdArenaRows.size());
        differences++;
    } else {
        for (size_t i = 0; i < old.hdArenaRows.size(); i++) {
            if (old.hdArenaRows[i].first != now.arenaOf(now.hdArenaRows[i].row) ||
                old.hdArenaRows[i].second != *now.hdArenaRows[i].path) {
                if (differences++ < 5) {
                    std::printf("    window row %zu: table %s %s, text %s %s\n", i,
                                now.arenaOf(now.hdArenaRows[i].row),
                                now.hdArenaRows[i].path->c_str(),
                                old.hdArenaRows[i].first.c_str(),
                                old.hdArenaRows[i].second.c_str());
                }
            }
        }
    }
    return differences;
}

// The HD pack by content key: the keys `manifest` names, two in three of them
// (in key order), as loadHdPack's byKey would hold them.
std::map<std::string, std::string> hdFor(const std::map<std::string, uint32_t>& dims,
                                         int32_t level) {
    std::map<std::string, std::string> hd;
    int32_t n = 0;
    for (std::map<std::string, uint32_t>::const_iterator it = dims.begin(); it != dims.end();
         ++it) {
        const size_t mark = it->first.find('@');
        if (mark == std::string::npos || n++ % 3 == 0) {
            continue;
        }
        hd[lowerName3ds(it->first.substr(mark + 1))] =
            "sdmc:/3ds/mdk/hd/textures/LEVEL" + std::to_string(level) + "/" +
            it->first.substr(9) + ".t3x";
    }
    return hd;
}

// One level, through the oracle and through loadOrigByName on `dir` (which
// holds manifest.txt, byname.txt and maybe byname.bin). The source the load
// took goes to `source`; the number of differences is returned.
int compareLevel(const std::string& dir, int32_t level, mdk::LevelData& levelData,
                 const mdk::OrigManifestStamp& stamp, mdk::BynameSource* source,
                 std::string* refused, mdk::OrigByName* now) {
    std::map<std::string, uint32_t> dims;
    oldManifestDims(dir + "/manifest.txt", &dims);
    const std::map<std::string, std::string> hd = hdFor(dims, level);
    OldByName old;
    oldLoadOrigByName(dir + "/byname.txt", level, levelData, hd, dims, &old);
    std::map<std::string, std::string> hdByKey = hd;
    mdk::BynameLoadReport report;
    const bool have =
        mdk::loadOrigByName(dir.c_str(), level, levelData, stamp, &hdByKey, dims, now, &report);
    *source = report.source;
    *refused = report.refused;
    int differences = sameAnswers(old, *now);
    if (have != !old.paths.empty()) {
        std::printf("    level %d: table says %d, text %d\n", (int)level, (int)have,
                    (int)!old.paths.empty());
        differences++;
    }
    return differences;
}

// The table's level block as written, for the byte comparison.
bool tableBlock(const std::string& path, int32_t level, std::string* block) {
    std::string bytes;
    if (!readFile(path, &bytes) || bytes.size() < BynameIndex::kHeaderBytes) {
        return false;
    }
    BynameIndex::Header header;
    if (BynameIndex::parseHeader((const uint8_t*)bytes.data(), bytes.size(),
                                 (uint32_t)bytes.size(), &header) != BynameIndex::Status::Ok) {
        return false;
    }
    const BynameIndex::Slot& slot = header.slots[level];
    *block = bytes.substr(slot.offset, slot.bytes);
    return true;
}

bool sameBlockBytes(const std::string& dir, int32_t level) {
    std::string text;
    std::map<std::string, uint32_t> dims;
    oldManifestDims(dir + "/manifest.txt", &dims);
    if (!readFile(dir + "/byname.txt", &text)) {
        return false;
    }
    std::vector<uint8_t> built;
    if (!BynameIndex::buildBlock(level, (const uint8_t*)text.data(), text.size(), dims, &built)) {
        return false;
    }
    std::string written;
    if (!tableBlock(dir + "/byname.bin", level, &written)) {
        return false;
    }
    // A level with no rows has no block in the table; buildBlock's is the empty one.
    if (written.empty()) {
        return built.size() == BynameIndex::kBlockHeaderBytes && built[0] == 0;
    }
    return written == std::string(built.begin(), built.end());
}

// ---------------------------------------------------------------------------
// The rule-hitting set, on level 3's real arenas.
// ---------------------------------------------------------------------------

struct Synthetic {
    std::string owner;      // an arena with its own colours that has a corridor
    std::string corridor;   // "C" + owner: borrows the owner's palette object
    std::string other;      // another arena with colours of its own
};

bool pickArenas(mdk::LevelData& level, Synthetic* out) {
    for (size_t i = 0; i < level.arenas().size(); i++) {
        const mdk::formats::Arena* arena = level.arenas()[i];
        if (arena->paletteRgbBytes() == 0) {
            continue;
        }
        const mdk::formats::Arena* corridor = level.arenaNamed("C" + arena->name());
        if (!corridor || corridor->paletteRgbBytes() != 0 ||
            &level.paletteOf(corridor) != &level.paletteOf(arena)) {
            continue;
        }
        if (out->owner.empty()) {
            out->owner = arena->name();
            out->corridor = corridor->name();
        } else if (out->other.empty()) {
            out->other = arena->name();
            return true;
        }
    }
    return false;
}

std::string row(int level, const std::string& arena, const std::string& name,
                const std::string& rel) {
    return std::to_string(level) + "\t" + arena + "\t" + name + "\t" + rel + "\n";
}

std::string manifestLine(const std::string& rel, int w, int h) {
    return rel + "\tetc1\t" + std::to_string(w) + "\t" + std::to_string(h) +
           "\t64\t64\t100\t0123456789abcdef\t1024\tv1\n";
}

bool writeSynthetic(const std::string& dir, const Synthetic& s) {
    const std::string& O = s.owner;
    const std::string& C = s.corridor;
    const std::string& P = s.other;
    std::string byname = "# level\tarena\tname\trel\n";
    byname += row(3, O, "X", "textures/x@k1");
    byname += row(3, C, "X", "textures/x@k2");             // a corridor's other file: ignored
    byname += row(3, C, "Y", "textures/y@k3");
    byname += row(3, O, "Y", "textures/y@k4");             // the owner's other file: it wins
    byname += row(3, O, "Z", "textures/z@k5");
    byname += row(3, O, "Z", "textures/z@k5");             // a duplicate
    byname += row(3, P, "X", "textures/x@k6");             // another palette, the same name
    byname += row(3, "NOPE_1", "X", "textures/x@k7");      // no such arena
    byname += row(4, O, "W", "textures/w@k8");             // another level
    byname += row(3, O, "PAGED", "textures/paged@k9");     // a strip over 1024: no HD
    byname += row(3, O, "NODIMS", "textures/nodims@ka");   // not in the manifest
    byname += row(3, O, "NOKEY", "textures/nokey");        // no '@': no key
    byname += row(3, O, "MiXeD", "textures/mixed@KB");     // the key's case
    byname += row(3, C, "V", "textures/v@kd");             // the corridor alone
    byname += row(3, C, "X", "textures/x@k2");             // the corridor's other file, again
    byname += row(3, O, std::string(600, 'L'), "textures/l@kc");   // over 510: skipped
    byname += "3\t" + O + "\tFIVE\ttextures/five@kc\textra\n";     // five fields: skipped
    byname += "\n";
    byname += "3\t" + O + "\tLAST\ttextures/last@kc";             // no newline at the end
    std::string manifest =
        "# key\tformat\tsrc_w\tsrc_h\tw\th\tbytes\tsrc_sha256_16\tmax\tsettings\n";
    manifest += manifestLine("textures/x@k1", 64, 64);
    manifest += manifestLine("textures/x@k2", 64, 64);
    manifest += manifestLine("textures/y@k3", 32, 32);
    manifest += manifestLine("textures/y@k4", 32, 64);
    manifest += manifestLine("textures/z@k5", 16, 16);
    manifest += manifestLine("textures/x@k6", 128, 64);
    manifest += manifestLine("textures/x@k7", 64, 64);
    manifest += manifestLine("textures/w@k8", 64, 64);
    manifest += manifestLine("textures/paged@k9", 64, 2048);
    manifest += manifestLine("textures/mixed@KB", 8, 8);
    manifest += manifestLine("textures/v@kd", 64, 64);
    manifest += manifestLine("textures/last@kc", 64, 64);
    return writeFile(dir + "/byname.txt", byname) && writeFile(dir + "/manifest.txt", manifest);
}

} // namespace

// The rule-hitting set: the oracle's answers, by the table and by the fallback;
// a few answers named outright so the comparison cannot pass on two empty
// indexes.
MDK_TEST(BynameIndexSyntheticSameAsText) {
    mdk::formats::MdkData game = locateData();
    MDK_CHECK(game.found());
    if (!game.found()) {
        return;
    }
    mdk::LevelData level;
    MDK_CHECK(mdk::LevelData::load(&level, &game, 3));
    Synthetic s;
    MDK_CHECK(pickArenas(level, &s));
    const std::string dir = makeTempDir();
    MDK_CHECK(!dir.empty());
    MDK_CHECK(writeSynthetic(dir, s));
    MDK_CHECK(writeTable(dir));
    MDK_CHECK(sameBlockBytes(dir, 3));
    MDK_CHECK(sameBlockBytes(dir, 4));
    MDK_CHECK(sameBlockBytes(dir, 5));

    const mdk::OrigManifestStamp stamp = stampOf(dir + "/manifest.txt");
    mdk::BynameSource source = mdk::BynameSource::None;
    std::string refused;
    mdk::OrigByName now;
    MDK_CHECK_EQ(compareLevel(dir, 3, level, stamp, &source, &refused, &now), 0);
    MDK_CHECK(source == mdk::BynameSource::Binary);
    MDK_CHECK_STR_EQ(refused, "");

    // Named outright. The HD map is hdFor's (two manifest keys in three, in key
    // order): k1 and k9 have an HD file, k2 and k3 do not.
    const void* o = &level.paletteOf(level.arenaNamed(s.owner));
    const void* p = &level.paletteOf(level.arenaNamed(s.other));
    std::string path;
    int32_t r = now.find(o, "x");
    MDK_CHECK(r >= 0);
    if (r >= 0) {
        now.origPath(kRoot, (uint32_t)r, &path);
        MDK_CHECK_STR_EQ(path, "romfs:/orig/textures/x@k1.t3x");
    }
    r = now.find(o, "y");
    MDK_CHECK(r >= 0);
    if (r >= 0) {
        now.origPath(kRoot, (uint32_t)r, &path);
        MDK_CHECK_STR_EQ(path, "romfs:/orig/textures/y@k4.t3x");
        MDK_CHECK_EQ(now.index.row((uint32_t)r).dims, (32u << 16) | 64u);
    }
    r = now.find(p, "x");
    MDK_CHECK(r >= 0);
    if (r >= 0) {
        now.origPath(kRoot, (uint32_t)r, &path);
        MDK_CHECK_STR_EQ(path, "romfs:/orig/textures/x@k6.t3x");
    }
    r = now.find(o, "paged");
    MDK_CHECK(r >= 0 && now.hd[(uint32_t)r] == nullptr);
    r = now.find(o, "nodims");
    MDK_CHECK(r >= 0 && !now.index.row((uint32_t)r).hasDims());
    MDK_CHECK(now.find(o, "v") >= 0);
    MDK_CHECK(now.find(o, "last") >= 0);
    MDK_CHECK(now.find(o, "five") < 0);
    MDK_CHECK(now.find(o, "w") < 0);
    MDK_CHECK(now.find(o, std::string(600, 'l').c_str()) < 0);
    MDK_CHECK(now.find(o, "nosuchname") < 0);
    MDK_CHECK(now.answers >= 9);
    r = now.find(o, "x");
    MDK_CHECK(r >= 0 && now.hd[(uint32_t)r] != nullptr);
    // The corridor's ignored rows put its arena's HD file in its window, twice.
    int corridorRows = 0;
    for (size_t i = 0; i < now.hdArenaRows.size(); i++) {
        if (s.corridor == now.arenaOf(now.hdArenaRows[i].row) &&
            *now.hdArenaRows[i].path == "sdmc:/3ds/mdk/hd/textures/LEVEL3/x@k1.t3x") {
            corridorRows++;
        }
    }
    MDK_CHECK_EQ(corridorRows, 2);

    // The fallback: no table, the same answers from byname.txt.
    MDK_CHECK(unlink((dir + "/byname.bin").c_str()) == 0);
    MDK_CHECK_EQ(compareLevel(dir, 3, level, stamp, &source, &refused, &now), 0);
    MDK_CHECK(source == mdk::BynameSource::Text);
    MDK_CHECK_STR_EQ(refused, "no byname.bin");
    removeTree(dir);
}

// THE STAMP AND THE REFUSALS: each falls back to byname.txt with the oracle's
// answers -- including a byname.txt changed after the table was written, whose
// NEW rows are what must be answered.
MDK_TEST(BynameIndexStaleOrBadTableFallsBack) {
    mdk::formats::MdkData game = locateData();
    MDK_CHECK(game.found());
    if (!game.found()) {
        return;
    }
    mdk::LevelData level;
    MDK_CHECK(mdk::LevelData::load(&level, &game, 3));
    Synthetic s;
    MDK_CHECK(pickArenas(level, &s));
    const std::string dir = makeTempDir();
    MDK_CHECK(writeSynthetic(dir, s));
    MDK_CHECK(writeTable(dir));
    std::string good;
    MDK_CHECK(readFile(dir + "/byname.bin", &good));
    std::string text;
    MDK_CHECK(readFile(dir + "/byname.txt", &text));
    const mdk::OrigManifestStamp stamp = stampOf(dir + "/manifest.txt");

    struct Case {
        const char* what;
        std::string table;          // byname.bin's bytes ("" : none)
        bool otherManifest;         // the stamp handed in is not the table's
        bool otherText;             // byname.txt changed after the table
        const char* refusedPrefix;
    };
    std::string badMagic = good;
    badMagic[0] = 'X';
    std::string badVersion = good;
    badVersion[8] = 9;
    // A byte in the middle of level 3's block.
    std::string badBlock = good;
    BynameIndex::Header header;
    MDK_CHECK(BynameIndex::parseHeader((const uint8_t*)good.data(), good.size(),
                                       (uint32_t)good.size(), &header) == BynameIndex::Status::Ok);
    badBlock[header.slots[3].offset + header.slots[3].bytes / 2] ^= 0x40;
    const Case cases[] = {
        {"manifest stamp", good, true, false, "byname.bin stamp: built from a "},
        {"byname.txt changed", good, false, true, "byname.bin stamp: built from a "},
        {"truncated", good.substr(0, good.size() - 4), false, false, "byname.bin header: "},
        {"header only", good.substr(0, 100), false, false, "byname.bin header: "},
        {"bad magic", badMagic, false, false, "byname.bin header: bad magic"},
        {"bad version", badVersion, false, false, "byname.bin header: bad version"},
        {"corrupt block", badBlock, false, false, "byname.bin level block: bad hash"},
        {"no table", "", false, false, "no byname.bin"},
    };
    for (const Case& c : cases) {
        std::printf("    case: %s\n", c.what);
        unlink((dir + "/byname.bin").c_str());
        if (!c.table.empty()) {
            MDK_CHECK(writeFile(dir + "/byname.bin", c.table));
        }
        // A changed byname.txt: one more row, the same manifest.
        const std::string now =
            c.otherText ? text + "\n" + row(3, s.owner, "ADDED", "textures/x@k1") : text;
        MDK_CHECK(writeFile(dir + "/byname.txt", now));
        mdk::OrigManifestStamp handed = stamp;
        if (c.otherManifest) {
            handed.hash ^= 1;
        }
        mdk::BynameSource source = mdk::BynameSource::None;
        std::string refused;
        mdk::OrigByName index;
        MDK_CHECK_EQ(compareLevel(dir, 3, level, handed, &source, &refused, &index), 0);
        MDK_CHECK(source == mdk::BynameSource::Text);
        MDK_CHECK(refused.compare(0, strlen(c.refusedPrefix), c.refusedPrefix) == 0);
        if (refused.compare(0, strlen(c.refusedPrefix), c.refusedPrefix) != 0) {
            std::printf("    refused: %s\n", refused.c_str());
        }
        if (c.otherText) {
            const void* o = &level.paletteOf(level.arenaNamed(s.owner));
            MDK_CHECK(index.find(o, "added") >= 0);
        }
    }
    // Neither file: no index at all.
    unlink((dir + "/byname.bin").c_str());
    unlink((dir + "/byname.txt").c_str());
    mdk::OrigByName none;
    mdk::BynameLoadReport report;
    std::map<std::string, std::string> hdByKey;
    std::map<std::string, uint32_t> dims;
    MDK_CHECK(!mdk::loadOrigByName(dir.c_str(), 3, level, stamp, &hdByKey, dims, &none, &report));
    MDK_CHECK(report.source == mdk::BynameSource::None);
    MDK_CHECK_EQ(none.answers, 0);
    removeTree(dir);
}

// THE BLOCK'S OWN CHECKS: a block whose sorted list is out of order, names a row
// twice, or points a string or an arena outside the block is refused whole.
MDK_TEST(BynameIndexRefusesABadBlock) {
    const std::string text = "3\tA_1\tbeta\ttextures/b@2\n3\tA_1\talpha\ttextures/a@1\n"
                             "3\tB_1\tbeta\ttextures/b@3\n";
    std::map<std::string, uint32_t> dims;
    std::vector<uint8_t> block;
    MDK_CHECK(BynameIndex::buildBlock(3, (const uint8_t*)text.data(), text.size(), dims, &block));
    std::vector<uint8_t> copy = block;
    BynameIndex index;
    MDK_CHECK(index.adopt(&copy, false, 0) == BynameIndex::Status::Ok);
    MDK_CHECK_EQ(index.rowCount(), 3u);
    MDK_CHECK_EQ(index.arenaCount(), 2u);
    uint32_t first = 0;
    uint32_t last = 0;
    index.equalRange("beta", &first, &last);
    MDK_CHECK_EQ(last - first, 2u);
    MDK_CHECK_EQ(index.sortedRow(first), 0u);   // file order within a name
    MDK_CHECK_EQ(index.sortedRow(first + 1), 2u);
    index.equalRange("alpha", &first, &last);
    MDK_CHECK_EQ(last - first, 1u);
    index.equalRange("gamma", &first, &last);
    MDK_CHECK_EQ(last - first, 0u);

    const size_t sorted = BynameIndex::kBlockHeaderBytes + 2 * 4 + 3 * BynameIndex::kRowBytes;
    const size_t rows = BynameIndex::kBlockHeaderBytes + 2 * 4;
    // sorted is [1, 0, 2]: alpha, then beta's rows in file order.
    struct Damage {
        const char* what;
        size_t at;
        uint8_t value;
        size_t at2;     // a second byte (0: none)
        uint8_t value2;
    };
    const Damage damage[] = {
        {"beta's rows out of file order", sorted + 4, 2, sorted + 8, 0},
        {"names out of order", sorted, 0, sorted + 4, 1},
        {"a row twice", sorted + 4, 1, 0, 0},
        {"a row past the end", sorted + 8, 7, 0, 0},
        {"an arena past the table", rows, 5, 0, 0},
        {"a name past the strings", rows + 4 + 3, 0x7F, 0, 0},
        {"an unknown flag", rows + 2, 0x80, 0, 0},
    };
    for (const Damage& d : damage) {
        std::vector<uint8_t> bad = block;
        bad[d.at] = d.value;
        if (d.at2 != 0) {
            bad[d.at2] = d.value2;
        }
        BynameIndex refused;
        const bool ok = refused.adopt(&bad, false, 0) == BynameIndex::Status::Ok;
        MDK_CHECK(!ok);
        if (ok) {
            std::printf("    accepted: %s\n", d.what);
        }
        MDK_CHECK(refused.empty());
    }
    std::vector<uint8_t> shortBlock(block.begin(), block.end() - 4);
    BynameIndex refused;
    MDK_CHECK(refused.adopt(&shortBlock, false, 0) != BynameIndex::Status::Ok);
}

// THE REAL ROWS: every level of the built orig pack (MDK_BYNAME_DIR, else the
// repo's build-arena-etc1 that stage_romfs.sh fills), through the oracle and
// the table, on the real levels. Without a built pack it says so: the synthetic
// set above still runs.
MDK_TEST(BynameIndexRealPackSameAsText) {
    const char* env = getenv("MDK_BYNAME_DIR");
    const std::string source = env && *env ? env : "../../build-arena-etc1";
    struct stat st;
    if (stat((source + "/byname.txt").c_str(), &st) != 0 ||
        stat((source + "/manifest.txt").c_str(), &st) != 0) {
        std::printf("    SKIPPED: no built orig pack at %s (set MDK_BYNAME_DIR, or build the "
                    "3DS target once); the synthetic cases still ran\n",
                    source.c_str());
        return;
    }
    mdk::formats::MdkData game = locateData();
    MDK_CHECK(game.found());
    if (!game.found()) {
        return;
    }
    const std::string dir = makeTempDir();
    MDK_CHECK(copyFile(source + "/byname.txt", dir + "/byname.txt"));
    MDK_CHECK(copyFile(source + "/manifest.txt", dir + "/manifest.txt"));
    MDK_CHECK(writeTable(dir));
    const mdk::OrigManifestStamp stamp = stampOf(dir + "/manifest.txt");
    for (int32_t number = 3; number <= 8; number++) {
        mdk::LevelData level;
        MDK_CHECK(mdk::LevelData::load(&level, &game, number));
        MDK_CHECK(sameBlockBytes(dir, number));
        mdk::BynameSource from = mdk::BynameSource::None;
        std::string refused;
        mdk::OrigByName now;
        MDK_CHECK_EQ(compareLevel(dir, number, level, stamp, &from, &refused, &now), 0);
        MDK_CHECK(from == mdk::BynameSource::Binary);
        MDK_CHECK(now.answers > 500);
        std::printf("    level %d: %d rows, %d answers, %d with an HD copy, %zu window rows "
                    "(table == text)\n",
                    (int)number, (int)now.index.rowCount(), (int)now.answers,
                    (int)now.hdAnswers, now.hdArenaRows.size());
    }
    // And the fallback, on the real rows of the largest level.
    MDK_CHECK(unlink((dir + "/byname.bin").c_str()) == 0);
    mdk::LevelData level;
    MDK_CHECK(mdk::LevelData::load(&level, &game, 4));
    mdk::BynameSource from = mdk::BynameSource::None;
    std::string refused;
    mdk::OrigByName now;
    MDK_CHECK_EQ(compareLevel(dir, 4, level, stamp, &from, &refused, &now), 0);
    MDK_CHECK(from == mdk::BynameSource::Text);
    removeTree(dir);
}
