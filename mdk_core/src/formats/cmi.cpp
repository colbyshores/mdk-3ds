// ============================================================================
// cmi.cpp — see cmi.h.
// ============================================================================

#include "formats/cmi.h"

#include <string.h>

#include "formats/ascii_case.h"
#include "formats/bin_reader.h"

namespace mdk {
namespace formats {

namespace {

// Where the retail file's directories are: after the header.
const int32_t kDirectoryOffset = 0x14;

// The offsets in the file are from 4.
const int32_t kFileBase = 4;

// The longest name a directory entry can have: its length is a byte.
const int32_t kMaxNameLength = 255;

// A pascal string: a u8 length and then that many characters, cut at the
// first NUL.
bool readPascalName(BinReader* reader, std::string* out) {
    if (reader->pos() + 1 > reader->length()) {
        return false;
    }
    const int32_t length = (int32_t)reader->u8();
    if (length < 0 || length > kMaxNameLength || reader->pos() + length > reader->length()) {
        return false;
    }
    char name[kMaxNameLength + 1] = {0};
    reader->name(name, length);
    if (out) {
        *out = std::string(name);
    }
    return true;
}

} // namespace

int32_t cmiAbsoluteOffset(uint32_t offset) {
    return offset == 0 ? 0 : kFileBase + (int32_t)offset;
}

bool findOffset(const std::vector<std::pair<std::string, int32_t>>* map, const std::string& name,
                int32_t* outOffset) {
    if (!map || !outOffset) {
        return false;
    }
    for (size_t i = 0; i < map->size(); i++) {
        if ((*map)[i].first == name) {
            *outOffset = (*map)[i].second;
            return true;
        }
    }
    return false;
}

void setOffset(std::vector<std::pair<std::string, int32_t>>* map, const std::string& name, int32_t offset) {
    if (!map) {
        return;
    }
    for (size_t i = 0; i < map->size(); i++) {
        if ((*map)[i].first == name) {
            (*map)[i].second = offset;
            return;
        }
    }
    map->push_back(std::make_pair(name, offset));
}

bool findText(const std::vector<std::pair<std::string, std::string>>* map, const std::string& name,
              std::string* outText) {
    if (!map || !outText) {
        return false;
    }
    for (size_t i = 0; i < map->size(); i++) {
        if ((*map)[i].first == name) {
            *outText = (*map)[i].second;
            return true;
        }
    }
    return false;
}

void setText(std::vector<std::pair<std::string, std::string>>* map, const std::string& name,
             const std::string& text) {
    if (!map) {
        return;
    }
    for (size_t i = 0; i < map->size(); i++) {
        if ((*map)[i].first == name) {
            (*map)[i].second = text;
            return;
        }
    }
    map->push_back(std::make_pair(name, text));
}

bool Cmi::parse(Cmi* out, const uint8_t* bytes, int32_t byteCount) {
    if (!out) {
        return false;
    }
    std::vector<uint8_t> copy(bytes, bytes + byteCount);
    return parse(out, std::move(copy));
}

bool Cmi::parse(Cmi* out, std::vector<uint8_t>&& bytes) {
    if (!out) {
        return false;
    }
    Cmi cmi;
    cmi.bytes_ = std::move(bytes);
    cmi.dialect_ = ScriptDialect::Retail;
    if (!cmi.readDirectories(kDirectoryOffset, false)) {
        return false;
    }
    if (!cmi.readArenas()) {
        return false;
    }
    *out = std::move(cmi);
    return true;
}

bool Cmi::readDirectories(int32_t directory, bool beta) {
    const int32_t byteCount = (int32_t)bytes_.size();
    if (directory < 0 || directory + 4 > byteCount) {
        return false;
    }

    BinReader reader(bytes_.data(), byteCount, directory);
    std::vector<std::pair<std::string, int32_t>>* directories[4] = {
        &alienScripts_, &modelOffsets_, &objectScripts_, &arenaScripts_,
    };

    for (int32_t d = 0; d < 4; d++) {
        if (reader.pos() + 4 > byteCount) {
            return false;
        }
        const uint32_t count = reader.u32();
        for (uint32_t i = 0; i < count; i++) {
            if (reader.pos() + 1 > byteCount) {
                return false;
            }
            const int32_t nameLength = (int32_t)reader.u8();
            if (nameLength < 0 || nameLength > kMaxNameLength) {
                return false;
            }
            if (reader.pos() + nameLength + 4 > byteCount) {
                return false;
            }
            char name[kMaxNameLength + 1] = {0};
            reader.name(name, nameLength);
            const int32_t offset = cmiAbsoluteOffset(reader.u32());

            std::string entryName(name);
            if (beta) {
                // The demo's names are in lower case; the port's are in
                // upper case.
                entryName = toUpperAscii(entryName);
            }
            setOffset(directories[d], entryName, offset);

            // The alien and the object scripts are the ones the script VM
            // starts from. The models and the arenas are looked up by name.
            if (offset != 0 && (d == 0 || d == 2)) {
                entries_.push_back(offset);
            }
        }
    }
    return true;
}

bool Cmi::readArenas() {
    // The C# takes a snapshot of the arena names before it changes their
    // offsets, because it changes them while it walks them.
    std::vector<std::string> arenas;
    for (size_t i = 0; i < arenaScripts_.size(); i++) {
        arenas.push_back(arenaScripts_[i].first);
    }

    const int32_t byteCount = (int32_t)bytes_.size();
    for (size_t i = 0; i < arenas.size(); i++) {
        int32_t offset = 0;
        findOffset(&arenaScripts_, arenas[i], &offset);
        if (offset < 0 || offset >= byteCount) {
            return false;
        }

        BinReader record(bytes_.data(), byteCount, offset);
        // The "NONE?" name, which the C# reads and throws away.
        if (!readPascalName(&record, nullptr)) {
            return false;
        }
        std::string music;
        if (!readPascalName(&record, &music)) {
            return false;
        }
        setText(&arenaMusic_, arenas[i], music);

        // The record says where the arena's SCRIPT is, and the directory
        // entry is replaced by it.
        const int32_t script = cmiAbsoluteOffset(record.u32());
        setOffset(&arenaScripts_, arenas[i], script);
        if (script != 0) {
            entries_.push_back(script);
        }
    }
    return true;
}

bool Cmi::model(const std::string& name, Model* out) const {
    if (!out) {
        return false;
    }
    int32_t offset = 0;
    if (!findOffset(&modelOffsets_, name, &offset) || offset == 0) {
        return false;
    }
    for (size_t i = 0; i < models_.size(); i++) {
        if (models_[i].first == name) {
            *out = models_[i].second;
            return true;
        }
    }

    Model model;
    if (!Model::parse(&model, name, bytes_.data(), (int32_t)bytes_.size(), offset)) {
        return false;
    }
    models_.push_back(std::make_pair(name, model));
    *out = model;
    return true;
}

} // namespace formats
} // namespace mdk
