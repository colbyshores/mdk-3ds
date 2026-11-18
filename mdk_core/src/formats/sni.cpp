// ============================================================================
// sni.cpp — see sni.h.
// ============================================================================

#include "formats/sni.h"

#include <string.h>

#include "formats/ascii_case.h"
#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

namespace {

// Where the retail archive's directory is: after the header.
const int32_t kDirectoryOffset = 0x14;

// Where the 1996 demo's archive has its directory: the count is at 4,
// because its files start with a size word and no header.
const int32_t kBetaDirectoryOffset = 4;

// The name of a RIFF WAV.
const char kRiff[4] = {'R', 'I', 'F', 'F'};

// A directory entry: a twelve-byte name, two u16s and two u32s.
const int32_t kEntryBytes = 24;

// More than any sprite animation of the archives is (the largest retail one is
// 33,463 bytes): a size word past this is not an animation's.
const uint32_t kMaxAnimationBytes = 4u << 20;

// An entry's offset: the stored one is from byte 4 of the file. One that would
// not fit the signed offsets used everywhere else is -1, which every reader
// already refuses.
int32_t entryOffset(uint32_t stored) {
    return stored > 0x7FFFFF00u ? -1 : 4 + (int32_t)stored;
}

// More entries than any archive has (the largest retail one has 53): a count
// past this is not an SNI, and is refused before anything is allocated for it.
const uint32_t kMaxEntries = 4096;

} // namespace

std::string Sni::Entry::nameString() const {
    return std::string(name);
}

bool Sni::parse(Sni* out, const uint8_t* bytes, int32_t byteCount) {
    if (!out) {
        return false;
    }
    std::vector<uint8_t> copy(bytes, bytes + byteCount);
    return read(out, std::move(copy), kDirectoryOffset, false);
}

bool Sni::parse(Sni* out, std::vector<uint8_t>&& bytes) {
    if (!out) {
        return false;
    }
    return read(out, std::move(bytes), kDirectoryOffset, false);
}

bool Sni::parseBeta(Sni* out, const uint8_t* bytes, int32_t byteCount) {
    if (!out) {
        return false;
    }
    std::vector<uint8_t> copy(bytes, bytes + byteCount);
    return read(out, std::move(copy), kBetaDirectoryOffset, true);
}

bool Sni::parseBeta(Sni* out, std::vector<uint8_t>&& bytes) {
    if (!out) {
        return false;
    }
    return read(out, std::move(bytes), kBetaDirectoryOffset, true);
}

bool Sni::read(Sni* out, std::vector<uint8_t>&& bytes, int32_t directory, bool beta) {
    if (!out) {
        return false;
    }
    const int32_t byteCount = (int32_t)bytes.size();
    if (directory < 0 || directory + 4 > byteCount) {
        return false;
    }

    Sni sni;
    sni.bytes_ = std::move(bytes);

    BinReader reader(sni.bytes_.data(), byteCount, directory);
    const uint32_t count = reader.u32();

    // Every entry is a twelve-byte name, two u16s and two u32s: 24 bytes.
    if ((int64_t)count * 24 > (int64_t)(byteCount - directory - 4)) {
        return false;
    }

    sni.entries_.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        Entry entry;
        reader.name(entry.name, 12);
        if (beta) {
            // The demo's names are in lower case; the port's are in upper
            // case.
            const std::string upper = toUpperAscii(std::string(entry.name));
            memcpy(entry.name, upper.c_str(), upper.size() < 12 ? upper.size() : 12);
            if (upper.size() < 12) {
                entry.name[upper.size()] = '\0';
            }
        }
        entry.flags = (int32_t)reader.u16();
        entry.volume = (int32_t)reader.u16();
        entry.offset = entryOffset(reader.u32());
        entry.length = (int32_t)reader.u32();
        entry.riff = entry.offset >= 0 && entry.offset + 4 <= byteCount &&
                     memcmp(sni.bytes_.data() + entry.offset, kRiff, 4) == 0;
        sni.entries_.push_back(entry);
    }

    *out = std::move(sni);
    return true;
}

