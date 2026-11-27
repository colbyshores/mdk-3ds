// ============================================================================
// byname_index.cpp -- see byname_index.h.
// ============================================================================

#include "formats/byname_index.h"

#include <string.h>

#include <algorithm>

namespace mdk {
namespace formats {

namespace {

const char kMagic[8] = {'M', 'D', 'K', 'B', 'Y', 'N', 'M', '\0'};

uint32_t readU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

uint16_t readU16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint64_t readU64(const uint8_t* p) {
    return (uint64_t)readU32(p) | ((uint64_t)readU32(p + 4) << 32);
}

void putU32(std::vector<uint8_t>* out, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        (*out)[at + (size_t)i] = (uint8_t)(v >> (8 * i));
    }
}

void putU16(std::vector<uint8_t>* out, size_t at, uint16_t v) {
    (*out)[at] = (uint8_t)v;
    (*out)[at + 1] = (uint8_t)(v >> 8);
}

// The C library's atoi on a field that holds no tab or newline: leading
// whitespace, a sign, digits. Overflow wraps (atoi's is undefined; no row is
// near it).
int32_t fieldAtoi(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\v' ||
                            s[i] == '\f' || s[i] == '\r')) {
        i++;
    }
    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        negative = s[i] == '-';
        i++;
    }
    uint32_t value = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        value = value * 10u + (uint32_t)(s[i] - '0');
        i++;
    }
    return (int32_t)(negative ? 0u - value : value);
}

std::string lowerAscii(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i] >= 'A' && out[i] <= 'Z') {
            out[i] = (char)(out[i] - 'A' + 'a');
        }
    }
    return out;
}

struct TextRow {
    uint32_t arena;
    uint32_t name;
    uint32_t rel;
    uint32_t key;
    uint16_t flags;
    uint32_t dims;
};

// The string block being written: each string once, in first-use order.
struct Strings {
    std::map<std::string, uint32_t> offsets;
    std::vector<uint8_t> bytes;
    uint32_t intern(const std::string& s) {
        std::map<std::string, uint32_t>::const_iterator found = offsets.find(s);
        if (found != offsets.end()) {
            return found->second;
        }
        const uint32_t at = (uint32_t)bytes.size();
        bytes.insert(bytes.end(), s.begin(), s.end());
        bytes.push_back(0);
        offsets[s] = at;
        return at;
    }
};

} // namespace

bool BynameIndex::nextLine(const char* text, size_t size, size_t* pos, const char** line,
                           size_t* length, bool* newline) {
    while (*pos < size) {
        const char* start = text + *pos;
        const char* nl = (const char*)memchr(start, '\n', size - *pos);
        const size_t n = nl ? (size_t)(nl - start) : size - *pos;
        *pos += n + (nl ? 1 : 0);
        if (n > kMaxLineChars) {
            continue;
        }
        *line = start;
        *length = n;
        *newline = nl != nullptr;
        return true;
    }
    return false;
}

