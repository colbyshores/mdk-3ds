// ============================================================================
// kurt_pack_tests.cpp -- Kurt's pack (romfs:/kurt/kurt.pak): the writer
// (mdk_3ds/tools/kurt_pack.py) round-tripped through the reader
// (formats/kurt_pack.h), and the reader's refusals.
//
// WHAT IS PINNED HERE.
//   1. THE ROUND TRIP. A set (manifest.txt + textures/**.t3x) packed by the
//      Python writer parses here, every entry's bytes are its file's bytes, the
//      start set is the contiguous front of the data, an animation's frames are
//      contiguous and in frame order, a level's own copy is found for its level
//      and the every-level copy for the others.
//   2. DETERMINISM AND INCREMENTALITY. Two packs of the same set are byte-
//      identical, and an unchanged pack is not rewritten.
//   3. THE REFUSALS. A short file, a bad magic, a bad version, a bad layout and
//      a truncated file refuse the whole pack; an entry out of range, a start-set
//      entry outside the start set and an unterminated name refuse that entry
//      only -- and a level's own bad copy is not replaced by the every-level one.
// ============================================================================

#include "test_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "formats/kurt_pack.h"

using mdk::formats::KurtPack;

namespace {

const char kWriter[] = "../../mdk_3ds/tools/kurt_pack.py";

std::string makeTempDir() {
    const char* base = getenv("TMPDIR");
    std::string pattern = std::string(base && *base ? base : "/tmp") + "/mdk-kurtpack-XXXXXX";
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

bool writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) {
        return false;
    }
    const bool ok = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return fclose(file) == 0 && ok;
}

bool readFile(const std::string& path, std::vector<uint8_t>* out) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        return false;
    }
    out->clear();
    uint8_t chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        out->insert(out->end(), chunk, chunk + got);
    }
    fclose(file);
    return true;
}

// Runs a command, its output into `output`; the exit status.
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

// A stand-in .t3x: bytes that say which frame they are (the pack never looks
// inside them), of an odd length so the padding is exercised.
std::vector<uint8_t> fakeT3x(const std::string& rel, size_t size) {
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; i++) {
        bytes[i] = (uint8_t)(rel[i % rel.size()] + i);
    }
    return bytes;
}

struct SetFrame {
    const char* rel;   // the manifest key
    int width;
    int height;
    size_t size;
    // The manifest's `max` column ("orig", or the cap the converter fitted the
    // image to) and the image size the pack must then carry -- worked out here
    // by hand from kurt_pack.fit_size's rule (the long side 256, the short one
    // rounded), not by calling it.
    const char* cap;
    int imageWidth;
    int imageHeight;
};

// A set in kurt_etc1_convert.py's layout, deliberately listed out of the pack's
// order: a start-set animation after another, frames out of order, a level copy.
// K_CHUTEC_0 (347x295: 256 x 295*256/347 = 217.6 -> 218) and K_LOOKD_0 (287x343:
// 287*256/343 = 214.2 -> 214 x 256) are FITTED, as the converter stores them.
const SetFrame kSet[] = {
    {"textures/K_RUN_1", 60, 150, 37, "orig", 60, 150},
    {"textures/K_FALL_0", 70, 120, 101, "orig", 70, 120},
    {"textures/K_RUN_0", 61, 151, 48, "orig", 61, 151},
    {"textures/K_STILL_0", 52, 146, 77, "orig", 52, 146},
    {"textures/K_RUN_10", 62, 152, 19, "orig", 62, 152},
    {"textures/K_LOOKD_0", 287, 343, 55, "256", 214, 256},
    {"textures/LEVEL3/K_LOOKD_0", 287, 343, 56, "256", 214, 256},
    {"textures/K_RUN_2", 63, 153, 64, "orig", 63, 153},
    {"textures/K_BANG_0", 58, 144, 33, "orig", 58, 144},
    {"textures/K_CHUTEC_0", 347, 295, 91, "256", 256, 218},
};

