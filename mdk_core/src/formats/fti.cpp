// ============================================================================
// fti.cpp — see fti.h.
// ============================================================================

#include "formats/fti.h"

#include <algorithm>
#include <string.h>

#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

namespace {

// The longest entry name an index can hold. The FTI's are eight bytes and
// the BNI's are twelve.
const int32_t kMaxNameLength = 16;

// Set a name's entry, overwriting the value in place when the name is
// already there. That is what the C#'s dictionary does.
void setEntry(std::vector<std::pair<std::string, ArchiveIndex::Entry>>* entries, const std::string& name,
              const ArchiveIndex::Entry& entry) {
    for (size_t i = 0; i < entries->size(); i++) {
        if ((*entries)[i].first == name) {
            (*entries)[i].second = entry;
            return;
        }
    }
    entries->push_back(std::make_pair(name, entry));
}

} // namespace

bool ArchiveIndex::read(ArchiveIndex* out, const uint8_t* bytes, int32_t byteCount, int32_t nameLength) {
    if (!out || !bytes || byteCount < 0) {
        return false;
    }
    std::vector<uint8_t> copy(bytes, bytes + byteCount);
    return read(out, std::move(copy), nameLength);
}

bool ArchiveIndex::read(ArchiveIndex* out, std::vector<uint8_t>&& bytes, int32_t nameLength) {
    if (!out || bytes.size() < 8 || nameLength <= 0 || nameLength > kMaxNameLength) {
        return false;
    }
    const int32_t byteCount = (int32_t)bytes.size();

    ArchiveIndex index;
    index.bytes_ = std::move(bytes);

    // The count is at 4: the first u32 is the size of the whole archive.
    //
    // THE READER IS OVER index.bytes_, NOT OVER `bytes`: the vector has just
    // been moved into it, so `bytes` is empty and its data() is null, and a
    // reader over it with the OLD length reads through a null pointer.
    BinReader reader(index.bytes_.data(), byteCount, 4);
    const uint32_t count = reader.u32();

    // Every entry is a name and an offset, and the offsets are in the file,
    // so a count that does not fit is a broken file.
    if ((int64_t)count * (int64_t)(nameLength + 4) > (int64_t)(byteCount - 8)) {
        return false;
    }

    std::vector<std::pair<int32_t, std::string>> offsets;
    offsets.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        char name[kMaxNameLength + 1] = {0};
        reader.name(name, nameLength);
        offsets.push_back(std::make_pair(4 + (int32_t)reader.u32(), std::string(name)));
    }

    // The C# sorts the (offset, name) pairs. The offsets come first; the
    // name only breaks a tie, and it breaks it the way the C#'s string
    // comparison does.
    std::sort(offsets.begin(), offsets.end(),
              [](const std::pair<int32_t, std::string>& a, const std::pair<int32_t, std::string>& b) {
                  if (a.first != b.first) {
                      return a.first < b.first;
                  }
                  return a.second < b.second;
              });

    for (size_t i = 0; i < offsets.size(); i++) {
        const int32_t end = (i + 1 < offsets.size()) ? offsets[i + 1].first : (int32_t)index.bytes_.size();
        Entry entry;
        entry.offset = offsets[i].first;
        entry.size = end - offsets[i].first;
        setEntry(&index.entries_, offsets[i].second, entry);
    }

    *out = index;
    return true;
}

const ArchiveIndex::Entry* ArchiveIndex::find(const std::string& name) const {
    for (size_t i = 0; i < entries_.size(); i++) {
        if (entries_[i].first == name) {
            return &entries_[i].second;
        }
    }
    return nullptr;
}

void ArchiveIndex::add(const std::string& name, const uint8_t* data, int32_t size) {
    Entry entry;
    entry.offset = (int32_t)bytes_.size();
    entry.size = size;
    setEntry(&entries_, name, entry);
    if (data && size > 0) {
        bytes_.insert(bytes_.end(), data, data + size);
    }
}

bool ArchiveIndex::getBytes(const std::string& name, std::vector<uint8_t>* out) const {
    if (!out) {
        return false;
    }
    const Entry* entry = find(name);
    if (!entry) {
        return false;
    }
    if (entry->offset < 0 || entry->size < 0 || entry->offset + entry->size > (int32_t)bytes_.size()) {
        return false;
    }
    out->assign(bytes_.begin() + entry->offset, bytes_.begin() + entry->offset + entry->size);
    return true;
}

bool Fti::parse(Fti* out, const uint8_t* bytes, int32_t byteCount) {
    if (!out) {
        return false;
    }
    Fti fti;
    if (!ArchiveIndex::read(&fti.index_, bytes, byteCount, 8)) {
        return false;
    }
    *out = fti;
    return true;
}

bool Fti::parse(Fti* out, std::vector<uint8_t>&& bytes) {
    if (!out) {
        return false;
    }
    Fti fti;
    if (!ArchiveIndex::read(&fti.index_, std::move(bytes), 8)) {
        return false;
    }
    *out = fti;
    return true;
}

bool Fti::has(const std::string& name) const {
    return index_.find(name) != nullptr;
}

bool Fti::getBytes(const std::string& name, std::vector<uint8_t>* out) const {
    return index_.getBytes(name, out);
}

std::vector<uint8_t> Fti::getTextBytes(const std::string& name) const {
    std::vector<uint8_t> out;
    const ArchiveIndex::Entry* entry = index_.find(name);
    if (!entry) {
        return out;
    }
    if (entry->offset < 0 || entry->size < 0 || entry->offset + entry->size > (int32_t)index_.bytes().size()) {
        return out;
    }
    // The text runs to the first NUL, or to the end of the entry.
    const uint8_t* start = index_.bytes().data() + entry->offset;
    size_t length = 0;
    while (length < (size_t)entry->size && start[length] != 0) {
        length++;
    }
    out.assign(start, start + length);
    return out;
}

std::string Fti::getText(const std::string& name, const std::string& fallback) const {
    const ArchiveIndex::Entry* entry = index_.find(name);
    if (!entry) {
        return fallback;
    }
    if (entry->offset < 0 || entry->size < 0 || entry->offset + entry->size > (int32_t)index_.bytes().size()) {
        return fallback;
    }

    // The C#'s Bin.Ascii: up to the first NUL within the entry.
    const uint8_t* start = index_.bytes().data() + entry->offset;
    size_t length = 0;
    while (length < (size_t)entry->size && start[length] != 0) {
        length++;
    }
    std::string text((const char*)start, length);

    // The two character "\n" becomes a newline. The game's texts write it
    // that way so the file stays one line per entry.
    const std::string escape("\\n");
    size_t at = 0;
    while ((at = text.find(escape, at)) != std::string::npos) {
        text.replace(at, escape.size(), "\n");
        at += 1;
    }
    return text;
}

} // namespace formats
} // namespace mdk
