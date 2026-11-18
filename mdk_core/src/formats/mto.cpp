// ============================================================================
// mto.cpp — see mto.h.
// ============================================================================

#include "formats/mto.h"

#include <utility>

#include "formats/arena.h"
#include "formats/bin_reader.h"
#include "formats/mdk_data.h"

namespace mdk {
namespace formats {

namespace {
// The directory of the arena list.
const int32_t kDirectoryOffset = 0x14;

// The largest arena block the streamed form accepts (the largest retail one is
// 1,656,047 bytes, LEVEL8O.MTO's GUNT_10).
const uint32_t kMaxBlockBytes = 8u << 20;

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
      data_(other.data_),
      path_(std::move(other.path_)),
      readFailures_(other.readFailures_),
      names_(std::move(other.names_)),
      offsets_(std::move(other.offsets_)),
      arenas_(std::move(other.arenas_)),
      owned_(std::move(other.owned_)) {
    // Nothing to release: a constructor's target owns nothing yet. The source
    // is left with a buffer it cannot index, so it is told so.
    other.bytes_ = nullptr;
    other.byteCount_ = 0;
    other.data_ = nullptr;
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
        data_ = other.data_;
        path_ = std::move(other.path_);
        readFailures_ = other.readFailures_;
        names_ = std::move(other.names_);
        offsets_ = std::move(other.offsets_);
        arenas_ = std::move(other.arenas_);
        owned_ = std::move(other.owned_);
        // The moved-from Mto must not keep a buffer it can no longer index.
        other.bytes_ = nullptr;
        other.byteCount_ = 0;
        other.data_ = nullptr;
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

bool Mto::open(Mto* out, const MdkData* data, const std::string& path) {
    if (!out || !data) {
        return false;
    }
    *out = Mto();
    std::vector<uint8_t> head;
    if (!data->readRange(path, 0, kDirectoryOffset + 4, &head)) {
        return false;
    }
    const uint32_t count = binU32(head.data(), kDirectoryOffset);
    // A directory entry is 12 bytes; a count past a few thousand is not an MTO.
    if (count > 4096) {
        return false;
    }
    std::vector<uint8_t> directory;
    if (!data->readRange(path, kDirectoryOffset + 4, (int32_t)count * 12, &directory)) {
        return false;
    }
    BinReader reader(directory.data(), (int32_t)directory.size(), 0);
    for (uint32_t i = 0; i < count; i++) {
        char name[9];
        reader.name(name, 8);
        out->names_.push_back(name);
        out->offsets_.push_back((int32_t)reader.u32());
    }
    out->data_ = data;
    // Resolved ONCE: every later read of a block goes straight to the file.
    out->path_ = data->pathOf(path);
    return true;
}

bool Mto::readBlock(const std::string& name, int32_t at, int32_t length,
                    std::vector<uint8_t>* out) const {
    if (!out || at < 0 || length < 0 || (uint32_t)at + (uint32_t)length > kMaxBlockBytes) {
        return false;
    }
    for (size_t i = 0; i < names_.size(); i++) {
        if (names_[i] != name) {
            continue;
        }
        if (data_) {
            return data_->readRangeAt(path_, offsets_[i] + at, length, out);
        }
        if (bytes_ && offsets_[i] > 0 &&
            (int64_t)offsets_[i] + at + length <= (int64_t)byteCount_) {
            out->assign(bytes_ + offsets_[i] + at, bytes_ + offsets_[i] + at + length);
            return true;
        }
        return false;
    }
    return false;
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
        Arena* arena = nullptr;
        if (bytes_) {
            arena = Arena::parse(name, bytes_, byteCount_, offsets_[i]);
        } else if (data_) {
            // The streamed form: this block alone, its size field first. The
            // block buffer is freed on return; the arena owns copies of all it
            // keeps (Arena::parse).
            //
            // THE SIZE COUNTS ITSELF: a block is [offset, offset + size) --
            // measured on all 60 arenas of levels 3-8 (the next block starts at
            // offset + size, padded to 4). The largest retail block is 1.66 MB;
            // a size past kMaxBlockBytes is corrupt, and refusing it before the
            // allocation keeps a garbage field from aborting the 3DS (no
            // exceptions: a failed new is fatal).
            std::vector<uint8_t> size;
            std::vector<uint8_t> block;
            const bool sized = data_->readRangeAt(path_, offsets_[i], 4, &size);
            const uint32_t blockSize = sized ? binU32(size.data(), 0) : 0;
            if (sized && blockSize >= 16 && blockSize <= kMaxBlockBytes &&
                data_->readRangeAt(path_, offsets_[i], (int32_t)blockSize, &block)) {
                arena = Arena::parse(name, block.data(), (int32_t)block.size(), 0);
            } else {
                // An unreadable block is a failed LOAD (LevelData::load checks
                // readFailures), not a quietly missing arena.
                readFailures_++;
            }
        }
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