bool writeSet(const std::string& dir) {
    if (mkdir((dir + "/textures").c_str(), 0777) != 0 ||
        mkdir((dir + "/textures/LEVEL3").c_str(), 0777) != 0) {
        return false;
    }
    std::string manifest = "# key\tformat\tsrc_w\tsrc_h\tw\th\tbytes\tsrc_sha256_16\tmax\tsettings\n";
    for (const SetFrame& frame : kSet) {
        const std::vector<uint8_t> bytes = fakeT3x(frame.rel, frame.size);
        if (!writeFile(dir + "/" + frame.rel + ".t3x", bytes)) {
            return false;
        }
        char line[256];
        snprintf(line, sizeof(line), "%s\tetc1a4\t%d\t%d\t64\t256\t%u\t0123456789abcdef\t%s\tv1\n",
                 frame.rel, frame.width, frame.height, (unsigned)frame.size, frame.cap);
        manifest += line;
    }
    return writeFile(dir + "/manifest.txt",
                     std::vector<uint8_t>(manifest.begin(), manifest.end()));
}

const SetFrame* setFrame(const std::string& name, int level) {
    for (const SetFrame& frame : kSet) {
        const std::string rel = frame.rel;
        const std::string want =
            level > 0 ? "textures/LEVEL" + std::to_string(level) + "/" + name : "textures/" + name;
        if (rel == want) {
            return &frame;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// A pack built here, independently of the writer, for the refusals.
// ---------------------------------------------------------------------------

void putU16(std::vector<uint8_t>* out, size_t at, uint16_t v) {
    (*out)[at] = (uint8_t)v;
    (*out)[at + 1] = (uint8_t)(v >> 8);
}

void putU32(std::vector<uint8_t>* out, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        (*out)[at + (size_t)i] = (uint8_t)(v >> (8 * i));
    }
}

struct Built {
    const char* name;
    uint8_t level;
    uint8_t flags;
    uint32_t size;
};

// Three entries: a start-set frame, an every-level frame and its level-5 copy.
std::vector<uint8_t> buildPack() {
    const Built entries[] = {
        {"K_STILL_0", 0, KurtPack::kFlagStartSet, 20},
        {"K_RUN_0", 0, 0, 30},
        {"K_RUN_0", 5, 0, 30},
    };
    const uint32_t count = 3;
    const uint32_t dataOffset = (KurtPack::kHeaderBytes + count * KurtPack::kEntryBytes + 15) / 16 * 16;
    uint32_t offsets[3];
    uint32_t at = dataOffset;
    for (uint32_t i = 0; i < count; i++) {
        offsets[i] = at;
        at = (at + entries[i].size + 15) / 16 * 16;
    }
    const uint32_t startEnd = offsets[0] + entries[0].size;
    const uint32_t fileBytes = offsets[2] + entries[2].size;
    std::vector<uint8_t> out(fileBytes, 0);
    memcpy(out.data(), "KPAK", 4);
    putU16(&out, 4, KurtPack::kVersion);
    putU16(&out, 6, (uint16_t)KurtPack::kHeaderBytes);
    putU32(&out, 8, count);
    putU32(&out, 12, KurtPack::kEntryBytes);
    putU32(&out, 16, dataOffset);
    putU32(&out, 20, startEnd);
    putU32(&out, 24, fileBytes);
    putU32(&out, 28, KurtPack::kAlign);
    for (uint32_t i = 0; i < count; i++) {
        const size_t e = KurtPack::kHeaderBytes + (size_t)i * KurtPack::kEntryBytes;
        memcpy(out.data() + e, entries[i].name, strlen(entries[i].name));
        out[e + 24] = entries[i].level;
        out[e + 25] = entries[i].flags;
        putU16(&out, e + 28, 50);
        putU16(&out, e + 30, 140);
        putU32(&out, e + 32, offsets[i]);
        putU32(&out, e + 36, entries[i].size);
        putU16(&out, e + 40, 50);
        putU16(&out, e + 42, 140);
        for (uint32_t b = 0; b < entries[i].size; b++) {
            out[offsets[i] + b] = (uint8_t)(i + 1);
        }
    }
    return out;
}

size_t entryAt(uint32_t i) {
    return KurtPack::kHeaderBytes + (size_t)i * KurtPack::kEntryBytes;
}

} // namespace

// THE ROUND TRIP: the Python writer's pack, read here.
MDK_TEST(KurtPackWriterRoundTripsThroughTheReader) {
    MDK_CHECK(access(kWriter, R_OK) == 0);
    const std::string dir = makeTempDir();
    MDK_CHECK(!dir.empty());
    if (dir.empty() || access(kWriter, R_OK) != 0) {
        return;
    }
    MDK_CHECK(writeSet(dir));
    std::string output;
    MDK_CHECK_EQ(run(std::string("python3 ") + kWriter + " '" + dir + "' '" + dir + "/a.pak'",
                     &output),
                 0);
    MDK_CHECK_EQ(run(std::string("python3 ") + kWriter + " '" + dir + "' '" + dir + "/b.pak'",
                     nullptr),
                 0);
    std::vector<uint8_t> a;
    std::vector<uint8_t> b;
    MDK_CHECK(readFile(dir + "/a.pak", &a));
    MDK_CHECK(readFile(dir + "/b.pak", &b));
    // DETERMINISTIC: the same set, the same bytes.
    MDK_CHECK(!a.empty() && a == b);
    // INCREMENTAL: an unchanged pack is not rewritten.
    std::string again;
    MDK_CHECK_EQ(run(std::string("python3 ") + kWriter + " '" + dir + "' '" + dir + "/a.pak'",
                     &again),
                 0);
    MDK_CHECK(again.find("unchanged") != std::string::npos);
    MDK_CHECK(output.find("unchanged") == std::string::npos);

    KurtPack pack;
    MDK_CHECK(KurtPack::parse(a.data(), a.size(), &pack) == KurtPack::Status::Ok);
    const KurtPack::Header& header = pack.header();
    const size_t count = sizeof(kSet) / sizeof(kSet[0]);
    MDK_CHECK_EQ(header.entryCount, count);
    MDK_CHECK_EQ(header.fileBytes, a.size());
    MDK_CHECK_EQ(header.dataOffset % KurtPack::kAlign, 0);
    MDK_CHECK(header.dataOffset >= header.indexEnd());
    MDK_CHECK_EQ(pack.badEntries(), 0);
    MDK_CHECK_EQ(pack.entries().size(), count);
    if (pack.entries().size() != count) {
        removeTree(dir);
        return;
    }

    // THE ORDER: the start set first, in KurtSprite's order (K_STILL, K_FALL);
    // then the other animations by name, frames in NUMERIC order (10 after 2), a
    // level's copy after the every-level one.
    const char* const order[] = {"K_STILL_0", "K_FALL_0", "K_CHUTEC_0", "K_BANG_0", "K_LOOKD_0", "K_LOOKD_0",
                                 "K_RUN_0",   "K_RUN_1",  "K_RUN_2",  "K_RUN_10"};
    for (size_t i = 0; i < count; i++) {
        MDK_CHECK_STR_EQ(pack.entries()[i].name, order[i]);
    }
    MDK_CHECK_EQ(pack.entries()[4].level, 0);
    MDK_CHECK_EQ(pack.entries()[5].level, 3);

    uint32_t previousEnd = header.dataOffset;
    for (size_t i = 0; i < count; i++) {
        const KurtPack::Entry& entry = pack.entries()[i];
        MDK_CHECK(entry.valid);
        const SetFrame* source = setFrame(entry.name, entry.level);
        MDK_CHECK(source != nullptr);
        if (!source) {
            continue;
        }
        MDK_CHECK_EQ(entry.width, source->width);
        MDK_CHECK_EQ(entry.height, source->height);
        // The image size: the frame's own, or the fitted one for a "256" row.
        MDK_CHECK_EQ(entry.imageWidth, source->imageWidth);
        MDK_CHECK_EQ(entry.imageHeight, source->imageHeight);
        MDK_CHECK_EQ(entry.size, source->size);
        MDK_CHECK_EQ(entry.offset % KurtPack::kAlign, 0);
        // Laid out in index order, with no overlap and no gap past the alignment.
        MDK_CHECK(entry.offset >= previousEnd && entry.offset - previousEnd < KurtPack::kAlign);
        previousEnd = entry.offset + entry.size;
        // The bytes are the file's.
        const std::vector<uint8_t> want = fakeT3x(source->rel, source->size);
        MDK_CHECK(entry.offset + entry.size <= a.size() &&
                  memcmp(a.data() + entry.offset, want.data(), want.size()) == 0);
        // The start set is the front of the data, exactly.
        const bool start = entry.name.rfind("K_STILL_", 0) == 0 ||
                           entry.name.rfind("K_FALL_", 0) == 0 ||
                           entry.name.rfind("K_CHUTEC_", 0) == 0;
        MDK_CHECK_EQ((entry.flags & KurtPack::kFlagStartSet) != 0, start);
        if (start) {
            MDK_CHECK(entry.offset + entry.size <= header.startEnd);
        } else {
            MDK_CHECK(entry.offset >= header.startEnd);
        }
    }
    MDK_CHECK_EQ(header.startEnd, pack.entries()[2].offset + pack.entries()[2].size);
    MDK_CHECK_EQ(previousEnd, a.size());

    // A level's own copy for its level, the every-level copy for the others.
    MDK_CHECK_EQ(pack.find("K_LOOKD_0", 3), 5);
    MDK_CHECK_EQ(pack.find("K_LOOKD_0", 4), 4);
    MDK_CHECK_EQ(pack.find("K_LOOKD_0", 0), 4);
    MDK_CHECK_EQ(pack.find("K_RUN_1", 8), 7);
    MDK_CHECK_EQ(pack.find("K_RUN_3", 3), -1);
    removeTree(dir);
}

// The writer refuses a set it cannot pack faithfully instead of packing it wrong.
MDK_TEST(KurtPackWriterRefusesABadSet) {
    const std::string dir = makeTempDir();
    MDK_CHECK(!dir.empty() && writeSet(dir));
    // The manifest's size disagrees with the file (a half-written .t3x).
    std::vector<uint8_t> shortFile(5, 1);
    MDK_CHECK(writeFile(dir + "/textures/K_RUN_1.t3x", shortFile));
    MDK_CHECK(run(std::string("python3 ") + kWriter + " '" + dir + "'", nullptr) != 0);
    MDK_CHECK(access((dir + "/kurt.pak").c_str(), F_OK) != 0);
    removeTree(dir);
}

MDK_TEST(KurtPackReaderAcceptsAWellFormedPack) {
    const std::vector<uint8_t> bytes = buildPack();
    KurtPack pack;
    MDK_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::Ok);
    MDK_CHECK_EQ(pack.entries().size(), 3);
    MDK_CHECK_EQ(pack.badEntries(), 0);
    MDK_CHECK_EQ(pack.firstBadEntry(), -1);
    MDK_CHECK_EQ(pack.find("K_RUN_0", 5), 2);
    MDK_CHECK_EQ(pack.find("K_RUN_0", 4), 1);
    // The header alone, then the index on its own (the 3DS's two reads).
    KurtPack::Header header;
    MDK_CHECK(KurtPack::parseHeader(bytes.data(), KurtPack::kHeaderBytes, (uint32_t)bytes.size(),
                                    &header) == KurtPack::Status::Ok);
    KurtPack split;
    MDK_CHECK(KurtPack::parseIndex(header, bytes.data() + KurtPack::kHeaderBytes,
                                   header.indexEnd() - KurtPack::kHeaderBytes,
                                   &split) == KurtPack::Status::Ok);
    MDK_CHECK_EQ(split.entries().size(), 3);
    // An index shorter than its count says.
    MDK_CHECK(KurtPack::parseIndex(header, bytes.data() + KurtPack::kHeaderBytes,
                                   header.indexEnd() - KurtPack::kHeaderBytes - 1,
                                   &split) == KurtPack::Status::TooSmall);
    MDK_CHECK(split.entries().empty());
}

