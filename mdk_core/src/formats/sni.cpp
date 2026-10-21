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
        entry.offset = 4 + (int32_t)reader.u32();
        entry.length = (int32_t)reader.u32();
        sni.entries_.push_back(entry);
    }

    *out = std::move(sni);
    return true;
}

bool Sni::isSound(const Entry& entry) const {
    if (entry.offset < 0 || entry.offset + 4 > (int32_t)bytes_.size()) {
        return false;
    }
    return memcmp(bytes_.data() + entry.offset, kRiff, 4) == 0;
}

bool Sni::getBytes(const Entry& entry, std::vector<uint8_t>* out) const {
    if (!out) {
        return false;
    }
    if (entry.offset < 0 || entry.length < 0 || entry.offset + entry.length > (int32_t)bytes_.size()) {
        return false;
    }
    out->assign(bytes_.begin() + entry.offset, bytes_.begin() + entry.offset + entry.length);
    return true;
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
    // The entry starts with the animation's size.
    return SpriteAnimation::parse(out, name, bytes_.data(), (int32_t)bytes_.size(), found->offset + 4);
}

} // namespace formats
} // namespace mdk
