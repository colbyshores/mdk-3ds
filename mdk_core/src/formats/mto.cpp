// ============================================================================
// mto.cpp — see mto.h.
// ============================================================================

#include "formats/mto.h"

#include <utility>

#include "formats/arena.h"
#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

namespace {
// The directory of the arena list.
const int32_t kDirectoryOffset = 0x14;

// Free every arena an Mto parsed. The destructor and the move assignment both
// go through this, so they cannot drift apart on what "release" means.
void releaseOwned(std::vector<Arena*>& owned) {
    for (size_t i = 0; i < owned.size(); i++) {
        delete owned[i];
    }
    owned.clear();
}
} // namespace

Mto::~Mto() {
    // Exactly the arenas this Mto parsed. The add()ed ones are the caller's,
    // and deleting them here is the double free the split in mto.h exists to
    // avoid.
    releaseOwned(owned_);
}

Mto::Mto(Mto&& other) noexcept
    : bytes_(other.bytes_),
      byteCount_(other.byteCount_),
      names_(std::move(other.names_)),
      offsets_(std::move(other.offsets_)),
      arenas_(std::move(other.arenas_)),
      owned_(std::move(other.owned_)) {
    // Nothing to release: a constructor's target owns nothing yet. The source
    // is left with a buffer it cannot index, so it is told so.
    other.bytes_ = nullptr;
    other.byteCount_ = 0;
}

Mto& Mto::operator=(Mto&& other) noexcept {
    if (this != &other) {
        // Hand-written rather than defaulted, and this line is the reason: a
        // defaulted move would move the owned_ vector out and drop the old
        // Arena pointers on the floor, which is the leak this file just fixed
        // in a new place. LevelData::load move-assigns onto an out-param that
        // may already hold a level, so this path is the real one.
        releaseOwned(owned_);
        bytes_ = other.bytes_;
        byteCount_ = other.byteCount_;
        names_ = std::move(other.names_);
        offsets_ = std::move(other.offsets_);
        arenas_ = std::move(other.arenas_);
        owned_ = std::move(other.owned_);
        // The moved-from Mto must not keep a buffer it can no longer index.
        other.bytes_ = nullptr;
        other.byteCount_ = 0;
    }
    return *this;
}

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

Arena* Mto::getArena(const std::string& name) {
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
        // The cache keeps whatever the parse gave, including a null, exactly
        // as before and exactly as the C#'s `_arenas[name] = Arena.Parse(...)`
        // does. owned_ records only a real arena: there is nothing to delete
        // for a null, and deleting one would be worse.
        if (arena) {
            owned_.push_back(arena);
        }
        arenas_.push_back(std::make_pair(name, arena));
        return arena;
    }
    return nullptr;
}

} // namespace formats
} // namespace mdk