bool Sni::open(Sni* out, const MdkData& data, const std::string& path) {
    if (!out || !data.found()) {
        return false;
    }
    const std::string resolved = data.pathOf(path);
    std::vector<uint8_t> head;
    if (!data.readRangeAt(resolved, 0, kDirectoryOffset + 4, &head)) {
        return false;
    }
    const uint32_t count = binU32(head.data(), kDirectoryOffset);
    if (count > kMaxEntries) {
        return false;
    }
    std::vector<uint8_t> directory;
    if (!data.readRangeAt(resolved, kDirectoryOffset + 4, (int32_t)count * kEntryBytes,
                           &directory)) {
        return false;
    }

    Sni sni;
    sni.streamed_ = true;
    sni.data_ = data;
    sni.path_ = resolved;
    BinReader reader(directory.data(), (int32_t)directory.size(), 0);
    sni.entries_.reserve(count);
    std::vector<uint8_t> magic;
    for (uint32_t i = 0; i < count; i++) {
        Entry entry;
        reader.name(entry.name, 12);
        entry.flags = (int32_t)reader.u16();
        entry.volume = (int32_t)reader.u16();
        entry.offset = entryOffset(reader.u32());
        entry.length = (int32_t)reader.u32();
        // What the entry IS, read now (four bytes) so isSound() never reads.
        //
        // A KIND THAT CANNOT BE READ IS NOT "NOT A SOUND": it fails the open.
        // The whole-file form classifies from memory and cannot be wrong; here a
        // read that fails would turn a music track into "a corridor", the level's
        // load would parse a WAV as world geometry ('RIFF' is a material count
        // of 1.18e9), and the level would have no music with nothing said
        // (review, 2026-11-18). A missing archive is a level that does not load,
        // as before; a half-read one must be the same.
        if (entry.offset < 0 || !data.readRangeAt(resolved, entry.offset, 4, &magic)) {
            return false;
        }
        entry.riff = memcmp(magic.data(), kRiff, 4) == 0;
        sni.entries_.push_back(entry);
    }

    *out = std::move(sni);
    return true;
}

bool Sni::isSound(const Entry& entry) const {
    if (streamed_) {
        return entry.riff;
    }
    if (entry.offset < 0 || entry.offset + 4 > (int32_t)bytes_.size()) {
        return false;
    }
    return memcmp(bytes_.data() + entry.offset, kRiff, 4) == 0;
}

bool Sni::readEntry(const Entry& entry, int32_t at, int32_t length,
                    std::vector<uint8_t>* out) const {
    if (!out || at < 0 || length < 0 || entry.offset < 0 || entry.length < 0 ||
        (int64_t)at + length > (int64_t)entry.length) {
        return false;
    }
    if (streamed_) {
        if (entry.length > kMaxStreamedEntryBytes ||
            (int64_t)entry.offset + entry.length > (int64_t)0x7FFFFFFF) {
            readFailures_++;
            return false;
        }
        if (!data_.readRangeAt(path_, entry.offset + at, length, out)) {
            readFailures_++;
            return false;
        }
        return true;
    }
    if ((int64_t)entry.offset + entry.length > (int64_t)bytes_.size()) {
        return false;
    }
    out->assign(bytes_.begin() + entry.offset + at, bytes_.begin() + entry.offset + at + length);
    return true;
}

bool Sni::getBytes(const Entry& entry, std::vector<uint8_t>* out) const {
    return readEntry(entry, 0, entry.length, out);
}

bool Sni::animation(const std::string& name, SpriteAnimation* out) const {
    if (!out) {
        return false;
    }
    const Entry* found = nullptr;
    for (size_t i = 0; i < entries_.size(); i++) {
        if (entries_[i].nameString() == name) {
            found = &entries_[i];
            break;
        }
    }
    if (!found || isSound(*found)) {
        return false;
    }
    if (streamed_) {
        // The animation reads these bytes when its frames are decoded, so they
        // are kept: the entry and nothing else of the file. A frame that
        // pointed past its entry would be refused by the parse's own bounds
        // (the format tests compare both forms for every retail animation).
        //
        // HOW LONG THE ENTRY IS. Not the directory's length: a sprite
        // animation's is 0xFFFFFFFF in every retail archive (LEVEL4S.SNI's
        // K_SURF and K_SURFJ, LEVEL6S.SNI's four slides). The entry STARTS with
        // its size, which does not count itself (K_SURF: size 24497, the next
        // entry 24504 bytes on), so the entry is that word and `size` bytes.
        std::map<std::string, std::vector<uint8_t>>::iterator held = held_.find(name);
        if (held == held_.end()) {
            std::vector<uint8_t> word;
            if (found->offset < 0 || !data_.readRangeAt(path_, found->offset, 4, &word)) {
                readFailures_++;
                return false;
            }
            const uint32_t size = binU32(word.data(), 0);
            std::vector<uint8_t> bytes;
            if (size > kMaxAnimationBytes ||
                !data_.readRangeAt(path_, found->offset, (int32_t)size + 4, &bytes)) {
                readFailures_++;
                return false;
            }
            held = held_.insert(std::make_pair(name, std::move(bytes))).first;
        }
        return SpriteAnimation::parse(out, name, held->second.data(),
                                      (int32_t)held->second.size(), 4);
    }
    // The entry starts with the animation's size.
    return SpriteAnimation::parse(out, name, bytes_.data(), (int32_t)bytes_.size(), found->offset + 4);
}

} // namespace formats
} // namespace mdk
