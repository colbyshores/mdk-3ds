// ============================================================================
// kurt_pack.cpp -- see kurt_pack.h.
// ============================================================================

#include "formats/kurt_pack.h"

#include <string.h>

namespace mdk {
namespace formats {

namespace {

uint16_t u16At(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t u32At(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

} // namespace

KurtPack::Status KurtPack::parseHeader(const uint8_t* bytes, size_t size,
                                       uint32_t actualFileBytes, Header* out) {
    if (!bytes || !out || size < kHeaderBytes || actualFileBytes < kHeaderBytes) {
        return Status::TooSmall;
    }
    if (memcmp(bytes, "KPAK", 4) != 0) {
        return Status::BadMagic;
    }
    Header header;
    header.version = u16At(bytes + 4);
    if (header.version != kVersion) {
        return Status::BadVersion;
    }
    const uint16_t headerBytes = u16At(bytes + 6);
    header.entryCount = u32At(bytes + 8);
    const uint32_t entryBytes = u32At(bytes + 12);
    header.dataOffset = u32At(bytes + 16);
    header.startEnd = u32At(bytes + 20);
    header.fileBytes = u32At(bytes + 24);
    header.align = u32At(bytes + 28);
    // The count is bounded first, so indexEnd() cannot overflow.
    if (headerBytes != kHeaderBytes || entryBytes != kEntryBytes ||
        header.entryCount > kMaxEntries || header.align != kAlign ||
        header.fileBytes > kMaxFileBytes || header.dataOffset < header.indexEnd() ||
        (header.dataOffset % kAlign) != 0 || header.startEnd < header.dataOffset ||
        header.startEnd > header.fileBytes || header.dataOffset > header.fileBytes) {
        return Status::BadLayout;
    }
    // The header's own size against the file's: a short file is refused here,
    // before any read past its end is attempted.
    if (actualFileBytes < header.fileBytes) {
        return Status::Truncated;
    }
    *out = header;
    return Status::Ok;
}

KurtPack::Status KurtPack::parseIndex(const Header& header, const uint8_t* index, size_t size,
                                      KurtPack* out) {
    if (!out) {
        return Status::TooSmall;
    }
    *out = KurtPack();
    const size_t want = (size_t)header.entryCount * kEntryBytes;
    if ((!index && want > 0) || size < want) {
        return Status::TooSmall;
    }
    out->header_ = header;
    out->entries_.resize(header.entryCount);
    for (uint32_t i = 0; i < header.entryCount; i++) {
        const uint8_t* p = index + (size_t)i * kEntryBytes;
        Entry& entry = out->entries_[i];
        size_t length = 0;
        while (length < kNameBytes && p[length] != 0) {
            length++;
        }
        entry.name.assign((const char*)p, length);
        entry.level = p[24];
        entry.flags = p[25];
        entry.width = u16At(p + 28);
        entry.height = u16At(p + 30);
        entry.offset = u32At(p + 32);
        entry.size = u32At(p + 36);
        const char* why = nullptr;
        if (length == 0 || length >= kNameBytes) {
            why = "name";   // empty, or not NUL-terminated inside its field
        } else if (entry.width == 0 || entry.height == 0 || entry.width > 1024 ||
                   entry.height > 1024) {
            why = "size";
        } else if (entry.size == 0 || entry.size > kMaxFrameBytes ||
                   entry.offset < header.dataOffset || entry.offset > header.fileBytes ||
                   entry.size > header.fileBytes - entry.offset ||
                   (entry.offset % kAlign) != 0) {
            why = "range";
        } else if ((entry.flags & kFlagStartSet) != 0 &&
                   (entry.offset >= header.startEnd ||
                    entry.size > header.startEnd - entry.offset)) {
            why = "start set";   // flagged as the start set but outside its bytes
        }
        entry.valid = why == nullptr;
        if (!entry.valid) {
            if (out->firstBad_ < 0) {
                out->firstBad_ = (int32_t)i;
                out->firstBadReason_ = why;
            }
            out->badEntries_++;
        }
    }
    return Status::Ok;
}

KurtPack::Status KurtPack::parse(const uint8_t* bytes, size_t size, KurtPack* out) {
    if (!out) {
        return Status::TooSmall;
    }
    *out = KurtPack();
    Header header;
    const Status status =
        parseHeader(bytes, size, size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)size, &header);
    if (status != Status::Ok) {
        return status;
    }
    return parseIndex(header, bytes + kHeaderBytes, size - kHeaderBytes, out);
}

int32_t KurtPack::find(const std::string& name, int32_t level) const {
    int32_t common = -1;
    for (size_t i = 0; i < entries_.size(); i++) {
        const Entry& entry = entries_[i];
        if (entry.name != name) {
            continue;
        }
        if (level > 0 && entry.level == level) {
            return entry.valid ? (int32_t)i : -1;
        }
        if (entry.level == 0 && common < 0) {
            common = (int32_t)i;
        }
    }
    return common;
}

const char* KurtPack::statusName(Status status) {
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
    }
    return "?";
}

} // namespace formats
} // namespace mdk