uint64_t BynameIndex::hash(const uint8_t* bytes, size_t size) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; i++) {
        h ^= bytes[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

BynameIndex::Status BynameIndex::parseHeader(const uint8_t* bytes, size_t size,
                                             uint32_t actualFileBytes, Header* out) {
    if (size < kHeaderBytes) {
        return Status::TooSmall;
    }
    if (memcmp(bytes, kMagic, sizeof(kMagic)) != 0) {
        return Status::BadMagic;
    }
    Header header;
    header.version = readU32(bytes + 8);
    if (header.version != kVersion) {
        return Status::BadVersion;
    }
    if (readU32(bytes + 12) != kLevelSlots) {
        return Status::BadLayout;
    }
    header.fileBytes = readU32(bytes + 16);
    if (header.fileBytes < kHeaderBytes) {
        return Status::BadLayout;
    }
    if (header.fileBytes != actualFileBytes) {
        return Status::Truncated;
    }
    header.stamp.manifestBytes = readU32(bytes + 20);
    header.stamp.manifestHash = readU64(bytes + 24);
    header.stamp.bynameBytes = readU32(bytes + 32);
    header.stamp.bynameHash = readU64(bytes + 40);
    for (uint32_t i = 0; i < kLevelSlots; i++) {
        const uint8_t* slot = bytes + 64 + i * 16;
        Slot& s = header.slots[i];
        s.offset = readU32(slot);
        s.bytes = readU32(slot + 4);
        s.hash = readU64(slot + 8);
        if (s.bytes == 0) {
            continue;
        }
        if (s.offset < kHeaderBytes || (s.offset & 3u) != 0 || (s.bytes & 3u) != 0 ||
            s.bytes > kMaxBlockBytes || (uint64_t)s.offset + s.bytes > header.fileBytes) {
            return Status::BadLayout;
        }
    }
    *out = header;
    return Status::Ok;
}

void BynameIndex::clear() {
    std::vector<uint8_t>().swap(block_);
    rows_ = 0;
    arenas_ = 0;
    arenaTable_ = 0;
    rowTable_ = 0;
    sortedTable_ = 0;
    strings_ = 0;
    stringBytes_ = 0;
}

BynameIndex::Status BynameIndex::adopt(std::vector<uint8_t>* bytes, bool checkHash,
                                       uint64_t expected) {
    clear();
    const std::vector<uint8_t>& b = *bytes;
    if (b.size() < kBlockHeaderBytes) {
        return Status::TooSmall;
    }
    if (checkHash && hash(b.data(), b.size()) != expected) {
        return Status::BadHash;
    }
    const uint32_t rows = readU32(b.data());
    const uint32_t arenas = readU32(b.data() + 4);
    const uint32_t stringBytes = readU32(b.data() + 8);
    if (rows > kMaxRows || arenas > kMaxRows || stringBytes > kMaxBlockBytes) {
        return Status::BadLayout;
    }
    const uint64_t need = (uint64_t)kBlockHeaderBytes + (uint64_t)arenas * 4u +
                          (uint64_t)rows * (kRowBytes + 4u) + stringBytes;
    if (b.size() != ((need + 3u) & ~(uint64_t)3u)) {
        return b.size() < need ? Status::TooSmall : Status::BadLayout;
    }
    const uint32_t arenaTable = kBlockHeaderBytes;
    const uint32_t rowTable = arenaTable + arenas * 4u;
    const uint32_t sortedTable = rowTable + rows * kRowBytes;
    const uint32_t strings = sortedTable + rows * 4u;
    // Every string ends inside the block: the block's last string byte is a NUL.
    if (stringBytes == 0 ? (rows != 0 || arenas != 0) : b[strings + stringBytes - 1] != 0) {
        return Status::BadLayout;
    }
    for (uint32_t i = 0; i < arenas; i++) {
        if (readU32(b.data() + arenaTable + i * 4u) >= stringBytes) {
            return Status::BadLayout;
        }
    }
    for (uint32_t i = 0; i < rows; i++) {
        const uint8_t* r = b.data() + rowTable + i * kRowBytes;
        const uint32_t key = readU32(r + 12);
        if (readU16(r) >= arenas || (readU16(r + 2) & ~kFlagDims) != 0 ||
            readU32(r + 4) >= stringBytes || readU32(r + 8) >= stringBytes ||
            (key != kNoKey && key >= stringBytes)) {
            return Status::BadLayout;
        }
    }
    // The sorted list: each row once, by (name, row number).
    std::vector<uint8_t> seen(rows, 0);
    const char* base = (const char*)b.data() + strings;
    for (uint32_t i = 0; i < rows; i++) {
        const uint32_t row = readU32(b.data() + sortedTable + i * 4u);
        if (row >= rows || seen[row]) {
            return Status::BadLayout;
        }
        seen[row] = 1;
        if (i > 0) {
            const uint32_t prev = readU32(b.data() + sortedTable + (i - 1) * 4u);
            const int order = strcmp(base + readU32(b.data() + rowTable + prev * kRowBytes + 4),
                                     base + readU32(b.data() + rowTable + row * kRowBytes + 4));
            if (order > 0 || (order == 0 && prev > row)) {
                return Status::BadLayout;
            }
        }
    }
    block_.swap(*bytes);
    rows_ = rows;
    arenas_ = arenas;
    arenaTable_ = arenaTable;
    rowTable_ = rowTable;
    sortedTable_ = sortedTable;
    strings_ = strings;
    stringBytes_ = stringBytes;
    return Status::Ok;
}

bool BynameIndex::buildBlock(int32_t level, const uint8_t* text, size_t size,
                             const std::map<std::string, uint32_t>& dims,
                             std::vector<uint8_t>* out) {
    Strings strings;
    std::map<std::string, uint32_t> arenaIndex;
    std::vector<uint32_t> arenaTable;
    std::vector<TextRow> rows;
    std::vector<std::string> names;   // each row's lower-cased name, for the sort
    size_t pos = 0;
    const char* start = nullptr;
    size_t length = 0;
    bool newline = false;
    while (nextLine((const char*)text, size, &pos, &start, &length, &newline)) {
        const uint8_t* line = (const uint8_t*)start;
        if (length == 0 || line[0] == '#' || memchr(line, 0, length) != nullptr) {
            continue;
        }
        // level \t arena \t name \t rel   (arena_etc1_convert.py)
        std::string fields[4];
        size_t field = 0;
        size_t from = 0;
        bool tooMany = false;
        for (size_t i = 0; i <= length; i++) {
            if (i == length || line[i] == '\t') {
                if (field == 4) {
                    tooMany = true;
                    break;
                }
                fields[field++].assign((const char*)line + from, i - from);
                from = i + 1;
            }
        }
        if (tooMany || field != 4 || fieldAtoi(fields[0]) != level) {
            continue;
        }
        if (rows.size() >= kMaxRows) {
            return false;
        }
        TextRow row;
        std::map<std::string, uint32_t>::const_iterator known = arenaIndex.find(fields[1]);
        if (known == arenaIndex.end()) {
            if (arenaTable.size() >= kMaxRows) {
                return false;
            }
            row.arena = (uint32_t)arenaTable.size();
            arenaIndex[fields[1]] = row.arena;
            arenaTable.push_back(strings.intern(fields[1]));
        } else {
            row.arena = known->second;
        }
        names.push_back(lowerAscii(fields[2]));
        row.name = strings.intern(names.back());
        row.rel = strings.intern(fields[3]);
        const size_t mark = fields[3].find('@');
        row.key = kNoKey;
        if (mark != std::string::npos) {
            row.key = strings.intern(lowerAscii(fields[3].substr(mark + 1)));
        }
        std::map<std::string, uint32_t>::const_iterator source = dims.find(fields[3]);
        row.flags = source != dims.end() ? kFlagDims : 0;
        row.dims = source != dims.end() ? source->second : 0;
        rows.push_back(row);
    }
    const uint32_t count = (uint32_t)rows.size();
    std::vector<uint32_t> sorted(count);
    for (uint32_t i = 0; i < count; i++) {
        sorted[i] = i;
    }
    std::sort(sorted.begin(), sorted.end(), [&names](uint32_t a, uint32_t b) {
        const int order = strcmp(names[a].c_str(), names[b].c_str());
        return order != 0 ? order < 0 : a < b;
    });
    const uint32_t stringBytes = (uint32_t)strings.bytes.size();
    const size_t need = kBlockHeaderBytes + arenaTable.size() * 4u + count * (kRowBytes + 4u) +
                        stringBytes;
    out->assign((need + 3u) & ~(size_t)3u, 0);
    putU32(out, 0, count);
    putU32(out, 4, (uint32_t)arenaTable.size());
    putU32(out, 8, stringBytes);
    size_t at = kBlockHeaderBytes;
    for (size_t i = 0; i < arenaTable.size(); i++, at += 4) {
        putU32(out, at, arenaTable[i]);
    }
    for (uint32_t i = 0; i < count; i++, at += kRowBytes) {
        putU16(out, at, (uint16_t)rows[i].arena);
        putU16(out, at + 2, rows[i].flags);
        putU32(out, at + 4, rows[i].name);
        putU32(out, at + 8, rows[i].rel);
        putU32(out, at + 12, rows[i].key);
        putU32(out, at + 16, rows[i].dims);
    }
    for (uint32_t i = 0; i < count; i++, at += 4) {
        putU32(out, at, sorted[i]);
    }
    if (stringBytes > 0) {
        memcpy(out->data() + at, strings.bytes.data(), stringBytes);
    }
    return true;
}

const char* BynameIndex::string(uint32_t offset) const {
    return (const char*)block_.data() + strings_ + offset;
}

const char* BynameIndex::arenaName(uint32_t arena) const {
    return arena < arenas_ ? string(readU32(block_.data() + arenaTable_ + arena * 4u)) : "";
}

BynameIndex::Row BynameIndex::row(uint32_t index) const {
    Row row;
    if (index >= rows_) {
        return row;
    }
    const uint8_t* r = block_.data() + rowTable_ + index * kRowBytes;
    row.arena = readU16(r);
    row.flags = readU16(r + 2);
    row.name = string(readU32(r + 4));
    row.rel = string(readU32(r + 8));
    const uint32_t key = readU32(r + 12);
    row.key = key == kNoKey ? nullptr : string(key);
    row.dims = readU32(r + 16);
    return row;
}

uint32_t BynameIndex::sortedRow(uint32_t i) const {
    return i < rows_ ? readU32(block_.data() + sortedTable_ + i * 4u) : 0;
}

void BynameIndex::equalRange(const char* name, uint32_t* first, uint32_t* last) const {
    // The first position whose name is not below `name`, then the first above it.
    uint32_t lo = 0;
    uint32_t hi = rows_;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (strcmp(row(sortedRow(mid)).name, name) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    uint32_t end = lo;
    while (end < rows_ && strcmp(row(sortedRow(end)).name, name) == 0) {
        end++;
    }
    *first = lo;
    *last = end;
}

const char* BynameIndex::statusName(Status status) {
    switch (status) {
    case Status::Ok:
        return "ok";
    case Status::TooSmall:
        return "too small";
    case Status::BadMagic:
        return "bad magic";
    case Status::BadVersion:
        return "bad version";
    case Status::BadLayout:
        return "bad layout";
    case Status::Truncated:
        return "truncated";
    case Status::BadHash:
        return "bad hash";
    }
    return "unknown";
}

} // namespace formats
} // namespace mdk