// A BAD HEADER refuses the whole pack.
MDK_TEST(KurtPackReaderRefusesACorruptHeader) {
    const std::vector<uint8_t> good = buildPack();
    KurtPack pack;

    MDK_CHECK(KurtPack::parse(good.data(), 10, &pack) == KurtPack::Status::TooSmall);
    MDK_CHECK(KurtPack::parse(nullptr, 0, &pack) == KurtPack::Status::TooSmall);

    std::vector<uint8_t> bytes = good;
    bytes[0] = 'X';
    MDK_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::BadMagic);

    bytes = good;
    putU16(&bytes, 4, KurtPack::kVersion + 1);
    MDK_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::BadVersion);

    // The layout: entry size, header size, alignment, the data before the index
    // ends, the start set past the file, a count no pack has.
    bytes = good;
    putU32(&bytes, 12, KurtPack::kEntryBytes + 1);
    MDK_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::BadLayout);
    bytes = good;
    putU16(&bytes, 6, 33);
    MDK_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::BadLayout);
    bytes = good;
    putU32(&bytes, 28, 4);
    MDK_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::BadLayout);
    bytes = good;
    putU32(&bytes, 16, 64);
    MDK_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::BadLayout);
    bytes = good;
    putU32(&bytes, 20, (uint32_t)good.size() + 16);
    MDK_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::BadLayout);
    bytes = good;
    putU32(&bytes, 8, KurtPack::kMaxEntries + 1);
    MDK_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::BadLayout);
    MDK_CHECK(pack.entries().empty());
}

