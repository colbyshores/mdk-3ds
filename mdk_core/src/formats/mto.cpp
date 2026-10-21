// ============================================================================
// mto.cpp — see mto.h.
// ============================================================================

#include "formats/mto.h"

#include "formats/arena.h"
#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

namespace {
// The directory of the arena list.
const int32_t kDirectoryOffset = 0x14;
} // namespace

Mto Mto::parse(const uint8_t* bytes, int32_t byteCount) {
    Mto mto;
    mto.bytes_ = bytes;
    mto.byteCount_ = byteCount;
    if (!bytes || byteCount < kDirectoryOffset + 4) {
        return mto;
    }
    BinReader reader(bytes, byteCount, kDirectoryOffset);
    const uint32_t count = reader.u32();
    for (uint32_t i = 0; i < count; i++) {
        char name[9];
        reader.name(name, 8);
        mto.names_.push_back(name);
        mto.offsets_.push_back((int32_t)reader.u32());
    }
    return mto;
}

void Mto::add(const Arena* arena) {
    if (!arena) {
        return;
    }
    names_.push_back(arena->name());
    offsets_.push_back(0);
    arenas_.push_back(std::make_pair(arena->name(), const_cast<Arena*>(arena)));
}

bool Mto::has(const std::string& name) const {
    for (size_t i = 0; i < names_.size(); i++) {
        if (names_[i] == name) {
            return true;
        }
    }
    return false;
}

const Arena* Mto::getArena(const std::string& name) {
    for (size_t i = 0; i < arenas_.size(); i++) {
        if (arenas_[i].first == name) {
            return arenas_[i].second;
        }
    }
    for (size_t i = 0; i < names_.size(); i++) {
        if (names_[i] != name) {
            continue;
        }
        Arena* arena = Arena::parse(name, bytes_, byteCount_, offsets_[i]);
        arenas_.push_back(std::make_pair(name, arena));
        return arena;
    }
    return nullptr;
}

} // namespace formats
} // namespace mdk