// A TRUNCATED FILE: shorter than its header says, refused before any read past
// its end.
MDK_TEST(KurtPackReaderRefusesATruncatedFile) {
    const std::vector<uint8_t> good = buildPack();
    KurtPack pack;
    MDK_CHECK(KurtPack::parse(good.data(), good.size() - 1, &pack) ==
              KurtPack::Status::Truncated);
    KurtPack::Header header;
    MDK_CHECK(KurtPack::parseHeader(good.data(), good.size(), (uint32_t)good.size() - 7,
                                    &header) == KurtPack::Status::Truncated);
    // Cut inside the index.
    MDK_CHECK(KurtPack::parse(good.data(), KurtPack::kHeaderBytes + 5, &pack) ==
              KurtPack::Status::Truncated);
}

// A BAD ENTRY costs only its own frame, and the first one is named.
MDK_TEST(KurtPackReaderRefusesAnEntryOutOfRange) {
    const std::vector<uint8_t> good = buildPack();
    KurtPack pack;

    // Past the end of the file.
    std::vector<uint8_t> bytes = good;
    putU32(&bytes, entryAt(1) + 36, (uint32_t)good.size());
    MDK_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::Ok);
    MDK_CHECK_EQ(pack.badEntries(), 1);
    MDK_CHECK_EQ(pack.firstBadEntry(), 1);
    MDK_CHECK_STR_EQ(pack.firstBadReason(), "range");
    MDK_CHECK(pack.entries()[0].valid && !pack.entries()[1].valid && pack.entries()[2].valid);
    // The every-level copy is still found (the caller checks `valid`)...
    MDK_CHECK_EQ(pack.find("K_RUN_0", 4), 1);

    // ...and a level's own BAD copy is not replaced by the every-level one.
    bytes = good;
    putU32(&bytes, entryAt(2) + 32, 0xFFFFFFF0u);
    MDK_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::Ok);
    MDK_CHECK(!pack.entries()[2].valid);
    MDK_CHECK_EQ(pack.find("K_RUN_0", 5), -1);
    MDK_CHECK_EQ(pack.find("K_RUN_0", 6), 1);

    // An offset into the index, a misaligned offset, an empty frame, a size
    // no frame has.
    bytes = good;
    putU32(&bytes, entryAt(1) + 32, KurtPack::kHeaderBytes);
    MDK_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::Ok &&
              !pack.entries()[1].valid);
    bytes = good;
    putU32(&bytes, entryAt(1) + 32, pack.entries()[1].offset + 1);
    MDK_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::Ok &&
              !pack.entries()[1].valid);
    bytes = good;
    putU32(&bytes, entryAt(1) + 36, 0);
    MDK_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::Ok &&
              !pack.entries()[1].valid);
    bytes = good;
    putU16(&bytes, entryAt(1) + 28, 0);
    MDK_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::Ok &&
              !pack.entries()[1].valid);
    MDK_CHECK_STR_EQ(pack.firstBadReason(), "size");
    // An image LARGER than its frame (a fit never enlarges), or of no size.
    bytes = good;
    putU16(&bytes, entryAt(1) + 40, 51);
    MDK_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::Ok &&
              !pack.entries()[1].valid);
    MDK_CHECK_STR_EQ(pack.firstBadReason(), "size");
    bytes = good;
    putU16(&bytes, entryAt(1) + 42, 0);
    MDK_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::Ok &&
              !pack.entries()[1].valid);
    // A smaller image is a fitted frame: valid.
    bytes = good;
    putU16(&bytes, entryAt(1) + 40, 25);
    putU16(&bytes, entryAt(1) + 42, 70);
    MDK_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::Ok &&
              pack.entries()[1].valid && pack.entries()[1].imageWidth == 25 &&
              pack.entries()[1].imageHeight == 70);

    // Flagged as the start set but outside its bytes.
    bytes = good;
    bytes[entryAt(1) + 25] = KurtPack::kFlagStartSet;
    MDK_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::Ok);
    MDK_CHECK(!pack.entries()[1].valid);
    MDK_CHECK_STR_EQ(pack.firstBadReason(), "start set");

    // A name with no terminator inside its field.
    bytes = good;
    memset(bytes.data() + entryAt(0), 'K', KurtPack::kNameBytes);
    MDK_CHECK(KurtPack::parse(bytes.data(), bytes.size(), &pack) == KurtPack::Status::Ok);
    MDK_CHECK(!pack.entries()[0].valid);
    MDK_CHECK_STR_EQ(pack.firstBadReason(), "name");
    MDK_CHECK_EQ(pack.badEntries(), 1);
}
